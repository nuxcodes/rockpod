/* Minimal stand-in so firmware/usbstack/usb_iap_hid.c compiles for the
 * host. Only its transmit path is under test; the rest just has to
 * parse and link. */
#ifndef HIDSTUB_LOGF_H
#define HIDSTUB_LOGF_H
#define logf(...) do { } while (0)
#endif
