/***************************************************************************
 * Linker stubs for codec_malloc/codec_calloc/codec_free/codec_realloc.
 *
 * Never called at runtime — FAAD_STATIC_ALLOC eliminates all dynamic
 * allocation in libfaad (verified: 0 reachable malloc calls in AAC-LC
 * decode path). These exist solely to satisfy the linker because
 * codeclib.h's #define malloc(x) codec_malloc(x) macro catches any
 * stray malloc() calls in translation units that include <stdlib.h>.
 ****************************************************************************/

#include <stddef.h>
#include <string.h>

void *codec_malloc(size_t s)
{
    (void)s;
    return NULL;
}

void *codec_calloc(size_t n, size_t s)
{
    (void)n;
    (void)s;
    return NULL;
}

void *codec_realloc(void *p, size_t s)
{
    (void)p;
    (void)s;
    return NULL;
}

void codec_free(void *p)
{
    (void)p;
}

size_t codec_strlen(const char *s)
{
    return __builtin_strlen(s);
}
