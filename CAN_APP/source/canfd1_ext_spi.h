#ifndef CANFD1_EXT_SPI_H
#define CANFD1_EXT_SPI_H

/* 文件说明：
 * 本头文件封装 CH0 外置 MCP2517FD 的驱动接口。
 * 该控制器通过 LPSPI1 与 MCU 通信，因此与片上 FlexCAN 路径不同。
 * 向上层暴露统一的配置、收发、事件和运行态接口。 */

#include <stdbool.h>
#include <stdint.h>

#include "can_types.h" /* 需要统一帧结构、事件结构和运行态结构。 */

typedef struct
{
    uint32_t txDropCount;
    uint32_t txHwFailCount;
    uint32_t rxDropCount;
} canfd1_ext_stats_t;

/** 初始化外置 MCP2517FD 驱动。 */
bool CANFD1_ExtSpiInit(void);
/** 对外置 MCP2517FD 应用新的通道配置。 */
bool CANFD1_ExtSpiApplyConfig(const can_channel_config_t *config);
/** 周期任务入口，负责推进 TX/RX/TEF/错误状态。 */
void CANFD1_ExtSpiTask(void);
/** 向 MCP2517FD 发送一帧。 */
bool CANFD1_ExtSpiSend(const can_frame_t *frame);
/** 从 MCP2517FD 取出一帧接收数据。 */
bool CANFD1_ExtSpiReceive(can_frame_t *frame);
/** 轮询 MCP2517FD 事件队列。 */
bool CANFD1_ExtSpiPollEvent(can_bus_event_t *event);
/** 获取 MCP2517FD 当前运行态快照。 */
bool CANFD1_ExtSpiGetRuntimeState(can_driver_runtime_state_t *state);
/** 获取调试统计信息，用于分析丢包、硬件失败和接收丢失。 */
void CANFD1_ExtSpiGetStats(canfd1_ext_stats_t *stats);

#endif
