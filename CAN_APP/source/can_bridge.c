#include "can_bridge.h"

#include <string.h>

bool CAN_BridgeDecodeUsb(const uint8_t *data, uint32_t length, can_bridge_msg_t *msg)
{
    uint8_t dlc;
    uint32_t expectedLen;

    if ((data == NULL) || (msg == NULL) || (length < 8U))
    {
        return false;
    }
    if (data[0] != CAN_BRIDGE_SYNC)
    {
        return false;
    }
    if (data[1] >= (uint8_t)kCanChannel_Count)
    {
        return false;
    }

    dlc = data[2];
    if (dlc > 64U)
    {
        return false;
    }

    expectedLen = 8U + (uint32_t)dlc;
    if (length < expectedLen)
    {
        return false;
    }

    msg->channel = (can_channel_t)data[1];
    msg->frame.dlc = dlc;
    msg->frame.flags = data[3];
    msg->frame.id = ((uint32_t)data[4]) | ((uint32_t)data[5] << 8U) | ((uint32_t)data[6] << 16U) | ((uint32_t)data[7] << 24U);
    (void)memset(msg->frame.data, 0, sizeof(msg->frame.data));
    if (dlc > 0U)
    {
        (void)memcpy(msg->frame.data, &data[8], dlc);
    }

    return true;
}

uint32_t CAN_BridgeEncodeUsb(const can_bridge_msg_t *msg, uint8_t *data, uint32_t maxLength)
{
    uint32_t outLen;

    if ((msg == NULL) || (data == NULL))
    {
        return 0U;
    }
    if ((uint8_t)msg->channel >= (uint8_t)kCanChannel_Count)
    {
        return 0U;
    }
    if (msg->frame.dlc > 64U)
    {
        return 0U;
    }

    outLen = 8U + (uint32_t)msg->frame.dlc;
    if (maxLength < outLen)
    {
        return 0U;
    }

    data[0] = CAN_BRIDGE_SYNC;
    data[1] = (uint8_t)msg->channel;
    data[2] = msg->frame.dlc;
    data[3] = msg->frame.flags;
    data[4] = (uint8_t)(msg->frame.id & 0xFFU);
    data[5] = (uint8_t)((msg->frame.id >> 8U) & 0xFFU);
    data[6] = (uint8_t)((msg->frame.id >> 16U) & 0xFFU);
    data[7] = (uint8_t)((msg->frame.id >> 24U) & 0xFFU);

    if (msg->frame.dlc > 0U)
    {
        (void)memcpy(&data[8], msg->frame.data, msg->frame.dlc);
    }

    return outLen;
}

bool CAN_BridgeNormalizeHostTx(can_bridge_msg_t *msg)
{
    if (msg == NULL)
    {
        return false;
    }
    if ((uint8_t)msg->channel >= (uint8_t)kCanChannel_Count)
    {
        return false;
    }
    if (msg->frame.dlc > 64U)
    {
        return false;
    }
    if ((msg->frame.flags & (CAN_BRIDGE_FLAG_CONTROL | CAN_BRIDGE_FLAG_ERROR)) != 0U)
    {
        return false;
    }

    msg->frame.flags = (uint8_t)((msg->frame.flags & CAN_BRIDGE_FLAG_CANFD) | CAN_BRIDGE_FLAG_TX);
    return true;
}

void CAN_BridgeBuildTxEcho(const can_bridge_msg_t *txReq, can_bridge_msg_t *msg)
{
    if ((txReq == NULL) || (msg == NULL))
    {
        return;
    }

    msg->channel = txReq->channel;
    msg->frame = txReq->frame;
    msg->frame.flags = (uint8_t)((txReq->frame.flags & CAN_BRIDGE_FLAG_CANFD) | CAN_BRIDGE_FLAG_TX);
}

void CAN_BridgeBuildRxUplink(can_channel_t channel, const can_frame_t *rxFrame, can_bridge_msg_t *msg)
{
    if ((rxFrame == NULL) || (msg == NULL))
    {
        return;
    }

    msg->channel = channel;
    msg->frame = *rxFrame;
    msg->frame.flags = (uint8_t)(rxFrame->flags & (CAN_BRIDGE_FLAG_CANFD | CAN_BRIDGE_FLAG_ERROR | CAN_BRIDGE_FLAG_ERROR_MASK));
    msg->frame.flags &= (uint8_t)(~CAN_BRIDGE_FLAG_TX);

    if ((msg->frame.flags & CAN_BRIDGE_FLAG_ERROR) == 0U)
    {
        msg->frame.flags &= (uint8_t)(~CAN_BRIDGE_FLAG_ERROR_MASK);
    }
}

void CAN_BridgeBuildError(can_channel_t channel, uint32_t id, uint8_t baseFlags, bool isTx, uint8_t errorCode, can_bridge_msg_t *msg)
{
    if (msg == NULL)
    {
        return;
    }

    msg->channel = channel;
    msg->frame.id = id;
    msg->frame.dlc = 0U;
    (void)memset(msg->frame.data, 0, sizeof(msg->frame.data));

    msg->frame.flags = (uint8_t)(baseFlags & CAN_BRIDGE_FLAG_CANFD);
    if (isTx)
    {
        msg->frame.flags |= CAN_BRIDGE_FLAG_TX;
    }
    msg->frame.flags |= CAN_BRIDGE_FLAG_ERROR;
    msg->frame.flags |= (uint8_t)((errorCode & 0x0FU) << CAN_BRIDGE_FLAG_ERROR_SHIFT);
}

