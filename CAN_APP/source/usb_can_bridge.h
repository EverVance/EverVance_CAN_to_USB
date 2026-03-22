#ifndef USB_CAN_BRIDGE_H
#define USB_CAN_BRIDGE_H

/* 文件说明：
 * 本头文件定义 USB Bulk 与 CAN 子系统之间的桥接层接口。
 * 主要职责：
 * 1. 接收 Host 下发的控制包 / 数据包
 * 2. 投递给 CAN 栈
 * 3. 将 CAN 侧 TX/RX/错误结果重新打包上送 USB
 * 4. 维护桥接层统计与 Host 会话状态 */

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h" /* 需要 TickType_t 与 RTOS 基础类型。 */
#include "queue.h"    /* 需要桥接层对外暴露的发送队列句柄类型。 */
#include "task.h"     /* 需要任务相关的时间片与节拍定义。 */

#include "can_bridge.h" /* 需要 USB 协议包与内部桥接消息的转换结构。 */
#include "can_types.h"  /* 需要通道、帧和错误类型定义。 */

typedef struct
{
    uint32_t rxFramesAccepted;
    uint32_t rxFramesDroppedInvalid;
    uint32_t rxFramesDroppedOverflow;
    uint32_t controlFramesQueued;
    uint32_t controlFramesDropped;
    uint32_t dataFramesQueued;
    uint32_t dataFramesDropped;
    uint16_t rxStreamBytes;
    uint8_t controlPending;
    uint8_t reserved0;
    uint16_t canTxPending[kCanChannel_Count];
    uint16_t usbTxPending[kCanChannel_Count];
    uint32_t canTxDropped[kCanChannel_Count];
    uint32_t usbTxDropped[kCanChannel_Count];
} usb_can_bridge_stats_t;

/** 初始化 USB-CAN 桥接层。 */
bool USB_CanBridgeInit(void);
/** 获取指定通道对应的 CAN 发送队列。 */
QueueHandle_t USB_CanBridgeGetCanTxQueue(can_channel_t channel);
/** 获取桥接层统计信息。 */
void USB_CanBridgeGetStats(usb_can_bridge_stats_t *stats);
/** 返回当前 Host 会话是否处于连接状态。 */
bool USB_CanBridgeIsHostConnected(void);
/** 处理 Host 连接状态变化。 */
void USB_CanBridgeHandleHostLinkState(bool connected);
/** 执行一次 USB->CAN 的接收处理步骤。 */
void USB_CanBridgeRunRxStep(uint64_t tick);
/** 执行一次 CAN/控制结果 -> USB 的发送处理步骤。 */
void USB_CanBridgeRunTxStep(TickType_t waitTicks);
/** 上报一次 CAN 发送结果。 */
bool USB_CanBridgePostCanTxResult(const can_bridge_msg_t *txReq, bool sendOk, uint8_t errorCode);
/** 上报一帧 CAN 接收数据。 */
bool USB_CanBridgePostCanRxFrame(can_channel_t channel, const can_frame_t *rxFrame);
/** 上报一次 CAN 错误事件。 */
bool USB_CanBridgePostCanError(can_channel_t channel,
                               const can_frame_t *frameHint,
                               bool isTx,
                               uint8_t errorCode);

#endif
