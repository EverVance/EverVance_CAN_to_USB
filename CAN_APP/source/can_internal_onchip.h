#ifndef CAN_INTERNAL_ONCHIP_H
#define CAN_INTERNAL_ONCHIP_H

#include <stdbool.h>

#include "can_types.h"

bool CAN_InternalOnChipInit(void);
bool CAN_InternalOnChipApplyConfig(can_channel_t channel, const can_channel_config_t *config);
bool CAN_InternalOnChipGetAppliedConfig(can_channel_t channel, can_channel_config_t *config);
void CAN_InternalOnChipTask(void);
void CAN_InternalOnChipTaskChannel(can_channel_t channel);
bool CAN_InternalOnChipSend(can_channel_t channel, const can_frame_t *frame);
bool CAN_InternalOnChipReceive(can_channel_t channel, can_frame_t *frame);
bool CAN_InternalOnChipPollEvent(can_channel_t channel, can_bus_event_t *event);
bool CAN_InternalOnChipGetRuntimeState(can_channel_t channel, can_driver_runtime_state_t *state);

#endif
