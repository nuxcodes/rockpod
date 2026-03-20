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
 * iTunesDB binary parser - extracts video track metadata from
 * /iPod_Control/iTunes/iTunesDB for the Videos menu.
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
#include "itunesdb.h"
#include "string-extra.h"

#include <string.h>

#define ITUNESDB_PATH "/iPod_Control/iTunes/iTunesDB"

/* iTunesDB mediatype values */
#define MEDIATYPE_AUDIO       0x01
#define MEDIATYPE_MOVIE       0x02
#define MEDIATYPE_PODCAST     0x04
#define MEDIATYPE_VIDPODCAST  0x06
#define MEDIATYPE_AUDIOBOOK   0x08
#define MEDIATYPE_MUSICVID    0x20
#define MEDIATYPE_TVSHOW      0x40
#define MEDIATYPE_TVSHOW2     0x60

/* mhit field offsets */
#define MHIT_HEADER_LEN       4
#define MHIT_TOTAL_LEN        8
#define MHIT_NUM_MHOD        12
#define MHIT_DURATION_MS      40
#define MHIT_DBID            112
#define MHIT_ARTWORK_COUNT   124
#define MHIT_HAS_ARTWORK     164
#define MHIT_MEDIATYPE       208
#define MHIT_MHII_LINK       352

/* mhod field offsets */
#define MHOD_HEADER_LEN       4
#define MHOD_TOTAL_LEN        8
#define MHOD_TYPE            12
#define MHOD_STRING_LEN      28
#define MHOD_ENCODING        32
#define MHOD_STRING_DATA     40

/* mhod string types */
#define MHOD_TITLE            1
#define MHOD_FILEPATH         2
#define MHOD_ARTIST           4

/* Read buffer for sequential I/O */
#define READ_BUF_SIZE  4096

static uint32_t get_le32(const unsigned char *buf)
{
    return (uint32_t)buf[0]
         | ((uint32_t)buf[1] << 8)
         | ((uint32_t)buf[2] << 16)
         | ((uint32_t)buf[3] << 24);
}

static uint16_t get_le16(const unsigned char *buf)
{
    return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

static uint64_t get_le64(const unsigned char *buf)
{
    return (uint64_t)get_le32(buf) | ((uint64_t)get_le32(buf + 4) << 32);
}

/* Read exactly n bytes at a given offset. Returns n on success, <n on error. */
static ssize_t read_at(int fd, off_t offset, void *buf, size_t n)
{
    if (lseek(fd, offset, SEEK_SET) != offset)
        return -1;
    return read(fd, buf, n);
}

/* Convert UTF-16LE string to ASCII/Latin-1, stripping high bytes.
 * src_len is in bytes (not characters). */
static void utf16le_to_ascii(const unsigned char *src, size_t src_len,
                             char *dst, size_t dst_size)
{
    size_t i, j;
    size_t nchars = src_len / 2;

    for (i = 0, j = 0; i < nchars && j < dst_size - 1; i++)
    {
        unsigned char lo = src[i * 2];
        unsigned char hi = src[i * 2 + 1];

        if (hi == 0 && lo >= 0x20)
            dst[j++] = (char)lo;
        else if (hi == 0 && lo == 0)
            break;
        else
            dst[j++] = '?';
    }
    dst[j] = '\0';
}

/* Convert iTunesDB colon-separated path to slash-separated.
 * Input:  ":iPod_Control:Music:F06:DTVC.mp4"
 * Output: "/iPod_Control/Music/F06/DTVC.mp4" */
static void convert_path(const char *src, char *dst, size_t dst_size)
{
    size_t i;

    for (i = 0; src[i] && i < dst_size - 1; i++)
    {
        if (src[i] == ':')
            dst[i] = '/';
        else
            dst[i] = src[i];
    }
    dst[i] = '\0';
}

static enum video_type mediatype_to_videotype(uint32_t mediatype)
{
    switch (mediatype)
    {
        case MEDIATYPE_MOVIE:
            return VIDEO_MOVIE;
        case MEDIATYPE_MUSICVID:
            return VIDEO_MUSIC_VID;
        case MEDIATYPE_TVSHOW:
        case MEDIATYPE_TVSHOW2:
            return VIDEO_TV_SHOW;
        default:
            return VIDEO_TYPE_COUNT; /* invalid */
    }
}

static bool is_video_mediatype(uint32_t mediatype)
{
    return mediatype == MEDIATYPE_MOVIE
        || mediatype == MEDIATYPE_MUSICVID
        || mediatype == MEDIATYPE_TVSHOW
        || mediatype == MEDIATYPE_TVSHOW2;
}

/* Read a string mhod and store it in dst.
 * Returns 0 on success, -1 on error. */
static int read_mhod_string(int fd, off_t mhod_offset,
                            uint32_t mhod_total_len,
                            char *dst, size_t dst_size)
{
    unsigned char buf[8];
    uint32_t string_len, encoding;
    unsigned char *strbuf;
    unsigned char stackbuf[512];

    if (mhod_total_len < MHOD_STRING_DATA + 2)
        return -1;

    /* Read string_len and encoding fields */
    if (read_at(fd, mhod_offset + MHOD_STRING_LEN, buf, 8) < 8)
        return -1;

    string_len = get_le32(buf);
    encoding = get_le32(buf + 4);

    if (string_len == 0 || string_len > mhod_total_len - MHOD_STRING_DATA)
        return -1;

    /* Use stack buffer if small enough, otherwise skip large strings */
    if (string_len > sizeof(stackbuf))
        return -1;

    strbuf = stackbuf;

    if (read_at(fd, mhod_offset + MHOD_STRING_DATA, strbuf, string_len)
        < (ssize_t)string_len)
        return -1;

    if (encoding == 2)
    {
        /* UTF-8 */
        size_t copy_len = string_len;
        if (copy_len >= dst_size)
            copy_len = dst_size - 1;
        memcpy(dst, strbuf, copy_len);
        dst[copy_len] = '\0';
    }
    else
    {
        /* UTF-16LE (encoding 0 or 1) */
        utf16le_to_ascii(strbuf, string_len, dst, dst_size);
    }

    return 0;
}

/* Parse a single mhit + its mhod children.
 * Returns 1 if a video entry was added, 0 if skipped, -1 on error. */
static int parse_mhit(int fd, off_t mhit_offset,
                      struct video_library *lib)
{
    unsigned char hdr[356];
    uint32_t hdr_len, num_mhod, duration_ms, mediatype;
    struct video_entry *entry;
    off_t mhod_offset;
    uint32_t i;
    char raw_path[256];

    /* Read enough of the mhit header for all fields up to mhii_link (352+4) */
    if (read_at(fd, mhit_offset, hdr, 356) < 212)
        return -1;

    /* Verify magic */
    if (memcmp(hdr, "mhit", 4) != 0)
        return -1;

    hdr_len   = get_le32(hdr + MHIT_HEADER_LEN);
    num_mhod  = get_le32(hdr + MHIT_NUM_MHOD);
    duration_ms = get_le32(hdr + MHIT_DURATION_MS);
    mediatype = get_le32(hdr + MHIT_MEDIATYPE);

    /* Skip non-video tracks */
    if (!is_video_mediatype(mediatype))
        return 0;

    /* Check capacity */
    if (lib->count >= lib->capacity)
        return 0;

    entry = &lib->entries[lib->count];
    memset(entry, 0, sizeof(*entry));
    entry->duration_ms = duration_ms;
    entry->type = mediatype_to_videotype(mediatype);

    /* Extract artwork fields (only if header is large enough) */
    if (hdr_len >= MHIT_DBID + 8)
        entry->dbid = get_le64(hdr + MHIT_DBID);
    if (hdr_len >= MHIT_ARTWORK_COUNT + 2)
        entry->artwork_count = get_le16(hdr + MHIT_ARTWORK_COUNT);
    if (hdr_len >= MHIT_HAS_ARTWORK + 1)
        entry->has_artwork = hdr[MHIT_HAS_ARTWORK];
    if (hdr_len >= MHIT_MHII_LINK + 4)
        entry->mhii_link = get_le32(hdr + MHIT_MHII_LINK);

    /* Parse mhod children for title, path, artist */
    mhod_offset = mhit_offset + hdr_len;
    raw_path[0] = '\0';

    for (i = 0; i < num_mhod; i++)
    {
        unsigned char mhod_hdr[16];
        uint32_t mhod_total, mhod_type;

        if (read_at(fd, mhod_offset, mhod_hdr, 16) < 16)
            break;

        if (memcmp(mhod_hdr, "mhod", 4) != 0)
            break;

        mhod_total = get_le32(mhod_hdr + MHOD_TOTAL_LEN);
        mhod_type  = get_le32(mhod_hdr + MHOD_TYPE);

        if (mhod_type == MHOD_TITLE && entry->title[0] == '\0')
        {
            read_mhod_string(fd, mhod_offset, mhod_total,
                             entry->title, sizeof(entry->title));
        }
        else if (mhod_type == MHOD_FILEPATH && raw_path[0] == '\0')
        {
            read_mhod_string(fd, mhod_offset, mhod_total,
                             raw_path, sizeof(raw_path));
        }
        else if (mhod_type == MHOD_ARTIST && entry->artist[0] == '\0')
        {
            read_mhod_string(fd, mhod_offset, mhod_total,
                             entry->artist, sizeof(entry->artist));
        }

        mhod_offset += mhod_total;
    }

    /* Convert colon path to slash path */
    if (raw_path[0])
        convert_path(raw_path, entry->filepath, sizeof(entry->filepath));

    /* Only add if we got at least a title or filepath */
    if (entry->title[0] || entry->filepath[0])
    {
        if (entry->title[0] == '\0')
            strmemccpy(entry->title, entry->filepath, sizeof(entry->title));

        lib->counts[entry->type]++;
        lib->count++;
        return 1;
    }

    return 0;
}

int itunesdb_load_videos(struct video_library *lib,
                         void *buffer, size_t bufsize)
{
    int fd;
    unsigned char hdr[256];
    uint32_t mhbd_hdr_len;
    off_t offset;
    uint32_t mhsd_type, mhsd_hdr_len, mhsd_total_len;
    off_t tracks_offset = 0;
    uint32_t track_count = 0;
    uint32_t mhlt_hdr_len;
    off_t mhit_offset;
    uint32_t i;

    /* Initialize library */
    lib->entries = (struct video_entry *)buffer;
    lib->capacity = bufsize / sizeof(struct video_entry);
    lib->count = 0;
    memset(lib->counts, 0, sizeof(lib->counts));

    if (lib->capacity == 0)
        return -1;

    fd = open(ITUNESDB_PATH, O_RDONLY);
    if (fd < 0)
        return -1;

    /* Read and validate mhbd header */
    if (read_at(fd, 0, hdr, 16) < 16 || memcmp(hdr, "mhbd", 4) != 0)
    {
        close(fd);
        return -1;
    }

    mhbd_hdr_len = get_le32(hdr + 4);

    /* Walk mhsd sections to find type 1 (Track List) */
    offset = mhbd_hdr_len;

    for (;;)
    {
        if (read_at(fd, offset, hdr, 16) < 16)
            break;

        if (memcmp(hdr, "mhsd", 4) != 0)
            break;

        mhsd_hdr_len   = get_le32(hdr + 4);
        mhsd_total_len = get_le32(hdr + 8);
        mhsd_type      = get_le32(hdr + 12);

        if (mhsd_type == 1)
        {
            tracks_offset = offset + mhsd_hdr_len;
            break;
        }

        offset += mhsd_total_len;
    }

    if (tracks_offset == 0)
    {
        close(fd);
        return -1;
    }

    /* Read mhlt header */
    if (read_at(fd, tracks_offset, hdr, 12) < 12
        || memcmp(hdr, "mhlt", 4) != 0)
    {
        close(fd);
        return -1;
    }

    mhlt_hdr_len = get_le32(hdr + 4);
    track_count  = get_le32(hdr + 8);

    /* Iterate mhit records */
    mhit_offset = tracks_offset + mhlt_hdr_len;

    for (i = 0; i < track_count; i++)
    {
        unsigned char mhit_peek[12];
        uint32_t mhit_total;

        if (read_at(fd, mhit_offset, mhit_peek, 12) < 12)
            break;

        if (memcmp(mhit_peek, "mhit", 4) != 0)
            break;

        mhit_total = get_le32(mhit_peek + 8);

        parse_mhit(fd, mhit_offset, lib);

        mhit_offset += mhit_total;
    }

    close(fd);

    return lib->count;
}
