/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2011 by Jonathan Gordon
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
#include "system.h"
#include "lcd.h"
#include "font.h"
#include "button.h"
#include "string.h"
#include "settings.h"
#include "kernel.h"
#include "file.h"

#include "action.h"
#include "screen_access.h"
#include "list.h"
#include "scrollbar.h"
#include "lang.h"
#include "sound.h"
#include "misc.h"
#include "viewport.h"
#include "statusbar-skinned.h"
#include "skin_engine/skin_engine.h"
#include "skin_engine/skin_display.h"
#include "skin_engine/skin_albumart_color.h"
#include "appevents.h"

static struct listitem_viewport_cfg *listcfg[NB_SCREENS] = {NULL};
static struct gui_synclist *current_list;

static int current_row;
static int current_column;

void skinlist_set_cfg(enum screen_type screen,
                      struct listitem_viewport_cfg *cfg)
{
    if (listcfg[screen] != cfg)
    {
        if (listcfg[screen])
            screens[screen].scroll_stop_viewport(&listcfg[screen]->selected_item_vp.vp);
        listcfg[screen] = cfg;
        current_list = NULL;
        current_column = -1;
        current_row = -1;
    }
}

static bool skinlist_is_configured(enum screen_type screen,
                                    struct gui_synclist *list)
{
    if (listcfg[screen] == NULL)
        return false;
    if (list && list->selected_size != 1)
        return false;
    return true;
}
static int current_drawing_line;
static int offset_to_item(int offset, bool wrap)
{
    int item = current_drawing_line + offset;
    if (!current_list || current_list->nb_items == 0)
        return -1;
    if (item < 0)
    {
        if (!wrap)
            return -1;
        else
            item = (item + current_list->nb_items) % current_list->nb_items;
    }
    else if (item >= current_list->nb_items && !wrap)
        return -1;
    else
        item = item % current_list->nb_items;
    return item;
}

int skinlist_get_item_number()
{
    return current_drawing_line;
}

int skinlist_get_item_row()
{
    return current_row;
}

int skinlist_get_item_column()
{
    return current_column;
}


const char* skinlist_get_item_text(int offset, bool wrap, char* buf, size_t buf_size)
{
    int item = offset_to_item(offset, wrap);
    if (item < 0 || !current_list)
        return NULL;
    const char* ret = current_list->callback_get_item_name(
                    item, current_list->data, buf, buf_size);
    return P2STR((unsigned char*)ret);
}

enum themable_icons skinlist_get_item_icon(int offset, bool wrap)
{
    int item = offset_to_item(offset, wrap);
    if (item < 0 || !current_list || current_list->callback_get_item_icon == NULL)
        return Icon_NOICON;
    return current_list->callback_get_item_icon(item, current_list->data);
}

static bool is_selected = false;
bool skinlist_is_selected_item(void)
{
    return is_selected;
}

int skinlist_get_line_count(enum screen_type screen, struct gui_synclist *list)
{
    struct viewport *parent = (list->parent[screen]);
    if (!skinlist_is_configured(screen, list))
        return -1;
    if (listcfg[screen]->tile == true)
    {
        int rows = (parent->height / listcfg[screen]->height);
        int cols = (parent->width / listcfg[screen]->width);
        return rows*cols;
    }
    else
    {
        int h = listcfg[screen]->height;
        if (list && list->callback_draw_margin
            && list->line_height[screen] > h)
            h = list->line_height[screen];
        return (parent->height / h);
    }
}

static int current_item;
static int current_nbitems;
static bool needs_scrollbar[NB_SCREENS];
bool skinlist_needs_scrollbar(enum screen_type screen)
{
    return needs_scrollbar[screen];
}

void skinlist_get_scrollbar(int* nb_item, int* first_shown, int* last_shown)
{
    if (!skinlist_is_configured(0, NULL))
    {
        *nb_item = 0;
        *first_shown = 0;
        *last_shown = 0;
    }
    else
    {
        *nb_item = current_item;
        *first_shown = 0;
        *last_shown = current_nbitems;
    }
}

bool skinlist_get_item(struct screen *display, struct gui_synclist *list, int x, int y, int *item)
{
    const int screen = display->screen_type;
    if (!skinlist_is_configured(screen, list))
        return false;

    int h = listcfg[screen]->height;
    if (list && list->callback_draw_margin
        && list->line_height[screen] > h)
        h = list->line_height[screen];
    int row = y / h;
    int column = x / listcfg[screen]->width;
    struct viewport *parent = (list->parent[screen]);
    int cols = (parent->width / listcfg[screen]->width);
    *item = row * cols+ column;
    return true;
}

bool skinlist_draw(struct screen *display, struct gui_synclist *list)
{
    int cur_line, display_lines;
    const int screen = display->screen_type;
    struct viewport *parent = (list->parent[screen]);
    char* label = NULL;
    const int list_start_item = list->start_item[screen];
    struct gui_wps wps;
    if (!skinlist_is_configured(screen, list))
        return false;

    current_list = list;
#if defined(HAVE_ALBUMART) && defined(HAVE_LCD_COLOR)
    dynamic_colors_check_extraction(-1);
#endif
    wps.display = display;
    wps.data = listcfg[screen]->data;
    display_lines = skinlist_get_line_count(screen, list);
    label = (char *)SKINOFFSETTOPTR(get_skin_buffer(wps.data), listcfg[screen]->label);
    if (!label)
        return false;

    display->set_viewport(parent);
#if defined(HAVE_ALBUMART) && defined(HAVE_LCD_COLOR)
    unsigned int dc_saved_fg = parent->fg_pattern;
    unsigned int dc_saved_bg = parent->bg_pattern;
    parent->fg_pattern = dynamic_colors_resolve(dc_saved_fg);
    parent->bg_pattern = dynamic_colors_resolve(dc_saved_bg);
    display->set_foreground(parent->fg_pattern);
    display->set_background(parent->bg_pattern);
#endif
    display->clear_viewport();
    current_item = list->selected_item;
    current_nbitems = list->nb_items;
    needs_scrollbar[screen] = list->nb_items > display_lines;

    for (cur_line = 0; cur_line < display_lines; cur_line++)
    {
        struct skin_element* viewport;
        struct skin_viewport* skin_viewport = NULL;
        int margin_w = list->left_margin_width;
        bool has_margin = !listcfg[screen]->tile
            && margin_w > 0
            && list->callback_draw_margin != NULL;
        unsigned margin_fill_color = 0;
        bool margin_color_set = false;
        bool margin_painted = false;
        struct skin_viewport deco_svp_copy;
        struct skin_element *deco_vp_element = NULL;
        bool have_deco = false;
        int item_h = listcfg[screen]->height;
        if (has_margin && list->line_height[screen] > item_h)
            item_h = list->line_height[screen];

        if (list_start_item+cur_line+1 > list->nb_items)
            break;
        current_drawing_line = list_start_item+cur_line;
        is_selected = list_start_item+cur_line == list->selected_item;

        /* Inner loop: render each skin viewport for this item */
        for (viewport = SKINOFFSETTOPTR(get_skin_buffer(wps.data), listcfg[screen]->data->tree);
             viewport;
             viewport = SKINOFFSETTOPTR(get_skin_buffer(wps.data), viewport->next))
        {
            int original_x, original_y;
            int skin_orig_width, skin_orig_height;
            skin_viewport = SKINOFFSETTOPTR(get_skin_buffer(wps.data), viewport->data);
            char *viewport_label = NULL;
            if (skin_viewport)
                viewport_label = SKINOFFSETTOPTR(get_skin_buffer(wps.data), skin_viewport->label);
            if (viewport->children == 0 || !viewport_label ||
                (skin_viewport->label && strcmp(label, viewport_label))
                )
                continue;
            if (is_selected)
            {
                memcpy(&listcfg[screen]->selected_item_vp, skin_viewport, sizeof(struct skin_viewport));
                skin_viewport = &listcfg[screen]->selected_item_vp;
            }
            original_x = skin_viewport->vp.x;
            original_y = skin_viewport->vp.y;
            skin_orig_width = skin_viewport->vp.width;
            skin_orig_height = skin_viewport->vp.height;
            if (listcfg[screen]->tile)
            {
                int cols = (parent->width / listcfg[screen]->width);
                current_column = (cur_line)%cols;
                current_row = (cur_line)/cols;

                skin_viewport->vp.x = parent->x + listcfg[screen]->width*current_column + original_x;
                skin_viewport->vp.y = parent->y + listcfg[screen]->height*current_row + original_y;
            }
            else
            {
                current_column = 1;
                current_row = cur_line;
                skin_viewport->vp.x = parent->x + original_x;
                skin_viewport->vp.y = parent->y + original_y +
                                   (item_h*cur_line);
            }
            if (has_margin)
            {
                int shift = margin_w - original_x;
                if (shift > 0)
                {
                    skin_viewport->vp.x += shift;
                    skin_viewport->vp.width -= shift;
                }
                if (skin_viewport->vp.width < 0)
                    skin_viewport->vp.width = 0;
            }
            /* Expand all VPs to full item height when margin present */
            if (has_margin && item_h > listcfg[screen]->height)
            {
                skin_viewport->vp.height = item_h;
                skin_viewport->vp.y = parent->y + (item_h * cur_line);
            }
            display->set_viewport(&skin_viewport->vp);
#if defined(HAVE_ALBUMART) && defined(HAVE_LCD_COLOR)
            /* Dynamic colors: resolve from stored originals */
            skin_viewport->vp.fg_pattern =
                dynamic_colors_resolve(skin_viewport->dc_orig_fg);
            skin_viewport->vp.bg_pattern =
                dynamic_colors_resolve(skin_viewport->dc_orig_bg);
            display->set_foreground(skin_viewport->vp.fg_pattern);
            display->set_background(skin_viewport->vp.bg_pattern);
#endif
            if (has_margin && !margin_color_set)
            {
                margin_fill_color = is_selected
                    ? skin_viewport->vp.fg_pattern
                    : skin_viewport->vp.bg_pattern;
                margin_color_set = true;
            }

            /* Save decoration VP data for overlay after VP loop */
            if (has_margin && original_x == 0)
            {
                memcpy(&deco_svp_copy, skin_viewport,
                       sizeof(struct skin_viewport));
                deco_vp_element = viewport;
                have_deco = true;

                /* For thumbnail lists: fill row with card color, defer
                 * corner glyphs to the overlay pass which renders them
                 * at the correct top/bottom edge positions */
                if (item_h > listcfg[screen]->height)
                {
                    struct viewport fill_vp;
                    viewport_set_defaults(&fill_vp, screen);
                    fill_vp.x = parent->x;
                    fill_vp.y = parent->y + item_h * cur_line;
                    fill_vp.width = parent->width;
                    fill_vp.height = item_h;
                    display->set_viewport(&fill_vp);
                    display->set_drawmode(DRMODE_SOLID);
                    display->set_foreground(margin_fill_color);
                    display->fillrect(0, 0, parent->width, item_h);

                    if (!is_selected)
                    {
                        skin_viewport->vp.x = original_x;
                        skin_viewport->vp.y = original_y;
                    }
                    continue;
                }
            }

            if (has_margin && !margin_painted && original_x > 0)
            {
                int fill_w = margin_w + 8;
                struct viewport margin_vp;

                viewport_set_defaults(&margin_vp, screen);
                margin_vp.x = parent->x;
                margin_vp.y = parent->y + item_h * cur_line;
                margin_vp.width = fill_w;
                margin_vp.height = item_h;
                display->set_viewport(&margin_vp);
                display->set_drawmode(DRMODE_SOLID);
                display->set_foreground(margin_fill_color);
                display->fillrect(0, 0, fill_w, item_h);

                list->callback_draw_margin(
                    current_drawing_line, display,
                    0, 0, margin_w, item_h,
                    is_selected, list->data);

                margin_painted = true;
                display->set_viewport(&skin_viewport->vp);
#if defined(HAVE_ALBUMART) && defined(HAVE_LCD_COLOR)
                display->set_foreground(skin_viewport->vp.fg_pattern);
                display->set_background(skin_viewport->vp.bg_pattern);
#endif
            }

            /* Set images to not to be displayed */
            struct skin_token_list *imglist = SKINOFFSETTOPTR(get_skin_buffer(wps.data), wps.data->images);
            while (imglist)
            {
                struct wps_token *token = SKINOFFSETTOPTR(get_skin_buffer(wps.data), imglist->token);
                struct gui_img *img = NULL;
                if (token)
                    img = SKINOFFSETTOPTR(get_skin_buffer(wps.data), token->value.data);
                if (img)
                    img->display = -1;
                imglist = SKINOFFSETTOPTR(get_skin_buffer(wps.data), imglist->next);
            }
            if (skin_viewport->vp.width > 0)
            {
                struct skin_element** children = SKINOFFSETTOPTR(get_skin_buffer(wps.data), viewport->children);
                if (children && *children)
                {
                    int y_off = 0;
                    if (has_margin && item_h > listcfg[screen]->height
                        && original_x > 0)
                        y_off = (item_h - display->getcharheight()) / 2;
                    skin_render_viewport(SKINOFFSETTOPTR(get_skin_buffer(wps.data), (intptr_t)children[0]),
                                         &wps, skin_viewport, SKIN_REFRESH_ALL, false, y_off);
                }
                wps_display_images(&wps, &skin_viewport->vp);
            }
            /* force disableing scroll because it breaks later */
            if (!is_selected)
            {
                display->scroll_stop_viewport(&skin_viewport->vp);
                skin_viewport->vp.x = original_x;
                skin_viewport->vp.y = original_y;
                if (has_margin)
                {
                    skin_viewport->vp.width = skin_orig_width;
                    skin_viewport->vp.height = skin_orig_height;
                }
            }
        }

        /* Fallback: paint margin if no VP with original_x > 0 was seen */
        if (has_margin && !margin_painted)
        {
            struct viewport margin_vp;

            viewport_set_defaults(&margin_vp, screen);
            margin_vp.x = parent->x;
            margin_vp.y = parent->y + item_h * cur_line;
            margin_vp.width = margin_w;
            margin_vp.height = item_h;
            display->set_viewport(&margin_vp);
            display->set_drawmode(DRMODE_SOLID);
            display->set_foreground(margin_fill_color);
            display->fillrect(0, 0, margin_w, item_h);

            list->callback_draw_margin(
                current_drawing_line, display,
                0, 0, margin_w, item_h,
                is_selected, list->data);
        }

        /* Overlay: re-render decoration VP at original (unshifted) position
         * AFTER the VP loop, so corner glyphs paint on top of the thumbnail
         * without corrupting VP loop state */
        if (has_margin && have_deco && deco_vp_element)
        {
            int glyph_h = listcfg[screen]->height;
            int half_h = glyph_h / 2;
            int row_top = parent->y + item_h * cur_line;
            struct skin_element** dchildren =
                SKINOFFSETTOPTR(get_skin_buffer(wps.data),
                                deco_vp_element->children);

            if (dchildren && *dchildren)
            {
                int parent_bottom = parent->y + parent->height;

                /* TL+TR corner: top half of glyph at row top */
                struct skin_viewport tl_vp;
                memcpy(&tl_vp, &deco_svp_copy, sizeof(struct skin_viewport));
                tl_vp.vp.x = parent->x;
                tl_vp.vp.y = row_top;
                tl_vp.vp.width = parent->width;
                tl_vp.vp.height = half_h;
                if (tl_vp.vp.y + tl_vp.vp.height > parent_bottom)
                    tl_vp.vp.height = parent_bottom - tl_vp.vp.y;
#if defined(HAVE_ALBUMART) && defined(HAVE_LCD_COLOR)
                tl_vp.vp.fg_pattern =
                    dynamic_colors_resolve(tl_vp.dc_orig_fg);
                tl_vp.vp.bg_pattern =
                    dynamic_colors_resolve(tl_vp.dc_orig_bg);
#endif
                if (tl_vp.vp.height > 0)
                {
                display->set_viewport(&tl_vp.vp);
                display->set_foreground(tl_vp.vp.bg_pattern);
                display->set_background(tl_vp.vp.bg_pattern);
                lcd_set_alpha_refbg(true, margin_fill_color);
                skin_render_viewport(
                    SKINOFFSETTOPTR(get_skin_buffer(wps.data),
                                    (intptr_t)dchildren[0]),
                    &wps, &tl_vp, SKIN_REFRESH_ALL, true, 0);
                wps_display_images(&wps, &tl_vp.vp);
                lcd_set_alpha_refbg(false, 0);
                }

                /* BL+BR corner: bottom half of glyph at row bottom */
                struct skin_viewport bl_vp;
                memcpy(&bl_vp, &deco_svp_copy, sizeof(struct skin_viewport));
                bl_vp.vp.x = parent->x;
                bl_vp.vp.y = row_top + item_h - half_h;
                bl_vp.vp.width = parent->width;
                bl_vp.vp.height = half_h;
                if (bl_vp.vp.y + bl_vp.vp.height > parent_bottom)
                    bl_vp.vp.height = parent_bottom - bl_vp.vp.y;
#if defined(HAVE_ALBUMART) && defined(HAVE_LCD_COLOR)
                bl_vp.vp.fg_pattern =
                    dynamic_colors_resolve(bl_vp.dc_orig_fg);
                bl_vp.vp.bg_pattern =
                    dynamic_colors_resolve(bl_vp.dc_orig_bg);
#endif
                if (bl_vp.vp.height > 0 && bl_vp.vp.y < parent_bottom)
                {
                display->set_viewport(&bl_vp.vp);
                display->set_foreground(bl_vp.vp.bg_pattern);
                display->set_background(bl_vp.vp.bg_pattern);
                lcd_set_alpha_refbg(true, margin_fill_color);
                skin_render_viewport(
                    SKINOFFSETTOPTR(get_skin_buffer(wps.data),
                                    (intptr_t)dchildren[0]),
                    &wps, &bl_vp, SKIN_REFRESH_ALL, true, -half_h);
                wps_display_images(&wps, &bl_vp.vp);
                lcd_set_alpha_refbg(false, 0);
                }
            }
        }
    }
    current_column = -1;
    current_row = -1;
#if defined(HAVE_ALBUMART) && defined(HAVE_LCD_COLOR)
    parent->fg_pattern = dc_saved_fg;
    parent->bg_pattern = dc_saved_bg;
#endif
    display->set_viewport(parent);
    if (list_need_full_update() | skin_render_pending_update())
    {
        display->set_viewport(NULL);
        display->update();
        sb_skin_force_next_update();
    }
    else
        display->update_viewport();
    current_drawing_line = list->selected_item;
    return true;
}
