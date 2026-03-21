/***************************************************************************
 * Shim codeclib.h for building libfaad + IMDCT into the core binary.
 *
 * Shadows the real lib/rbcodec/codecs/lib/codeclib.h via -I priority.
 * Provides everything the real one does EXCEPT codecs.h (the codec ABI).
 *
 * Only common.h includes codeclib.h (single choke point — verified).
 * Also used by mdct.c and mdct_lookup.c (pure math, no ci-> refs).
 ****************************************************************************/
#ifndef FAAD_CODECLIB_SHIM_H
#define FAAD_CODECLIB_SHIM_H

#include "config.h"
#include "system.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* === DO NOT include "codecs.h" — codec ABI, not for core === */

/* Memory redirects — linker stubs only, never called at runtime.
 * FAAD_STATIC_ALLOC (common.h:69) eliminates all faad_malloc/faad_free.
 * These macros catch any stray malloc() via #include <stdlib.h>. */
void *codec_malloc(size_t size);
void *codec_calloc(size_t nmemb, size_t size);
void *codec_realloc(void *ptr, size_t size);
void  codec_free(void *ptr);
size_t codec_strlen(const char *s);

#define malloc(x)     codec_malloc(x)
#define calloc(x,y)   codec_calloc(x,y)
#define realloc(x,y)  codec_realloc(x,y)
#define free(x)       codec_free(x)
#define alloca(x)     __builtin_alloca(x)
#define strlen(s)     codec_strlen(s)

/* When compiled into core, IRAM is shared with firmware — don't use it.
 * Override IBSS_ATTR/ICODE_ATTR/ICONST_ATTR to empty BEFORE including
 * any headers that use them. Codecs get their own IRAM, core does not. */
#undef IBSS_ATTR
#undef ICODE_ATTR
#undef ICONST_ATTR
#undef IDATA_ATTR
#define IBSS_ATTR
#define ICODE_ATTR
#define ICONST_ATTR
#define IDATA_ATTR

/* IMDCT/FFT — from lib/rbcodec/codecs/lib/ (found via -I path).
 * These headers do NOT include codeclib.h — no circular dependency. */
#include "mdct.h"
#include "fft.h"
#include "codeclib_misc.h"

/* Bit-scanning for wl_min_lzc macro in common.h.
 * ARM926EJ-S (ARMv5TEJ) has CLZ instruction. */
#define BS_LOG2  0
#define BS_CLZ   1
#define BS_SHORT 2
#define BS_0_0   4

static inline unsigned int bs_generic(unsigned int v, int mode)
{
    unsigned int r = __builtin_clz(v);
    if (mode & BS_CLZ)
    {
        if (mode & BS_0_0)
            r &= 31;
    }
    else
    {
        r = 31 - r;
        if ((mode & BS_0_0) && (signed)r < 0)
            r += 1;
    }
    return r;
}

/* swap32 for bits.h BSWAP macro */
#ifndef swap32
#define swap32(x) __builtin_bswap32(x)
#endif

#endif /* FAAD_CODECLIB_SHIM_H */
