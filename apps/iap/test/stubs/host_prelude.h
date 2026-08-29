/* Host test harness prelude, force-included before everything else.
 *
 * Bridges the few places where Rockbox's freestanding libc assumptions
 * collide with a hosted macOS/Linux libc. Nothing here alters iAP
 * behaviour; the protocol sources are compiled unmodified.
 */
#ifndef IAP_TEST_HOST_PRELUDE_H
#define IAP_TEST_HOST_PRELUDE_H

#include <stddef.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

/* Rockbox re-declares the POSIX file and directory calls for its own
 * filesystem layer. On a hosted build those clash with the system
 * headers, so tell firmware/include/{file,dir,filesystem-native}.h that
 * the declarations are already in scope and let libc provide them.
 * Directory types stay Rockbox's own: the iAP sources use DIR and
 * dirent.d_name, which the harness implements in rb_stubs.c.
 */
#define FILEFUNCTIONS_DECLARED
#define FILEFUNCTIONS_DEFINED

/* GNU extension used by firmware/include/string-extra.h, absent on macOS. */
#ifndef HAVE_MEMPCPY
#define HAVE_MEMPCPY
static inline void *mempcpy(void *d, const void *s, size_t n)
{
    memcpy(d, s, n);
    return (char *)d + n;
}
#endif

#endif
