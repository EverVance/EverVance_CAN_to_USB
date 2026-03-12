#ifndef USB_VENDOR_BULK_H
#define USB_VENDOR_BULK_H

#include <stdbool.h>
#include <stdint.h>

#include "usb_device_config.h"
#include "usb.h"
#include "usb_device.h"
#include "usb_vendor_bulk_desc.h"

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

void USB_VendorBulkInit(void);
void USB_VendorBulkTask(void);
void USB_VendorBulkUpdateHwTick(uint64_t tick);
bool USB_VendorBulkIsConfigured(void);
void USB_VendorBulkGetStats(usb_vendor_bulk_stats_t *stats);
void USB_VendorBulkGetDebugState(usb_vendor_bulk_debug_state_t *state);
bool USB_VendorBulkPopRxPacket(uint8_t *data, uint32_t *length, uint32_t maxLength);
bool USB_VendorBulkSendPacket(const uint8_t *data, uint32_t length);

#endif

