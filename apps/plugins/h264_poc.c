/***************************************************************************
 * S5L8702 H.264 Hardware Video Decoder - PoC v25
 *
 * HYBRID PATH TEST: SW coefficients → HW IDCT/deblock/MC
 *
 * Instead of feeding H.264 CAVLC bitstream to the entropy decoder
 * (which we've proven doesn't work), this version feeds software-
 * prepared coefficient data directly to the IDCT pipeline.
 *
 * Based on decompilation of Apple's FUN_0009144c (per-MB writeback):
 *   - SUB+18/1C = coefficient buffer address (512 bytes per MB)
 *   - XFORM+800 = IDCT command (0x00020341 | has_data << 19)
 *   - XFORM+808 = IDCT status (poll bit 1 for ready)
 *   - DMA+10C   = DMA control (has_data << 3 | 0x31)
 *   - DEBLK+0C  = deblock trigger (frame_toggle << 30 | 0x80)
 ****************************************************************************/

#include "plugin.h"
#include "s5l87xx.h"

#define LOG_PATH "/vdec_poc.log"
#define REG32(addr) (*(volatile uint32_t *)(addr))

/* Decoder sub-block base addresses */
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

#define TEST_WIDTH_MBS    8
#define TEST_HEIGHT_MBS   8
#define TEST_WIDTH        128
#define TEST_HEIGHT       128
#define Y_PLANE_SIZE      0x8000      /* 32 KB */
#define CBCR_PLANE_SIZE   0x4000      /* 16 KB */

/* Buffer sizes from Apple firmware */
#define DMA_WORK_SIZE     0x400       /* 1 KB */
#define WORK_BUF_SIZE     0x20000     /* 128 KB each */
#define COEFF_BUF_SIZE    0x200       /* 512 bytes per MB */

/* Cache-line alignment */
#define ALIGN32(x)   (((uintptr_t)(x) + 31) & ~31)
/* 4KB page alignment */
#define ALIGN4K(x)   (((uintptr_t)(x) + 0xFFF) & ~0xFFF)

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

/* ===================== POWER ON (v11g sequence) ===================== */
static void vdec_power_on(void)
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

    REG32(0x38100000 + 0x314) &= ~1;

    /* Initial IRQ clear */
    REG32(VDEC_MAIN + 0x1C) = 0xFFFFFFFF;
    REG32(VDEC_CORE)        = 0xFFFFFFFF;
    REG32(VDEC_CORE)        = 0xFFFFFFFF;
    REG32(VDEC_DMA + 0x100) = 0xFFFFFFFF;
    REG32(VDEC_DEBLK)       = 0xFFFFFFFF;
    REG32(VDEC_SUB)         = 0xFFFFFFFF;
    REG32(VDEC_MAIN)        = 0xFFFFFFFF;

    /* Config writes */
    REG32(VDEC_MAIN + 0x04) = 0x40;
    REG32(VDEC_MAIN + 0x0C) = 0;
    REG32(VDEC_MAIN + 0x10) = 0x10100;
    REG32(VDEC_SUB  + 0x04) = 2;
    REG32(VDEC_SUB  + 0x10) = 0x182;
    REG32(VDEC_DMA + 0x110) = 0x800;
    REG32(VDEC_XFORM+ 0x804)= 0x40;
    REG32(VDEC_DEBLK+ 0x10) = 0x10;
    REG32(VDEC_SUB  + 0x6C) = 0x10001;
}

static void vdec_power_off(void)
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

/* ===================== PHASE 1: Apple-style Reset ===================== */
/* Matches FUN_0007476c exactly (validated from Ghidra globals) */
static void vdec_reset(void)
{
    poc_log("Phase 1: Apple-style reset (FUN_0007476c)");
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

/* ===================== PHASE 2: Frame Setup ===================== */
static void vdec_frame_setup(uint32_t dma_addr, uint32_t work1_addr,
                             uint32_t work2_addr)
{
    int i;
    poc_log("Phase 2: frame_setup");
    poc_log("  dma=%08lx w1=%08lx w2=%08lx",
            (unsigned long)dma_addr, (unsigned long)work1_addr,
            (unsigned long)work2_addr);

    REG32(VDEC_SUB + 0x20) = dma_addr;
    REG32(VDEC_SUB + 0x24) = DMA_WORK_SIZE;
    REG32(VDEC_SUB + 0x78) = work1_addr;
    REG32(VDEC_SUB + 0x7C) = WORK_BUF_SIZE;
    REG32(VDEC_SUB + 0x80) = 0;
    REG32(VDEC_MAIN + 0x10) = 0x80000000;
    REG32(VDEC_CORE + 0x1C) = 1;
    REG32(VDEC_MAIN + 0x04) = 0x142;
    REG32(VDEC_DMA + 0x104) = 0x14;
    REG32(VDEC_CORE + 0x04) = 0x3F;
    REG32(VDEC_SUB  + 0x04) = 0x12;

    /* Scaling matrices (all 16s — identity for baseline) */
    for (i = 0; i < 64; i++) {
        REG32(VDEC_XFORM + 0x200 + i * 4) = 16;
        REG32(VDEC_XFORM + 0x300 + i * 4) = 16;
    }
    poc_log("  scaling matrices written");

    /* Per-frame XFORM config */
    REG32(VDEC_XFORM + 0x804) = (0xFE << 8) | 0x20;  /* chroma_qp_offset=-2 */
    poc_log("  XFORM+804=0x%08lx", (unsigned long)REG32(VDEC_XFORM + 0x804));

    /* Deblocking config (disabled for test) */
    REG32(VDEC_DEBLK + 0x10) = 0x0C02 | (1 << 2);
    poc_log("  DEBLK+10=0x%08lx", (unsigned long)REG32(VDEC_DEBLK + 0x10));
}

/* ===================== PHASE 3: Output Config ===================== */
static void vdec_output_config(int w_mbs, int h_mbs)
{
    uint32_t v;
    poc_log("Phase 3: output_config (%dx%d MBs)", w_mbs, h_mbs);

    REG32(VDEC_SUB + 0x6C) = (0 << 22) | (w_mbs << 16) | (0 << 6) | h_mbs;

    v = REG32(VDEC_MAIN + 0x10);
    v |= ((w_mbs - 1) << 16) | ((h_mbs - 1) << 8);
    REG32(VDEC_MAIN + 0x10) = v;

    REG32(VDEC_SUB + 0x10) = 0x200;
}

/* ===================== PHASE 4: Reference Frame Setup ===================== */
static void vdec_refframe_set(uint32_t ref_y, uint32_t ref_cbcr,
                               uint32_t out_y, uint32_t out_cbcr)
{
    poc_log("Phase 4: ref Y=%08lx C=%08lx, out Y=%08lx C=%08lx",
            (unsigned long)ref_y, (unsigned long)ref_cbcr,
            (unsigned long)out_y, (unsigned long)out_cbcr);

    /* Reference frame (slot A) */
    REG32(VDEC_SUB + 0x2C) = ref_y;
    REG32(VDEC_SUB + 0x30) = 0;
    REG32(VDEC_SUB + 0x34) = ref_cbcr;
    REG32(VDEC_SUB + 0x38) = 0;

    /* Output frame (slot B) */
    REG32(VDEC_SUB + 0x3C) = out_y;
    REG32(VDEC_SUB + 0x40) = 0;
    REG32(VDEC_SUB + 0x44) = out_cbcr;
    REG32(VDEC_SUB + 0x48) = 0;
}

/* ===================== PER-MB HARDWARE IDCT ===================== */
/* Implements Apple's FUN_0009144c register sequence.
 * Feeds 512-byte coefficient buffer to HW IDCT/deblock pipeline. */
static void hw_mb_submit(uint32_t coeff_buf_phys,
                         uint32_t ref_y, uint32_t out_y,
                         int frame_toggle, int has_data)
{
    /* 1. Point SUB DMA to coefficient buffer (512 bytes) */
    REG32(VDEC_SUB + 0x18) = coeff_buf_phys;
    REG32(VDEC_SUB + 0x1C) = coeff_buf_phys + COEFF_BUF_SIZE;
    REG32(VDEC_SUB + 0x0C) = 3;

    /* 2. Set frame addresses */
    REG32(VDEC_SUB + 0x2C) = ref_y;
    REG32(VDEC_SUB + 0x3C) = out_y;

    /* 3. Wait for DEBLK ready, write control */
    while (REG32(VDEC_DEBLK + 0x14) & 0x10000) {}
    REG32(VDEC_DEBLK + 0x0C) = ((uint32_t)frame_toggle << 30) | 0x80;

    /* 4. XFORM IDCT command — 2 partitions (luma then chroma) */
    {
        int p;
        for (p = 0; p < 2; p++) {
            int d = has_data;
            while (XFORM_808 & 2) {}
            XFORM_800 = XFORM_CMD_BASE | ((uint32_t)d << 19);
            DMA_10C = ((uint32_t)d << 3) | 0x31;
        }
    }
}

/* Fill a 512-byte coefficient buffer for one MB.
 * Format: 256 bytes luma (64 uint32, byte-swapped) +
 *         256 bytes chroma (64 uint32, byte-swapped).
 * Exact coefficient layout within each section is TBD —
 * start with DC-only test to see if output changes at all. */
static void fill_test_coefficients(uint32_t *coeff_buf, int mb_idx)
{
    int i;

    /* Zero all 128 words (512 bytes) */
    for (i = 0; i < 128; i++)
        coeff_buf[i] = 0;

    /* Put recognizable non-zero values in DC positions.
     * Byte-swap: ARM host is little-endian, HW wants big-endian.
     * Use __builtin_bswap32 (GCC built-in, available on ARM). */
    {
        /* Luma DC: varies per MB for easy identification */
        uint32_t luma_dc = (uint32_t)((mb_idx + 1) * 4);
        coeff_buf[0] = __builtin_bswap32(luma_dc);

        /* Chroma DC: neutral value */
        uint32_t chroma_dc = 0x80;
        coeff_buf[64] = __builtin_bswap32(chroma_dc);
    }
}

/* ===================== MAIN ===================== */
enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    size_t buf_size;
    uint8_t *buf, *p;
    uint8_t *dma_work, *work_buf1, *work_buf2;
    uint8_t *ref_y, *ref_cbcr;
    uint8_t *out_y, *out_cbcr;
    uint8_t *coeff_buf;
    uint32_t v;
    int i;
    int total_mbs = TEST_WIDTH_MBS * TEST_HEIGHT_MBS;

    rb->splash(HZ/2, "v25 START");

    log_fd = rb->open(LOG_PATH, O_WRONLY|O_CREAT|O_TRUNC, 0666);
    poc_log("=== v25 — SW coeff + HW IDCT test (128x128) ===");

    /* ---- Allocate buffers from audio buffer ---- */
    buf = rb->plugin_get_audio_buffer(&buf_size);
    poc_log("Audio buffer: %08lx size=%lu",
            (unsigned long)(uintptr_t)buf, (unsigned long)buf_size);

    if (buf_size < DMA_WORK_SIZE + 2 * WORK_BUF_SIZE +
                   2 * Y_PLANE_SIZE + 2 * CBCR_PLANE_SIZE +
                   COEFF_BUF_SIZE + 0x2000) {
        poc_log("ERROR: buffer too small");
        if (log_fd >= 0) rb->close(log_fd);
        rb->splash(HZ*3, "Buffer too small!");
        return PLUGIN_ERROR;
    }

    p = (uint8_t *)ALIGN32(buf);

    dma_work  = p;                        p += DMA_WORK_SIZE;
    work_buf1 = (uint8_t *)ALIGN32(p);    p = work_buf1 + WORK_BUF_SIZE;
    work_buf2 = (uint8_t *)ALIGN32(p);    p = work_buf2 + WORK_BUF_SIZE;
    ref_y     = (uint8_t *)ALIGN4K(p);    p = ref_y + Y_PLANE_SIZE;
    ref_cbcr  = (uint8_t *)ALIGN4K(p);    p = ref_cbcr + CBCR_PLANE_SIZE;
    out_y     = (uint8_t *)ALIGN4K(p);    p = out_y + Y_PLANE_SIZE;
    out_cbcr  = (uint8_t *)ALIGN4K(p);    p = out_cbcr + CBCR_PLANE_SIZE;
    coeff_buf = (uint8_t *)ALIGN32(p);

    rb->memset(dma_work, 0, DMA_WORK_SIZE);
    rb->memset(work_buf1, 0xBB, WORK_BUF_SIZE);
    rb->memset(work_buf2, 0, WORK_BUF_SIZE);
    rb->memset(ref_y, 0x40, Y_PLANE_SIZE);
    rb->memset(ref_cbcr, 0x80, CBCR_PLANE_SIZE);
    rb->memset(out_y, 0xAA, Y_PLANE_SIZE);
    rb->memset(out_cbcr, 0xAA, CBCR_PLANE_SIZE);
    rb->memset(coeff_buf, 0, COEFF_BUF_SIZE);

    poc_log("Buffers allocated:");
    poc_log("  dma_work=%08lx", (unsigned long)(uintptr_t)dma_work);
    poc_log("  work1=%08lx work2=%08lx",
            (unsigned long)(uintptr_t)work_buf1,
            (unsigned long)(uintptr_t)work_buf2);
    poc_log("  ref_y=%08lx ref_cbcr=%08lx",
            (unsigned long)(uintptr_t)ref_y,
            (unsigned long)(uintptr_t)ref_cbcr);
    poc_log("  out_y=%08lx out_cbcr=%08lx",
            (unsigned long)(uintptr_t)out_y,
            (unsigned long)(uintptr_t)out_cbcr);
    poc_log("  coeff_buf=%08lx (%d bytes)",
            (unsigned long)(uintptr_t)coeff_buf, COEFF_BUF_SIZE);
    lflush();

    /* ---- Power on decoder ---- */
    poc_log("--- Power on ---");
    rb->splash(HZ/4, "Power on...");
    vdec_power_on();

    v = REG32(VDEC_MAIN + 0x08);
    poc_log("MAIN+08 = %08lx (hw status)", (unsigned long)v);
    if (v == 0)
        poc_log("WARNING: MAIN+08 is 0, decoder may not be powered");
    lflush();

    /* ---- Flush cache before HW access ---- */
    poc_log("Flushing cache...");
    rb->commit_dcache();

    poc_log("--- Status BEFORE decode ---");
    poc_log("  MAIN+00=%08lx SUB+00=%08lx",
            (unsigned long)REG32(VDEC_MAIN),
            (unsigned long)REG32(VDEC_SUB));
    lflush();

    /* ---- Execute decode sequence ---- */
    rb->splash(HZ/4, "Decoding...");

    /* Phase 1: Apple-style reset */
    vdec_reset();
    poc_log("  [post-P1] MAIN=%08lx SUB=%08lx CORE=%08lx",
            (unsigned long)REG32(VDEC_MAIN),
            (unsigned long)REG32(VDEC_SUB),
            (unsigned long)REG32(VDEC_CORE));
    lflush();

    /* Phase 2: Frame setup (scaling matrices, work buffers) */
    vdec_frame_setup(
        (uint32_t)(uintptr_t)dma_work,
        (uint32_t)(uintptr_t)work_buf1,
        (uint32_t)(uintptr_t)work_buf2
    );
    poc_log("  [post-P2] MAIN=%08lx SUB=%08lx CORE=%08lx",
            (unsigned long)REG32(VDEC_MAIN),
            (unsigned long)REG32(VDEC_SUB),
            (unsigned long)REG32(VDEC_CORE));
    lflush();

    /* Phase 3: Output config */
    vdec_output_config(TEST_WIDTH_MBS, TEST_HEIGHT_MBS);
    poc_log("  [post-P3] MAIN=%08lx SUB=%08lx CORE=%08lx",
            (unsigned long)REG32(VDEC_MAIN),
            (unsigned long)REG32(VDEC_SUB),
            (unsigned long)REG32(VDEC_CORE));
    lflush();

    /* Phase 4: Reference and output frame addresses */
    vdec_refframe_set(
        (uint32_t)(uintptr_t)ref_y,
        (uint32_t)(uintptr_t)ref_cbcr,
        (uint32_t)(uintptr_t)out_y,
        (uint32_t)(uintptr_t)out_cbcr
    );
    poc_log("  [post-P4] MAIN=%08lx SUB=%08lx CORE=%08lx",
            (unsigned long)REG32(VDEC_MAIN),
            (unsigned long)REG32(VDEC_SUB),
            (unsigned long)REG32(VDEC_CORE));
    lflush();

    /* NOTE: Skipping CORE+10/14/34/9C setup entirely.
     * The hybrid path (FUN_0007e9e0 → FUN_0009144c) does NOT configure
     * CORE registers — validated via Ghidra callee analysis. */
    poc_log("Phase 5: per-MB coefficient feed (%d MBs)", total_mbs);
    poc_log("  XFORM+800 pre: %08lx  XFORM+808 pre: %08lx",
            (unsigned long)XFORM_800, (unsigned long)XFORM_808);
    lflush();

    /* Flush all buffer data to physical memory before DMA */
    rb->commit_discard_dcache();

    /* ---- Per-MB coefficient feed loop ---- */
    {
        int mb, frame_toggle = 0;

        for (mb = 0; mb < total_mbs; mb++) {
            /* Fill coefficient buffer with test data */
            fill_test_coefficients((uint32_t *)(void *)coeff_buf, mb);

            /* Flush coefficient buffer to physical memory */
            rb->commit_dcache();

            /* Submit to hardware IDCT */
            hw_mb_submit(
                (uint32_t)(uintptr_t)coeff_buf,
                (uint32_t)(uintptr_t)ref_y,
                (uint32_t)(uintptr_t)out_y,
                frame_toggle,
                1  /* has_data = 1 */
            );

            /* Toggle frame buffer (double-buffering, per Apple firmware) */
            frame_toggle ^= 1;

            /* Log first few and last MB for debugging */
            if (mb < 3 || mb == total_mbs - 1) {
                poc_log("  MB[%d]: SUB=%08lx DEBLK=%08lx XFORM800=%08lx",
                        mb,
                        (unsigned long)REG32(VDEC_SUB),
                        (unsigned long)REG32(VDEC_DEBLK),
                        (unsigned long)XFORM_800);
            }
        }

        poc_log("  All %d MBs submitted", total_mbs);
        poc_log("  Final toggle=%d", frame_toggle);
    }
    lflush();

    /* NOTE: Skipping Phase 6 (CORE+0C trigger) — not used in hybrid path */

    /* ---- Small delay for any pending HW operations ---- */
    rb->sleep(HZ/10);

    /* ---- Invalidate cache to see DMA results ---- */
    rb->commit_discard_dcache();

    /* ---- Check output buffer ---- */
    poc_log("--- Output buffer check ---");
    {
        int changed = 0;
        for (i = 0; i < Y_PLANE_SIZE; i++) {
            if (out_y[i] != 0xAA) {
                changed++;
            }
        }
        poc_log("Y plane: %d/%d bytes changed from 0xAA", changed, Y_PLANE_SIZE);

        if (changed > 0) {
            int first_changed = -1, last_changed = -1;
            poc_log("*** OUTPUT BUFFER MODIFIED! ***");
            for (i = 0; i < Y_PLANE_SIZE; i++) {
                if (out_y[i] != 0xAA) {
                    if (first_changed < 0) first_changed = i;
                    last_changed = i;
                }
            }
            poc_log("  Changed range: [%d..%d] (%d bytes)",
                    first_changed, last_changed, changed);

            {
                int dump_start = (first_changed / 16) * 16;
                int dump_end = ((last_changed / 16) + 1) * 16;
                if (dump_end > Y_PLANE_SIZE) dump_end = Y_PLANE_SIZE;
                if (dump_end - dump_start > 256) dump_end = dump_start + 256;
                for (i = dump_start; i < dump_end; i += 16) {
                    poc_log("  Y[%04x] %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x",
                            i,
                            out_y[i], out_y[i+1], out_y[i+2], out_y[i+3],
                            out_y[i+4], out_y[i+5], out_y[i+6], out_y[i+7],
                            out_y[i+8], out_y[i+9], out_y[i+10], out_y[i+11],
                            out_y[i+12], out_y[i+13], out_y[i+14], out_y[i+15]);
                }
            }
        }

        changed = 0;
        for (i = 0; i < CBCR_PLANE_SIZE; i++) {
            if (out_cbcr[i] != 0xAA) {
                changed++;
            }
        }
        poc_log("CbCr plane: %d/%d bytes changed from 0xAA", changed, CBCR_PLANE_SIZE);

        {
            int work_changed = 0;
            for (i = 0; i < WORK_BUF_SIZE; i++) {
                if (work_buf1[i] != 0xBB) work_changed++;
            }
            poc_log("work_buf1: %d/%d bytes changed from 0xBB", work_changed, WORK_BUF_SIZE);
        }

        {
            int ref_changed = 0;
            for (i = 0; i < Y_PLANE_SIZE; i++) {
                if (ref_y[i] != 0x40) ref_changed++;
            }
            poc_log("ref_y: %d/%d bytes changed from 0x40", ref_changed, Y_PLANE_SIZE);
        }
    }
    lflush();

    /* ---- Binary dump ---- */
    poc_log("--- Dumping output buffers to files ---");
    {
        int dump_fd;
        ssize_t written;

        dump_fd = rb->open("/vdec_y.bin", O_WRONLY|O_CREAT|O_TRUNC, 0666);
        if (dump_fd >= 0) {
            written = rb->write(dump_fd, out_y, Y_PLANE_SIZE);
            rb->close(dump_fd);
            poc_log("  Y dump: %ld bytes to /vdec_y.bin", (long)written);
        }

        dump_fd = rb->open("/vdec_cbcr.bin", O_WRONLY|O_CREAT|O_TRUNC, 0666);
        if (dump_fd >= 0) {
            written = rb->write(dump_fd, out_cbcr, CBCR_PLANE_SIZE);
            rb->close(dump_fd);
            poc_log("  CbCr dump: %ld bytes to /vdec_cbcr.bin", (long)written);
        }
    }
    lflush();

    /* ---- Post-decode register dump ---- */
    poc_log("--- Post-decode register dump ---");
    poc_log("MAIN:");
    {
        int off;
        for (off = 0; off <= 0x2C; off += 4)
            poc_log("  +%02x=%08lx", off, (unsigned long)REG32(VDEC_MAIN + off));
    }
    poc_log("CORE:");
    {
        int off;
        for (off = 0; off <= 0x1C; off += 4)
            poc_log("  +%02x=%08lx", off, (unsigned long)REG32(VDEC_CORE + off));
        poc_log("  +34=%08lx +9C=%08lx +A0=%08lx",
                (unsigned long)REG32(VDEC_CORE+0x34),
                (unsigned long)REG32(VDEC_CORE+0x9C),
                (unsigned long)REG32(VDEC_CORE+0xA0));
    }
    poc_log("SUB:");
    {
        int off;
        for (off = 0; off <= 0x80; off += 4)
            poc_log("  +%02x=%08lx", off, (unsigned long)REG32(VDEC_SUB + off));
    }
    poc_log("DMA:");
    {
        int off;
        for (off = 0; off <= 0x20; off += 4)
            poc_log("  +%02x=%08lx", off, (unsigned long)REG32(VDEC_DMA + off));
        poc_log("  +100=%08lx +104=%08lx +10c=%08lx +110=%08lx",
                (unsigned long)REG32(VDEC_DMA+0x100),
                (unsigned long)REG32(VDEC_DMA+0x104),
                (unsigned long)REG32(VDEC_DMA+0x10C),
                (unsigned long)REG32(VDEC_DMA+0x110));
    }
    poc_log("XFORM: +800=%08lx +804=%08lx +808=%08lx",
            (unsigned long)XFORM_800,
            (unsigned long)REG32(VDEC_XFORM+0x804),
            (unsigned long)XFORM_808);
    poc_log("DEBLK: +00=%08lx +0c=%08lx +10=%08lx +14=%08lx",
            (unsigned long)REG32(VDEC_DEBLK),
            (unsigned long)REG32(VDEC_DEBLK+0x0C),
            (unsigned long)REG32(VDEC_DEBLK+0x10),
            (unsigned long)REG32(VDEC_DEBLK+0x14));
    lflush();

    /* ---- Display Y buffer on LCD ---- */
    poc_log("--- Displaying Y buffer on LCD ---");
    lflush();
    {
        fb_data *frame_rgb = (fb_data *)(void *)work_buf1;
        int px;

        for (px = 0; px < TEST_WIDTH * TEST_HEIGHT; px++) {
            uint8_t luma = out_y[px];
            frame_rgb[px] = ((luma >> 3) << 11)
                          | ((luma >> 2) << 5)
                          | (luma >> 3);
        }

        rb->lcd_clear_display();
        rb->lcd_bitmap(frame_rgb, 0, 0, TEST_WIDTH, TEST_HEIGHT);
        rb->lcd_update();

        poc_log("  LCD updated, waiting for button press...");
        lflush();
        while (rb->button_get(true) == BUTTON_NONE) {}
    }

    /* ---- Cleanup ---- */
    vdec_power_off();
    poc_log("=== v25 done ===");
    lflush();
    if (log_fd >= 0) rb->close(log_fd);

    rb->splashf(HZ*3, "v25 done");
    return PLUGIN_OK;
}
