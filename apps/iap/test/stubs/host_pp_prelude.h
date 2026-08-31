/* Host-build shim for the PP targets.
 *
 * firmware/export/pp5020.h maps USB_DEVBSS_ATTR to IBSS_ATTR, which
 * config.h expands to __attribute__((section(".ibss"))) whenever
 * USE_IRAM is on -- and it is on for every CPU_PP and CPU_S5L87XX
 * build. Mach-O wants "segment,section", so the host compiler rejects
 * it outright. The 6G never hit this only because its own config
 * defines USB_DEVBSS_ATTR as a plain alignment attribute.
 *
 * Pulling config.h in here and undefining afterwards works because of
 * its include guard: by the time the source under test includes it,
 * this file already has, so the definition below is the one that
 * stands. Placement attributes mean nothing in a host binary.
 */
#ifndef HOST_PP_PRELUDE_H
#define HOST_PP_PRELUDE_H

#include "config.h"
/* cpu.h is what actually drags in pp5020.h, and config.h does not
 * include it -- undefining before that happens achieves nothing,
 * because the guarded header then defines the macro afresh. */
#include "cpu.h"

#undef USB_DEVBSS_ATTR
#define USB_DEVBSS_ATTR

#endif
