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

/* 文件说明：
 * 本文件是设备端 RTOS 组织层，负责把 USB、CAN、灯语、日志等模块装配成
 * 任务系统。连接/断开、会话初始化、LED 周期更新等跨模块问题通常都在这里收敛。 */

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
static void Led_Task(void *arg);
static void Startup_Task(void *arg);
static void RTOS_UpdateStatusLeds(uint32_t tickMs);
static uint8_t RTOS_GetChannelSendFailureCode(can_channel_t channel);
static void RTOS_UpdateDisconnectedLedChase(uint32_t tickMs);
static void RTOS_HandleHostLinkTransition(bool connected);

static uint32_t s_HostLinkGeneration = 1U;
static uint32_t s_ChannelRecoveredGeneration[kCanChannel_Count];

/* 致命错误兜底。
 * 若系统连任务都创建失败，继续运行只会把问题掩盖，因此直接停机并留串口日志。 */
static void RTOS_FatalLoop(const char *reason)
{
    PRINTF("%s\r\n", reason);
    taskDISABLE_INTERRUPTS();
    for (;;)
    {
    }
}

/* 统一创建任务并在失败时立即停机，避免调用点分散写样板代码。 */
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

/* 创建设备端所有后台任务。
 * 优先级设计：
 * - USB RX/TX 最高，避免上位机收发被低优先级灯语影响
 * - 各 CAN 通道次之
 * - LED/Monitor 放在较低优先级，只做状态展示 */
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

    RTOS_CreateTaskOrDie(Led_Task, "led", 512, NULL, tskIDLE_PRIORITY + 1U);
    RTOS_CreateTaskOrDie(Monitor_Task, "mon", 768, NULL, tskIDLE_PRIORITY + 1U);
}

/* 处理 Host 会话连断切换。
 * 会话层面断开时需要：
 * - 清灯状态
 * - 清桥接层残留队列
 * - 触发各通道进入新一轮 host generation */
static void RTOS_HandleHostLinkTransition(bool connected)
{
    uint32_t i;

    BSP_ResetCanLedState();
    USB_CanBridgeHandleHostLinkState(connected);
    if (!connected)
    {
        s_HostLinkGeneration++;
        for (i = 0U; i < (uint32_t)kCanChannel_Count; i++)
        {
            s_ChannelRecoveredGeneration[i] = 0U;
        }
    }
}

/* 更新所有状态灯。
 * 当前策略：
 * - 未连 Host：跑马灯
 * - 已连 Host：CH0~CH3 进入通道灯语，LED5/6 表示链路状态 */
static void RTOS_UpdateStatusLeds(uint32_t tickMs)
{
    bool channelEnabled[kCanChannel_Count] = {false};
    bool channelBusOff[kCanChannel_Count] = {false};
    uint32_t i;

    if (!USB_CanBridgeIsHostConnected())
    {
        RTOS_UpdateDisconnectedLedChase(tickMs);
        return;
    }

    for (i = 0U; i < (uint32_t)kCanChannel_Count; i++)
    {
        can_channel_config_t config;
        can_channel_runtime_status_t status;
        if (CAN_StackGetChannelConfig((can_channel_t)i, &config))
        {
            channelEnabled[i] = config.enabled;
        }
        if (CAN_StackPeekChannelRuntimeStatus((can_channel_t)i, &status))
        {
            channelBusOff[i] = (status.busOff != 0U);
        }
    }

    BSP_UpdateCanActivityLeds(tickMs, channelEnabled, channelBusOff, (uint32_t)kCanChannel_Count);
    BSP_SetStatusLed(kBspStatusLed5, USB_VendorBulkIsConfigured());
    BSP_SetStatusLed(kBspStatusLed6, USB_CanBridgeIsHostConnected());
}

/* 未建立 Host 会话时的跑马灯效果。 */
static void RTOS_UpdateDisconnectedLedChase(uint32_t tickMs)
{
    static const bsp_status_led_t s_ChaseOrder[] = {
        kBspStatusLed1,
        kBspStatusLed3,
        kBspStatusLed5,
        kBspStatusLed6,
        kBspStatusLed4,
        kBspStatusLed2,
    };
    uint32_t step;
    uint32_t i;

    step = (tickMs / 100U) % (uint32_t)(sizeof(s_ChaseOrder) / sizeof(s_ChaseOrder[0]));
    for (i = 0U; i < (uint32_t)(sizeof(s_ChaseOrder) / sizeof(s_ChaseOrder[0])); i++)
    {
        BSP_SetStatusLed(s_ChaseOrder[i], i == step);
    }
}

/* 当某通道发送失败时，从运行态里读出最近错误码作为上报依据。 */
static uint8_t RTOS_GetChannelSendFailureCode(can_channel_t channel)
{
    can_channel_runtime_status_t status;

    if (CAN_StackGetChannelRuntimeStatus(channel, &status))
    {
        return (uint8_t)(status.lastErrorCode & 0xFFU);
    }

    return 0U;
}

/* USB 接收后台任务。时间片 1ms，尽量平滑消费底层 USB 收包。 */
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

/* USB 发送后台任务。阻塞等待有数据可发，减少 CPU 忙轮询。 */
static void USB_TxTask(void *arg)
{
    (void)arg;
    for (;;)
    {
        USB_CanBridgeRunTxStep(portMAX_DELAY);
    }
}

/* 单通道 CAN 后台任务。
 * 每个逻辑通道各跑一个实例，负责：
 * - 消费 Host 下发的 TX 请求
 * - 驱动对应通道的 task/channel 维护
 * - 上报 RX/TX/ERROR 事件 */
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
        if (s_ChannelRecoveredGeneration[channel] != s_HostLinkGeneration)
        {
            (void)CAN_StackResetChannelForHostSession(channel);
            s_ChannelRecoveredGeneration[channel] = s_HostLinkGeneration;
        }

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

/* LED 后台任务。
 * 维护注意：
 * - 当前只读取缓存状态，不允许主动触发底层驱动轮询
 * - 这样可以避免灯语影响 CH0 SPI 路径或整体时序 */
static void Led_Task(void *arg)
{
    TickType_t lastWakeTime;
    bool hostConnected;
    bool lastHostConnected;

    (void)arg;
    lastWakeTime = xTaskGetTickCount();
    lastHostConnected = USB_CanBridgeIsHostConnected();
    for (;;)
    {
        hostConnected = USB_CanBridgeIsHostConnected();
        if (hostConnected != lastHostConnected)
        {
            RTOS_HandleHostLinkTransition(hostConnected);
            lastHostConnected = hostConnected;
        }
        RTOS_UpdateStatusLeds((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));
        vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(20));
    }
}

/* 周期监控任务，统一输出 alive/debug 日志。 */
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

/* 启动任务。
 * 负责：
 * - 初始化 CAN/USB/桥接层
 * - 创建其他后台任务
 * - 最后删除自身 */
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

/* FreeRTOS 栈溢出钩子。 */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    (void)pcTaskName;
    taskDISABLE_INTERRUPTS();
    for (;;)
    {
    }
}

/* FreeRTOS 堆分配失败钩子。 */
void vApplicationMallocFailedHook(void)
{
    taskDISABLE_INTERRUPTS();
    for (;;)
    {
    }
}

/* RTOS 启动入口，由 main 调用。 */
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

