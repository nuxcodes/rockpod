/* Host test harness: no ARM intrinsics on the host. */
#ifndef SYSTEM_ARM_H
#define SYSTEM_ARM_H
#define nop do { } while (0)
#endif
