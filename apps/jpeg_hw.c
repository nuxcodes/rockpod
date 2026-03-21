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
 * VPU-A hardware-accelerated JPEG baseline decoder for S5L8702.
 *
 * Architecture: SW Huffman entropy decode → HW 8×8 IDCT via VPU-A.
 * Same hybrid approach Apple uses (FUN_0007e9e0 in OF).
 *
 * Supports: Baseline DCT (SOF0), 4:2:0 chroma, 8-bit, restart markers.
 * Unsupported formats return false (caller falls back to SW decoder).
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
#include "kernel.h"
#include "s5l87xx.h"
/* core_alloc not needed — VPU-A uses static vpu_frame_buf */
#include "file.h"
#include "jpeg_hw.h"

#include <string.h>

/* ================================================================
 * 1. VPU-A register definitions (from jpeg_poc.c)
 * ================================================================ */

#define REG32(addr) (*(volatile uint32_t *)(addr))

#define VDEC_MAIN   0x39600000
#define VDEC_CORE   0x39610000
#define VDEC_DMA    0x39630000
#define VDEC_XFORM  0x39641000
#define VDEC_DEBLK  0x39650000
#define VDEC_SUB    0x39660000

#define XFORM_800   (*(volatile uint32_t *)0x39641800)
#define XFORM_808   (*(volatile uint32_t *)0x39641808)
#define DMA_10C     (*(volatile uint32_t *)0x3963010C)

#define XFORM_CMD_BASE  0x00020341

#define CLK_BASE        0x3C500000
#define VPU_MODE_REG    0x38100314

#define DMA_WORK_SIZE   0x400
#define WORK_BUF_SIZE   0x20000
#define COEFF_BUF_SIZE  0x200
#define SMALL_BUF_SIZE  0x400

#define ALIGN32(x)  (((uintptr_t)(x) + 31) & ~31)
#define ALIGN4K(x)  (((uintptr_t)(x) + 0xFFF) & ~0xFFF)

/* Static frame buffer for VPU-A decode — avoids core_alloc() which
 * would trigger audio buffer shrink callback and stop playback.
 * 768KB handles sources up to ~640×640 4:2:0 (720×480 DVD, 800×480 wide).
 * Larger sources fall back to SW decoder. */
#define VPU_FRAME_BUF_SIZE  0xC0000
static uint8_t vpu_frame_buf[VPU_FRAME_BUF_SIZE] CACHEALIGN_ATTR;

/* JPEG zigzag scan order → raster position mapping.
 * DQT marker stores Q values in zigzag order; XFORM registers
 * are indexed by raster position.  Apple de-zigzags Q tables
 * during DQT parse (FUN_000df908). */
static const uint8_t zz_to_raster[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

/* ================================================================
 * 2. VPU-A hardware control
 * ================================================================ */

static void vpua_power_on(void)
{
    uint32_t cg, pw;

    cg = REG32(CLK_BASE + 0x08);
    cg &= ~0x80000000;
    cg |= 0x30000000;
    REG32(CLK_BASE + 0x08) = cg;
    /* No delay needed: PLL2 already locked (system clock), CG16_SVID
     * is just a gate/divider.  Apple FW has zero delay here.
     * All Rockbox drivers (LCD, ATA, USB) access registers immediately
     * after clockgate_enable() with zero delay. */

    pw = PWRCON(0);
    PWRCON(0) = pw & ~((7 << 14) | (1 << 18));

    REG32(VPU_MODE_REG) &= ~1; /* JPEG mode */

    REG32(VDEC_MAIN + 0x1C) = 0xFFFFFFFF;
    REG32(VDEC_CORE)        = 0xFFFFFFFF;
    REG32(VDEC_CORE)        = 0xFFFFFFFF;
    REG32(VDEC_DMA + 0x100) = 0xFFFFFFFF;
    REG32(VDEC_DEBLK)       = 0xFFFFFFFF;
    REG32(VDEC_SUB)         = 0xFFFFFFFF;
    REG32(VDEC_MAIN)        = 0xFFFFFFFF;

    REG32(VDEC_MAIN + 0x04) = 0x40;
    REG32(VDEC_MAIN + 0x0C) = 0;
    REG32(VDEC_MAIN + 0x10) = 0x10100;
    REG32(VDEC_SUB  + 0x04) = 2;
    REG32(VDEC_SUB  + 0x10) = 0x182;
    REG32(VDEC_DMA + 0x110) = 0x800;
    REG32(VDEC_XFORM + 0x804) = 0x40;
    REG32(VDEC_DEBLK + 0x10) = 0x10;
    REG32(VDEC_SUB  + 0x6C) = 0x10001;
}

static void vpua_power_off(void)
{
    uint32_t cg;

    REG32(VDEC_MAIN + 0x1C) = 0xFFFFFFFF;
    REG32(VDEC_CORE)        = 0xFFFFFFFF;
    REG32(VDEC_DMA + 0x100) = 0xFFFFFFFF;
    REG32(VDEC_DEBLK)       = 0xFFFFFFFF;
    REG32(VDEC_SUB)         = 0xFFFFFFFF;
    REG32(VDEC_MAIN)        = 0xFFFFFFFF;

    PWRCON(0) |= (7 << 14) | (1 << 18);
    REG32(VPU_MODE_REG) |= 1;

    cg = REG32(CLK_BASE + 0x08);
    cg |= 0x80000000;
    cg &= ~0x30000000;
    REG32(CLK_BASE + 0x08) = cg;
}

static void vpua_jpeg_init(uint32_t dma_addr, uint32_t work_addr)
{
    int i;

    REG32(VDEC_MAIN + 0x1C) = 0xFFFFFFFF;
    REG32(VDEC_MAIN + 0x0C) = 0;
    REG32(VDEC_CORE)        = 0xFFFFFFFF;
    REG32(VDEC_CORE)        = 0xFFFFFFFF;
    REG32(VDEC_DMA + 0x100) = 0xFFFFFFFF;
    REG32(VDEC_DEBLK)       = 0xFFFFFFFF;
    REG32(VDEC_SUB)         = 0xFFFFFFFF;
    REG32(VDEC_MAIN)        = 0xFFFFFFFF;

    REG32(VDEC_MAIN + 0x04) = 0x40;
    REG32(VDEC_SUB  + 0x04) = 2;
    REG32(VDEC_SUB  + 0x10) = 0x182;
    REG32(VDEC_MAIN + 0x10) = 0x00010100;
    REG32(VDEC_DMA + 0x110) = 0x800;
    REG32(VDEC_XFORM + 0x804) = 0x40;
    REG32(VDEC_DEBLK + 0x10)  = 0x10;
    REG32(VDEC_SUB  + 0x6C)   = 0x10001;

    REG32(VDEC_SUB + 0x20) = dma_addr;
    REG32(VDEC_SUB + 0x24) = DMA_WORK_SIZE;
    REG32(VDEC_SUB + 0x78) = work_addr;
    REG32(VDEC_SUB + 0x7C) = WORK_BUF_SIZE;
    REG32(VDEC_SUB + 0x80) = 0;

    /* Q tables loaded later from JPEG DQT */
    for (i = 0; i < 64; i++)
    {
        REG32(VDEC_XFORM + 0x200 + i * 4) = 16;
        REG32(VDEC_XFORM + 0x300 + i * 4) = 16;
    }
}

static int hw_mb_submit(uint32_t coeff_phys, uint32_t ref_addr,
                        uint32_t out_addr, int toggle, int is_chroma)
{
    int timeout, p;

    REG32(VDEC_SUB + 0x18) = coeff_phys;
    REG32(VDEC_SUB + 0x1C) = coeff_phys + COEFF_BUF_SIZE;
    REG32(VDEC_SUB + 0x0C) = 3;

    REG32(VDEC_SUB + 0x2C) = ref_addr;
    REG32(VDEC_SUB + 0x3C) = out_addr;

    timeout = 100000;
    while ((REG32(VDEC_DEBLK + 0x14) & 0x10000) && --timeout > 0) {}
    if (timeout == 0) return -1;
    REG32(VDEC_DEBLK + 0x0C) = ((uint32_t)toggle << 30) | 0x80;

    for (p = 0; p < 2; p++)
    {
        timeout = 100000;
        while ((XFORM_808 & 2) && --timeout > 0) {}
        if (timeout == 0) return -1;
        XFORM_800 = XFORM_CMD_BASE | ((uint32_t)is_chroma << 19);
        DMA_10C = ((uint32_t)is_chroma << 3) | 0x31;
    }
    return 0;
}

static void readback_luma(const uint8_t *src, uint8_t *frame,
                          int mb_col, int mb_row, int row_off, int stride)
{
    int row;
    for (row = 0; row < 8; row++)
    {
        const uint32_t *s = (const uint32_t *)(src + row * 32);
        uint32_t *d = (uint32_t *)(frame +
                      (mb_row * 16 + row_off + row) * stride +
                      mb_col * 16);
        d[0] = __builtin_bswap32(s[0]);
        d[1] = __builtin_bswap32(s[1]);
        d[2] = __builtin_bswap32(s[2]);
        d[3] = __builtin_bswap32(s[3]);
    }
}

static void readback_chroma(const uint8_t *src, uint8_t *cb, uint8_t *cr,
                            int mb_col, int mb_row, int cstride)
{
    int row;
    for (row = 0; row < 8; row++)
    {
        const uint32_t *s = (const uint32_t *)(src + row * 32);
        uint32_t *cb_d = (uint32_t *)(cb +
                         (mb_row * 8 + row) * cstride + mb_col * 8);
        uint32_t *cr_d = (uint32_t *)(cr +
                         (mb_row * 8 + row) * cstride + mb_col * 8);
        cb_d[0] = __builtin_bswap32(s[0]);
        cb_d[1] = __builtin_bswap32(s[1]);
        cr_d[0] = __builtin_bswap32(s[2]);
        cr_d[1] = __builtin_bswap32(s[3]);
    }
}

/* ================================================================
 * 3. JPEG decoder state
 * ================================================================ */

struct huff_table {
    uint8_t  bits[17];      /* bits[i] = count of i-bit codes */
    uint8_t  vals[256];
    int      maxcode[18];   /* max code value + 1 for each bit length */
    int      valptr[17];    /* index into vals[] for first code of length */
    uint8_t  look_sym[256]; /* 8-bit lookahead: symbol */
    uint8_t  look_len[256]; /* 8-bit lookahead: code length (0=need slow) */
};

struct jpeg_hw_state {
    /* Source data (in-memory) */
    const uint8_t *data;
    unsigned long  data_len;
    unsigned long  pos;

    /* Image parameters (from SOF0) */
    int width, height;
    int mb_w, mb_h;         /* dimensions in 16×16 MBs */

    /* Quantization tables (up to 4, zigzag order) */
    uint16_t qt[4][64];
    int qt_sel[3];          /* component → qt index */

    /* Huffman tables: 2 DC + 2 AC */
    struct huff_table dc_tab[2];
    struct huff_table ac_tab[2];
    int dc_sel[3], ac_sel[3]; /* component → table index */

    /* Restart interval */
    int restart_interval;

    /* Bitstream state */
    uint32_t bitbuf;
    int bits_left;

    /* DC prediction per component */
    int last_dc[3];
};

/* ================================================================
 * 4. Huffman table builder
 * ================================================================ */

static void build_huff_lut(struct huff_table *ht)
{
    int p, i, l, code, si;

    /* Generate code values from bit counts */
    code = 0;
    p = 0;
    for (l = 1; l <= 16; l++)
    {
        ht->valptr[l] = p;
        if (ht->bits[l])
        {
            ht->maxcode[l] = code + ht->bits[l];
        }
        else
        {
            ht->maxcode[l] = -1;
        }
        code += ht->bits[l];
        p += ht->bits[l];
        code <<= 1;
    }
    ht->maxcode[17] = 0x1FFFF;

    /* Build 8-bit lookahead table */
    memset(ht->look_len, 0, 256);

    p = 0;
    code = 0;
    for (l = 1; l <= 8; l++)
    {
        for (i = 0; i < ht->bits[l]; i++)
        {
            int lookbits = code << (8 - l);
            int fill = 1 << (8 - l);
            for (si = 0; si < fill; si++)
            {
                ht->look_sym[lookbits] = ht->vals[p];
                ht->look_len[lookbits] = (uint8_t)l;
                lookbits++;
            }
            p++;
            code++;
        }
        code <<= 1;
    }
}

/* ================================================================
 * 5. Bitstream reader
 * ================================================================ */

/* Fill bit buffer, handling JPEG byte stuffing (FF 00).
 * Stops at RST/EOI markers — caller handles restart logic. */
static void fill_bits(struct jpeg_hw_state *j)
{
    while (j->bits_left <= 24 && j->pos < j->data_len)
    {
        uint8_t b = j->data[j->pos++];
        if (b == 0xFF)
        {
            uint8_t next = (j->pos < j->data_len) ? j->data[j->pos] : 0;
            if (next == 0x00)
            {
                j->pos++; /* stuffed zero — keep 0xFF */
            }
            else
            {
                /* Any marker (RST, EOI, etc) — back up and stop */
                j->pos--;
                return;
            }
        }
        j->bitbuf = (j->bitbuf << 8) | b;
        j->bits_left += 8;
    }
}

static int get_bits(struct jpeg_hw_state *j, int n)
{
    if (j->bits_left < n)
        fill_bits(j);
    j->bits_left -= n;
    return (j->bitbuf >> j->bits_left) & ((1 << n) - 1);
}


/* ================================================================
 * 6. Huffman decoder
 * ================================================================ */

static int huff_decode(struct jpeg_hw_state *j, struct huff_table *ht)
{
    int look, nb, code, l;

    if (j->bits_left < 8)
        fill_bits(j);

    if (j->bits_left >= 8)
    {
        look = (j->bitbuf >> (j->bits_left - 8)) & 0xFF;
        nb = ht->look_len[look];
        if (nb)
        {
            j->bits_left -= nb;
            return ht->look_sym[look];
        }
    }

    /* Slow path for codes > 8 bits */
    code = 0;
    for (l = 1; l <= 16; l++)
    {
        code = (code << 1) | get_bits(j, 1);
        if (code < ht->maxcode[l])
            return ht->vals[ht->valptr[l] + code - (ht->maxcode[l] - ht->bits[l])];
    }
    return 0; /* error */
}

/* Extend sign of a Huffman-decoded magnitude */
static int huff_extend(int val, int bits)
{
    if (val < (1 << (bits - 1)))
        val += (-1 << bits) + 1;
    return val;
}

/* ================================================================
 * 7. Block decoder (one 8×8 block → 64 coefficients)
 * ================================================================ */

static void decode_block(struct jpeg_hw_state *j, int comp, int16_t *coeff)
{
    struct huff_table *dc_ht = &j->dc_tab[j->dc_sel[comp]];
    struct huff_table *ac_ht = &j->ac_tab[j->ac_sel[comp]];
    int s, r, k;

    memset(coeff, 0, 64 * sizeof(int16_t));

    /* DC coefficient (DPCM) */
    s = huff_decode(j, dc_ht);
    if (s > 0)
    {
        r = get_bits(j, s);
        j->last_dc[comp] += huff_extend(r, s);
    }
    coeff[0] = (int16_t)j->last_dc[comp];

    /* AC coefficients (run/size pairs) */
    k = 1;
    while (k < 64)
    {
        s = huff_decode(j, ac_ht);
        r = s >> 4;   /* run of zeros */
        s &= 0x0F;    /* coefficient size */

        if (s == 0)
        {
            if (r == 0)
                break;      /* EOB — rest are zero */
            if (r == 15)
            {
                k += 16;    /* ZRL — skip 16 zeros */
                continue;
            }
            break;
        }

        k += r;
        if (k >= 64) break;

        r = get_bits(j, s);
        coeff[k] = (int16_t)huff_extend(r, s);
        k++;
    }
}

/* ================================================================
 * 8. JPEG marker parser
 * ================================================================ */

static uint16_t read_be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

/* Parse all markers up to SOS.  Returns offset of entropy data, or 0. */
static unsigned long parse_markers(struct jpeg_hw_state *j)
{
    unsigned long p = 0;

    /* SOI */
    if (p + 2 > j->data_len || j->data[p] != 0xFF || j->data[p+1] != 0xD8)
        return 0;
    p += 2;

    while (p + 4 <= j->data_len)
    {
        if (j->data[p] != 0xFF)
            return 0;

        uint8_t marker = j->data[p + 1];
        p += 2;

        if (marker == 0xD9) /* EOI */
            return 0;
        if (marker == 0xDA) /* SOS — handle below */
            break;
        if (marker >= 0xD0 && marker <= 0xD7) /* RST0-RST7 only */
            continue;

        /* Marker with length */
        if (p + 2 > j->data_len) return 0;
        uint16_t len = read_be16(j->data + p);
        if (len < 2 || p + len > j->data_len) return 0;

        switch (marker)
        {
        case 0xC0: /* SOF0 — Baseline DCT */
        {
            if (len < 8) return 0;
            if (j->data[p + 2] != 8) return 0; /* 8-bit only */

            j->height = read_be16(j->data + p + 3);
            j->width  = read_be16(j->data + p + 5);
            int ncomp = j->data[p + 7];
            if (ncomp != 3) return 0; /* 4:2:0 needs 3 components */
            if (len < 8 + ncomp * 3) return 0;

            int c;
            for (c = 0; c < 3; c++)
            {
                int off = p + 8 + c * 3;
                /* id = j->data[off]; */
                int h_samp = j->data[off + 1] >> 4;
                int v_samp = j->data[off + 1] & 0xF;
                j->qt_sel[c] = j->data[off + 2];

                /* Enforce 4:2:0: Y=2×2, Cb=1×1, Cr=1×1 */
                if (c == 0 && (h_samp != 2 || v_samp != 2)) return 0;
                if (c > 0 && (h_samp != 1 || v_samp != 1)) return 0;
            }

            j->mb_w = (j->width + 15) / 16;
            j->mb_h = (j->height + 15) / 16;
            break;
        }

        case 0xC2: /* SOF2 — Progressive: not supported */
            return 0;

        case 0xDB: /* DQT */
        {
            unsigned long dp = p + 2;
            while (dp < p + len)
            {
                int info = j->data[dp++];
                int prec = info >> 4;  /* 0=8bit, 1=16bit */
                int id   = info & 0xF;
                if (id > 3) return 0;

                int i;
                for (i = 0; i < 64; i++)
                {
                    if (prec)
                    {
                        if (dp + 2 > p + len) return 0;
                        j->qt[id][i] = read_be16(j->data + dp);
                        dp += 2;
                    }
                    else
                    {
                        if (dp >= p + len) return 0;
                        j->qt[id][i] = j->data[dp++];
                    }
                }
            }
            break;
        }

        case 0xC4: /* DHT */
        {
            unsigned long dp = p + 2;
            while (dp < p + len)
            {
                int info = j->data[dp++];
                int cls  = info >> 4;  /* 0=DC, 1=AC */
                int id   = info & 0xF;
                if (id > 1) return 0;

                struct huff_table *ht = (cls == 0)
                    ? &j->dc_tab[id] : &j->ac_tab[id];

                int total = 0, i;
                ht->bits[0] = 0;
                for (i = 1; i <= 16; i++)
                {
                    if (dp >= p + len) return 0;
                    ht->bits[i] = j->data[dp++];
                    total += ht->bits[i];
                }

                if (dp + total > p + len || total > 256)
                    return 0;

                for (i = 0; i < total; i++)
                    ht->vals[i] = j->data[dp++];

                build_huff_lut(ht);
            }
            break;
        }

        case 0xDD: /* DRI */
            if (len >= 4)
                j->restart_interval = read_be16(j->data + p + 2);
            break;

        default: /* APPn, COM, etc — skip */
            break;
        }

        p += len;
    }

    /* Parse SOS header */
    if (p + 2 > j->data_len) return 0;
    {
        uint16_t sos_len = read_be16(j->data + p);
        if (sos_len < 6 || p + sos_len > j->data_len) return 0;

        int ncomp = j->data[p + 2];
        if (ncomp != 3) return 0;

        int c;
        for (c = 0; c < ncomp; c++)
        {
            int off = p + 3 + c * 2;
            /* comp_id = j->data[off]; */
            j->dc_sel[c] = j->data[off + 1] >> 4;
            j->ac_sel[c] = j->data[off + 1] & 0xF;
        }

        return p + sos_len; /* offset of entropy-coded data */
    }
}

/* ================================================================
 * 9. MCU decode + VPU-A submission
 * ================================================================ */

/* Pack two 8×8 blocks into 512-byte coefficient buffer (big-endian).
 * Coefficients stay in zigzag order — VPU-A un-zigzags internally.
 * Confirmed by v35d round-trip test and Apple FW analysis. */
static void pack_coeff_pair(uint32_t *buf, const int16_t *b0,
                            const int16_t *b1)
{
    int i;
    for (i = 0; i < 64; i++)
        buf[i] = __builtin_bswap32((uint32_t)(int32_t)b0[i]);
    for (i = 0; i < 64; i++)
        buf[64 + i] = __builtin_bswap32((uint32_t)(int32_t)b1[i]);
}

/* Decode all MCUs and run through VPU-A IDCT.
 * Returns 0 on success, -1 on error. */
static int decode_scan(struct jpeg_hw_state *j,
                       uint32_t coeff_phys, uint32_t sa_phys,
                       uint32_t sb_phys, uint8_t *small_a,
                       uint8_t *small_b, uint32_t *coeff_buf,
                       uint8_t *frame_y, uint8_t *frame_cb,
                       uint8_t *frame_cr)
{
    int mb_col, mb_row, toggle = 0;
    int restart_count = 0;
    int y_stride = j->mb_w * 16;
    int c_stride = j->mb_w * 8;
    int16_t blocks[6][64]; /* Y0,Y1,Y2,Y3,Cb,Cr */
    uint8_t *active;

    /* Uncacheable aliases for CPU access to DMA buffers.
     * VPU-A DMA uses physical addresses (0x08xxxxxx) via sa_phys/sb_phys/coeff_phys.
     * CPU reads/writes through uncacheable VA (0x48xxxxxx) to bypass D-cache entirely.
     * Eliminates all commit_dcache()/commit_discard_dcache() calls (~61% of decode time).
     * Apple uses the same approach (0x8xxxxxxx uncacheable aliases in OF). */
    uint32_t *coeff_uc = S5L8702_UNCACHED_ADDR(coeff_buf);
    uint8_t *small_a_uc = S5L8702_UNCACHED_ADDR(small_a);
    uint8_t *small_b_uc = S5L8702_UNCACHED_ADDR(small_b);

    for (mb_row = 0; mb_row < j->mb_h; mb_row++)
    {
        for (mb_col = 0; mb_col < j->mb_w; mb_col++)
        {
            /* Handle restart markers */
            if (j->restart_interval > 0 && restart_count > 0
                && restart_count % j->restart_interval == 0)
            {
                j->last_dc[0] = j->last_dc[1] = j->last_dc[2] = 0;
                j->bits_left = 0;
                j->bitbuf = 0;
                /* Skip to next byte-aligned position after RST marker */
                while (j->pos < j->data_len - 1)
                {
                    if (j->data[j->pos] == 0xFF
                        && j->data[j->pos + 1] >= 0xD0
                        && j->data[j->pos + 1] <= 0xD7)
                    {
                        j->pos += 2;
                        break;
                    }
                    j->pos++;
                }
            }

            /* Decode 6 blocks: Y0 Y1 Y2 Y3 Cb Cr */
            decode_block(j, 0, blocks[0]); /* Y0 top-left */
            decode_block(j, 0, blocks[1]); /* Y1 top-right */
            decode_block(j, 0, blocks[2]); /* Y2 bottom-left */
            decode_block(j, 0, blocks[3]); /* Y3 bottom-right */
            decode_block(j, 1, blocks[4]); /* Cb */
            decode_block(j, 2, blocks[5]); /* Cr */

            /* Sub-call 1: Y-top (Y0, Y1) */
            active = (toggle == 0) ? small_a_uc : small_b_uc;
            pack_coeff_pair(coeff_uc, blocks[0], blocks[1]);
            if (hw_mb_submit(coeff_phys, sa_phys, sb_phys,
                             toggle, 0) < 0)
                return -1;
            readback_luma(active, frame_y, mb_col, mb_row, 0, y_stride);
            toggle ^= 1;

            /* Sub-call 2: Y-bottom (Y2, Y3) */
            active = (toggle == 0) ? small_a_uc : small_b_uc;
            pack_coeff_pair(coeff_uc, blocks[2], blocks[3]);
            if (hw_mb_submit(coeff_phys, sa_phys, sb_phys,
                             toggle, 0) < 0)
                return -1;
            readback_luma(active, frame_y, mb_col, mb_row, 8, y_stride);
            toggle ^= 1;

            /* Sub-call 3: Chroma (Cb, Cr) */
            active = (toggle == 0) ? small_a_uc : small_b_uc;
            pack_coeff_pair(coeff_uc, blocks[4], blocks[5]);
            if (hw_mb_submit(coeff_phys, sa_phys, sb_phys,
                             toggle, 1) < 0)
                return -1;
            readback_chroma(active, frame_cb, frame_cr,
                            mb_col, mb_row, c_stride);
            toggle ^= 1;

            restart_count++;
        }
    }

    /* Deblock flush (extra submission to push pipeline) */
    {
        int16_t zeros[64];
        memset(zeros, 0, sizeof(zeros));
        pack_coeff_pair(coeff_uc, zeros, zeros);
        hw_mb_submit(coeff_phys, sa_phys, sb_phys, toggle, 0);
    }

    return 0;
}

/* ================================================================
 * 10. YCbCr → RGB565 with downscale + letterbox
 * ================================================================ */

static int clamp8(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

static void ycbcr_to_rgb565_scaled(const uint8_t *fy, const uint8_t *fcb,
                                   const uint8_t *fcr,
                                   int src_w, int src_h,
                                   fb_data *out, int out_w, int out_h)
{
    int dst_w, dst_h, pad_x, pad_y;
    int x, y, cw;

    /* Letterbox: fit source aspect into output dimensions */
    dst_w = out_w;
    dst_h = (src_h * out_w) / src_w;
    if (dst_h > out_h)
    {
        dst_h = out_h;
        dst_w = (src_w * out_h) / src_h;
    }
    pad_x = (out_w - dst_w) / 2;
    pad_y = (out_h - dst_h) / 2;
    cw = src_w / 2;

    memset(out, 0, out_w * out_h * sizeof(fb_data));

    /* Precompute source coordinate LUTs to avoid per-pixel divides
     * (ARM926 has no HW divide — ~50 cycles each via libgcc) */
    int sy_lut[dst_h], sx_lut[dst_w];
    for (y = 0; y < dst_h; y++)
    {
        sy_lut[y] = y * src_h / dst_h;
        if (sy_lut[y] >= src_h) sy_lut[y] = src_h - 1;
    }
    for (x = 0; x < dst_w; x++)
    {
        sx_lut[x] = x * src_w / dst_w;
        if (sx_lut[x] >= src_w) sx_lut[x] = src_w - 1;
    }

    for (y = 0; y < dst_h; y++)
    {
        int sy = sy_lut[y];
        const uint8_t *y_row = fy + sy * src_w;
        const uint8_t *cb_row = fcb + (sy / 2) * cw;
        const uint8_t *cr_row = fcr + (sy / 2) * cw;
        fb_data *dst_row = out + (y + pad_y) * out_w + pad_x;

        for (x = 0; x < dst_w; x++)
        {
            int sx = sx_lut[x];
            uint8_t yv  = y_row[sx];
            uint8_t cbv = cb_row[sx / 2];
            uint8_t crv = cr_row[sx / 2];

            int r = clamp8(yv + (((int)crv - 128) * 359 >> 8));
            int g = clamp8(yv - (((int)cbv - 128) * 88 >> 8)
                              - (((int)crv - 128) * 183 >> 8));
            int b = clamp8(yv + (((int)cbv - 128) * 454 >> 8));

            dst_row[x] =
                (fb_data)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
}

/* ================================================================
 * 11. Top-level decode function
 * ================================================================ */

bool jpeg_hw_decode_fd(int fd, unsigned long jpeg_size,
                       fb_data *out, int out_w, int out_h,
                       void *work, size_t work_size)
{
    struct jpeg_hw_state js;
    unsigned long entropy_off;
    bool result = false;

    /* Read JPEG data into work buffer */
    if (jpeg_size > work_size || jpeg_size < 20)
        return false;
    if (read(fd, work, jpeg_size) < (long)jpeg_size)
        return false;

    /* Initialize parser state */
    memset(&js, 0, sizeof(js));
    js.data = (const uint8_t *)work;
    js.data_len = jpeg_size;

    /* Parse markers */
    entropy_off = parse_markers(&js);
    if (entropy_off == 0 || js.width == 0 || js.height == 0)
        return false;

    /* Set bitstream position to entropy data */
    js.pos = entropy_off;
    js.bitbuf = 0;
    js.bits_left = 0;

    /* Use static frame buffer for VPU-A working memory.
     * Avoids core_alloc() which would shrink the audio buffer. */
    {
        int frame_w = js.mb_w * 16;
        int frame_h = js.mb_h * 16;
        size_t y_size  = frame_w * frame_h;
        size_t c_size  = (frame_w / 2) * (frame_h / 2);
        size_t frame_need = y_size + c_size * 2;
        size_t fixed_overhead = DMA_WORK_SIZE + 32 + WORK_BUF_SIZE + 32
            + SMALL_BUF_SIZE + 4096 + SMALL_BUF_SIZE + 4096
            + COEFF_BUF_SIZE + 32 + 4096 + 64;

        if (frame_need + fixed_overhead > VPU_FRAME_BUF_SIZE)
            return false;
    }

    {
        uint8_t *p = (uint8_t *)ALIGN32(vpu_frame_buf);
        uint8_t *dma_work, *work_buf1, *small_a, *small_b;
        uint8_t *coeff_mem, *frame_y, *frame_cb, *frame_cr;
        uint32_t *coeff_buf;
        int frame_w = js.mb_w * 16;
        int frame_h = js.mb_h * 16;
        size_t y_size  = frame_w * frame_h;
        size_t c_size  = (frame_w / 2) * (frame_h / 2);
        int i;

        dma_work  = p;                        p += DMA_WORK_SIZE;
        work_buf1 = (uint8_t *)ALIGN32(p);    p = work_buf1 + WORK_BUF_SIZE;
        small_a   = (uint8_t *)ALIGN4K(p);    p = small_a + SMALL_BUF_SIZE;
        small_b   = (uint8_t *)ALIGN4K(p);    p = small_b + SMALL_BUF_SIZE;
        coeff_mem = (uint8_t *)ALIGN32(p);    p = coeff_mem + COEFF_BUF_SIZE;
        frame_y   = (uint8_t *)ALIGN4K(p);    p = frame_y + y_size;
        frame_cb  = (uint8_t *)ALIGN32(p);    p = frame_cb + c_size;
        frame_cr  = (uint8_t *)ALIGN32(p);

        coeff_buf = (uint32_t *)(void *)coeff_mem;

        memset(dma_work, 0, DMA_WORK_SIZE);
        memset(work_buf1, 0, WORK_BUF_SIZE);
        memset(small_a, 0, SMALL_BUF_SIZE);
        memset(small_b, 0, SMALL_BUF_SIZE);
        memset(frame_y, 0, y_size);
        memset(frame_cb, 0x80, c_size);
        memset(frame_cr, 0x80, c_size);

        /* Power on VPU-A */
        vpua_power_on();

        /* Reset + init */
        REG32(VDEC_MAIN + 0x2C) = 2;
        REG32(VDEC_MAIN + 0x1C) = 0xFFFFFFFF;
        REG32(VDEC_MAIN + 0x0C) = 0;

        vpua_jpeg_init((uint32_t)(uintptr_t)dma_work,
                       (uint32_t)(uintptr_t)work_buf1);

        /* Load JPEG Q tables into XFORM scaling matrices.
         * DQT stores Q values in zigzag order; XFORM registers are
         * indexed by raster position.  De-zigzag during load
         * (matches Apple FUN_000df908). */
        for (i = 0; i < 64; i++)
        {
            REG32(VDEC_XFORM + 0x200 + zz_to_raster[i] * 4) =
                js.qt[js.qt_sel[0]][i];
            REG32(VDEC_XFORM + 0x300 + zz_to_raster[i] * 4) =
                js.qt[js.qt_sel[1]][i];
        }

        commit_dcache();
        commit_discard_dcache();

        /* Decode all MCUs through VPU-A IDCT */
        if (decode_scan(&js,
                        (uint32_t)(uintptr_t)coeff_mem,
                        (uint32_t)(uintptr_t)small_a,
                        (uint32_t)(uintptr_t)small_b,
                        small_a, small_b, coeff_buf,
                        frame_y, frame_cb, frame_cr) == 0)
        {
            /* Convert YCbCr → RGB565 with downscale + letterbox */
            ycbcr_to_rgb565_scaled(frame_y, frame_cb, frame_cr,
                                   js.width, js.height,
                                   out, out_w, out_h);
            result = true;
        }

        vpua_power_off();
    }

    return result;
}
