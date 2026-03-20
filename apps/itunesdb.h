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
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#ifndef __ITUNESDB_H__
#define __ITUNESDB_H__

#include <stdint.h>
#include <stddef.h>

enum video_type {
    VIDEO_MOVIE = 0,
    VIDEO_MUSIC_VID,
    VIDEO_TV_SHOW,
    VIDEO_TYPE_COUNT
};

struct video_entry {
    char title[128];
    char artist[64];
    char filepath[256];
    uint32_t duration_ms;
    enum video_type type;
    uint64_t dbid;           /* mhit+112: ArtworkDB lookup key */
    uint32_t mhii_link;      /* mhit+352: → ArtworkDB mhii.id */
    uint16_t artwork_count;  /* mhit+124: ≥1 required for ithmb */
    uint8_t  has_artwork;    /* mhit+164: 0x01=yes, 0x02=no */
};

struct video_library {
    struct video_entry *entries;
    int count;
    int capacity;
    int counts[VIDEO_TYPE_COUNT];
};

/* Parse iTunesDB and populate library with video entries.
 * buffer/bufsize: memory for video_entry array.
 * Returns number of video entries found, or -1 on error. */
int itunesdb_load_videos(struct video_library *lib,
                         void *buffer, size_t bufsize);

#endif /* __ITUNESDB_H__ */
