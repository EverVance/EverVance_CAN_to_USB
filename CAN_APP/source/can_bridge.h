#ifndef CAN_BRIDGE_H
#define CAN_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

#include "can_types.h"

#define CAN_BRIDGE_SYNC (0xA5U)
#define CAN_BRIDGE_USB_MAX_LEN (72U)

#define CAN_BRIDGE_FLAG_CANFD (0x01U)
#define CAN_BRIDGE_FLAG_TX (0x02U)
#define CAN_BRIDGE_FLAG_ERROR (0x04U)
#define CAN_BRIDGE_FLAG_CONTROL (0x08U)
#define CAN_BRIDGE_FLAG_ERROR_SHIFT (4U)
#define CAN_BRIDGE_FLAG_ERROR_MASK (0xF0U)

typedef struct
{
    can_channel_t channel;
    can_frame_t frame;
} can_bridge_msg_t;

bool CAN_BridgeDecodeUsb(const uint8_t *data, uint32_t length, can_bridge_msg_t *msg);
uint32_t CAN_BridgeEncodeUsb(const can_bridge_msg_t *msg, uint8_t *data, uint32_t maxLength);
bool CAN_BridgeNormalizeHostTx(can_bridge_msg_t *msg);
void CAN_BridgeBuildTxEcho(const can_bridge_msg_t *txReq, can_bridge_msg_t *msg);
void CAN_BridgeBuildRxUplink(can_channel_t channel, const can_frame_t *rxFrame, can_bridge_msg_t *msg);
void CAN_BridgeBuildError(can_channel_t channel, uint32_t id, uint8_t baseFlags, bool isTx, uint8_t errorCode, can_bridge_msg_t *msg);

#endif

