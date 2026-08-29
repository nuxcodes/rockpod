/* Force-included before usb_iap_hid.c. Gives it the handful of Rockbox
 * and USB-stack declarations its transmit path needs, without dragging
 * in the stack itself. */
#ifndef HID_PRELUDE_H
#define HID_PRELUDE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define USB_DEVBSS_ATTR
#define HZ 100

/* iap_hid_tx() is file-static in the real build; -Dstatic= exposes it,
 * and this gives the test the name it calls. */
#define iap_hid_tx iap_hid_tx_for_test
#define iap_hid_process_rx iap_hid_process_rx_for_test

#endif
