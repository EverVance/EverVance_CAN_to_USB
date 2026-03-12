#ifndef CANFD1_EXT_SPI_H
#define CANFD1_EXT_SPI_H

#include <stdbool.h>
#include <stdint.h>

#include "can_types.h"

typedef struct
{
    /* 软件层统计计数，用于联调观察链路健康状态。 */
    uint32_t txDropCount;
    uint32_t txHwFailCount;
    uint32_t rxDropCount;
} canfd1_ext_stats_t;

bool CANFD1_ExtSpiInit(void);
void CANFD1_ExtSpiTask(void);
bool CANFD1_ExtSpiSend(const can_frame_t *frame);
bool CANFD1_ExtSpiReceive(can_frame_t *frame);
/* 获取外置控制器软件统计信息。 */
void CANFD1_ExtSpiGetStats(canfd1_ext_stats_t *stats);

#endif
