/* Minimal stand-in so firmware/usbstack/usb_iap_hid.c compiles for the
 * host. Only its transmit path is under test; the rest just has to
 * parse and link. */
#ifndef HIDSTUB_USB_DRV_H
#define HIDSTUB_USB_DRV_H
int  usb_drv_send_nonblocking(int endpoint, void *ptr, int length);
int  usb_drv_recv_nonblocking(int endpoint, void *ptr, int length);
void usb_drv_control_response(int resp, void *data, int length);
/* The real values, from firmware/export/usb_drv.h:59-63. This used to
 * define RECEIVE as 1, which is the real STALL -- self-consistent while
 * nothing compared the two, and exactly the copied-constant drift the
 * main runner now refuses to start on for the volume range. */
#define USB_CONTROL_ACK      0
#define USB_CONTROL_STALL    1
#define USB_CONTROL_RECEIVE  2
#endif
