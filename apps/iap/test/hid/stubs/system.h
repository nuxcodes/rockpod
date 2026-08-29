/* Minimal stand-in so firmware/usbstack/usb_iap_hid.c compiles for the
 * host. Only its transmit path is under test; the rest just has to
 * parse and link. */
#ifndef HIDSTUB_SYSTEM_H
#define HIDSTUB_SYSTEM_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))

/* Macros, not inline functions: the sub-suites compile firmware
 * sources with -Dstatic=, which turns a "static inline" into an
 * undefined external. The uart harness learned this the same way. */
#define disable_irq_save() 0
#define restore_irq(level) ((void)(level))
#endif
