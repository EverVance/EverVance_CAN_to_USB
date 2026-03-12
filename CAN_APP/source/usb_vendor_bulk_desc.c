#include "usb_vendor_bulk_desc.h"

#include <string.h>

#include "usb.h"
#include "usb_spec.h"

#define USB_VENDOR_ID (0x1FC9U)
#define USB_PRODUCT_ID (0x0135U)
#define USB_DEVICE_BCD (0x0101U)

#define USB_STRING_COUNT (4U)
#define USB_LANGUAGE_COUNT (1U)
#define USB_STRING_LANGID (0x0409U)

#define USB_MS_OS_STRING_INDEX (0xEEU)
#define USB_MS_OS_COMPAT_ID_INDEX (0x0004U)
#define USB_MS_OS_EXT_PROP_INDEX (0x0005U)

/* Stable GUID for WinUSB discovery on the host side. */
#define USB_VENDOR_BULK_IF_GUID_CHARS \
    '{', 0x00U, '6', 0x00U, '2', 0x00U, '0', 0x00U, 'A', 0x00U, 'B', 0x00U, 'C', 0x00U, 'C', 0x00U, '-', 0x00U, \
    '9', 0x00U, 'C', 0x00U, '2', 0x00U, '8', 0x00U, '-', 0x00U, '4', 0x00U, 'D', 0x00U, '6', 0x00U, '8', 0x00U, \
    '-', 0x00U, 'A', 0x00U, 'A', 0x00U, '5', 0x00U, '6', 0x00U, '-', 0x00U, 'D', 0x00U, '7', 0x00U, 'F', 0x00U, \
    '2', 0x00U, '7', 0x00U, 'D', 0x00U, 'E', 0x00U, '4', 0x00U, 'B', 0x00U, '3', 0x00U, '0', 0x00U, '6', 0x00U, \
    '}', 0x00U, 0x00U, 0x00U

/* Windows registers WinUSB interface classes from a REG_MULTI_SZ named DeviceInterfaceGUIDs. */
#define USB_VENDOR_BULK_IF_GUID_MULTI_SZ_CHARS \
    '{', 0x00U, '6', 0x00U, '2', 0x00U, '0', 0x00U, 'A', 0x00U, 'B', 0x00U, 'C', 0x00U, 'C', 0x00U, '-', 0x00U, \
    '9', 0x00U, 'C', 0x00U, '2', 0x00U, '8', 0x00U, '-', 0x00U, '4', 0x00U, 'D', 0x00U, '6', 0x00U, '8', 0x00U, \
    '-', 0x00U, 'A', 0x00U, 'A', 0x00U, '5', 0x00U, '6', 0x00U, '-', 0x00U, 'D', 0x00U, '7', 0x00U, 'F', 0x00U, \
    '2', 0x00U, '7', 0x00U, 'D', 0x00U, 'E', 0x00U, '4', 0x00U, 'B', 0x00U, '3', 0x00U, '0', 0x00U, '6', 0x00U, \
    '}', 0x00U, 0x00U, 0x00U, 0x00U, 0x00U

USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
static uint8_t s_DeviceDescriptor[] = {
    USB_DESCRIPTOR_LENGTH_DEVICE,
    USB_DESCRIPTOR_TYPE_DEVICE,
    0x00U,
    0x02U,
    0x00U,
    0x00U,
    0x00U,
    USB_CONTROL_MAX_PACKET_SIZE,
    (uint8_t)(USB_VENDOR_ID & 0xFFU),
    (uint8_t)((USB_VENDOR_ID >> 8U) & 0xFFU),
    (uint8_t)(USB_PRODUCT_ID & 0xFFU),
    (uint8_t)((USB_PRODUCT_ID >> 8U) & 0xFFU),
    (uint8_t)(USB_DEVICE_BCD & 0xFFU),
    (uint8_t)((USB_DEVICE_BCD >> 8U) & 0xFFU),
    0x01U,
    0x02U,
    0x03U,
    0x01U,
};

USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
static uint8_t s_DeviceQualifierDescriptor[] = {
    USB_DESCRIPTOR_LENGTH_DEVICE_QUALITIER,
    USB_DESCRIPTOR_TYPE_DEVICE_QUALITIER,
    0x00U,
    0x02U,
    0x00U,
    0x00U,
    0x00U,
    USB_CONTROL_MAX_PACKET_SIZE,
    0x01U,
    0x00U,
};

USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
static uint8_t s_ConfigDescriptor[] = {
    USB_DESCRIPTOR_LENGTH_CONFIGURE,
    USB_DESCRIPTOR_TYPE_CONFIGURE,
    0x20U,
    0x00U,
    0x01U,
    USB_VENDOR_BULK_CONFIGURE_INDEX,
    0x00U,
    0x80U,
    0x32U,

    USB_DESCRIPTOR_LENGTH_INTERFACE,
    USB_DESCRIPTOR_TYPE_INTERFACE,
    USB_VENDOR_BULK_INTERFACE_INDEX,
    0x00U,
    0x02U,
    0xFFU,
    0x00U,
    0x00U,
    0x04U,

    USB_DESCRIPTOR_LENGTH_ENDPOINT,
    USB_DESCRIPTOR_TYPE_ENDPOINT,
    USB_VENDOR_BULK_OUT_ENDPOINT,
    USB_ENDPOINT_BULK,
    (uint8_t)(USB_VENDOR_BULK_EP_MAX_PACKET_SIZE_HS & 0xFFU),
    (uint8_t)((USB_VENDOR_BULK_EP_MAX_PACKET_SIZE_HS >> 8U) & 0xFFU),
    0x00U,

    USB_DESCRIPTOR_LENGTH_ENDPOINT,
    USB_DESCRIPTOR_TYPE_ENDPOINT,
    USB_VENDOR_BULK_IN_ENDPOINT,
    USB_ENDPOINT_BULK,
    (uint8_t)(USB_VENDOR_BULK_EP_MAX_PACKET_SIZE_HS & 0xFFU),
    (uint8_t)((USB_VENDOR_BULK_EP_MAX_PACKET_SIZE_HS >> 8U) & 0xFFU),
    0x00U,
};

static uint8_t s_CurrentSpeed = USB_SPEED_FULL;

USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
static uint8_t s_String0[] = {0x04U, USB_DESCRIPTOR_TYPE_STRING, 0x09U, 0x04U};

USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
static uint8_t s_String1[] = {
    0x12U, USB_DESCRIPTOR_TYPE_STRING,
    'V', 0x00U, 'B', 0x00U, 'A', 0x00U, '_', 0x00U, 'C', 0x00U, 'A', 0x00U, 'N', 0x00U,
};

USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
static uint8_t s_String2[] = {
    0x20U, USB_DESCRIPTOR_TYPE_STRING,
    'R', 0x00U, 'T', 0x00U, '1', 0x00U, '0', 0x00U, '6', 0x00U, '2', 0x00U, ' ', 0x00U,
    'B', 0x00U, 'u', 0x00U, 'l', 0x00U, 'k', 0x00U, ' ', 0x00U, 'C', 0x00U, 'A', 0x00U, 'N', 0x00U,
};

USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
static uint8_t s_String3[] = {
    0x0AU, USB_DESCRIPTOR_TYPE_STRING,
    '0', 0x00U, '0', 0x00U, '0', 0x00U, '1', 0x00U,
};

USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
static uint8_t s_String4[] = {
    0x18U, USB_DESCRIPTOR_TYPE_STRING,
    'V', 0x00U, 'e', 0x00U, 'n', 0x00U, 'd', 0x00U, 'o', 0x00U, 'r', 0x00U, ' ', 0x00U,
    'B', 0x00U, 'u', 0x00U, 'l', 0x00U, 'k', 0x00U,
};

USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
static uint8_t s_MsOsStringDescriptor[] = {
    0x12U, USB_DESCRIPTOR_TYPE_STRING,
    'M', 0x00U, 'S', 0x00U, 'F', 0x00U, 'T', 0x00U, '1', 0x00U, '0', 0x00U, '0', 0x00U,
    USB_VENDOR_BULK_MS_OS_VENDOR_CODE, 0x00U,
};

USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
static uint8_t s_MsCompatIdDescriptor[] = {
    0x28U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x01U,
    0x04U, 0x00U,
    0x01U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    USB_VENDOR_BULK_INTERFACE_INDEX,
    0x01U,
    'W', 'I', 'N', 'U', 'S', 'B', 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
};

USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
static uint8_t s_MsExtendedPropertiesDescriptor[] = {
    0x92U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x01U,
    0x05U, 0x00U,
    0x01U, 0x00U,
    0x88U, 0x00U, 0x00U, 0x00U,
    0x07U, 0x00U, 0x00U, 0x00U,
    0x2AU, 0x00U,
    'D', 0x00U, 'e', 0x00U, 'v', 0x00U, 'i', 0x00U, 'c', 0x00U, 'e', 0x00U, 'I', 0x00U, 'n', 0x00U,
    't', 0x00U, 'e', 0x00U, 'r', 0x00U, 'f', 0x00U, 'a', 0x00U, 'c', 0x00U, 'e', 0x00U, 'G', 0x00U,
    'U', 0x00U, 'I', 0x00U, 'D', 0x00U, 's', 0x00U, 0x00U, 0x00U,
    0x50U, 0x00U, 0x00U, 0x00U,
    USB_VENDOR_BULK_IF_GUID_MULTI_SZ_CHARS,
};

static uint8_t *s_StringDescriptors[USB_STRING_COUNT] = {
    s_String1,
    s_String2,
    s_String3,
    s_String4,
};

static uint32_t s_StringLengths[USB_STRING_COUNT] = {
    sizeof(s_String1),
    sizeof(s_String2),
    sizeof(s_String3),
    sizeof(s_String4),
};

static usb_language_t s_Language = {
    s_StringDescriptors,
    s_StringLengths,
    USB_STRING_LANGID,
};

static usb_language_t s_LanguageList[USB_LANGUAGE_COUNT] = {
    {
        s_StringDescriptors,
        s_StringLengths,
        USB_STRING_LANGID,
    },
};

static usb_language_list_t s_StringList = {
    s_String0,
    sizeof(s_String0),
    s_LanguageList,
    USB_LANGUAGE_COUNT,
};

static void USB_VendorBulkPatchConfigDescriptor(void)
{
    uint16_t packetSize = USB_VendorBulkGetCurrentPacketSize();

    s_ConfigDescriptor[22] = (uint8_t)(packetSize & 0xFFU);
    s_ConfigDescriptor[23] = (uint8_t)((packetSize >> 8U) & 0xFFU);
    s_ConfigDescriptor[29] = (uint8_t)(packetSize & 0xFFU);
    s_ConfigDescriptor[30] = (uint8_t)((packetSize >> 8U) & 0xFFU);
}

void USB_VendorBulkSetSpeed(uint8_t speed)
{
    s_CurrentSpeed = speed;
    USB_VendorBulkPatchConfigDescriptor();
}

uint16_t USB_VendorBulkGetCurrentPacketSize(void)
{
    return (s_CurrentSpeed == USB_SPEED_HIGH) ? USB_VENDOR_BULK_EP_MAX_PACKET_SIZE_HS : USB_VENDOR_BULK_EP_MAX_PACKET_SIZE_FS;
}

usb_status_t USB_VendorBulkGetDescriptor(uint32_t event, void *param)
{
    if (param == NULL)
    {
        return kStatus_USB_InvalidParameter;
    }

    switch (event)
    {
        case kUSB_DeviceEventGetDeviceDescriptor:
        {
            usb_device_get_device_descriptor_struct_t *desc =
                (usb_device_get_device_descriptor_struct_t *)param;
            desc->buffer = s_DeviceDescriptor;
            desc->length = sizeof(s_DeviceDescriptor);
            return kStatus_USB_Success;
        }

        case kUSB_DeviceEventGetConfigurationDescriptor:
        {
            usb_device_get_configuration_descriptor_struct_t *desc =
                (usb_device_get_configuration_descriptor_struct_t *)param;
            if (desc->configuration != 0U)
            {
                return kStatus_USB_InvalidRequest;
            }
            USB_VendorBulkPatchConfigDescriptor();
            desc->buffer = s_ConfigDescriptor;
            desc->length = sizeof(s_ConfigDescriptor);
            return kStatus_USB_Success;
        }

        case kUSB_DeviceEventGetStringDescriptor:
        {
            usb_device_get_string_descriptor_struct_t *desc =
                (usb_device_get_string_descriptor_struct_t *)param;

            if (desc->stringIndex == 0U)
            {
                desc->buffer = s_StringList.languageString;
                desc->length = s_StringList.stringLength;
                return kStatus_USB_Success;
            }

            if (desc->stringIndex == USB_MS_OS_STRING_INDEX)
            {
                desc->buffer = s_MsOsStringDescriptor;
                desc->length = sizeof(s_MsOsStringDescriptor);
                return kStatus_USB_Success;
            }

            if ((desc->languageId != USB_STRING_LANGID) ||
                (desc->stringIndex > USB_STRING_COUNT))
            {
                return kStatus_USB_InvalidRequest;
            }

            desc->buffer = s_Language.string[desc->stringIndex - 1U];
            desc->length = s_Language.length[desc->stringIndex - 1U];
            return kStatus_USB_Success;
        }

        case kUSB_DeviceEventGetDeviceQualifierDescriptor:
        {
            usb_device_get_device_qualifier_descriptor_struct_t *desc =
                (usb_device_get_device_qualifier_descriptor_struct_t *)param;
            desc->buffer = s_DeviceQualifierDescriptor;
            desc->length = sizeof(s_DeviceQualifierDescriptor);
            return kStatus_USB_Success;
        }

        default:
            return kStatus_USB_InvalidRequest;
    }
}

bool USB_VendorBulkHandleMsOsVendorRequest(usb_setup_struct_t *setup, uint8_t **buffer, uint32_t *length)
{
    if ((setup == NULL) || (buffer == NULL) || (length == NULL))
    {
        return false;
    }

    if (((setup->bmRequestType & USB_REQUEST_TYPE_DIR_MASK) != USB_REQUEST_TYPE_DIR_IN) ||
        ((setup->bmRequestType & USB_REQUEST_TYPE_TYPE_MASK) != USB_REQUEST_TYPE_TYPE_VENDOR) ||
        (setup->bRequest != USB_VENDOR_BULK_MS_OS_VENDOR_CODE))
    {
        return false;
    }

    if ((setup->bmRequestType & USB_REQUEST_TYPE_RECIPIENT_MASK) == USB_REQUEST_TYPE_RECIPIENT_DEVICE)
    {
        switch (setup->wIndex)
        {
            case USB_MS_OS_COMPAT_ID_INDEX:
                *buffer = s_MsCompatIdDescriptor;
                *length = sizeof(s_MsCompatIdDescriptor);
                return true;

            case USB_MS_OS_EXT_PROP_INDEX:
                *buffer = s_MsExtendedPropertiesDescriptor;
                *length = sizeof(s_MsExtendedPropertiesDescriptor);
                return true;

            default:
                return false;
        }
    }

    if ((setup->bmRequestType & USB_REQUEST_TYPE_RECIPIENT_MASK) == USB_REQUEST_TYPE_RECIPIENT_INTERFACE)
    {
        if (setup->wIndex == USB_MS_OS_EXT_PROP_INDEX)
        {
            *buffer = s_MsExtendedPropertiesDescriptor;
            *length = sizeof(s_MsExtendedPropertiesDescriptor);
            return true;
        }
    }

    return false;
}
