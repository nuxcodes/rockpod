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

#define TITLE_BAR_H      28
#define TRANSPORT_BAR_H  36
#define OSD_SHOW_TICKS   (HZ * 4)
#define OSD_PAD          4
#define ICON_SIZE        12
#define PROGRESS_H       3
#define KNOB_W           7
#define KNOB_H           7
#define VOL_IND_W        28
#define SEEK_STEP_MS     10000
#define SEEK_FAST_MS     30000

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

    /* Theme */
    unsigned accent_color;

    /* Button state for tap vs hold detection */
    bool play_held;
    bool right_held;
    bool left_held;
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
/* Utility: darken framebuffer rect (50% brightness)                  */
/* ------------------------------------------------------------------ */

static void darken_rect(int x, int y, int w, int h)
{
    int row, col;

    /* Clamp to screen */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_WIDTH)  w = LCD_WIDTH - x;
    if (y + h > LCD_HEIGHT) h = LCD_HEIGHT - y;
    if (w <= 0 || h <= 0)
        return;

    for (row = y; row < y + h; row++)
    {
        fb_data *p = FBADDR(x, row);
        for (col = 0; col < w; col++)
            p[col] = (p[col] >> 1) & 0x7BEF;
    }
}

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
    int i;
    lcd_set_foreground(LCD_WHITE);
    for (i = 0; i < sz; i++)
    {
        int w = (i < sz / 2) ? (i + 1) : (sz - i);
        lcd_fillrect(cx + 1, cy + i, w, 1);
    }
}

/* ------------------------------------------------------------------ */
/* OSD: pause icon (two vertical bars)                                */
/* ------------------------------------------------------------------ */

static void draw_pause_icon(int cx, int cy, int sz)
{
    lcd_set_foreground(LCD_WHITE);
    lcd_fillrect(cx + 1, cy + 2, 3, sz - 4);
    lcd_fillrect(cx + sz - 4, cy + 2, 3, sz - 4);
}

/* ------------------------------------------------------------------ */
/* OSD: progress bar with knob                                        */
/* ------------------------------------------------------------------ */

static void draw_progress_bar(int x, int y, int w,
                               uint32_t elapsed, uint32_t total)
{
    int fill_w, knob_x;

    /* Track */
    lcd_set_foreground(LCD_DARKGRAY);
    lcd_fillrect(x, y, w, PROGRESS_H);

    /* Fill */
    fill_w = (total > 0) ? (int)((uint64_t)elapsed * w / total) : 0;
    if (fill_w > w) fill_w = w;

    lcd_set_foreground(ps.accent_color);
    lcd_fillrect(x, y, fill_w, PROGRESS_H);

    /* Knob */
    knob_x = x + fill_w - KNOB_W / 2;
    if (knob_x < x) knob_x = x;
    if (knob_x + KNOB_W > x + w) knob_x = x + w - KNOB_W;
    lcd_fillrect(knob_x, y - (KNOB_H - PROGRESS_H) / 2, KNOB_W, KNOB_H);
}

/* ------------------------------------------------------------------ */
/* OSD: volume indicator (4 bars of increasing height)                */
/* ------------------------------------------------------------------ */

static void draw_volume_indicator(int x, int y_base, int h)
{
    int vol = global_status.volume;
    int vol_min = sound_min(SOUND_VOLUME);
    int vol_max = sound_max(SOUND_VOLUME);
    int vol_pct, filled, i;

    if (vol_max > vol_min)
        vol_pct = (vol - vol_min) * 100 / (vol_max - vol_min);
    else
        vol_pct = 0;

    filled = (vol_pct + 12) / 25;
    if (filled > 4) filled = 4;

    for (i = 0; i < 4; i++)
    {
        int bar_h = 4 + i * 3;
        int bar_y = y_base + h - OSD_PAD - bar_h;

        lcd_set_foreground((i < filled) ? LCD_WHITE : LCD_DARKGRAY);
        lcd_fillrect(x + i * 6, bar_y, 4, bar_h);
    }
}

/* ------------------------------------------------------------------ */
/* OSD: title bar (top)                                               */
/* ------------------------------------------------------------------ */

static void draw_title_bar(void)
{
    int tw, th, ty;

    darken_rect(0, 0, LCD_WIDTH, TITLE_BAR_H);

    lcd_set_foreground(LCD_WHITE);
    lcd_setfont(FONT_UI);
    lcd_getstringsize(ps.title, &tw, &th);

    ty = (TITLE_BAR_H - th) / 2;
    if (ty < 0) ty = 0;
    lcd_putsxy(OSD_PAD * 2, ty, ps.title);

    lcd_setfont(FONT_SYSFIXED);
}

/* ------------------------------------------------------------------ */
/* OSD: transport bar (bottom)                                        */
/* ------------------------------------------------------------------ */

static void draw_transport_bar(void)
{
    char e_str[16], r_str[16];
    int ew, rw, th;
    int bar_top = LCD_HEIGHT - TRANSPORT_BAR_H;
    int icon_x, icon_y, x_cur;
    int text_y, prog_x, prog_w, prog_y;
    int vol_x, remain_x;
    uint32_t remain;

    darken_rect(0, bar_top, LCD_WIDTH, TRANSPORT_BAR_H);

    lcd_setfont(FONT_SYSFIXED);
    lcd_set_foreground(LCD_WHITE);

    /* Status icon */
    icon_x = OSD_PAD;
    icon_y = bar_top + (TRANSPORT_BAR_H - ICON_SIZE) / 2;

    if (ps.state == PB_PLAYING)
        draw_play_icon(icon_x, icon_y, ICON_SIZE);
    else
        draw_pause_icon(icon_x, icon_y, ICON_SIZE);

    x_cur = icon_x + ICON_SIZE + OSD_PAD;

    /* Elapsed time */
    format_time(e_str, sizeof(e_str), (long)ps.curr_time_ms);
    lcd_getstringsize(e_str, &ew, &th);
    text_y = bar_top + (TRANSPORT_BAR_H - th) / 2;
    lcd_putsxy(x_cur, text_y, e_str);
    x_cur += ew + 6;

    /* Remaining time (negative) */
    remain = (ps.duration_ms > ps.curr_time_ms) ?
             (ps.duration_ms - ps.curr_time_ms) : 0;
    format_time(r_str, sizeof(r_str), -(long)remain);
    lcd_getstringsize(r_str, &rw, &th);

    /* Volume is at far right */
    vol_x = LCD_WIDTH - OSD_PAD - VOL_IND_W;
    remain_x = vol_x - 8 - rw;
    lcd_putsxy(remain_x, text_y, r_str);

    /* Progress bar fills the gap */
    prog_x = x_cur;
    prog_w = remain_x - 6 - prog_x;
    prog_y = bar_top + (TRANSPORT_BAR_H - PROGRESS_H) / 2 - 1;

    if (prog_w > 20)
        draw_progress_bar(prog_x, prog_y, prog_w,
                          ps.curr_time_ms, ps.duration_ms);

    /* Volume indicator */
    draw_volume_indicator(vol_x, bar_top, TRANSPORT_BAR_H);
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

    /* Redraw test frame under OSD regions (fresh pixels for darkening) */
    draw_test_frame();

    draw_title_bar();
    draw_transport_bar();

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

    lcd_update();
}

/* ------------------------------------------------------------------ */
/* Volume control (wraps core adjust_volume)                          */
/* ------------------------------------------------------------------ */

static void do_adjust_volume(int steps)
{
    adjust_volume(steps);
    osd_show();
    ps.need_osd_redraw = true;
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
/* Resume: save                                                       */
/* ------------------------------------------------------------------ */

static void resume_save(const char *filepath)
{
    struct resume_entry entries[MAX_RESUME];
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
    struct resume_entry entries[MAX_RESUME];
    uint32_t hash = fnv1a_hash(filepath);
    int fd, count, i;

    fd = open(RESUME_PATH, O_RDONLY);
    if (fd < 0)
        return 0;

    count = read(fd, entries, sizeof(entries));
    close(fd);

    count /= sizeof(struct resume_entry);
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
        int btn = button_get_w_tmo(HZ / 10);

        /* Update simulated playback time */
        update_sim_time();

        /* Auto-hide OSD */
        if (ps.osd_visible && TIME_AFTER(current_tick, ps.osd_hide_tick))
            osd_hide();

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
    struct mp4v_demux_res demux;
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
