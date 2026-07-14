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

/* Update compositor with new frame: changes YUV plane pointers
 * and retrigggers the compositor GO bit. Call after each decoded frame. */
void compositor_update(const uint8_t *y, const uint8_t *cb, const uint8_t *cr);

/* Stop compositor: disables passthrough, restores landscape MADCTL,
 * restores all LCD registers to pre-compositor state, and refreshes
 * the Rockbox UI via lcd_update(). */
void compositor_stop(void);

/* Returns true if compositor passthrough is currently active. */
bool compositor_is_active(void);

#endif /* COMPOSITOR_S5L8702_H */
