#ifndef USB_VENDOR_BULK_DESC_H
#define USB_VENDOR_BULK_DESC_H

#include "usb.h"
#include "usb_device.h"
#include "usb_device_class.h"

#define USB_VENDOR_BULK_CONFIGURE_INDEX (1U)
#define USB_VENDOR_BULK_INTERFACE_INDEX (0U)

#define USB_VENDOR_BULK_OUT_ENDPOINT (0x01U)
#define USB_VENDOR_BULK_IN_ENDPOINT (0x81U)

#define USB_VENDOR_BULK_EP_MAX_PACKET_SIZE_HS (512U)
#define USB_VENDOR_BULK_EP_MAX_PACKET_SIZE_FS (64U)

#define USB_VENDOR_BULK_MS_OS_VENDOR_CODE (0x20U)

void USB_VendorBulkSetSpeed(uint8_t speed);
uint16_t USB_VendorBulkGetCurrentPacketSize(void);
usb_status_t USB_VendorBulkGetDescriptor(uint32_t event, void *param);
bool USB_VendorBulkHandleMsOsVendorRequest(usb_setup_struct_t *setup, uint8_t **buffer, uint32_t *length);

#endif
