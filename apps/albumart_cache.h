/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 Rockbox contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#ifndef _ALBUMART_CACHE_H_
#define _ALBUMART_CACHE_H_

#include "config.h"

#ifdef HAVE_ALBUMART

#include <stddef.h>
#include <stdbool.h>
#include "bmp.h"
#include "metadata.h"

#define AA_WINDOW_RADIUS 2
#define AA_WINDOW_SIZE   (2 * AA_WINDOW_RADIUS + 1)

struct dim;

/* Bytes to carve from the audio buffer for the given claimed AA dims.
 * dims[i] is unused when width or height is 0. */
size_t albumart_cache_pool_size(const struct dim *dims, int nslots);

/* Bind the cache to a freshly carved pool. Clears all loaded images. */
void albumart_cache_reset(void *buf, size_t size,
                          const struct dim *dims, int nslots);

/* Drop decoded images but keep the pool (playback stop). */
void albumart_cache_clear(void);

/* Invalidate in-flight decode and start a new window fill. */
void albumart_cache_kick(void);

/* Decode and publish one playlist-relative offset (all claimed dims).
 * id3 may be NULL when metadata is unavailable. Returns true if this
 * completed offset 0 for the current generation. */
bool albumart_cache_work(int pl_offset, const struct mp3entry *id3);

/* Bitmap for playlist-relative offset and claimed dim slot, or NULL. */
struct bitmap *albumart_cache_get(int pl_offset, int dim_slot);

/* Serialize skip_offset vs window: lock, slide, unlock. */
void albumart_cache_lock(void);
void albumart_cache_unlock(void);
void albumart_cache_slide_locked(int delta);
struct bitmap *albumart_cache_get_locked(int pl_offset, int dim_slot);

struct albumart_cache_debug {
    size_t pool_size;   /* decoded-image capacity, excluding decode scratch */
    size_t used_size;   /* bytes in occupied slots */
    int slots_used;
    int slots_total;
    /* occupied[i] is playlist offset i - AA_WINDOW_RADIUS */
    char occupied[AA_WINDOW_SIZE];
    int work_index;     /* 0..AA_WINDOW_SIZE; SIZE means idle */
    int work_offset;    /* next/current playlist-relative load offset */
    int skip_offset;    /* WPS preview offset; 0 if none */
};

void albumart_cache_get_debugdata(struct albumart_cache_debug *dbg);

#endif /* HAVE_ALBUMART */

#endif /* _ALBUMART_CACHE_H_ */
