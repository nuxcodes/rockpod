/***************************************************************************
 * S5L8702 H.264 Hardware Video Decoder - PoC v30b
 *
 * HYBRID PATH TEST: SW coefficients → HW IDCT/deblock/MC
 *
 * v30b: Per-MB addressing + 3-sub-call architecture.
 * v30a proved NO auto-stride (only 1/64 MB positions had data).
 * Now: update SUB+2C/3C per sub-call with computed offset.
 * 3 sub-calls per MB: Y-top, Y-bottom, chroma.
 * De-tiling readback for LCD display.
 *
 * Based on decompilation of Apple's FUN_0009144c (per-MB writeback):
 *   - SUB+18/1C = coefficient buffer address (512 bytes per MB)
 *   - XFORM+800 = IDCT command (0x00020341 | is_chroma << 19)
 *   - XFORM+808 = IDCT status (poll bit 1 for ready)
 *   - DMA+10C   = DMA control (is_chroma << 3 | 0x31)
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
#define HW_STRIDE         (TEST_WIDTH_MBS * 32)  /* 256: HW output row stride */

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

/* ===================== PHASE 2: H.264 Init ===================== */
/* From Apple's FUN_0007e9e0 — uses H.264 hybrid path register values,
 * NOT the MPEG-4 path values from FUN_000692e0 that v25 used. */
static void vdec_h264_init(uint32_t dma_addr, uint32_t work1_addr)
{
    int i;
    poc_log("Phase 2: H.264 init (FUN_0007e9e0 sequence)");

    /* Second clear block — Apple does this in FUN_0007e9e0 after
     * calling FUN_0007476c.  Notably includes MAIN+0 = 0xFFFFFFFF
     * which FUN_0007476c does NOT write. */
    REG32(VDEC_MAIN + 0x1C) = 0xFFFFFFFF;
    REG32(VDEC_MAIN + 0x0C) = 0;
    REG32(VDEC_CORE)         = 0xFFFFFFFF;
    REG32(VDEC_CORE)         = 0xFFFFFFFF;
    REG32(VDEC_DMA + 0x100)  = 0xFFFFFFFF;
    REG32(VDEC_DEBLK)        = 0xFFFFFFFF;
    REG32(VDEC_SUB)          = 0xFFFFFFFF;
    REG32(VDEC_MAIN)         = 0xFFFFFFFF;

    /* H.264 config — from FUN_0007e9e0 via puVar8 = DAT_0007f470 (MAIN base).
     * MAIN+04 = 0x40 (SUB block only, no DMA output — H.264 uses SW readback).
     * SUB+10 = 0x182 (H.264 per-MB double buffer output mode, NOT 0x200 MPEG-4).
     * MAIN+10 = (mb_h << 16) | (mb_w << 8), SUB+6C = MAIN+10 - 0xFF. */
    {
        uint32_t main10 = ((uint32_t)TEST_HEIGHT_MBS << 16)
                        | ((uint32_t)TEST_WIDTH_MBS << 8);
        REG32(VDEC_MAIN + 0x04) = 0x40;
        REG32(VDEC_SUB  + 0x04) = 2;
        REG32(VDEC_SUB  + 0x10) = 0x182;
        REG32(VDEC_MAIN + 0x10) = main10;
        REG32(VDEC_DMA + 0x110) = 0x800;
        REG32(VDEC_XFORM+ 0x804)= 0x40;
        REG32(VDEC_DEBLK+ 0x10) = 0x10;
        REG32(VDEC_SUB  + 0x6C) = main10 - 0xFF;
    }

    poc_log("  MAIN+04=%08lx DEBLK+10=%08lx XFORM+804=%08lx",
            (unsigned long)REG32(VDEC_MAIN + 0x04),
            (unsigned long)REG32(VDEC_DEBLK + 0x10),
            (unsigned long)REG32(VDEC_XFORM + 0x804));

    /* Work buffer setup — kept for safety */
    REG32(VDEC_SUB + 0x20) = dma_addr;
    REG32(VDEC_SUB + 0x24) = DMA_WORK_SIZE;
    REG32(VDEC_SUB + 0x78) = work1_addr;
    REG32(VDEC_SUB + 0x7C) = WORK_BUF_SIZE;
    REG32(VDEC_SUB + 0x80) = 0;

    /* Scaling matrices (flat=16 for baseline H.264) */
    for (i = 0; i < 64; i++) {
        REG32(VDEC_XFORM + 0x200 + i * 4) = 16;
        REG32(VDEC_XFORM + 0x300 + i * 4) = 16;
    }
    poc_log("  scaling matrices written");
}

/* Phase 3 (vdec_output_config) REMOVED in v29 — it was overwriting H.264
 * register values with MPEG-4 values (SUB+10=0x200, wrong MAIN+10/SUB+6C).
 * Phase 2 now sets all dimension/mode registers correctly. */

/* ===================== PER-MB HARDWARE IDCT ===================== */
/* Implements Apple's FUN_0009144c register sequence.
 * Feeds 512-byte coefficient buffer to HW IDCT/deblock pipeline.
 * XFORM bit 19 is luma(0)/chroma(1) mode selector (confirmed from
 * FUN_0009144c reading plane index from ring entry field 0).
 * Returns 0 on success, -1 on timeout. */
static int hw_mb_submit(uint32_t coeff_buf_phys,
                        uint32_t ref_y, uint32_t out_y,
                        int frame_toggle, int is_chroma)
{
    int timeout;

    /* 1. Point SUB DMA to coefficient buffer (512 bytes) */
    REG32(VDEC_SUB + 0x18) = coeff_buf_phys;
    REG32(VDEC_SUB + 0x1C) = coeff_buf_phys + COEFF_BUF_SIZE;
    REG32(VDEC_SUB + 0x0C) = 3;

    /* 2. Set frame addresses (both always written, toggle selects active) */
    REG32(VDEC_SUB + 0x2C) = ref_y;
    REG32(VDEC_SUB + 0x3C) = out_y;

    /* 3. Wait for DEBLK ready, write control */
    timeout = 100000;
    while ((REG32(VDEC_DEBLK + 0x14) & 0x10000) && --timeout > 0) {}
    if (timeout == 0) {
        poc_log("  TIMEOUT: DEBLK+14=%08lx (bit16 stuck)",
                (unsigned long)REG32(VDEC_DEBLK + 0x14));
        return -1;
    }
    REG32(VDEC_DEBLK + 0x0C) = ((uint32_t)frame_toggle << 30) | 0x80;

    /* 4. XFORM IDCT command — 2 blocks, both same type.
     *    Apple's FUN_0009144c reads plane index from ring entries:
     *    luma blocks → both bit19=0, chroma blocks → both bit19=1 */
    {
        int p;
        for (p = 0; p < 2; p++) {
            timeout = 100000;
            while ((XFORM_808 & 2) && --timeout > 0) {}
            if (timeout == 0) {
                poc_log("  TIMEOUT: XFORM+808=%08lx (bit1 stuck, part %d)",
                        (unsigned long)XFORM_808, p);
                return -1;
            }
            XFORM_800 = XFORM_CMD_BASE | ((uint32_t)is_chroma << 19);
            DMA_10C = ((uint32_t)is_chroma << 3) | 0x31;
        }
    }
    return 0;
}

/* Fill 512-byte coefficient buffer with DC values for two 8×8 blocks.
 * Each block: 64 uint32 coefficients. Block 0 at [0], Block 1 at [64].
 * HW expects big-endian, so byte-swap DC values. */
static void fill_coeff_pair(uint32_t *buf, uint32_t dc0, uint32_t dc1)
{
    int i;
    for (i = 0; i < 128; i++)
        buf[i] = 0;
    buf[0]  = __builtin_bswap32(dc0);
    buf[64] = __builtin_bswap32(dc1);
}

/* Wait for HW completion + clear status (replaces inline wait code) */
static int wait_and_clear(void)
{
    int wait = 50000;
    while (!(REG32(VDEC_SUB) & 0x7F) && --wait > 0) {}
    REG32(VDEC_SUB)  = 0xFFFFFFFF;
    REG32(VDEC_MAIN) = 0xFFFFFFFF;
    return (wait > 0) ? 0 : -1;
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

    rb->splash(HZ/2, "v30b START");

    log_fd = rb->open(LOG_PATH, O_WRONLY|O_CREAT|O_TRUNC, 0666);
    poc_log("=== v30b — Per-MB addressing + 3-sub-call architecture (128x128) ===");

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
    rb->memset(ref_y, 0x00, Y_PLANE_SIZE);
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

    /* Phase 2: H.264 init (FUN_0007e9e0 register values, NOT MPEG-4) */
    vdec_h264_init(
        (uint32_t)(uintptr_t)dma_work,
        (uint32_t)(uintptr_t)work_buf1
    );
    /* v29: verify Phase 2 set the correct H.264 values (not MPEG-4) */
    poc_log("  [post-P2] MAIN+04=%08lx SUB+10=%08lx MAIN+10=%08lx SUB+6C=%08lx",
            (unsigned long)REG32(VDEC_MAIN + 0x04),
            (unsigned long)REG32(VDEC_SUB + 0x10),
            (unsigned long)REG32(VDEC_MAIN + 0x10),
            (unsigned long)REG32(VDEC_SUB + 0x6C));
    poc_log("  Expected: MAIN+04=0x40 SUB+10=0x182 MAIN+10=0x%08lx SUB+6C=0x%08lx",
            (unsigned long)(((uint32_t)TEST_HEIGHT_MBS << 16) | ((uint32_t)TEST_WIDTH_MBS << 8)),
            (unsigned long)(((uint32_t)TEST_HEIGHT_MBS << 16) | ((uint32_t)TEST_WIDTH_MBS << 8)) - 0xFF);
    lflush();

    /* Phase 3/4 REMOVED in v29 — Phase 3 was MPEG-4 output config that
     * overwrote SUB+10 to 0x200 (wrong mode). Phase 4 wrote extra SUB
     * registers that FUN_0009144c doesn't use. SUB+2C/3C are now set
     * per-MB in hw_mb_submit, matching Apple's FUN_0009144c exactly. */

    poc_log("Phase 3: per-MB decode (%d MBs, 3 sub-calls each)", total_mbs);
    poc_log("  HW_STRIDE=%d, XFORM luma=0x%08lx chroma=0x%08lx",
            HW_STRIDE,
            (unsigned long)XFORM_CMD_BASE,
            (unsigned long)(XFORM_CMD_BASE | (1u << 19)));
    lflush();

    /* Flush all buffer data to physical memory before DMA */
    rb->commit_discard_dcache();

    /* ---- Per-MB loop: 3 sub-calls per MB ---- */
    {
        int mb, frame_toggle = 0;
        int timeouts = 0;
        uint32_t coeff_phys = (uint32_t)(uintptr_t)coeff_buf;
        uint32_t *cb = (uint32_t *)(void *)coeff_buf;

        for (mb = 0; mb < total_mbs; mb++) {
            int mc = mb % TEST_WIDTH_MBS;
            int mr = mb / TEST_WIDTH_MBS;
            uint32_t y_base = (uint32_t)(uintptr_t)ref_y
                            + (uint32_t)(mr * 16 * HW_STRIDE + mc * 32);
            uint32_t c_base = (uint32_t)(uintptr_t)ref_cbcr
                            + (uint32_t)(mr * 8 * HW_STRIDE + mc * 32);
            uint32_t dc = (uint32_t)((mb + 1) * 2);

            /* Sub-call 1: Y-top (blocks Y0+Y1, rows 0-7) */
            fill_coeff_pair(cb, dc, dc);
            rb->commit_dcache();
            if (hw_mb_submit(coeff_phys, y_base, y_base,
                             frame_toggle, 0) < 0)
                timeouts++;
            wait_and_clear();
            frame_toggle ^= 1;

            /* Sub-call 2: Y-bottom (blocks Y2+Y3, rows 8-15) */
            fill_coeff_pair(cb, dc, dc);
            rb->commit_dcache();
            if (hw_mb_submit(coeff_phys,
                             y_base + 8 * HW_STRIDE,
                             y_base + 8 * HW_STRIDE,
                             frame_toggle, 0) < 0)
                timeouts++;
            wait_and_clear();
            frame_toggle ^= 1;

            /* Sub-call 3: Chroma (Cb+Cr blocks) */
            fill_coeff_pair(cb, 0, 0);
            rb->commit_dcache();
            if (hw_mb_submit(coeff_phys, c_base, c_base,
                             frame_toggle, 1) < 0)
                timeouts++;
            wait_and_clear();
            frame_toggle ^= 1;

            /* Log first 2 + last MB */
            if (mb < 2 || mb == total_mbs - 1) {
                poc_log("  MB[%d](%d,%d): y_base=%08lx SUB=%08lx DEBLK+14=%08lx",
                        mb, mc, mr, (unsigned long)y_base,
                        (unsigned long)REG32(VDEC_SUB),
                        (unsigned long)REG32(VDEC_DEBLK + 0x14));
            }

            if (timeouts >= 3) {
                poc_log("  ABORT: %d timeouts at MB %d", timeouts, mb);
                lflush();
                break;
            }
        }

        poc_log("  %d/%d MBs submitted (%d timeouts)", mb, total_mbs, timeouts);
    }
    lflush();

    /* ---- Check output buffers ---- */
    rb->commit_discard_dcache();
    poc_log("--- Output buffer check ---");
    {
        int ref_changed = 0;
        for (i = 0; i < Y_PLANE_SIZE; i++)
            if (ref_y[i] != 0x00) ref_changed++;
        poc_log("ref_y: %d/%d bytes non-zero", ref_changed, Y_PLANE_SIZE);

        int rc_changed = 0;
        for (i = 0; i < CBCR_PLANE_SIZE; i++)
            if (ref_cbcr[i] != 0x80) rc_changed++;
        poc_log("ref_cbcr: %d/%d bytes changed from 0x80", rc_changed, CBCR_PLANE_SIZE);
    }
    lflush();

    /* ---- MB position grid scan ---- */
    poc_log("--- MB position scan (HW_STRIDE=%d) ---", HW_STRIDE);
    {
        int mr, mc, mb_found = 0;
        for (mr = 0; mr < TEST_HEIGHT_MBS; mr++) {
            for (mc = 0; mc < TEST_WIDTH_MBS; mc++) {
                int off = mr * 16 * HW_STRIDE + mc * 32;
                int c = 0;
                for (i = 0; i < 16; i++)
                    if (ref_y[off + i] != 0x00) c++;
                if (c > 0) {
                    mb_found++;
                    if (mb_found <= 4 || mr == TEST_HEIGHT_MBS - 1)
                        poc_log("  MB(%d,%d) off=%d: %d/16 [0]=%02x [8]=%02x",
                                mc, mr, off, c, ref_y[off], ref_y[off + 8]);
                }
            }
        }
        poc_log("  Total: %d/%d MB positions with data", mb_found, total_mbs);
    }
    lflush();

    /* ---- Binary dump ---- */
    poc_log("--- Dumping ref_y to file ---");
    {
        int dump_fd;
        ssize_t written;
        dump_fd = rb->open("/vdec_refy.bin", O_WRONLY|O_CREAT|O_TRUNC, 0666);
        if (dump_fd >= 0) {
            written = rb->write(dump_fd, ref_y, Y_PLANE_SIZE);
            rb->close(dump_fd);
            poc_log("  ref_y dump: %ld bytes", (long)written);
        }
    }
    lflush();

    /* ---- Post-decode register dump ---- */
    poc_log("--- Post-decode register dump ---");
    poc_log("MAIN: +00=%08lx +04=%08lx +08=%08lx +10=%08lx +2C=%08lx",
            (unsigned long)REG32(VDEC_MAIN),
            (unsigned long)REG32(VDEC_MAIN + 0x04),
            (unsigned long)REG32(VDEC_MAIN + 0x08),
            (unsigned long)REG32(VDEC_MAIN + 0x10),
            (unsigned long)REG32(VDEC_MAIN + 0x2C));
    poc_log("SUB: +00=%08lx +10=%08lx +6C=%08lx +74=%08lx",
            (unsigned long)REG32(VDEC_SUB),
            (unsigned long)REG32(VDEC_SUB + 0x10),
            (unsigned long)REG32(VDEC_SUB + 0x6C),
            (unsigned long)REG32(VDEC_SUB + 0x74));
    poc_log("DEBLK: +00=%08lx +14=%08lx",
            (unsigned long)REG32(VDEC_DEBLK),
            (unsigned long)REG32(VDEC_DEBLK + 0x14));
    lflush();

    /* ---- De-tile HW buffer → LCD display ---- */
    poc_log("--- De-tiling ref_y → LCD ---");
    lflush();
    {
        fb_data *frame_rgb = (fb_data *)(void *)work_buf1;
        int px, py;

        for (py = 0; py < TEST_HEIGHT; py++) {
            for (px = 0; px < TEST_WIDTH; px++) {
                int mc = px / 16, mr = py / 16;
                int bc = (px / 8) & 1, br = (py / 8) & 1;
                int bx = px % 8, by = py % 8;
                int hw_off = (mr * 16 + br * 8 + by) * HW_STRIDE
                           + mc * 32 + bc * 8 + bx;
                /* Byte-swap within uint32 (HW stores big-endian pixels) */
                int wb = hw_off & ~3;
                int bi = 3 - (hw_off & 3);
                uint8_t pixel = ref_y[wb + bi];
                frame_rgb[py * TEST_WIDTH + px] = ((pixel >> 3) << 11)
                                                | ((pixel >> 2) << 5)
                                                | (pixel >> 3);
            }
        }

        rb->lcd_clear_display();
        rb->lcd_bitmap(frame_rgb, 0, 0, TEST_WIDTH, TEST_HEIGHT);
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

    /* ---- Cleanup ---- */
    vdec_power_off();
    poc_log("=== v30b done ===");
    lflush();
    if (log_fd >= 0) rb->close(log_fd);

    rb->splashf(HZ*3, "v30b done");
    return PLUGIN_OK;
}
