#include "usb_vendor_bulk.h"

#include <string.h>

#include "board.h"
#include "clock_config.h"
#include "fsl_clock.h"
#include "fsl_debug_console.h"
#include "usb_device_config.h"
#include "usb_device.h"
#include "usb_device_ch9.h"
#include "usb_charger_detect.h"
#include "usb_phy.h"
#include "usb_spec.h"

#define USB_DEVICE_CONTROLLER_ID ((uint8_t)kUSB_ControllerEhci0)
#define USB_BULK_BUFFER_SIZE (USB_VENDOR_BULK_EP_MAX_PACKET_SIZE_HS)
#define USB_RX_RING_DEPTH (32U)
#define USB_VENDOR_BULK_IRQ_PRIORITY (6U)

extern void USB_DeviceEhciIsrFunction(void *deviceHandle);
static usb_status_t USB_DeviceCallback(usb_device_handle handle, uint32_t event, void *param);

static usb_device_handle s_UsbDeviceHandle;
static uint8_t s_CurrentConfiguration;
static uint8_t s_Attached;
static uint8_t s_BulkInBusy;
static uint8_t s_CurrentSpeed;
static volatile uint8_t s_RxRingHead;
static volatile uint8_t s_RxRingTail;

USB_DMA_NONINIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
static uint8_t s_BulkOutBuffer[USB_DATA_ALIGN_SIZE_MULTIPLE(USB_BULK_BUFFER_SIZE)];
USB_DMA_NONINIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
static uint8_t s_BulkInBuffer[USB_DATA_ALIGN_SIZE_MULTIPLE(USB_BULK_BUFFER_SIZE)];
static uint8_t s_VendorCtrlBuffer[64];
static uint8_t s_RxRingData[USB_RX_RING_DEPTH][USB_BULK_BUFFER_SIZE];
static uint16_t s_RxRingLength[USB_RX_RING_DEPTH];

static uint32_t s_RxPackets;
static uint32_t s_RxDropped;
static uint32_t s_TxPackets;
static uint32_t s_TxBusyDrop;
static uint32_t s_UsbIrqCount;
static uint8_t s_AttachEventSeen;
static uint8_t s_BusResetSeen;
static uint8_t s_RunBitSeen;
static uint8_t s_DcdFinished;
static uint8_t s_DcdResult;
static usb_device_class_config_list_struct_t s_UsbClassConfigList = {
    NULL,
    USB_DeviceCallback,
    0U,
};

static uint32_t EnterCritical(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    return primask;
}

static void ExitCritical(uint32_t primask)
{
    __set_PRIMASK(primask);
}

static void USB_DeviceClockInit(void)
{
    usb_phy_config_struct_t phyConfig = {
        BOARD_USB_PHY_D_CAL,
        BOARD_USB_PHY_TXCAL45DP,
        BOARD_USB_PHY_TXCAL45DM,
    };

    (void)CLOCK_EnableUsbhs0PhyPllClock(kCLOCK_Usbphy480M, 480000000U);
    (void)CLOCK_EnableUsbhs0Clock(kCLOCK_Usb480M, 480000000U);
    USB_EhciPhyInit(USB_DEVICE_CONTROLLER_ID, BOARD_XTAL0_CLK_HZ, &phyConfig);
}

static void USB_DeviceIsrEnable(void)
{
    uint8_t irqNumber;
    uint8_t usbDeviceEhciIrq[] = USBHS_IRQS;

    irqNumber = usbDeviceEhciIrq[USB_DEVICE_CONTROLLER_ID - (uint8_t)kUSB_ControllerEhci0];
    NVIC_SetPriority((IRQn_Type)irqNumber, USB_VENDOR_BULK_IRQ_PRIORITY);
    EnableIRQ((IRQn_Type)irqNumber);
}

static void USB_DeviceUpdateSpeed(usb_device_handle handle)
{
    uint8_t speed = USB_SPEED_FULL;

    if ((handle != NULL) && (USB_DeviceGetStatus(handle, kUSB_DeviceStatusSpeed, &speed) == kStatus_USB_Success))
    {
        s_CurrentSpeed = speed;
    }
    else
    {
        s_CurrentSpeed = USB_SPEED_FULL;
    }

    USB_VendorBulkSetSpeed(s_CurrentSpeed);
    PRINTF("USB negotiated speed: %s (%u-byte bulk)\r\n",
           (s_CurrentSpeed == USB_SPEED_HIGH) ? "HIGH" : "FULL",
           USB_VendorBulkGetCurrentPacketSize());
}

static void USB_RxRingReset(void)
{
    uint32_t primask = EnterCritical();
    s_RxRingHead = 0U;
    s_RxRingTail = 0U;
    ExitCritical(primask);
}

static usb_status_t USB_BulkEndpointCallback(usb_device_handle handle,
                                             usb_device_endpoint_callback_message_struct_t *message,
                                             void *callbackParam)
{
    uint8_t endpointAddress = (uint8_t)(uintptr_t)callbackParam;

    if ((message == NULL) || (message->length == USB_CANCELLED_TRANSFER_LENGTH))
    {
        return kStatus_USB_Error;
    }

    if ((endpointAddress & USB_DESCRIPTOR_ENDPOINT_ADDRESS_DIRECTION_MASK) != 0U)
    {
        s_BulkInBusy = 0U;
        return kStatus_USB_Success;
    }

    if ((message->length != 0U) && (message->buffer != NULL))
    {
        uint8_t nextHead = (uint8_t)((s_RxRingHead + 1U) % USB_RX_RING_DEPTH);
        if (nextHead != s_RxRingTail)
        {
            uint32_t storeLen = message->length;
            if (storeLen > USB_BULK_BUFFER_SIZE)
            {
                storeLen = USB_BULK_BUFFER_SIZE;
            }

            (void)memcpy(s_RxRingData[s_RxRingHead], message->buffer, storeLen);
            s_RxRingLength[s_RxRingHead] = (uint16_t)storeLen;
            s_RxRingHead = nextHead;
            s_RxPackets++;
        }
        else
        {
            s_RxDropped++;
        }
    }

    (void)USB_DeviceRecvRequest(handle, USB_VENDOR_BULK_OUT_ENDPOINT, s_BulkOutBuffer, sizeof(s_BulkOutBuffer));
    return kStatus_USB_Success;
}

static usb_status_t USB_VendorBulkSetConfigure(usb_device_handle handle, uint8_t configure)
{
    usb_device_endpoint_init_struct_t epInit;
    usb_device_endpoint_callback_struct_t epCallback;

    if (configure == USB_VENDOR_BULK_CONFIGURE_INDEX)
    {
        USB_DeviceUpdateSpeed(handle);

        epInit.zlt = 0U;
        epInit.transferType = USB_ENDPOINT_BULK;
        epInit.interval = 0U;
        epInit.maxPacketSize = USB_VendorBulkGetCurrentPacketSize();

        epCallback.callbackFn = USB_BulkEndpointCallback;

        epInit.endpointAddress = USB_VENDOR_BULK_OUT_ENDPOINT;
        epCallback.callbackParam = (void *)(uintptr_t)USB_VENDOR_BULK_OUT_ENDPOINT;
        if (USB_DeviceInitEndpoint(handle, &epInit, &epCallback) != kStatus_USB_Success)
        {
            return kStatus_USB_Error;
        }

        epInit.endpointAddress = USB_VENDOR_BULK_IN_ENDPOINT;
        epCallback.callbackParam = (void *)(uintptr_t)USB_VENDOR_BULK_IN_ENDPOINT;
        if (USB_DeviceInitEndpoint(handle, &epInit, &epCallback) != kStatus_USB_Success)
        {
            (void)USB_DeviceDeinitEndpoint(handle, USB_VENDOR_BULK_OUT_ENDPOINT);
            return kStatus_USB_Error;
        }

        s_BulkInBusy = 0U;
        s_Attached = 1U;
        USB_RxRingReset();
        (void)USB_DeviceRecvRequest(handle, USB_VENDOR_BULK_OUT_ENDPOINT, s_BulkOutBuffer, sizeof(s_BulkOutBuffer));
    }
    else
    {
        (void)USB_DeviceDeinitEndpoint(handle, USB_VENDOR_BULK_IN_ENDPOINT);
        (void)USB_DeviceDeinitEndpoint(handle, USB_VENDOR_BULK_OUT_ENDPOINT);
        s_BulkInBusy = 0U;
        s_Attached = 0U;
        USB_RxRingReset();
    }

    return kStatus_USB_Success;
}

static usb_status_t USB_DeviceCallback(usb_device_handle handle, uint32_t event, void *param)
{
    usb_status_t status = kStatus_USB_Success;

    switch (event)
    {
        case kUSB_DeviceEventBusReset:
            s_CurrentConfiguration = 0U;
            s_Attached = 0U;
            s_BulkInBusy = 0U;
            s_BusResetSeen = 1U;
            USB_RxRingReset();
            USB_DeviceUpdateSpeed(handle);
            break;

#if (defined(USB_DEVICE_CONFIG_DETACH_ENABLE) && (USB_DEVICE_CONFIG_DETACH_ENABLE > 0U))
        case kUSB_DeviceEventAttach:
            s_AttachEventSeen = 1U;
            s_DcdFinished = 0U;
            s_DcdResult = 0xFFU;
            SDK_DelayAtLeastUs(5000U, SDK_DEVICE_MAXIMUM_CPU_CLOCK_FREQUENCY);
            status = USB_DeviceRun(handle);
            if ((status == kStatus_USB_Success) && ((USB1->USBCMD & USBHS_USBCMD_RS_MASK) != 0U))
            {
                s_RunBitSeen = 1U;
            }
            break;

        case kUSB_DeviceEventDetach:
            s_AttachEventSeen = 0U;
            s_Attached = 0U;
            s_CurrentConfiguration = 0U;
            s_BulkInBusy = 0U;
            USB_RxRingReset();
            (void)USB_DeviceStop(handle);
            break;
#endif

#if (defined(USB_DEVICE_CONFIG_CHARGER_DETECT) && (USB_DEVICE_CONFIG_CHARGER_DETECT > 0U))
        case kUSB_DeviceEventDcdDetectionfinished:
            if (param == NULL)
            {
                return kStatus_USB_InvalidParameter;
            }

            s_DcdFinished = 1U;
            s_DcdResult = *(uint8_t *)param;

            switch (*(uint8_t *)param)
            {
                case kUSB_DcdSDP:
                case kUSB_DcdCDP:
                case kUSB_DcdDCP:
                    SDK_DelayAtLeastUs(5000U, SystemCoreClock);
                    status = USB_DeviceRun(handle);
                    if ((status == kStatus_USB_Success) && ((USB1->USBCMD & USBHS_USBCMD_RS_MASK) != 0U))
                    {
                        s_RunBitSeen = 1U;
                    }
                    break;

                default:
                    status = kStatus_USB_Error;
                    break;
            }
            break;
#endif

        case kUSB_DeviceEventSetConfiguration:
            if (param == NULL)
            {
                return kStatus_USB_InvalidParameter;
            }
            s_CurrentConfiguration = (uint8_t)(*(uint16_t *)param);
            status = USB_VendorBulkSetConfigure(handle, s_CurrentConfiguration);
            break;

        case kUSB_DeviceEventGetConfiguration:
            if (param == NULL)
            {
                return kStatus_USB_InvalidParameter;
            }
            *(uint8_t *)param = s_CurrentConfiguration;
            break;

        case kUSB_DeviceEventSetInterface:
        case kUSB_DeviceEventGetInterface:
            break;

        case kUSB_DeviceEventVendorRequest:
            if (param == NULL)
            {
                return kStatus_USB_InvalidParameter;
            }
            {
                usb_device_control_request_struct_t *request = (usb_device_control_request_struct_t *)param;
                if (request->isSetup != 0U)
                {
                    if (USB_VendorBulkHandleMsOsVendorRequest(request->setup, &request->buffer, &request->length))
                    {
                        status = kStatus_USB_Success;
                    }
                    else if ((request->setup->bmRequestType & USB_REQUEST_TYPE_DIR_MASK) != 0U)
                    {
                        s_VendorCtrlBuffer[0] = 0x01U;
                        request->buffer = s_VendorCtrlBuffer;
                        request->length = 1U;
                        status = kStatus_USB_Success;
                    }
                    else if (request->setup->wLength > 0U)
                    {
                        request->buffer = s_VendorCtrlBuffer;
                        request->length = sizeof(s_VendorCtrlBuffer);
                        status = kStatus_USB_Success;
                    }
                }
            }
            break;

        case kUSB_DeviceEventGetDeviceDescriptor:
        case kUSB_DeviceEventGetConfigurationDescriptor:
        case kUSB_DeviceEventGetStringDescriptor:
        case kUSB_DeviceEventGetDeviceQualifierDescriptor:
            status = USB_VendorBulkGetDescriptor(event, param);
            break;

        default:
            break;
    }

    return status;
}

void USB_OTG1_IRQHandler(void)
{
    s_UsbIrqCount++;
    if (s_UsbDeviceHandle != NULL)
    {
        USB_DeviceEhciIsrFunction(s_UsbDeviceHandle);
    }
    __DSB();
}

void USB_VendorBulkInit(void)
{
    s_UsbDeviceHandle = NULL;
    s_CurrentConfiguration = 0U;
    s_Attached = 0U;
    s_BulkInBusy = 0U;
    s_CurrentSpeed = USB_SPEED_FULL;
    s_RxPackets = 0U;
    s_RxDropped = 0U;
    s_TxPackets = 0U;
    s_TxBusyDrop = 0U;
    s_UsbIrqCount = 0U;
    s_AttachEventSeen = 0U;
    s_BusResetSeen = 0U;
    s_RunBitSeen = 0U;
    s_DcdFinished = 0U;
    s_DcdResult = 0xFFU;
    USB_RxRingReset();
    USB_VendorBulkSetSpeed(s_CurrentSpeed);

    USB_DeviceClockInit();

    if (USB_DeviceClassInit(USB_DEVICE_CONTROLLER_ID, &s_UsbClassConfigList, &s_UsbDeviceHandle) !=
        kStatus_USB_Success)
    {
        return;
    }

    USB_DeviceIsrEnable();
    SDK_DelayAtLeastUs(5000U, SDK_DEVICE_MAXIMUM_CPU_CLOCK_FREQUENCY);

#if !((defined(USB_DEVICE_CONFIG_DETACH_ENABLE) && (USB_DEVICE_CONFIG_DETACH_ENABLE > 0U)) && \
      (defined(USB_DEVICE_CONFIG_CHARGER_DETECT) && (USB_DEVICE_CONFIG_CHARGER_DETECT > 0U)))
    if (USB_DeviceRun(s_UsbDeviceHandle) == kStatus_USB_Success)
    {
        if ((USB1->USBCMD & USBHS_USBCMD_RS_MASK) != 0U)
        {
            s_RunBitSeen = 1U;
        }
    }
#endif
}

void USB_VendorBulkTask(void)
{
}

void USB_VendorBulkUpdateHwTick(uint64_t tick)
{
#if ((defined(USB_DEVICE_CONFIG_REMOTE_WAKEUP)) && (USB_DEVICE_CONFIG_REMOTE_WAKEUP > 0U)) || \
    (((defined(USB_DEVICE_CONFIG_CHARGER_DETECT) && (USB_DEVICE_CONFIG_CHARGER_DETECT > 0U)) && \
      (defined(FSL_FEATURE_SOC_USB_ANALOG_COUNT) && (FSL_FEATURE_SOC_USB_ANALOG_COUNT > 0U))))
    if (s_UsbDeviceHandle != NULL)
    {
        (void)USB_DeviceUpdateHwTick(s_UsbDeviceHandle, tick);
    }
#else
    (void)tick;
#endif
}

bool USB_VendorBulkIsConfigured(void)
{
    return (s_UsbDeviceHandle != NULL) && (s_CurrentConfiguration == USB_VENDOR_BULK_CONFIGURE_INDEX);
}

void USB_VendorBulkGetStats(usb_vendor_bulk_stats_t *stats)
{
    uint32_t primask;

    if (stats == NULL)
    {
        return;
    }

    primask = EnterCritical();
    stats->rxPackets = s_RxPackets;
    stats->rxDropped = s_RxDropped;
    stats->txPackets = s_TxPackets;
    stats->txBusyDrop = s_TxBusyDrop;
    stats->configured = (s_CurrentConfiguration == USB_VENDOR_BULK_CONFIGURE_INDEX) ? 1U : 0U;
    ExitCritical(primask);
}

void USB_VendorBulkGetDebugState(usb_vendor_bulk_debug_state_t *state)
{
    uint32_t primask;
    uint32_t statusPending;
    uint32_t otgPending;

    if (state == NULL)
    {
        return;
    }

    primask = EnterCritical();
    state->usbcmd = USB1->USBCMD;
    state->usbmode = USB1->USBMODE;
    state->otgsc = USB1->OTGSC;
    state->portsc1 = USB1->PORTSC1;
    state->usbsts = USB1->USBSTS;
    state->usbphyCtrl = USBPHY1->CTRL;
    state->usbphyStatus = USBPHY1->STATUS;
    state->usbncCtrl = USBNC1->USB_OTGn_CTRL;
    state->usbncPhyCtrl0 = USBNC1->USB_OTGn_PHY_CTRL_0;
    state->phyVbusDetStat = USB_ANALOG->INSTANCE[0].VBUS_DETECT_STAT;
    state->usbintr = USB1->USBINTR;
    state->configured = (s_CurrentConfiguration == USB_VENDOR_BULK_CONFIGURE_INDEX) ? 1U : 0U;
    state->attached = s_Attached;
    state->attachEventSeen = s_AttachEventSeen;
    statusPending = (state->usbsts & state->usbintr);
    otgPending = (state->otgsc & USBHS_OTGSC_BSVIS_MASK);
    state->rawIrqPending = ((statusPending != 0U) || (otgPending != 0U)) ? 1U : 0U;
    state->irqSeen = (s_UsbIrqCount != 0U) ? 1U : 0U;
    state->busResetSeen = s_BusResetSeen;
    state->runBitSeen = (((state->usbcmd & USBHS_USBCMD_RS_MASK) != 0U) || (s_RunBitSeen != 0U)) ? 1U : 0U;
    state->dcdFinished = s_DcdFinished;
    state->dcdResult = s_DcdResult;
    state->speed = s_CurrentSpeed;
    state->vbusValid = (((state->otgsc & USBHS_OTGSC_BSV_MASK) != 0U) ||
                        ((state->phyVbusDetStat & USB_ANALOG_VBUS_DETECT_STAT_VBUS_VALID_MASK) != 0U))
                           ? 1U
                           : 0U;
    ExitCritical(primask);
}

bool USB_VendorBulkPopRxPacket(uint8_t *data, uint32_t *length, uint32_t maxLength)
{
    uint32_t primask;
    uint8_t tail;
    uint16_t packetLength;

    if ((data == NULL) || (length == NULL) || (maxLength == 0U))
    {
        return false;
    }

    primask = EnterCritical();
    if (s_RxRingHead == s_RxRingTail)
    {
        ExitCritical(primask);
        return false;
    }

    tail = s_RxRingTail;
    packetLength = s_RxRingLength[tail];
    if (packetLength > maxLength)
    {
        packetLength = (uint16_t)maxLength;
    }

    (void)memcpy(data, s_RxRingData[tail], packetLength);
    s_RxRingTail = (uint8_t)((s_RxRingTail + 1U) % USB_RX_RING_DEPTH);
    ExitCritical(primask);

    *length = packetLength;
    return true;
}

bool USB_VendorBulkSendPacket(const uint8_t *data, uint32_t length)
{
    uint32_t primask;
    bool canSend = false;

    if ((data == NULL) || (length == 0U) || (length > USB_BULK_BUFFER_SIZE))
    {
        return false;
    }

    primask = EnterCritical();
    if ((s_UsbDeviceHandle != NULL) && (s_CurrentConfiguration == USB_VENDOR_BULK_CONFIGURE_INDEX) && (s_BulkInBusy == 0U))
    {
        s_BulkInBusy = 1U;
        canSend = true;
    }
    else
    {
        s_TxBusyDrop++;
    }
    ExitCritical(primask);

    if (!canSend)
    {
        return false;
    }

    (void)memcpy(s_BulkInBuffer, data, length);
    if (USB_DeviceSendRequest(s_UsbDeviceHandle, USB_VENDOR_BULK_IN_ENDPOINT, s_BulkInBuffer, length) != kStatus_USB_Success)
    {
        primask = EnterCritical();
        s_BulkInBusy = 0U;
        s_TxBusyDrop++;
        ExitCritical(primask);
        return false;
    }

    primask = EnterCritical();
    s_TxPackets++;
    ExitCritical(primask);
    return true;
}
