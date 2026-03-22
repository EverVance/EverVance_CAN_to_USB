#ifndef BSP_PERIPHERALS_H
#define BSP_PERIPHERALS_H

/* 文件说明：
 * 本头文件封装板级外设控制能力，重点包括：
 * 1. 状态灯与活动灯控制
 * 2. 可控 CAN 终端电阻开关
 * 3. 与网表一致的 GPIO/时钟/复用初始化入口
 *
 * 这里不直接承载协议逻辑，只向上提供“板级资源控制”能力。 */

#include <stdbool.h>
#include <stdint.h>

#include "can_types.h" /* 需要 can_channel_t，用于将板级资源与逻辑通道绑定。 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    kBspStatusLed1 = 0,
    kBspStatusLed2,
    kBspStatusLed3,
    kBspStatusLed4,
    kBspStatusLed5,
    kBspStatusLed6,
    kBspStatusLedCount
} bsp_status_led_t;

/**
 * @brief 初始化板级外设控制资源。
 * @details
 * 该函数负责把 Hardware 网表里定义的板级资源真正配置到 MCU 上，包括：
 * 1. CAN/CAN FD 收发器使能脚与终端控制脚
 * 2. 状态灯 GPIO
 * 3. MCP2517FD 所需的 LPSPI1 管脚与时钟
 * 4. 片上 FlexCAN 所需的 TX/RX pin mux 与 daisy 输入
 * @note 调用时机：系统启动早期，由 main() 在 RTOS 启动前调用一次。
 * @note 优先级/时间片：裸机初始化阶段，无任务抢占。
 */
void BSP_PeripheralsInit(void);

/**
 * @brief 一次性设置全部状态灯的点亮状态。
 * @param on `true` 表示点亮，`false` 表示熄灭。
 * @note 调用时机：启动灯语、异常恢复、统一清灯时使用。
 */
void BSP_SetStatusLeds(bool on);

/**
 * @brief 设置单个状态灯的逻辑点亮状态。
 * @param led 目标灯编号。
 * @param on `true` 表示点亮，`false` 表示熄灭。
 * @note 本项目 LED 为共阳/低电平点亮，具体极性在实现文件中处理，
 *       调用者无需关心物理高低电平。
 */
void BSP_SetStatusLed(bsp_status_led_t led, bool on);

/**
 * @brief 执行上电跑马灯。
 * @details 用于直观确认 6 路 LED 以及 GPIO 控制链是否正常。
 * @note 调用时机：通常在启动阶段或用户明确要求做灯语自检时调用。
 */
void BSP_RunStartupLedSweep(void);

/**
 * @brief 通知板级灯语系统发生了一次 CAN 发送活动。
 * @param channel 发生发送活动的逻辑通道。
 * @param tickMs 当前系统节拍，单位毫秒。
 * @note 实现层不会立即强制刷新 GPIO，而是记录活动时间戳，供周期更新函数统一处理。
 */
void BSP_NotifyCanTxActivity(can_channel_t channel, uint32_t tickMs);

/**
 * @brief 通知板级灯语系统发生了一次 CAN 接收活动。
 * @param channel 发生接收活动的逻辑通道。
 * @param tickMs 当前系统节拍，单位毫秒。
 */
void BSP_NotifyCanRxActivity(can_channel_t channel, uint32_t tickMs);

/**
 * @brief 周期更新 CAN 活动灯状态。
 * @param tickMs 当前系统节拍，单位毫秒。
 * @param channelEnabled 每个通道当前是否启用。
 * @param channelBusOff 每个通道当前是否 BusOff。
 * @param channelCount 数组中有效的通道数量。
 * @details
 * 该函数统一处理：
 * 1. 未连接时的跑马灯
 * 2. 通道启用后的呼吸灯/常亮
 * 3. 收发活动时的闪烁优先级覆盖
 * 4. BusOff 常亮优先级
 */
void BSP_UpdateCanActivityLeds(uint32_t tickMs,
                               const bool *channelEnabled,
                               const bool *channelBusOff,
                               uint32_t channelCount);

/**
 * @brief 清空 CAN 灯语内部状态缓存。
 * @details 用于会话切换、启动复位、异常恢复时，防止旧的闪烁/呼吸状态遗留到下一轮。
 */
void BSP_ResetCanLedState(void);

/**
 * @brief 控制指定逻辑通道的板载终端电阻开关。
 * @param channel 逻辑通道编号。
 * @param enabled `true` 连接终端，`false` 断开终端。
 * @return 是否控制成功。
 * @note 这里控制的是板级可控终端网络，不是 CAN 控制器内部功能。
 */
bool BSP_SetCanTermination(can_channel_t channel, bool enabled);

/**
 * @brief 读取指定逻辑通道的板载终端开关状态。
 * @param channel 逻辑通道编号。
 * @param enabled 输出当前终端是否处于连接状态。
 * @return 是否读取成功。
 */
bool BSP_GetCanTermination(can_channel_t channel, bool *enabled);

#ifdef __cplusplus
}
#endif

#endif
