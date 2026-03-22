#ifndef USB_VENDOR_BULK_DESC_H
#define USB_VENDOR_BULK_DESC_H

/* 文件说明：
 * 本头文件定义 USB Vendor Bulk 设备描述符、端点号和与描述符查询相关的接口。
 * 它服务于 USB Device 栈，不直接参与业务逻辑。 */

#include "usb.h"              /* USB 通用类型与状态码。 */
#include "usb_device.h"       /* USB Device 层描述符查询参数。 */
#include "usb_device_class.h" /* 设备类相关结构。 */

#define USB_VENDOR_BULK_CONFIGURE_INDEX (1U)
#define USB_VENDOR_BULK_INTERFACE_INDEX (0U)

#define USB_VENDOR_BULK_OUT_ENDPOINT (0x01U)
#define USB_VENDOR_BULK_IN_ENDPOINT (0x81U)

#define USB_VENDOR_BULK_EP_MAX_PACKET_SIZE_HS (512U)
#define USB_VENDOR_BULK_EP_MAX_PACKET_SIZE_FS (64U)

#define USB_VENDOR_BULK_MS_OS_VENDOR_CODE (0x20U)

/** 根据当前速度切换描述符中的端点包长。 */
void USB_VendorBulkSetSpeed(uint8_t speed);
/** 返回当前速度下实际生效的端点最大包长。 */
uint16_t USB_VendorBulkGetCurrentPacketSize(void);
/** 提供给 USB Device 栈的描述符查询回调。 */
usb_status_t USB_VendorBulkGetDescriptor(uint32_t event, void *param);
/** 处理微软 OS 描述符相关 Vendor Request。 */
bool USB_VendorBulkHandleMsOsVendorRequest(usb_setup_struct_t *setup, uint8_t **buffer, uint32_t *length);

#endif
