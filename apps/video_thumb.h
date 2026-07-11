/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2025 Nux Li
 *
 * Video thumbnail extraction from MP4 cover art.
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
#ifndef __VIDEO_THUMB_H__
#define __VIDEO_THUMB_H__

#include "lcd.h"

/* Thumbnail dimensions — square with letterboxed content */
#define THUMB_SIZE  48
#define THUMB_BPP   sizeof(fb_data)
#define THUMB_BYTES (THUMB_SIZE * THUMB_SIZE * THUMB_BPP)

/* Extract a thumbnail from an MP4 file's cover art.
 * Decodes the embedded JPEG and scales/letterboxes to THUMB_SIZE x THUMB_SIZE.
 *
 * filepath:  path to the .mp4/.m4v file
 * thumb_buf: output buffer, must be at least THUMB_BYTES
 * work_buf:  working memory for JPEG decoder (needs ~50KB)
 * work_size: size of work_buf
 *
 * Returns true on success, false if no cover art or decode failed. */
bool video_thumb_extract(const char *filepath,
                         void *thumb_buf,
                         void *work_buf, size_t work_size);

/* Load a thumbnail from a standalone JPEG file (for fallback/testing). */
bool video_thumb_from_jpeg(const char *jpeg_path,
                           void *thumb_buf,
                           void *work_buf, size_t work_size);

#endif /* __VIDEO_THUMB_H__ */
