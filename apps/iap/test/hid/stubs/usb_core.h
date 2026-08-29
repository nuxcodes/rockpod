/* Minimal stand-in so firmware/usbstack/usb_iap_hid.c compiles for the
 * host. The real usb_class_driver.h next to that source is used as-is,
 * so this only has to supply what it and the source need from the core.
 * Only the transmit path is under test; the rest just has to link. */
#ifndef HIDSTUB_USB_CORE_H
#define HIDSTUB_USB_CORE_H
#include "usb_ch9.h"
#define DIR_IN  1
#define DIR_OUT 0
#endif
