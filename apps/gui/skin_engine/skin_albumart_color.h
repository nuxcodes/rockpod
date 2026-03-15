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

#ifndef SKIN_ALBUMART_COLOR_H
#define SKIN_ALBUMART_COLOR_H

#if defined(HAVE_ALBUMART) && defined(HAVE_LCD_COLOR)

/* Initialize dynamic colors: save theme defaults, register playback events */
void dynamic_colors_init(void);

/* Resolve a color: if it matches theme fg/bg and dynamic colors are active,
 * return the album-art-derived color (with fade interpolation).
 * Otherwise return the original color unchanged. */
unsigned int dynamic_colors_resolve(unsigned int original);

/* Returns true while a color fade is in progress (for fast refresh) */
bool dynamic_colors_fading(void);

/* Check if color extraction is needed and perform it (call from UI thread) */
void dynamic_colors_check_extraction(int aa_slot);

/* Re-save theme default colors (call after theme .cfg is applied) */
void dynamic_colors_save_theme(void);

/* Returns true once after a fade completes, to request a full screen redraw */
bool dynamic_colors_needs_full_update(void);

/* Returns true once after a fade completes, to clear full-screen bg gaps */
bool dynamic_colors_screen_clear_needed(void);

/* Returns true when color extraction is queued but not yet performed */
bool dynamic_colors_pending(void);

#else /* !HAVE_ALBUMART || !HAVE_LCD_COLOR */

#define dynamic_colors_init()                do {} while(0)
#define dynamic_colors_resolve(c)            (c)
#define dynamic_colors_fading()              false
#define dynamic_colors_check_extraction(s)   do {} while(0)
#define dynamic_colors_save_theme()          do {} while(0)
#define dynamic_colors_needs_full_update()   false
#define dynamic_colors_screen_clear_needed() false
#define dynamic_colors_pending()             false

#endif /* HAVE_ALBUMART && HAVE_LCD_COLOR */

#endif /* SKIN_ALBUMART_COLOR_H */
