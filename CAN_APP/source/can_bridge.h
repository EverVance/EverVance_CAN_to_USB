#ifndef CAN_BRIDGE_H
#define CAN_BRIDGE_H

/* 文件说明：
 * 本头文件定义 Host USB 包格式 与 设备内部 CAN 抽象帧之间的转换接口。
 * 它是协议边界层，不直接驱动硬件，但负责统一：
 * - 包头/同步字
 * - 标志位
 * - 错误码打包
 * - TX 回显 / RX 上行 / 错误上报的统一格式 */

#include <stdbool.h>
#include <stdint.h>

#include "can_types.h" /* 需要设备内部统一使用的 CAN 帧与事件类型定义。 */

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

/**
 * @brief 将 USB 收到的原始字节流解码为桥接层消息。
 * @param data 原始 USB 包数据。
 * @param length 数据长度。
 * @param msg 输出解码后的桥接消息。
 * @return 解码是否成功。
 * @note 调用时机：USB RX 线程或桥接层接收步骤中。
 */
bool CAN_BridgeDecodeUsb(const uint8_t *data, uint32_t length, can_bridge_msg_t *msg);

/**
 * @brief 将桥接层消息编码为 USB 上行/下行数据包。
 * @param msg 待编码的桥接消息。
 * @param data 输出缓冲区。
 * @param maxLength 输出缓冲区最大长度。
 * @return 实际编码后的字节数，0 表示失败。
 */
uint32_t CAN_BridgeEncodeUsb(const can_bridge_msg_t *msg, uint8_t *data, uint32_t maxLength);

/**
 * @brief 规范化 Host 下发的发送请求。
 * @details 主要用于修正 DLC、标志位和长度的一致性，防止 Host 层传来非法组合。
 */
bool CAN_BridgeNormalizeHostTx(can_bridge_msg_t *msg);

/**
 * @brief 根据原始发送请求构造一个 TX 回显包。
 * @param txReq 原始发送请求。
 * @param msg 输出的回显消息。
 */
void CAN_BridgeBuildTxEcho(const can_bridge_msg_t *txReq, can_bridge_msg_t *msg);

/**
 * @brief 构造一个 RX 上行消息。
 * @param channel 收到该帧的逻辑通道。
 * @param rxFrame 接收到的实际 CAN 帧。
 * @param msg 输出给 USB 上行层的桥接消息。
 */
void CAN_BridgeBuildRxUplink(can_channel_t channel, const can_frame_t *rxFrame, can_bridge_msg_t *msg);

/**
 * @brief 构造一个错误上报消息。
 * @param channel 错误所属的逻辑通道。
 * @param id 帧 ID，若无上下文可为 0。
 * @param baseFlags 原始标志位。
 * @param isTx 是否为发送侧错误。
 * @param errorCode 归一化后的错误码。
 * @param msg 输出的桥接消息。
 */
void CAN_BridgeBuildError(can_channel_t channel, uint32_t id, uint8_t baseFlags, bool isTx, uint8_t errorCode, can_bridge_msg_t *msg);

#endif

