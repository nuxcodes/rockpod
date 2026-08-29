/* Host test harness: stand-in for the S5L8702 platform header.
 * The iAP code is pure protocol logic, so nothing target-specific is
 * needed beyond satisfying system.h.
 */
#ifndef SYSTEM_TARGET_H
#define SYSTEM_TARGET_H

#include "system-arm.h"

#define CPUFREQ_DEFAULT  54000000
#define CPUFREQ_NORMAL   54000000
#define CPUFREQ_MAX     216000000

static inline void udelay(unsigned usecs) { (void)usecs; }
static inline void mdelay(unsigned msecs) { (void)msecs; }


/* --- platform bits the iAP core touches --- */
#define DEFAULT_STACK_SIZE 0x400

/* Macros, not "static inline": the e2e sub-suite compiles firmware
 * sources with -Dstatic=, which turns a static inline into an
 * undefined external at link time. usb_iap_hid.c started calling these
 * when its two iap_getc() feed loops were bracketed. */
#define disable_irq_save()  (0)
#define restore_irq(level)  ((void)(level))
static inline void disable_irq(void)        { }
static inline void enable_irq(void)         { }

#endif
