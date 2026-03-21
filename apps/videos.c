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
#include "screen_access.h"
#include "itunesdb.h"
#include "artworkdb.h"
#include "video_thumb.h"
#include "video_playback.h"
#include "videos.h"

#include "file.h"
#include "string-extra.h"
#include "lcd.h"

#include <string.h>
#include <stdio.h>

#define MAX_VIDEO_ENTRIES 256
#define CORNER_RADIUS 8

/* Thumbnail storage: one THUMB_SIZE x THUMB_SIZE RGB565 bitmap per video */
#define MAX_THUMBS 64
static fb_data thumb_bitmaps[MAX_THUMBS][THUMB_SIZE * THUMB_SIZE];
static bool    thumb_valid[MAX_THUMBS];

/* Line height for thumbnail lists */
#define THUMB_LINE_HEIGHT THUMB_SIZE

/* Margin width: thumbnail + right padding before text */
#define THUMB_MARGIN_WIDTH (THUMB_SIZE + 8)

#define VIDEOS_CACHE_PATH ROCKBOX_DIR "/videos.cache"
#define ITUNESDB_PATH     "/iPod_Control/iTunes/iTunesDB"
#define CACHE_MAGIC       0x56494443  /* "VIDC" */
#define CACHE_VERSION     2

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

/* --- Thumbnail loading --- */

/* JPEG decoder needs working memory: struct jpeg (~50KB) + decode buf (38KB)
 * + output pixels. 128KB should be safe. */
#define THUMB_WORK_SIZE (128 * 1024)
static unsigned char thumb_work_buf[THUMB_WORK_SIZE] CACHEALIGN_ATTR;

/* Try to find a fallback JPEG thumbnail for a video.
 * Looks for <artist>.jpg in .rockbox/albumart/ */
static bool load_fallback_thumb(struct video_entry *entry, int thumb_idx)
{
    char path[MAX_PATH];

    if (entry->artist[0])
    {
        snprintf(path, sizeof(path),
                 "/.rockbox/albumart/%s.jpg", entry->artist);
        if (video_thumb_from_jpeg(path, thumb_bitmaps[thumb_idx],
                                  thumb_work_buf, THUMB_WORK_SIZE))
            return true;
    }
    return false;
}

static void load_thumbnails(int *indices, int count)
{
    int i;
    int limit = (count < MAX_THUMBS) ? count : MAX_THUMBS;

    memset(thumb_valid, 0, sizeof(thumb_valid));

    for (i = 0; i < limit; i++)
    {
        struct video_entry *entry = &video_lib.entries[indices[i]];
        bool ok = false;

        /* 1. Try ArtworkDB ithmb (iTunes-synced, pre-rendered RGB565) */
        if (entry->artwork_count >= 1 && entry->has_artwork == 0x01)
            ok = artworkdb_load_thumb(entry->dbid, entry->mhii_link,
                                      thumb_bitmaps[i],
                                      THUMB_SIZE, THUMB_SIZE,
                                      thumb_work_buf, THUMB_WORK_SIZE);

        /* 2. Try MP4 embedded covr atom → JPEG decode */
        if (!ok)
            ok = video_thumb_extract(entry->filepath,
                                     thumb_bitmaps[i],
                                     thumb_work_buf, THUMB_WORK_SIZE);

        /* 3. Try standalone artist JPEG */
        if (!ok)
            ok = load_fallback_thumb(entry, i);

        thumb_valid[i] = ok;
    }
}

static void open_video(struct video_entry *entry)
{
    video_playback_start(entry->filepath, entry->title);
}

/* --- Video list with thumbnails --- */

struct video_list_ctx {
    int *indices;
    int count;
};


static const char *video_list_get_name(int selected_item, void *data,
                                       char *buffer, size_t buffer_len)
{
    struct video_list_ctx *ctx = (struct video_list_ctx *)data;
    struct video_entry *entry;
    uint32_t dur, h, m, s;

    if (selected_item < 0 || selected_item >= ctx->count)
        return "";

    entry = &video_lib.entries[ctx->indices[selected_item]];

    dur = entry->duration_ms / 1000;
    h = dur / 3600;
    m = (dur % 3600) / 60;
    s = dur % 60;

    if (h > 0)
        snprintf(buffer, buffer_len, "%s  %lu:%02lu:%02lu",
                 entry->title, (unsigned long)h,
                 (unsigned long)m, (unsigned long)s);
    else
        snprintf(buffer, buffer_len, "%s  %lu:%02lu",
                 entry->title, (unsigned long)m, (unsigned long)s);

    return buffer;
}

static void video_margin_draw(int item_index, struct screen *display,
                              int x, int y, int width, int height,
                              bool is_selected, void *data)
{
    (void)is_selected;
    (void)data;
    (void)width;

    if (item_index >= 0 && item_index < MAX_THUMBS
        && thumb_valid[item_index])
    {
        int thumb_x = x;
        int thumb_y = y + (height - THUMB_SIZE) / 2;
        struct viewport *vp = lcd_current_viewport;
        int abs_x = vp->x + thumb_x;
        int abs_y = vp->y + thumb_y;
        int r = CORNER_RADIUS;
        int r2x4 = 4 * r * r;
        int px, py;
        fb_data bg;

        bg = *FBADDR(abs_x, abs_y);

        display->bitmap(thumb_bitmaps[item_index],
                        thumb_x, thumb_y, THUMB_SIZE, THUMB_SIZE);

        for (py = 0; py < r; py++)
            for (px = 0; px < r; px++)
            {
                int dx = 2 * r - 1 - 2 * px;
                int dy = 2 * r - 1 - 2 * py;
                if (dx * dx + dy * dy > r2x4)
                {
                    *FBADDR(abs_x + px, abs_y + py) = bg;
                    *FBADDR(abs_x + px,
                            abs_y + THUMB_SIZE - 1 - py) = bg;
                }
            }
    }
}

static int show_artist_videos(const char *artist, int *indices, int count)
{
    struct gui_synclist list;
    struct video_list_ctx ctx;
    int action;
    bool has_any_thumbs = false;
    int i;
    int ret = GO_TO_PREVIOUS;

    ctx.indices = indices;
    ctx.count = count;
    /* Load thumbnails for these videos */
    load_thumbnails(indices, count);
    for (i = 0; i < count && i < MAX_THUMBS; i++)
    {
        if (thumb_valid[i])
        {
            has_any_thumbs = true;
            break;
        }
    }


    gui_synclist_init(&list, video_list_get_name, &ctx, false, 1, NULL);
    gui_synclist_set_nb_items(&list, count);
    gui_synclist_set_title(&list, artist, Icon_NOICON);

    if (has_any_thumbs)
    {
        gui_synclist_set_margin_callback(&list, video_margin_draw,
                                         THUMB_MARGIN_WIDTH);
        list.line_height[SCREEN_MAIN] = THUMB_LINE_HEIGHT;
    }

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
                    open_video(entry);
                    gui_synclist_draw(&list);
                }
                break;
            }

            case ACTION_STD_CANCEL:
                ret = GO_TO_PREVIOUS;
                goto out;

            default:
                if (default_event_handler(action) == SYS_USB_CONNECTED)
                {
                    ret = GO_TO_ROOT;
                    goto out;
                }
                break;
        }
    }

out:
    lcd_scroll_stop();
    return ret;
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

    snprintf(buffer, buffer_len, "%s (%d)",
             ctx->artists[selected_item].name,
             ctx->artists[selected_item].count);
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

    /* Single artist — skip straight to video list */
    if (ctx.count == 1)
        return show_artist_videos(ctx.artists[0].name,
                                  ctx.indices, vid_count);

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
                    /* Redraw; list_draw clears its own viewport */
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
    enum video_type t;

    if (selected_item < 0 || selected_item >= ctx->count)
        return "";

    t = ctx->types[selected_item];
    snprintf(buffer, buffer_len, "%s (%d)",
             category_names[t], video_lib.counts[t]);
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
