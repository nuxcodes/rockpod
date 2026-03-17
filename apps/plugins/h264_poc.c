/***************************************************************************
 * S5L8702 H.264 Hardware Video Decoder - PoC v16
 *
 * Full hardware decode of a 128x128 I-frame with:
 *   - Gradient test frame (per-MB gray levels for tiling analysis)
 *   - Binary buffer dump (/vdec_y.bin, /vdec_cbcr.bin)
 *   - Software LCD display (Y→RGB565 grayscale)
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

/* 128x128 gradient IDR I-frame — baseline profile, CAVLC, QP 23
 * Generated: ffmpeg -f lavfi -i "nullsrc=s=128x128:r=1,
 *   geq=lum='clip(trunc(X/16)*4+trunc(Y/16)*32,0,255)':cb=128:cr=128"
 *   -frames:v 1 -pix_fmt yuv420p -c:v libx264 -profile:v baseline -level 1.1
 *   -g 1 -bf 0 -refs 0 -qp 26
 *   -x264-params "no-deblock=1:no-cabac=1:aq-mode=0" -f h264
 *
 * Each MB has a distinct gray level: MB(row,col) ≈ row*32 + col*4
 * This allows identification of macroblock tiling order in the output buffer.
 *
 * NAL header byte (0x65) + slice payload.
 * Slice header: 24 bits, slice_type=7(I), QP=23, deblocking=off
 * PPS: chroma_qp_index_offset=-2, pic_init_qp=26
 * 64 macroblocks (8x8), all I16x16.
 */
static const uint8_t test_nal[] = {
    0x65, 0x88, 0x84, 0x3a, 0x26, 0x28, 0x00, 0x08,
    0xd3, 0xc9, 0x8a, 0x00, 0x64, 0xc5, 0x00, 0x32,
    0x62, 0x80, 0x19, 0x31, 0x40, 0x0c, 0x98, 0xa0,
    0x06, 0x4c, 0x50, 0x03, 0x26, 0x28, 0x01, 0xac,
    0x50, 0x00, 0x10, 0x50, 0xbc, 0x50, 0x03, 0x78,
    0xa0, 0x06, 0xf1, 0x40, 0x0d, 0xe2, 0x80, 0x1b,
    0xc5, 0x00, 0x37, 0x8a, 0x00, 0x6f, 0x14, 0x00,
    0xd6, 0x28, 0x00, 0x08, 0x28, 0x5e, 0x28, 0x01,
    0xbc, 0x50, 0x03, 0x78, 0xa0, 0x06, 0xf1, 0x40,
    0x0d, 0xe2, 0x80, 0x1b, 0xc5, 0x00, 0x37, 0x8a,
    0x00, 0x6b, 0x14, 0x00, 0x04, 0x14, 0x2f, 0x14,
    0x00, 0xde, 0x28, 0x01, 0xbc, 0x50, 0x03, 0x78,
    0xa0, 0x06, 0xf1, 0x40, 0x0d, 0xe2, 0x80, 0x1b,
    0xc5, 0x00, 0x35, 0x8a, 0x00, 0x02, 0x0a, 0x17,
    0x8a, 0x00, 0x6f, 0x14, 0x00, 0xde, 0x28, 0x01,
    0xbc, 0x50, 0x03, 0x78, 0xa0, 0x06, 0xf1, 0x40,
    0x0d, 0xe2, 0x80, 0x1a, 0xc5, 0x00, 0x01, 0x05,
    0x0b, 0xc5, 0x00, 0x37, 0x8a, 0x00, 0x6f, 0x14,
    0x00, 0xde, 0x28, 0x01, 0xbc, 0x50, 0x03, 0x78,
    0xa0, 0x06, 0xf1, 0x40, 0x0d, 0x62, 0x80, 0x00,
    0x82, 0x85, 0xe2, 0x80, 0x1b, 0xc5, 0x00, 0x37,
    0x8a, 0x00, 0x6f, 0x14, 0x00, 0xde, 0x28, 0x01,
    0xbc, 0x50, 0x03, 0x78, 0xa0, 0x06, 0xb1, 0x40,
    0x00, 0x41, 0x42, 0xf1, 0x40, 0x0d, 0xe2, 0x80,
    0x1b, 0xc5, 0x00, 0x37, 0x8a, 0x00, 0x6f, 0x14,
    0x00, 0xde, 0x28, 0x01, 0xbc, 0x50, 0x03, 0x80
};

#define TEST_NAL_SIZE     208
#define SLICE_HDR_BITS    32  /* 8 (NAL header) + 24 (slice header) */
#define TEST_WIDTH_MBS    8
#define TEST_HEIGHT_MBS   8
#define TEST_WIDTH        128
#define TEST_HEIGHT       128
#define Y_PLANE_SIZE      0x8000      /* 32 KB */
#define CBCR_PLANE_SIZE   0x4000      /* 16 KB */

/* Buffer sizes from Apple firmware */
#define DMA_WORK_SIZE     0x400       /* 1 KB */
#define WORK_BUF_SIZE     0x20000     /* 128 KB each */
#define BS_PAD            0x200       /* extra padding for DMA end addr */

/* Cache-line alignment */
#define ALIGN32(x)   (((uintptr_t)(x) + 31) & ~31)
/* 4KB page alignment — hardware rounds ref/output addrs to 512B boundaries */
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

    /* CG16_SVID enable with PLL2 */
    cg = REG32(CLK_BASE + 0x08);
    cg &= ~0x80000000;
    cg |= 0x30000000;
    REG32(CLK_BASE + 0x08) = cg;
    rb->sleep(HZ/5);

    /* PWRCON(0) bits 14-16 + bit 18 */
    pw = PWRCON(0);
    PWRCON(0) = pw & ~((7 << 14) | (1 << 18));
    rb->sleep(HZ/5);

    /* MIU bus gate */
    REG32(0x38100000 + 0x314) &= ~1;

    /* IRQ clear writes (prevents bus hangs) */
    REG32(VDEC_MAIN + 0x1C) = 0xFFFFFFFF;
    REG32(VDEC_CORE)        = 0xFFFFFFFF;
    REG32(VDEC_CORE)        = 0xFFFFFFFF;
    REG32(VDEC_DMA + 0x100) = 0xFFFFFFFF;
    REG32(VDEC_DEBLK)       = 0xFFFFFFFF;
    REG32(VDEC_SUB)         = 0xFFFFFFFF;
    REG32(VDEC_MAIN)        = 0xFFFFFFFF;

    /* Config writes (brings decoder out of reset) */
    REG32(VDEC_MAIN + 0x04) = 0x40;
    REG32(VDEC_MAIN + 0x0C) = 0;
    REG32(VDEC_MAIN + 0x10) = 0x10100;
    REG32(VDEC_SUB  + 0x04) = 2;
    REG32(VDEC_SUB  + 0x10) = 0x182;
    REG32(VDEC_DMA + 0x110) = 0x800;
    REG32(VDEC_XFORM+ 0x804)= 0x40;
    REG32(VDEC_DEBLK+ 0x10) = 0x10;
    REG32(VDEC_SUB  + 0x6C) = 0x10001;

    /* Note: Apple enables VIC1 interrupt 13 via its own IRQ framework.
     * 0x38E02004 = VIC1EDGE0 (edge config), not VICINTENABLE (0x38E01010).
     * Our write was a no-op. Removed — decoder status regs work without VIC. */
}

static void vdec_power_off(void)
{
    uint32_t cg;

    /* Clear decoder IRQs and reset */
    REG32(VDEC_MAIN + 0x1C) = 0xFFFFFFFF;
    REG32(VDEC_CORE)        = 0xFFFFFFFF;
    REG32(VDEC_DMA + 0x100) = 0xFFFFFFFF;
    REG32(VDEC_DEBLK)       = 0xFFFFFFFF;
    REG32(VDEC_SUB)         = 0xFFFFFFFF;
    REG32(VDEC_MAIN)        = 0xFFFFFFFF;

    /* Re-gate decoder + VPP clocks */
    PWRCON(0) |= (7 << 14) | (1 << 18);

    /* Close MIU bus gate */
    REG32(0x38100000 + 0x314) |= 1;

    /* Disable CG16_SVID */
    cg = REG32(CLK_BASE + 0x08);
    cg |= 0x80000000;
    cg &= ~0x30000000;
    REG32(CLK_BASE + 0x08) = cg;
}

/* ===================== PHASE 1: Reset/Clear ===================== */
static void vdec_decoder_config(void)
{
    poc_log("Phase 1: decoder_config (reset/clear)");
    REG32(VDEC_MAIN + 0x2C) = 0x02;
    REG32(VDEC_MAIN + 0x1C) = 0xFFFFFFFF;
    REG32(VDEC_MAIN + 0x0C) = 0x00;
    REG32(VDEC_CORE)        = 0xFFFFFFFF;
    REG32(VDEC_CORE)        = 0xFFFFFFFF;
    REG32(VDEC_DEBLK)       = 0xFFFFFFFF;
    REG32(VDEC_DMA + 0x100) = 0xFFFFFFFF;
    REG32(VDEC_SUB)         = 0xFFFFFFFF;
    REG32(VDEC_SUB)         = 0xFFFFFFFF;
}

/* ===================== PHASE 2: Bitstream Submit ===================== */
static void vdec_bitstream_submit(uint32_t bs_addr, uint32_t dma_addr,
                                  uint32_t work1_addr, uint32_t work2_addr)
{
    int i;
    poc_log("Phase 2: bitstream_submit");
    poc_log("  bs=%08lx dma=%08lx w1=%08lx w2=%08lx",
            (unsigned long)bs_addr, (unsigned long)dma_addr,
            (unsigned long)work1_addr, (unsigned long)work2_addr);

    REG32(VDEC_SUB + 0x18)  = bs_addr;
    REG32(VDEC_SUB + 0x20)  = dma_addr;
    /* SUB+24 is also a 20-bit register (wrote 0x083ED180, read 0x000ED180).
     * Write size/offset instead of absolute end address. */
    REG32(VDEC_SUB + 0x24)  = DMA_WORK_SIZE;  /* 0x400 offset */
    REG32(VDEC_SUB + 0x78)  = work1_addr;
    /* SUB+7C is a 20-bit register — stores offset from work1, not absolute.
     * Confirmed: wrote 0x0840D180, read back 0x0000D180 (upper bits lost).
     * Apple's buffers are contiguous: work2 = work1 + 0x20000. */
    REG32(VDEC_SUB + 0x7C)  = WORK_BUF_SIZE;  /* 0x20000 offset */
    REG32(VDEC_SUB + 0x80)  = 0;
    REG32(VDEC_MAIN + 0x10) = 0x80000000;
    REG32(VDEC_CORE + 0x1C) = 1;
    REG32(VDEC_MAIN + 0x04) = 0x142;
    REG32(VDEC_DMA + 0x104) = 0x14;
    REG32(VDEC_CORE + 0x04) = 0x3F;
    REG32(VDEC_SUB  + 0x04) = 0x12;

    /* Default scaling matrices (all 16s — identity for baseline) */
    for (i = 0; i < 64; i++) {
        REG32(VDEC_XFORM + 0x200 + i * 4) = 16;
        REG32(VDEC_XFORM + 0x300 + i * 4) = 16;
    }
    poc_log("  scaling matrices written");
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
static void vdec_refframe_set(uint32_t y_addr, uint32_t cbcr_addr)
{
    poc_log("Phase 4: refframe_set Y=%08lx C=%08lx",
            (unsigned long)y_addr, (unsigned long)cbcr_addr);

    /* Slot A (reference — use output buffer for I-frame;
     * NULL (0) causes RefDMA to read from addr 0 = boot vectors) */
    REG32(VDEC_SUB + 0x2C) = y_addr;
    REG32(VDEC_SUB + 0x30) = 0;
    REG32(VDEC_SUB + 0x34) = cbcr_addr;
    REG32(VDEC_SUB + 0x38) = 0;

    /* Slot B (output) */
    REG32(VDEC_SUB + 0x3C) = y_addr;
    REG32(VDEC_SUB + 0x40) = 0;
    REG32(VDEC_SUB + 0x44) = cbcr_addr;
    REG32(VDEC_SUB + 0x48) = 0;
}

/* ===================== PHASE 5: Slice Submit ===================== */
static void vdec_slice_submit(uint32_t bs_addr, int bs_len, int bit_offset)
{
    int log2_mbs, total_mbs, tmp;
    uint32_t dma_start, dma_end;

    poc_log("Phase 5: slice_submit bs=%08lx len=%d hdr_bits=%d",
            (unsigned long)bs_addr, bs_len, bit_offset);
    lflush();

    /* Transform config: chroma_qp_index_offset=-2 (0xFE as ubyte), | 0x20 */
    REG32(VDEC_XFORM + 0x804) = (0xFE << 8) | 0x20;
    poc_log("  XFORM+804=0xFE20 OK");

    /* Deblocking mode: 0xC02 | deblk_idc<<2, idc=1 (disabled) */
    REG32(VDEC_DEBLK + 0x10) = 0x0C02 | (1 << 2);
    poc_log("  DEBLK+10=0xC06 OK");
    lflush();

    /* Slice config composite: slice_type=2 (I), all other fields=0 */
    poc_log("  Writing CORE+10...");
    lflush();
    REG32(VDEC_CORE + 0x10) = 0x00000002;
    poc_log("  CORE+10=2 OK");

    poc_log("  Writing CORE+14...");
    lflush();
    REG32(VDEC_CORE + 0x14) = 23;
    poc_log("  CORE+14=23 OK");

    poc_log("  Writing CORE+34...");
    lflush();
    REG32(VDEC_CORE + 0x34) = 3;
    poc_log("  CORE+34=3 OK");

    poc_log("  Writing CORE+9C...");
    lflush();
    total_mbs = TEST_WIDTH_MBS * TEST_HEIGHT_MBS;
    log2_mbs = 0;
    for (tmp = total_mbs; tmp != 0; tmp >>= 1)
        log2_mbs++;
    REG32(VDEC_CORE + 0x9C) = (0 << 8) | (log2_mbs << 4) | 1;
    poc_log("  CORE+9C=0x%02x OK", (log2_mbs << 4) | 1);

    poc_log("  Writing CORE+A0..A8...");
    lflush();
    REG32(VDEC_CORE + 0xA0) = 0;
    REG32(VDEC_CORE + 0xA4) = 0;
    REG32(VDEC_CORE + 0xA8) = 0;
    poc_log("  CORE+A0..A8=0 OK");

    /* DMA pre-config */
    REG32(VDEC_DMA + 0x100) = 0x20;
    poc_log("  DMA+100=0x20 OK");
    lflush();

    /* DECODE TRIGGER */
    poc_log("  -> DECODE TRIGGER (CORE=4)");
    lflush();
    REG32(VDEC_CORE) = 4;
    poc_log("  CORE=4 OK");

    /* Clear bitstream DMA status */
    REG32(VDEC_SUB) = 0xFFFFFFFF;
    poc_log("  SUB cleared OK");

    /* DMA source addresses */
    dma_start = bs_addr + (bit_offset / 8);
    dma_end   = bs_addr + bs_len + 0x10;  /* tight padding, not +0x103 */
    poc_log("  DMA start=%08lx end=%08lx", (unsigned long)dma_start, (unsigned long)dma_end);
    lflush();
    REG32(VDEC_SUB + 0x18) = dma_start;
    REG32(VDEC_SUB + 0x1C) = dma_end;
    poc_log("  DMA addrs written OK");

    /* DMA KICK */
    poc_log("  -> DMA KICK (DMA+110=2)");
    lflush();
    REG32(VDEC_DMA + 0x110) = 2;
    poc_log("  DMA KICK OK");

    /* SUB+0C = 3 MUST come AFTER DMA kick but BEFORE alignment reads.
     * Apple's firmware calls FUN_00083620(1,3) here, inside slice_submit.
     * This enables the DMA pipeline so alignment reads don't hang. */
    REG32(VDEC_SUB + 0x0C) = 3;
    poc_log("  SUB+0C=3 OK");
    lflush();

    /* Alignment barrier reads — synchronize DMA engine with byte offset.
     * Each read from DMA+0x20 advances the internal pointer by 1 byte. */
    {
        int align_count = dma_start & 3;
        int bit_align = bit_offset % 8;
        volatile uint32_t dummy;
        int j;
        poc_log("  align: %d byte reads + %d bit reads", align_count, bit_align);
        lflush();
        for (j = 0; j < align_count; j++)
            dummy = REG32(VDEC_DMA + 0x20);
        for (j = 0; j < bit_align; j++)
            dummy = REG32(VDEC_DMA + 0x04);
        (void)dummy;
        poc_log("  align reads OK");
    }
}

/* ===================== PHASE 6: Top-level Trigger ===================== */
static void vdec_trigger(void)
{
    /* Apple's sequence: CORE+0C=0x0C immediately after slice_submit.
     * No waiting for RefDMA first — Apple does this without any delay. */
    poc_log("Phase 6: CORE+0C=0x0C (immediate trigger)");
    REG32(VDEC_CORE + 0x0C) = 0x0C;
}

/* ===================== STATUS POLLING ===================== */
static int vdec_poll_completion(int timeout_ticks)
{
    int i;
    uint32_t sub, top, core;
    uint32_t prev_sub = 0, prev_top = 0;
    int got_frame_done = 0;

    for (i = 0; i < timeout_ticks; i++) {
        sub = REG32(VDEC_SUB);
        top = REG32(VDEC_MAIN);

        /* Detect ANY new status bits in SUB or MAIN */
        if ((sub & 0x1FF) != (prev_sub & 0x1FF) ||
            (top & 0x1FF) != (prev_top & 0x1FF)) {

            poc_log("  status[%d]: top=%08lx sub=%08lx",
                    i, (unsigned long)top, (unsigned long)sub);

            if (sub & 0x40) {
                poc_log("  *** FRAME DONE! ***");
                got_frame_done = 1;
            }
            if (sub & 0x10) poc_log("  -> Output DMA done");
            if (sub & 0x02) poc_log("  -> Ref DMA done");
            if (sub & 0x01) poc_log("  -> Slice done");
            if (sub & 0x100) poc_log("  -> ERROR/overflow bit");
            if (top & 0x100) poc_log("  -> Core IRQ (ack)");

            /* Full Apple-style IRQ ack (FUN_0004c758).
             * MUST clear CORE+00 when MAIN has bit 0x100 —
             * the core waits for this ack before continuing. */
            if (top & 0x100)
                REG32(VDEC_CORE) = 0xFFFFFFFF;
            if (top & 0x02)
                REG32(VDEC_DMA + 0x100) = 0xFFFFFFFF;
            REG32(VDEC_SUB) = 0xFFFFFFFF;
            REG32(VDEC_MAIN) = 0xFFFFFFFF;

            prev_sub = 0;
            prev_top = 0;

            if (got_frame_done)
                return 1;

            lflush();
        } else {
            prev_sub = sub;
            prev_top = top;
        }

        if (i < 200000)
            ; /* tight loop */
        else
            rb->yield();
    }

    /* Timeout — dump final state */
    sub = REG32(VDEC_SUB);
    top = REG32(VDEC_MAIN);
    core = REG32(VDEC_CORE);
    poc_log("  TIMEOUT: top=%08lx sub=%08lx core=%08lx",
            (unsigned long)top, (unsigned long)sub, (unsigned long)core);
    return 0;
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
    uint8_t *bs_buf;
    uint32_t v;
    int ok, i;

    rb->splash(HZ/2, "v16 START");

    log_fd = rb->open(LOG_PATH, O_WRONLY|O_CREAT|O_TRUNC, 0666);
    poc_log("=== v16 — H.264 decode test (128x128) ===");

    /* ---- Allocate buffers from audio buffer ---- */
    buf = rb->plugin_get_audio_buffer(&buf_size);
    poc_log("Audio buffer: %08lx size=%lu",
            (unsigned long)(uintptr_t)buf, (unsigned long)buf_size);

    if (buf_size < DMA_WORK_SIZE + 2 * WORK_BUF_SIZE +
                   2 * Y_PLANE_SIZE + 2 * CBCR_PLANE_SIZE +
                   TEST_NAL_SIZE + BS_PAD + 0x2000) {
        poc_log("ERROR: buffer too small");
        if (log_fd >= 0) rb->close(log_fd);
        rb->splash(HZ*3, "Buffer too small!");
        return PLUGIN_ERROR;
    }

    p = (uint8_t *)ALIGN32(buf);

    dma_work  = p;                        p += DMA_WORK_SIZE;
    work_buf1 = (uint8_t *)ALIGN32(p);    p = work_buf1 + WORK_BUF_SIZE;
    work_buf2 = (uint8_t *)ALIGN32(p);    p = work_buf2 + WORK_BUF_SIZE;
    /* All frame buffers 4KB-aligned — hardware rounds addrs to 512B */
    ref_y     = (uint8_t *)ALIGN4K(p);    p = ref_y + Y_PLANE_SIZE;
    ref_cbcr  = (uint8_t *)ALIGN4K(p);    p = ref_cbcr + CBCR_PLANE_SIZE;
    out_y     = (uint8_t *)ALIGN4K(p);    p = out_y + Y_PLANE_SIZE;
    out_cbcr  = (uint8_t *)ALIGN4K(p);    p = out_cbcr + CBCR_PLANE_SIZE;
    bs_buf    = (uint8_t *)ALIGN32(p);

    /* Zero DMA working buffers */
    rb->memset(dma_work, 0, DMA_WORK_SIZE);
    /* Fill work_buf1 with sentinel to detect if core writes here */
    rb->memset(work_buf1, 0xBB, WORK_BUF_SIZE);
    rb->memset(work_buf2, 0, WORK_BUF_SIZE);

    /* Reference frame: zero (black) — SEPARATE from output */
    rb->memset(ref_y, 0, Y_PLANE_SIZE);
    rb->memset(ref_cbcr, 0x80, CBCR_PLANE_SIZE); /* 0x80 = neutral chroma */

    /* Pre-fill output with sentinel pattern */
    rb->memset(out_y, 0xAA, Y_PLANE_SIZE);
    rb->memset(out_cbcr, 0xAA, CBCR_PLANE_SIZE);

    /* Copy NAL payload to aligned buffer with padding */
    rb->memset(bs_buf, 0, TEST_NAL_SIZE + BS_PAD);
    rb->memcpy(bs_buf, test_nal, TEST_NAL_SIZE);

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
    poc_log("  bs_buf=%08lx (NAL %d bytes + %d pad)",
            (unsigned long)(uintptr_t)bs_buf, TEST_NAL_SIZE, BS_PAD);
    lflush();

    /* ---- Power on decoder ---- */
    poc_log("--- Power on ---");
    rb->splash(HZ/4, "Power on...");
    vdec_power_on();

    /* Verify decoder is alive (from v11g) */
    v = REG32(VDEC_MAIN + 0x08);
    poc_log("MAIN+08 = %08lx (hw status)", (unsigned long)v);
    if (v == 0)
        poc_log("WARNING: MAIN+08 is 0, decoder may not be powered");
    lflush();

    /* ---- Flush cache before DMA ---- */
    poc_log("Flushing cache...");
    rb->commit_dcache();

    /* ---- Read status BEFORE decode ---- */
    poc_log("--- Status BEFORE decode ---");
    poc_log("  MAIN+00=%08lx SUB+00=%08lx",
            (unsigned long)REG32(VDEC_MAIN),
            (unsigned long)REG32(VDEC_SUB));
    lflush();

    /* ---- Execute decode sequence ---- */
    rb->splash(HZ/4, "Decoding...");

    vdec_decoder_config();
    poc_log("  [post-P1] MAIN=%08lx SUB=%08lx CORE=%08lx",
            (unsigned long)REG32(VDEC_MAIN),
            (unsigned long)REG32(VDEC_SUB),
            (unsigned long)REG32(VDEC_CORE));
    lflush();

    vdec_bitstream_submit(
        (uint32_t)(uintptr_t)bs_buf,
        (uint32_t)(uintptr_t)dma_work,
        (uint32_t)(uintptr_t)work_buf1,
        (uint32_t)(uintptr_t)work_buf2
    );
    poc_log("  [post-P2] MAIN=%08lx SUB=%08lx CORE=%08lx",
            (unsigned long)REG32(VDEC_MAIN),
            (unsigned long)REG32(VDEC_SUB),
            (unsigned long)REG32(VDEC_CORE));
    lflush();

    vdec_output_config(TEST_WIDTH_MBS, TEST_HEIGHT_MBS);
    poc_log("  [post-P3] MAIN=%08lx SUB=%08lx CORE=%08lx",
            (unsigned long)REG32(VDEC_MAIN),
            (unsigned long)REG32(VDEC_SUB),
            (unsigned long)REG32(VDEC_CORE));
    lflush();

    /* Slot A = reference (separate zeroed buffer) */
    vdec_refframe_set(
        (uint32_t)(uintptr_t)ref_y,
        (uint32_t)(uintptr_t)ref_cbcr
    );
    /* Slot B = output (sentinel-filled) */
    REG32(VDEC_SUB + 0x3C) = (uint32_t)(uintptr_t)out_y;
    REG32(VDEC_SUB + 0x40) = 0;
    REG32(VDEC_SUB + 0x44) = (uint32_t)(uintptr_t)out_cbcr;
    REG32(VDEC_SUB + 0x48) = 0;
    poc_log("Phase 4: ref Y=%08lx C=%08lx, out Y=%08lx C=%08lx",
            (unsigned long)(uintptr_t)ref_y,
            (unsigned long)(uintptr_t)ref_cbcr,
            (unsigned long)(uintptr_t)out_y,
            (unsigned long)(uintptr_t)out_cbcr);
    poc_log("  [post-P4] MAIN=%08lx SUB=%08lx CORE=%08lx",
            (unsigned long)REG32(VDEC_MAIN),
            (unsigned long)REG32(VDEC_SUB),
            (unsigned long)REG32(VDEC_CORE));
    lflush();

    /* Flush cache — all register writes and buffer data visible to HW */
    rb->commit_dcache();

    vdec_slice_submit(
        (uint32_t)(uintptr_t)bs_buf,
        TEST_NAL_SIZE,
        SLICE_HDR_BITS
    );

    /* Phase 6 IMMEDIATELY after Phase 5 — Apple does this without delay */
    vdec_trigger();

    /* ---- Poll for completion ---- */
    poc_log("--- Polling for FrameDone (200K tight + 5K yield) ---");
    lflush();

    ok = vdec_poll_completion(205000);

    if (!ok) {
        poc_log("TIMEOUT: no status change after 5000 iterations");

        /* Read status anyway */
        poc_log("  MAIN+00=%08lx SUB+00=%08lx",
                (unsigned long)REG32(VDEC_MAIN),
                (unsigned long)REG32(VDEC_SUB));
        poc_log("  MAIN+04=%08lx MAIN+08=%08lx MAIN+0C=%08lx",
                (unsigned long)REG32(VDEC_MAIN + 0x04),
                (unsigned long)REG32(VDEC_MAIN + 0x08),
                (unsigned long)REG32(VDEC_MAIN + 0x0C));
        poc_log("  CORE+00=%08lx CORE+34=%08lx",
                (unsigned long)REG32(VDEC_CORE),
                (unsigned long)REG32(VDEC_CORE + 0x34));
        poc_log("  DMA+100=%08lx DMA+110=%08lx",
                (unsigned long)REG32(VDEC_DMA + 0x100),
                (unsigned long)REG32(VDEC_DMA + 0x110));
    }
    lflush();

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

            /* Dump around the changed region */
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

        /* Scan work_buf1 for decoded data (filled with 0xBB sentinel) */
        {
            int work_changed = 0;
            for (i = 0; i < WORK_BUF_SIZE; i++) {
                if (work_buf1[i] != 0xBB) work_changed++;
            }
            poc_log("work_buf1: %d/%d bytes changed from 0xBB", work_changed, WORK_BUF_SIZE);
            if (work_changed > 0 && work_changed < 64) {
                for (i = 0; i < WORK_BUF_SIZE; i++) {
                    if (work_buf1[i] != 0xBB) {
                        poc_log("  First change at work1+%04x: %02x", i, work_buf1[i]);
                        break;
                    }
                }
            }
        }

        /* Scan ref_y for changes (filled with 0x00) */
        {
            int ref_changed = 0;
            for (i = 0; i < Y_PLANE_SIZE; i++) {
                if (ref_y[i] != 0) ref_changed++;
            }
            poc_log("ref_y: %d/%d bytes changed from 0x00", ref_changed, Y_PLANE_SIZE);
        }
    }
    lflush();

    /* ---- Binary dump of output buffers ---- */
    poc_log("--- Dumping output buffers to files ---");
    {
        int dump_fd;
        ssize_t written;

        dump_fd = rb->open("/vdec_y.bin", O_WRONLY|O_CREAT|O_TRUNC, 0666);
        if (dump_fd >= 0) {
            written = rb->write(dump_fd, out_y, Y_PLANE_SIZE);
            rb->close(dump_fd);
            poc_log("  Y dump: %ld bytes to /vdec_y.bin", (long)written);
        } else {
            poc_log("  ERROR: cannot open /vdec_y.bin");
        }

        dump_fd = rb->open("/vdec_cbcr.bin", O_WRONLY|O_CREAT|O_TRUNC, 0666);
        if (dump_fd >= 0) {
            written = rb->write(dump_fd, out_cbcr, CBCR_PLANE_SIZE);
            rb->close(dump_fd);
            poc_log("  CbCr dump: %ld bytes to /vdec_cbcr.bin", (long)written);
        } else {
            poc_log("  ERROR: cannot open /vdec_cbcr.bin");
        }
    }
    lflush();

    /* ---- Comprehensive post-decode register dump ---- */
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
        poc_log("  +100=%08lx +104=%08lx +110=%08lx",
                (unsigned long)REG32(VDEC_DMA+0x100),
                (unsigned long)REG32(VDEC_DMA+0x104),
                (unsigned long)REG32(VDEC_DMA+0x110));
    }
    poc_log("XFORM: +804=%08lx", (unsigned long)REG32(VDEC_XFORM+0x804));
    poc_log("DEBLK: +00=%08lx +10=%08lx", (unsigned long)REG32(VDEC_DEBLK),
            (unsigned long)REG32(VDEC_DEBLK+0x10));
    poc_log("VIC1EDGE0: %08lx", (unsigned long)REG32(0x38E02004));
    lflush();

    /* ---- Display Y buffer on LCD (assumes raster order) ---- */
    poc_log("--- Displaying Y buffer on LCD ---");
    lflush();
    {
        /* Reuse work_buf1 (128KB, no longer needed) as RGB565 framebuffer */
        fb_data *frame_rgb = (fb_data *)(void *)work_buf1;
        int px;

        /* Convert Y to grayscale RGB565 */
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
        /* Wait for any button press before exiting */
        while (rb->button_get(true) == BUTTON_NONE) {}
    }

    /* ---- Cleanup ---- */
    vdec_power_off();
    poc_log("=== v16 done ===");
    lflush();
    if (log_fd >= 0) rb->close(log_fd);

    rb->splashf(HZ*3, "v16: %s", ok ? "STATUS!" : "timeout");
    return PLUGIN_OK;
}
