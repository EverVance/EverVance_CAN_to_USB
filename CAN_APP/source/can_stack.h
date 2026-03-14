#ifndef CAN_STACK_H
#define CAN_STACK_H

#include <stdbool.h>
#include <stdint.h>

#include "can_types.h"

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

bool CAN_StackInit(void);
void CAN_StackTask(void);
void CAN_StackTaskChannel(can_channel_t channel);
bool CAN_StackSend(can_channel_t channel, const can_frame_t *frame);
bool CAN_StackReceive(can_channel_t channel, can_frame_t *frame);
bool CAN_StackPollEvent(can_channel_t channel, can_bus_event_t *event);
void CAN_StackGetDefaultChannelConfig(can_channel_t channel, can_channel_config_t *config);
bool CAN_StackApplyChannelConfig(can_channel_t channel, const can_channel_config_t *config, uint8_t *statusCode);
bool CAN_StackGetChannelConfig(can_channel_t channel, can_channel_config_t *config);
bool CAN_StackGetChannelCapabilities(can_channel_t channel, can_channel_capabilities_t *capabilities);
bool CAN_StackGetChannelRuntimeStatus(can_channel_t channel, can_channel_runtime_status_t *status);
uint32_t CAN_StackGetFeatureFlags(void);

#endif
