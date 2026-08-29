/* Minimal stand-in so firmware/usbstack/usb_iap_hid.c compiles for the
 * host. Only its transmit path is under test; the rest just has to
 * parse and link. */
#ifndef HIDSTUB_KERNEL_H
#define HIDSTUB_KERNEL_H
struct semaphore { int dummy; };
void semaphore_init(struct semaphore *s, int max, int start);
/* Mirrors firmware/kernel/include/kernel.h -- the return distinguishes
 * a released semaphore from a timeout, and iap_hid_tx() has to act on
 * the difference. */
#define OBJ_WAIT_TIMEDOUT     (-1)
#define OBJ_WAIT_FAILED       0
#define OBJ_WAIT_SUCCEEDED    1
int semaphore_wait(struct semaphore *s, int timeout);
void semaphore_release(struct semaphore *s);
struct mutex { int dummy; };
void mutex_init(struct mutex *m);
void mutex_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);
#endif
