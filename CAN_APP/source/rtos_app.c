#include "rtos_app.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include <stdint.h>

#include "can_bridge.h"
#include "can_stack.h"
#include "canfd1_ext_spi.h"
#include "fsl_debug_console.h"
#include "bsp_peripherals.h"
#include "usb_charger_detect.h"
#include "usb_can_bridge.h"
#include "usb_vendor_bulk.h"

static const char *s_CanTaskNames[kCanChannel_Count] = {
    "canfd1",
    "canfd2",
    "can1",
    "can2",
};

static void USB_Task(void *arg);
static void USB_TxTask(void *arg);
static void CAN_ChannelTask(void *arg);
static void Monitor_Task(void *arg);
static void Startup_Task(void *arg);
static void RTOS_UpdateStatusLeds(uint32_t tickMs);
static uint8_t RTOS_GetChannelSendFailureCode(can_channel_t channel);

static void RTOS_FatalLoop(const char *reason)
{
    PRINTF("%s\r\n", reason);
    taskDISABLE_INTERRUPTS();
    for (;;)
    {
    }
}

static void RTOS_CreateTaskOrDie(TaskFunction_t entry,
                                 const char *name,
                                 configSTACK_DEPTH_TYPE stackDepth,
                                 void *arg,
                                 UBaseType_t priority)
{
    if (xTaskCreate(entry, name, stackDepth, arg, priority, NULL) != pdPASS)
    {
        RTOS_FatalLoop("task create failed");
    }
}

static void RTOS_CreateWorkerTasks(void)
{
    uint32_t i;

    RTOS_CreateTaskOrDie(USB_Task, "usb", 1024, NULL, tskIDLE_PRIORITY + 3U);
    RTOS_CreateTaskOrDie(USB_TxTask, "usb_tx", 768, NULL, tskIDLE_PRIORITY + 3U);

    for (i = 0U; i < (uint32_t)kCanChannel_Count; i++)
    {
        RTOS_CreateTaskOrDie(CAN_ChannelTask,
                             s_CanTaskNames[i],
                             768,
                             (void *)(uintptr_t)i,
                             tskIDLE_PRIORITY + 2U);
    }

    RTOS_CreateTaskOrDie(Monitor_Task, "mon", 768, NULL, tskIDLE_PRIORITY + 1U);
}

static void RTOS_UpdateStatusLeds(uint32_t tickMs)
{
    BSP_UpdateCanActivityLeds(tickMs);
    BSP_SetStatusLed(kBspStatusLed5, USB_VendorBulkIsConfigured());
    BSP_SetStatusLed(kBspStatusLed6, USB_CanBridgeIsHostConnected());
}

static uint8_t RTOS_GetChannelSendFailureCode(can_channel_t channel)
{
    can_channel_runtime_status_t status;

    if (CAN_StackGetChannelRuntimeStatus(channel, &status))
    {
        return (uint8_t)(status.lastErrorCode & 0xFFU);
    }

    return 0U;
}

static void USB_Task(void *arg)
{
    TickType_t lastWakeTime;

    (void)arg;
    lastWakeTime = xTaskGetTickCount();
    for (;;)
    {
        USB_CanBridgeRunRxStep((uint64_t)xTaskGetTickCount());
        vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(1));
    }
}

static void USB_TxTask(void *arg)
{
    (void)arg;
    for (;;)
    {
        USB_CanBridgeRunTxStep(portMAX_DELAY);
    }
}

static void CAN_ChannelTask(void *arg)
{
    can_channel_t channel = (can_channel_t)(uintptr_t)arg;
    QueueHandle_t q;
    can_bridge_msg_t msg;
    can_bus_event_t event;
    bool sendOk;
    uint32_t tickMs;

    if ((uint8_t)channel >= (uint8_t)kCanChannel_Count)
    {
        vTaskDelete(NULL);
    }

    q = USB_CanBridgeGetCanTxQueue(channel);
    for (;;)
    {
        if ((q != NULL) && (xQueueReceive(q, &msg, pdMS_TO_TICKS(10)) == pdPASS))
        {
            sendOk = CAN_StackSend(msg.channel, &msg.frame);
            if (!sendOk)
            {
                (void)USB_CanBridgePostCanTxResult(&msg, false, RTOS_GetChannelSendFailureCode(msg.channel));
            }
        }

        CAN_StackTaskChannel(channel);

        while (CAN_StackPollEvent(channel, &event))
        {
            tickMs = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            if (event.type == kCanBusEvent_RxFrame)
            {
                BSP_NotifyCanRxActivity(channel, tickMs);
                (void)USB_CanBridgePostCanRxFrame(channel, &event.frame);
            }
            else if (event.type == kCanBusEvent_TxComplete)
            {
                BSP_NotifyCanTxActivity(channel, tickMs);
                msg.channel = channel;
                msg.frame = event.frame;
                (void)USB_CanBridgePostCanTxResult(&msg, true, 0U);
            }
            else if (event.type == kCanBusEvent_Error)
            {
                (void)USB_CanBridgePostCanError(channel, &event.frame, event.isTx != 0U, event.errorCode);
            }
        }
    }
}

static void Monitor_Task(void *arg)
{
    can_channel_config_t ch0Config;
    can_channel_runtime_status_t ch0Status;
    canfd1_ext_stats_t canfd1Stats;
    usb_can_bridge_stats_t bridgeStats;
    usb_vendor_bulk_stats_t usbStats;
    TickType_t nextLogTick;

    (void)arg;
    nextLogTick = xTaskGetTickCount();

    for (;;)
    {
        CANFD1_ExtSpiGetStats(&canfd1Stats);
        USB_CanBridgeGetStats(&bridgeStats);
        USB_VendorBulkGetStats(&usbStats);
        RTOS_UpdateStatusLeds((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));

        if ((int32_t)(xTaskGetTickCount() - nextLogTick) >= 0)
        {
            (void)CAN_StackGetChannelConfig(kCanChannel_CanFd1Ext, &ch0Config);
            (void)CAN_StackGetChannelRuntimeStatus(kCanChannel_CanFd1Ext, &ch0Status);
            PRINTF("rtos alive; usb cfg=%u host=%u rx=%u drop=%u tx=%u busy=%u | bridge ok=%u bad=%u ovf=%u ctrlPend=%u ctrlDrop=%u dataDrop=%u canPend=%u,%u,%u,%u usbPend=%u,%u,%u,%u canDrop=%u,%u,%u,%u usbDrop=%u,%u,%u,%u | ch0 fmt=%s en=%u term=%u n=%u/%u d=%u/%u phy=%u bo=%u ep=%u lastErr=0x%08X | canfd1 txDrop=%u txHwFail=%u rxDrop=%u\r\n",
                   usbStats.configured,
                   USB_CanBridgeIsHostConnected() ? 1U : 0U,
                   usbStats.rxPackets,
                   usbStats.rxDropped,
                   usbStats.txPackets,
                   usbStats.txBusyDrop,
                   bridgeStats.rxFramesAccepted,
                   bridgeStats.rxFramesDroppedInvalid,
                   bridgeStats.rxFramesDroppedOverflow,
                   (uint32_t)bridgeStats.controlPending,
                   bridgeStats.controlFramesDropped,
                   bridgeStats.dataFramesDropped,
                   (uint32_t)bridgeStats.canTxPending[0],
                   (uint32_t)bridgeStats.canTxPending[1],
                   (uint32_t)bridgeStats.canTxPending[2],
                   (uint32_t)bridgeStats.canTxPending[3],
                   (uint32_t)bridgeStats.usbTxPending[0],
                   (uint32_t)bridgeStats.usbTxPending[1],
                   (uint32_t)bridgeStats.usbTxPending[2],
                   (uint32_t)bridgeStats.usbTxPending[3],
                   bridgeStats.canTxDropped[0],
                   bridgeStats.canTxDropped[1],
                   bridgeStats.canTxDropped[2],
                   bridgeStats.canTxDropped[3],
                   bridgeStats.usbTxDropped[0],
                   bridgeStats.usbTxDropped[1],
                   bridgeStats.usbTxDropped[2],
                   bridgeStats.usbTxDropped[3],
                   (ch0Config.frameFormat == kCanFrameFormat_Fd) ? "FD" : "CAN",
                   ch0Config.enabled ? 1U : 0U,
                   ch0Config.terminationEnabled ? 1U : 0U,
                   ch0Config.nominalBitrate,
                   ch0Config.nominalSamplePointPermille,
                   ch0Config.dataBitrate,
                   ch0Config.dataSamplePointPermille,
                   ch0Status.phyStandby,
                   ch0Status.busOff,
                   ch0Status.errorPassive,
                   (unsigned int)ch0Status.lastErrorCode,
                   canfd1Stats.txDropCount,
                   canfd1Stats.txHwFailCount,
                   canfd1Stats.rxDropCount);
            nextLogTick = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void Startup_Task(void *arg)
{
    bool canReady;

    (void)arg;
    PRINTF("rtos startup task begin\r\n");

    USB_VendorBulkInit();

    canReady = CAN_StackInit();
    PRINTF("CAN stack init: %s\r\n", canReady ? "ok" : "fail");

    RTOS_CreateWorkerTasks();
    PRINTF("rtos worker tasks ready\r\n");

    vTaskDelete(NULL);
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    (void)pcTaskName;
    taskDISABLE_INTERRUPTS();
    for (;;)
    {
    }
}

void vApplicationMallocFailedHook(void)
{
    taskDISABLE_INTERRUPTS();
    for (;;)
    {
    }
}

void RTOS_AppStart(void)
{
    uint32_t i;

    if (!USB_CanBridgeInit())
    {
        RTOS_FatalLoop("rtos usb-can bridge init failed");
    }

    for (i = 0U; i < (uint32_t)kCanChannel_Count; i++)
    {
        if (USB_CanBridgeGetCanTxQueue((can_channel_t)i) == NULL)
        {
            PRINTF("rtos bridge queue[%u] create failed\r\n", i);
            RTOS_FatalLoop("rtos can queue create failed");
        }
    }

    RTOS_CreateTaskOrDie(Startup_Task, "startup", 1024, NULL, tskIDLE_PRIORITY + 4U);

    vTaskStartScheduler();

    RTOS_FatalLoop("scheduler exited unexpectedly");
}

