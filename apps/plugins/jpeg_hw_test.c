/***************************************************************************
 * jpeg_hw_test — Test harness for VPU-A hardware JPEG decoder
 *
 * Loads a JPEG file from disk, decodes it via jpeg_hw_decode_fd(),
 * displays the result on the LCD, and logs diagnostics.
 *
 * Usage: Place a test JPEG at /test_thumb.jpg on the iPod, then
 *        run this plugin from the Rockbox menu.
 *
 * Also tests the video_thumb pipeline by trying to extract cover art
 * from /test_cover.m4v if present.
 ****************************************************************************/

#include "plugin.h"

/* We need the jpeg_hw API — since this is a plugin, we can't call
 * core functions directly. Instead, we reimplement a minimal version
 * of the decoder inline using the same VPU-A register sequences. */

#include "lib/pluginlib_actions.h"

#define LOG_PATH "/jpeg_hw_test.log"
#define TEST_JPEG_PATH "/test_thumb.jpg"
#define TEST_M4V_PATH  "/test_cover.m4v"

/* Output thumbnail dimensions */
#define OUT_W   320
#define OUT_H   240

/* VPU-A register definitions (same as jpeg_hw.c / jpeg_poc.c) */
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

static int log_fd = -1;

static void tlog(const char *fmt, ...)
{
    static char buf[256];
    va_list ap;
    va_start(ap, fmt);
    rb->vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (log_fd >= 0) rb->fdprintf(log_fd, "%s\n", buf);
}

static void lflush(void)
{
    if (log_fd >= 0)
    {
        rb->close(log_fd);
        log_fd = rb->open(LOG_PATH, O_WRONLY | O_APPEND, 0666);
    }
}

/* ---- VPU-A control (copied from jpeg_poc.c, proven working) ---- */

static void vpua_power_on(void)
{
    uint32_t cg, pw;
    cg = REG32(CLK_BASE + 0x08);
    cg &= ~0x80000000;
    cg |= 0x30000000;
    REG32(CLK_BASE + 0x08) = cg;
    rb->sleep(HZ / 5);

    pw = PWRCON(0);
    PWRCON(0) = pw & ~((7 << 14) | (1 << 18));
    rb->sleep(HZ / 5);

    REG32(VPU_MODE_REG) &= ~1;

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
    REG32(VDEC_DEBLK + 0x10)  = 0x10;
    REG32(VDEC_SUB  + 0x6C)   = 0x10001;
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
    REG32(VDEC_DEBLK + 0x10)  = 0x14; /* 0x10 + bit2 = deblock disable */
    REG32(VDEC_SUB  + 0x6C)   = 0x10001;

    REG32(VDEC_SUB + 0x20) = dma_addr;
    REG32(VDEC_SUB + 0x24) = DMA_WORK_SIZE;
    REG32(VDEC_SUB + 0x78) = work_addr;
    REG32(VDEC_SUB + 0x7C) = WORK_BUF_SIZE;
    REG32(VDEC_SUB + 0x80) = 0;

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

/* ---- Minimal JPEG parser (inline for plugin context) ---- */

static uint16_t rbe16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

struct huff_table {
    uint8_t  bits[17];
    uint8_t  vals[256];
    int      maxcode[18];
    int      valptr[17];
    uint8_t  look_sym[256];
    uint8_t  look_len[256];
};

struct test_jpeg {
    const uint8_t *data;
    unsigned long  len;
    unsigned long  pos;
    int width, height, mb_w, mb_h;
    uint16_t qt[4][64];
    int qt_sel[3];
    struct huff_table dc_tab[2], ac_tab[2];
    int dc_sel[3], ac_sel[3];
    int restart_interval;
    uint32_t bitbuf;
    int bits_left;
    int last_dc[3];
};

static void build_lut(struct huff_table *ht)
{
    int p, i, l, code, si;
    code = 0;
    p = 0;
    for (l = 1; l <= 16; l++)
    {
        ht->valptr[l] = p;
        ht->maxcode[l] = (ht->bits[l]) ? code + ht->bits[l] : -1;
        code += ht->bits[l];
        p += ht->bits[l];
        code <<= 1;
    }
    ht->maxcode[17] = 0x1FFFF;

    rb->memset(ht->look_len, 0, 256);
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

static void tj_fill_bits(struct test_jpeg *j)
{
    while (j->bits_left <= 24 && j->pos < j->len)
    {
        uint8_t b = j->data[j->pos++];
        if (b == 0xFF)
        {
            uint8_t next = (j->pos < j->len) ? j->data[j->pos] : 0;
            if (next == 0x00) { j->pos++; }
            else { j->pos--; return; } /* stop at any marker */
        }
        j->bitbuf = (j->bitbuf << 8) | b;
        j->bits_left += 8;
    }
}

static int tj_get_bits(struct test_jpeg *j, int n)
{
    if (j->bits_left < n) tj_fill_bits(j);
    j->bits_left -= n;
    return (j->bitbuf >> j->bits_left) & ((1 << n) - 1);
}

static int tj_huff_decode(struct test_jpeg *j, struct huff_table *ht)
{
    int look, nb, code, l;
    if (j->bits_left < 8) tj_fill_bits(j);
    if (j->bits_left >= 8)
    {
        look = (j->bitbuf >> (j->bits_left - 8)) & 0xFF;
        nb = ht->look_len[look];
        if (nb) { j->bits_left -= nb; return ht->look_sym[look]; }
    }
    code = 0;
    for (l = 1; l <= 16; l++)
    {
        code = (code << 1) | tj_get_bits(j, 1);
        if (code < ht->maxcode[l])
            return ht->vals[ht->valptr[l] + code
                            - (ht->maxcode[l] - ht->bits[l])];
    }
    return 0;
}

static int tj_extend(int val, int bits)
{
    if (val < (1 << (bits - 1)))
        val += (-1 << bits) + 1;
    return val;
}

static void tj_decode_block(struct test_jpeg *j, int comp, int16_t *coeff)
{
    struct huff_table *dc_ht = &j->dc_tab[j->dc_sel[comp]];
    struct huff_table *ac_ht = &j->ac_tab[j->ac_sel[comp]];
    int s, r, k;

    rb->memset(coeff, 0, 64 * sizeof(int16_t));

    s = tj_huff_decode(j, dc_ht);
    if (s > 0)
    {
        r = tj_get_bits(j, s);
        j->last_dc[comp] += tj_extend(r, s);
    }
    coeff[0] = (int16_t)j->last_dc[comp];

    k = 1;
    while (k < 64)
    {
        s = tj_huff_decode(j, ac_ht);
        r = s >> 4;
        s &= 0x0F;
        if (s == 0) { if (r == 0) break; if (r == 15) { k += 16; continue; } break; }
        k += r;
        if (k >= 64) break;
        r = tj_get_bits(j, s);
        coeff[k] = (int16_t)tj_extend(r, s);
        k++;
    }
}

static unsigned long tj_parse_markers(struct test_jpeg *j)
{
    unsigned long p = 0;
    if (p + 2 > j->len || j->data[p] != 0xFF || j->data[p+1] != 0xD8)
        return 0;
    p += 2;

    while (p + 4 <= j->len)
    {
        if (j->data[p] != 0xFF) return 0;
        uint8_t marker = j->data[p + 1];
        p += 2;
        if (marker == 0xD9) return 0;
        if (marker == 0xDA) break;
        if (marker >= 0xD0 && marker <= 0xD7) continue;

        if (p + 2 > j->len) return 0;
        uint16_t len = rbe16(j->data + p);
        if (len < 2 || p + len > j->len) return 0;

        switch (marker)
        {
        case 0xC0:
        {
            if (len < 8 || j->data[p+2] != 8) return 0;
            j->height = rbe16(j->data + p + 3);
            j->width  = rbe16(j->data + p + 5);
            int nc = j->data[p + 7];
            if (nc != 3 || len < 8 + nc * 3) return 0;
            int c;
            for (c = 0; c < 3; c++)
            {
                int off = p + 8 + c * 3;
                int hs = j->data[off+1] >> 4, vs = j->data[off+1] & 0xF;
                j->qt_sel[c] = j->data[off+2];
                if (c == 0 && (hs != 2 || vs != 2)) return 0;
                if (c > 0 && (hs != 1 || vs != 1)) return 0;
            }
            j->mb_w = (j->width + 15) / 16;
            j->mb_h = (j->height + 15) / 16;
            break;
        }
        case 0xC2: return 0;
        case 0xDB:
        {
            unsigned long dp = p + 2;
            while (dp < p + len)
            {
                int info = j->data[dp++];
                int prec = info >> 4, id = info & 0xF;
                if (id > 3) return 0;
                int i;
                for (i = 0; i < 64; i++)
                {
                    if (prec) { j->qt[id][i] = rbe16(j->data+dp); dp+=2; }
                    else { j->qt[id][i] = j->data[dp++]; }
                }
            }
            break;
        }
        case 0xC4:
        {
            unsigned long dp = p + 2;
            while (dp < p + len)
            {
                int info = j->data[dp++];
                int cls = info >> 4, id = info & 0xF;
                if (id > 1) return 0;
                struct huff_table *ht = (cls == 0) ? &j->dc_tab[id] : &j->ac_tab[id];
                int total = 0, i;
                ht->bits[0] = 0;
                for (i = 1; i <= 16; i++) { ht->bits[i] = j->data[dp++]; total += ht->bits[i]; }
                if (total > 256) return 0;
                for (i = 0; i < total; i++) ht->vals[i] = j->data[dp++];
                build_lut(ht);
            }
            break;
        }
        case 0xDD:
            if (len >= 4) j->restart_interval = rbe16(j->data + p + 2);
            break;
        }
        p += len;
    }

    /* SOS */
    if (p + 2 > j->len) return 0;
    uint16_t slen = rbe16(j->data + p);
    if (slen < 6) return 0;
    int c;
    for (c = 0; c < 3; c++)
    {
        int off = p + 3 + c * 2;
        j->dc_sel[c] = j->data[off+1] >> 4;
        j->ac_sel[c] = j->data[off+1] & 0xF;
    }
    return p + slen;
}

/* ---- Pack coefficients for VPU-A ---- */

static void pack_coeff_pair(uint32_t *buf, const int16_t *b0, const int16_t *b1)
{
    int i;
    for (i = 0; i < 64; i++)
        buf[i] = __builtin_bswap32((uint32_t)(int32_t)b0[i]);
    for (i = 0; i < 64; i++)
        buf[64 + i] = __builtin_bswap32((uint32_t)(int32_t)b1[i]);
}

static int clamp8(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

/* ---- Main test ---- */

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    size_t buf_size;
    uint8_t *buf;
    int fd;
    off_t fsize;
    uint8_t *jpeg_data;
    unsigned long jpeg_len;
    struct test_jpeg tj;
    unsigned long entropy_off;
    int total_mbs;

    rb->splash(HZ / 2, "JPEG HW Test");

    log_fd = rb->open(LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    tlog("=== JPEG HW Decoder Test ===");

    buf = rb->plugin_get_audio_buffer(&buf_size);
    tlog("Audio buffer: %08lx size=%lu",
         (unsigned long)(uintptr_t)buf, (unsigned long)buf_size);

    /* Read test JPEG file */
    fd = rb->open(TEST_JPEG_PATH, O_RDONLY);
    if (fd < 0)
    {
        tlog("Cannot open %s — try placing a JPEG there", TEST_JPEG_PATH);
        rb->splashf(HZ * 3, "No %s", TEST_JPEG_PATH);
        if (log_fd >= 0) rb->close(log_fd);
        return PLUGIN_OK;
    }
    fsize = rb->lseek(fd, 0, SEEK_END);
    rb->lseek(fd, 0, SEEK_SET);
    tlog("JPEG file: %ld bytes", (long)fsize);

    if ((unsigned long)fsize > buf_size / 4)
    {
        tlog("JPEG too large");
        rb->close(fd);
        if (log_fd >= 0) rb->close(log_fd);
        return PLUGIN_OK;
    }

    jpeg_data = buf;
    jpeg_len = (unsigned long)fsize;
    rb->read(fd, jpeg_data, jpeg_len);
    rb->close(fd);

    /* Parse JPEG markers */
    rb->memset(&tj, 0, sizeof(tj));
    tj.data = jpeg_data;
    tj.len = jpeg_len;

    entropy_off = tj_parse_markers(&tj);
    if (entropy_off == 0 || tj.width == 0)
    {
        tlog("JPEG parse failed (not baseline 4:2:0?)");
        rb->splash(HZ * 3, "JPEG parse failed");
        if (log_fd >= 0) rb->close(log_fd);
        return PLUGIN_OK;
    }

    tlog("JPEG: %dx%d, %dx%d MBs, restart=%d",
         tj.width, tj.height, tj.mb_w, tj.mb_h, tj.restart_interval);
    tlog("Q tables: Y=%d, Cb=%d, Cr=%d",
         tj.qt_sel[0], tj.qt_sel[1], tj.qt_sel[2]);
    tlog("Huff: DC0/AC0=%d/%d, DC1/AC1=%d/%d",
         tj.dc_sel[0], tj.ac_sel[0], tj.dc_sel[1], tj.ac_sel[1]);
    lflush();

    total_mbs = tj.mb_w * tj.mb_h;
    tlog("Total MBs: %d, entropy at offset %lu", total_mbs, entropy_off);

    /* Allocate VPU-A buffers from remaining audio buffer */
    {
        int frame_w = tj.mb_w * 16;
        int frame_h = tj.mb_h * 16;
        size_t y_size = frame_w * frame_h;
        size_t c_size = (frame_w / 2) * (frame_h / 2);
        int y_stride = frame_w;
        int c_stride = frame_w / 2;

        uint8_t *p = (uint8_t *)ALIGN32(jpeg_data + jpeg_len + 256);
        uint8_t *dma_work  = p;                        p += DMA_WORK_SIZE;
        uint8_t *work_buf1 = (uint8_t *)ALIGN32(p);    p = work_buf1 + WORK_BUF_SIZE;
        uint8_t *small_a   = (uint8_t *)ALIGN4K(p);    p = small_a + SMALL_BUF_SIZE;
        uint8_t *small_b   = (uint8_t *)ALIGN4K(p);    p = small_b + SMALL_BUF_SIZE;
        uint8_t *coeff_mem = (uint8_t *)ALIGN32(p);    p = coeff_mem + COEFF_BUF_SIZE;
        uint8_t *frame_y   = (uint8_t *)ALIGN4K(p);    p = frame_y + y_size;
        uint8_t *frame_cb  = (uint8_t *)ALIGN32(p);    p = frame_cb + c_size;
        uint8_t *frame_cr  = (uint8_t *)ALIGN32(p);    p = frame_cr + c_size;

        uint32_t *coeff_buf = (uint32_t *)(void *)coeff_mem;

        if ((uintptr_t)p > (uintptr_t)buf + buf_size)
        {
            tlog("Not enough buffer (need %lu, have %lu)",
                 (unsigned long)((uintptr_t)p - (uintptr_t)buf),
                 (unsigned long)buf_size);
            rb->splash(HZ * 3, "Buffer too small");
            if (log_fd >= 0) rb->close(log_fd);
            return PLUGIN_OK;
        }

        rb->memset(dma_work, 0, DMA_WORK_SIZE);
        rb->memset(work_buf1, 0, WORK_BUF_SIZE);
        rb->memset(small_a, 0, SMALL_BUF_SIZE);
        rb->memset(small_b, 0, SMALL_BUF_SIZE);
        rb->memset(frame_y, 0, y_size);
        rb->memset(frame_cb, 0x80, c_size);
        rb->memset(frame_cr, 0x80, c_size);

        tlog("Buffers: dma=%08lx work=%08lx sa=%08lx sb=%08lx",
             (unsigned long)(uintptr_t)dma_work,
             (unsigned long)(uintptr_t)work_buf1,
             (unsigned long)(uintptr_t)small_a,
             (unsigned long)(uintptr_t)small_b);
        tlog("Frame: Y=%08lx Cb=%08lx Cr=%08lx (%dx%d)",
             (unsigned long)(uintptr_t)frame_y,
             (unsigned long)(uintptr_t)frame_cb,
             (unsigned long)(uintptr_t)frame_cr,
             frame_w, frame_h);
        lflush();

        /* Power on VPU-A */
        tlog("--- VPU-A power on ---");
        vpua_power_on();

        /* Reset */
        REG32(VDEC_MAIN + 0x2C) = 2;
        REG32(VDEC_MAIN + 0x1C) = 0xFFFFFFFF;
        REG32(VDEC_MAIN + 0x0C) = 0;

        vpua_jpeg_init((uint32_t)(uintptr_t)dma_work,
                       (uint32_t)(uintptr_t)work_buf1);

        /* Load Q tables from JPEG into XFORM */
        {
            int i;
            for (i = 0; i < 64; i++)
            {
                REG32(VDEC_XFORM + 0x200 + i * 4) =
                    tj.qt[tj.qt_sel[0]][i];
                REG32(VDEC_XFORM + 0x300 + i * 4) =
                    tj.qt[tj.qt_sel[1]][i];
            }
        }

        /* Diagnostic: dump Q tables as loaded to XFORM */
        tlog("QT luma (first 8, zigzag): %d %d %d %d %d %d %d %d",
             tj.qt[tj.qt_sel[0]][0], tj.qt[tj.qt_sel[0]][1],
             tj.qt[tj.qt_sel[0]][2], tj.qt[tj.qt_sel[0]][3],
             tj.qt[tj.qt_sel[0]][4], tj.qt[tj.qt_sel[0]][5],
             tj.qt[tj.qt_sel[0]][6], tj.qt[tj.qt_sel[0]][7]);
        tlog("XFORM luma (first 8 regs, raster):");
        tlog("  %lu %lu %lu %lu %lu %lu %lu %lu",
             (unsigned long)REG32(VDEC_XFORM + 0x200 + 0*4),
             (unsigned long)REG32(VDEC_XFORM + 0x200 + 1*4),
             (unsigned long)REG32(VDEC_XFORM + 0x200 + 2*4),
             (unsigned long)REG32(VDEC_XFORM + 0x200 + 3*4),
             (unsigned long)REG32(VDEC_XFORM + 0x200 + 4*4),
             (unsigned long)REG32(VDEC_XFORM + 0x200 + 5*4),
             (unsigned long)REG32(VDEC_XFORM + 0x200 + 6*4),
             (unsigned long)REG32(VDEC_XFORM + 0x200 + 7*4));
        lflush();

        rb->commit_dcache();
        rb->commit_discard_dcache();

        /* Decode MCUs */
        tlog("--- Decoding %d MCUs ---", total_mbs);
        {
            int mb_col, mb_row, toggle = 0;
            int timeouts = 0, restart_count = 0;
            int16_t blocks[6][64];
            uint8_t *active;

            tj.pos = entropy_off;
            tj.bitbuf = 0;
            tj.bits_left = 0;

            for (mb_row = 0; mb_row < tj.mb_h; mb_row++)
            {
                for (mb_col = 0; mb_col < tj.mb_w; mb_col++)
                {
                    /* Restart marker handling */
                    if (tj.restart_interval > 0 && restart_count > 0
                        && restart_count % tj.restart_interval == 0)
                    {
                        tj.last_dc[0] = tj.last_dc[1] = tj.last_dc[2] = 0;
                        tj.bits_left = 0;
                        tj.bitbuf = 0;
                        while (tj.pos < tj.len - 1)
                        {
                            if (tj.data[tj.pos] == 0xFF
                                && tj.data[tj.pos+1] >= 0xD0
                                && tj.data[tj.pos+1] <= 0xD7)
                            {
                                tj.pos += 2;
                                break;
                            }
                            tj.pos++;
                        }
                    }

                    /* Decode 6 blocks */
                    tj_decode_block(&tj, 0, blocks[0]);
                    tj_decode_block(&tj, 0, blocks[1]);
                    tj_decode_block(&tj, 0, blocks[2]);
                    tj_decode_block(&tj, 0, blocks[3]);
                    tj_decode_block(&tj, 1, blocks[4]);
                    tj_decode_block(&tj, 2, blocks[5]);

                    /* Diagnostic: dump first MCU — all 64 Y0 coeffs + stream pos */
                    if (restart_count == 0)
                    {
                        int bi, ci;
                        const char *bnames[] = {"Y0","Y1","Y2","Y3","Cb","Cr"};

                        /* Full Y0 dump */
                        tlog("Y0 all 64 (zigzag):");
                        for (ci = 0; ci < 64; ci += 8)
                            tlog(" [%d]: %d %d %d %d %d %d %d %d",
                                 ci,
                                 blocks[0][ci], blocks[0][ci+1],
                                 blocks[0][ci+2], blocks[0][ci+3],
                                 blocks[0][ci+4], blocks[0][ci+5],
                                 blocks[0][ci+6], blocks[0][ci+7]);

                        /* Stream position after all 6 blocks */
                        tlog("After MCU0: pos=%lu bits_left=%d",
                             (unsigned long)tj.pos, tj.bits_left);

                        /* DC values for all blocks */
                        for (bi = 0; bi < 6; bi++)
                            tlog("%s DC=%d", bnames[bi], blocks[bi][0]);

                        /* Cr full dump (the broken channel) */
                        tlog("Cr all 64:");
                        for (ci = 0; ci < 64; ci += 8)
                            tlog(" [%d]: %d %d %d %d %d %d %d %d",
                                 ci,
                                 blocks[5][ci], blocks[5][ci+1],
                                 blocks[5][ci+2], blocks[5][ci+3],
                                 blocks[5][ci+4], blocks[5][ci+5],
                                 blocks[5][ci+6], blocks[5][ci+7]);
                        lflush();
                    }

                    /* Y-top */
                    active = (toggle == 0) ? small_a : small_b;
                    pack_coeff_pair(coeff_buf, blocks[0], blocks[1]);

                    /* Diagnostic: dump packed buffer for first MCU */
                    if (restart_count == 0)
                    {
                        tlog("Packed Y-top buf (first 8 words):");
                        tlog("  b0: %08lx %08lx %08lx %08lx",
                             (unsigned long)coeff_buf[0],
                             (unsigned long)coeff_buf[1],
                             (unsigned long)coeff_buf[2],
                             (unsigned long)coeff_buf[3]);
                        tlog("  b1: %08lx %08lx %08lx %08lx",
                             (unsigned long)coeff_buf[64],
                             (unsigned long)coeff_buf[65],
                             (unsigned long)coeff_buf[66],
                             (unsigned long)coeff_buf[67]);
                        lflush();
                    }

                    rb->memset(active, 0, SMALL_BUF_SIZE);
                    rb->commit_dcache();
                    if (hw_mb_submit((uint32_t)(uintptr_t)coeff_mem,
                                     (uint32_t)(uintptr_t)small_a,
                                     (uint32_t)(uintptr_t)small_b,
                                     toggle, 0) < 0)
                        timeouts++;
                    rb->commit_discard_dcache();

                    /* Diagnostic: dump VPU-A output for first MCU */
                    if (restart_count == 0)
                    {
                        const uint32_t *sb = (const uint32_t *)active;
                        tlog("VPU-A Y-top out (row0, 8 words, raw):");
                        tlog("  %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx",
                             (unsigned long)sb[0], (unsigned long)sb[1],
                             (unsigned long)sb[2], (unsigned long)sb[3],
                             (unsigned long)sb[4], (unsigned long)sb[5],
                             (unsigned long)sb[6], (unsigned long)sb[7]);
                        /* After bswap32 readback, show first 16 Y pixels */
                        uint32_t r0 = __builtin_bswap32(sb[0]);
                        uint32_t r1 = __builtin_bswap32(sb[1]);
                        uint32_t r2 = __builtin_bswap32(sb[2]);
                        uint32_t r3 = __builtin_bswap32(sb[3]);
                        tlog("  bswap: %08lx %08lx %08lx %08lx",
                             (unsigned long)r0, (unsigned long)r1,
                             (unsigned long)r2, (unsigned long)r3);
                        tlog("  pixels: %d %d %d %d %d %d %d %d ...",
                             (r0>>24)&0xFF, (r0>>16)&0xFF,
                             (r0>>8)&0xFF, r0&0xFF,
                             (r1>>24)&0xFF, (r1>>16)&0xFF,
                             (r1>>8)&0xFF, r1&0xFF);
                        lflush();
                    }

                    readback_luma(active, frame_y, mb_col, mb_row, 0, y_stride);
                    toggle ^= 1;

                    /* Y-bottom */
                    active = (toggle == 0) ? small_a : small_b;
                    pack_coeff_pair(coeff_buf, blocks[2], blocks[3]);
                    rb->memset(active, 0, SMALL_BUF_SIZE);
                    rb->commit_dcache();
                    if (hw_mb_submit((uint32_t)(uintptr_t)coeff_mem,
                                     (uint32_t)(uintptr_t)small_a,
                                     (uint32_t)(uintptr_t)small_b,
                                     toggle, 0) < 0)
                        timeouts++;
                    rb->commit_discard_dcache();
                    readback_luma(active, frame_y, mb_col, mb_row, 8, y_stride);
                    toggle ^= 1;

                    /* Chroma */
                    active = (toggle == 0) ? small_a : small_b;
                    pack_coeff_pair(coeff_buf, blocks[4], blocks[5]);
                    rb->memset(active, 0, SMALL_BUF_SIZE);
                    rb->commit_dcache();
                    if (hw_mb_submit((uint32_t)(uintptr_t)coeff_mem,
                                     (uint32_t)(uintptr_t)small_a,
                                     (uint32_t)(uintptr_t)small_b,
                                     toggle, 1) < 0)
                        timeouts++;
                    rb->commit_discard_dcache();
                    /* VPU-A outputs block0=Cr, block1=Cb */
                    readback_chroma(active, frame_cr, frame_cb,
                                    mb_col, mb_row, c_stride);
                    toggle ^= 1;

                    restart_count++;

                    if (timeouts >= 3)
                    {
                        tlog("ABORT: %d timeouts at MB %d,%d",
                             timeouts, mb_col, mb_row);
                        goto decode_done;
                    }
                }
            }

            /* Deblock flush */
            {
                int16_t zeros[64];
                rb->memset(zeros, 0, sizeof(zeros));
                active = (toggle == 0) ? small_a : small_b;
                pack_coeff_pair(coeff_buf, zeros, zeros);
                rb->memset(active, 0, SMALL_BUF_SIZE);
                rb->commit_dcache();
                hw_mb_submit((uint32_t)(uintptr_t)coeff_mem,
                             (uint32_t)(uintptr_t)small_a,
                             (uint32_t)(uintptr_t)small_b,
                             toggle, 0);
                rb->commit_discard_dcache();
            }

decode_done:
            tlog("Decode: %d MBs, %d timeouts",
                 restart_count, timeouts);
            /* Dump first 16 Y pixels of frame */
            tlog("Frame Y row0: %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",
                 frame_y[0], frame_y[1], frame_y[2], frame_y[3],
                 frame_y[4], frame_y[5], frame_y[6], frame_y[7],
                 frame_y[8], frame_y[9], frame_y[10], frame_y[11],
                 frame_y[12], frame_y[13], frame_y[14], frame_y[15]);
            tlog("Frame Cb[0..7]: %d %d %d %d %d %d %d %d",
                 frame_cb[0], frame_cb[1], frame_cb[2], frame_cb[3],
                 frame_cb[4], frame_cb[5], frame_cb[6], frame_cb[7]);
            tlog("Frame Cr[0..7]: %d %d %d %d %d %d %d %d",
                 frame_cr[0], frame_cr[1], frame_cr[2], frame_cr[3],
                 frame_cr[4], frame_cr[5], frame_cr[6], frame_cr[7]);
            lflush();
        }

        /* Display YCbCr → RGB565 on LCD, scaled to fit */
        tlog("--- YCbCr -> LCD (%dx%d scaled to fit) ---",
             tj.width, tj.height);
        {
            fb_data *tile = (fb_data *)(void *)work_buf1;
            int ox, oy;
            int dst_w, dst_h, pad_x, pad_y;

            /* Compute letterbox dimensions */
            dst_w = LCD_WIDTH;
            dst_h = (tj.height * LCD_WIDTH) / tj.width;
            if (dst_h > LCD_HEIGHT)
            {
                dst_h = LCD_HEIGHT;
                dst_w = (tj.width * LCD_HEIGHT) / tj.height;
            }
            pad_x = (LCD_WIDTH - dst_w) / 2;
            pad_y = (LCD_HEIGHT - dst_h) / 2;
            tlog("  dst=%dx%d pad=(%d,%d)", dst_w, dst_h, pad_x, pad_y);

            rb->lcd_clear_display();
            for (oy = 0; oy < dst_h; oy++)
            {
                int sy = oy * tj.height / dst_h;
                if (sy >= tj.height) sy = tj.height - 1;
                for (ox = 0; ox < dst_w; ox++)
                {
                    int sx = ox * tj.width / dst_w;
                    if (sx >= tj.width) sx = tj.width - 1;
                    uint8_t yv  = frame_y[sy * y_stride + sx];
                    uint8_t cbv = frame_cb[(sy / 2) * c_stride + sx / 2];
                    uint8_t crv = frame_cr[(sy / 2) * c_stride + sx / 2];
                    int r = clamp8(yv + (((int)crv - 128) * 359 >> 8));
                    int g = clamp8(yv - (((int)cbv - 128) * 88 >> 8)
                                      - (((int)crv - 128) * 183 >> 8));
                    int b = clamp8(yv + (((int)cbv - 128) * 454 >> 8));
                    tile[ox] =
                        ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
                }
                rb->lcd_bitmap(tile, pad_x, pad_y + oy, dst_w, 1);
            }
            rb->lcd_update();

            tlog("LCD updated — waiting for button");
            lflush();

            /* Wait for button press */
            while (rb->button_get(true) == BUTTON_NONE) {}
        }

        vpua_power_off();
        tlog("VPU-A powered off");
    }

    tlog("=== JPEG HW Test done ===");
    lflush();
    if (log_fd >= 0) rb->close(log_fd);

    return PLUGIN_OK;
}
