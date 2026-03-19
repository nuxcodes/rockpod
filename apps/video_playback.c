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
 * Full-screen video player UI with semi-transparent OSD overlay.
 * Replicates stock iPod video playback experience:
 *  - Video plays full-screen with no OSD on start
 *  - SELECT toggles OSD (title bar + transport bar)
 *  - Scroll wheel adjusts volume and shows OSD
 *  - OSD auto-hides after 4 seconds
 *
 * Currently displays a test frame; decode pipeline will be integrated later.
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

#ifdef IPOD_6G

#include "system.h"
#include "lcd.h"
#include "font.h"
#include "button.h"
#include "kernel.h"
#include "settings.h"
#include "sound.h"
#include "misc.h"
#include "screen_access.h"
#include "backlight.h"
#include "mp4_demux.h"
#include "video_playback.h"
#include "file.h"
#include "string-extra.h"
#include "splash.h"
#include "gui/viewport.h"

#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Constants                                                          */
/* ------------------------------------------------------------------ */

#define OSD_SHOW_TICKS   (HZ * 4)
#define OSD_PAD          6
#define PROGRESS_H       2
#define SEEK_STEP_MS     10000
#define SEEK_FAST_MS     30000
#define VOL_SHOW_TICKS   (HZ * 2)
#define VOL_BAR_W        180
#define VOL_BAR_H        6
#define VOL_BOX_PAD      10
#define VOL_ICON_W       14

/* Resume file */
#define RESUME_PATH      ROCKBOX_DIR "/video_resume.dat"
#define MAX_RESUME       64

/* ------------------------------------------------------------------ */
/* Types                                                              */
/* ------------------------------------------------------------------ */

enum playback_state {
    PB_STOPPED = 0,
    PB_PLAYING,
    PB_PAUSED
};

struct resume_entry {
    uint32_t path_hash;
    uint32_t resume_time;
    uint32_t duration;
};

/* ------------------------------------------------------------------ */
/* Static state                                                       */
/* ------------------------------------------------------------------ */

static struct {
    /* Video metadata */
    char title[128];
    uint16_t video_w, video_h;
    uint32_t duration_ms;

    /* Display rect (letterboxed within LCD) */
    int disp_x, disp_y, disp_w, disp_h;

    /* Playback */
    enum playback_state state;
    uint32_t curr_time_ms;
    long play_start_tick;
    uint32_t play_start_time;

    /* OSD */
    bool osd_visible;
    long osd_hide_tick;
    bool need_full_redraw;
    bool need_osd_redraw;

    /* OSD layout (computed from font metrics at init) */
    int osd_font_id;
    int title_bar_h;
    int transport_bar_h;
    int icon_size;
    int font_h;

    /* Theme */
    unsigned accent_color;

    /* Button state for tap vs hold detection */
    bool play_held;
    bool right_held;
    bool left_held;

    /* Volume overlay (centered bar, auto-hides) */
    long vol_show_until;
} ps;

/* Minimal demux buffers — just enough for metadata */
static uint32_t pb_sample_buf[1];
static uint32_t pb_chunk_buf[1];

/* ------------------------------------------------------------------ */
/* Utility: FNV-1a hash                                               */
/* ------------------------------------------------------------------ */

static uint32_t fnv1a_hash(const char *str)
{
    uint32_t h = 2166136261u;
    while (*str)
    {
        h ^= (uint8_t)*str++;
        h *= 16777619u;
    }
    return h;
}

/* format_time() from misc.h: formats milliseconds into M:SS or H:MM:SS.
 * adjust_volume() from misc.h: adjusts system volume by N steps. */

/* ------------------------------------------------------------------ */
/* Letterbox calculation                                              */
/* ------------------------------------------------------------------ */

static void calc_letterbox(uint16_t vid_w, uint16_t vid_h)
{
    int scale_w, scale_h, scale;

    if (vid_w == 0 || vid_h == 0)
    {
        ps.disp_x = 0;
        ps.disp_y = 0;
        ps.disp_w = LCD_WIDTH;
        ps.disp_h = LCD_HEIGHT;
        return;
    }

    scale_w = (LCD_WIDTH  * 1000) / vid_w;
    scale_h = (LCD_HEIGHT * 1000) / vid_h;
    scale = (scale_w < scale_h) ? scale_w : scale_h;

    ps.disp_w = (vid_w * scale) / 1000;
    ps.disp_h = (vid_h * scale) / 1000;
    if (ps.disp_w > LCD_WIDTH)  ps.disp_w = LCD_WIDTH;
    if (ps.disp_h > LCD_HEIGHT) ps.disp_h = LCD_HEIGHT;

    ps.disp_x = (LCD_WIDTH  - ps.disp_w) / 2;
    ps.disp_y = (LCD_HEIGHT - ps.disp_h) / 2;
}

/* ------------------------------------------------------------------ */
/* Duration from stts table                                           */
/* ------------------------------------------------------------------ */

static uint32_t calc_duration_ms(const struct mp4v_demux_res *d)
{
    uint64_t ticks = 0;
    uint32_t i;

    for (i = 0; i < d->num_stts; i++)
        ticks += (uint64_t)d->stts[i].sample_count * d->stts[i].sample_delta;

    if (d->timescale > 0)
        return (uint32_t)(ticks * 1000 / d->timescale);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Test frame: SMPTE color bars                                       */
/* ------------------------------------------------------------------ */

static void draw_test_frame(void)
{
    static const unsigned short bars[] = {
        0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F
    };
    int bar_w = ps.disp_w / 7;
    int i;

    for (i = 0; i < 7; i++)
    {
        int x = ps.disp_x + i * bar_w;
        int w = (i == 6) ? (ps.disp_w - 6 * bar_w) : bar_w;
        lcd_set_foreground(bars[i]);
        lcd_fillrect(x, ps.disp_y, w, ps.disp_h);
    }
}

/* ------------------------------------------------------------------ */
/* OSD: play icon (right-pointing triangle)                           */
/* ------------------------------------------------------------------ */

static void draw_play_icon(int cx, int cy, int sz)
{
    int half = sz / 2;
    int max_w = sz * 2 / 3;
    int i;

    if (max_w < 2) max_w = 2;
    lcd_set_foreground(LCD_WHITE);
    for (i = 0; i < sz; i++)
    {
        int dist = (i < half) ? i : (sz - 1 - i);
        int w = (dist + 1) * max_w / half;
        if (w < 1) w = 1;
        lcd_fillrect(cx, cy + i, w, 1);
    }
}

/* ------------------------------------------------------------------ */
/* OSD: pause icon (two vertical bars)                                */
/* ------------------------------------------------------------------ */

static void draw_pause_icon(int cx, int cy, int sz)
{
    int bar_w = sz / 4;
    int gap = bar_w;
    int total_w, x_off;
    int bar_h = sz - 2;

    if (bar_w < 2) bar_w = 2;
    if (gap < 2) gap = 2;
    total_w = 2 * bar_w + gap;
    x_off = (sz - total_w) / 2;

    lcd_set_foreground(LCD_WHITE);
    lcd_fillrect(cx + x_off, cy + 1, bar_w, bar_h);
    lcd_fillrect(cx + x_off + bar_w + gap, cy + 1, bar_w, bar_h);
}

/* ------------------------------------------------------------------ */
/* OSD: progress bar with knob                                        */
/* ------------------------------------------------------------------ */

static void draw_progress_bar(int x, int y, int w,
                               uint32_t elapsed, uint32_t total)
{
    int fill_w, knob_x, knob_y;
    int knob_sz = ps.icon_size - 2;

    if (knob_sz < 5) knob_sz = 5;

    /* Track background */
    lcd_set_foreground(LCD_DARKGRAY);
    lcd_fillrect(x, y, w, PROGRESS_H);

    /* Filled portion */
    fill_w = (total > 0) ? (int)((uint64_t)elapsed * w / total) : 0;
    if (fill_w > w) fill_w = w;

    lcd_set_foreground(ps.accent_color);
    lcd_fillrect(x, y, fill_w, PROGRESS_H);

    /* Knob — centered on fill position */
    knob_x = x + fill_w - knob_sz / 2;
    if (knob_x < x) knob_x = x;
    if (knob_x + knob_sz > x + w) knob_x = x + w - knob_sz;
    knob_y = y + PROGRESS_H / 2 - knob_sz / 2;
    lcd_fillrect(knob_x, knob_y, knob_sz, knob_sz);
}

/* ------------------------------------------------------------------ */
/* Volume overlay (centered bar, independent of OSD)                  */
/* ------------------------------------------------------------------ */

static void draw_speaker_icon(int x, int y, int h)
{
    /* Speaker shape: rectangle body + expanding cone (megaphone) */
    int body_w = 3;
    int body_h = h * 2 / 5;
    int cone_w = h * 3 / 7;
    int half_h = h / 2;
    int half_bh = body_h / 2;
    int body_top = half_h - half_bh;
    int right = x + body_w + cone_w;
    int i;

    if (body_h < 3) body_h = 3;
    if (cone_w < 3) cone_w = 3;
    if (half_h <= half_bh) half_bh = half_h - 1;

    lcd_set_foreground(LCD_WHITE);

    /* Body rectangle */
    lcd_fillrect(x, y + body_top, body_w, body_h);

    /* Cone: wide at body rows, narrows toward top/bottom */
    for (i = 0; i < h; i++)
    {
        int dist = (i < half_h) ? (half_h - i) : (i - half_h);
        int left;

        if (dist <= half_bh)
            left = x + body_w;
        else
            left = x + body_w +
                   cone_w * (dist - half_bh) / (half_h - half_bh);

        if (right > left)
            lcd_fillrect(left, y + i, right - left, 1);
    }
}

static void draw_volume_overlay(void)
{
    int vol = global_status.volume;
    int vol_min = sound_min(SOUND_VOLUME);
    int vol_max = sound_max(SOUND_VOLUME);
    int fill_w;
    int icon_space = VOL_ICON_W + 6;
    int box_w = icon_space + VOL_BAR_W + 2 * VOL_BOX_PAD;
    int box_h = VOL_BAR_H + 2 * VOL_BOX_PAD;
    int box_x = (LCD_WIDTH - box_w) / 2;
    int box_y = (LCD_HEIGHT - box_h) / 2;
    int bar_x = box_x + VOL_BOX_PAD + icon_space;
    int bar_y = box_y + (box_h - VOL_BAR_H) / 2;

    fill_w = (vol_max > vol_min) ?
             (vol - vol_min) * VOL_BAR_W / (vol_max - vol_min) : 0;
    if (fill_w < 0) fill_w = 0;
    if (fill_w > VOL_BAR_W) fill_w = VOL_BAR_W;

    lcd_set_drawmode(DRMODE_SOLID);

    /* Solid black box */
    lcd_set_foreground(LCD_BLACK);
    lcd_set_background(LCD_BLACK);
    lcd_fillrect(box_x, box_y, box_w, box_h);

    /* Border */
    lcd_set_foreground(LCD_DARKGRAY);
    lcd_drawrect(box_x, box_y, box_w, box_h);

    /* Speaker icon */
    draw_speaker_icon(box_x + VOL_BOX_PAD,
                      box_y + (box_h - VOL_ICON_W) / 2,
                      VOL_ICON_W);

    /* Track background */
    lcd_set_foreground(LCD_DARKGRAY);
    lcd_fillrect(bar_x, bar_y, VOL_BAR_W, VOL_BAR_H);

    /* Filled portion */
    if (fill_w > 0)
    {
        lcd_set_foreground(LCD_WHITE);
        lcd_fillrect(bar_x, bar_y, fill_w, VOL_BAR_H);
    }
}

/* ------------------------------------------------------------------ */
/* OSD: title bar (top)                                               */
/* ------------------------------------------------------------------ */

static void draw_title_bar(void)
{
    int tw, th, ty;
    int max_tw = LCD_WIDTH - 2 * (OSD_PAD + 2);

    lcd_setfont(ps.osd_font_id);
    lcd_set_drawmode(DRMODE_SOLID);
    lcd_set_foreground(LCD_BLACK);
    lcd_set_background(LCD_BLACK);
    lcd_fillrect(0, 0, LCD_WIDTH, ps.title_bar_h);

    lcd_set_foreground(LCD_WHITE);
    lcd_getstringsize(ps.title, &tw, &th);

    ty = (ps.title_bar_h - 1 - th) / 2;
    if (ty < 0) ty = 0;

    if (tw > max_tw)
    {
        char trunc[128];
        int dotw, doth, len;

        strmemccpy(trunc, ps.title, sizeof(trunc));
        lcd_getstringsize("...", &dotw, &doth);
        len = strlen(trunc);
        while (len > 0)
        {
            trunc[--len] = '\0';
            lcd_getstringsize(trunc, &tw, &th);
            if (tw + dotw <= max_tw)
                break;
        }
        strlcat(trunc, "...", sizeof(trunc));
        lcd_putsxy(OSD_PAD + 2, ty, trunc);
    }
    else
    {
        lcd_putsxy(OSD_PAD + 2, ty, ps.title);
    }

    lcd_set_foreground(LCD_DARKGRAY);
    lcd_hline(0, LCD_WIDTH - 1, ps.title_bar_h - 1);
}

/* ------------------------------------------------------------------ */
/* OSD: transport bar (bottom)                                        */
/* ------------------------------------------------------------------ */

static void draw_transport_bar(void)
{
    char e_str[16], r_str[16];
    int ew, rw, th;
    int bar_top = LCD_HEIGHT - ps.transport_bar_h;
    int icon_x, icon_y, x_cur;
    int text_y, prog_x, prog_w, prog_y;
    int remain_x;
    uint32_t remain;

    lcd_setfont(ps.osd_font_id);
    lcd_set_drawmode(DRMODE_SOLID);
    lcd_set_foreground(LCD_BLACK);
    lcd_set_background(LCD_BLACK);
    lcd_fillrect(0, bar_top, LCD_WIDTH, ps.transport_bar_h);

    /* Separator line at top */
    lcd_set_foreground(LCD_DARKGRAY);
    lcd_hline(0, LCD_WIDTH - 1, bar_top);

    /* Status icon */
    icon_x = OSD_PAD;
    icon_y = bar_top + (ps.transport_bar_h - ps.icon_size) / 2;

    if (ps.state == PB_PLAYING)
        draw_play_icon(icon_x, icon_y, ps.icon_size);
    else
        draw_pause_icon(icon_x, icon_y, ps.icon_size);

    x_cur = icon_x + ps.icon_size + OSD_PAD;

    /* Elapsed time */
    lcd_set_foreground(LCD_WHITE);
    format_time(e_str, sizeof(e_str), (long)ps.curr_time_ms);
    lcd_getstringsize(e_str, &ew, &th);
    text_y = bar_top + (ps.transport_bar_h - th) / 2;
    lcd_putsxy(x_cur, text_y, e_str);
    x_cur += ew + 8;

    /* Remaining time (negative) at far right */
    remain = (ps.duration_ms > ps.curr_time_ms) ?
             (ps.duration_ms - ps.curr_time_ms) : 0;
    format_time(r_str, sizeof(r_str), -(long)remain);
    lcd_getstringsize(r_str, &rw, &th);

    remain_x = LCD_WIDTH - OSD_PAD - rw;
    lcd_putsxy(remain_x, text_y, r_str);

    /* Progress bar fills the center gap */
    prog_x = x_cur;
    prog_w = remain_x - 8 - prog_x;
    prog_y = bar_top + ps.transport_bar_h / 2;

    if (prog_w > 20)
        draw_progress_bar(prog_x, prog_y, prog_w,
                          ps.curr_time_ms, ps.duration_ms);
}

/* ------------------------------------------------------------------ */
/* OSD: show / hide / toggle                                          */
/* ------------------------------------------------------------------ */

static void osd_show(void)
{
    if (!ps.osd_visible)
    {
        ps.osd_visible = true;
        ps.need_osd_redraw = true;
    }
    ps.osd_hide_tick = current_tick + OSD_SHOW_TICKS;
}

static void osd_hide(void)
{
    if (ps.osd_visible)
    {
        ps.osd_visible = false;
        ps.need_full_redraw = true;
    }
}

static void osd_toggle(void)
{
    if (ps.osd_visible)
        osd_hide();
    else
        osd_show();
}

/* ------------------------------------------------------------------ */
/* OSD: full redraw                                                   */
/* ------------------------------------------------------------------ */

static void osd_draw(void)
{
    lcd_set_viewport(NULL);

    draw_title_bar();
    draw_transport_bar();

    if (ps.vol_show_until && TIME_BEFORE(current_tick, ps.vol_show_until))
        draw_volume_overlay();

    lcd_update();
}

/* ------------------------------------------------------------------ */
/* Full screen redraw                                                 */
/* ------------------------------------------------------------------ */

static void full_redraw(void)
{
    lcd_set_viewport(NULL);
    lcd_set_foreground(LCD_BLACK);
    lcd_set_background(LCD_BLACK);
    lcd_clear_display();

    draw_test_frame();

    if (ps.osd_visible)
    {
        draw_title_bar();
        draw_transport_bar();
    }

    if (ps.vol_show_until && TIME_BEFORE(current_tick, ps.vol_show_until))
        draw_volume_overlay();

    lcd_update();
}

/* ------------------------------------------------------------------ */
/* Volume control (wraps core adjust_volume)                          */
/* ------------------------------------------------------------------ */

static void do_adjust_volume(int steps)
{
    adjust_volume(steps);
    ps.vol_show_until = current_tick + VOL_SHOW_TICKS;
}

/* ------------------------------------------------------------------ */
/* Simulated time update                                              */
/* ------------------------------------------------------------------ */

static void update_sim_time(void)
{
    if (ps.state == PB_PLAYING)
    {
        long elapsed = current_tick - ps.play_start_tick;
        uint32_t ms = (uint32_t)((uint64_t)elapsed * 1000 / HZ);
        ps.curr_time_ms = ps.play_start_time + ms;

        if (ps.curr_time_ms >= ps.duration_ms)
        {
            ps.curr_time_ms = ps.duration_ms;
            ps.state = PB_STOPPED;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Play / Pause                                                       */
/* ------------------------------------------------------------------ */

static void play_pause(void)
{
    if (ps.state == PB_PLAYING)
    {
        update_sim_time();
        ps.state = PB_PAUSED;
    }
    else
    {
        if (ps.curr_time_ms >= ps.duration_ms)
            ps.curr_time_ms = 0;

        ps.play_start_tick = current_tick;
        ps.play_start_time = ps.curr_time_ms;
        ps.state = PB_PLAYING;
    }

    osd_show();
    ps.need_osd_redraw = true;
}

/* ------------------------------------------------------------------ */
/* Seeking                                                            */
/* ------------------------------------------------------------------ */

static void do_seek(int delta_ms)
{
    int32_t t;

    update_sim_time();

    t = (int32_t)ps.curr_time_ms + delta_ms;
    if (t < 0) t = 0;
    if ((uint32_t)t > ps.duration_ms) t = (int32_t)ps.duration_ms;

    ps.curr_time_ms = (uint32_t)t;

    if (ps.state == PB_PLAYING)
    {
        ps.play_start_tick = current_tick;
        ps.play_start_time = ps.curr_time_ms;
    }

    osd_show();
    ps.need_osd_redraw = true;
}

/* ------------------------------------------------------------------ */
/* Theme colors                                                       */
/* ------------------------------------------------------------------ */

static void load_theme_colors(void)
{
    unsigned c = global_settings.lss_color;
    unsigned r = (c >> 11) & 0x1F;
    unsigned g = (c >> 5)  & 0x3F;
    unsigned b = c & 0x1F;
    unsigned lum = r * 2 + g + b * 2;

    ps.accent_color = (lum >= 30) ? c : LCD_RGBPACK(0x40, 0x80, 0xFF);
}

/* ------------------------------------------------------------------ */
/* OSD layout: compute dimensions from FONT_UI metrics                */
/* ------------------------------------------------------------------ */

static void osd_layout_init(void)
{
    int tw, th;

    lcd_setfont(ps.osd_font_id);
    lcd_getstringsize("0:00:00", &tw, &th);

    ps.font_h = th;
    if (ps.font_h < 8) ps.font_h = 8;

    ps.title_bar_h = ps.font_h + 10;
    ps.transport_bar_h = ps.font_h + 16;

    ps.icon_size = ps.font_h;
    if (ps.icon_size > 16) ps.icon_size = 16;
    if (ps.icon_size < 8) ps.icon_size = 8;
}

/* ------------------------------------------------------------------ */
/* Resume: save                                                       */
/* ------------------------------------------------------------------ */

static void resume_save(const char *filepath)
{
    static struct resume_entry entries[MAX_RESUME];
    uint32_t hash = fnv1a_hash(filepath);
    int count = 0, found = -1, i, fd;

    fd = open(RESUME_PATH, O_RDONLY);
    if (fd >= 0)
    {
        ssize_t n = read(fd, entries, sizeof(entries));
        close(fd);
        count = n / (int)sizeof(struct resume_entry);
        if (count < 0) count = 0;
        if (count > MAX_RESUME) count = MAX_RESUME;
    }

    for (i = 0; i < count; i++)
    {
        if (entries[i].path_hash == hash)
        {
            found = i;
            break;
        }
    }

    if (found >= 0)
    {
        entries[found].resume_time = ps.curr_time_ms;
        entries[found].duration = ps.duration_ms;
    }
    else
    {
        if (count >= MAX_RESUME)
            count = MAX_RESUME - 1;
        entries[count].path_hash = hash;
        entries[count].resume_time = ps.curr_time_ms;
        entries[count].duration = ps.duration_ms;
        count++;
    }

    fd = open(RESUME_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0)
    {
        write(fd, entries, count * sizeof(struct resume_entry));
        close(fd);
    }
}

/* ------------------------------------------------------------------ */
/* Resume: load                                                       */
/* ------------------------------------------------------------------ */

static uint32_t resume_load(const char *filepath)
{
    static struct resume_entry entries[MAX_RESUME];
    uint32_t hash = fnv1a_hash(filepath);
    int fd, count, i;
    ssize_t n;

    fd = open(RESUME_PATH, O_RDONLY);
    if (fd < 0)
        return 0;

    n = read(fd, entries, sizeof(entries));
    close(fd);

    if (n <= 0)
        return 0;

    count = n / (int)sizeof(struct resume_entry);
    for (i = 0; i < count; i++)
    {
        if (entries[i].path_hash == hash)
            return entries[i].resume_time;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Resume dialog                                                      */
/* ------------------------------------------------------------------ */

static int resume_dialog(uint32_t resume_ms)
{
    /* Returns: 1 = resume, 0 = start over, -1 = cancel */
    char tbuf[16];
    int btn;

    format_time(tbuf, sizeof(tbuf), (long)resume_ms);

    splashf(0, "Resume at %s?\n\n"
               "SELECT = Resume\n"
               "PLAY = Start over\n"
               "MENU = Cancel", tbuf);

    while (1)
    {
        btn = button_get(true);

        switch (btn)
        {
            case BUTTON_SELECT | BUTTON_REL:
                return 1;
            case BUTTON_PLAY | BUTTON_REL:
                return 0;
            case BUTTON_MENU:
            case BUTTON_LEFT:
                return -1;
        }

        if (default_event_handler(btn) == SYS_USB_CONNECTED)
            return -1;
    }
}

/* ------------------------------------------------------------------ */
/* Main button loop                                                   */
/* ------------------------------------------------------------------ */

static void button_loop(const char *filepath)
{
    uint32_t last_sec = 0xFFFFFFFF;
    bool exit_loop = false;

    while (!exit_loop)
    {
        long btn = button_get_w_tmo(HZ / 10);

        /* Update simulated playback time */
        update_sim_time();

        /* Auto-hide OSD */
        if (ps.osd_visible && TIME_AFTER(current_tick, ps.osd_hide_tick))
            osd_hide();

        /* Auto-hide volume overlay */
        if (ps.vol_show_until && TIME_AFTER(current_tick, ps.vol_show_until))
        {
            ps.vol_show_until = 0;
            ps.need_full_redraw = true;
        }

        /* Check if displayed second changed */
        if (ps.osd_visible && ps.state == PB_PLAYING)
        {
            uint32_t sec = ps.curr_time_ms / 1000;
            if (sec != last_sec)
            {
                last_sec = sec;
                ps.need_osd_redraw = true;
            }
        }

        switch (btn)
        {
            case BUTTON_NONE:
                break;

            /* SELECT tap = toggle OSD */
            case BUTTON_SELECT | BUTTON_REL:
                osd_toggle();
                break;

            /* PLAY press = reset hold flag */
            case BUTTON_PLAY:
                ps.play_held = false;
                break;

            /* PLAY hold = stop & exit */
            case BUTTON_PLAY | BUTTON_REPEAT:
                ps.play_held = true;
                exit_loop = true;
                break;

            /* PLAY tap = play/pause (only if not held) */
            case BUTTON_PLAY | BUTTON_REL:
                if (!ps.play_held)
                    play_pause();
                break;

            /* Volume */
            case BUTTON_SCROLL_FWD:
            case BUTTON_SCROLL_FWD | BUTTON_REPEAT:
                do_adjust_volume(1);
                break;

            case BUTTON_SCROLL_BACK:
            case BUTTON_SCROLL_BACK | BUTTON_REPEAT:
                do_adjust_volume(-1);
                break;

            /* Seek: RIGHT */
            case BUTTON_RIGHT:
                ps.right_held = false;
                break;

            case BUTTON_RIGHT | BUTTON_REPEAT:
                ps.right_held = true;
                do_seek(SEEK_FAST_MS);
                break;

            case BUTTON_RIGHT | BUTTON_REL:
                if (!ps.right_held)
                    do_seek(SEEK_STEP_MS);
                break;

            /* Seek: LEFT */
            case BUTTON_LEFT:
                ps.left_held = false;
                break;

            case BUTTON_LEFT | BUTTON_REPEAT:
                ps.left_held = true;
                do_seek(-SEEK_FAST_MS);
                break;

            case BUTTON_LEFT | BUTTON_REL:
                if (!ps.left_held)
                    do_seek(-SEEK_STEP_MS);
                break;

            /* MENU = exit */
            case BUTTON_MENU:
                exit_loop = true;
                break;

            default:
                if (default_event_handler(btn) == SYS_USB_CONNECTED)
                    exit_loop = true;
                break;
        }

        /* Redraw */
        if (ps.need_full_redraw)
        {
            full_redraw();
            ps.need_full_redraw = false;
            ps.need_osd_redraw = false;
        }
        else if (ps.need_osd_redraw && ps.osd_visible)
        {
            osd_draw();
            ps.need_osd_redraw = false;
        }
        else if (ps.vol_show_until &&
                 TIME_BEFORE(current_tick, ps.vol_show_until))
        {
            /* Volume overlay without OSD — redraw frame + overlay */
            lcd_set_viewport(NULL);
            full_redraw();
            ps.need_full_redraw = false;
        }
    }

    /* Save resume position */
    update_sim_time();
    resume_save(filepath);
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void video_playback_start(const char *filepath, const char *title)
{
    static struct mp4v_demux_res demux;
    int ret;
    uint32_t resume_time;

    if (!filepath)
        return;

    memset(&ps, 0, sizeof(ps));

    /* Set title */
    if (title && title[0])
    {
        strmemccpy(ps.title, title, sizeof(ps.title));
    }
    else
    {
        const char *p = strrchr(filepath, '/');
        if (p) p++; else p = filepath;
        strmemccpy(ps.title, p, sizeof(ps.title));
        char *dot = strrchr(ps.title, '.');
        if (dot) *dot = '\0';
    }

    /* Parse MP4 for metadata */
    ret = mp4v_demux_open(filepath, &demux,
                          pb_sample_buf, 1, pb_chunk_buf, 1);
    if (ret < 0)
    {
        splashf(HZ * 2, "Cannot open:\n%s", filepath);
        return;
    }

    ps.video_w = demux.width;
    ps.video_h = demux.height;
    ps.duration_ms = calc_duration_ms(&demux);
    if (ps.duration_ms == 0)
        ps.duration_ms = 60000;

    calc_letterbox(ps.video_w, ps.video_h);
    load_theme_colors();

    /* Resolve the actual UI font ID before disabling the theme.
     * FONT_UI is a virtual ID (12) that lcd_setfont() doesn't resolve;
     * font_get() walks backward from slot 11 and may find an SBS icon
     * font instead of the user's text font. getuifont() returns the
     * real slot number from global_status.font_id[]. */
    ps.osd_font_id = screens[SCREEN_MAIN].getuifont();
    osd_layout_init();

    /* Check for resume position */
    resume_time = resume_load(filepath);
    if (resume_time > 0 && resume_time < ps.duration_ms)
    {
        int choice = resume_dialog(resume_time);
        if (choice < 0)
            return;
        if (choice == 1)
            ps.curr_time_ms = resume_time;
    }

    /* Take over the screen */
    viewportmanager_theme_enable(SCREEN_MAIN, false, NULL);

    lcd_set_viewport(NULL);
    lcd_set_foreground(LCD_BLACK);
    lcd_set_background(LCD_BLACK);
    lcd_clear_display();

    /* Start playing (simulated) */
    ps.state = PB_PLAYING;
    ps.play_start_tick = current_tick;
    ps.play_start_time = ps.curr_time_ms;
    ps.osd_visible = false;
    ps.need_full_redraw = true;

    backlight_on();

    button_loop(filepath);

    /* Restore UI */
    lcd_set_viewport(NULL);
    viewportmanager_theme_undo(SCREEN_MAIN, true);
}

#endif /* IPOD_6G */
