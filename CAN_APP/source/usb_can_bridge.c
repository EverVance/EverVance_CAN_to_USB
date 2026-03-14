#include "usb_can_bridge.h"

#include <string.h>

#include "can_stack.h"
#include "fsl_debug_console.h"
#include "usb_vendor_bulk.h"

#define CAN_BRIDGE_CFG_PROTO_VERSION (1U)
#define CAN_BRIDGE_CMD_SET_CHANNEL_CONFIG (0x01U)
#define CAN_BRIDGE_CMD_GET_CHANNEL_CONFIG (0x02U)
#define CAN_BRIDGE_CMD_GET_DEVICE_INFO (0x03U)
#define CAN_BRIDGE_CMD_GET_CHANNEL_CAPS (0x04U)
#define CAN_BRIDGE_CMD_GET_RUNTIME_STATUS (0x05U)
#define CAN_BRIDGE_CMD_HEARTBEAT (0x06U)

#define CAN_BRIDGE_CFG_STATUS_OK (0U)
#define CAN_BRIDGE_CFG_STATUS_INVALID (1U)
#define CAN_BRIDGE_CFG_STATUS_STAGED_ONLY (2U)
#define CAN_BRIDGE_CFG_STATUS_INTERNAL_ERR (3U)

#define CAN_BRIDGE_CFG_PAYLOAD_LEN (16U)
#define CAN_BRIDGE_DEVICE_INFO_PAYLOAD_LEN (16U)
#define CAN_BRIDGE_CHANNEL_CAPS_PAYLOAD_LEN (20U)
#define CAN_BRIDGE_RUNTIME_STATUS_PAYLOAD_LEN (20U)

#define USB_CAN_BRIDGE_CAN_TX_QUEUE_DEPTH (64U)
#define USB_CAN_BRIDGE_CTRL_TX_QUEUE_DEPTH (16U)
#define USB_CAN_BRIDGE_DATA_TX_QUEUE_DEPTH (128U)
#define USB_CAN_BRIDGE_RX_STREAM_CAPACITY (1024U)
#define USB_CAN_BRIDGE_HOST_LINK_TIMEOUT_MS (2000U)

#define USB_VENDOR_ID_DEV (0x1FC9U)
#define USB_PRODUCT_ID_DEV (0x0135U)
#define FW_VERSION_MAJOR (0U)
#define FW_VERSION_MINOR (3U)
#define FW_VERSION_PATCH (0U)

typedef struct
{
    uint8_t data[CAN_BRIDGE_USB_MAX_LEN];
    uint16_t length;
    uint8_t channel;
    uint8_t reserved0;
} usb_can_bridge_packet_t;

static QueueHandle_t s_CanTxQueues[kCanChannel_Count];
static QueueHandle_t s_UsbCtrlTxQueue;
static QueueHandle_t s_UsbDataTxQueue;
static uint8_t s_RxStreamBuffer[USB_CAN_BRIDGE_RX_STREAM_CAPACITY];
static uint16_t s_RxStreamLength;
static uint16_t s_UsbDataPending[kCanChannel_Count];
static uint32_t s_RxFramesAccepted;
static uint32_t s_RxFramesDroppedInvalid;
static uint32_t s_RxFramesDroppedOverflow;
static uint32_t s_ControlFramesQueued;
static uint32_t s_ControlFramesDropped;
static uint32_t s_DataFramesQueued;
static uint32_t s_DataFramesDropped;
static uint32_t s_CanTxDropped[kCanChannel_Count];
static uint32_t s_UsbTxDropped[kCanChannel_Count];
static TickType_t s_HostActivityTick;
static uint8_t s_HostConnected;

static const char *USB_CanBridgeCmdName(uint8_t command)
{
    switch (command)
    {
        case CAN_BRIDGE_CMD_SET_CHANNEL_CONFIG:
            return "SetChannelConfig";
        case CAN_BRIDGE_CMD_GET_CHANNEL_CONFIG:
            return "GetChannelConfig";
        case CAN_BRIDGE_CMD_GET_DEVICE_INFO:
            return "GetDeviceInfo";
        case CAN_BRIDGE_CMD_GET_CHANNEL_CAPS:
            return "GetChannelCaps";
        case CAN_BRIDGE_CMD_GET_RUNTIME_STATUS:
            return "GetRuntimeStatus";
        case CAN_BRIDGE_CMD_HEARTBEAT:
            return "Heartbeat";
        default:
            return "Unknown";
    }
}

static const char *USB_CanBridgeStatusName(uint8_t status)
{
    switch (status)
    {
        case CAN_BRIDGE_CFG_STATUS_OK:
            return "OK";
        case CAN_BRIDGE_CFG_STATUS_INVALID:
            return "INVALID";
        case CAN_BRIDGE_CFG_STATUS_STAGED_ONLY:
            return "STAGED";
        case CAN_BRIDGE_CFG_STATUS_INTERNAL_ERR:
            return "INTERNAL";
        default:
            return "UNKNOWN";
    }
}

static const char *USB_CanBridgeFrameFormatName(can_frame_format_t frameFormat)
{
    return (frameFormat == kCanFrameFormat_Fd) ? "FD" : "CAN";
}

static void USB_CanBridgeLogControlCommand(const char *stage, uint8_t command, uint8_t channel, uint8_t sequence, uint32_t length)
{
    PRINTF("USB ctrl %s cmd=%s(0x%02X) ch=%u seq=0x%02X len=%u\r\n",
           stage,
           USB_CanBridgeCmdName(command),
           command,
           channel,
           sequence,
           (unsigned int)length);
}

static void USB_CanBridgeWriteU32Le(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)((value >> 8U) & 0xFFU);
    dst[2] = (uint8_t)((value >> 16U) & 0xFFU);
    dst[3] = (uint8_t)((value >> 24U) & 0xFFU);
}

static void USB_CanBridgeWriteU16Le(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)((value >> 8U) & 0xFFU);
}

static uint32_t USB_CanBridgeReadU32Le(const uint8_t *src)
{
    return ((uint32_t)src[0]) | ((uint32_t)src[1] << 8U) | ((uint32_t)src[2] << 16U) | ((uint32_t)src[3] << 24U);
}

static uint16_t USB_CanBridgeReadU16Le(const uint8_t *src)
{
    return (uint16_t)(((uint16_t)src[0]) | ((uint16_t)src[1] << 8U));
}

static uint8_t USB_CanBridgeClampUBaseTypeToU8(UBaseType_t value)
{
    return (value > 0xFFU) ? 0xFFU : (uint8_t)value;
}

static uint8_t USB_CanBridgeClampU32ToU8(uint32_t value)
{
    return (value > 0xFFU) ? 0xFFU : (uint8_t)value;
}

static bool USB_CanBridgeIsNodeStateOnlyError(uint8_t errorCode)
{
    return (errorCode == 0x07U);
}

static bool USB_CanBridgeIsStateOnlyError(const can_frame_t *frameHint, bool isTx, uint8_t errorCode)
{
    if (USB_CanBridgeIsNodeStateOnlyError(errorCode))
    {
        return true;
    }
    if (isTx)
    {
        return false;
    }
    if (errorCode != 0x06U)
    {
        return false;
    }
    if (frameHint == NULL)
    {
        return true;
    }

    return (frameHint->id == 0U) && (frameHint->dlc == 0U) && (frameHint->flags == 0U);
}

static void USB_CanBridgeNoteHostActivity(TickType_t tick)
{
    taskENTER_CRITICAL();
    s_HostActivityTick = tick;
    s_HostConnected = 1U;
    taskEXIT_CRITICAL();
}

static void USB_CanBridgeClearHostActivity(void)
{
    taskENTER_CRITICAL();
    s_HostActivityTick = 0U;
    s_HostConnected = 0U;
    taskEXIT_CRITICAL();
}

static void USB_CanBridgeNoteCanTxDrop(uint8_t channel)
{
    if (channel < (uint8_t)kCanChannel_Count)
    {
        s_CanTxDropped[channel]++;
    }
}

static void USB_CanBridgeNoteUsbTxDrop(uint8_t channel)
{
    s_DataFramesDropped++;
    if (channel < (uint8_t)kCanChannel_Count)
    {
        s_UsbTxDropped[channel]++;
    }
}

static void USB_CanBridgeIncrementUsbDataPending(uint8_t channel)
{
    if (channel >= (uint8_t)kCanChannel_Count)
    {
        return;
    }

    taskENTER_CRITICAL();
    s_UsbDataPending[channel]++;
    taskEXIT_CRITICAL();
}

static void USB_CanBridgeDecrementUsbDataPending(uint8_t channel)
{
    if (channel >= (uint8_t)kCanChannel_Count)
    {
        return;
    }

    taskENTER_CRITICAL();
    if (s_UsbDataPending[channel] > 0U)
    {
        s_UsbDataPending[channel]--;
    }
    taskEXIT_CRITICAL();
}

static void USB_CanBridgeDropUsbDataPending(uint8_t channel, uint16_t count)
{
    if ((channel >= (uint8_t)kCanChannel_Count) || (count == 0U))
    {
        return;
    }

    taskENTER_CRITICAL();
    if (s_UsbDataPending[channel] > count)
    {
        s_UsbDataPending[channel] = (uint16_t)(s_UsbDataPending[channel] - count);
    }
    else
    {
        s_UsbDataPending[channel] = 0U;
    }
    taskEXIT_CRITICAL();
}

static uint8_t USB_CanBridgeGetUsbDataPending(uint8_t channel)
{
    uint16_t pending = 0U;

    if (channel >= (uint8_t)kCanChannel_Count)
    {
        return 0U;
    }

    taskENTER_CRITICAL();
    pending = s_UsbDataPending[channel];
    taskEXIT_CRITICAL();
    return (pending > 0xFFU) ? 0xFFU : (uint8_t)pending;
}

static void USB_CanBridgeFlushChannelQueues(uint8_t channel)
{
    can_bridge_msg_t canMsg;
    usb_can_bridge_packet_t packet;
    UBaseType_t pendingCount;
    UBaseType_t index;
    uint16_t removedUplink = 0U;

    if (channel >= (uint8_t)kCanChannel_Count)
    {
        return;
    }

    if (s_CanTxQueues[channel] != NULL)
    {
        while (xQueueReceive(s_CanTxQueues[channel], &canMsg, 0U) == pdPASS)
        {
            s_CanTxDropped[channel]++;
        }
    }

    if (s_UsbDataTxQueue == NULL)
    {
        return;
    }

    vTaskSuspendAll();
    pendingCount = uxQueueMessagesWaiting(s_UsbDataTxQueue);
    for (index = 0U; index < pendingCount; index++)
    {
        if (xQueueReceive(s_UsbDataTxQueue, &packet, 0U) != pdPASS)
        {
            break;
        }

        if (packet.channel == channel)
        {
            removedUplink++;
            continue;
        }

        (void)xQueueSendToBack(s_UsbDataTxQueue, &packet, 0U);
    }
    (void)xTaskResumeAll();

    USB_CanBridgeDropUsbDataPending(channel, removedUplink);
}

static bool USB_CanBridgeQueueControlPacket(const uint8_t *packet, uint32_t length)
{
    usb_can_bridge_packet_t item;

    if ((packet == NULL) || (s_UsbCtrlTxQueue == NULL) || (length == 0U) || (length > sizeof(item.data)))
    {
        return false;
    }

    (void)memset(&item, 0, sizeof(item));
    (void)memcpy(item.data, packet, length);
    item.length = (uint16_t)length;
    item.channel = packet[1];
    if (xQueueSend(s_UsbCtrlTxQueue, &item, 0U) != pdPASS)
    {
        return false;
    }

    s_ControlFramesQueued++;
    return true;
}

static bool USB_CanBridgeQueueCanMessage(const can_bridge_msg_t *msg)
{
    usb_can_bridge_packet_t item;

    if ((msg == NULL) || (s_UsbDataTxQueue == NULL))
    {
        return false;
    }

    item.length = (uint16_t)CAN_BridgeEncodeUsb(msg, item.data, sizeof(item.data));
    if (item.length == 0U)
    {
        return false;
    }

    item.channel = (uint8_t)msg->channel;
    item.reserved0 = 0U;
    if (xQueueSend(s_UsbDataTxQueue, &item, 0U) != pdPASS)
    {
        USB_CanBridgeNoteUsbTxDrop(item.channel);
        return false;
    }

    s_DataFramesQueued++;
    USB_CanBridgeIncrementUsbDataPending(item.channel);
    return true;
}

static void USB_CanBridgeDiscardRxPrefix(uint16_t count)
{
    if (count >= s_RxStreamLength)
    {
        s_RxStreamLength = 0U;
        return;
    }

    (void)memmove(s_RxStreamBuffer, &s_RxStreamBuffer[count], s_RxStreamLength - count);
    s_RxStreamLength = (uint16_t)(s_RxStreamLength - count);
}

static bool USB_CanBridgeAppendRxPacket(const uint8_t *packet, uint32_t length)
{
    if ((packet == NULL) || (length == 0U))
    {
        return false;
    }
    if (length > sizeof(s_RxStreamBuffer))
    {
        s_RxFramesDroppedOverflow++;
        s_RxStreamLength = 0U;
        return false;
    }
    if ((uint32_t)s_RxStreamLength + length > sizeof(s_RxStreamBuffer))
    {
        s_RxFramesDroppedOverflow++;
        s_RxStreamLength = 0U;
    }

    (void)memcpy(&s_RxStreamBuffer[s_RxStreamLength], packet, length);
    s_RxStreamLength = (uint16_t)(s_RxStreamLength + length);
    return true;
}

static bool USB_CanBridgeTryExtractRxFrame(uint8_t *packet, uint32_t *packetLength, uint32_t maxLength)
{
    uint32_t expectedLength;
    uint8_t dlc;
    uint16_t index;

    if ((packet == NULL) || (packetLength == NULL) || (maxLength < 8U))
    {
        return false;
    }

    while (s_RxStreamLength > 0U)
    {
        if (s_RxStreamBuffer[0] != CAN_BRIDGE_SYNC)
        {
            for (index = 1U; index < s_RxStreamLength; index++)
            {
                if (s_RxStreamBuffer[index] == CAN_BRIDGE_SYNC)
                {
                    break;
                }
            }
            s_RxFramesDroppedInvalid++;
            USB_CanBridgeDiscardRxPrefix(index);
            continue;
        }

        if (s_RxStreamLength < 8U)
        {
            return false;
        }

        dlc = s_RxStreamBuffer[2];
        if (dlc > 64U)
        {
            s_RxFramesDroppedInvalid++;
            USB_CanBridgeDiscardRxPrefix(1U);
            continue;
        }

        expectedLength = 8U + (uint32_t)dlc;
        if (s_RxStreamLength < expectedLength)
        {
            return false;
        }
        if (expectedLength > maxLength)
        {
            s_RxFramesDroppedOverflow++;
            USB_CanBridgeDiscardRxPrefix((uint16_t)expectedLength);
            continue;
        }

        (void)memcpy(packet, s_RxStreamBuffer, expectedLength);
        USB_CanBridgeDiscardRxPrefix((uint16_t)expectedLength);
        *packetLength = expectedLength;
        s_RxFramesAccepted++;
        return true;
    }

    return false;
}

static uint32_t USB_CanBridgeBuildChannelConfigPayload(const can_channel_config_t *config, uint8_t *payload, uint32_t maxLength)
{
    if ((config == NULL) || (payload == NULL) || (maxLength < CAN_BRIDGE_CFG_PAYLOAD_LEN))
    {
        return 0U;
    }

    payload[0] = CAN_BRIDGE_CFG_PROTO_VERSION;
    payload[1] = (config->frameFormat == kCanFrameFormat_Fd) ? 1U : 0U;
    payload[2] = config->enabled ? 1U : 0U;
    payload[3] = config->terminationEnabled ? 1U : 0U;
    USB_CanBridgeWriteU32Le(&payload[4], config->nominalBitrate);
    USB_CanBridgeWriteU16Le(&payload[8], config->nominalSamplePointPermille);
    USB_CanBridgeWriteU32Le(&payload[10], config->dataBitrate);
    USB_CanBridgeWriteU16Le(&payload[14], config->dataSamplePointPermille);
    return CAN_BRIDGE_CFG_PAYLOAD_LEN;
}

static bool USB_CanBridgeParseChannelConfigPayload(const uint8_t *payload, uint32_t length, can_channel_config_t *config)
{
    if ((payload == NULL) || (config == NULL) || (length != CAN_BRIDGE_CFG_PAYLOAD_LEN))
    {
        return false;
    }
    if (payload[0] != CAN_BRIDGE_CFG_PROTO_VERSION)
    {
        return false;
    }

    config->frameFormat = (payload[1] != 0U) ? kCanFrameFormat_Fd : kCanFrameFormat_Classic;
    config->enabled = (payload[2] != 0U);
    config->terminationEnabled = (payload[3] != 0U);
    config->nominalBitrate = USB_CanBridgeReadU32Le(&payload[4]);
    config->nominalSamplePointPermille = USB_CanBridgeReadU16Le(&payload[8]);
    config->dataBitrate = USB_CanBridgeReadU32Le(&payload[10]);
    config->dataSamplePointPermille = USB_CanBridgeReadU16Le(&payload[14]);
    return true;
}

static uint32_t USB_CanBridgeBuildDeviceInfoPayload(uint8_t *payload, uint32_t maxLength)
{
    if ((payload == NULL) || (maxLength < CAN_BRIDGE_DEVICE_INFO_PAYLOAD_LEN))
    {
        return 0U;
    }

    (void)memset(payload, 0, CAN_BRIDGE_DEVICE_INFO_PAYLOAD_LEN);
    payload[0] = CAN_BRIDGE_CFG_PROTO_VERSION;
    payload[1] = FW_VERSION_MAJOR;
    payload[2] = FW_VERSION_MINOR;
    payload[3] = FW_VERSION_PATCH;
    USB_CanBridgeWriteU32Le(&payload[4], CAN_StackGetFeatureFlags());
    USB_CanBridgeWriteU16Le(&payload[8], USB_VENDOR_ID_DEV);
    USB_CanBridgeWriteU16Le(&payload[10], USB_PRODUCT_ID_DEV);
    payload[12] = (uint8_t)kCanChannel_Count;
    return CAN_BRIDGE_DEVICE_INFO_PAYLOAD_LEN;
}

static uint32_t USB_CanBridgeBuildChannelCapsPayload(can_channel_t channel, uint8_t *payload, uint32_t maxLength)
{
    can_channel_capabilities_t capabilities;
    uint8_t flags = 0U;

    if ((payload == NULL) || (maxLength < CAN_BRIDGE_CHANNEL_CAPS_PAYLOAD_LEN) ||
        !CAN_StackGetChannelCapabilities(channel, &capabilities))
    {
        return 0U;
    }

    if (capabilities.supportsClassic != 0U)
    {
        flags |= 0x01U;
    }
    if (capabilities.supportsFd != 0U)
    {
        flags |= 0x02U;
    }
    if (capabilities.supportsTermination != 0U)
    {
        flags |= 0x04U;
    }

    (void)memset(payload, 0, CAN_BRIDGE_CHANNEL_CAPS_PAYLOAD_LEN);
    payload[0] = CAN_BRIDGE_CFG_PROTO_VERSION;
    payload[1] = (uint8_t)channel;
    payload[2] = flags;
    payload[3] = capabilities.driverType;
    USB_CanBridgeWriteU32Le(&payload[4], capabilities.nominalBitrateMin);
    USB_CanBridgeWriteU32Le(&payload[8], capabilities.nominalBitrateMax);
    USB_CanBridgeWriteU32Le(&payload[12], capabilities.dataBitrateMax);
    USB_CanBridgeWriteU16Le(&payload[16], capabilities.nominalSampleMinPermille);
    USB_CanBridgeWriteU16Le(&payload[18], capabilities.nominalSampleMaxPermille);
    return CAN_BRIDGE_CHANNEL_CAPS_PAYLOAD_LEN;
}

static uint32_t USB_CanBridgeBuildRuntimeStatusPayload(can_channel_t channel, uint8_t *payload, uint32_t maxLength)
{
    can_channel_runtime_status_t status;
    uint8_t flags = 0U;
    uint8_t hostPending;
    uint8_t uplinkPending;

    if ((payload == NULL) || (maxLength < CAN_BRIDGE_RUNTIME_STATUS_PAYLOAD_LEN) ||
        !CAN_StackGetChannelRuntimeStatus(channel, &status))
    {
        return 0U;
    }

    hostPending = USB_CanBridgeClampUBaseTypeToU8((s_CanTxQueues[channel] != NULL) ? uxQueueMessagesWaiting(s_CanTxQueues[channel]) : 0U);
    uplinkPending = USB_CanBridgeGetUsbDataPending((uint8_t)channel);

    if (status.ready != 0U)
    {
        flags |= 0x01U;
    }
    if (status.enabled != 0U)
    {
        flags |= 0x02U;
    }
    if (status.busOff != 0U)
    {
        flags |= 0x04U;
    }
    if (status.errorPassive != 0U)
    {
        flags |= 0x08U;
    }
    if (status.phyStandby != 0U)
    {
        flags |= 0x10U;
    }
    if ((status.rxPending != 0U) || (uplinkPending != 0U))
    {
        flags |= 0x20U;
    }
    if ((status.txPending != 0U) || (hostPending != 0U))
    {
        flags |= 0x40U;
    }

    (void)memset(payload, 0, CAN_BRIDGE_RUNTIME_STATUS_PAYLOAD_LEN);
    payload[0] = CAN_BRIDGE_CFG_PROTO_VERSION;
    payload[1] = flags;
    payload[2] = (uint8_t)(status.lastErrorCode & 0xFFU);
    USB_CanBridgeWriteU32Le(&payload[4], status.txCount);
    USB_CanBridgeWriteU32Le(&payload[8], status.rxCount);
    USB_CanBridgeWriteU32Le(&payload[12], status.lastErrorCode);
    payload[16] = hostPending;
    payload[17] = uplinkPending;
    payload[18] = USB_CanBridgeClampU32ToU8(s_CanTxDropped[channel]);
    payload[19] = USB_CanBridgeClampU32ToU8(s_UsbTxDropped[channel]);
    return CAN_BRIDGE_RUNTIME_STATUS_PAYLOAD_LEN;
}

static bool USB_CanBridgeBuildControlPacket(uint8_t channel,
                                            uint8_t command,
                                            uint8_t status,
                                            uint8_t sequence,
                                            const uint8_t *payload,
                                            uint32_t payloadLength,
                                            uint8_t *packet,
                                            uint32_t maxLength,
                                            uint32_t *packetLength)
{
    if ((packet == NULL) || (packetLength == NULL) || (payloadLength > 64U) || (maxLength < (8U + payloadLength)))
    {
        return false;
    }

    packet[0] = CAN_BRIDGE_SYNC;
    packet[1] = channel;
    packet[2] = (uint8_t)payloadLength;
    packet[3] = CAN_BRIDGE_FLAG_CONTROL;
    packet[4] = command;
    packet[5] = status;
    packet[6] = sequence;
    packet[7] = CAN_BRIDGE_CFG_PROTO_VERSION;
    if ((payloadLength > 0U) && (payload != NULL))
    {
        (void)memcpy(&packet[8], payload, payloadLength);
    }

    *packetLength = 8U + payloadLength;
    return true;
}

static bool USB_CanBridgeRespondControlStatus(uint8_t channel, uint8_t command, uint8_t status, uint8_t sequence)
{
    uint8_t response[CAN_BRIDGE_USB_MAX_LEN];
    uint32_t responseLength = 0U;

    if (!USB_CanBridgeBuildControlPacket(channel, command, status, sequence, NULL, 0U, response, sizeof(response), &responseLength))
    {
        return false;
    }

    return USB_CanBridgeQueueControlPacket(response, responseLength);
}

static bool USB_CanBridgeRespondChannelConfig(uint8_t channel, uint8_t command, uint8_t status, uint8_t sequence)
{
    uint8_t response[CAN_BRIDGE_USB_MAX_LEN];
    uint8_t payload[CAN_BRIDGE_CFG_PAYLOAD_LEN];
    can_channel_config_t applied;
    uint32_t responseLength = 0U;

    if ((channel >= (uint8_t)kCanChannel_Count) || !CAN_StackGetChannelConfig((can_channel_t)channel, &applied))
    {
        return USB_CanBridgeRespondControlStatus(channel, command, CAN_BRIDGE_CFG_STATUS_INVALID, sequence);
    }
    if (USB_CanBridgeBuildChannelConfigPayload(&applied, payload, sizeof(payload)) == 0U)
    {
        return USB_CanBridgeRespondControlStatus(channel, command, CAN_BRIDGE_CFG_STATUS_INTERNAL_ERR, sequence);
    }
    if (!USB_CanBridgeBuildControlPacket(channel, command, status, sequence, payload, sizeof(payload), response, sizeof(response),
                                         &responseLength))
    {
        return false;
    }
    PRINTF("USB ctrl rsp cmd=%s ch=%u seq=0x%02X status=%s fmt=%s en=%u term=%u n=%u/%u d=%u/%u\r\n",
           USB_CanBridgeCmdName(command),
           channel,
           sequence,
           USB_CanBridgeStatusName(status),
           USB_CanBridgeFrameFormatName(applied.frameFormat),
           applied.enabled ? 1U : 0U,
           applied.terminationEnabled ? 1U : 0U,
           applied.nominalBitrate,
           applied.nominalSamplePointPermille,
           applied.dataBitrate,
           applied.dataSamplePointPermille);
    return USB_CanBridgeQueueControlPacket(response, responseLength);
}

static bool USB_CanBridgeHandleChannelConfigPacket(const uint8_t *packet, uint32_t length)
{
    can_channel_config_t requested;
    uint8_t status = CAN_BRIDGE_CFG_STATUS_INTERNAL_ERR;
    uint8_t channel;

    if ((packet == NULL) || (length != (8U + CAN_BRIDGE_CFG_PAYLOAD_LEN)))
    {
        return false;
    }

    channel = packet[1];
    USB_CanBridgeLogControlCommand("req", CAN_BRIDGE_CMD_SET_CHANNEL_CONFIG, channel, packet[6], packet[2]);
    if (channel >= (uint8_t)kCanChannel_Count)
    {
        return USB_CanBridgeRespondControlStatus(channel, CAN_BRIDGE_CMD_SET_CHANNEL_CONFIG, CAN_BRIDGE_CFG_STATUS_INVALID, packet[6]);
    }
    if (!USB_CanBridgeParseChannelConfigPayload(&packet[8], packet[2], &requested))
    {
        return USB_CanBridgeRespondChannelConfig(channel, CAN_BRIDGE_CMD_SET_CHANNEL_CONFIG, CAN_BRIDGE_CFG_STATUS_INVALID, packet[6]);
    }
    if (!CAN_StackApplyChannelConfig((can_channel_t)channel, &requested, &status))
    {
        status = CAN_BRIDGE_CFG_STATUS_INVALID;
    }
    else if ((status == CAN_BRIDGE_CFG_STATUS_OK) || (status == CAN_BRIDGE_CFG_STATUS_STAGED_ONLY))
    {
        USB_CanBridgeFlushChannelQueues(channel);
    }

    return USB_CanBridgeRespondChannelConfig(channel, CAN_BRIDGE_CMD_SET_CHANNEL_CONFIG, status, packet[6]);
}

static bool USB_CanBridgeHandleGetChannelConfigPacket(const uint8_t *packet, uint32_t length)
{
    uint8_t channel;

    if ((packet == NULL) || (length != 8U))
    {
        return false;
    }

    channel = packet[1];
    USB_CanBridgeLogControlCommand("req", CAN_BRIDGE_CMD_GET_CHANNEL_CONFIG, channel, packet[6], 0U);
    if (channel >= (uint8_t)kCanChannel_Count)
    {
        return USB_CanBridgeRespondControlStatus(channel, CAN_BRIDGE_CMD_GET_CHANNEL_CONFIG, CAN_BRIDGE_CFG_STATUS_INVALID, packet[6]);
    }

    return USB_CanBridgeRespondChannelConfig(channel, CAN_BRIDGE_CMD_GET_CHANNEL_CONFIG, CAN_BRIDGE_CFG_STATUS_OK, packet[6]);
}

static bool USB_CanBridgeHandleGetDeviceInfoPacket(const uint8_t *packet, uint32_t length)
{
    uint8_t response[CAN_BRIDGE_USB_MAX_LEN];
    uint8_t payload[CAN_BRIDGE_DEVICE_INFO_PAYLOAD_LEN];
    uint32_t responseLength = 0U;
    uint32_t payloadLength;

    if ((packet == NULL) || (length != 8U))
    {
        return false;
    }

    USB_CanBridgeLogControlCommand("req", CAN_BRIDGE_CMD_GET_DEVICE_INFO, packet[1], packet[6], 0U);
    payloadLength = USB_CanBridgeBuildDeviceInfoPayload(payload, sizeof(payload));
    if ((payloadLength == 0U) ||
        !USB_CanBridgeBuildControlPacket(packet[1], CAN_BRIDGE_CMD_GET_DEVICE_INFO, CAN_BRIDGE_CFG_STATUS_OK, packet[6], payload,
                                         payloadLength, response, sizeof(response), &responseLength))
    {
        return false;
    }

    return USB_CanBridgeQueueControlPacket(response, responseLength);
}

static bool USB_CanBridgeHandleGetChannelCapsPacket(const uint8_t *packet, uint32_t length)
{
    uint8_t response[CAN_BRIDGE_USB_MAX_LEN];
    uint8_t payload[CAN_BRIDGE_CHANNEL_CAPS_PAYLOAD_LEN];
    uint32_t responseLength = 0U;
    uint32_t payloadLength;
    uint8_t channel;

    if ((packet == NULL) || (length != 8U))
    {
        return false;
    }

    channel = packet[1];
    USB_CanBridgeLogControlCommand("req", CAN_BRIDGE_CMD_GET_CHANNEL_CAPS, channel, packet[6], 0U);
    if (channel >= (uint8_t)kCanChannel_Count)
    {
        return USB_CanBridgeRespondControlStatus(channel, CAN_BRIDGE_CMD_GET_CHANNEL_CAPS, CAN_BRIDGE_CFG_STATUS_INVALID, packet[6]);
    }

    payloadLength = USB_CanBridgeBuildChannelCapsPayload((can_channel_t)channel, payload, sizeof(payload));
    if ((payloadLength == 0U) ||
        !USB_CanBridgeBuildControlPacket(channel, CAN_BRIDGE_CMD_GET_CHANNEL_CAPS, CAN_BRIDGE_CFG_STATUS_OK, packet[6], payload,
                                         payloadLength, response, sizeof(response), &responseLength))
    {
        return false;
    }

    PRINTF("USB ctrl rsp cmd=%s ch=%u seq=0x%02X flags=0x%02X drv=%u n=%u..%u dmax=%u\r\n",
           USB_CanBridgeCmdName(CAN_BRIDGE_CMD_GET_CHANNEL_CAPS),
           channel,
           packet[6],
           payload[2],
           payload[3],
           (unsigned int)USB_CanBridgeReadU32Le(&payload[4]),
           (unsigned int)USB_CanBridgeReadU32Le(&payload[8]),
           (unsigned int)USB_CanBridgeReadU32Le(&payload[12]));

    return USB_CanBridgeQueueControlPacket(response, responseLength);
}

static bool USB_CanBridgeHandleGetRuntimeStatusPacket(const uint8_t *packet, uint32_t length)
{
    uint8_t response[CAN_BRIDGE_USB_MAX_LEN];
    uint8_t payload[CAN_BRIDGE_RUNTIME_STATUS_PAYLOAD_LEN];
    uint32_t responseLength = 0U;
    uint32_t payloadLength;
    uint8_t channel;

    if ((packet == NULL) || (length != 8U))
    {
        return false;
    }

    channel = packet[1];
    USB_CanBridgeLogControlCommand("req", CAN_BRIDGE_CMD_GET_RUNTIME_STATUS, channel, packet[6], 0U);
    if (channel >= (uint8_t)kCanChannel_Count)
    {
        return USB_CanBridgeRespondControlStatus(channel, CAN_BRIDGE_CMD_GET_RUNTIME_STATUS, CAN_BRIDGE_CFG_STATUS_INVALID, packet[6]);
    }

    payloadLength = USB_CanBridgeBuildRuntimeStatusPayload((can_channel_t)channel, payload, sizeof(payload));
    if ((payloadLength == 0U) ||
        !USB_CanBridgeBuildControlPacket(channel, CAN_BRIDGE_CMD_GET_RUNTIME_STATUS, CAN_BRIDGE_CFG_STATUS_OK, packet[6], payload,
                                         payloadLength, response, sizeof(response), &responseLength))
    {
        return false;
    }

    PRINTF("USB ctrl rsp cmd=%s ch=%u seq=0x%02X flags=0x%02X tx=%u rx=%u err=0x%08X hostQ=%u usbQ=%u hostDrop=%u usbDrop=%u\r\n",
           USB_CanBridgeCmdName(CAN_BRIDGE_CMD_GET_RUNTIME_STATUS),
           channel,
           packet[6],
           payload[1],
           (unsigned int)USB_CanBridgeReadU32Le(&payload[4]),
           (unsigned int)USB_CanBridgeReadU32Le(&payload[8]),
           (unsigned int)USB_CanBridgeReadU32Le(&payload[12]),
           payload[16],
           payload[17],
           payload[18],
           payload[19]);

    return USB_CanBridgeQueueControlPacket(response, responseLength);
}

static bool USB_CanBridgeHandleHeartbeatPacket(const uint8_t *packet, uint32_t length)
{
    uint8_t response[CAN_BRIDGE_USB_MAX_LEN];
    uint32_t responseLength = 0U;
    uint32_t payloadLength = 0U;

    if ((packet == NULL) || (length < 8U))
    {
        return false;
    }

    payloadLength = packet[2];
    if ((8U + payloadLength) != length)
    {
        return false;
    }

    if (!((payloadLength == 4U) && ((memcmp(&packet[8], "LINK", 4U) == 0) || (memcmp(&packet[8], "UNLK", 4U) == 0))))
    {
        USB_CanBridgeLogControlCommand("req", CAN_BRIDGE_CMD_HEARTBEAT, packet[1], packet[6], payloadLength);
    }

    if ((payloadLength == 4U) && (memcmp(&packet[8], "UNLK", 4U) == 0))
    {
        USB_CanBridgeClearHostActivity();
    }

    if (!USB_CanBridgeBuildControlPacket(packet[1], CAN_BRIDGE_CMD_HEARTBEAT, CAN_BRIDGE_CFG_STATUS_OK, packet[6], &packet[8],
                                         payloadLength, response, sizeof(response), &responseLength))
    {
        return false;
    }

    return USB_CanBridgeQueueControlPacket(response, responseLength);
}

static bool USB_CanBridgeConsumeControlPacket(const uint8_t *packet, uint32_t length, TickType_t tick)
{
    if ((packet == NULL) || (length < 8U) || (packet[0] != CAN_BRIDGE_SYNC) || ((packet[3] & CAN_BRIDGE_FLAG_CONTROL) == 0U))
    {
        return false;
    }
    if ((length != (8U + packet[2])) || (packet[7] != CAN_BRIDGE_CFG_PROTO_VERSION))
    {
        s_RxFramesDroppedInvalid++;
        return true;
    }

    USB_CanBridgeNoteHostActivity(tick);

    switch (packet[4])
    {
        case CAN_BRIDGE_CMD_SET_CHANNEL_CONFIG:
            if (!USB_CanBridgeHandleChannelConfigPacket(packet, length))
            {
                s_ControlFramesDropped++;
            }
            return true;
        case CAN_BRIDGE_CMD_GET_CHANNEL_CONFIG:
            if (!USB_CanBridgeHandleGetChannelConfigPacket(packet, length))
            {
                s_ControlFramesDropped++;
            }
            return true;
        case CAN_BRIDGE_CMD_GET_DEVICE_INFO:
            if (!USB_CanBridgeHandleGetDeviceInfoPacket(packet, length))
            {
                s_ControlFramesDropped++;
            }
            return true;
        case CAN_BRIDGE_CMD_GET_CHANNEL_CAPS:
            if (!USB_CanBridgeHandleGetChannelCapsPacket(packet, length))
            {
                s_ControlFramesDropped++;
            }
            return true;
        case CAN_BRIDGE_CMD_GET_RUNTIME_STATUS:
            if (!USB_CanBridgeHandleGetRuntimeStatusPacket(packet, length))
            {
                s_ControlFramesDropped++;
            }
            return true;
        case CAN_BRIDGE_CMD_HEARTBEAT:
            if (!USB_CanBridgeHandleHeartbeatPacket(packet, length))
            {
                s_ControlFramesDropped++;
            }
            return true;
        default:
            if (!USB_CanBridgeRespondControlStatus(packet[1], packet[4], CAN_BRIDGE_CFG_STATUS_INVALID, packet[6]))
            {
                s_ControlFramesDropped++;
            }
            return true;
    }
}

bool USB_CanBridgeInit(void)
{
    uint32_t i;

    (void)memset(s_CanTxQueues, 0, sizeof(s_CanTxQueues));
    (void)memset(s_UsbDataPending, 0, sizeof(s_UsbDataPending));
    (void)memset(s_CanTxDropped, 0, sizeof(s_CanTxDropped));
    (void)memset(s_UsbTxDropped, 0, sizeof(s_UsbTxDropped));
    s_UsbCtrlTxQueue = NULL;
    s_UsbDataTxQueue = NULL;
    s_RxStreamLength = 0U;
    s_RxFramesAccepted = 0U;
    s_RxFramesDroppedInvalid = 0U;
    s_RxFramesDroppedOverflow = 0U;
    s_ControlFramesQueued = 0U;
    s_ControlFramesDropped = 0U;
    s_DataFramesQueued = 0U;
    s_DataFramesDropped = 0U;
    s_HostActivityTick = 0U;
    s_HostConnected = 0U;

    for (i = 0U; i < (uint32_t)kCanChannel_Count; i++)
    {
        s_CanTxQueues[i] = xQueueCreate(USB_CAN_BRIDGE_CAN_TX_QUEUE_DEPTH, sizeof(can_bridge_msg_t));
        if (s_CanTxQueues[i] == NULL)
        {
            return false;
        }
    }

    s_UsbCtrlTxQueue = xQueueCreate(USB_CAN_BRIDGE_CTRL_TX_QUEUE_DEPTH, sizeof(usb_can_bridge_packet_t));
    if (s_UsbCtrlTxQueue == NULL)
    {
        return false;
    }

    s_UsbDataTxQueue = xQueueCreate(USB_CAN_BRIDGE_DATA_TX_QUEUE_DEPTH, sizeof(usb_can_bridge_packet_t));
    if (s_UsbDataTxQueue == NULL)
    {
        return false;
    }

    return true;
}

bool USB_CanBridgeIsHostConnected(void)
{
    TickType_t nowTick;
    TickType_t lastTick;
    uint8_t connected;

    if (!USB_VendorBulkIsConfigured())
    {
        USB_CanBridgeClearHostActivity();
        return false;
    }

    taskENTER_CRITICAL();
    lastTick = s_HostActivityTick;
    connected = s_HostConnected;
    taskEXIT_CRITICAL();

    if (connected == 0U)
    {
        return false;
    }

    nowTick = xTaskGetTickCount();
    return (nowTick - lastTick) <= pdMS_TO_TICKS(USB_CAN_BRIDGE_HOST_LINK_TIMEOUT_MS);
}

QueueHandle_t USB_CanBridgeGetCanTxQueue(can_channel_t channel)
{
    if ((uint8_t)channel >= (uint8_t)kCanChannel_Count)
    {
        return NULL;
    }

    return s_CanTxQueues[channel];
}

void USB_CanBridgeGetStats(usb_can_bridge_stats_t *stats)
{
    uint32_t i;

    if (stats == NULL)
    {
        return;
    }

    (void)memset(stats, 0, sizeof(*stats));
    stats->rxFramesAccepted = s_RxFramesAccepted;
    stats->rxFramesDroppedInvalid = s_RxFramesDroppedInvalid;
    stats->rxFramesDroppedOverflow = s_RxFramesDroppedOverflow;
    stats->controlFramesQueued = s_ControlFramesQueued;
    stats->controlFramesDropped = s_ControlFramesDropped;
    stats->dataFramesQueued = s_DataFramesQueued;
    stats->dataFramesDropped = s_DataFramesDropped;
    stats->rxStreamBytes = s_RxStreamLength;
    stats->controlPending = USB_CanBridgeClampUBaseTypeToU8((s_UsbCtrlTxQueue != NULL) ? uxQueueMessagesWaiting(s_UsbCtrlTxQueue) : 0U);

    for (i = 0U; i < (uint32_t)kCanChannel_Count; i++)
    {
        stats->canTxPending[i] = (uint16_t)((s_CanTxQueues[i] != NULL) ? uxQueueMessagesWaiting(s_CanTxQueues[i]) : 0U);
        stats->usbTxPending[i] = s_UsbDataPending[i];
        stats->canTxDropped[i] = s_CanTxDropped[i];
        stats->usbTxDropped[i] = s_UsbTxDropped[i];
    }
}

void USB_CanBridgeRunRxStep(uint64_t tick)
{
    uint8_t usbPacket[USB_VENDOR_BULK_MAX_PACKET];
    uint8_t framePacket[CAN_BRIDGE_USB_MAX_LEN];
    uint32_t usbLength = 0U;
    uint32_t frameLength = 0U;
    can_bridge_msg_t msg;
    QueueHandle_t q;
    TickType_t tickCount;

    tickCount = (TickType_t)tick;

    USB_VendorBulkTask();
    USB_VendorBulkUpdateHwTick(tick);

    while (USB_VendorBulkPopRxPacket(usbPacket, &usbLength, sizeof(usbPacket)))
    {
        if ((usbLength == 0U) || !USB_CanBridgeAppendRxPacket(usbPacket, usbLength))
        {
            usbLength = 0U;
            continue;
        }

        while (USB_CanBridgeTryExtractRxFrame(framePacket, &frameLength, sizeof(framePacket)))
        {
            if (USB_CanBridgeConsumeControlPacket(framePacket, frameLength, tickCount))
            {
                frameLength = 0U;
                continue;
            }

            if (!CAN_BridgeDecodeUsb(framePacket, frameLength, &msg) || !CAN_BridgeNormalizeHostTx(&msg))
            {
                s_RxFramesDroppedInvalid++;
                if (framePacket[1] < (uint8_t)kCanChannel_Count)
                {
                    USB_CanBridgeNoteCanTxDrop(framePacket[1]);
                }
                frameLength = 0U;
                continue;
            }

            q = USB_CanBridgeGetCanTxQueue(msg.channel);
            if ((q == NULL) || (xQueueSend(q, &msg, 0U) != pdPASS))
            {
                USB_CanBridgeNoteCanTxDrop((uint8_t)msg.channel);
            }
            frameLength = 0U;
        }

        usbLength = 0U;
    }
}

void USB_CanBridgeRunTxStep(TickType_t waitTicks)
{
    usb_can_bridge_packet_t item;
    BaseType_t hasItem;
    bool isControl = false;
    TickType_t dataWaitTicks = waitTicks;

    if (dataWaitTicks == portMAX_DELAY)
    {
        dataWaitTicks = pdMS_TO_TICKS(2);
    }

    if (s_UsbCtrlTxQueue != NULL)
    {
        hasItem = xQueueReceive(s_UsbCtrlTxQueue, &item, 0U);
        if (hasItem == pdPASS)
        {
            isControl = true;
        }
    }
    else
    {
        hasItem = pdFALSE;
    }

    if ((hasItem != pdPASS) && ((s_UsbDataTxQueue == NULL) || (xQueueReceive(s_UsbDataTxQueue, &item, dataWaitTicks) != pdPASS)))
    {
        return;
    }

    if (!isControl)
    {
        USB_CanBridgeDecrementUsbDataPending(item.channel);
    }

    if (!USB_VendorBulkIsConfigured())
    {
        if (isControl)
        {
            s_ControlFramesDropped++;
        }
        else
        {
            USB_CanBridgeNoteUsbTxDrop(item.channel);
        }
        return;
    }

    while (!USB_VendorBulkSendPacket(item.data, item.length))
    {
        if (!USB_VendorBulkIsConfigured())
        {
            if (isControl)
            {
                s_ControlFramesDropped++;
            }
            else
            {
                USB_CanBridgeNoteUsbTxDrop(item.channel);
            }
            return;
        }
        USB_VendorBulkTask();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

bool USB_CanBridgePostCanTxResult(const can_bridge_msg_t *txReq, bool sendOk, uint8_t errorCode)
{
    can_bridge_msg_t uplinkMsg;

    if ((txReq == NULL) || (s_UsbDataTxQueue == NULL))
    {
        return false;
    }

    if (sendOk)
    {
        CAN_BridgeBuildTxEcho(txReq, &uplinkMsg);
    }
    else
    {
        if (USB_CanBridgeIsNodeStateOnlyError(errorCode))
        {
            return true;
        }
        CAN_BridgeBuildError(txReq->channel, txReq->frame.id, txReq->frame.flags, true, errorCode, &uplinkMsg);
    }

    return USB_CanBridgeQueueCanMessage(&uplinkMsg);
}

bool USB_CanBridgePostCanRxFrame(can_channel_t channel, const can_frame_t *rxFrame)
{
    can_bridge_msg_t uplinkMsg;

    if ((rxFrame == NULL) || (s_UsbDataTxQueue == NULL))
    {
        return false;
    }

    CAN_BridgeBuildRxUplink(channel, rxFrame, &uplinkMsg);
    return USB_CanBridgeQueueCanMessage(&uplinkMsg);
}

bool USB_CanBridgePostCanError(can_channel_t channel, const can_frame_t *frameHint, bool isTx, uint8_t errorCode)
{
    can_bridge_msg_t uplinkMsg;
    uint32_t id = 0U;
    uint8_t baseFlags = 0U;

    if (s_UsbDataTxQueue == NULL)
    {
        return false;
    }
    if (USB_CanBridgeIsStateOnlyError(frameHint, isTx, errorCode))
    {
        return true;
    }
    if (frameHint != NULL)
    {
        id = frameHint->id;
        baseFlags = frameHint->flags;
    }

    CAN_BridgeBuildError(channel, id, baseFlags, isTx, errorCode, &uplinkMsg);
    return USB_CanBridgeQueueCanMessage(&uplinkMsg);
}
