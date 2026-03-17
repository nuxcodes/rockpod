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
 * Videos menu screen - replicates stock iPod "Videos" menu by parsing
 * the iTunesDB to show Movies, Music Videos, and TV Shows.
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
#include "action.h"
#include "list.h"
#include "splash.h"
#include "lang.h"
#include "icon.h"
#include "misc.h"
#include "root_menu.h"
#include "itunesdb.h"
#include "videos.h"

#include "file.h"
#include "string-extra.h"

#include <string.h>
#include <stdio.h>

#define MAX_VIDEO_ENTRIES 256

#define VIDEOS_CACHE_PATH ROCKBOX_DIR "/videos.cache"
#define ITUNESDB_PATH     "/iPod_Control/iTunes/iTunesDB"
#define CACHE_MAGIC       0x56494443  /* "VIDC" */
#define CACHE_VERSION     1

struct videos_cache_header {
    uint32_t magic;
    uint32_t version;
    uint32_t itunesdb_size;  /* iTunesDB file size as staleness check */
    uint32_t entry_count;
    uint32_t counts[VIDEO_TYPE_COUNT];
};

static struct video_library video_lib;
static struct video_entry video_entries[MAX_VIDEO_ENTRIES];
static bool video_lib_loaded = false;

static const char *category_names[VIDEO_TYPE_COUNT] = {
    "Movies",
    "Music Videos",
    "TV Shows"
};

/* Get iTunesDB file size (used as change indicator) */
static off_t get_itunesdb_size(void)
{
    int fd = open(ITUNESDB_PATH, O_RDONLY);
    off_t size;

    if (fd < 0)
        return -1;

    size = lseek(fd, 0, SEEK_END);
    close(fd);
    return size;
}

/* Try to load video library from disk cache.
 * Returns true if cache was valid and loaded. */
static bool load_cache(off_t itunesdb_size)
{
    struct videos_cache_header hdr;
    int fd;
    ssize_t n;
    int i;

    fd = open(VIDEOS_CACHE_PATH, O_RDONLY);
    if (fd < 0)
        return false;

    n = read(fd, &hdr, sizeof(hdr));
    if (n < (ssize_t)sizeof(hdr)
        || hdr.magic != CACHE_MAGIC
        || hdr.version != CACHE_VERSION
        || hdr.itunesdb_size != (uint32_t)itunesdb_size
        || hdr.entry_count == 0
        || hdr.entry_count > MAX_VIDEO_ENTRIES)
    {
        close(fd);
        return false;
    }

    n = read(fd, video_entries, hdr.entry_count * sizeof(struct video_entry));
    close(fd);

    if (n < (ssize_t)(hdr.entry_count * sizeof(struct video_entry)))
        return false;

    video_lib.entries = video_entries;
    video_lib.count = hdr.entry_count;
    video_lib.capacity = MAX_VIDEO_ENTRIES;
    for (i = 0; i < VIDEO_TYPE_COUNT; i++)
        video_lib.counts[i] = hdr.counts[i];

    return true;
}

/* Save video library to disk cache. */
static void save_cache(off_t itunesdb_size)
{
    struct videos_cache_header hdr;
    int fd;
    int i;

    fd = open(VIDEOS_CACHE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return;

    hdr.magic = CACHE_MAGIC;
    hdr.version = CACHE_VERSION;
    hdr.itunesdb_size = (uint32_t)itunesdb_size;
    hdr.entry_count = video_lib.count;
    for (i = 0; i < VIDEO_TYPE_COUNT; i++)
        hdr.counts[i] = video_lib.counts[i];

    write(fd, &hdr, sizeof(hdr));
    write(fd, video_entries, video_lib.count * sizeof(struct video_entry));
    close(fd);
}

/* Ensure the video library is loaded (from cache or by parsing iTunesDB).
 * Returns the number of video entries, or -1 on error. */
static int ensure_video_lib(void)
{
    off_t db_size;
    int ret;

    /* Already loaded in memory this session */
    if (video_lib_loaded && video_lib.count > 0)
        return video_lib.count;

    db_size = get_itunesdb_size();
    if (db_size <= 0)
        return -1;

    /* Try disk cache first */
    if (load_cache(db_size))
    {
        video_lib_loaded = true;
        return video_lib.count;
    }

    /* Cache miss — parse iTunesDB */
    splash(0, "Loading videos...");

    ret = itunesdb_load_videos(&video_lib, video_entries,
                               sizeof(video_entries));
    if (ret <= 0)
    {
        video_lib_loaded = false;
        return -1;
    }

    video_lib_loaded = true;

    /* Write cache for next time */
    save_cache(db_size);

    return ret;
}

/* --- Video list (innermost: title + duration, no artist) --- */

struct video_list_ctx {
    int *indices;
    int count;
};

static const char *video_list_get_name(int selected_item, void *data,
                                       char *buffer, size_t buffer_len)
{
    struct video_list_ctx *ctx = (struct video_list_ctx *)data;
    struct video_entry *entry;

    if (selected_item < 0 || selected_item >= ctx->count)
        return "";

    entry = &video_lib.entries[ctx->indices[selected_item]];

    strmemccpy(buffer, entry->title, buffer_len);
    return buffer;
}

static int show_artist_videos(const char *artist, int *indices, int count)
{
    struct gui_synclist list;
    struct video_list_ctx ctx;
    int action;

    ctx.indices = indices;
    ctx.count = count;

    gui_synclist_init(&list, video_list_get_name, &ctx, false, 1, NULL);
    gui_synclist_set_nb_items(&list, count);
    gui_synclist_set_title(&list, artist, Icon_NOICON);
    gui_synclist_draw(&list);

    for (;;)
    {
        list_do_action(CONTEXT_STD, HZ / 2, &list, &action);

        switch (action)
        {
            case ACTION_STD_OK:
            {
                int sel = gui_synclist_get_sel_pos(&list);
                if (sel >= 0 && sel < count)
                {
                    struct video_entry *entry =
                        &video_lib.entries[indices[sel]];
                    splashf(HZ * 3, "%s\n%s", entry->title, entry->filepath);
                    gui_synclist_draw(&list);
                }
                break;
            }

            case ACTION_STD_CANCEL:
                return GO_TO_PREVIOUS;

            default:
                if (default_event_handler(action) == SYS_USB_CONNECTED)
                    return GO_TO_ROOT;
                break;
        }
    }
}

/* --- Artist list (middle layer: group by artist within a category) --- */

#define MAX_ARTISTS 64

struct artist_entry {
    const char *name;  /* points into video_entries[].artist */
    int first_index;   /* first video index for this artist */
    int count;         /* number of videos by this artist */
};

struct artist_list_ctx {
    struct artist_entry artists[MAX_ARTISTS];
    int count;
    /* All video indices for this category, grouped by artist */
    int indices[MAX_VIDEO_ENTRIES];
};

static const char *artist_list_get_name(int selected_item, void *data,
                                        char *buffer, size_t buffer_len)
{
    struct artist_list_ctx *ctx = (struct artist_list_ctx *)data;

    if (selected_item < 0 || selected_item >= ctx->count)
        return "";

    strmemccpy(buffer, ctx->artists[selected_item].name, buffer_len);
    return buffer;
}

/* Build artist groupings for a category and show the artist list */
static int show_category(enum video_type type)
{
    struct gui_synclist list;
    struct artist_list_ctx ctx;
    int action;
    int i, j;
    int vid_count;

    memset(&ctx, 0, sizeof(ctx));

    /* Collect all video indices for this category, grouped by artist */
    vid_count = 0;
    for (i = 0; i < video_lib.count && vid_count < MAX_VIDEO_ENTRIES; i++)
    {
        if (video_lib.entries[i].type == type)
            ctx.indices[vid_count++] = i;
    }

    if (vid_count == 0)
    {
        splash(HZ, "No videos");
        return GO_TO_PREVIOUS;
    }

    /* Build unique artist list */
    ctx.count = 0;
    for (i = 0; i < vid_count; i++)
    {
        struct video_entry *entry = &video_lib.entries[ctx.indices[i]];
        const char *name = entry->artist[0] ? entry->artist : "Unknown";
        bool found = false;

        for (j = 0; j < ctx.count; j++)
        {
            if (strcmp(ctx.artists[j].name, name) == 0)
            {
                ctx.artists[j].count++;
                found = true;
                break;
            }
        }

        if (!found && ctx.count < MAX_ARTISTS)
        {
            ctx.artists[ctx.count].name = name;
            ctx.artists[ctx.count].first_index = i;
            ctx.artists[ctx.count].count = 1;
            ctx.count++;
        }
    }

    gui_synclist_init(&list, artist_list_get_name, &ctx, false, 1, NULL);
    gui_synclist_set_nb_items(&list, ctx.count);
    gui_synclist_set_title(&list, category_names[type], Icon_NOICON);
    gui_synclist_draw(&list);

    for (;;)
    {
        list_do_action(CONTEXT_STD, HZ / 2, &list, &action);

        switch (action)
        {
            case ACTION_STD_OK:
            {
                int sel = gui_synclist_get_sel_pos(&list);
                if (sel >= 0 && sel < ctx.count)
                {
                    const char *artist = ctx.artists[sel].name;
                    int art_indices[MAX_VIDEO_ENTRIES];
                    int art_count = 0;

                    /* Collect videos for this artist */
                    for (i = 0; i < vid_count; i++)
                    {
                        struct video_entry *e =
                            &video_lib.entries[ctx.indices[i]];
                        const char *n = e->artist[0] ? e->artist : "Unknown";
                        if (strcmp(n, artist) == 0)
                            art_indices[art_count++] = ctx.indices[i];
                    }

                    int sub_ret = show_artist_videos(artist,
                                                     art_indices, art_count);
                    if (sub_ret == GO_TO_ROOT)
                        return GO_TO_ROOT;
                    gui_synclist_draw(&list);
                }
                break;
            }

            case ACTION_STD_CANCEL:
                return GO_TO_PREVIOUS;

            default:
                if (default_event_handler(action) == SYS_USB_CONNECTED)
                    return GO_TO_ROOT;
                break;
        }
    }
}

/* Category menu */
struct cat_menu_ctx {
    enum video_type types[VIDEO_TYPE_COUNT];
    int count;
};

static const char *cat_menu_get_name(int selected_item, void *data,
                                     char *buffer, size_t buffer_len)
{
    struct cat_menu_ctx *ctx = (struct cat_menu_ctx *)data;

    if (selected_item < 0 || selected_item >= ctx->count)
        return "";

    strmemccpy(buffer, category_names[ctx->types[selected_item]], buffer_len);
    return buffer;
}

int videos_screen(void)
{
    struct gui_synclist list;
    struct cat_menu_ctx cat_ctx;
    int action;
    int i;

    if (ensure_video_lib() <= 0)
    {
        splash(HZ * 2, "No videos found");
        return GO_TO_PREVIOUS;
    }

    /* Build category menu, hiding empty categories */
    cat_ctx.count = 0;
    for (i = 0; i < VIDEO_TYPE_COUNT; i++)
    {
        if (video_lib.counts[i] > 0)
            cat_ctx.types[cat_ctx.count++] = (enum video_type)i;
    }

    gui_synclist_init(&list, cat_menu_get_name, &cat_ctx, false, 1, NULL);
    gui_synclist_set_nb_items(&list, cat_ctx.count);
    gui_synclist_set_title(&list, "Videos", Icon_NOICON);
    gui_synclist_draw(&list);

    for (;;)
    {
        list_do_action(CONTEXT_STD, HZ / 2, &list, &action);

        switch (action)
        {
            case ACTION_STD_OK:
            {
                int sel = gui_synclist_get_sel_pos(&list);
                if (sel >= 0 && sel < cat_ctx.count)
                {
                    int sub_ret = show_category(cat_ctx.types[sel]);
                    if (sub_ret == GO_TO_ROOT)
                        return GO_TO_ROOT;
                    gui_synclist_draw(&list);
                }
                break;
            }

            case ACTION_STD_CANCEL:
                return GO_TO_PREVIOUS;

            default:
                if (default_event_handler(action) == SYS_USB_CONNECTED)
                    return GO_TO_ROOT;
                break;
        }
    }
}
