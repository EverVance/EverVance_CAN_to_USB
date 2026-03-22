#include "usb_can_bridge.h"

#include <string.h>

#include "can_stack.h"
#include "fsl_debug_console.h"
#include "usb_vendor_bulk.h"

/* 文件说明：
 * 本文件实现 USB Bulk 与 CAN 子系统之间的桥接总控逻辑。
 * 它负责：
 * - 解析 Host 控制包/数据包
 * - 投递到 CAN 栈
 * - 把 TX/RX/错误结果重新编码回 USB
 * - 维护 Host 会话状态和桥接统计
 *
 * 如果现象是“Host 看到的结果不对”或“连接/初始化/回执顺序异常”，优先查这里。 */

#define CAN_BRIDGE_CFG_PROTO_VERSION (1U)
#define CAN_BRIDGE_CMD_SET_CHANNEL_CONFIG (0x01U)
#define CAN_BRIDGE_CMD_GET_CHANNEL_CONFIG (0x02U)
#define CAN_BRIDGE_CMD_GET_DEVICE_INFO (0x03U)
#define CAN_BRIDGE_CMD_GET_CHANNEL_CAPS (0x04U)
#define CAN_BRIDGE_CMD_GET_RUNTIME_STATUS (0x05U)
#define CAN_BRIDGE_CMD_HEARTBEAT (0x06U)

#define CAN_BRIDGE_CFG_STATUS_OK (0U)
#define CAN_BRIDGE_CFG_STATUS_INVALID (1U)
#define CAN_BRIDGE_CFG_STATUS_STAGED_ONLY (2U)
#define CAN_BRIDGE_CFG_STATUS_INTERNAL_ERR (3U)

#define CAN_BRIDGE_CFG_PAYLOAD_LEN (16U)
#define CAN_BRIDGE_DEVICE_INFO_PAYLOAD_LEN (16U)
#define CAN_BRIDGE_CHANNEL_CAPS_PAYLOAD_LEN (20U)
#define CAN_BRIDGE_RUNTIME_STATUS_PAYLOAD_LEN (20U)

#define USB_CAN_BRIDGE_CAN_TX_QUEUE_DEPTH (64U)
#define USB_CAN_BRIDGE_CTRL_TX_QUEUE_DEPTH (16U)
#define USB_CAN_BRIDGE_DATA_TX_QUEUE_DEPTH (128U)
#define USB_CAN_BRIDGE_RX_STREAM_CAPACITY (1024U)

#define USB_VENDOR_ID_DEV (0x1FC9U)
#define USB_PRODUCT_ID_DEV (0x0135U)
#define FW_VERSION_MAJOR (0U)
#define FW_VERSION_MINOR (3U)
#define FW_VERSION_PATCH (0U)

typedef struct
{
    uint8_t data[CAN_BRIDGE_USB_MAX_LEN];
    uint16_t length;
    uint8_t channel;
    uint8_t reserved0;
} usb_can_bridge_packet_t;

static QueueHandle_t s_CanTxQueues[kCanChannel_Count];
static QueueHandle_t s_UsbCtrlTxQueue;
static QueueHandle_t s_UsbDataTxQueue;
static uint8_t s_RxStreamBuffer[USB_CAN_BRIDGE_RX_STREAM_CAPACITY];
static uint16_t s_RxStreamLength;
static uint16_t s_UsbDataPending[kCanChannel_Count];
static uint32_t s_RxFramesAccepted;
static uint32_t s_RxFramesDroppedInvalid;
static uint32_t s_RxFramesDroppedOverflow;
static uint32_t s_ControlFramesQueued;
static uint32_t s_ControlFramesDropped;
static uint32_t s_DataFramesQueued;
static uint32_t s_DataFramesDropped;
static uint32_t s_CanTxDropped[kCanChannel_Count];
static uint32_t s_UsbTxDropped[kCanChannel_Count];
static TickType_t s_HostActivityTick;
static uint8_t s_HostConnected;

static void USB_CanBridgeFlushChannelQueues(uint8_t channel);

/* 控制命令码转字符串，仅用于串口调试输出。 */
static const char *USB_CanBridgeCmdName(uint8_t command)
{
    switch (command)
    {
        case CAN_BRIDGE_CMD_SET_CHANNEL_CONFIG:
            return "SetChannelConfig";
        case CAN_BRIDGE_CMD_GET_CHANNEL_CONFIG:
            return "GetChannelConfig";
        case CAN_BRIDGE_CMD_GET_DEVICE_INFO:
            return "GetDeviceInfo";
        case CAN_BRIDGE_CMD_GET_CHANNEL_CAPS:
            return "GetChannelCaps";
        case CAN_BRIDGE_CMD_GET_RUNTIME_STATUS:
            return "GetRuntimeStatus";
        case CAN_BRIDGE_CMD_HEARTBEAT:
            return "Heartbeat";
        default:
            return "Unknown";
    }
}

/* 配置状态码转字符串，仅用于串口调试输出。 */
static const char *USB_CanBridgeStatusName(uint8_t status)
{
    switch (status)
    {
        case CAN_BRIDGE_CFG_STATUS_OK:
            return "OK";
        case CAN_BRIDGE_CFG_STATUS_INVALID:
            return "INVALID";
        case CAN_BRIDGE_CFG_STATUS_STAGED_ONLY:
            return "STAGED";
        case CAN_BRIDGE_CFG_STATUS_INTERNAL_ERR:
            return "INTERNAL";
        default:
            return "UNKNOWN";
    }
}

/* 帧格式转字符串，用于日志。 */
static const char *USB_CanBridgeFrameFormatName(can_frame_format_t frameFormat)
{
    return (frameFormat == kCanFrameFormat_Fd) ? "FD" : "CAN";
}

/* 打印控制命令日志。
 * 维护提示：
 * - 连接后“配置到底有没有下发”优先看这里。
 * - 若 Host 与设备对配置回执理解不一致，也先从这里查。 */
static void USB_CanBridgeLogControlCommand(const char *stage, uint8_t command, uint8_t channel, uint8_t sequence, uint32_t length)
{
    PRINTF("USB ctrl %s cmd=%s(0x%02X) ch=%u seq=0x%02X len=%u\r\n",
           stage,
           USB_CanBridgeCmdName(command),
           command,
           channel,
           sequence,
           (unsigned int)length);
}

/* 小端写入 32bit，保持 USB 协议跨平台一致。 */
static void USB_CanBridgeWriteU32Le(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)((value >> 8U) & 0xFFU);
    dst[2] = (uint8_t)((value >> 16U) & 0xFFU);
    dst[3] = (uint8_t)((value >> 24U) & 0xFFU);
}

/* 小端写入 16bit。 */
static void USB_CanBridgeWriteU16Le(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)((value >> 8U) & 0xFFU);
}

/* 小端读取 32bit。 */
static uint32_t USB_CanBridgeReadU32Le(const uint8_t *src)
{
    return ((uint32_t)src[0]) | ((uint32_t)src[1] << 8U) | ((uint32_t)src[2] << 16U) | ((uint32_t)src[3] << 24U);
}

/* 小端读取 16bit。 */
static uint16_t USB_CanBridgeReadU16Le(const uint8_t *src)
{
    return (uint16_t)(((uint16_t)src[0]) | ((uint16_t)src[1] << 8U));
}

/* 将 RTOS 队列长度钳到单字节统计字段。 */
static uint8_t USB_CanBridgeClampUBaseTypeToU8(UBaseType_t value)
{
    return (value > 0xFFU) ? 0xFFU : (uint8_t)value;
}

/* 将 32bit 统计值钳到单字节展示字段。 */
static uint8_t USB_CanBridgeClampU32ToU8(uint32_t value)
{
    return (value > 0xFFU) ? 0xFFU : (uint8_t)value;
}

/* 判断错误码是否只表示“节点状态变化”，而非有意义的总线错误帧。 */
static bool USB_CanBridgeIsNodeStateOnlyError(uint8_t errorCode)
{
    return (errorCode == 0x07U);
}

/* 判断错误上报是否携带了有意义的帧上下文。 */
static bool USB_CanBridgeHasMeaningfulFrameContext(const can_frame_t *frameHint)
{
    if (frameHint == NULL)
    {
        return false;
    }

    return (frameHint->id != 0U) ||
           (frameHint->dlc != 0U) ||
           ((frameHint->flags & CAN_BRIDGE_FLAG_CANFD) != 0U);
}

/* 判断是否应把某个错误视为“仅状态类错误”并过滤掉。
 * 当前策略：
 * - Error Passive 不进总线监控
 * - 没有真实帧上下文的 RX 错误也不包装成 fake RX 0x000 */
static bool USB_CanBridgeIsStateOnlyError(const can_frame_t *frameHint, bool isTx, uint8_t errorCode)
{
    if (USB_CanBridgeIsNodeStateOnlyError(errorCode))
    {
        return true;
    }
    if (isTx)
    {
        return false;
    }

    return !USB_CanBridgeHasMeaningfulFrameContext(frameHint);
}

/* 记录 Host 活跃时间。
 * 说明：
 * - 现在不再只靠心跳刷新，合法数据包/控制包也会刷新这里。
 * - 这样可以避免高频数据场景被误判为“Host 已断开”。 */
static void USB_CanBridgeNoteHostActivity(TickType_t tick)
{
    taskENTER_CRITICAL();
    s_HostActivityTick = tick;
    s_HostConnected = 1U;
    taskEXIT_CRITICAL();
}

/* 清除 Host 活跃状态。 */
static void USB_CanBridgeClearHostActivity(void)
{
    taskENTER_CRITICAL();
    s_HostActivityTick = 0U;
    s_HostConnected = 0U;
    taskEXIT_CRITICAL();
}

/* 处理 Host 会话连断带来的桥接层状态刷新。
 * 关键职责：
 * - 清接收拼包缓存
 * - 清各通道待发送队列
 * - 清 USB 上行待发统计
 * 这样可保证一次新会话从干净状态开始。 */
void USB_CanBridgeHandleHostLinkState(bool connected)
{
    uint8_t channel;

    if (!connected)
    {
        USB_CanBridgeClearHostActivity();
    }

    s_RxStreamLength = 0U;
    (void)memset(s_UsbDataPending, 0, sizeof(s_UsbDataPending));
    for (channel = 0U; channel < (uint8_t)kCanChannel_Count; channel++)
    {
        USB_CanBridgeFlushChannelQueues(channel);
    }
}

/* 记录某一路 CAN TX 队列丢包。 */
static void USB_CanBridgeNoteCanTxDrop(uint8_t channel)
{
    if (channel < (uint8_t)kCanChannel_Count)
    {
        s_CanTxDropped[channel]++;
    }
}

/* 记录某一路 USB 上行队列丢包。 */
static void USB_CanBridgeNoteUsbTxDrop(uint8_t channel)
{
    s_DataFramesDropped++;
    if (channel < (uint8_t)kCanChannel_Count)
    {
        s_UsbTxDropped[channel]++;
    }
}

/* 增加某通道“待上行”的数据包计数。 */
static void USB_CanBridgeIncrementUsbDataPending(uint8_t channel)
{
    if (channel >= (uint8_t)kCanChannel_Count)
    {
        return;
    }

    taskENTER_CRITICAL();
    s_UsbDataPending[channel]++;
    taskEXIT_CRITICAL();
}

/* 成功发出一个 USB 上行包后，减少待发计数。 */
static void USB_CanBridgeDecrementUsbDataPending(uint8_t channel)
{
    if (channel >= (uint8_t)kCanChannel_Count)
    {
        return;
    }

    taskENTER_CRITICAL();
    if (s_UsbDataPending[channel] > 0U)
    {
        s_UsbDataPending[channel]--;
    }
    taskEXIT_CRITICAL();
}

/* 批量减少某通道待上行计数，用于清队列或丢弃残留数据。 */
static void USB_CanBridgeDropUsbDataPending(uint8_t channel, uint16_t count)
{
    if ((channel >= (uint8_t)kCanChannel_Count) || (count == 0U))
    {
        return;
    }

    taskENTER_CRITICAL();
    if (s_UsbDataPending[channel] > count)
    {
        s_UsbDataPending[channel] = (uint16_t)(s_UsbDataPending[channel] - count);
    }
    else
    {
        s_UsbDataPending[channel] = 0U;
    }
    taskEXIT_CRITICAL();
}

/* 读取某通道当前待上行计数。 */
static uint8_t USB_CanBridgeGetUsbDataPending(uint8_t channel)
{
    uint16_t pending = 0U;

    if (channel >= (uint8_t)kCanChannel_Count)
    {
        return 0U;
    }

    taskENTER_CRITICAL();
    pending = s_UsbDataPending[channel];
    taskEXIT_CRITICAL();
    return (pending > 0xFFU) ? 0xFFU : (uint8_t)pending;
}

/* 清空指定通道在桥接层上的残留队列。
 * 维护提示：
 * - 若出现“切换配置后还冒上一轮旧帧”，先看这里是否被调用。 */
static void USB_CanBridgeFlushChannelQueues(uint8_t channel)
{
    can_bridge_msg_t canMsg;
    usb_can_bridge_packet_t packet;
    UBaseType_t pendingCount;
    UBaseType_t index;
    uint16_t removedUplink = 0U;

    if (channel >= (uint8_t)kCanChannel_Count)
    {
        return;
    }

    if (s_CanTxQueues[channel] != NULL)
    {
        while (xQueueReceive(s_CanTxQueues[channel], &canMsg, 0U) == pdPASS)
        {
            s_CanTxDropped[channel]++;
        }
    }

    if (s_UsbDataTxQueue == NULL)
    {
        return;
    }

    vTaskSuspendAll();
    pendingCount = uxQueueMessagesWaiting(s_UsbDataTxQueue);
    for (index = 0U; index < pendingCount; index++)
    {
        if (xQueueReceive(s_UsbDataTxQueue, &packet, 0U) != pdPASS)
        {
            break;
        }

        if (packet.channel == channel)
        {
            removedUplink++;
            continue;
        }

        (void)xQueueSendToBack(s_UsbDataTxQueue, &packet, 0U);
    }
    (void)xTaskResumeAll();

    USB_CanBridgeDropUsbDataPending(channel, removedUplink);
}

/* 将控制应答包压入 USB 控制上行队列。 */
static bool USB_CanBridgeQueueControlPacket(const uint8_t *packet, uint32_t length)
{
    usb_can_bridge_packet_t item;

    if ((packet == NULL) || (s_UsbCtrlTxQueue == NULL) || (length == 0U) || (length > sizeof(item.data)))
    {
        return false;
    }

    (void)memset(&item, 0, sizeof(item));
    (void)memcpy(item.data, packet, length);
    item.length = (uint16_t)length;
    item.channel = packet[1];
    if (xQueueSend(s_UsbCtrlTxQueue, &item, 0U) != pdPASS)
    {
        return false;
    }

    s_ControlFramesQueued++;
    return true;
}

/* 将一条 CAN 桥接消息编码后压入 USB 数据上行队列。 */
static bool USB_CanBridgeQueueCanMessage(const can_bridge_msg_t *msg)
{
    usb_can_bridge_packet_t item;

    if ((msg == NULL) || (s_UsbDataTxQueue == NULL))
    {
        return false;
    }

    item.length = (uint16_t)CAN_BridgeEncodeUsb(msg, item.data, sizeof(item.data));
    if (item.length == 0U)
    {
        return false;
    }

    item.channel = (uint8_t)msg->channel;
    item.reserved0 = 0U;
    if (xQueueSend(s_UsbDataTxQueue, &item, 0U) != pdPASS)
    {
        USB_CanBridgeNoteUsbTxDrop(item.channel);
        return false;
    }

    s_DataFramesQueued++;
    USB_CanBridgeIncrementUsbDataPending(item.channel);
    return true;
}

/* 丢弃接收拼包缓存的前缀字节。
 * 用于从坏数据、错位数据中重新同步到下一帧起点。 */
static void USB_CanBridgeDiscardRxPrefix(uint16_t count)
{
    if (count >= s_RxStreamLength)
    {
        s_RxStreamLength = 0U;
        return;
    }

    (void)memmove(s_RxStreamBuffer, &s_RxStreamBuffer[count], s_RxStreamLength - count);
    s_RxStreamLength = (uint16_t)(s_RxStreamLength - count);
}

/* 将 USB 底层收到的一段原始字节追加到拼包缓存。 */
static bool USB_CanBridgeAppendRxPacket(const uint8_t *packet, uint32_t length)
{
    if ((packet == NULL) || (length == 0U))
    {
        return false;
    }
    if (length > sizeof(s_RxStreamBuffer))
    {
        s_RxFramesDroppedOverflow++;
        s_RxStreamLength = 0U;
        return false;
    }
    if ((uint32_t)s_RxStreamLength + length > sizeof(s_RxStreamBuffer))
    {
        s_RxFramesDroppedOverflow++;
        s_RxStreamLength = 0U;
    }

    (void)memcpy(&s_RxStreamBuffer[s_RxStreamLength], packet, length);
    s_RxStreamLength = (uint16_t)(s_RxStreamLength + length);
    return true;
}

/* 从拼包缓存中尝试提取一条完整协议帧。 */
static bool USB_CanBridgeTryExtractRxFrame(uint8_t *packet, uint32_t *packetLength, uint32_t maxLength)
{
    uint32_t expectedLength;
    uint8_t dlc;
    uint16_t index;

    if ((packet == NULL) || (packetLength == NULL) || (maxLength < 8U))
    {
        return false;
    }

    while (s_RxStreamLength > 0U)
    {
        if (s_RxStreamBuffer[0] != CAN_BRIDGE_SYNC)
        {
            for (index = 1U; index < s_RxStreamLength; index++)
            {
                if (s_RxStreamBuffer[index] == CAN_BRIDGE_SYNC)
                {
                    break;
                }
            }
            s_RxFramesDroppedInvalid++;
            USB_CanBridgeDiscardRxPrefix(index);
            continue;
        }

        if (s_RxStreamLength < 8U)
        {
            return false;
        }

        dlc = s_RxStreamBuffer[2];
        if (dlc > 64U)
        {
            s_RxFramesDroppedInvalid++;
            USB_CanBridgeDiscardRxPrefix(1U);
            continue;
        }

        expectedLength = 8U + (uint32_t)dlc;
        if (s_RxStreamLength < expectedLength)
        {
            return false;
        }
        if (expectedLength > maxLength)
        {
            s_RxFramesDroppedOverflow++;
            USB_CanBridgeDiscardRxPrefix((uint16_t)expectedLength);
            continue;
        }

        (void)memcpy(packet, s_RxStreamBuffer, expectedLength);
        USB_CanBridgeDiscardRxPrefix((uint16_t)expectedLength);
        *packetLength = expectedLength;
        s_RxFramesAccepted++;
        return true;
    }

    return false;
}

/* 将通道配置编码成控制应答 payload。 */
static uint32_t USB_CanBridgeBuildChannelConfigPayload(const can_channel_config_t *config, uint8_t *payload, uint32_t maxLength)
{
    if ((config == NULL) || (payload == NULL) || (maxLength < CAN_BRIDGE_CFG_PAYLOAD_LEN))
    {
        return 0U;
    }

    payload[0] = CAN_BRIDGE_CFG_PROTO_VERSION;
    payload[1] = (config->frameFormat == kCanFrameFormat_Fd) ? 1U : 0U;
    payload[2] = config->enabled ? 1U : 0U;
    payload[3] = config->terminationEnabled ? 1U : 0U;
    USB_CanBridgeWriteU32Le(&payload[4], config->nominalBitrate);
    USB_CanBridgeWriteU16Le(&payload[8], config->nominalSamplePointPermille);
    USB_CanBridgeWriteU32Le(&payload[10], config->dataBitrate);
    USB_CanBridgeWriteU16Le(&payload[14], config->dataSamplePointPermille);
    return CAN_BRIDGE_CFG_PAYLOAD_LEN;
}

/* 将 Host 下发的配置 payload 解析为内部配置结构。 */
static bool USB_CanBridgeParseChannelConfigPayload(const uint8_t *payload, uint32_t length, can_channel_config_t *config)
{
    if ((payload == NULL) || (config == NULL) || (length != CAN_BRIDGE_CFG_PAYLOAD_LEN))
    {
        return false;
    }
    if (payload[0] != CAN_BRIDGE_CFG_PROTO_VERSION)
    {
        return false;
    }

    config->frameFormat = (payload[1] != 0U) ? kCanFrameFormat_Fd : kCanFrameFormat_Classic;
    config->enabled = (payload[2] != 0U);
    config->terminationEnabled = (payload[3] != 0U);
    config->nominalBitrate = USB_CanBridgeReadU32Le(&payload[4]);
    config->nominalSamplePointPermille = USB_CanBridgeReadU16Le(&payload[8]);
    config->dataBitrate = USB_CanBridgeReadU32Le(&payload[10]);
    config->dataSamplePointPermille = USB_CanBridgeReadU16Le(&payload[14]);
    return true;
}

/* 构造设备信息 payload。 */
static uint32_t USB_CanBridgeBuildDeviceInfoPayload(uint8_t *payload, uint32_t maxLength)
{
    if ((payload == NULL) || (maxLength < CAN_BRIDGE_DEVICE_INFO_PAYLOAD_LEN))
    {
        return 0U;
    }

    (void)memset(payload, 0, CAN_BRIDGE_DEVICE_INFO_PAYLOAD_LEN);
    payload[0] = CAN_BRIDGE_CFG_PROTO_VERSION;
    payload[1] = FW_VERSION_MAJOR;
    payload[2] = FW_VERSION_MINOR;
    payload[3] = FW_VERSION_PATCH;
    USB_CanBridgeWriteU32Le(&payload[4], CAN_StackGetFeatureFlags());
    USB_CanBridgeWriteU16Le(&payload[8], USB_VENDOR_ID_DEV);
    USB_CanBridgeWriteU16Le(&payload[10], USB_PRODUCT_ID_DEV);
    payload[12] = (uint8_t)kCanChannel_Count;
    return CAN_BRIDGE_DEVICE_INFO_PAYLOAD_LEN;
}

/* 构造通道能力信息 payload。 */
static uint32_t USB_CanBridgeBuildChannelCapsPayload(can_channel_t channel, uint8_t *payload, uint32_t maxLength)
{
    can_channel_capabilities_t capabilities;
    uint8_t flags = 0U;

    if ((payload == NULL) || (maxLength < CAN_BRIDGE_CHANNEL_CAPS_PAYLOAD_LEN) ||
        !CAN_StackGetChannelCapabilities(channel, &capabilities))
    {
        return 0U;
    }

    if (capabilities.supportsClassic != 0U)
    {
        flags |= 0x01U;
    }
    if (capabilities.supportsFd != 0U)
    {
        flags |= 0x02U;
    }
    if (capabilities.supportsTermination != 0U)
    {
        flags |= 0x04U;
    }

    (void)memset(payload, 0, CAN_BRIDGE_CHANNEL_CAPS_PAYLOAD_LEN);
    payload[0] = CAN_BRIDGE_CFG_PROTO_VERSION;
    payload[1] = (uint8_t)channel;
    payload[2] = flags;
    payload[3] = capabilities.driverType;
    USB_CanBridgeWriteU32Le(&payload[4], capabilities.nominalBitrateMin);
    USB_CanBridgeWriteU32Le(&payload[8], capabilities.nominalBitrateMax);
    USB_CanBridgeWriteU32Le(&payload[12], capabilities.dataBitrateMax);
    USB_CanBridgeWriteU16Le(&payload[16], capabilities.nominalSampleMinPermille);
    USB_CanBridgeWriteU16Le(&payload[18], capabilities.nominalSampleMaxPermille);
    return CAN_BRIDGE_CHANNEL_CAPS_PAYLOAD_LEN;
}

/* 构造运行态 payload。
 * 注意：
 * - 这里展示的是“桥接层补充后的运行态”，不仅仅是 CAN 控制器硬件状态。 */
static uint32_t USB_CanBridgeBuildRuntimeStatusPayload(can_channel_t channel, uint8_t *payload, uint32_t maxLength)
{
    can_channel_runtime_status_t status;
    uint8_t flags = 0U;
    uint8_t hostPending;
    uint8_t uplinkPending;

    if ((payload == NULL) || (maxLength < CAN_BRIDGE_RUNTIME_STATUS_PAYLOAD_LEN) ||
        !CAN_StackGetChannelRuntimeStatus(channel, &status))
    {
        return 0U;
    }

    hostPending = USB_CanBridgeClampUBaseTypeToU8((s_CanTxQueues[channel] != NULL) ? uxQueueMessagesWaiting(s_CanTxQueues[channel]) : 0U);
    uplinkPending = USB_CanBridgeGetUsbDataPending((uint8_t)channel);

    if (status.ready != 0U)
    {
        flags |= 0x01U;
    }
    if (status.enabled != 0U)
    {
        flags |= 0x02U;
    }
    if (status.busOff != 0U)
    {
        flags |= 0x04U;
    }
    if (status.errorPassive != 0U)
    {
        flags |= 0x08U;
    }
    if (status.phyStandby != 0U)
    {
        flags |= 0x10U;
    }
    if ((status.rxPending != 0U) || (uplinkPending != 0U))
    {
        flags |= 0x20U;
    }
    if ((status.txPending != 0U) || (hostPending != 0U))
    {
        flags |= 0x40U;
    }

    (void)memset(payload, 0, CAN_BRIDGE_RUNTIME_STATUS_PAYLOAD_LEN);
    payload[0] = CAN_BRIDGE_CFG_PROTO_VERSION;
    payload[1] = flags;
    payload[2] = (uint8_t)(status.lastErrorCode & 0xFFU);
    USB_CanBridgeWriteU32Le(&payload[4], status.txCount);
    USB_CanBridgeWriteU32Le(&payload[8], status.rxCount);
    USB_CanBridgeWriteU32Le(&payload[12], status.lastErrorCode);
    payload[16] = hostPending;
    payload[17] = uplinkPending;
    payload[18] = USB_CanBridgeClampU32ToU8(s_CanTxDropped[channel]);
    payload[19] = USB_CanBridgeClampU32ToU8(s_UsbTxDropped[channel]);
    return CAN_BRIDGE_RUNTIME_STATUS_PAYLOAD_LEN;
}

static bool USB_CanBridgeBuildControlPacket(uint8_t channel,
                                            uint8_t command,
                                            uint8_t status,
                                            uint8_t sequence,
                                            const uint8_t *payload,
                                            uint32_t payloadLength,
                                            uint8_t *packet,
                                            uint32_t maxLength,
                                            uint32_t *packetLength)
{
    if ((packet == NULL) || (packetLength == NULL) || (payloadLength > 64U) || (maxLength < (8U + payloadLength)))
    {
        return false;
    }

    packet[0] = CAN_BRIDGE_SYNC;
    packet[1] = channel;
    packet[2] = (uint8_t)payloadLength;
    packet[3] = CAN_BRIDGE_FLAG_CONTROL;
    packet[4] = command;
    packet[5] = status;
    packet[6] = sequence;
    packet[7] = CAN_BRIDGE_CFG_PROTO_VERSION;
    if ((payloadLength > 0U) && (payload != NULL))
    {
        (void)memcpy(&packet[8], payload, payloadLength);
    }

    *packetLength = 8U + payloadLength;
    return true;
}

/* 统一回复一个仅带状态码的控制响应。 */
static bool USB_CanBridgeRespondControlStatus(uint8_t channel, uint8_t command, uint8_t status, uint8_t sequence)
{
    uint8_t response[CAN_BRIDGE_USB_MAX_LEN];
    uint32_t responseLength = 0U;

    if (!USB_CanBridgeBuildControlPacket(channel, command, status, sequence, NULL, 0U, response, sizeof(response), &responseLength))
    {
        return false;
    }

    return USB_CanBridgeQueueControlPacket(response, responseLength);
}

/* 回复带配置 payload 的控制响应。 */
static bool USB_CanBridgeRespondChannelConfig(uint8_t channel, uint8_t command, uint8_t status, uint8_t sequence)
{
    uint8_t response[CAN_BRIDGE_USB_MAX_LEN];
    uint8_t payload[CAN_BRIDGE_CFG_PAYLOAD_LEN];
    can_channel_config_t applied;
    uint32_t responseLength = 0U;

    if ((channel >= (uint8_t)kCanChannel_Count) || !CAN_StackGetChannelConfig((can_channel_t)channel, &applied))
    {
        return USB_CanBridgeRespondControlStatus(channel, command, CAN_BRIDGE_CFG_STATUS_INVALID, sequence);
    }
    if (USB_CanBridgeBuildChannelConfigPayload(&applied, payload, sizeof(payload)) == 0U)
    {
        return USB_CanBridgeRespondControlStatus(channel, command, CAN_BRIDGE_CFG_STATUS_INTERNAL_ERR, sequence);
    }
    if (!USB_CanBridgeBuildControlPacket(channel, command, status, sequence, payload, sizeof(payload), response, sizeof(response),
                                         &responseLength))
    {
        return false;
    }
    PRINTF("USB ctrl rsp cmd=%s ch=%u seq=0x%02X status=%s fmt=%s en=%u term=%u n=%u/%u d=%u/%u\r\n",
           USB_CanBridgeCmdName(command),
           channel,
           sequence,
           USB_CanBridgeStatusName(status),
           USB_CanBridgeFrameFormatName(applied.frameFormat),
           applied.enabled ? 1U : 0U,
           applied.terminationEnabled ? 1U : 0U,
           applied.nominalBitrate,
           applied.nominalSamplePointPermille,
           applied.dataBitrate,
           applied.dataSamplePointPermille);
    return USB_CanBridgeQueueControlPacket(response, responseLength);
}

/* 处理 Host 下发的通道配置包。
 * 这是“连接后设备初始化”最关键的入口之一。 */
static bool USB_CanBridgeHandleChannelConfigPacket(const uint8_t *packet, uint32_t length)
{
    can_channel_config_t requested;
    uint8_t status = CAN_BRIDGE_CFG_STATUS_INTERNAL_ERR;
    uint8_t channel;

    if ((packet == NULL) || (length != (8U + CAN_BRIDGE_CFG_PAYLOAD_LEN)))
    {
        return false;
    }

    channel = packet[1];
    USB_CanBridgeLogControlCommand("req", CAN_BRIDGE_CMD_SET_CHANNEL_CONFIG, channel, packet[6], packet[2]);
    if (channel >= (uint8_t)kCanChannel_Count)
    {
        return USB_CanBridgeRespondControlStatus(channel, CAN_BRIDGE_CMD_SET_CHANNEL_CONFIG, CAN_BRIDGE_CFG_STATUS_INVALID, packet[6]);
    }
    if (!USB_CanBridgeParseChannelConfigPayload(&packet[8], packet[2], &requested))
    {
        return USB_CanBridgeRespondChannelConfig(channel, CAN_BRIDGE_CMD_SET_CHANNEL_CONFIG, CAN_BRIDGE_CFG_STATUS_INVALID, packet[6]);
    }
    if (!CAN_StackApplyChannelConfig((can_channel_t)channel, &requested, &status))
    {
        status = CAN_BRIDGE_CFG_STATUS_INVALID;
    }
    else if ((status == CAN_BRIDGE_CFG_STATUS_OK) || (status == CAN_BRIDGE_CFG_STATUS_STAGED_ONLY))
    {
        USB_CanBridgeFlushChannelQueues(channel);
    }

    return USB_CanBridgeRespondChannelConfig(channel, CAN_BRIDGE_CMD_SET_CHANNEL_CONFIG, status, packet[6]);
}

/* 处理“读取当前配置”控制包。 */
static bool USB_CanBridgeHandleGetChannelConfigPacket(const uint8_t *packet, uint32_t length)
{
    uint8_t channel;

    if ((packet == NULL) || (length != 8U))
    {
        return false;
    }

    channel = packet[1];
    USB_CanBridgeLogControlCommand("req", CAN_BRIDGE_CMD_GET_CHANNEL_CONFIG, channel, packet[6], 0U);
    if (channel >= (uint8_t)kCanChannel_Count)
    {
        return USB_CanBridgeRespondControlStatus(channel, CAN_BRIDGE_CMD_GET_CHANNEL_CONFIG, CAN_BRIDGE_CFG_STATUS_INVALID, packet[6]);
    }

    return USB_CanBridgeRespondChannelConfig(channel, CAN_BRIDGE_CMD_GET_CHANNEL_CONFIG, CAN_BRIDGE_CFG_STATUS_OK, packet[6]);
}

/* 处理“读取设备信息”控制包。 */
static bool USB_CanBridgeHandleGetDeviceInfoPacket(const uint8_t *packet, uint32_t length)
{
    uint8_t response[CAN_BRIDGE_USB_MAX_LEN];
    uint8_t payload[CAN_BRIDGE_DEVICE_INFO_PAYLOAD_LEN];
    uint32_t responseLength = 0U;
    uint32_t payloadLength;

    if ((packet == NULL) || (length != 8U))
    {
        return false;
    }

    USB_CanBridgeLogControlCommand("req", CAN_BRIDGE_CMD_GET_DEVICE_INFO, packet[1], packet[6], 0U);
    payloadLength = USB_CanBridgeBuildDeviceInfoPayload(payload, sizeof(payload));
    if ((payloadLength == 0U) ||
        !USB_CanBridgeBuildControlPacket(packet[1], CAN_BRIDGE_CMD_GET_DEVICE_INFO, CAN_BRIDGE_CFG_STATUS_OK, packet[6], payload,
                                         payloadLength, response, sizeof(response), &responseLength))
    {
        return false;
    }

    return USB_CanBridgeQueueControlPacket(response, responseLength);
}

/* 处理“读取通道能力”控制包。 */
static bool USB_CanBridgeHandleGetChannelCapsPacket(const uint8_t *packet, uint32_t length)
{
    uint8_t response[CAN_BRIDGE_USB_MAX_LEN];
    uint8_t payload[CAN_BRIDGE_CHANNEL_CAPS_PAYLOAD_LEN];
    uint32_t responseLength = 0U;
    uint32_t payloadLength;
    uint8_t channel;

    if ((packet == NULL) || (length != 8U))
    {
        return false;
    }

    channel = packet[1];
    USB_CanBridgeLogControlCommand("req", CAN_BRIDGE_CMD_GET_CHANNEL_CAPS, channel, packet[6], 0U);
    if (channel >= (uint8_t)kCanChannel_Count)
    {
        return USB_CanBridgeRespondControlStatus(channel, CAN_BRIDGE_CMD_GET_CHANNEL_CAPS, CAN_BRIDGE_CFG_STATUS_INVALID, packet[6]);
    }

    payloadLength = USB_CanBridgeBuildChannelCapsPayload((can_channel_t)channel, payload, sizeof(payload));
    if ((payloadLength == 0U) ||
        !USB_CanBridgeBuildControlPacket(channel, CAN_BRIDGE_CMD_GET_CHANNEL_CAPS, CAN_BRIDGE_CFG_STATUS_OK, packet[6], payload,
                                         payloadLength, response, sizeof(response), &responseLength))
    {
        return false;
    }

    PRINTF("USB ctrl rsp cmd=%s ch=%u seq=0x%02X flags=0x%02X drv=%u n=%u..%u dmax=%u\r\n",
           USB_CanBridgeCmdName(CAN_BRIDGE_CMD_GET_CHANNEL_CAPS),
           channel,
           packet[6],
           payload[2],
           payload[3],
           (unsigned int)USB_CanBridgeReadU32Le(&payload[4]),
           (unsigned int)USB_CanBridgeReadU32Le(&payload[8]),
           (unsigned int)USB_CanBridgeReadU32Le(&payload[12]));

    return USB_CanBridgeQueueControlPacket(response, responseLength);
}

/* 处理“读取运行态”控制包。 */
static bool USB_CanBridgeHandleGetRuntimeStatusPacket(const uint8_t *packet, uint32_t length)
{
    uint8_t response[CAN_BRIDGE_USB_MAX_LEN];
    uint8_t payload[CAN_BRIDGE_RUNTIME_STATUS_PAYLOAD_LEN];
    uint32_t responseLength = 0U;
    uint32_t payloadLength;
    uint8_t channel;

    if ((packet == NULL) || (length != 8U))
    {
        return false;
    }

    channel = packet[1];
    USB_CanBridgeLogControlCommand("req", CAN_BRIDGE_CMD_GET_RUNTIME_STATUS, channel, packet[6], 0U);
    if (channel >= (uint8_t)kCanChannel_Count)
    {
        return USB_CanBridgeRespondControlStatus(channel, CAN_BRIDGE_CMD_GET_RUNTIME_STATUS, CAN_BRIDGE_CFG_STATUS_INVALID, packet[6]);
    }

    payloadLength = USB_CanBridgeBuildRuntimeStatusPayload((can_channel_t)channel, payload, sizeof(payload));
    if ((payloadLength == 0U) ||
        !USB_CanBridgeBuildControlPacket(channel, CAN_BRIDGE_CMD_GET_RUNTIME_STATUS, CAN_BRIDGE_CFG_STATUS_OK, packet[6], payload,
                                         payloadLength, response, sizeof(response), &responseLength))
    {
        return false;
    }

    PRINTF("USB ctrl rsp cmd=%s ch=%u seq=0x%02X flags=0x%02X tx=%u rx=%u err=0x%08X hostQ=%u usbQ=%u hostDrop=%u usbDrop=%u\r\n",
           USB_CanBridgeCmdName(CAN_BRIDGE_CMD_GET_RUNTIME_STATUS),
           channel,
           packet[6],
           payload[1],
           (unsigned int)USB_CanBridgeReadU32Le(&payload[4]),
           (unsigned int)USB_CanBridgeReadU32Le(&payload[8]),
           (unsigned int)USB_CanBridgeReadU32Le(&payload[12]),
           payload[16],
           payload[17],
           payload[18],
           payload[19]);

    return USB_CanBridgeQueueControlPacket(response, responseLength);
}

/* 处理 Host 心跳包。
 * 当前仍保留该命令，但 Host 活跃状态已不再只依赖心跳维持。 */
static bool USB_CanBridgeHandleHeartbeatPacket(const uint8_t *packet, uint32_t length)
{
    uint8_t response[CAN_BRIDGE_USB_MAX_LEN];
    uint32_t responseLength = 0U;
    uint32_t payloadLength = 0U;

    if ((packet == NULL) || (length < 8U))
    {
        return false;
    }

    payloadLength = packet[2];
    if ((8U + payloadLength) != length)
    {
        return false;
    }

    if (!((payloadLength == 4U) && ((memcmp(&packet[8], "LINK", 4U) == 0) || (memcmp(&packet[8], "UNLK", 4U) == 0))))
    {
        USB_CanBridgeLogControlCommand("req", CAN_BRIDGE_CMD_HEARTBEAT, packet[1], packet[6], payloadLength);
    }

    if ((payloadLength == 4U) && (memcmp(&packet[8], "UNLK", 4U) == 0))
    {
        USB_CanBridgeClearHostActivity();
    }

    if (!USB_CanBridgeBuildControlPacket(packet[1], CAN_BRIDGE_CMD_HEARTBEAT, CAN_BRIDGE_CFG_STATUS_OK, packet[6], &packet[8],
                                         payloadLength, response, sizeof(response), &responseLength))
    {
        return false;
    }

    return USB_CanBridgeQueueControlPacket(response, responseLength);
}

/* 消费一条控制包并路由到具体处理函数。 */
static bool USB_CanBridgeConsumeControlPacket(const uint8_t *packet, uint32_t length, TickType_t tick)
{
    if ((packet == NULL) || (length < 8U) || (packet[0] != CAN_BRIDGE_SYNC) || ((packet[3] & CAN_BRIDGE_FLAG_CONTROL) == 0U))
    {
        return false;
    }
    if ((length != (8U + packet[2])) || (packet[7] != CAN_BRIDGE_CFG_PROTO_VERSION))
    {
        s_RxFramesDroppedInvalid++;
        return true;
    }

    USB_CanBridgeNoteHostActivity(tick);

    switch (packet[4])
    {
        case CAN_BRIDGE_CMD_SET_CHANNEL_CONFIG:
            if (!USB_CanBridgeHandleChannelConfigPacket(packet, length))
            {
                s_ControlFramesDropped++;
            }
            return true;
        case CAN_BRIDGE_CMD_GET_CHANNEL_CONFIG:
            if (!USB_CanBridgeHandleGetChannelConfigPacket(packet, length))
            {
                s_ControlFramesDropped++;
            }
            return true;
        case CAN_BRIDGE_CMD_GET_DEVICE_INFO:
            if (!USB_CanBridgeHandleGetDeviceInfoPacket(packet, length))
            {
                s_ControlFramesDropped++;
            }
            return true;
        case CAN_BRIDGE_CMD_GET_CHANNEL_CAPS:
            if (!USB_CanBridgeHandleGetChannelCapsPacket(packet, length))
            {
                s_ControlFramesDropped++;
            }
            return true;
        case CAN_BRIDGE_CMD_GET_RUNTIME_STATUS:
            if (!USB_CanBridgeHandleGetRuntimeStatusPacket(packet, length))
            {
                s_ControlFramesDropped++;
            }
            return true;
        case CAN_BRIDGE_CMD_HEARTBEAT:
            if (!USB_CanBridgeHandleHeartbeatPacket(packet, length))
            {
                s_ControlFramesDropped++;
            }
            return true;
        default:
            if (!USB_CanBridgeRespondControlStatus(packet[1], packet[4], CAN_BRIDGE_CFG_STATUS_INVALID, packet[6]))
            {
                s_ControlFramesDropped++;
            }
            return true;
    }
}

/* 初始化 USB-CAN 桥接层。
 * 这里只做软件资源分配，不主动驱动 USB/CAN 硬件进入工作态。 */
bool USB_CanBridgeInit(void)
{
    uint32_t i;

    (void)memset(s_CanTxQueues, 0, sizeof(s_CanTxQueues));
    (void)memset(s_UsbDataPending, 0, sizeof(s_UsbDataPending));
    (void)memset(s_CanTxDropped, 0, sizeof(s_CanTxDropped));
    (void)memset(s_UsbTxDropped, 0, sizeof(s_UsbTxDropped));
    s_UsbCtrlTxQueue = NULL;
    s_UsbDataTxQueue = NULL;
    s_RxStreamLength = 0U;
    s_RxFramesAccepted = 0U;
    s_RxFramesDroppedInvalid = 0U;
    s_RxFramesDroppedOverflow = 0U;
    s_ControlFramesQueued = 0U;
    s_ControlFramesDropped = 0U;
    s_DataFramesQueued = 0U;
    s_DataFramesDropped = 0U;
    s_HostActivityTick = 0U;
    s_HostConnected = 0U;

    for (i = 0U; i < (uint32_t)kCanChannel_Count; i++)
    {
        s_CanTxQueues[i] = xQueueCreate(USB_CAN_BRIDGE_CAN_TX_QUEUE_DEPTH, sizeof(can_bridge_msg_t));
        if (s_CanTxQueues[i] == NULL)
        {
            return false;
        }
    }

    s_UsbCtrlTxQueue = xQueueCreate(USB_CAN_BRIDGE_CTRL_TX_QUEUE_DEPTH, sizeof(usb_can_bridge_packet_t));
    if (s_UsbCtrlTxQueue == NULL)
    {
        return false;
    }

    s_UsbDataTxQueue = xQueueCreate(USB_CAN_BRIDGE_DATA_TX_QUEUE_DEPTH, sizeof(usb_can_bridge_packet_t));
    if (s_UsbDataTxQueue == NULL)
    {
        return false;
    }

    return true;
}

/* 查询当前是否认为 Host 会话已建立。 */
bool USB_CanBridgeIsHostConnected(void)
{
    uint8_t connected;

    if (!USB_VendorBulkIsConfigured())
    {
        USB_CanBridgeClearHostActivity();
        return false;
    }

    taskENTER_CRITICAL();
    connected = s_HostConnected;
    taskEXIT_CRITICAL();

    return (connected != 0U);
}

/* 获取某通道对应的 Host->CAN 发送队列句柄。 */
QueueHandle_t USB_CanBridgeGetCanTxQueue(can_channel_t channel)
{
    if ((uint8_t)channel >= (uint8_t)kCanChannel_Count)
    {
        return NULL;
    }

    return s_CanTxQueues[channel];
}

/* 输出桥接层统计，供串口日志与调试页读取。 */
void USB_CanBridgeGetStats(usb_can_bridge_stats_t *stats)
{
    uint32_t i;

    if (stats == NULL)
    {
        return;
    }

    (void)memset(stats, 0, sizeof(*stats));
    stats->rxFramesAccepted = s_RxFramesAccepted;
    stats->rxFramesDroppedInvalid = s_RxFramesDroppedInvalid;
    stats->rxFramesDroppedOverflow = s_RxFramesDroppedOverflow;
    stats->controlFramesQueued = s_ControlFramesQueued;
    stats->controlFramesDropped = s_ControlFramesDropped;
    stats->dataFramesQueued = s_DataFramesQueued;
    stats->dataFramesDropped = s_DataFramesDropped;
    stats->rxStreamBytes = s_RxStreamLength;
    stats->controlPending = USB_CanBridgeClampUBaseTypeToU8((s_UsbCtrlTxQueue != NULL) ? uxQueueMessagesWaiting(s_UsbCtrlTxQueue) : 0U);

    for (i = 0U; i < (uint32_t)kCanChannel_Count; i++)
    {
        stats->canTxPending[i] = (uint16_t)((s_CanTxQueues[i] != NULL) ? uxQueueMessagesWaiting(s_CanTxQueues[i]) : 0U);
        stats->usbTxPending[i] = s_UsbDataPending[i];
        stats->canTxDropped[i] = s_CanTxDropped[i];
        stats->usbTxDropped[i] = s_UsbTxDropped[i];
    }
}

/* USB 接收方向后台步骤。
 * 调用上下文：
 * - 由 USB_Task 周期调用
 * 主要职责：
 * - 从底层 USB Ring 取包
 * - 拼包
 * - 区分控制包与数据包
 * - 将 Host TX 请求送入各通道队列 */
void USB_CanBridgeRunRxStep(uint64_t tick)
{
    uint8_t usbPacket[USB_VENDOR_BULK_MAX_PACKET];
    uint8_t framePacket[CAN_BRIDGE_USB_MAX_LEN];
    uint32_t usbLength = 0U;
    uint32_t frameLength = 0U;
    can_bridge_msg_t msg;
    QueueHandle_t q;
    TickType_t tickCount;

    tickCount = (TickType_t)tick;

    USB_VendorBulkTask();
    USB_VendorBulkUpdateHwTick(tick);

    while (USB_VendorBulkPopRxPacket(usbPacket, &usbLength, sizeof(usbPacket)))
    {
        if ((usbLength == 0U) || !USB_CanBridgeAppendRxPacket(usbPacket, usbLength))
        {
            usbLength = 0U;
            continue;
        }

        while (USB_CanBridgeTryExtractRxFrame(framePacket, &frameLength, sizeof(framePacket)))
        {
            if (USB_CanBridgeConsumeControlPacket(framePacket, frameLength, tickCount))
            {
                frameLength = 0U;
                continue;
            }

            if (!CAN_BridgeDecodeUsb(framePacket, frameLength, &msg) || !CAN_BridgeNormalizeHostTx(&msg))
            {
                s_RxFramesDroppedInvalid++;
                if (framePacket[1] < (uint8_t)kCanChannel_Count)
                {
                    USB_CanBridgeNoteCanTxDrop(framePacket[1]);
                }
                frameLength = 0U;
                continue;
            }

            USB_CanBridgeNoteHostActivity(tickCount);
            q = USB_CanBridgeGetCanTxQueue(msg.channel);
            if ((q == NULL) || (xQueueSend(q, &msg, 0U) != pdPASS))
            {
                USB_CanBridgeNoteCanTxDrop((uint8_t)msg.channel);
            }
            frameLength = 0U;
        }

        usbLength = 0U;
    }
}

/* USB 发送方向后台步骤。
 * 由专门的发送任务阻塞式调用，尽量减少忙轮询 CPU 占用。 */
void USB_CanBridgeRunTxStep(TickType_t waitTicks)
{
    usb_can_bridge_packet_t item;
    BaseType_t hasItem;
    bool isControl = false;
    TickType_t dataWaitTicks = waitTicks;

    if (dataWaitTicks == portMAX_DELAY)
    {
        dataWaitTicks = pdMS_TO_TICKS(2);
    }

    if (s_UsbCtrlTxQueue != NULL)
    {
        hasItem = xQueueReceive(s_UsbCtrlTxQueue, &item, 0U);
        if (hasItem == pdPASS)
        {
            isControl = true;
        }
    }
    else
    {
        hasItem = pdFALSE;
    }

    if ((hasItem != pdPASS) && ((s_UsbDataTxQueue == NULL) || (xQueueReceive(s_UsbDataTxQueue, &item, dataWaitTicks) != pdPASS)))
    {
        return;
    }

    if (!isControl)
    {
        USB_CanBridgeDecrementUsbDataPending(item.channel);
    }

    if (!USB_VendorBulkIsConfigured())
    {
        if (isControl)
        {
            s_ControlFramesDropped++;
        }
        else
        {
            USB_CanBridgeNoteUsbTxDrop(item.channel);
        }
        return;
    }

    while (!USB_VendorBulkSendPacket(item.data, item.length))
    {
        if (!USB_VendorBulkIsConfigured())
        {
            if (isControl)
            {
                s_ControlFramesDropped++;
            }
            else
            {
                USB_CanBridgeNoteUsbTxDrop(item.channel);
            }
            return;
        }
        USB_VendorBulkTask();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/* 上报一条发送结果。
 * sendOk=false 且 errorCode=0 表示“本地发送请求未成功进入控制器”，
 * Host 侧应避免把它误渲染成总线层的 None 错误。 */
bool USB_CanBridgePostCanTxResult(const can_bridge_msg_t *txReq, bool sendOk, uint8_t errorCode)
{
    can_bridge_msg_t uplinkMsg;

    if ((txReq == NULL) || (s_UsbDataTxQueue == NULL))
    {
        return false;
    }

    if (sendOk)
    {
        CAN_BridgeBuildTxEcho(txReq, &uplinkMsg);
    }
    else
    {
        if (USB_CanBridgeIsNodeStateOnlyError(errorCode))
        {
            return true;
        }
        CAN_BridgeBuildError(txReq->channel, txReq->frame.id, txReq->frame.flags, true, errorCode, &uplinkMsg);
    }

    return USB_CanBridgeQueueCanMessage(&uplinkMsg);
}

/* 上报一条接收到的总线帧。 */
bool USB_CanBridgePostCanRxFrame(can_channel_t channel, const can_frame_t *rxFrame)
{
    can_bridge_msg_t uplinkMsg;

    if ((rxFrame == NULL) || (s_UsbDataTxQueue == NULL))
    {
        return false;
    }

    CAN_BridgeBuildRxUplink(channel, rxFrame, &uplinkMsg);
    return USB_CanBridgeQueueCanMessage(&uplinkMsg);
}

/* 上报一条总线错误。
 * 这里会先做“状态类错误过滤”，避免再次出现 fake RX 0x000。 */
bool USB_CanBridgePostCanError(can_channel_t channel, const can_frame_t *frameHint, bool isTx, uint8_t errorCode)
{
    can_bridge_msg_t uplinkMsg;
    uint32_t id = 0U;
    uint8_t baseFlags = 0U;

    if (s_UsbDataTxQueue == NULL)
    {
        return false;
    }
    if (USB_CanBridgeIsStateOnlyError(frameHint, isTx, errorCode))
    {
        return true;
    }
    if (frameHint != NULL)
    {
        id = frameHint->id;
        baseFlags = frameHint->flags;
    }

    CAN_BridgeBuildError(channel, id, baseFlags, isTx, errorCode, &uplinkMsg);
    return USB_CanBridgeQueueCanMessage(&uplinkMsg);
}
