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
 * Full-screen video player with H.264 hardware decode via VPU-B.
 * Replicates stock iPod video playback experience:
 *  - Video plays full-screen with no OSD on start
 *  - SELECT toggles OSD (title bar + transport bar)
 *  - Scroll wheel adjusts volume and shows OSD
 *  - OSD auto-hides after 4 seconds
 *
 * Decode pipeline: MP4 demux -> VPU H.264 -> lcd_blit_yuv
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
#include "vpu_h264.h"
#include "core_alloc.h"
#include "audio.h"
#include "file.h"
#include "string-extra.h"
#include "splash.h"
#include "gui/viewport.h"

#include <string.h>
#include <stdio.h>

/* ARM assembly YUV420->RGB565 converter (writes 2 lines to contiguous outbuf).
 * Global symbol from firmware/target/arm/s5l8702/lcd-asm-s5l8702.S. */
extern void lcd_write_yuv420_lines(unsigned char const * const src[3],
                                   uint16_t *outbuf,
                                   int width,
                                   int stride);

/* Direct DMA buffer access for compositing (lcd-s5l8702.c) */
extern uint16_t *lcd_begin_frame(void);
extern void lcd_end_frame(void);

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
#define VOL_FADE_TICKS   (HZ / 2)
#define OSD_ANIM_STEPS   6

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

    /* Display rect (centered within LCD, with optional scaling) */
    int disp_x, disp_y, disp_w, disp_h;

    /* Downscaler state */
    bool need_scale;
    int dst_w, dst_h;
    uint8_t *scale_y, *scale_cb, *scale_cr;

    /* Playback */
    enum playback_state state;
    uint32_t curr_time_ms;
    long play_start_tick;
    uint32_t play_start_time;

    /* Decode state */
    struct vpu_h264 *decoder;
    int vid_fd;
    uint32_t cur_sample;
    uint32_t num_samples;
    uint32_t nalu_len_size;
    const struct mp4v_demux_res *demux;
    uint8_t *read_buf;
    int read_buf_size;

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

    /* OSD animation (non-blocking, advances per frame) */
    int osd_anim_step;    /* 0 = idle, 1..OSD_ANIM_STEPS = in progress */
    bool osd_anim_show;   /* true = fly-in, false = fly-out */

    /* Direct DMA buffer (non-NULL during lcd_begin_frame/lcd_end_frame) */
    uint16_t *dma_buf;
} ps;

/* Tiny buffers for initial metadata-only demux pass */
static uint32_t pb_tmp_sample[1];
static uint32_t pb_tmp_chunk[1];

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
/* Fast 2x2 box filter for exact 2:1 downscale                       */
/* ------------------------------------------------------------------ */

static void scale_plane_box2x2(const uint8_t *src, int src_stride,
                                uint8_t *dst, int dst_w, int dst_h)
{
    int r, c;
    for (r = 0; r < dst_h; r++)
    {
        const uint8_t *r0 = src + (2 * r) * src_stride;
        const uint8_t *r1 = r0 + src_stride;
        uint8_t *out = dst + r * dst_w;
        for (c = 0; c < dst_w; c++)
        {
            out[c] = (uint8_t)((r0[2*c] + r0[2*c+1]
                              + r1[2*c] + r1[2*c+1] + 2) >> 2);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Bilinear downscale for a single YUV plane (center-mapped)          */
/* ------------------------------------------------------------------ */

static void scale_plane_downscale(const uint8_t *src, int src_w, int src_h,
                                    int src_stride,
                                    uint8_t *dst, int dst_w, int dst_h)
{
    uint32_t x_step, y_step;
    int r, c;
    /* Precomputed X tables — avoid per-pixel multiply in inner loop */
    uint16_t xtab[LCD_WIDTH];   /* x0 | (x1 << 8) packed */
    uint8_t  xftab[LCD_WIDTH];  /* fractional weight */

    if (dst_w < 1 || dst_h < 1 || src_w < 1 || src_h < 1)
        return;
    if (dst_w > LCD_WIDTH) dst_w = LCD_WIDTH;

    x_step = ((uint32_t)src_w << 16) / (unsigned)dst_w;
    y_step = ((uint32_t)src_h << 16) / (unsigned)dst_h;

    /* Precompute source X positions + weights for the entire row */
    for (c = 0; c < dst_w; c++)
    {
        int32_t sx = (int32_t)((uint32_t)c * x_step + (x_step >> 1))
                     - (1 << 15);
        int x0, x1;
        if (sx < 0) sx = 0;
        x0 = (uint32_t)sx >> 16;
        x1 = x0 + 1;
        if (x1 >= src_w) x1 = src_w - 1;
        xtab[c] = (uint16_t)(x0 | (x1 << 8));
        xftab[c] = (uint8_t)(((uint32_t)sx >> 8) & 0xFF);
    }

    for (r = 0; r < dst_h; r++)
    {
        int32_t sy = (int32_t)((uint32_t)r * y_step + (y_step >> 1))
                     - (1 << 15);
        int y0, y1;
        uint32_t yf;
        const uint8_t *row0, *row1;
        uint8_t *out;

        if (sy < 0) sy = 0;
        y0 = (uint32_t)sy >> 16;
        y1 = y0 + 1;
        yf = ((uint32_t)sy >> 8) & 0xFF;
        if (y1 >= src_h) y1 = src_h - 1;

        row0 = src + y0 * src_stride;
        row1 = src + y1 * src_stride;
        out = dst + r * dst_w;

        for (c = 0; c < dst_w; c++)
        {
            uint32_t xf = xftab[c];
            int x0 = xtab[c] & 0xFF;
            int x1 = xtab[c] >> 8;
            uint32_t ab, de;

            ab = row0[x0] * (256 - xf) + row0[x1] * xf;
            de = row1[x0] * (256 - xf) + row1[x1] * xf;
            out[c] = (uint8_t)((ab * (256 - yf) + de * yf
                                + (1 << 15)) >> 16);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Scale and blit a decoded YUV420 frame                              */
/* ------------------------------------------------------------------ */

static void scale_and_blit(const uint8_t *y, const uint8_t *cb,
                            const uint8_t *cr, int w, int h)
{
    unsigned char *src[3];

    if (ps.need_scale)
    {
        int cdst_w = ps.dst_w / 2;
        int cdst_h = ps.dst_h / 2;
        if (cdst_w < 2) cdst_w = 2;
        if (cdst_h < 2) cdst_h = 2;

        /* Auto-detect exact 2:1 for fast box filter, else bilinear */
        if (w == ps.dst_w * 2 && h == ps.dst_h * 2)
        {
            scale_plane_box2x2(y, w, ps.scale_y, ps.dst_w, ps.dst_h);
            scale_plane_box2x2(cb, w / 2, ps.scale_cb, cdst_w, cdst_h);
            scale_plane_box2x2(cr, w / 2, ps.scale_cr, cdst_w, cdst_h);
        }
        else
        {
            scale_plane_downscale(y, w, h, w,
                                 ps.scale_y, ps.dst_w, ps.dst_h);
            scale_plane_downscale(cb, w / 2, h / 2, w / 2,
                                 ps.scale_cb, cdst_w, cdst_h);
            scale_plane_downscale(cr, w / 2, h / 2, w / 2,
                                 ps.scale_cr, cdst_w, cdst_h);
        }
        src[0] = (unsigned char *)ps.scale_y;
        src[1] = (unsigned char *)ps.scale_cb;
        src[2] = (unsigned char *)ps.scale_cr;
        lcd_blit_yuv(src, 0, 0, ps.dst_w,
                     ps.disp_x, ps.disp_y, ps.dst_w, ps.dst_h);
    }
    else
    {
        src[0] = (unsigned char *)y;
        src[1] = (unsigned char *)cb;
        src[2] = (unsigned char *)cr;
        lcd_blit_yuv(src, 0, 0, w,
                     ps.disp_x, ps.disp_y, ps.disp_w, ps.disp_h);
    }
}

/* ------------------------------------------------------------------ */
/* Blit YUV420 to arbitrary RGB565 buffer                             */
/* Used for compositing to either framebuffer or DMA staging buffer   */
/* ------------------------------------------------------------------ */

static void blit_yuv_to_buf(const uint8_t *y, const uint8_t *cb,
                             const uint8_t *cr, int stride,
                             uint16_t *buf, int buf_stride,
                             int x, int y_pos, int w, int h)
{
    unsigned char const *yuv_src[3];
    int pairs = h >> 1;

    w = (w + 1) & ~1;
    yuv_src[0] = y;
    yuv_src[1] = cb;
    yuv_src[2] = cr;

    if (x == 0 && w == buf_stride)
    {
        /* Fast path: video width == buffer stride, write directly */
        uint16_t *out = buf + y_pos * buf_stride;
        while (pairs-- > 0)
        {
            lcd_write_yuv420_lines(yuv_src, out, w, stride);
            yuv_src[0] += stride << 1;
            yuv_src[1] += stride >> 1;
            yuv_src[2] += stride >> 1;
            out += buf_stride << 1;
        }
    }
    else
    {
        /* Slow path: temp buffer for stride mismatch */
        uint16_t line_buf[LCD_WIDTH * 2];
        int row = y_pos;
        while (pairs-- > 0)
        {
            lcd_write_yuv420_lines(yuv_src, line_buf, w, stride);
            memcpy(buf + row * buf_stride + x, line_buf, w * 2);
            memcpy(buf + (row + 1) * buf_stride + x, line_buf + w, w * 2);
            yuv_src[0] += stride << 1;
            yuv_src[1] += stride >> 1;
            yuv_src[2] += stride >> 1;
            row += 2;
        }
    }
}

/* Scale + blit to a caller-provided buffer (dblbuf or framebuffer) */
static void scale_and_blit_to(const uint8_t *y, const uint8_t *cb,
                               const uint8_t *cr, int w, int h,
                               uint16_t *buf, int buf_stride)
{
    if (ps.need_scale)
    {
        int cdst_w = ps.dst_w / 2;
        int cdst_h = ps.dst_h / 2;
        if (cdst_w < 2) cdst_w = 2;
        if (cdst_h < 2) cdst_h = 2;

        if (w == ps.dst_w * 2 && h == ps.dst_h * 2)
        {
            scale_plane_box2x2(y, w, ps.scale_y, ps.dst_w, ps.dst_h);
            scale_plane_box2x2(cb, w / 2, ps.scale_cb, cdst_w, cdst_h);
            scale_plane_box2x2(cr, w / 2, ps.scale_cr, cdst_w, cdst_h);
        }
        else
        {
            scale_plane_downscale(y, w, h, w,
                                  ps.scale_y, ps.dst_w, ps.dst_h);
            scale_plane_downscale(cb, w / 2, h / 2, w / 2,
                                  ps.scale_cb, cdst_w, cdst_h);
            scale_plane_downscale(cr, w / 2, h / 2, w / 2,
                                  ps.scale_cr, cdst_w, cdst_h);
        }
        blit_yuv_to_buf(ps.scale_y, ps.scale_cb, ps.scale_cr,
                         ps.dst_w, buf, buf_stride,
                         ps.disp_x, ps.disp_y, ps.dst_w, ps.dst_h);
    }
    else
    {
        blit_yuv_to_buf(y, cb, cr, w, buf, buf_stride,
                         ps.disp_x, ps.disp_y, ps.disp_w, ps.disp_h);
    }
}

/* Scale + blit to framebuffer (paused/fallback path) */
static void scale_and_blit_fb(const uint8_t *y, const uint8_t *cb,
                               const uint8_t *cr, int w, int h)
{
    lcd_set_viewport(NULL);
    scale_and_blit_to(y, cb, cr, w, h,
                      (uint16_t *)FBADDR(0, 0), LCD_WIDTH);
}

/* ------------------------------------------------------------------ */
/* Clear letterbox bars (black areas outside video rect)              */
/* ------------------------------------------------------------------ */

static void clear_letterbox_bars(bool push)
{
    lcd_set_foreground(LCD_BLACK);
    if (ps.disp_y > 0)
    {
        int bot_y = ps.disp_y + ps.disp_h;
        lcd_fillrect(0, 0, LCD_WIDTH, ps.disp_y);
        if (push) lcd_update_rect(0, 0, LCD_WIDTH, ps.disp_y);
        lcd_fillrect(0, bot_y, LCD_WIDTH, LCD_HEIGHT - bot_y);
        if (push) lcd_update_rect(0, bot_y, LCD_WIDTH, LCD_HEIGHT - bot_y);
    }
    if (ps.disp_x > 0)
    {
        int right_x = ps.disp_x + ps.disp_w;
        lcd_fillrect(0, ps.disp_y, ps.disp_x, ps.disp_h);
        if (push) lcd_update_rect(0, ps.disp_y, ps.disp_x, ps.disp_h);
        lcd_fillrect(right_x, ps.disp_y,
                     LCD_WIDTH - right_x, ps.disp_h);
        if (push)
            lcd_update_rect(right_x, ps.disp_y,
                            LCD_WIDTH - right_x, ps.disp_h);
    }
}

/* ------------------------------------------------------------------ */
/* Decode: feed one MP4 sample to VPU                                 */
/* ------------------------------------------------------------------ */

/* Returns: 1 = frame decoded, 0 = no frame yet (SPS/PPS), -1 = error/EOF */
static int decode_one_frame(bool display)
{
    uint32_t offset, size;
    int pos, ret;

    if (ps.cur_sample >= ps.num_samples)
        return -1;

    if (mp4v_get_sample_offset(ps.demux, ps.cur_sample, &offset, &size) < 0)
        return -1;

    if ((int)size > ps.read_buf_size)
        size = (uint32_t)ps.read_buf_size;

    lseek(ps.vid_fd, offset, SEEK_SET);
    if (read(ps.vid_fd, ps.read_buf, size) != (ssize_t)size)
        return -1;

    ps.cur_sample++;

    /* Each MP4 sample contains 1+ length-prefixed NALUs */
    pos = 0;
    while (pos + (int)ps.nalu_len_size <= (int)size)
    {
        uint32_t nalu_len = 0;
        int i;
        for (i = 0; i < (int)ps.nalu_len_size; i++)
            nalu_len = (nalu_len << 8) | ps.read_buf[pos + i];
        pos += ps.nalu_len_size;

        if (nalu_len == 0 || pos + (int)nalu_len > (int)size)
            break;

        ret = vpu_h264_decode_nalu(ps.decoder, ps.read_buf + pos, nalu_len);
        pos += nalu_len;

        if (ret == 1)
        {
            if (display)
            {
                const uint8_t *y, *cb, *cr;
                int w, h;

                vpu_h264_get_frame(ps.decoder, &y, &cb, &cr, &w, &h);
                if (ps.dma_buf)
                    scale_and_blit_to(y, cb, cr, w, h,
                                      ps.dma_buf, LCD_WIDTH);
                else if (ps.osd_visible)
                    scale_and_blit_fb(y, cb, cr, w, h);
                else
                    scale_and_blit(y, cb, cr, w, h);
            }
            return 1;
        }
        else if (ret < 0)
        {
            return -1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Decode: re-blit last decoded frame (for OSD hide, pause, etc.)     */
/* ------------------------------------------------------------------ */

static void blit_last_frame(void)
{
    const uint8_t *y, *cb, *cr;
    int w, h;

    if (!ps.decoder) return;

    vpu_h264_get_frame(ps.decoder, &y, &cb, &cr, &w, &h);
    if (!y || w == 0 || h == 0) return;

    scale_and_blit(y, cb, cr, w, h);
}

/* Like blit_last_frame() but writes to framebuffer for compositing */
static void blit_last_frame_fb(void)
{
    const uint8_t *y, *cb, *cr;
    int w, h;

    if (!ps.decoder) return;

    vpu_h264_get_frame(ps.decoder, &y, &cb, &cr, &w, &h);
    if (!y || w == 0 || h == 0) return;

    scale_and_blit_fb(y, cb, cr, w, h);
}

/* ------------------------------------------------------------------ */
/* Decode: update current time from sample position                   */
/* ------------------------------------------------------------------ */

static void update_frame_time(void)
{
    uint64_t ticks = 0;
    uint32_t remaining = ps.cur_sample > 0 ? ps.cur_sample - 1 : 0;
    uint32_t i;

    for (i = 0; i < ps.demux->num_stts && remaining > 0; i++)
    {
        uint32_t count = ps.demux->stts[i].sample_count;
        if (remaining <= count)
        {
            ticks += (uint64_t)remaining * ps.demux->stts[i].sample_delta;
            break;
        }
        ticks += (uint64_t)count * ps.demux->stts[i].sample_delta;
        remaining -= count;
    }

    if (ps.demux->timescale > 0)
        ps.curr_time_ms = (uint32_t)(ticks * 1000 / ps.demux->timescale);
}

/* ------------------------------------------------------------------ */
/* Decode: seek to time (keyframe + decode forward)                   */
/* ------------------------------------------------------------------ */

static void seek_to_time(uint32_t target_ms)
{
    uint64_t target_ticks;
    uint64_t acc = 0;
    uint32_t sample = 0;
    uint32_t key = 0;
    uint32_t i;

    if (!ps.demux || ps.num_samples == 0) return;

    /* Convert target_ms to sample index via stts */
    target_ticks = (uint64_t)target_ms * ps.demux->timescale / 1000;

    for (i = 0; i < ps.demux->num_stts && sample < ps.num_samples; i++)
    {
        uint64_t run = (uint64_t)ps.demux->stts[i].sample_count
                       * ps.demux->stts[i].sample_delta;
        if (acc + run > target_ticks)
        {
            sample += (uint32_t)((target_ticks - acc)
                                  / ps.demux->stts[i].sample_delta);
            break;
        }
        acc += run;
        sample += ps.demux->stts[i].sample_count;
    }
    if (sample >= ps.num_samples)
        sample = ps.num_samples - 1;

    /* Walk backward to nearest keyframe */
    for (i = 0; i < ps.demux->num_stss; i++)
    {
        uint32_t s = ps.demux->stss[i] - 1; /* stss is 1-based */
        if (s <= sample)
            key = s;
        else
            break;
    }
    if (ps.demux->num_stss == 0)
        key = sample; /* no stss = all sync */

    /* Decode from keyframe to target — show every 8th frame for
     * visual fast-forward feedback during long seeks */
    ps.cur_sample = key;
    while (ps.cur_sample <= sample)
    {
        bool is_last = (ps.cur_sample >= sample);
        bool show = is_last || ((ps.cur_sample - key) % 8 == 0);
        int ret = decode_one_frame(show);
        if (ret < 0) break;
    }

    ps.curr_time_ms = target_ms;
    ps.play_start_tick = current_tick;
    ps.play_start_time = target_ms;
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
/* Color dimming for fade effects                                     */
/* ------------------------------------------------------------------ */

static unsigned dim_color(unsigned rgb565, int alpha)
{
    /* alpha: 0 = black, 15 = fully opaque (identity) */
    unsigned a;
    if (alpha <= 0) return 0;
    a = (unsigned)(alpha + 1);
    return (((rgb565 & 0xF81F) * a >> 4) & 0xF81F)
         | (((rgb565 & 0x07E0) * a >> 4) & 0x07E0);
}

/* ------------------------------------------------------------------ */
/* Volume overlay (lower third, independent of OSD)                   */
/* ------------------------------------------------------------------ */

static void draw_speaker_icon(int x, int y, int h, unsigned color)
{
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

    lcd_set_foreground(color);

    lcd_fillrect(x, y + body_top, body_w, body_h);

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
    int fill_w, alpha;
    long remaining;
    int icon_space = VOL_ICON_W + 6;
    int box_w = icon_space + VOL_BAR_W + 2 * VOL_BOX_PAD;
    int box_h = VOL_BAR_H + 2 * VOL_BOX_PAD;
    int box_x = (LCD_WIDTH - box_w) / 2;
    int box_y = LCD_HEIGHT * 5 / 8 - box_h / 2;
    int bar_x = box_x + VOL_BOX_PAD + icon_space;
    int bar_y = box_y + (box_h - VOL_BAR_H) / 2;

    fill_w = (vol_max > vol_min) ?
             (vol - vol_min) * VOL_BAR_W / (vol_max - vol_min) : 0;
    if (fill_w < 0) fill_w = 0;
    if (fill_w > VOL_BAR_W) fill_w = VOL_BAR_W;

    /* Fade-out during last 500ms */
    remaining = ps.vol_show_until - current_tick;
    alpha = 15;
    if (remaining < VOL_FADE_TICKS)
    {
        alpha = (int)(remaining * 15 / VOL_FADE_TICKS);
        if (alpha < 1) alpha = 1;
    }

    lcd_set_drawmode(DRMODE_SOLID);

    /* Black box background (stays black during fade) */
    lcd_set_foreground(LCD_BLACK);
    lcd_set_background(LCD_BLACK);
    lcd_fillrect(box_x, box_y, box_w, box_h);

    /* Border */
    lcd_set_foreground(dim_color(LCD_DARKGRAY, alpha));
    lcd_drawrect(box_x, box_y, box_w, box_h);

    /* Speaker icon */
    draw_speaker_icon(box_x + VOL_BOX_PAD,
                      box_y + (box_h - VOL_ICON_W) / 2,
                      VOL_ICON_W, dim_color(LCD_WHITE, alpha));

    /* Track background */
    lcd_set_foreground(dim_color(LCD_DARKGRAY, alpha));
    lcd_fillrect(bar_x, bar_y, VOL_BAR_W, VOL_BAR_H);

    /* Filled portion */
    if (fill_w > 0)
    {
        lcd_set_foreground(dim_color(LCD_WHITE, alpha));
        lcd_fillrect(bar_x, bar_y, fill_w, VOL_BAR_H);
    }
}

/* ------------------------------------------------------------------ */
/* OSD: title bar (top)                                               */
/* ------------------------------------------------------------------ */

static void draw_title_bar(int y_off)
{
    int tw, th, ty;
    int max_tw = LCD_WIDTH - 2 * (OSD_PAD + 2);

    lcd_setfont(ps.osd_font_id);
    lcd_set_drawmode(DRMODE_SOLID);
    lcd_set_foreground(LCD_BLACK);
    lcd_set_background(LCD_BLACK);
    lcd_fillrect(0, y_off, LCD_WIDTH, ps.title_bar_h);

    lcd_set_foreground(LCD_WHITE);
    lcd_getstringsize(ps.title, &tw, &th);

    ty = (ps.title_bar_h - 1 - th) / 2 + y_off;

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
    lcd_hline(0, LCD_WIDTH - 1, ps.title_bar_h - 1 + y_off);
}

/* ------------------------------------------------------------------ */
/* OSD: transport bar (bottom)                                        */
/* ------------------------------------------------------------------ */

static void draw_transport_bar(int y_off)
{
    char e_str[16], r_str[16];
    int ew, rw, th;
    int bar_top = y_off;
    int icon_x, icon_y, x_cur;
    int text_y, prog_x, prog_w, prog_y;
    int remain_x;
    uint32_t remain;

    lcd_setfont(ps.osd_font_id);
    lcd_set_drawmode(DRMODE_SOLID);
    lcd_set_foreground(LCD_BLACK);
    lcd_set_background(LCD_BLACK);
    lcd_fillrect(0, bar_top, LCD_WIDTH, ps.transport_bar_h);

    lcd_set_foreground(LCD_DARKGRAY);
    lcd_hline(0, LCD_WIDTH - 1, bar_top);

    icon_x = OSD_PAD;
    icon_y = bar_top + (ps.transport_bar_h - ps.icon_size) / 2;

    if (ps.state == PB_PLAYING)
        draw_play_icon(icon_x, icon_y, ps.icon_size);
    else
        draw_pause_icon(icon_x, icon_y, ps.icon_size);

    x_cur = icon_x + ps.icon_size + OSD_PAD;

    lcd_set_foreground(LCD_WHITE);
    format_time(e_str, sizeof(e_str), (long)ps.curr_time_ms);
    lcd_getstringsize(e_str, &ew, &th);
    text_y = bar_top + (ps.transport_bar_h - th) / 2;
    lcd_putsxy(x_cur, text_y, e_str);
    x_cur += ew + 8;

    remain = (ps.duration_ms > ps.curr_time_ms) ?
             (ps.duration_ms - ps.curr_time_ms) : 0;
    format_time(r_str, sizeof(r_str), -(long)remain);
    lcd_getstringsize(r_str, &rw, &th);

    remain_x = LCD_WIDTH - OSD_PAD - rw;
    lcd_putsxy(remain_x, text_y, r_str);

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
        ps.osd_anim_step = 1;
        ps.osd_anim_show = true;
        ps.osd_visible = true;
    }
    else if (ps.osd_anim_step > 0 && !ps.osd_anim_show)
    {
        /* Interrupt fly-out: mirror step for smooth reversal */
        ps.osd_anim_step = OSD_ANIM_STEPS + 1 - ps.osd_anim_step;
        ps.osd_anim_show = true;
    }
    ps.osd_hide_tick = current_tick + OSD_SHOW_TICKS;
    ps.need_osd_redraw = true;
}

static void osd_hide(void)
{
    if (ps.osd_visible)
    {
        if (ps.osd_anim_step > 0 && ps.osd_anim_show)
        {
            /* Interrupt fly-in: mirror step for smooth reversal */
            ps.osd_anim_step = OSD_ANIM_STEPS + 1 - ps.osd_anim_step;
        }
        else if (ps.osd_anim_step == 0)
        {
            /* Start new fly-out */
            ps.osd_anim_step = 1;
        }
        /* else: already flying out, do nothing */
        ps.osd_anim_show = false;
    }
}

static void osd_toggle(void)
{
    if (ps.osd_anim_step > 0)
    {
        /* Animation in progress: reverse it */
        if (ps.osd_anim_show)
            osd_hide();
        else
            osd_show();
    }
    else if (ps.osd_visible)
        osd_hide();
    else
        osd_show();
}

/* ------------------------------------------------------------------ */
/* OSD: draw overlay (uses lcd_update_rect, not lcd_update)           */
/* ------------------------------------------------------------------ */

static void osd_draw(void)
{
    int title_y = 0;
    int trans_y = LCD_HEIGHT - ps.transport_bar_h;
    int title_vis, trans_vis;

    /* Compute animated positions if animation is active */
    if (ps.osd_anim_step > 0)
    {
        int step = ps.osd_anim_show ? ps.osd_anim_step
                                    : (OSD_ANIM_STEPS - ps.osd_anim_step);
        int remain = OSD_ANIM_STEPS - step;
        int pct = 100 - (remain * remain * 100)
                        / (OSD_ANIM_STEPS * OSD_ANIM_STEPS);

        title_y = -ps.title_bar_h + (ps.title_bar_h * pct) / 100;
        trans_y = LCD_HEIGHT - (ps.transport_bar_h * pct) / 100;
    }

    title_vis = ps.title_bar_h + title_y;
    trans_vis = LCD_HEIGHT - trans_y;

    lcd_set_viewport(NULL);

    /* Draw OSD elements to main framebuffer */
    if (title_vis > 0)
        draw_title_bar(title_y);
    if (trans_vis > 0)
        draw_transport_bar(trans_y);
    if (ps.vol_show_until && TIME_BEFORE(current_tick, ps.vol_show_until))
        draw_volume_overlay();

    if (ps.dma_buf)
    {
        /* Fast path: video already in dma_buf from decode.
         * Copy ONLY bar/letterbox regions from framebuffer (~28KB). */
        int top_h = title_vis;
        int bot_start = trans_y;

        /* Extend to cover letterbox if needed */
        if (ps.disp_y > top_h)
            top_h = ps.disp_y;
        if (ps.disp_y + ps.disp_h < bot_start)
            bot_start = ps.disp_y + ps.disp_h;

        /* Clear letterbox areas in dblbuf (before bar copy) */
        if (ps.disp_y > 0)
        {
            memset(ps.dma_buf, 0,
                   ps.disp_y * LCD_WIDTH * sizeof(uint16_t));
            memset(ps.dma_buf + (ps.disp_y + ps.disp_h) * LCD_WIDTH, 0,
                   (LCD_HEIGHT - ps.disp_y - ps.disp_h)
                   * LCD_WIDTH * sizeof(uint16_t));
        }
        if (ps.disp_x > 0)
        {
            int right_x = ps.disp_x + ps.disp_w;
            int r;
            for (r = ps.disp_y; r < ps.disp_y + ps.disp_h; r++)
            {
                memset(ps.dma_buf + r * LCD_WIDTH, 0,
                       ps.disp_x * sizeof(uint16_t));
                memset(ps.dma_buf + r * LCD_WIDTH + right_x, 0,
                       (LCD_WIDTH - right_x) * sizeof(uint16_t));
            }
        }

        /* Copy top region (title bar + letterbox above video) */
        if (top_h > 0)
        {
            memcpy(ps.dma_buf, FBADDR(0, 0),
                   top_h * LCD_WIDTH * sizeof(uint16_t));
            /* Clear any gap between title bar bottom and video top */
            if (title_vis < top_h && title_vis > 0)
                memset(ps.dma_buf + title_vis * LCD_WIDTH, 0,
                       (top_h - title_vis) * LCD_WIDTH * sizeof(uint16_t));
        }

        /* Copy bottom region (transport bar + letterbox below video) */
        if (bot_start < LCD_HEIGHT)
            memcpy(ps.dma_buf + bot_start * LCD_WIDTH,
                   FBADDR(0, bot_start),
                   (LCD_HEIGHT - bot_start) * LCD_WIDTH * sizeof(uint16_t));

        /* Copy volume overlay region if active */
        if (ps.vol_show_until && TIME_BEFORE(current_tick, ps.vol_show_until))
        {
            int box_w = (VOL_ICON_W + 6) + VOL_BAR_W + 2 * VOL_BOX_PAD;
            int box_h = VOL_BAR_H + 2 * VOL_BOX_PAD;
            int box_x = (LCD_WIDTH - box_w) / 2;
            int box_y = LCD_HEIGHT * 5 / 8 - box_h / 2;
            int r;
            for (r = 0; r < box_h; r++)
                memcpy(ps.dma_buf + (box_y + r) * LCD_WIDTH + box_x,
                       FBADDR(box_x, box_y + r),
                       box_w * sizeof(uint16_t));
        }

        lcd_end_frame();
        ps.dma_buf = NULL;
    }
    else
    {
        /* Fallback: full framebuffer compositing (paused states) */
        clear_letterbox_bars(false);
        lcd_update();
    }

    /* Advance animation */
    if (ps.osd_anim_step > 0)
    {
        ps.osd_anim_step++;
        if (ps.osd_anim_step > OSD_ANIM_STEPS)
        {
            ps.osd_anim_step = 0;
            if (!ps.osd_anim_show)
            {
                /* Fly-out complete */
                ps.osd_visible = false;
                ps.need_full_redraw = true;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Full screen redraw (re-blit last decoded frame + OSD if visible)   */
/* ------------------------------------------------------------------ */

static void full_redraw(void)
{
    if (ps.osd_visible && ps.osd_anim_step == 0)
    {
        /* Composite path: video + letterbox + bars -> framebuffer -> lcd_update */
        blit_last_frame_fb();
        osd_draw();
    }
    else
    {
        /* Direct path: video straight to LCD */
        blit_last_frame();
        clear_letterbox_bars(true);
    }
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
/* Play / Pause                                                       */
/* ------------------------------------------------------------------ */

static void play_pause(void)
{
    if (ps.state == PB_PLAYING)
    {
        ps.state = PB_PAUSED;
        cpu_boost(false);
    }
    else
    {
        if (ps.curr_time_ms >= ps.duration_ms)
        {
            ps.curr_time_ms = 0;
            ps.cur_sample = 0;
        }

        ps.play_start_tick = current_tick;
        ps.play_start_time = ps.curr_time_ms;
        ps.state = PB_PLAYING;
        cpu_boost(true);
    }

    osd_show();
}

/* ------------------------------------------------------------------ */
/* Seeking                                                            */
/* ------------------------------------------------------------------ */

static void do_seek(int delta_ms)
{
    int32_t t;

    t = (int32_t)ps.curr_time_ms + delta_ms;
    if (t < 0) t = 0;
    if ((uint32_t)t > ps.duration_ms) t = (int32_t)ps.duration_ms;

    cpu_boost(true);
    seek_to_time((uint32_t)t);
    cpu_boost(false);
    osd_show();
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
        (void)write(fd, entries, count * sizeof(struct resume_entry));
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
/* Main button loop — frame-driven decode                             */
/* ------------------------------------------------------------------ */

static void button_loop(const char *filepath)
{
    bool exit_loop = false;
    int no_frame_count = 0;

    while (!exit_loop)
    {
        long btn;

        if (ps.state == PB_PLAYING)
        {
            /* Acquire DMA buffer before decode if OSD visible.
             * decode_one_frame will write YUV->RGB directly to dblbuf. */
            if (ps.osd_visible && !ps.dma_buf)
                ps.dma_buf = lcd_begin_frame();

            /* Decode and display one frame */
            int ret = decode_one_frame(true);

            if (ret < 0)
            {
                ps.state = PB_STOPPED;
                cpu_boost(false);
                ps.curr_time_ms = ps.duration_ms;
                osd_show();
            }
            else if (ret == 0)
            {
                /* SPS/PPS consumed, no frame yet — decode next immediately.
                 * Limit retries to prevent infinite loop on malformed files. */
                if (++no_frame_count > 16)
                {
                    no_frame_count = 0;
                    yield();
                }
                else
                {
                    continue;
                }
            }
            else
            {
                update_frame_time();
                no_frame_count = 0;
            }

            /* OSD overlay: video is in dma_buf (direct path) or needs
             * framebuffer fallback. osd_draw handles both via ps.dma_buf. */
            if (ps.osd_visible)
            {
                if (ret != 1 && ps.dma_buf)
                {
                    /* No frame decoded (EOF/SPS). Render last frame to dblbuf. */
                    const uint8_t *fy, *fcb, *fcr;
                    int fw, fh;
                    vpu_h264_get_frame(ps.decoder, &fy, &fcb, &fcr, &fw, &fh);
                    if (fy && fw > 0 && fh > 0)
                        scale_and_blit_to(fy, fcb, fcr, fw, fh,
                                          ps.dma_buf, LCD_WIDTH);
                }
                else if (ret != 1 && !ps.dma_buf)
                {
                    blit_last_frame_fb();
                }
                osd_draw();
                ps.need_osd_redraw = false;
            }
            else if (ps.dma_buf)
            {
                /* OSD was hidden between lcd_begin_frame and here.
                 * Release the DMA buffer without using it — video
                 * wasn't rendered to it (decode used lcd_blit_yuv). */
                lcd_end_frame();
                ps.dma_buf = NULL;
            }

            /* Accurate frame pacing using absolute tick targets.
             * This avoids cumulative rounding error from integer division. */
            {
                long target_tick = ps.play_start_tick +
                    (long)((uint64_t)(ps.curr_time_ms - ps.play_start_time)
                           * HZ / 1000);
                long wait = target_tick - current_tick;

                /* Frame skip: if >66ms behind, decode without display
                 * to catch up (max 3 frames). */
                if (wait < -(long)(HZ / 15))
                {
                    int skipped = 0;
                    while (skipped < 3 && wait < 0)
                    {
                        int sr = decode_one_frame(false);
                        if (sr <= 0) break;
                        update_frame_time();
                        skipped++;
                        target_tick = ps.play_start_tick +
                            (long)((uint64_t)(ps.curr_time_ms
                                              - ps.play_start_time)
                                   * HZ / 1000);
                        wait = target_tick - current_tick;
                    }
                }

                if (wait < 0) wait = 0;
                if (wait > HZ) wait = HZ;

                btn = button_get_w_tmo(wait > 0 ? wait : 0);
            }
        }
        else
        {
            /* Paused or stopped — ~180ms animation, 10Hz idle */
            btn = button_get_w_tmo(ps.osd_anim_step > 0 ? 3 : HZ / 10);
        }

        /* Auto-hide OSD (guard: don't re-trigger during fly-out) */
        if (ps.osd_visible && ps.osd_anim_step == 0
            && TIME_AFTER(current_tick, ps.osd_hide_tick))
            osd_hide();

        /* Auto-hide volume overlay */
        if (ps.vol_show_until && TIME_AFTER(current_tick, ps.vol_show_until))
        {
            ps.vol_show_until = 0;
            if (ps.osd_visible && ps.osd_anim_step == 0)
            {
                blit_last_frame_fb();
                osd_draw();
            }
            else
            {
                blit_last_frame();
                clear_letterbox_bars(true);
            }
        }

        /* Handle buttons */
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

        /* Redraw for state changes (OSD toggle while paused, etc.) */
        if (ps.osd_anim_step > 0 && ps.state != PB_PLAYING)
        {
            /* Composite: video + letterbox + bars -> framebuffer -> lcd_update */
            blit_last_frame_fb();
            osd_draw();
        }
        else if (ps.need_full_redraw)
        {
            full_redraw();
            ps.need_full_redraw = false;
            ps.need_osd_redraw = false;
        }
        else if (ps.need_osd_redraw && ps.osd_visible
                 && ps.osd_anim_step == 0)
        {
            blit_last_frame_fb();
            osd_draw();
            ps.need_osd_redraw = false;
        }
        else if (ps.vol_show_until &&
                 TIME_BEFORE(current_tick, ps.vol_show_until))
        {
            /* Volume overlay without OSD — draw independently */
            int box_w = (VOL_ICON_W + 6) + VOL_BAR_W + 2 * VOL_BOX_PAD;
            int box_h = VOL_BAR_H + 2 * VOL_BOX_PAD;
            int box_y = LCD_HEIGHT * 5 / 8 - box_h / 2;
            draw_volume_overlay();
            lcd_update_rect((LCD_WIDTH - box_w) / 2, box_y, box_w, box_h);
        }
    }

    /* Save resume position */
    resume_save(filepath);
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void video_playback_start(const char *filepath, const char *title)
{
    static struct mp4v_demux_res demux;
    int mem_handle = -1;
    bool theme_disabled = false;
    uint32_t resume_time;
    int max_w, max_h;
    size_t dec_size, alloc_size;
    uint8_t *mem, *p;
    uint32_t *sample_buf, *chunk_buf;
    uint8_t *dec_buf;

    if (!filepath)
        return;

    memset(&ps, 0, sizeof(ps));
    ps.vid_fd = -1;

    /* Set title */
    if (title && title[0])
    {
        strmemccpy(ps.title, title, sizeof(ps.title));
    }
    else
    {
        const char *sl = strrchr(filepath, '/');
        if (sl) sl++; else sl = filepath;
        strmemccpy(ps.title, sl, sizeof(ps.title));
        char *dot = strrchr(ps.title, '.');
        if (dot) *dot = '\0';
    }

    /* Step 1: Quick metadata parse to get dimensions */
    if (mp4v_demux_open(filepath, &demux,
                        pb_tmp_sample, 1, pb_tmp_chunk, 1) < 0)
    {
        splashf(HZ * 2, "Cannot open:\n%s", filepath);
        return;
    }

    /* Reject unsupported H.264 profiles */
    if (demux.avc_profile != 66)
    {
        const char *pname = demux.avc_profile == 77 ? "Main" :
                            demux.avc_profile == 100 ? "High" : "Unknown";
        splashf(HZ * 3, "Unsupported:\n%s Profile H.264\n\nNeed Baseline",
                pname);
        return;
    }

    max_w = (demux.width + 15) & ~15;
    max_h = (demux.height + 15) & ~15;

    /* Reject resolutions the VPU can't handle */
    if (max_w > 1280 || max_h > 720)
    {
        splashf(HZ * 3, "Unsupported:\n%dx%d too large\n\nMax 1280x720",
                demux.width, demux.height);
        return;
    }

    dec_size = vpu_h264_buf_size(max_w, max_h);

    /* Stop audio playback to free core memory for decode buffers */
    audio_hard_stop();

    {
        /* Always allocate scale buffer — PAR from tkhd may require scaling
         * even when coded dimensions fit in LCD (e.g., 640x480 coded but
         * 853x480 display for 16:9 content). 115KB is negligible. */
        size_t scale_size = (size_t)LCD_WIDTH * LCD_HEIGHT * 3 / 2 + 128;

        alloc_size = MP4V_MAX_SAMPLES * sizeof(uint32_t) + 32
                   + MP4V_MAX_STCO * sizeof(uint32_t) + 32
                   + dec_size + 4096
                   + 256 * 1024 + 32
                   + scale_size;
    }

    mem_handle = core_alloc(alloc_size);
    if (mem_handle < 0)
    {
        splashf(HZ * 2, "Not enough memory");
        return;
    }
    core_pin(mem_handle);
    mem = core_get_data(mem_handle);
    p = mem;

    /* Bump-allocate from core block */
    sample_buf = (uint32_t *)(uintptr_t)ALIGN_UP((uintptr_t)p, 32);
    p = (uint8_t *)(sample_buf + MP4V_MAX_SAMPLES);

    chunk_buf = (uint32_t *)(uintptr_t)ALIGN_UP((uintptr_t)p, 32);
    p = (uint8_t *)(chunk_buf + MP4V_MAX_STCO);

    dec_buf = (uint8_t *)(uintptr_t)ALIGN_UP((uintptr_t)p, 4096);
    p = dec_buf + dec_size;

    ps.read_buf_size = 256 * 1024;
    ps.read_buf = (uint8_t *)(uintptr_t)ALIGN_UP((uintptr_t)p, 32);
    p = ps.read_buf + ps.read_buf_size;

    /* Allocate scale buffer (always — PAR may require scaling) */
    {
        int sw = LCD_WIDTH;
        int sh = LCD_HEIGHT;
        ps.scale_y = (uint8_t *)(uintptr_t)ALIGN_UP((uintptr_t)p, 32);
        p = ps.scale_y + sw * sh;
        ps.scale_cb = (uint8_t *)(uintptr_t)ALIGN_UP((uintptr_t)p, 32);
        p = ps.scale_cb + (sw / 2) * (sh / 2);
        ps.scale_cr = (uint8_t *)(uintptr_t)ALIGN_UP((uintptr_t)p, 32);
        p = ps.scale_cr + (sw / 2) * (sh / 2);
    }

    /* Step 2: Re-parse MP4 with full sample tables */
    if (mp4v_demux_open(filepath, &demux,
                        sample_buf, MP4V_MAX_SAMPLES,
                        chunk_buf, MP4V_MAX_STCO) < 0)
    {
        splashf(HZ * 2, "MP4 parse failed");
        goto cleanup;
    }

    /* Open VPU decoder */
    ps.decoder = vpu_h264_open(dec_buf, dec_size, max_w, max_h);
    if (!ps.decoder)
    {
        splashf(HZ * 2, "Decoder init failed");
        goto cleanup;
    }

    /* Configure with avcC from MP4 */
    if (vpu_h264_configure(ps.decoder, demux.codecdata,
                            (int)demux.codecdata_len) < 0)
    {
        splashf(HZ * 2, "avcC config failed");
        goto cleanup;
    }

    /* Open file for sample reads */
    ps.vid_fd = open(filepath, O_RDONLY);
    if (ps.vid_fd < 0)
    {
        splashf(HZ * 2, "Cannot open file");
        goto cleanup;
    }

    /* Set up decode state */
    ps.demux = &demux;
    ps.num_samples = demux.num_samples;
    ps.nalu_len_size = demux.nalu_len_size;
    ps.duration_ms = calc_duration_ms(&demux);
    if (ps.duration_ms == 0) ps.duration_ms = 60000;

    /* Use SPS dimensions (from avcC) as authoritative video size.
     * MP4 container metadata may have wrong dimensions. */
    {
        const uint8_t *tmp_y;
        int sps_w = 0, sps_h = 0;
        vpu_h264_get_frame(ps.decoder, &tmp_y, NULL, NULL, &sps_w, &sps_h);
        if (sps_w > 0 && sps_h > 0)
        {
            ps.video_w = sps_w;
            ps.video_h = sps_h;
        }
        else
        {
            ps.video_w = demux.width;
            ps.video_h = demux.height;
        }
    }

    /* Compute display rect with aspect-preserving downscale.
     * Use display dimensions from tkhd (PAR-adjusted) for aspect ratio,
     * but coded dimensions (video_w/h) for VPU output stride. */
    {
        uint16_t ar_w = demux.display_width ? demux.display_width : ps.video_w;
        uint16_t ar_h = demux.display_height ? demux.display_height : ps.video_h;

        if (ar_w > LCD_WIDTH || ar_h > LCD_HEIGHT)
        {
            uint32_t sx = ((uint32_t)LCD_WIDTH << 16) / ar_w;
            uint32_t sy = ((uint32_t)LCD_HEIGHT << 16) / ar_h;
            uint32_t s = MIN(sx, sy);
            ps.dst_w = (int)(((uint32_t)ar_w * s) >> 16) & ~1;
            ps.dst_h = (int)(((uint32_t)ar_h * s) >> 16) & ~1;
            if (ps.dst_w < 4) ps.dst_w = 4;
            if (ps.dst_h < 4) ps.dst_h = 4;
            ps.need_scale = true;
        }
        else
        {
            ps.dst_w = ar_w & ~1;
            ps.dst_h = ar_h & ~1;
            ps.need_scale = (ar_w != demux.width || ar_h != demux.height);
        }
        ps.disp_w = ps.dst_w;
        ps.disp_h = ps.dst_h;
        ps.disp_x = (LCD_WIDTH - ps.disp_w) / 2;
        ps.disp_y = (LCD_HEIGHT - ps.disp_h) / 2;
    }

    load_theme_colors();

    /* Resolve font ID before disabling theme */
    ps.osd_font_id = screens[SCREEN_MAIN].getuifont();
    osd_layout_init();

    /* Check for resume position */
    resume_time = resume_load(filepath);
    if (resume_time > 0 && resume_time < ps.duration_ms)
    {
        int choice = resume_dialog(resume_time);
        if (choice < 0)
            goto cleanup;
        if (choice == 1)
            ps.curr_time_ms = resume_time;
    }

    /* Take over the screen */
    viewportmanager_theme_enable(SCREEN_MAIN, false, NULL);
    theme_disabled = true;

    lcd_set_viewport(NULL);
    lcd_set_foreground(LCD_BLACK);
    lcd_set_background(LCD_BLACK);
    lcd_clear_display();
    lcd_update();

    backlight_on();
    backlight_set_timeout(0); /* keep backlight on during playback */

    /* If resuming, seek to the saved position */
    if (ps.curr_time_ms > 0)
    {
        cpu_boost(true);
        seek_to_time(ps.curr_time_ms);
        cpu_boost(false);
    }

    /* Start playing */
    ps.state = PB_PLAYING;
    cpu_boost(true);
    ps.play_start_tick = current_tick;
    ps.play_start_time = ps.curr_time_ms;
    ps.osd_visible = false;
    ps.need_full_redraw = false;

    button_loop(filepath);

cleanup:
    cpu_boost(false);
    if (ps.decoder) { vpu_h264_close(ps.decoder); ps.decoder = NULL; }
    if (ps.vid_fd >= 0) { close(ps.vid_fd); ps.vid_fd = -1; }
    ps.demux = NULL;
    if (mem_handle >= 0) { core_unpin(mem_handle); core_free(mem_handle); }

    /* Restore backlight timeout and UI */
    backlight_set_timeout(global_settings.backlight_timeout);
    lcd_set_viewport(NULL);
    if (theme_disabled)
        viewportmanager_theme_undo(SCREEN_MAIN, true);
}

#endif /* IPOD_6G */
