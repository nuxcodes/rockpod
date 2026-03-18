/***************************************************************************
 * S5L8702 VPU-A JPEG IDCT Engine — Proof of Concept
 *
 * This file preserves the VPU-A (0x39600000) JPEG hardware IDCT pipeline
 * discovered during v33-v36 reverse engineering. Originally misidentified
 * as an "H.264 hybrid decode path," the VPU-A is actually Apple's JPEG
 * decoder (FUN_0007e9e0 = JPEG Huffman + HW IDCT).
 *
 * CORRECTION: FUN_0007e9e0 = JPEG decoder (not H.264!)
 *             FUN_0001b388 = bswap32 (not forward DCT!)
 *             Apple's real H.264 decoder = VPU-B at 0x39800000 (see h264_poc.c)
 *
 * This code is preserved for future HW-accelerated album art (JPEG) decoding.
 * Tests: Phase 1 (reset), Phase 2 (JPEG IDCT init), Phase 3 (per-MB IDCT).
 ****************************************************************************/

#include "plugin.h"
#include "s5l87xx.h"

#define LOG_PATH "/vdec_jpeg.log"
#define REG32(addr) (*(volatile uint32_t *)(addr))

/* ---- VPU-A sub-block base addresses (0x39600000) ---- */
#define VDEC_MAIN   0x39600000
#define VDEC_CORE   0x39610000
#define VDEC_DMA    0x39630000
#define VDEC_XFORM  0x39641000
#define VDEC_DEBLK  0x39650000
#define VDEC_SUB    0x39660000

/* XFORM+800 register group (IDCT command interface) */
#define XFORM_800   (*(volatile uint32_t *)0x39641800)
#define XFORM_808   (*(volatile uint32_t *)0x39641808)
#define DMA_10C     (*(volatile uint32_t *)0x3963010C)

/* IDCT command constant from Apple's DAT_00091530 */
#define XFORM_CMD_BASE  0x00020341

#define TEST_WIDTH_MBS    20
#define TEST_HEIGHT_MBS   15
#define TEST_WIDTH        320
#define TEST_HEIGHT       240

/* Buffer sizes */
#define DMA_WORK_SIZE     0x400
#define WORK_BUF_SIZE     0x20000
#define COEFF_BUF_SIZE    0x200
#define SMALL_BUF_SIZE    0x400
#define FRAME_Y_SIZE      (TEST_WIDTH * TEST_HEIGHT)
#define FRAME_CB_SIZE     (TEST_WIDTH / 2 * TEST_HEIGHT / 2)
#define FRAME_CR_SIZE     FRAME_CB_SIZE

#define ALIGN32(x)   (((uintptr_t)(x) + 31) & ~31)
#define ALIGN4K(x)   (((uintptr_t)(x) + 0xFFF) & ~0xFFF)

static uint8_t *frame_y, *frame_cb, *frame_cr;
static int log_fd = -1;

static void lflush(void)
{
    if (log_fd >= 0) {
        rb->close(log_fd);
        log_fd = rb->open(LOG_PATH, O_WRONLY|O_APPEND, 0666);
    }
}

static void poc_log(const char *fmt, ...)
{
    static char buf[200];
    va_list ap;
    va_start(ap, fmt);
    rb->vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (log_fd >= 0) rb->fdprintf(log_fd, "%s\n", buf);
}

/* ===================== VPU-A POWER ON (JPEG mode) ===================== */
static void vpua_power_on(void)
{
    uint32_t cg, pw;

    cg = REG32(CLK_BASE + 0x08);
    cg &= ~0x80000000;
    cg |= 0x30000000;
    REG32(CLK_BASE + 0x08) = cg;
    rb->sleep(HZ/5);

    pw = PWRCON(0);
    PWRCON(0) = pw & ~((7 << 14) | (1 << 18));
    rb->sleep(HZ/5);

    REG32(0x38100000 + 0x314) &= ~1; /* JPEG mode (clear bit 0) */

    REG32(VDEC_MAIN + 0x1C) = 0xFFFFFFFF;
    REG32(VDEC_CORE)        = 0xFFFFFFFF;
    REG32(VDEC_CORE)        = 0xFFFFFFFF;
    REG32(VDEC_DMA + 0x100) = 0xFFFFFFFF;
    REG32(VDEC_DEBLK)       = 0xFFFFFFFF;
    REG32(VDEC_SUB)         = 0xFFFFFFFF;
    REG32(VDEC_MAIN)        = 0xFFFFFFFF;

    REG32(VDEC_MAIN + 0x04) = 0x40;    /* JPEG mode */
    REG32(VDEC_MAIN + 0x0C) = 0;
    REG32(VDEC_MAIN + 0x10) = 0x10100; /* 1x1 MBs, stride=32 */
    REG32(VDEC_SUB  + 0x04) = 2;
    REG32(VDEC_SUB  + 0x10) = 0x182;
    REG32(VDEC_DMA + 0x110) = 0x800;
    REG32(VDEC_XFORM+ 0x804)= 0x40;
    REG32(VDEC_DEBLK+ 0x10) = 0x10;
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
    REG32(0x38100000 + 0x314) |= 1;

    cg = REG32(CLK_BASE + 0x08);
    cg |= 0x80000000;
    cg &= ~0x30000000;
    REG32(CLK_BASE + 0x08) = cg;
}

static void vpua_reset(void)
{
    REG32(VDEC_MAIN + 0x2C) = 2;
    REG32(VDEC_MAIN + 0x1C) = 0xFFFFFFFF;
    REG32(VDEC_MAIN + 0x0C) = 0;
    REG32(VDEC_CORE)         = 0xFFFFFFFF;
    REG32(VDEC_CORE)         = 0xFFFFFFFF;
    REG32(VDEC_DEBLK)        = 0xFFFFFFFF;
    REG32(VDEC_DMA + 0x100)  = 0xFFFFFFFF;
    REG32(VDEC_SUB)          = 0xFFFFFFFF;
    REG32(VDEC_SUB)          = 0xFFFFFFFF;
}

static void vpua_jpeg_init(uint32_t dma_addr, uint32_t work1_addr)
{
    int i;

    REG32(VDEC_MAIN + 0x1C) = 0xFFFFFFFF;
    REG32(VDEC_MAIN + 0x0C) = 0;
    REG32(VDEC_CORE)         = 0xFFFFFFFF;
    REG32(VDEC_CORE)         = 0xFFFFFFFF;
    REG32(VDEC_DMA + 0x100)  = 0xFFFFFFFF;
    REG32(VDEC_DEBLK)        = 0xFFFFFFFF;
    REG32(VDEC_SUB)          = 0xFFFFFFFF;
    REG32(VDEC_MAIN)         = 0xFFFFFFFF;

    {
        uint32_t main10 = 0x00010100;
        REG32(VDEC_MAIN + 0x04) = 0x40;
        REG32(VDEC_SUB  + 0x04) = 2;
        REG32(VDEC_SUB  + 0x10) = 0x182;
        REG32(VDEC_MAIN + 0x10) = main10;
        REG32(VDEC_DMA + 0x110) = 0x800;
        REG32(VDEC_XFORM+ 0x804)= 0x40;
        REG32(VDEC_DEBLK+ 0x10) = 0x10;
        REG32(VDEC_SUB  + 0x6C) = main10 - 0xFF;
    }

    REG32(VDEC_SUB + 0x20) = dma_addr;
    REG32(VDEC_SUB + 0x24) = DMA_WORK_SIZE;
    REG32(VDEC_SUB + 0x78) = work1_addr;
    REG32(VDEC_SUB + 0x7C) = WORK_BUF_SIZE;
    REG32(VDEC_SUB + 0x80) = 0;

    for (i = 0; i < 64; i++) {
        REG32(VDEC_XFORM + 0x200 + i * 4) = 16;
        REG32(VDEC_XFORM + 0x300 + i * 4) = 16;
    }
}

/* ===================== PER-BLOCK HARDWARE IDCT ===================== */
static int hw_mb_submit(uint32_t coeff_buf_phys,
                        uint32_t ref_addr, uint32_t out_addr,
                        int frame_toggle, int is_chroma)
{
    int timeout;

    REG32(VDEC_SUB + 0x18) = coeff_buf_phys;
    REG32(VDEC_SUB + 0x1C) = coeff_buf_phys + COEFF_BUF_SIZE;
    REG32(VDEC_SUB + 0x0C) = 3;

    REG32(VDEC_SUB + 0x2C) = ref_addr;
    REG32(VDEC_SUB + 0x3C) = out_addr;

    timeout = 100000;
    while ((REG32(VDEC_DEBLK + 0x14) & 0x10000) && --timeout > 0) {}
    if (timeout == 0) return -1;
    REG32(VDEC_DEBLK + 0x0C) = ((uint32_t)frame_toggle << 30) | 0x80;

    {
        int p;
        for (p = 0; p < 2; p++) {
            timeout = 100000;
            while ((XFORM_808 & 2) && --timeout > 0) {}
            if (timeout == 0) return -1;
            XFORM_800 = XFORM_CMD_BASE | ((uint32_t)is_chroma << 19);
            DMA_10C = ((uint32_t)is_chroma << 3) | 0x31;
        }
    }
    return 0;
}

static void fill_coeff_pair(uint32_t *buf, uint32_t dc0, uint32_t dc1)
{
    int i;
    for (i = 0; i < 128; i++)
        buf[i] = 0;
    buf[0]  = __builtin_bswap32(dc0);
    buf[64] = __builtin_bswap32(dc1);
}

/* ===================== READBACK ===================== */
static void readback_luma(const uint8_t *src, uint8_t *frame,
                          int mb_col, int mb_row, int row_offset)
{
    int row;
    for (row = 0; row < 8; row++) {
        const uint32_t *s = (const uint32_t *)(src + row * 32);
        uint32_t *d = (uint32_t *)(frame +
                      (mb_row * 16 + row_offset + row) * TEST_WIDTH +
                      mb_col * 16);
        d[0] = __builtin_bswap32(s[0]);
        d[1] = __builtin_bswap32(s[1]);
        d[2] = __builtin_bswap32(s[2]);
        d[3] = __builtin_bswap32(s[3]);
    }
}

static void readback_chroma(const uint8_t *src,
                            uint8_t *cb_frame, uint8_t *cr_frame,
                            int mb_col, int mb_row)
{
    int row;
    int cw = TEST_WIDTH / 2;
    for (row = 0; row < 8; row++) {
        const uint32_t *s = (const uint32_t *)(src + row * 32);
        uint32_t *cb_d = (uint32_t *)(cb_frame +
                         (mb_row * 8 + row) * cw + mb_col * 8);
        uint32_t *cr_d = (uint32_t *)(cr_frame +
                         (mb_row * 8 + row) * cw + mb_col * 8);
        cb_d[0] = __builtin_bswap32(s[0]);
        cb_d[1] = __builtin_bswap32(s[1]);
        cr_d[0] = __builtin_bswap32(s[2]);
        cr_d[1] = __builtin_bswap32(s[3]);
    }
}

static void dump_small_buf(const uint8_t *buf, const char *label)
{
    int row;
    poc_log("--- small_buf %s (stride-32, 8 rows) ---", label);
    for (row = 0; row < 8; row++) {
        const uint32_t *w = (const uint32_t *)(buf + row * 32);
        poc_log("  %03x: %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx",
                row * 32,
                (unsigned long)w[0], (unsigned long)w[1],
                (unsigned long)w[2], (unsigned long)w[3],
                (unsigned long)w[4], (unsigned long)w[5],
                (unsigned long)w[6], (unsigned long)w[7]);
    }
}

/* ===================== MAIN ===================== */
enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    size_t buf_size;
    uint8_t *buf, *p;
    uint8_t *dma_work, *work_buf1, *work_buf2;
    uint8_t *small_a, *small_b;
    uint8_t *coeff_buf;
    int i;
    int total_mbs = TEST_WIDTH_MBS * TEST_HEIGHT_MBS;

    rb->splash(HZ/2, "JPEG VPU-A PoC");

    log_fd = rb->open(LOG_PATH, O_WRONLY|O_CREAT|O_TRUNC, 0666);
    poc_log("=== JPEG VPU-A IDCT Engine PoC (%dx%d) ===", TEST_WIDTH, TEST_HEIGHT);
    poc_log("VPU-A at 0x39600000 — JPEG 8x8 IDCT + deblock");
    poc_log("(Apple's FUN_0007e9e0 is JPEG, not H.264)");

    /* ---- Allocate buffers ---- */
    buf = rb->plugin_get_audio_buffer(&buf_size);
    poc_log("Audio buffer: %08lx size=%lu",
            (unsigned long)(uintptr_t)buf, (unsigned long)buf_size);

    p = (uint8_t *)ALIGN32(buf);
    dma_work  = p;                        p += DMA_WORK_SIZE;
    work_buf1 = (uint8_t *)ALIGN32(p);    p = work_buf1 + WORK_BUF_SIZE;
    work_buf2 = (uint8_t *)ALIGN32(p);    p = work_buf2 + WORK_BUF_SIZE;
    small_a   = (uint8_t *)ALIGN4K(p);    p = small_a + SMALL_BUF_SIZE;
    small_b   = (uint8_t *)ALIGN4K(p);    p = small_b + SMALL_BUF_SIZE;
    frame_y   = (uint8_t *)ALIGN4K(p);    p = frame_y + FRAME_Y_SIZE;
    frame_cb  = (uint8_t *)ALIGN32(p);    p = frame_cb + FRAME_CB_SIZE;
    frame_cr  = (uint8_t *)ALIGN32(p);    p = frame_cr + FRAME_CR_SIZE;
    coeff_buf = (uint8_t *)ALIGN32(p);    p = coeff_buf + COEFF_BUF_SIZE;

    (void)work_buf2; /* reserved for future use */

    rb->memset(dma_work, 0, DMA_WORK_SIZE);
    rb->memset(work_buf1, 0xBB, WORK_BUF_SIZE);
    rb->memset(small_a, 0xCC, SMALL_BUF_SIZE);
    rb->memset(small_b, 0xCC, SMALL_BUF_SIZE);
    rb->memset(frame_y, 0, FRAME_Y_SIZE);
    rb->memset(frame_cb, 0x80, FRAME_CB_SIZE);
    rb->memset(frame_cr, 0x80, FRAME_CR_SIZE);
    rb->memset(coeff_buf, 0, COEFF_BUF_SIZE);

    poc_log("Buffers: dma=%08lx work=%08lx small_a=%08lx small_b=%08lx",
            (unsigned long)(uintptr_t)dma_work,
            (unsigned long)(uintptr_t)work_buf1,
            (unsigned long)(uintptr_t)small_a,
            (unsigned long)(uintptr_t)small_b);
    lflush();

    {
    uint32_t dma_phys   = (uint32_t)(uintptr_t)dma_work;
    uint32_t work1_phys = (uint32_t)(uintptr_t)work_buf1;
    uint32_t coeff_phys = (uint32_t)(uintptr_t)coeff_buf;
    uint32_t sa_phys    = (uint32_t)(uintptr_t)small_a;
    uint32_t sb_phys    = (uint32_t)(uintptr_t)small_b;

    /* ---- Phase 1: Power on + Reset ---- */
    poc_log("--- Phase 1: VPU-A power on ---");
    vpua_power_on();
    poc_log("  MAIN+08 = %08lx (hw status)",
            (unsigned long)REG32(VDEC_MAIN + 0x08));
    lflush();

    vpua_reset();

    /* ---- Phase 2: JPEG IDCT init ---- */
    poc_log("--- Phase 2: JPEG IDCT init ---");
    vpua_jpeg_init(dma_phys, work1_phys);
    poc_log("  MAIN+04=%08lx SUB+10=%08lx MAIN+10=%08lx SUB+6C=%08lx",
            (unsigned long)REG32(VDEC_MAIN + 0x04),
            (unsigned long)REG32(VDEC_SUB + 0x10),
            (unsigned long)REG32(VDEC_MAIN + 0x10),
            (unsigned long)REG32(VDEC_SUB + 0x6C));
    lflush();
    rb->commit_dcache();

    /* ---- Phase 3: Per-MB IDCT test ---- */
    poc_log("--- Phase 3: per-MB IDCT (%d MBs) ---", total_mbs);
    {
        int mb, timeouts = 0, frame_toggle = 0;
        uint32_t *cb = (uint32_t *)(void *)coeff_buf;
        uint8_t *active;

        rb->commit_discard_dcache();

        for (mb = 0; mb < total_mbs; mb++) {
            int mb_col = mb % TEST_WIDTH_MBS;
            int mb_row = mb / TEST_WIDTH_MBS;
            uint32_t dc = (uint32_t)((mb_col + 1) * 3);

            /* Sub-call 1: Y-top */
            active = (frame_toggle == 0) ? small_a : small_b;
            fill_coeff_pair(cb, dc, dc);
            rb->memset(active, 0xCC, SMALL_BUF_SIZE);
            rb->commit_dcache();
            if (hw_mb_submit(coeff_phys, sa_phys, sb_phys,
                             frame_toggle, 0) < 0)
                timeouts++;
            rb->commit_discard_dcache();
            if (mb == 0) dump_small_buf(active, "Y-top");
            readback_luma(active, frame_y, mb_col, mb_row, 0);
            frame_toggle ^= 1;

            /* Sub-call 2: Y-bottom */
            active = (frame_toggle == 0) ? small_a : small_b;
            fill_coeff_pair(cb, dc, dc);
            rb->memset(active, 0xCC, SMALL_BUF_SIZE);
            rb->commit_dcache();
            if (hw_mb_submit(coeff_phys, sa_phys, sb_phys,
                             frame_toggle, 0) < 0)
                timeouts++;
            rb->commit_discard_dcache();
            if (mb == 0) dump_small_buf(active, "Y-bot");
            readback_luma(active, frame_y, mb_col, mb_row, 8);
            frame_toggle ^= 1;

            /* Sub-call 3: Chroma */
            active = (frame_toggle == 0) ? small_a : small_b;
            fill_coeff_pair(cb, 0, 0);
            rb->memset(active, 0xCC, SMALL_BUF_SIZE);
            rb->commit_dcache();
            if (hw_mb_submit(coeff_phys, sa_phys, sb_phys,
                             frame_toggle, 1) < 0)
                timeouts++;
            rb->commit_discard_dcache();
            if (mb == 0) dump_small_buf(active, "Chroma");
            readback_chroma(active, frame_cb, frame_cr, mb_col, mb_row);
            frame_toggle ^= 1;

            if (mb < 2 || mb == total_mbs - 1)
                poc_log("  MB[%d](%d,%d): dc=%lu toggle=%d",
                        mb, mb_col, mb_row, (unsigned long)dc, frame_toggle);

            if (timeouts >= 3) {
                poc_log("  ABORT: %d timeouts at MB %d", timeouts, mb);
                break;
            }
        }
        poc_log("  %d/%d MBs (%d timeouts)", mb, total_mbs, timeouts);

        /* Deblock flush */
        {
            active = (frame_toggle == 0) ? small_a : small_b;
            fill_coeff_pair(cb, 60, 60);
            rb->memset(active, 0xCC, SMALL_BUF_SIZE);
            rb->commit_dcache();
            if (hw_mb_submit(coeff_phys, sa_phys, sb_phys,
                             frame_toggle, 0) < 0)
                poc_log("Deblock flush: TIMEOUT");
            else {
                rb->commit_discard_dcache();
                poc_log("Deblock flush: OK");
            }
        }
    }
    lflush();

    /* ---- LCD display ---- */
    poc_log("--- YCbCr -> LCD ---");
    {
        fb_data *tile = (fb_data *)(void *)work_buf1;
        int ty, py, px;

        rb->lcd_clear_display();
        for (ty = 0; ty < TEST_HEIGHT; ty += 16) {
            int rows = (ty + 16 <= TEST_HEIGHT) ? 16 : TEST_HEIGHT - ty;
            for (py = 0; py < rows; py++) {
                for (px = 0; px < TEST_WIDTH; px++) {
                    uint8_t yv  = frame_y[(ty + py) * TEST_WIDTH + px];
                    uint8_t cbv = frame_cb[((ty + py) / 2) * (TEST_WIDTH / 2)
                                           + px / 2];
                    uint8_t crv = frame_cr[((ty + py) / 2) * (TEST_WIDTH / 2)
                                           + px / 2];
                    int r = yv + (((int)crv - 128) * 359 >> 8);
                    int g = yv - (((int)cbv - 128) * 88 >> 8)
                              - (((int)crv - 128) * 183 >> 8);
                    int b = yv + (((int)cbv - 128) * 454 >> 8);
                    if (r < 0) r = 0;
                    if (r > 255) r = 255;
                    if (g < 0) g = 0;
                    if (g > 255) g = 255;
                    if (b < 0) b = 0;
                    if (b > 255) b = 255;
                    tile[py * TEST_WIDTH + px] =
                        ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
                }
            }
            rb->lcd_bitmap(tile, 0, ty, TEST_WIDTH, rows);
        }
        rb->lcd_update();

        poc_log("  LCD updated, 5s timeout...");
        lflush();
        {
            int btn_wait = 50;
            while (--btn_wait > 0) {
                if (rb->button_get(false) != BUTTON_NONE) break;
                rb->sleep(HZ/10);
            }
        }
    }

    } /* end phys-addr scope */

    /* ---- Cleanup ---- */
    vpua_power_off();
    poc_log("=== JPEG VPU-A PoC done ===");
    lflush();
    if (log_fd >= 0) rb->close(log_fd);

    for (i = 0; i < 1; i++) {} /* suppress unused warning */
    rb->splashf(HZ*3, "JPEG VPU-A done");
    return PLUGIN_OK;
}
