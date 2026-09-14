/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2007 Nicolas Pennequin
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

#ifndef _ALBUMART_H_
#define _ALBUMART_H_

#if defined(HAVE_ALBUMART) || defined(PLUGIN)

#include <stdbool.h>
#include <stddef.h>
#include "metadata.h"
#include "skin_engine/skin_engine.h"

/* Look for albumart bitmap in the same dir as the track and in its parent dir.
 * Calls size_func to get the dimensions to look for
 * Stores the found filename in the buf parameter.
 * Returns true if a bitmap was found, false otherwise */
bool find_albumart(const struct mp3entry *id3, char *buf, int buflen,
                    const struct dim *dim);

bool search_albumart_files(const struct mp3entry *id3, const char *size_string,
                           char *buf, int buflen);

void get_albumart_size(struct bitmap *bmp);

#ifndef PLUGIN
/* Decoded native bitmap bytes for a target dim, including struct bitmap */
size_t albumart_decoded_size(const struct dim *dim);
/* Extra workspace JPEG/BMP loaders need on top of the pixel buffer */
size_t albumart_decode_overhead(int width);
/* Decode file or embedded JPEG into buf. Returns total bytes used
 * (struct bitmap + pixels) or <= 0 on failure. */
int albumart_decode_fd(int fd, const char *path, const struct dim *dim,
                       struct mp3_albumart *embedded,
                       void *buf, size_t max_size);
#endif

#endif /* HAVE_ALBUMART */

#endif /* _ALBUMART_H_ */
