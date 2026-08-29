/* Minimal stand-in so firmware/usbstack/usb_iap_hid.c compiles for the
 * host. Only its transmit path is under test; the rest just has to
 * parse and link. */
#ifndef HIDSTUB_IAP_H
#define HIDSTUB_IAP_H
#include <stdbool.h>
extern void (*iap_transport_send)(const unsigned char *buf, int len);
void iap_getc(unsigned char c);
/* Mirrors firmware/export/iap.h. */
#define IAP_RATE_UNCHANGED (-1)
void iap_setup(int ratenum);
void iap_malloc(void);
void iap_rx_flush(void);
extern bool iap_running;
#endif
