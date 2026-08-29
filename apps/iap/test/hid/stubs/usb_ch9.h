/* Minimal stand-in so firmware/usbstack/usb_iap_hid.c compiles for the
 * host. Only its transmit path is under test; the rest just has to
 * parse and link. */
#ifndef HIDSTUB_USB_CH9_H
#define HIDSTUB_USB_CH9_H
#include <stdint.h>
#define USB_DT_INTERFACE        0x04
#define USB_DT_ENDPOINT         0x05
#define USB_DT_ENDPOINT_SIZE    7
#define USB_CLASS_HID           0x03
#define USB_DIR_IN              0x80
#define USB_DIR_OUT             0x00
#define USB_TYPE_MASK           0x60
#define USB_TYPE_STANDARD       0x00
#define USB_TYPE_CLASS          0x20
#define USB_TYPE_VENDOR         0x40
#define USB_RECIP_INTERFACE     0x01
#define USB_ENDPOINT_XFER_INT   3
#define USB_REQ_GET_DESCRIPTOR  0x06
struct usb_descriptor_header { uint8_t bLength, bDescriptorType; } __attribute__((packed));
struct usb_interface_descriptor {
    uint8_t bLength, bDescriptorType, bInterfaceNumber, bAlternateSetting;
    uint8_t bNumEndpoints, bInterfaceClass, bInterfaceSubClass;
    uint8_t bInterfaceProtocol, iInterface;
} __attribute__((packed));
struct usb_endpoint_descriptor {
    uint8_t bLength, bDescriptorType, bEndpointAddress, bmAttributes;
    uint16_t wMaxPacketSize; uint8_t bInterval;
} __attribute__((packed));
struct usb_ctrlrequest {
    uint8_t bRequestType, bRequest;
    uint16_t wValue, wIndex, wLength;
} __attribute__((packed));
#endif
