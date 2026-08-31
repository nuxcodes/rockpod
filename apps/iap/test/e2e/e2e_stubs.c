/***************************************************************************
 * The USB side of the end-to-end loop.
 *
 * Everything else -- the protocol layer, the Rockbox fakes -- is the
 * same code the protocol suite builds. Only the USB stack is replaced,
 * by a recorder for the IN reports and a driver for the OUT ones.
 ****************************************************************************/

#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include "e2e.h"

/* rb_stubs.c's blocking-call guard reports through iaptest_fail(). The
 * protocol suite defines that in iap_test.c, which this binary does not
 * link -- and the guard is worth more here than anywhere else, because
 * this is the only suite that drives a whole session through the real
 * transport. Route it into this binary's counters.
 *
 * Adding the guard without this is what made the mutation sweep say
 * "the baseline does not build" with nothing after it, which is why
 * mutate.py now prints the failing step. */
extern int failures;
extern const char *current;

void iaptest_fail(const char *file, int line, const char *fmt, ...)
{
    va_list ap;
    const char *base = strrchr(file, '/');
    failures++;
    fprintf(stderr, "  FAIL [%s] %s:%d\n        ",
            current, base ? base + 1 : file, line);
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fprintf(stderr, "\n");
}

static struct e2e_report reports[E2E_MAX_REPORTS];
static int nreports;
static int sem_waits;
static int locks, unlocks;

void e2e_usb_reset(void)
{
    nreports = 0;
    sem_waits = 0;
    locks = unlocks = 0;
    memset(reports, 0, sizeof(reports));
}

int e2e_report_count(void) { return nreports; }
int e2e_semaphore_waits(void) { return sem_waits; }

const struct e2e_report *e2e_report(int i)
{
    if (i < 0 || i >= nreports)
        return NULL;
    return &reports[i];
}

int usb_drv_send_nonblocking(int endpoint, void *ptr, int length)
{
    (void)endpoint;
    if (nreports < E2E_MAX_REPORTS) {
        struct e2e_report *r = &reports[nreports++];
        int n = length;
        if (n > (int)sizeof(r->data))
            n = (int)sizeof(r->data);
        memcpy(r->data, ptr, n);
        r->len = length;
    }
    return 0;
}

int usb_drv_recv_nonblocking(int endpoint, void *ptr, int length)
{ (void)endpoint; (void)ptr; (void)length; return 0; }

void usb_drv_control_response(int resp, void *data, int length)
{ (void)resp; (void)data; (void)length; }


/* Same values as firmware/kernel/include/kernel.h. Declared here
 * because these stubs stand in for the kernel rather than including
 * it. */
#ifndef OBJ_WAIT_TIMEDOUT
#define OBJ_WAIT_TIMEDOUT     (-1)
#define OBJ_WAIT_FAILED       0
#define OBJ_WAIT_SUCCEEDED    1
#endif

int semaphore_wait(struct semaphore *s, int timeout)
{ (void)s; (void)timeout; sem_waits++; return OBJ_WAIT_SUCCEEDED; }
void semaphore_release(struct semaphore *s) { (void)s; }
void semaphore_init(struct semaphore *s, int max, int start)
{ (void)s; (void)max; (void)start; }

/* The transport's frame lock. Recorded, so a test can see that a whole
 * frame is serialised rather than each report. */
struct mutex;
void mutex_init(struct mutex *m)   { (void)m; }
void mutex_lock(struct mutex *m)   { (void)m; locks++; }
void mutex_unlock(struct mutex *m) { (void)m; unlocks++; }
int  e2e_frame_locks(void)   { return locks; }
int  e2e_frame_unlocks(void) { return unlocks; }
