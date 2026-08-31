/* Everything usb_iap_hid.c's transmit path reaches outside itself.
 *
 * Only iap_hid_tx() is under test, so the USB stack is reduced to a
 * recorder: usb_drv_send_nonblocking() keeps the exact bytes and length
 * of every report, which is what the assertions are about. */

#include <string.h>
#include <stdio.h>
#include "hid_stubs.h"

static struct hidstub_report reports[HIDSTUB_MAX_REPORTS];
static int nreports;
static int sem_waits;
/* Pull the transport out from under a frame after this many reports,
 * standing in for usb_iap_hid_disconnect() landing on the USB thread
 * while iap_hid_tx() is blocked in semaphore_wait(). -1 disables. */
static int deactivate_after = -1;
extern bool iap_hid_active;
void hidstub_deactivate_after(int n) { deactivate_after = n; }

/* Record waits and sends in one sequence, so a test can see the order
 * and not merely the counts. "One wait per report" cannot tell a wait
 * before the send from a wait after it -- and after is exactly the
 * mutation that removes the protection. */
static char trace[256];
static int  trace_n;

static void trace_put(char c)
{
    if (trace_n < (int)sizeof(trace) - 1)
        trace[trace_n++] = c;
}

const char *hidstub_trace(void) { trace[trace_n] = '\0'; return trace; }

/* Knobs for what happens while the sender is blocked; see
 * semaphore_wait() below. Declared here so hidstub_reset() can clear
 * them. */
static int sem_timeout_at = -1;
static int deactivate_in_wait_at = -1;

void hidstub_reset(void)
{
    nreports = 0;
    sem_waits = 0;
    trace_n = 0;
    deactivate_after = -1;
    deactivate_in_wait_at = -1;
    sem_timeout_at = -1;
    memset(reports, 0, sizeof(reports));
}

int hidstub_tx_count(void) { return nreports; }

/* -Dstatic= exposes usb_iap_hid.c's own gate, which iap_hid_tx()
 * checks before doing anything. */
void hidstub_set_active(bool on) { iap_hid_active = on; }
int hidstub_semaphore_waits(void) { return sem_waits; }

const struct hidstub_report *hidstub_tx(int i)
{
    if (i < 0 || i >= nreports)
        return NULL;
    return &reports[i];
}

int usb_drv_send_nonblocking(int endpoint, void *ptr, int length)
{
    (void)endpoint;
    if (nreports < HIDSTUB_MAX_REPORTS) {
        struct hidstub_report *r = &reports[nreports++];
        int n = length;
        if (n > (int)sizeof(r->data))
            n = (int)sizeof(r->data);
        memcpy(r->data, ptr, n);
        r->len = length;
    }
    trace_put('s');
    if (deactivate_after >= 0 && nreports >= deactivate_after)
        iap_hid_active = false;
    return 0;
}

/* The single-slot semaphore that serialises access to tx_buf. Counting
 * the waits is the only way to tell a per-report acquire from a
 * per-frame one. */

/* Same values as firmware/kernel/include/kernel.h. Declared here
 * because these stubs stand in for the kernel rather than including
 * it. */
#ifndef OBJ_WAIT_TIMEDOUT
#define OBJ_WAIT_TIMEDOUT     (-1)
#define OBJ_WAIT_FAILED       0
#define OBJ_WAIT_SUCCEEDED    1
#endif

struct semaphore;
/* Returns the real API's values. A case can make the nth wait time out,
 * which is what happens when the host stops polling: the previous
 * fragment is still the source of a live DMA read, so the sender must
 * not refill tx_buf. */
void hidstub_timeout_wait_at(int n) { sem_timeout_at = n; }

/* Clear the active flag from inside the nth wait. That is the real
 * shape of the race: usb_iap_hid_disconnect() runs on the USB thread
 * while this one is blocked, and its own semaphore_release() is what
 * wakes the sender straight into the next send.
 * hidstub_deactivate_after() cannot model it -- it clears the flag
 * after a send returns, which only ever reaches the check at the top of
 * the loop. */
void hidstub_deactivate_in_wait_at(int n) { deactivate_in_wait_at = n; }

int semaphore_wait(struct semaphore *s, int timeout)
{
    (void)s; (void)timeout;
    int n = sem_waits++;
    trace_put('w');
    if (deactivate_in_wait_at >= 0 && n == deactivate_in_wait_at) {
        hidstub_set_active(false);
        trace_put('d');
    }
    if (sem_timeout_at >= 0 && n == sem_timeout_at) {
        trace_put('t');
        return OBJ_WAIT_TIMEDOUT;
    }
    return OBJ_WAIT_SUCCEEDED;
}
void semaphore_release(struct semaphore *s) { (void)s; }
void semaphore_init(struct semaphore *s, int max, int start)
{ (void)s; (void)max; (void)start; }

/* The frame lock. Recorded too: a frame must take it once, before its
 * first wait, and release it once, after its last send. */
struct mutex;
void mutex_init(struct mutex *m)   { (void)m; }
void mutex_lock(struct mutex *m)   { (void)m; trace_put('L'); }
void mutex_unlock(struct mutex *m) { (void)m; trace_put('U'); }

/* The receive path is compiled but not exercised here; these only need
 * to link. iap_test covers the protocol layer they feed. */
/* Record what the reassembly path feeds the protocol layer, so the
 * receive side can be asserted on the same way as the transmit side. */
static unsigned char rx_bytes[4096];
static int rx_n;

void iap_getc(unsigned char c)
{
    if (rx_n < (int)sizeof(rx_bytes))
        rx_bytes[rx_n++] = c;
}

int hidstub_rx_count(void) { return rx_n; }
const unsigned char *hidstub_rx(void) { return rx_bytes; }
void hidstub_rx_clear(void) { rx_n = 0; }
void iap_setup(int ratenum)    { (void)ratenum; }
void iap_malloc(void)          { }
bool iap_running = false;
void (*iap_transport_send)(const unsigned char *buf, int len);

int usb_drv_recv_nonblocking(int endpoint, void *ptr, int length)
{ (void)endpoint; (void)ptr; (void)length; return 0; }
/* Record the control response, so the descriptor answers can be checked
 * against the descriptor itself. */
static unsigned char ctrl[512];
static int ctrl_len = -1;
static int ctrl_resp;

void usb_drv_control_response(int resp, void *data, int length)
{
    ctrl_resp = resp;
    ctrl_len = length;
    if (data && length > 0) {
        int n = length > (int)sizeof(ctrl) ? (int)sizeof(ctrl) : length;
        memcpy(ctrl, data, n);
    }
}

int  hidstub_ctrl_len(void)  { return ctrl_len; }
int  hidstub_ctrl_resp(void) { return ctrl_resp; }
const unsigned char *hidstub_ctrl(void) { return ctrl; }
void hidstub_ctrl_reset(void) { ctrl_len = -1; memset(ctrl, 0, sizeof(ctrl)); }

/* A canary immediately after the transport's rx_buf, so a write past it
 * is visible. dfb271a239 fixed a 32-byte overflow there and nothing
 * covered it. */
extern unsigned char rx_buf[];
/* Seed the transport's receive buffer, so a test can present a report
 * the ID table does not know. */
void hidstub_rx_seed(const unsigned char *b, int n)
{
    if (n > 64) n = 64;
    memcpy(rx_buf, b, n);
}

unsigned char hidstub_rx_canary_check(void)
{
    /* rx_buf is 64 bytes; read the 32 bytes that the old bug wrote. */
    unsigned char acc = 0;
    for (int i = 64; i < 96; i++)
        acc |= rx_buf[i];
    return acc;
}



/* The protocol framer's flush. Only the receive path calls it, and the
 * framer itself is stubbed here, so it just has to link. */
void iap_rx_flush(void) { }
