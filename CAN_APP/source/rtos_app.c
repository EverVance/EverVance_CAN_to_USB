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
    can_frame_t rxFrame;
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
            tickMs = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            if (sendOk)
            {
                BSP_NotifyCanTxActivity(msg.channel, tickMs);
            }
            (void)USB_CanBridgePostCanTxResult(&msg, sendOk, 0x8U);
        }

        while (CAN_StackReceive(channel, &rxFrame))
        {
            tickMs = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            BSP_NotifyCanRxActivity(channel, tickMs);
            (void)USB_CanBridgePostCanRxFrame(channel, &rxFrame);
        }

        CAN_StackTaskChannel(channel);
    }
}

static void Monitor_Task(void *arg)
{
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
            PRINTF("rtos alive; usb cfg=%u host=%u rx=%u drop=%u tx=%u busy=%u | bridge ok=%u bad=%u ovf=%u ctrlPend=%u ctrlDrop=%u dataDrop=%u canPend=%u,%u,%u,%u usbPend=%u,%u,%u,%u canDrop=%u,%u,%u,%u usbDrop=%u,%u,%u,%u | canfd1 txDrop=%u txHwFail=%u rxDrop=%u\r\n",
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

