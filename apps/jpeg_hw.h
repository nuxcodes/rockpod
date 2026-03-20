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
 * VPU-A hardware-accelerated JPEG decoder for S5L8702 (iPod Classic 6G).
 * SW Huffman entropy decode + HW 8×8 IDCT via VPU-A at 0x39600000.
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
#ifndef __JPEG_HW_H__
#define __JPEG_HW_H__

#include <stdbool.h>
#include "lcd.h"

/* Decode a JPEG from an open fd into an RGB565 buffer, downscaling
 * to fit out_w × out_h with letterboxing.
 *
 * fd:        open file descriptor, seeked to start of JPEG data
 * jpeg_size: byte count of JPEG blob
 * out:       output buffer (must hold out_w * out_h * sizeof(fb_data))
 * out_w/h:   desired output dimensions (e.g. THUMB_SIZE)
 * work:      scratch buffer for JPEG source data (>= jpeg_size)
 * work_size: size of scratch buffer
 *
 * VPU-A frame/work buffers are allocated internally via core_alloc.
 * Returns true on success.  On any failure (unsupported format,
 * HW timeout, alloc failure), returns false without modifying out. */
bool jpeg_hw_decode_fd(int fd, unsigned long jpeg_size,
                       fb_data *out, int out_w, int out_h,
                       void *work, size_t work_size);

#endif /* __JPEG_HW_H__ */
