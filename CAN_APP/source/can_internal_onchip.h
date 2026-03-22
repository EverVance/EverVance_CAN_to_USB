#ifndef CAN_INTERNAL_ONCHIP_H
#define CAN_INTERNAL_ONCHIP_H

/* 文件说明：
 * 本头文件封装片上 FlexCAN 控制器（CH1~CH3）驱动接口。
 * 当前实现基于官方 SDK 驱动做薄封装，并向上暴露统一的：
 * - 初始化
 * - 配置应用
 * - 收发
 * - 事件轮询
 * - 运行态读取
 */

#include <stdbool.h>

#include "can_types.h" /* 需要统一的通道、帧、事件和运行态结构。 */

/** 初始化片上 CAN 控制器抽象层。启动时调用一次。 */
bool CAN_InternalOnChipInit(void);
/** 对指定片上通道应用配置。配置失败时返回 false。 */
bool CAN_InternalOnChipApplyConfig(can_channel_t channel, const can_channel_config_t *config);
/** 读取驱动层最终生效的配置镜像。 */
bool CAN_InternalOnChipGetAppliedConfig(can_channel_t channel, can_channel_config_t *config);
/** 驱动总任务入口，负责轮询/推进所有片上通道。 */
void CAN_InternalOnChipTask(void);
/** 驱动单通道任务入口，便于上层按通道粒度刷新。 */
void CAN_InternalOnChipTaskChannel(can_channel_t channel);
/** 发送一帧到指定片上通道。 */
bool CAN_InternalOnChipSend(can_channel_t channel, const can_frame_t *frame);
/** 从指定片上通道尝试取出一帧接收数据。 */
bool CAN_InternalOnChipReceive(can_channel_t channel, can_frame_t *frame);
/** 轮询指定片上通道的事件队列。 */
bool CAN_InternalOnChipPollEvent(can_channel_t channel, can_bus_event_t *event);
/** 获取指定片上通道当前的运行态快照。 */
bool CAN_InternalOnChipGetRuntimeState(can_channel_t channel, can_driver_runtime_state_t *state);

#endif
