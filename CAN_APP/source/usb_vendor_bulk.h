#ifndef USB_VENDOR_BULK_H
#define USB_VENDOR_BULK_H

/* 文件说明：
 * 本头文件封装设备侧 USB Vendor Bulk 传输层。
 * 该层位于 USB Device 栈之上、USB-CAN Bridge 之下，负责：
 * - 设备枚举
 * - Bulk IN/OUT 端点数据搬运
 * - USB 配置状态与调试状态输出 */

#include <stdbool.h>
#include <stdint.h>

#include "usb_device_config.h"   /* USB Device 栈的工程级配置。 */
#include "usb.h"                 /* USB 基础类型与状态码。 */
#include "usb_device.h"          /* USB Device 设备对象与回调接口。 */
#include "usb_vendor_bulk_desc.h" /* 设备描述符与端点描述定义。 */

#define USB_VENDOR_BULK_MAX_PACKET USB_VENDOR_BULK_EP_MAX_PACKET_SIZE_HS

typedef struct
{
    uint32_t rxPackets;
    uint32_t rxDropped;
    uint32_t txPackets;
    uint32_t txBusyDrop;
    uint8_t configured;
} usb_vendor_bulk_stats_t;

typedef struct
{
    uint32_t usbcmd;
    uint32_t usbmode;
    uint32_t otgsc;
    uint32_t portsc1;
    uint32_t usbsts;
    uint32_t usbphyCtrl;
    uint32_t usbphyStatus;
    uint32_t usbncCtrl;
    uint32_t usbncPhyCtrl0;
    uint32_t phyVbusDetStat;
    uint32_t usbintr;
    uint8_t configured;
    uint8_t attached;
    uint8_t attachEventSeen;
    uint8_t rawIrqPending;
    uint8_t irqSeen;
    uint8_t busResetSeen;
    uint8_t runBitSeen;
    uint8_t dcdFinished;
    uint8_t dcdResult;
    uint8_t vbusValid;
    uint8_t speed;
} usb_vendor_bulk_debug_state_t;

/** 初始化 USB Vendor Bulk 设备层。 */
void USB_VendorBulkInit(void);
/** USB 任务入口，推进枚举与传输状态机。 */
void USB_VendorBulkTask(void);
/** 更新 USB 层使用的硬件时间戳。 */
void USB_VendorBulkUpdateHwTick(uint64_t tick);
/** 返回设备是否已经完成 SetConfiguration。 */
bool USB_VendorBulkIsConfigured(void);
/** 获取 USB 层统计信息。 */
void USB_VendorBulkGetStats(usb_vendor_bulk_stats_t *stats);
/** 获取 USB 层调试状态。 */
void USB_VendorBulkGetDebugState(usb_vendor_bulk_debug_state_t *state);
/** 从 Bulk OUT 队列中取出一个收到的数据包。 */
bool USB_VendorBulkPopRxPacket(uint8_t *data, uint32_t *length, uint32_t maxLength);
/** 向 Bulk IN 端点发送一个数据包。 */
bool USB_VendorBulkSendPacket(const uint8_t *data, uint32_t length);

#endif

