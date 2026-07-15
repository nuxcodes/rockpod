/***************************************************************************
 * S5L8702 VPP Compositor Display Driver — public API
 *
 * Copyright (C) 2025 Nux Li
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
#ifndef COMPOSITOR_S5L8702_H
#define COMPOSITOR_S5L8702_H

#include <stdbool.h>
#include <stdint.h>

/* Start compositor passthrough with HW scaling and letterboxing.
 * frame_w/frame_h: native VPU output dimensions.
 * disp_w/disp_h: aspect-corrected display dimensions (<= 320x240).
 * disp_x/disp_y: centering offset within LCD.
 * y/cb/cr: YUV420 plane data in DRAM. */
void compositor_start(int frame_w, int frame_h,
                      int disp_w, int disp_h, int disp_x, int disp_y,
                      const uint8_t *y, const uint8_t *cb, const uint8_t *cr);

/* Update compositor with new frame: changes YUV plane pointers,
 * strobes the commit register, and pushes to LCD. */
void compositor_update(const uint8_t *y, const uint8_t *cb, const uint8_t *cr);

/* OSD overlay layer API (layers 0-4, RGB565).
 * Layers render ON TOP of the video (Layer 5).
 * Z-order: Layer 5 (back) -> Layer 0 -> ... -> Layer 4 (front). */
void compositor_layer_setup(int layer, int x, int y, int w, int h,
                            const uint16_t *fb);
void compositor_layer_show(int layer);
void compositor_layer_hide(int layer);

/* Stop compositor: disables passthrough, restores all LCD registers
 * to pre-compositor state. Does NOT change Entry Mode — caller must
 * call compositor_restore_entry_mode() after writing new framebuffer
 * content to avoid BGR/RGB color flash. */
void compositor_stop(void);

/* Restore ILI9326 Entry Mode to Rockbox default (0x0230, BGR=0).
 * Call AFTER lcd_update() has pushed new framebuffer content. */
void compositor_restore_entry_mode(void);

/* Returns true if compositor passthrough is currently active. */
bool compositor_is_active(void);

#endif /* COMPOSITOR_S5L8702_H */
