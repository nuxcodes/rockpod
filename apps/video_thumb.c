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
 * Video thumbnail extraction — decodes JPEG cover art and scales/letterboxes
 * into a square thumbnail for the Videos menu.
 *
 * Sources (tried in order):
 * 1. Embedded covr atom in MP4 file
 * 2. Standalone JPEG file (for fallback/testing)
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
#include "config.h"
#include "system.h"
#include "file.h"
#include "lcd.h"
#include "recorder/jpeg_load.h"
#include "mp4_demux.h"
#include "video_thumb.h"

#include <string.h>

/* Letterbox the decoded bitmap into a THUMB_SIZE x THUMB_SIZE square */
static void letterbox_to_thumb(struct bitmap *bm, void *thumb_buf)
{
    fb_data *src = (fb_data *)bm->data;
    fb_data *dst = (fb_data *)thumb_buf;
    int src_w = bm->width;
    int src_h = bm->height;
    int pad_x = (THUMB_SIZE - src_w) / 2;
    int pad_y = (THUMB_SIZE - src_h) / 2;
    int y;

    /* Clear to black */
    memset(dst, 0, THUMB_BYTES);

    /* Clamp to avoid out-of-bounds */
    if (pad_x < 0) pad_x = 0;
    if (pad_y < 0) pad_y = 0;
    if (src_w > THUMB_SIZE) src_w = THUMB_SIZE;
    if (src_h > THUMB_SIZE) src_h = THUMB_SIZE;

    for (y = 0; y < src_h && (y + pad_y) < THUMB_SIZE; y++)
    {
        memcpy(&dst[(y + pad_y) * THUMB_SIZE + pad_x],
               &src[y * src_w],
               src_w * sizeof(fb_data));
    }
}

bool video_thumb_extract(const char *filepath,
                         void *thumb_buf,
                         void *work_buf, size_t work_size)
{
    struct mp4v_demux_res res;
    uint32_t dummy_samples[1];
    uint32_t dummy_chunks[1];
    int fd;
    struct bitmap bm;
    int ret;

    /* Try embedded cover art from MP4 */
    if (mp4v_demux_open(filepath, &res, dummy_samples, 1,
                        dummy_chunks, 1) == 0
        && res.cover_size > 0 && res.cover_offset > 0)
    {
        fd = open(filepath, O_RDONLY);
        if (fd >= 0)
        {
            lseek(fd, res.cover_offset, SEEK_SET);

            memset(&bm, 0, sizeof(bm));
            bm.width = THUMB_SIZE;
            bm.height = THUMB_SIZE;
            bm.data = (unsigned char *)work_buf;

            ret = clip_jpeg_fd(fd, 0, res.cover_size, &bm,
                               work_size,
                               FORMAT_NATIVE | FORMAT_RESIZE | FORMAT_KEEP_ASPECT,
                               NULL);
            close(fd);

            if (ret > 0)
            {
                letterbox_to_thumb(&bm, thumb_buf);
                return true;
            }
        }
    }

    return false;
}

bool video_thumb_from_jpeg(const char *jpeg_path,
                           void *thumb_buf,
                           void *work_buf, size_t work_size)
{
    struct bitmap bm;
    int ret;

    memset(&bm, 0, sizeof(bm));
    bm.width = THUMB_SIZE;
    bm.height = THUMB_SIZE;
    bm.data = (unsigned char *)work_buf;

    ret = read_jpeg_file(jpeg_path, &bm, work_size,
                         FORMAT_NATIVE | FORMAT_RESIZE | FORMAT_KEEP_ASPECT,
                         NULL);
    if (ret <= 0)
        return false;
    letterbox_to_thumb(&bm, thumb_buf);
    return true;
}
