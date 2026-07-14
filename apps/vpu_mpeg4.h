/***************************************************************************
 * S5L8702 VPU-A MPEG-4 Part 2 hardware decoder.
 *
 * Drives the VPU-A block at 0x39600000 on iPod Classic 6G (S5L8702).
 * Hardware decode: VLC, dequant, IDCT, motion compensation, deblocking.
 *
 * Supports: MPEG-4 Simple Profile + B-frames (quasi-ASP).
 * Max resolution: 720x576. No quarter-pixel MC, no GMC, no interlaced.
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
 ***************************************************************************/

#ifndef VPU_MPEG4_H
#define VPU_MPEG4_H

#include <stdint.h>
#include <stddef.h>

struct vpu_mpeg4;

/* Returns minimum buffer size for given max dimensions. */
size_t vpu_mpeg4_buf_size(int max_w, int max_h);

/* Initialize decoder. Powers on VPU-A hardware.
 * esds/esds_len: decoder config from MP4 esds box (contains VOL headers).
 * Returns context pointer or NULL on error. */
struct vpu_mpeg4 *vpu_mpeg4_open(void *buf, size_t buf_size,
                                  int max_w, int max_h,
                                  const uint8_t *esds, int esds_len);

/* Feed one VOP (Video Object Plane) for decode.
 * Returns: 1 = frame decoded, 0 = consumed, -1 = error. */
int vpu_mpeg4_decode_vop(struct vpu_mpeg4 *v,
                          const uint8_t *data, int len);

/* Get last decoded frame (YCbCr 4:2:0 planar). */
void vpu_mpeg4_get_frame(const struct vpu_mpeg4 *v,
                          const uint8_t **y, const uint8_t **cb,
                          const uint8_t **cr, int *w, int *h);

/* Shutdown decoder. Powers off VPU-A. */
void vpu_mpeg4_close(struct vpu_mpeg4 *v);

#endif /* VPU_MPEG4_H */
