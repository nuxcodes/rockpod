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
 * ArtworkDB parser — reads iTunes ArtworkDB + ithmb thumbnail files
 * for video thumbnail display in the Videos menu.
 *
 * ArtworkDB structure:
 *   mhfd → mhsd(type 1) → mhli → mhii* → mhod(type 2)* → mhni → mhod(type 3)
 *
 * mhii.id matches mhit.mhii_link (iTunesDB offset 352).
 * mhni contains ithmb file reference, byte offset, and dimensions.
 * ithmb files contain raw byte-swapped RGB565 pixel data.
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
#include "artworkdb.h"

#include <string.h>
#include <stdio.h>

#define ARTWORKDB_PATH "/iPod_Control/Artwork/ArtworkDB"

/* mhii field offsets (from mhii start) */
#define MHII_HEADER_LEN    4
#define MHII_TOTAL_LEN     8
#define MHII_NUM_CHILDREN 12
#define MHII_ID           16   /* 4 bytes — matches mhit.mhii_link */
#define MHII_DBID         20   /* 8 bytes — matches mhit.dbid */
#define MHII_HEADER_SIZE  0x98 /* 152 bytes, fixed */

/* mhni field offsets (from mhni start) */
#define MHNI_HEADER_LEN    4
#define MHNI_TOTAL_LEN     8
#define MHNI_NUM_CHILDREN 12
#define MHNI_CORR_ID      16   /* 4 bytes — file ref → F{id}_1.ithmb */
#define MHNI_ITHMB_OFF    20   /* 4 bytes — byte offset in ithmb file */
#define MHNI_IMG_SIZE     24   /* 4 bytes — image data size in bytes */
#define MHNI_IMG_HEIGHT   32   /* 2 bytes */
#define MHNI_IMG_WIDTH    34   /* 2 bytes */
#define MHNI_HEADER_SIZE  0x4c /* 76 bytes, fixed */

/* mhod field offsets */
#define MHOD_HEADER_LEN    4
#define MHOD_TOTAL_LEN     8
#define MHOD_TYPE         12   /* 2 bytes (uint16) */

/* mhod string (type 3): UTF-16LE filename after 24-byte mhod header + 8 pad */
#define MHOD3_STR_LEN    24   /* offset of string length field */
#define MHOD3_STR_DATA   32   /* offset of UTF-16LE string data */

static uint32_t adb_get_le32(const unsigned char *buf)
{
    return (uint32_t)buf[0]
         | ((uint32_t)buf[1] << 8)
         | ((uint32_t)buf[2] << 16)
         | ((uint32_t)buf[3] << 24);
}

static uint16_t adb_get_le16(const unsigned char *buf)
{
    return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

static uint64_t adb_get_le64(const unsigned char *buf)
{
    return (uint64_t)adb_get_le32(buf)
         | ((uint64_t)adb_get_le32(buf + 4) << 32);
}

static ssize_t adb_read_at(int fd, off_t offset, void *buf, size_t n)
{
    if (lseek(fd, offset, SEEK_SET) != offset)
        return -1;
    return read(fd, buf, n);
}

/* Thumbnail candidate found during mhii scan */
struct thumb_info {
    uint32_t corr_id;
    uint32_t ithmb_off;
    uint32_t img_size;
    uint16_t width;
    uint16_t height;
    uint32_t pixels; /* width * height, for picking best */
    bool valid;
};

/* Parse an mhni record and fill thumb_info.
 * Returns true if the mhni is a usable thumbnail. */
static bool parse_mhni(int fd, off_t mhni_offset, struct thumb_info *ti)
{
    unsigned char hdr[MHNI_HEADER_SIZE];

    if (adb_read_at(fd, mhni_offset, hdr, MHNI_HEADER_SIZE)
        < MHNI_HEADER_SIZE)
        return false;

    if (memcmp(hdr, "mhni", 4) != 0)
        return false;

    ti->corr_id   = adb_get_le32(hdr + MHNI_CORR_ID);
    ti->ithmb_off = adb_get_le32(hdr + MHNI_ITHMB_OFF);
    ti->img_size  = adb_get_le32(hdr + MHNI_IMG_SIZE);
    ti->height    = adb_get_le16(hdr + MHNI_IMG_HEIGHT);
    ti->width     = adb_get_le16(hdr + MHNI_IMG_WIDTH);
    ti->pixels    = (uint32_t)ti->width * ti->height;
    ti->valid     = (ti->width > 0 && ti->height > 0 && ti->img_size > 0);

    return ti->valid;
}

#define MAX_THUMB_CANDIDATES 8

/* Collect all thumbnail candidates from mhii children.
 * Returns count of valid candidates found. */
static int collect_thumbs(int fd, off_t mhii_offset,
                          uint32_t mhii_total_len,
                          uint32_t num_children,
                          struct thumb_info *candidates, int max_cand)
{
    off_t pos = mhii_offset + MHII_HEADER_SIZE;
    off_t end = mhii_offset + mhii_total_len;
    uint32_t c;
    int ncand = 0;

    for (c = 0; c < num_children && pos < end; c++)
    {
        unsigned char od_hdr[16];
        uint32_t od_hdr_len, od_total_len;
        uint16_t od_type;

        if (adb_read_at(fd, pos, od_hdr, 16) < 16)
            break;

        if (memcmp(od_hdr, "mhod", 4) != 0)
            break;

        od_hdr_len  = adb_get_le32(od_hdr + MHOD_HEADER_LEN);
        od_total_len = adb_get_le32(od_hdr + MHOD_TOTAL_LEN);
        od_type     = adb_get_le16(od_hdr + MHOD_TYPE);

        if (od_type == 2 && ncand < max_cand)
        {
            off_t mhni_off = pos + od_hdr_len;
            if (parse_mhni(fd, mhni_off, &candidates[ncand]))
                ncand++;
        }

        pos += od_total_len;
    }

    return ncand;
}

/* Sort candidates: fitting sizes first (smallest first), then
 * non-fitting sizes (largest first). Simple insertion sort. */
static void sort_thumbs(struct thumb_info *cands, int n,
                        int target_w, int target_h)
{
    int i, j;

    for (i = 1; i < n; i++)
    {
        struct thumb_info key = cands[i];
        bool key_fits = (key.width >= (uint16_t)target_w
                      && key.height >= (uint16_t)target_h);

        j = i - 1;
        while (j >= 0)
        {
            bool j_fits = (cands[j].width >= (uint16_t)target_w
                        && cands[j].height >= (uint16_t)target_h);
            bool swap = false;

            if (key_fits && !j_fits)
                swap = true;
            else if (key_fits && j_fits && key.pixels < cands[j].pixels)
                swap = true;
            else if (!key_fits && !j_fits && key.pixels > cands[j].pixels)
                swap = true;

            if (!swap)
                break;

            cands[j + 1] = cands[j];
            j--;
        }
        cands[j + 1] = key;
    }
}

/* Load raw RGB565 pixels from an ithmb file and scale to out_w x out_h
 * with letterboxing.  iPod Classic stores ithmb as native LE RGB565. */
static bool load_ithmb(uint32_t corr_id, uint32_t ithmb_off,
                       int src_w, int src_h, uint32_t src_size,
                       fb_data *out, int out_w, int out_h,
                       void *work_buf, size_t work_size)
{
    char path[64];
    int fd;
    fb_data *raw;
    int x, y, sx, sy;
    int dst_w, dst_h, pad_x, pad_y;

    /* Need enough work space for the source pixel data */
    if (src_size > work_size)
        return false;

    snprintf(path, sizeof(path),
             "/iPod_Control/Artwork/F%04lu_1.ithmb",
             (unsigned long)corr_id);

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;

    if (lseek(fd, ithmb_off, SEEK_SET) != (off_t)ithmb_off)
    {
        close(fd);
        return false;
    }

    if (read(fd, work_buf, src_size) < (ssize_t)src_size)
    {
        close(fd);
        return false;
    }
    close(fd);

    /* iPod Classic ithmb is native little-endian RGB565 — no byte-swap */
    raw = (fb_data *)work_buf;

    /* Reject blank thumbnails (all white or all black) */
    {
        int npix = src_w * src_h;
        int check = (npix > 64) ? 64 : npix;
        bool all_same = true;
        fb_data first = raw[0];
        int k;
        for (k = 1; k < check; k++)
        {
            if (raw[k] != first) { all_same = false; break; }
        }
        if (all_same)
            return false;
    }

    /* Compute letterbox dimensions preserving aspect ratio */
    dst_w = out_w;
    dst_h = (src_h * out_w) / src_w;
    if (dst_h > out_h)
    {
        dst_h = out_h;
        dst_w = (src_w * out_h) / src_h;
    }
    pad_x = (out_w - dst_w) / 2;
    pad_y = (out_h - dst_h) / 2;

    /* Clear to black */
    memset(out, 0, out_w * out_h * sizeof(fb_data));

    /* Nearest-neighbor scale */
    for (y = 0; y < dst_h; y++)
    {
        sy = y * src_h / dst_h;
        if (sy >= src_h) sy = src_h - 1;
        for (x = 0; x < dst_w; x++)
        {
            sx = x * src_w / dst_w;
            if (sx >= src_w) sx = src_w - 1;
            out[(y + pad_y) * out_w + (x + pad_x)] =
                (fb_data)raw[sy * src_w + sx];
        }
    }

    return true;
}

bool artworkdb_load_thumb(uint64_t dbid, uint32_t mhii_link,
                          fb_data *out, int out_w, int out_h,
                          void *work_buf, size_t work_size)
{
    int fd;
    unsigned char hdr[32];
    uint32_t mhfd_hdr_len;
    off_t pos;
    uint32_t mhsd_hdr_len, mhsd_total_len, mhsd_type;
    off_t mhli_offset = 0;
    uint32_t num_images = 0;
    uint32_t mhli_hdr_len;
    off_t mhii_pos;
    uint32_t i;

    fd = open(ARTWORKDB_PATH, O_RDONLY);
    if (fd < 0)
        return false;

    /* Parse mhfd header */
    if (adb_read_at(fd, 0, hdr, 16) < 16 || memcmp(hdr, "mhfd", 4) != 0)
    {
        close(fd);
        return false;
    }
    mhfd_hdr_len = adb_get_le32(hdr + 4);

    /* Find mhsd type 1 (image list) */
    pos = mhfd_hdr_len;
    for (;;)
    {
        if (adb_read_at(fd, pos, hdr, 16) < 16)
            break;
        if (memcmp(hdr, "mhsd", 4) != 0)
            break;

        mhsd_hdr_len   = adb_get_le32(hdr + 4);
        mhsd_total_len = adb_get_le32(hdr + 8);
        mhsd_type      = adb_get_le32(hdr + 12);

        if (mhsd_type == 1)
        {
            mhli_offset = pos + mhsd_hdr_len;
            break;
        }
        pos += mhsd_total_len;
    }

    if (mhli_offset == 0)
    {
        close(fd);
        return false;
    }

    /* Parse mhli header */
    if (adb_read_at(fd, mhli_offset, hdr, 12) < 12
        || memcmp(hdr, "mhli", 4) != 0)
    {
        close(fd);
        return false;
    }
    mhli_hdr_len = adb_get_le32(hdr + 4);
    num_images   = adb_get_le32(hdr + 8);

    /* Walk mhii records looking for our track */
    mhii_pos = mhli_offset + mhli_hdr_len;

    for (i = 0; i < num_images; i++)
    {
        unsigned char mhii_hdr[28];
        uint32_t mhii_total, mhii_children, mhii_id;
        uint64_t mhii_dbid;

        if (adb_read_at(fd, mhii_pos, mhii_hdr, 28) < 28)
            break;
        if (memcmp(mhii_hdr, "mhii", 4) != 0)
            break;

        mhii_total    = adb_get_le32(mhii_hdr + MHII_TOTAL_LEN);
        mhii_children = adb_get_le32(mhii_hdr + MHII_NUM_CHILDREN);
        mhii_id       = adb_get_le32(mhii_hdr + MHII_ID);
        mhii_dbid     = adb_get_le64(mhii_hdr + MHII_DBID);

        /* Match by mhii_link (fast) or dbid (fallback) */
        if ((mhii_link != 0 && mhii_id == mhii_link)
            || (mhii_link == 0 && mhii_dbid == dbid))
        {
            struct thumb_info cands[MAX_THUMB_CANDIDATES];
            int ncand, ci;

            ncand = collect_thumbs(fd, mhii_pos, mhii_total,
                                   mhii_children, cands,
                                   MAX_THUMB_CANDIDATES);
            close(fd);

            if (ncand == 0)
                return false;

            sort_thumbs(cands, ncand, out_w, out_h);

            /* Try each candidate; skip blank/oversized thumbnails */
            for (ci = 0; ci < ncand; ci++)
            {
                if (load_ithmb(cands[ci].corr_id, cands[ci].ithmb_off,
                               cands[ci].width, cands[ci].height,
                               cands[ci].img_size,
                               out, out_w, out_h,
                               work_buf, work_size))
                    return true;
            }
            return false;
        }

        mhii_pos += mhii_total;
    }

    close(fd);
    return false;
}
