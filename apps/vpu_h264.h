/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * S5L8702 VPU-B H.264 Baseline hardware decoder.
 *
 * Drives the VPU-B block at 0x39800000 on iPod Classic 6G (S5L8702).
 * Full hardware decode: CAVLC, dequant, IDCT, intra/inter prediction,
 * motion compensation, and in-loop deblocking.
 *
 * Supports: H.264 Baseline profile, level <= 3.0, single slice per frame.
 * Proven bit-perfect against ffmpeg (16/16 frames, 0 pixel diffs).
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ********************************************************************* * Copyright (C) 2025 Nux Li
 *
 *******/

#ifndef VPU_H264_H
#define VPU_H264_H

#include <stdint.h>
#include <stddef.h>

struct vpu_h264;

/* Returns minimum buffer size needed for given max dimensions.
 * Caller must provide a contiguous buffer of at least this size
 * to vpu_h264_open(). Accounts for alignment padding. */
size_t vpu_h264_buf_size(int max_w, int max_h);

/* Initialize decoder. Allocates internal buffers from buf via bump
 * allocator. Powers on VPU-B hardware and IRQ.
 * Returns context pointer (inside buf) or NULL on error. */
struct vpu_h264 *vpu_h264_open(void *buf, size_t buf_size,
                                int max_w, int max_h);

/* Feed avcC decoder configuration (from MP4 container).
 * Parses SPS and PPS NALUs from the avcC blob.
 * Returns 0 on success, -1 on error. */
int vpu_h264_configure(struct vpu_h264 *v,
                        const uint8_t *avcc, int avcc_len);

/* Feed one raw NALU (including nal_header byte, excluding length prefix).
 * Accepts EBSP (emulation prevention bytes intact).
 * Handles SPS (7), PPS (8), IDR slice (5), non-IDR slice (1) internally.
 * For slice NALUs: triggers synchronous HW decode (~5ms).
 *
 * Returns:  1 = frame decoded (call vpu_h264_get_frame)
 *           0 = NALU consumed (SPS/PPS/SEI, no frame output)
 *          -1 = error (STATUS1 bits or timeout) */
int vpu_h264_decode_nalu(struct vpu_h264 *v,
                          const uint8_t *nalu, int nalu_len);

/* Get pointers to last decoded frame planes (YCbCr 4:2:0 planar).
 * Pointers are cached aliases; cache is already invalidated.
 * Valid until next vpu_h264_decode_nalu() call. */
void vpu_h264_get_frame(const struct vpu_h264 *v,
                         const uint8_t **y, const uint8_t **cb,
                         const uint8_t **cr, int *w, int *h);

/* Shutdown decoder. Powers off VPU-B hardware. */
void vpu_h264_close(struct vpu_h264 *v);

#endif /* VPU_H264_H */
