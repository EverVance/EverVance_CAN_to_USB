#ifndef CAN_STACK_H
#define CAN_STACK_H

/* 文件说明：
 * CAN Stack 是设备端 CAN 子系统的统一入口。
 * 它向上屏蔽了：
 * - CH0 外置 MCP2517FD
 * - CH1~CH3 片上 FlexCAN
 * - 板级终端与收发器 STB 控制
 *
 * Host、RTOS 任务、USB 桥接层都应优先通过本接口访问 CAN 能力。 */

#include <stdbool.h>
#include <stdint.h>

#include "can_types.h" /* 提供统一通道编号、帧格式、运行态和事件类型。 */

typedef struct
{
    uint8_t supportsClassic;
    uint8_t supportsFd;
    uint8_t supportsTermination;
    uint8_t driverType;
    uint32_t nominalBitrateMin;
    uint32_t nominalBitrateMax;
    uint32_t dataBitrateMax;
    uint16_t nominalSampleMinPermille;
    uint16_t nominalSampleMaxPermille;
    uint16_t dataSampleMinPermille;
    uint16_t dataSampleMaxPermille;
} can_channel_capabilities_t;

typedef struct
{
    uint8_t ready;
    uint8_t enabled;
    uint8_t busOff;
    uint8_t errorPassive;
    uint8_t phyStandby;
    uint8_t rxPending;
    uint8_t txPending;
    uint8_t reserved0;
    uint32_t txCount;
    uint32_t rxCount;
    uint32_t lastErrorCode;
} can_channel_runtime_status_t;

/** 初始化整个 CAN 子系统，包括控制器、板级物理层和默认会话状态。 */
bool CAN_StackInit(void);
/** 周期任务入口，统一推进所有 CAN 子模块。 */
void CAN_StackTask(void);
/** 单通道任务入口，按逻辑通道刷新对应驱动运行态。 */
void CAN_StackTaskChannel(can_channel_t channel);
/** 向指定逻辑通道发送一帧。 */
bool CAN_StackSend(can_channel_t channel, const can_frame_t *frame);
/** 从指定逻辑通道读取一帧接收数据。 */
bool CAN_StackReceive(can_channel_t channel, can_frame_t *frame);
/** 轮询指定逻辑通道的总线事件。 */
bool CAN_StackPollEvent(can_channel_t channel, can_bus_event_t *event);
/** 获取某个通道的默认配置模板。 */
void CAN_StackGetDefaultChannelConfig(can_channel_t channel, can_channel_config_t *config);
/** 对指定逻辑通道应用配置，并返回配置状态码。 */
bool CAN_StackApplyChannelConfig(can_channel_t channel, const can_channel_config_t *config, uint8_t *statusCode);
/** 读取设备侧记录的当前通道配置。 */
bool CAN_StackGetChannelConfig(can_channel_t channel, can_channel_config_t *config);
/** 读取某个通道的能力信息。 */
bool CAN_StackGetChannelCapabilities(can_channel_t channel, can_channel_capabilities_t *capabilities);
/** 获取指定通道最新运行态，必要时会刷新底层驱动状态。 */
bool CAN_StackGetChannelRuntimeStatus(can_channel_t channel, can_channel_runtime_status_t *status);
/** 仅返回缓存的运行态，不主动触发底层刷新。 */
bool CAN_StackPeekChannelRuntimeStatus(can_channel_t channel, can_channel_runtime_status_t *status);
/** 触发指定通道的恢复流程，通常用于 BusOff 后恢复。 */
bool CAN_StackRecoverChannel(can_channel_t channel);
/** 将指定通道重置为默认配置。 */
bool CAN_StackResetChannelToDefault(can_channel_t channel);
/** 将指定通道重置为 Host 会话禁用态。 */
bool CAN_StackResetChannelForHostSession(can_channel_t channel);
/** 返回当前栈支持的功能位集合。 */
uint32_t CAN_StackGetFeatureFlags(void);

#endif
