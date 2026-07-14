/***************************************************************************
 * S5L8702 VPP Compositor Display Driver — ILI9326 path
 *
 * Copyright (C) 2025 Nux Li
 *
 * Hardware-accelerated YCbCr→RGB display via the S5L8702 compositor.
 * Uses ILI9326 register commands (P18) for GRAM setup + P9 passthrough.
 * Panel is ILI9326 (no DCS decoder).
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
#include "cpu.h"
#include "lcd.h"
#include "s5l87xx.h"
#include "lcd-s5l8702.h"
#include "compositor-s5l8702.h"

#define COMP  0x38900000
#define CR(o) (*(volatile uint32_t*)(COMP+(o)))
#define LR(o) (*(volatile uint32_t*)(LCD_BASE+(o)))
#define PH(x) ((uint32_t)((uintptr_t)(x)&0x7FFFFFFF))

static bool comp_active;
static uint32_t saved_lcd_con;
static uint32_t saved_lcd_7c;
static uint32_t saved_lcd_88;
static uint32_t saved_lcd_20;
static uint32_t saved_lcd_74;
static uint32_t saved_lcd_78;

/* ILI9326 register write via P18 mode */
static void ili_cmd(uint16_t c) { while(LCD_STATUS&0x10); LCD_WCMD=c; }
static void ili_data(uint16_t d) { while(LCD_STATUS&0x10); LCD_WDATA=d; }

/* Push one frame: LCD+0x80 bracket with ILI9326 GRAM cursor reset.
 * P16 mode: PINMAP=0 for both P18 commands and P16 pixel data — no conflict. */
static void push_frame(void)
{
    { int t = 100000; while ((LR(0x8C) & 3) && --t > 0); }

    LR(0x80) = 1;

    while (!(LCD_STATUS & 0x2));
    LCD_CON = 0x80000DA9;
    ili_cmd(0x200); ili_data(0);
    ili_cmd(0x201); ili_data(0);
    ili_cmd(0x202);
    while (!(LCD_STATUS & 0x2));
    LCD_CON = 0x80100DB0;

    LR(0x80) = 0;
}

static void comp_hw_init(void)
{
    volatile uint32_t *c = (volatile uint32_t*)COMP;

    c[0x200/4] &= ~1;
    c[0x004/4] = 1;
    c[0x020/4] = 1;

    for (int i = 0; i < 256; i++) {
        c[0x400/4+i] = i*4;
        c[0x800/4+i] = i*4;
        c[0xC00/4+i] = i*4;
    }

    volatile uint32_t *s = (volatile uint32_t*)0x0890D2DC;
    uint32_t t[5];
    for (int i = 0; i < 5; i++) t[i] = s[i];
    if (t[0] > 0 && t[0] < 0x1000 && t[4] > 0 && t[4] < 0x1000)
        for (int i = 0; i < 5; i++) c[(0x1EC+i*4)/4] = t[i];
    else {
        uint32_t h[] = {0x0C, 0x26, 0x10, 0x82, 0x4E};
        for (int i = 0; i < 5; i++) c[(0x1EC+i*4)/4] = h[i];
    }

    c[0x0D8/4] = 0x1000; c[0x0DC/4] = 0;
    c[0x0E0/4] = 0x1000; c[0x0E4/4] = 0;
    c[0x0E8/4] = 0x1000; c[0x0EC/4] = 0;

    {
        uint32_t v = c[0x008/4];
        v &= ~0x20000000; v &= ~0x10000000;
        v &= ~0x03000000; v |= 0x01000000;
        v &= ~0x00300000; v |= 0x00100000;
        v &= ~0x00030000; v |= 0x00010000;
        v &= ~1; v |= 1;
        c[0x008/4] = v;
    }
    c[0x00C/4] = 0x000F0F0F;
    { uint32_t v = c[0x008/4]; v |= 0x8000;     c[0x008/4] = v; }
    { uint32_t v = c[0x008/4]; v &= ~2;         c[0x008/4] = v; }
    { uint32_t v = c[0x008/4]; v |= 0x100;      c[0x008/4] = v; }
    { uint32_t v = c[0x008/4]; v |= 0x80;       c[0x008/4] = v; }
    { uint32_t v = c[0x008/4]; v |= 0x40000000; c[0x008/4] = v; }

    c[0x200/4] |= 0x10080;
    c[0x204/4] = 2;
    c[0x208/4] = 0;
    c[0x20C/4] = 2;
    c[0x210/4] = 0x00010110;
    c[0x214/4] = 0x013F00EF;  /* landscape viewport */
    c[0x024/4] = 0x00FFFFFF;
}

void compositor_start(int frame_w, int frame_h,
                      const uint8_t *y, const uint8_t *cb, const uint8_t *cr)
{
    uint32_t h_scale, v_scale;

    if (comp_active)
        return;

    lcd_set_inhibit(true);

    { int t = 100000; while ((LR(0x8C) & 3) && --t > 0); }

    saved_lcd_con = LCD_CON;
    saved_lcd_7c = LR(0x7C);
    saved_lcd_88 = LR(0x88);
    saved_lcd_20 = LR(0x20);
    saved_lcd_74 = LR(0x74);
    saved_lcd_78 = LR(0x78);

    PWRCON(0) &= ~0x2080;
    { volatile int d = 0; while (d++ < 10000); }

    comp_hw_init();

    /* HW scaler: step = (src << 12) / dst. 0x1000 = unity, 0x2000 = 2:1 downscale */
    h_scale = ((uint32_t)frame_w << 12) / 320;
    v_scale = ((uint32_t)frame_h << 12) / 240;
    if (h_scale < 0x1000) h_scale = 0x1000;
    if (v_scale < 0x1000) v_scale = 0x1000;

    /* Layer 5 — YUV420 planes at native resolution */
    for (int o = 0x024; o <= 0x044; o += 4) CR(o) = 0;
    for (int o = 0x04C; o <= 0x058; o += 4) CR(o) = 0;
    CR(0x028) = 0x100;
    CR(0x02C) = frame_w | ((frame_w/2) << 16);
    CR(0x034) = frame_h | ((uint32_t)frame_w << 16);
    CR(0x04C) = (h_scale << 16) | v_scale;
    CR(0x054) = 0x014000F0;   /* P16 landscape: 320x240 */
    CR(0x038) = PH(y);
    CR(0x03C) = PH(cr);
    CR(0x040) = 0;
    CR(0x044) = PH(cb);
    CR(0x3AC) = 0;             /* rotation OFF for P16 */
    CR(0x0D4) = 1;
    { uint32_t v = CR(0x008); v &= ~0x100; CR(0x008) = v; }
    commit_discard_dcache();

    /* P16 passthrough — confirmed working */
    LCD_CON = 0x80100DB0;
    LR(0x88) = 0x01000000;
    LR(0x20) = 0x33;
    LR(0x7C) = 0x00000402;
    LR(0x78) = 0x000A000A;
    LR(0x74) = 0x014000F0;

    /* ILI9326 GRAM setup via P18 */
    while (!(LCD_STATUS & 0x2));
    LCD_CON = 0x80000DA9;
    ili_cmd(0x003); ili_data(0x1238);
    ili_cmd(0x210); ili_data(0);
    ili_cmd(0x211); ili_data(319);
    ili_cmd(0x212); ili_data(0);
    ili_cmd(0x213); ili_data(239);
    ili_cmd(0x200); ili_data(0);
    ili_cmd(0x201); ili_data(0);
    ili_cmd(0x202);
    while (!(LCD_STATUS & 0x2));
    LCD_CON = 0x80100DB0;

    /* Compositor GO=1 then passthrough enable */
    CR(0x000) = 1;
    LR(0x70) = 1;
    LR(0x80) = 0;

    /* Push initial frame */
    push_frame();

    comp_active = true;
}

void compositor_update(const uint8_t *y, const uint8_t *cb, const uint8_t *cr)
{
    if (!comp_active)
        return;

    CR(0x038) = PH(y);
    CR(0x03C) = PH(cr);
    CR(0x044) = PH(cb);
    commit_dcache();  /* clean only, no invalidate — compositor just reads */

    push_frame();
}

void compositor_stop(void)
{
    if (!comp_active)
        return;

    LR(0x70) = 0;
    LR(0x80) = 0;
    CR(0x000) = 0;

    /* Restore ILI9326 Entry Mode for Rockbox UI */
    while (!(LCD_STATUS & 0x2));
    LCD_CON = 0x80000DA9;
    ili_cmd(0x003); ili_data(0x0230);
    while (!(LCD_STATUS & 0x2));

    LCD_CON = saved_lcd_con;
    LR(0x88) = saved_lcd_88;
    LR(0x20) = saved_lcd_20;
    LR(0x7C) = saved_lcd_7c;
    LR(0x74) = saved_lcd_74;
    LR(0x78) = saved_lcd_78;

    { int t = 100000; while ((LR(0x8C) & 3) && --t > 0); }

    lcd_set_inhibit(false);
    lcd_update();

    comp_active = false;
}

bool compositor_is_active(void)
{
    return comp_active;
}

#endif /* IPOD_6G */
