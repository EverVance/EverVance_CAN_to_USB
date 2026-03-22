#ifndef CAN_TYPES_H
#define CAN_TYPES_H

/* 文件说明：
 * 本头文件定义设备端 CAN 子系统的统一基础类型。
 * 它被 CAN 栈、桥接层、片上/片外驱动和 Host 协议边界共同使用，
 * 因此这里的结构体应尽量保持稳定、简单和与硬件无关。 */

#include <stdbool.h>
#include <stdint.h>

/* 逻辑通道编号：
 * - CH0：外置 MCP2517FD
 * - CH1：片上 FLEXCAN3
 * - CH2：片上 FLEXCAN1
 * - CH3：片上 FLEXCAN2 */
typedef enum
{
    kCanChannel_CanFd1Ext = 0, /* CH0: LPSPI1 + MCP2517FD */
    kCanChannel_Can2,          /* CH1: FLEXCAN3 */
    kCanChannel_Can3,          /* CH2: FLEXCAN1 */
    kCanChannel_Can4,          /* CH3: FLEXCAN2 */
    kCanChannel_Count
} can_channel_t;

/* 统一帧结构：
 * - id   : 11bit/29bit 标识符
 * - dlc  : 数据长度码
 * - flags: FD/扩展帧/BRS 等标志
 * - data : 最大 64 字节，兼容 Classic CAN 与 CAN FD */
typedef struct
{
    uint32_t id;
    uint8_t dlc;
    uint8_t flags;
    uint8_t data[64];
} can_frame_t;

/* 总线事件类型，用于把底层中断/状态变化统一折叠成上层可消费事件。 */
typedef enum
{
    kCanBusEvent_None = 0,
    kCanBusEvent_RxFrame,
    kCanBusEvent_TxComplete,
    kCanBusEvent_Error
} can_bus_event_type_t;

/* 单条总线事件的统一承载结构。
 * - type      : 事件类型
 * - frame     : 对应的帧上下文（若有）
 * - errorCode : 归一化错误码
 * - isTx      : 错误是否发生在发送路径
 * - rawStatus : 便于调试的控制器原始状态值 */
typedef struct
{
    can_bus_event_type_t type;
    can_frame_t frame;
    uint8_t errorCode;
    uint8_t isTx;
    uint16_t reserved0;
    uint32_t rawStatus;
} can_bus_event_t;

/* 驱动运行态快照，供栈层和 Host 状态查询使用。 */
typedef struct
{
    uint8_t busOff;
    uint8_t errorPassive;
    uint8_t rxPending;
    uint8_t txPending;
    uint8_t reserved0;
    uint8_t reserved1;
    uint16_t reserved2;
    uint32_t lastErrorCode;
} can_driver_runtime_state_t;

/* 帧格式：Classic CAN 或 CAN FD。 */
typedef enum
{
    kCanFrameFormat_Classic = 0,
    kCanFrameFormat_Fd = 1
} can_frame_format_t;

/* 通道配置结构，统一描述 Host/设备/驱动之间的配置语义。 */
typedef struct
{
    bool enabled;
    bool terminationEnabled;
    can_frame_format_t frameFormat;
    uint32_t nominalBitrate;
    uint16_t nominalSamplePointPermille;
    uint32_t dataBitrate;
    uint16_t dataSamplePointPermille;
} can_channel_config_t;

#endif
