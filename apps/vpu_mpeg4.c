/***************************************************************************
 * S5L8702 VPU-A MPEG-4 Part 2 hardware decoder.
 *
 * Copyright (C) 2025 Nux Li
 *
 * Register map and decode sequence ROM-verified from retailos.bin:
 * - VOL parser: FUN_000E6FF0
 * - VOP parser: FUN_000E73F0
 * - Register setup: FUN_000940BC
 * - Decode submit: FUN_000692E0
 * - IRQ: #45 (VIC1 bit 13)
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
#include "config.h"

#ifdef IPOD_6G

#include "system.h"
#include "cpu.h"
#include "kernel.h"
#include "vpu_mpeg4.h"

#include <string.h>

/* VPU-A register blocks (shared with jpeg_hw.c) */
#define VPUA_BASE     0x39600000
#define VPUA_DECODE   0x39610000
#define VPUA_DMA      0x39630000
#define VPUA_QUANT    0x39641000
#define VPUA_STATUS   0x39650000
#define VPUA_FRAME    0x39660000
#define VPU_MODE_REG  0x38100314
#define CLK_BASE      0x3C500000

#define REG32(a)      (*(volatile uint32_t *)(a))
#define VPUA(off)     REG32(VPUA_BASE + (off))
#define VPUA_D(off)   REG32(VPUA_DECODE + (off))
#define VPUA_M(off)   REG32(VPUA_DMA + (off))
#define VPUA_Q(off)   REG32(VPUA_QUANT + (off))
#define VPUA_S(off)   REG32(VPUA_STATUS + (off))
#define VPUA_F(off)   REG32(VPUA_FRAME + (off))

#define PHYS(x) ((uint32_t)((uintptr_t)(x) & 0x7FFFFFFF))
#define UNCACHED(x) ((typeof(x))((uintptr_t)(x) | 0x40000000))

#define MAX_REF_FRAMES 3
#define BS_DMA_SIZE    (256 * 1024)

#define VOP_I  0
#define VOP_P  1
#define VOP_B  2

/* Bitstream reader */
struct bs_ctx {
    const uint8_t *data;
    int len;
    int pos;
};

static uint32_t bs_read(struct bs_ctx *b, int n)
{
    uint32_t val = 0;
    int i;
    for (i = 0; i < n; i++)
    {
        int byte_off = b->pos / 8;
        int bit_off  = 7 - (b->pos % 8);
        if (byte_off < b->len)
            val = (val << 1) | ((b->data[byte_off] >> bit_off) & 1);
        else
            val <<= 1;
        b->pos++;
    }
    return val;
}

static uint32_t bs_read1(struct bs_ctx *b)
{
    return bs_read(b, 1);
}

static int ceil_log2(int v)
{
    int n = 0;
    v--;
    while (v > 0) { v >>= 1; n++; }
    return n;
}

/* VOL parameters extracted from esds/elementary stream */
struct vol_info {
    int width, height;
    int wmb, hmb;
    int time_inc_bits;
    int quant_type;
    int quant_precision;
    int complexity_est_disable;
    int shape;
    int not_8_bit;
    int sprite_enable;
    int interlaced;
};

/* VOP header */
struct vop_info {
    int type;
    int coded;
    int rounding;
    int intra_dc_vlc_thr;
    int quant;
    int fcode_fwd;
    int fcode_bwd;
    uint32_t time_inc;
    int bit_position;
};

struct vpu_mpeg4 {
    int max_w, max_h;
    struct vol_info vol;

    uint8_t *frame_y[MAX_REF_FRAMES];
    uint8_t *frame_cb[MAX_REF_FRAMES];
    uint8_t *frame_cr[MAX_REF_FRAMES];
    int frame_y_size;
    int frame_c_size;

    int cur_out;
    int last_decoded;
    int ref_fwd;
    int ref_bwd;

    uint8_t *bs_dma;
};

/* ---- esds descriptor parser ---- */

static int esds_read_len(const uint8_t *p, int avail, int *consumed)
{
    int len = 0, i;
    *consumed = 0;
    for (i = 0; i < 4 && i < avail; i++)
    {
        len = (len << 7) | (p[i] & 0x7F);
        (*consumed)++;
        if (!(p[i] & 0x80))
            break;
    }
    return len;
}

static int esds_find_decoder_specific(const uint8_t *esds, int esds_len,
                                       const uint8_t **out, int *out_len)
{
    int pos = 4; /* skip version + flags */
    int tag, len, consumed;

    if (pos >= esds_len)
        return -1;

    /* ES_Descriptor (tag 0x03) */
    tag = esds[pos++];
    if (tag != 0x03 || pos >= esds_len)
        return -1;
    len = esds_read_len(esds + pos, esds_len - pos, &consumed);
    pos += consumed;
    pos += 3; /* ES_ID(2) + streamPriority(1) */
    (void)len;

    if (pos >= esds_len)
        return -1;

    /* DecoderConfigDescriptor (tag 0x04) */
    tag = esds[pos++];
    if (tag != 0x04 || pos >= esds_len)
        return -1;
    len = esds_read_len(esds + pos, esds_len - pos, &consumed);
    pos += consumed;
    pos += 13; /* objectType(1) + streamType(1) + bufferSize(3) + maxBR(4) + avgBR(4) */

    if (pos >= esds_len)
        return -1;

    /* DecoderSpecificInfo (tag 0x05) */
    tag = esds[pos++];
    if (tag != 0x05 || pos >= esds_len)
        return -1;
    len = esds_read_len(esds + pos, esds_len - pos, &consumed);
    pos += consumed;

    if (pos + len > esds_len)
        len = esds_len - pos;

    *out = esds + pos;
    *out_len = len;
    return 0;
}

/* ---- VOL parser ---- */

static int parse_vol(struct vol_info *vol, const uint8_t *data, int len)
{
    struct bs_ctx b = { data, len, 0 };
    uint32_t sc;
    int i;

    memset(vol, 0, sizeof(*vol));
    vol->quant_precision = 5;

    /* Search for VOL start code 0x00000120-0x0000012F */
    for (i = 0; i < len - 4; i++)
    {
        if (data[i] == 0 && data[i+1] == 0 && data[i+2] == 1
            && data[i+3] >= 0x20 && data[i+3] <= 0x2F)
        {
            b.pos = (i + 4) * 8;
            break;
        }
    }
    if (i >= len - 4)
        return -1;

    bs_read1(&b); /* random_accessible_vol */

    sc = bs_read(&b, 8); /* video_object_type_indication */
    (void)sc;

    if (bs_read1(&b)) /* is_object_layer_identifier */
    {
        bs_read(&b, 4); /* video_object_layer_verid */
        bs_read(&b, 3); /* video_object_layer_priority */
    }

    if (bs_read(&b, 4) == 0xF) /* aspect_ratio_info == extended */
    {
        bs_read(&b, 8); /* par_width */
        bs_read(&b, 8); /* par_height */
    }

    if (bs_read1(&b)) /* vol_control_parameters */
    {
        bs_read(&b, 2); /* chroma_format */
        bs_read1(&b);   /* low_delay */
        if (bs_read1(&b)) /* vbv_parameters */
        {
            bs_read(&b, 15); bs_read1(&b); /* first_half_bit_rate, marker */
            bs_read(&b, 15); bs_read1(&b); /* latter_half_bit_rate, marker */
            bs_read(&b, 15); bs_read1(&b); /* first_half_vbv_buffer_size, marker */
            bs_read(&b, 3);               /* latter_half_vbv_buffer_size */
            bs_read(&b, 11); bs_read1(&b); /* first_half_vbv_occupancy, marker */
            bs_read(&b, 15); bs_read1(&b); /* latter_half_vbv_occupancy, marker */
        }
    }

    vol->shape = bs_read(&b, 2);
    if (vol->shape != 0) /* only rectangular supported */
        return -1;

    bs_read1(&b); /* marker */

    vol->time_inc_bits = ceil_log2(bs_read(&b, 16)); /* vop_time_increment_resolution */
    if (vol->time_inc_bits < 1) vol->time_inc_bits = 1;

    bs_read1(&b); /* marker */

    if (bs_read1(&b)) /* fixed_vop_rate */
        bs_read(&b, vol->time_inc_bits);

    bs_read1(&b); /* marker */
    vol->width = bs_read(&b, 13);
    bs_read1(&b); /* marker */
    vol->height = bs_read(&b, 13);
    bs_read1(&b); /* marker */

    vol->wmb = (vol->width + 15) / 16;
    vol->hmb = (vol->height + 15) / 16;

    vol->interlaced = bs_read1(&b);
    if (vol->interlaced)
        return -1;

    bs_read1(&b); /* obmc_disable */

    vol->sprite_enable = bs_read(&b, 1);
    if (vol->sprite_enable)
        return -1;

    if (bs_read1(&b)) /* not_8_bit */
    {
        vol->not_8_bit = 1;
        vol->quant_precision = bs_read(&b, 4);
        bs_read(&b, 4); /* bits_per_pixel */
    }

    vol->quant_type = bs_read1(&b);

    if (vol->quant_type)
    {
        if (bs_read1(&b)) /* load_intra_quant_mat */
        {
            for (i = 0; i < 64; i++)
                bs_read(&b, 8);
        }
        if (bs_read1(&b)) /* load_nonintra_quant_mat */
        {
            for (i = 0; i < 64; i++)
                bs_read(&b, 8);
        }
    }

    bs_read1(&b); /* quarter_sample (must be 0 for Simple Profile) */
    vol->complexity_est_disable = bs_read1(&b);

    return 0;
}

/* ---- VOP parser ---- */

static int parse_vop(struct vop_info *vop, const struct vol_info *vol,
                     const uint8_t *data, int len)
{
    struct bs_ctx b = { data, len, 0 };
    int i;

    memset(vop, 0, sizeof(*vop));

    /* Find VOP start code 0x000001B6 */
    for (i = 0; i < len - 4; i++)
    {
        if (data[i] == 0 && data[i+1] == 0 && data[i+2] == 1
            && data[i+3] == 0xB6)
        {
            b.pos = (i + 4) * 8;
            break;
        }
    }
    if (i >= len - 4)
        return -1;

    vop->type = bs_read(&b, 2);

    /* modulo_time_base */
    while (bs_read1(&b))
        ;
    bs_read1(&b); /* marker */

    vop->time_inc = bs_read(&b, vol->time_inc_bits);
    bs_read1(&b); /* marker */

    vop->coded = bs_read1(&b);
    if (!vop->coded)
        return 0;

    if (vop->type == VOP_P)
        vop->rounding = bs_read1(&b);

    if (!vol->complexity_est_disable && vol->shape == 0
        && (vop->type == VOP_I || vop->type == VOP_P))
    {
        bs_read1(&b); /* reduced_resolution */
    }

    vop->intra_dc_vlc_thr = bs_read(&b, 3);
    vop->quant = bs_read(&b, vol->quant_precision);

    if (vop->type != VOP_I)
    {
        vop->fcode_fwd = bs_read(&b, 3);
    }
    if (vop->type == VOP_B)
    {
        vop->fcode_bwd = bs_read(&b, 3);
    }

    vop->bit_position = b.pos;
    return 1;
}

/* ---- VPU-A hardware control ---- */

static void vpua_reset(void)
{
    /* ROM FUN_0007476C — same sequence as jpeg_hw.c */
    VPUA(0x1C) = 0xFFFFFFFF;
    VPUA_D(0x00) = 0xFFFFFFFF;
    VPUA_D(0x00) = 0xFFFFFFFF;
    VPUA_M(0x100) = 0xFFFFFFFF;
    VPUA_S(0x00) = 0xFFFFFFFF;
    VPUA_F(0x00) = 0xFFFFFFFF;
    VPUA(0x00) = 0xFFFFFFFF;
}

static void vpua_power_on_video(void)
{
    uint32_t cg, pw;

    /* Clock setup — same as jpeg_hw.c */
    cg = REG32(CLK_BASE + 0x08);
    cg &= ~0x80000000;
    cg |= 0x30000000;
    REG32(CLK_BASE + 0x08) = cg;

    /* Enable VPP (bits 14-16) + VPU-A (bit 18) */
    pw = PWRCON(0);
    PWRCON(0) = pw & ~((7 << 14) | (1 << 18));

    /* Set VIDEO mode (bit 0 = 1), not JPEG mode (bit 0 = 0) */
    REG32(VPU_MODE_REG) |= 1;

    vpua_reset();

    /* MPEG-4 decode clock config (ROM: 0x142 for video, 0x40 for JPEG) */
    VPUA(0x04) = 0x142;
    VPUA(0x0C) = 0;
    VPUA(0x10) = 0x80000000;
}

static void vpua_power_off(void)
{
    vpua_reset();
    PWRCON(0) |= (7 << 14) | (1 << 18);
}

static void vpua_setup(struct vpu_mpeg4 *v, const uint8_t *bs_data, int bs_len)
{
    memcpy(UNCACHED(v->bs_dma), bs_data, MIN(bs_len, BS_DMA_SIZE));

    /* Per-frame register setup (power/clock already done in open) */
    VPUA_D(0x1C) = 1;
    VPUA_M(0x104) = 0x14;
    VPUA_D(0x04) = 0x3F;
    VPUA_F(0x04) = 0x12;
}

static void vpua_set_frame(struct vpu_mpeg4 *v, int idx)
{
    VPUA_F(0x18) = PHYS(v->frame_y[idx]);
    VPUA_F(0x20) = PHYS(v->frame_cb[idx]);
    VPUA_F(0x24) = PHYS(v->frame_y[idx]) + v->vol.wmb * 16;
    VPUA_F(0x78) = PHYS(v->frame_cb[idx]);
    VPUA_F(0x7C) = PHYS(v->frame_cr[idx]);
    VPUA_F(0x80) = 0;
}

static void vpua_set_ref(int slot, const uint8_t *y, const uint8_t *cbcr)
{
    uint32_t base = (slot == 0) ? 0x2C : 0x3C;
    VPUA_F(base + 0x00) = PHYS(y);
    VPUA_F(base + 0x04) = 0;
    VPUA_F(base + 0x08) = PHYS(cbcr);
    VPUA_F(base + 0x0C) = 0;
}

static void vpua_set_mb_dims(struct vpu_mpeg4 *v)
{
    int wmb = v->vol.wmb;
    int hmb = v->vol.hmb;
    int log2_mbs = ceil_log2(wmb * hmb);

    VPUA_F(0x6C) = ((uint32_t)wmb << 16) | (uint32_t)hmb;
    VPUA(0x10) = (VPUA(0x10) & 0xFF0000FF)
               | ((uint32_t)(wmb - 1) << 16)
               | ((uint32_t)(hmb - 1) << 8);
    VPUA_F(0x10) = 0x200;
    (void)log2_mbs;
}

static int vpua_decode(struct vpu_mpeg4 *v, const struct vop_info *vop,
                       const uint8_t *bs_data)
{
    uint32_t params;
    int byte_off, bit_off, i, t;

    /* Quantizer mode */
    VPUA_Q(0x804) = ((uint32_t)v->vol.not_8_bit << 8) | 0x20;

    /* Rounding control */
    VPUA_S(0x10) = 0x0C02 | ((uint32_t)vop->rounding << 2);

    /* Packed decode parameters */
    params = (uint32_t)vop->type
           | ((uint32_t)vop->fcode_fwd << 2)
           | ((uint32_t)vop->fcode_bwd << 5)
           | ((uint32_t)vop->intra_dc_vlc_thr << 11)
           | ((uint32_t)v->vol.quant_type << 17);
    VPUA_D(0x10) = params;

    /* Quantization parameter */
    VPUA_D(0x14) = (uint32_t)vop->quant;

    /* Decode mode */
    VPUA_D(0x34) = 3;

    /* Timing/dimension info */
    VPUA_D(0x9C) = (uint32_t)v->vol.time_inc_bits
                 | ((uint32_t)ceil_log2(v->vol.wmb * v->vol.hmb) << 4)
                 | ((uint32_t)vop->time_inc << 8);
    VPUA_D(0xA0) = 0;
    VPUA_D(0xA4) = 0;
    VPUA_D(0xA8) = 0;

    /* DMA config */
    VPUA_M(0x100) = 0x20;

    /* START DECODE */
    VPUA_D(0x00) = 4;

    /* Clear interrupt status */
    VPUA_F(0x00) = 0xFFFFFFFF;

    /* Bitstream pointers */
    byte_off = vop->bit_position / 8;
    VPUA_F(0x18) = PHYS(v->bs_dma) + byte_off;
    VPUA_F(0x1C) = PHYS(v->bs_dma) + byte_off + 0x103;

    /* DMA enable */
    VPUA_M(0x110) = 2;

    /* Interrupt enable */
    VPUA_F(0x0C) = 3;

    /* Bitstream alignment: byte FIFO */
    bit_off = vop->bit_position % 8;
    for (i = 0; i < (byte_off & 3); i++)
        (void)VPUA_M(0x20);
    for (i = 0; i < bit_off; i++)
        (void)VPUA_M(0x04);

    /* Wait for completion (poll status register) */
    t = 1000000;
    while ((VPUA_S(0x14) & 0x10000) && --t > 0)
        ;

    commit_discard_dcache();
    return (t > 0) ? 0 : -1;
}

/* ---- Public API ---- */

size_t vpu_mpeg4_buf_size(int max_w, int max_h)
{
    size_t sz = 0;
    int fy = max_w * max_h;
    int fc = (max_w / 2) * (max_h / 2);

    sz += sizeof(struct vpu_mpeg4) + 32;
    sz += (fy + fc + fc + 4096) * MAX_REF_FRAMES;
    sz += BS_DMA_SIZE + 32;
    return sz;
}

struct vpu_mpeg4 *vpu_mpeg4_open(void *buf, size_t buf_size,
                                  int max_w, int max_h,
                                  const uint8_t *esds, int esds_len)
{
    struct vpu_mpeg4 *v;
    uint8_t *p = (uint8_t *)buf;
    int fy, fc, i;

    if (buf_size < vpu_mpeg4_buf_size(max_w, max_h))
        return NULL;

    v = (struct vpu_mpeg4 *)(((uintptr_t)p + 31) & ~31UL);
    p = (uint8_t *)v + sizeof(*v);
    memset(v, 0, sizeof(*v));

    v->max_w = max_w;
    v->max_h = max_h;

    fy = max_w * max_h;
    fc = (max_w / 2) * (max_h / 2);
    v->frame_y_size = fy;
    v->frame_c_size = fc;

    for (i = 0; i < MAX_REF_FRAMES; i++)
    {
        p = (uint8_t *)(((uintptr_t)p + 4095) & ~4095UL);
        v->frame_y[i]  = p; p += fy;
        v->frame_cb[i] = p; p += fc;
        v->frame_cr[i] = p; p += fc;
        memset(v->frame_y[i], 0x80, fy);
        memset(v->frame_cb[i], 0x80, fc);
        memset(v->frame_cr[i], 0x80, fc);
    }

    v->bs_dma = (uint8_t *)(((uintptr_t)p + 31) & ~31UL);

    /* Extract VOL from esds DecoderSpecificInfo */
    {
        const uint8_t *vol_data = esds;
        int vol_len = esds_len;

        /* Try proper esds parsing first */
        if (esds_len > 4 && esds_find_decoder_specific(esds, esds_len,
                                                        &vol_data, &vol_len) < 0)
        {
            /* Fallback: search entire esds for VOL start code */
            vol_data = esds;
            vol_len = esds_len;
        }

        if (parse_vol(&v->vol, vol_data, vol_len) < 0)
            return NULL;

        if (v->vol.width > max_w || v->vol.height > max_h)
            return NULL;
    }

    /* Power on VPU-A in video decode mode */
    vpua_power_on_video();

    v->ref_fwd = -1;
    v->ref_bwd = -1;

    return v;
}

int vpu_mpeg4_decode_vop(struct vpu_mpeg4 *v,
                          const uint8_t *data, int len)
{
    struct vop_info vop;
    int ret, out_idx;

    ret = parse_vop(&vop, &v->vol, data, len);
    if (ret <= 0)
        return ret;

    out_idx = v->cur_out;

    /* Setup VPU-A */
    vpua_setup(v, data, len);
    vpua_set_frame(v, out_idx);
    vpua_set_mb_dims(v);

    /* Reference frames */
    if (vop.type == VOP_P || vop.type == VOP_B)
    {
        if (v->ref_fwd >= 0)
            vpua_set_ref(0, v->frame_y[v->ref_fwd],
                         v->frame_cb[v->ref_fwd]);
    }
    if (vop.type == VOP_B)
    {
        if (v->ref_bwd >= 0)
            vpua_set_ref(1, v->frame_y[v->ref_bwd],
                         v->frame_cb[v->ref_bwd]);
    }

    /* Decode */
    if (vpua_decode(v, &vop, data) < 0)
        return -1;

    /* Update reference frames */
    if (vop.type != VOP_B)
    {
        v->ref_bwd = v->ref_fwd;
        v->ref_fwd = out_idx;
    }

    v->last_decoded = out_idx;
    v->cur_out = (out_idx + 1) % MAX_REF_FRAMES;
    return 1;
}

void vpu_mpeg4_get_frame(const struct vpu_mpeg4 *v,
                          const uint8_t **y, const uint8_t **cb,
                          const uint8_t **cr, int *w, int *h)
{
    int idx = v->last_decoded;
    if (y)  *y  = v->frame_y[idx];
    if (cb) *cb = v->frame_cb[idx];
    if (cr) *cr = v->frame_cr[idx];
    if (w)  *w  = v->vol.width;
    if (h)  *h  = v->vol.height;
}

void vpu_mpeg4_close(struct vpu_mpeg4 *v)
{
    if (v)
        vpua_power_off();
}

#endif /* IPOD_6G */
