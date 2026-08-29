/* Shared declarations for the USB HID transport test. */
#ifndef HID_STUBS_H
#define HID_STUBS_H

#include <stdbool.h>
#include <stdint.h>

#define HIDSTUB_MAX_REPORTS 64

struct hidstub_report {
    unsigned char data[80];
    int           len;
};

void hidstub_reset(void);
int  hidstub_tx_count(void);
const struct hidstub_report *hidstub_tx(int i);
int  hidstub_semaphore_waits(void);

/* One character per event, in order: 'L' lock, 'w' semaphore wait,
 * 's' send, 'U' unlock. */
const char *hidstub_trace(void);

/* Clear iap_hid_active once this many reports have been sent, standing
 * in for a disconnect arriving mid-frame. -1 disables. */
void hidstub_deactivate_after(int n);
/* Clear the active flag from inside the nth wait, and time the nth wait
 * out. Both model things that happen while the sender is blocked, which
 * hidstub_deactivate_after() cannot reach -- it acts after a send. */
void hidstub_deactivate_in_wait_at(int n);
void hidstub_timeout_wait_at(int n);

/* The last usb_drv_control_response(): its length, its status, and the
 * bytes it carried. -1 length means it was never called. */
int  hidstub_ctrl_len(void);
int  hidstub_ctrl_resp(void);
const unsigned char *hidstub_ctrl(void);
void hidstub_ctrl_reset(void);
void hidstub_rx_seed(const unsigned char *b, int n);
unsigned char hidstub_rx_canary_check(void);

void hidstub_set_active(bool on);

/* Bytes the reassembly path handed to iap_getc(). */
int hidstub_rx_count(void);
const unsigned char *hidstub_rx(void);
void hidstub_rx_clear(void);

void iap_hid_process_rx_for_test(const unsigned char *data, int len);

/* usb_iap_hid.c's iap_hid_tx() is static; the test build exposes it
 * under this name. */
void iap_hid_tx_for_test(const unsigned char *buf, int len);

#endif
