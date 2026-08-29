/* Enough of the target config for uart-pp.c, without the real
 * firmware/export/config.h -- which on an arm64 host takes rbendian.h's
 * ARM path and then defines the same helpers twice, and pulls
 * thread.h's asm/thread.h in after it. The hid suite shadows its
 * headers for the same reason. */
#ifndef __CONFIG_H__
#define __CONFIG_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define CPU_PP
#define IPOD_ACCESSORY_PROTOCOL
#define HAVE_SERIAL
#define HZ 100

#endif
