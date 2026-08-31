/* Minimal stand-in so firmware/usbstack/usb_iap_hid.c compiles for the
 * host. The real mutex is a scheduler object; here it only has to
 * record that the frame lock was taken and released in the right
 * places. */
#ifndef HIDSTUB_MUTEX_H
#define HIDSTUB_MUTEX_H
#include "kernel.h"
#endif
