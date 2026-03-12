#ifndef USB_CAN_BRIDGE_H
#define USB_CAN_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "can_bridge.h"
#include "can_types.h"

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

bool USB_CanBridgeInit(void);
QueueHandle_t USB_CanBridgeGetCanTxQueue(can_channel_t channel);
void USB_CanBridgeGetStats(usb_can_bridge_stats_t *stats);
bool USB_CanBridgeIsHostConnected(void);
void USB_CanBridgeRunRxStep(uint64_t tick);
void USB_CanBridgeRunTxStep(TickType_t waitTicks);
bool USB_CanBridgePostCanTxResult(const can_bridge_msg_t *txReq, bool sendOk, uint8_t errorCode);
bool USB_CanBridgePostCanRxFrame(can_channel_t channel, const can_frame_t *rxFrame);

#endif
