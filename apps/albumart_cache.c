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

#include "config.h"

#ifdef HAVE_ALBUMART

#include <string.h>
#include "albumart_cache.h"
#include "albumart.h"
#include "file.h"
#include "kernel.h"
#include "logf.h"
#include "pathfuncs.h"
#include "settings.h"
#include "string-extra.h"
#include "system.h"

#define AA_MAX_DIM_SLOTS 4

enum aa_src
{
    AA_SRC_NONE = 0,
    AA_SRC_FILE,
    AA_SRC_EMBEDDED,
};

struct aa_ident
{
    enum aa_src src;
    int dim_slot;
    off_t emb_pos;
    int emb_size;
    char path[MAX_PATH];
};

struct aa_block
{
    struct aa_ident ident;
    struct bitmap *bmp;
    bool in_use;
};

static struct mutex aa_mutex;

static void *scratch;
static size_t scratch_size;

static struct dim slot_dim[AA_MAX_DIM_SLOTS];
static size_t slot_block_size[AA_MAX_DIM_SLOTS];
static int nslots;

static struct aa_block blocks[AA_MAX_DIM_SLOTS][AA_WINDOW_SIZE];
/* Published window: playlist offset -2..+2 -> block index or -1 */
static int window[AA_WINDOW_SIZE][AA_MAX_DIM_SLOTS];
static unsigned int work_gen;

static int win_index(int pl_offset)
{
    return pl_offset + AA_WINDOW_RADIUS;
}

static bool ident_equal(const struct aa_ident *a, const struct aa_ident *b)
{
    if (a->src != b->src || a->dim_slot != b->dim_slot ||
        a->src == AA_SRC_NONE)
        return false;

    if (a->src == AA_SRC_EMBEDDED &&
        (a->emb_pos != b->emb_pos || a->emb_size != b->emb_size))
        return false;

    return strcmp(a->path, b->path) == 0;
}

static void ident_clear(struct aa_ident *id)
{
    memset(id, 0, sizeof(*id));
}

static bool resolve_ident(const struct mp3entry *id3, int dim_slot,
                          struct aa_ident *out)
{
    ident_clear(out);
    out->dim_slot = dim_slot;

    if (!id3 || global_settings.album_art == AA_OFF)
        return false;

    char path[MAX_PATH];
    bool checked_image_file = false;

    if (global_settings.album_art == AA_PREFER_IMAGE_FILE)
    {
        if (find_albumart(id3, path, sizeof(path), &slot_dim[dim_slot]))
        {
            out->src = AA_SRC_FILE;
            strmemccpy(out->path, path, sizeof(out->path));
            return true;
        }
        checked_image_file = true;
    }

    if (id3->has_embedded_albumart &&
        (id3->albumart.type & AA_CLEAR_FLAGS_MASK) == AA_TYPE_JPG)
    {
        out->src = AA_SRC_EMBEDDED;
        out->emb_pos = id3->albumart.pos;
        out->emb_size = id3->albumart.size;
        strmemccpy(out->path, id3->path, sizeof(out->path));
        return true;
    }

    if (!checked_image_file &&
        find_albumart(id3, path, sizeof(path), &slot_dim[dim_slot]))
    {
        out->src = AA_SRC_FILE;
        strmemccpy(out->path, path, sizeof(out->path));
        return true;
    }

    return false;
}

static int find_block_by_ident(const struct aa_ident *id)
{
    int d = id->dim_slot;
    int b;

    for (b = 0; b < AA_WINDOW_SIZE; b++)
    {
        if (blocks[d][b].ident.src != AA_SRC_NONE &&
            ident_equal(&blocks[d][b].ident, id))
            return b;
    }
    return -1;
}

static bool block_in_window(int dim_slot, int b, int win[][AA_MAX_DIM_SLOTS])
{
    int i;
    for (i = 0; i < AA_WINDOW_SIZE; i++)
    {
        if (win[i][dim_slot] == b)
            return true;
    }
    return false;
}

static int find_free_block(int dim_slot, int wi)
{
    int b;

    for (b = 0; b < AA_WINDOW_SIZE; b++)
    {
        if (!block_in_window(dim_slot, b, window))
            return b;
    }

    /* Overwrite this offset's existing block if the window is full */
    if (wi >= 0 && wi < AA_WINDOW_SIZE && window[wi][dim_slot] >= 0)
        return window[wi][dim_slot];

    return -1;
}

static void recompute_in_use(void)
{
    int d, b, i;

    for (d = 0; d < nslots; d++)
    {
        for (b = 0; b < AA_WINDOW_SIZE; b++)
            blocks[d][b].in_use = false;

        for (i = 0; i < AA_WINDOW_SIZE; i++)
        {
            b = window[i][d];
            if (b >= 0)
            {
                blocks[d][b].in_use = true;
            }
        }
    }
}

static void windows_reset(int win[][AA_MAX_DIM_SLOTS])
{
    int i, d;
    for (i = 0; i < AA_WINDOW_SIZE; i++)
        for (d = 0; d < AA_MAX_DIM_SLOTS; d++)
            win[i][d] = -1;
}

static int decode_ident_to_scratch(const struct aa_ident *id)
{
    int d = id->dim_slot;
    size_t max_size = scratch_size;
    int fd;
    int rc;
    struct mp3_albumart embedded;
    struct mp3_albumart *emb = NULL;
    const char *path = id->path;

    if (!scratch || max_size == 0 || slot_block_size[d] == 0)
        return 0;

    if (id->src == AA_SRC_EMBEDDED)
    {
        embedded.pos = id->emb_pos;
        embedded.size = id->emb_size;
        embedded.type = AA_TYPE_JPG;
        emb = &embedded;
    }

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;

    rc = albumart_decode_fd(fd, path, &slot_dim[d], emb, scratch, max_size);
    close(fd);

    if (rc <= (int)sizeof(struct bitmap))
        return 0;

    if ((size_t)rc > slot_block_size[d])
        rc = slot_block_size[d];

    return rc;
}

static void install_scratch_into_block(const struct aa_ident *id, int b,
                                       int rc)
{
    int d = id->dim_slot;

    memcpy(blocks[d][b].bmp, scratch, rc);
    blocks[d][b].bmp->data =
        (unsigned char *)blocks[d][b].bmp + sizeof(struct bitmap);
    blocks[d][b].ident = *id;
    logf("AA cache load dim=%d blk=%d %s", d, b, id->path);
}

size_t albumart_cache_pool_size(const struct dim *dims, int n)
{
    size_t total = 0;
    size_t max_scratch = 0;
    int i;

    if (!dims || n <= 0)
        return 0;

    if (n > AA_MAX_DIM_SLOTS)
        n = AA_MAX_DIM_SLOTS;

    for (i = 0; i < n; i++)
    {
        size_t decoded;
        size_t scratch_need;

        if (dims[i].width <= 0 || dims[i].height <= 0)
            continue;

        decoded = albumart_decoded_size(&dims[i]);
        decoded = ALIGN_UP(decoded, sizeof(intptr_t));
        total += decoded * AA_WINDOW_SIZE;

        scratch_need = decoded + albumart_decode_overhead(dims[i].width);
        scratch_need = ALIGN_UP(scratch_need, sizeof(intptr_t));
        if (scratch_need > max_scratch)
            max_scratch = scratch_need;
    }

    if (total == 0)
        return 0;

    return total + max_scratch;
}

void albumart_cache_reset(void *buf, size_t size,
                          const struct dim *dims, int n)
{
    unsigned char *p;
    int d, b;
    static bool mutex_ready = false;

    if (!mutex_ready)
    {
        mutex_init(&aa_mutex);
        mutex_ready = true;
    }

    mutex_lock(&aa_mutex);

    scratch = NULL;
    scratch_size = 0;
    nslots = 0;
    memset(slot_dim, 0, sizeof(slot_dim));
    memset(slot_block_size, 0, sizeof(slot_block_size));
    memset(blocks, 0, sizeof(blocks));
    windows_reset(window);
    work_gen++;

    if (!buf || size == 0 || !dims || n <= 0)
        goto reset_done;

    if (n > AA_MAX_DIM_SLOTS)
        n = AA_MAX_DIM_SLOTS;

    nslots = n;
    p = buf;

    /* Scratch first so decode workspace is independent of slots */
    {
        size_t max_scratch = 0;
        for (d = 0; d < n; d++)
        {
            size_t need;
            if (dims[d].width <= 0 || dims[d].height <= 0)
                continue;
            need = albumart_decoded_size(&dims[d]) +
                   albumart_decode_overhead(dims[d].width);
            need = ALIGN_UP(need, sizeof(intptr_t));
            if (need > max_scratch)
                max_scratch = need;
        }

        if (max_scratch > size)
            goto reset_done;

        scratch = p;
        scratch_size = max_scratch;
        p += max_scratch;
        size -= max_scratch;
    }

    for (d = 0; d < n; d++)
    {
        size_t decoded;

        slot_dim[d] = dims[d];
        if (dims[d].width <= 0 || dims[d].height <= 0)
            continue;

        decoded = ALIGN_UP(albumart_decoded_size(&dims[d]), sizeof(intptr_t));
        if (decoded * AA_WINDOW_SIZE > size)
            break;

        slot_block_size[d] = decoded;
        for (b = 0; b < AA_WINDOW_SIZE; b++)
        {
            blocks[d][b].bmp = (struct bitmap *)p;
            ident_clear(&blocks[d][b].ident);
            blocks[d][b].in_use = false;
            p += decoded;
            size -= decoded;
        }
    }

reset_done:
    mutex_unlock(&aa_mutex);
}

void albumart_cache_clear(void)
{
    int d, b;

    mutex_lock(&aa_mutex);
    windows_reset(window);
    work_gen++;
    for (d = 0; d < AA_MAX_DIM_SLOTS; d++)
    {
        for (b = 0; b < AA_WINDOW_SIZE; b++)
        {
            ident_clear(&blocks[d][b].ident);
            blocks[d][b].in_use = false;
        }
    }
    mutex_unlock(&aa_mutex);
}

void albumart_cache_kick(void)
{
    mutex_lock(&aa_mutex);
    work_gen++;
    mutex_unlock(&aa_mutex);
}

bool albumart_cache_work(int pl_offset, const struct mp3entry *id3)
{
    int wi = win_index(pl_offset);
    int d;
    unsigned int gen;
    bool current;

    if (wi < 0 || wi >= AA_WINDOW_SIZE)
        return false;

    current = (pl_offset == 0);

    mutex_lock(&aa_mutex);
    gen = work_gen;
    mutex_unlock(&aa_mutex);

    for (d = 0; d < nslots; d++)
    {
        struct aa_ident id;
        int b;
        int rc;

        if (slot_block_size[d] == 0)
            continue;

        if (!id3 || !resolve_ident(id3, d, &id))
        {
            mutex_lock(&aa_mutex);
            if (gen != work_gen)
            {
                mutex_unlock(&aa_mutex);
                return false;
            }
            window[wi][d] = -1;
            recompute_in_use();
            mutex_unlock(&aa_mutex);
            continue;
        }

        mutex_lock(&aa_mutex);
        if (gen != work_gen)
        {
            mutex_unlock(&aa_mutex);
            return false;
        }

        b = find_block_by_ident(&id);
        if (b >= 0)
        {
            window[wi][d] = b;
            recompute_in_use();
            mutex_unlock(&aa_mutex);
            continue;
        }
        mutex_unlock(&aa_mutex);

        rc = decode_ident_to_scratch(&id);
        if (rc <= 0)
            continue;

        mutex_lock(&aa_mutex);
        if (gen != work_gen)
        {
            mutex_unlock(&aa_mutex);
            return false;
        }

        b = find_block_by_ident(&id);
        if (b < 0)
            b = find_free_block(d, wi);
        if (b < 0)
        {
            mutex_unlock(&aa_mutex);
            continue;
        }

        install_scratch_into_block(&id, b, rc);
        window[wi][d] = b;
        recompute_in_use();
        mutex_unlock(&aa_mutex);
    }

    mutex_lock(&aa_mutex);
    if (gen != work_gen)
        current = false;
    mutex_unlock(&aa_mutex);
    return current;
}

void albumart_cache_lock(void)
{
    mutex_lock(&aa_mutex);
}

void albumart_cache_unlock(void)
{
    mutex_unlock(&aa_mutex);
}

void albumart_cache_slide_locked(int delta)
{
    int d, i;
    int tmp[AA_WINDOW_SIZE];

    if (delta == 0)
        return;

    work_gen++;

    for (d = 0; d < nslots; d++)
    {
        for (i = 0; i < AA_WINDOW_SIZE; i++)
        {
            int src = i + delta;
            if (src >= 0 && src < AA_WINDOW_SIZE)
                tmp[i] = window[src][d];
            else
                tmp[i] = -1;
        }
        for (i = 0; i < AA_WINDOW_SIZE; i++)
            window[i][d] = tmp[i];
    }
    recompute_in_use();
}

struct bitmap *albumart_cache_get_locked(int pl_offset, int dim_slot)
{
    int wi = win_index(pl_offset);
    int b;

    if (wi < 0 || wi >= AA_WINDOW_SIZE ||
        (unsigned)dim_slot >= (unsigned)nslots)
        return NULL;

    b = window[wi][dim_slot];
    if (b >= 0 && blocks[dim_slot][b].in_use)
        return blocks[dim_slot][b].bmp;
    return NULL;
}

struct bitmap *albumart_cache_get(int pl_offset, int dim_slot)
{
    struct bitmap *bmp;
    mutex_lock(&aa_mutex);
    bmp = albumart_cache_get_locked(pl_offset, dim_slot);
    mutex_unlock(&aa_mutex);
    return bmp;
}

void albumart_cache_get_debugdata(struct albumart_cache_debug *dbg)
{
    int d, b, i;

    if (!dbg)
        return;

    memset(dbg, 0, sizeof(*dbg));

    mutex_lock(&aa_mutex);
    for (d = 0; d < nslots; d++)
    {
        if (slot_block_size[d] == 0)
            continue;

        dbg->slots_total += AA_WINDOW_SIZE;
        dbg->pool_size += slot_block_size[d] * AA_WINDOW_SIZE;

        for (b = 0; b < AA_WINDOW_SIZE; b++)
        {
            if (blocks[d][b].in_use)
            {
                dbg->slots_used++;
                dbg->used_size += slot_block_size[d];
            }
        }
    }

    for (i = 0; i < AA_WINDOW_SIZE; i++)
    {
        dbg->occupied[i] = 0;
        for (d = 0; d < nslots; d++)
        {
            b = window[i][d];
            if (b >= 0 && blocks[d][b].in_use)
            {
                dbg->occupied[i] = 1;
                break;
            }
        }
    }
    mutex_unlock(&aa_mutex);
}

#endif /* HAVE_ALBUMART */
