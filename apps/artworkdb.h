/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2025
 *
 * ArtworkDB parser — reads iTunes ArtworkDB + ithmb thumbnail files.
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
#ifndef __ARTWORKDB_H__
#define __ARTWORKDB_H__

#include <stdint.h>
#include <stdbool.h>
#include "lcd.h"

/* Load a thumbnail from the iTunes ArtworkDB into an RGB565 buffer.
 *
 * Looks up the artwork image item (mhii) by mhii_link (primary match
 * on mhii.id) or dbid (fallback match on mhii.dbid).  Picks the
 * smallest available thumbnail whose dimensions are >= out_w x out_h.
 * Reads raw RGB565 pixel data from the corresponding .ithmb file,
 * byte-swaps, and nearest-neighbor scales to out_w x out_h with
 * letterboxing.
 *
 * Returns true on success, false if ArtworkDB is missing, the track
 * has no artwork entry, or the ithmb file cannot be read. */
bool artworkdb_load_thumb(uint64_t dbid, uint32_t mhii_link,
                          fb_data *out, int out_w, int out_h,
                          void *work_buf, size_t work_size);

#endif /* __ARTWORKDB_H__ */
