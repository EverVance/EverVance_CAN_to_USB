#ifndef CANFD1_EXT_SPI_H
#define CANFD1_EXT_SPI_H

#include <stdbool.h>
#include <stdint.h>

#include "can_types.h"

typedef struct
{
    uint32_t txDropCount;
    uint32_t txHwFailCount;
    uint32_t rxDropCount;
} canfd1_ext_stats_t;

bool CANFD1_ExtSpiInit(void);
bool CANFD1_ExtSpiApplyConfig(const can_channel_config_t *config);
void CANFD1_ExtSpiTask(void);
bool CANFD1_ExtSpiSend(const can_frame_t *frame);
bool CANFD1_ExtSpiReceive(can_frame_t *frame);
bool CANFD1_ExtSpiPollEvent(can_bus_event_t *event);
bool CANFD1_ExtSpiGetRuntimeState(can_driver_runtime_state_t *state);
void CANFD1_ExtSpiGetStats(canfd1_ext_stats_t *stats);

#endif
