/* End-to-end: the iAP protocol layer talking through the real USB HID
 * transport. Shared declarations. */
#ifndef E2E_H
#define E2E_H

#include <stdbool.h>
#include <stdint.h>

#define E2E_MAX_REPORTS 64

struct e2e_report {
    unsigned char data[80];
    int           len;
};

void e2e_usb_reset(void);
int  e2e_report_count(void);
const struct e2e_report *e2e_report(int i);
int  e2e_semaphore_waits(void);
int  e2e_frame_locks(void);
int  e2e_frame_unlocks(void);

struct semaphore;

/* usb_iap_hid.c's own entry points, exposed for the test by -Dstatic=. */
void iap_hid_process_rx(const unsigned char *data, int len);

/* The transport's own lifecycle, so a case can model a bus reset. */
void usb_iap_hid_init(void);
void usb_iap_hid_init_connection(void);
void usb_iap_hid_disconnect(void);
extern bool iap_hid_active;

#endif
