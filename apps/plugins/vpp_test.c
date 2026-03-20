/***************************************************************************
 * S5L8702 VPP Pipeline — Hardware Validation Test
 *
 * Tests the Video Post-Processing pipeline (CLCD scaler + display engine)
 * by pushing a synthetic YUV420 test pattern through hardware scaling
 * and verifying LCD output.
 *
 * See ipod-re/vpp_report.md for full register documentation.
 ****************************************************************************/

#include "plugin.h"

#ifdef IPOD_6G
#include "s5l87xx.h"

/* ---- VPP Register Definitions ---- */

/* Block A: CLCD Controller */
#define CLCD_BASE       0x39100000
#define CLCD_REG(off)   (*(volatile uint32_t *)(CLCD_BASE + (off)))

#define CLCD_CTRL       CLCD_REG(0x000)  /* bit 0=enable, bit 1=busy(RO) */
#define CLCD_Y_ADDR     CLCD_REG(0x028)  /* Y plane base address */
#define CLCD_CB_ADDR    CLCD_REG(0x02C)  /* Cb plane base address */
#define CLCD_CR_ADDR    CLCD_REG(0x030)  /* Cr plane base address */
#define CLCD_CLEAR34    CLCD_REG(0x034)
#define CLCD_SRC_BUF_W  CLCD_REG(0x03C)  /* source buffer width */
#define CLCD_SRC_BUF_H  CLCD_REG(0x040)  /* source buffer height */
#define CLCD_X_OFFSET   CLCD_REG(0x044)
#define CLCD_Y_OFFSET   CLCD_REG(0x048)
#define CLCD_OUT_W      CLCD_REG(0x04C)
#define CLCD_OUT_H      CLCD_REG(0x050)
#define CLCD_CROP_X     CLCD_REG(0x054)
#define CLCD_CROP_Y     CLCD_REG(0x058)
#define CLCD_SRC_W      CLCD_REG(0x05C)
#define CLCD_SRC_H      CLCD_REG(0x060)
#define CLCD_H_STEP     CLCD_REG(0x064)  /* Q23.9 */
#define CLCD_V_STEP     CLCD_REG(0x068)  /* Q24.8 */
#define CLCD_FILT_EN    CLCD_REG(0x200)
#define CLCD_FILT_COMMIT CLCD_REG(0x20C)
/* +0x3C0: dual-purpose — YUV_PLANE_MODE (per-frame) / pipeline start (init) */
#define CLCD_YUV_MODE   CLCD_REG(0x3C0)  /* 1=planar 4:2:0, 0=semi-planar */
#define CLCD_LUMA_STRIDE  CLCD_REG(0x3C4)
#define CLCD_CHROMA_STRIDE CLCD_REG(0x3C8)
#define CLCD_YUV_ENABLE CLCD_REG(0x3CC)  /* also pipeline commit during init */

/* Block B: Overlay Mixer */
#define MIXER_BASE      0x39200000
#define MIXER_REG(off)  (*(volatile uint32_t *)(MIXER_BASE + (off)))

#define MIXER_CTRL      MIXER_REG(0x000)
#define MIXER_L5_EN     MIXER_REG(0x004)  /* bit 0 = layer 5 (video) */
#define MIXER_PIXFMT    MIXER_REG(0x00C)  /* bits[11:8] */
#define MIXER_YUV_BIAS  MIXER_REG(0x048)
#define MIXER_CMATRIX0  MIXER_REG(0x080)
#define MIXER_CMATRIX1  MIXER_REG(0x084)
#define MIXER_CMATRIX2  MIXER_REG(0x088)
#define MIXER_ENABLE    MIXER_REG(0x800)

/* Block C: Display Engine */
#define DISP_BASE       0x39300000
#define DISP_REG(off)   (*(volatile uint32_t *)(DISP_BASE + (off)))

#define DISP_CTRL       DISP_REG(0x000)
#define DISP_MODE       DISP_REG(0x008)
#define DISP_OUTPUT     DISP_REG(0x00C)  /* 5=DE mode (LCD), 6=free-running */
#define DISP_ENABLE     DISP_REG(0x010)
#define DISP_CSC_MODE   DISP_REG(0x180)
#define DISP_CSC_Y      DISP_REG(0x184)
#define DISP_CSC_CBCR   DISP_REG(0x188)
#define DISP_CSC_OFS    DISP_REG(0x18C)
#define DISP_OUT_FMT    DISP_REG(0x1C0)
#define DISP_SOFT_RST   DISP_REG(0x280)
#define DISP_GAMMA_COMMIT DISP_REG(0x284)
#define DISP_LATCH      DISP_REG(0x3C0)
#define DISP_LCD_SEL    DISP_REG(0x3D0)
#define DISP_TRIGGER    DISP_REG(0x03C)

/* LCD MCU controller (for panel commands) */
#define LCD_CMD_MODE    0x80000c20
#define LCD_FRAME_MODE  0x80100db0

/* ---- Logging ---- */

static int log_fd = -1;

static void log_open(void)
{
    log_fd = rb->open("/vpp_test.log", O_WRONLY|O_CREAT|O_TRUNC, 0666);
}

static void vlog(const char *fmt, ...)
{
    static char buf[256];
    va_list ap;
    va_start(ap, fmt);
    rb->vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (log_fd >= 0)
        rb->fdprintf(log_fd, "%s\n", buf);
}

static void log_close(void)
{
    if (log_fd >= 0) { rb->close(log_fd); log_fd = -1; }
}

/* ---- LCD MCU command helpers (replicate displaylcd_setup) ---- */

static void vpp_lcd_wait(void)
{
    while (!(LCD_STATUS & 0x2));
    /* brief delay (udelay not in plugin API, volatile loop instead) */
    for (volatile int i = 0; i < 100; i++);
}

static void vpp_lcd_config(uint32_t config)
{
    vpp_lcd_wait();
    LCD_CON = config;
}

static void vpp_lcd_cmd(uint8_t cmd)
{
    while (LCD_STATUS & 0x10);
    LCD_WCMD = cmd;
}

static void vpp_lcd_data(uint8_t data)
{
    while (LCD_STATUS & 0x10);
    LCD_WDATA = data;
}

static void vpp_lcd_send(uint8_t cmd, int len, const uint8_t *data)
{
    vpp_lcd_cmd(cmd);
    for (int i = 0; i < len; i++)
        vpp_lcd_data(data[i]);
}

static void vpp_lcd_window(int x, int y, int w, int h)
{
    int xe = x + w - 1;
    int ye = y + h - 1;
    uint8_t col[] = { x >> 8, x & 0xff, xe >> 8, xe & 0xff };
    uint8_t row[] = { y >> 8, y & 0xff, ye >> 8, ye & 0xff };

    vpp_lcd_config(LCD_CMD_MODE);
    vpp_lcd_send(0x2a, 4, col);  /* CASET */
    vpp_lcd_send(0x2b, 4, row);  /* RASET */
    vpp_lcd_cmd(0x2c);         /* RAMWR */
    vpp_lcd_config(LCD_FRAME_MODE);
}

/* ---- VPP Power Control ---- */

static void vpp_clocks_enable(bool enable)
{
    uint32_t pwrcon = PWRCON(0);
    uint32_t mask = (1 << 14) | (1 << 15) | (1 << 16);

    if (enable)
        pwrcon &= ~mask;  /* clear bits = enable clocks */
    else
        pwrcon |= mask;   /* set bits = disable clocks */

    PWRCON(0) = pwrcon;
}

static void vpp_svid_enable(bool enable)
{
    /* CG16_SVID register format (16-bit at CLK_BASE + 0x0A):
     * bit 15:    disable (1=masked)
     * bits 13:12: clock source (3=PLL2)
     * bits 7:4:  div2 field (actual divisor = field + 1)
     * bits 3:0:  div1 field (actual divisor = field + 1)
     *
     * Apple uses: PLL2, div1=3, div2=4 → PLL2/12 ≈ 18 MHz
     * Encoded: (3<<12) | ((4-1)<<4) | (3-1) = 0x3032
     *
     * Rockbox boot disables with 0x8000.
     */
    if (enable) {
        /* Apple's IRAM function thunk_EXT_FUN_22001fe0(0xE, 3, 4):
         * Sets CG16_SVID = 0x3003: PLL2 source, div1=4 (field=3)
         * Then FUN_0036c428(0xE, 1) clears the disable bit.
         *
         * CG16_SVID may only have DIV1 (no DIV2) per clock tree diagram.
         * 0x3003 = PLL2 / 4 = 54 MHz
         */
        volatile uint32_t *cg32 = (volatile uint32_t *)(0x3C500008);
        uint32_t val = *cg32;
        val = (val & 0x0000FFFF) | (0x3003 << 16);
        *cg32 = val;
        /* Brief settle time instead of spin loop (spin caused v7 freeze) */
        for (volatile int i = 0; i < 10000; i++);
    } else {
        CG16_SVID |= (1 << 15);
    }
}

/* ---- VPP Block Init (replicate ROM init functions) ---- */

/* Polyphase filter coefficients from ROM data pool.
 * Vertical: 4-tap, 16-phase, packed unsigned 8-bit big-endian.
 * We use a minimal identity/passthrough set for testing. */

static void clcd_init(void)
{
    /* Reset control (preserve busy bit) */
    CLCD_CTRL = CLCD_CTRL & 2;

    /* Clear config registers */
    CLCD_REG(0x004) = 0;
    CLCD_REG(0x008) = 0;
    CLCD_REG(0x00C) = 0;

    /* Scale factors: 4.28 fixed-point, 1:1 default */
    CLCD_REG(0x028) = 0x8000000;
    CLCD_REG(0x02C) = 0x8000000;
    CLCD_REG(0x030) = 0x8000000;
    CLCD_REG(0x034) = 0x8000000;
    CLCD_REG(0x038) = 0x8000000;

    /* Filter config: tap count and precision per channel */
    CLCD_REG(0x03C) = 0x40;  /* luma filter taps */
    CLCD_REG(0x040) = 0x10;  /* luma precision */
    CLCD_REG(0x044) = 0;
    CLCD_REG(0x048) = 0;
    CLCD_REG(0x04C) = 0x40;  /* chroma Cb taps */
    CLCD_REG(0x050) = 0x10;
    CLCD_REG(0x054) = 0;
    CLCD_REG(0x058) = 0;
    CLCD_REG(0x05C) = 0x40;  /* chroma Cr taps */
    CLCD_REG(0x060) = 0x10;
    CLCD_REG(0x064) = 0x200; /* H_STEP default = 1.0 in Q23.9 */
    CLCD_REG(0x068) = 0x200; /* V_STEP default */

    /* Polyphase filter coefficients from Apple ROM data pool (FUN_00167288).
     * Extracted by agent from ROM — 16 horizontal + 24 vertical entries.
     * Previous versions used 0x00FF0000 (identity) which may cause
     * accumulator overflow (0xFF exceeds per-phase unity gain of ~0x40). */
    static const uint32_t h_coeff[16] = {
        0x00070707, 0x07070707, 0x07070707, 0x07000000,
        0x00020405, 0x06060606, 0x06050504, 0x03020101,
        0x007A7470, 0x6E6C6B6C, 0x6C6E7073, 0x76787B7E,
        0x7F7E7D79, 0x726B6359, 0x4F44392E, 0x23191008,
    };
    static const uint32_t v_coeff[24] = {
        0x003D3A38, 0x38383839, 0x3A3B3C3D, 0x3E3F3F00,
        0x7F7E7C76, 0x6F665C51, 0x463B3025, 0x1B130B05,
        0x00050B13, 0x1B25303B, 0x46515C66, 0x6F767C7E,
        0x00003F3F, 0x3E3D3C3B, 0x3A393838, 0x38383A3D,
        0x6B6B6D6F, 0x3336393D, 0x3F010203, 0x03030202,
        0xAAA69F95, 0x88786754, 0x412E1E0F, 0x0279726D,
    };
    for (int i = 0; i < 16; i++)
        CLCD_REG(0x06C + i * 4) = h_coeff[i];
    for (int i = 0; i < 24; i++)
        CLCD_REG(0x0EC + i * 4) = v_coeff[i];

    /* YUV mode/enable FIRST (Apple does these before coefficients) */
    CLCD_YUV_MODE = 1;    /* +0x3C0 */
    CLCD_YUV_ENABLE = 1;  /* +0x3CC */

    /* Filter/DMA engine config (Apple writes 0x200-0x238) */
    CLCD_FILT_EN = 1;           /* +0x200 = 1 (master enable) */
    CLCD_FILT_COMMIT = 0;       /* +0x20C = 0 (NOT 1! Apple writes 0) */
    CLCD_REG(0x210) = 0;        /* clear */
    CLCD_REG(0x218) = 0x80;     /* DMA FIFO threshold */
    CLCD_REG(0x21C) = 0x80000080; /* DMA bus tag: 0x80|(CLCD_BASE<<11) */
    CLCD_REG(0x220) = 0x80;     /* DMA threshold */
    CLCD_REG(0x224) = 0x80;
    CLCD_REG(0x228) = 0x80;
    CLCD_REG(0x22C) = 0x80;
    CLCD_REG(0x230) = 0x80;
    CLCD_REG(0x234) = 0x80;
    CLCD_REG(0x238) = 0;        /* clear */
}

static void mixer_init(void)
{
    /* Zero all layer registers */
    for (int i = 0x004; i <= 0x044; i += 4)
        MIXER_REG(i) = 0;
    for (int i = 0x04C; i <= 0x058; i += 4)
        MIXER_REG(i) = 0;

    /* YCbCr bias: Y=0x10, Cb=0x80, Cr=0x80 (BT.601) */
    MIXER_YUV_BIAS = 0x00108080;

    /* Color conversion matrix */
    MIXER_CMATRIX0 = 0x08440832;
    MIXER_CMATRIX1 = 0x3B4DACE1;
    MIXER_CMATRIX2 = 0x0E1D13DC;

    /* Global enable */
    MIXER_ENABLE = 1;

    /* Set initial control to 6 (without ENVID) */
    MIXER_CTRL = 6;
}

/* Display engine filter coefficients from ROM data pool 0x167e54 */
static const uint32_t disp_filter_coeff[14] = {
    0x00FD00FE, 0x00050004, 0x00F700FA, 0x000E000A,
    0x01EC01F2, 0x001D0014, 0x000001FE, 0x03D803E4,
    0x03B003C7, 0x00790056, 0x000003F6, 0x072C0766,
    0x028B0265, 0x04000ECC
};

static void disp_init_lcd(void)
{
    /* ===== Full FUN_00167c34 replication ===== */

    /* Reset (preserve busy bit only) */
    DISP_CTRL = DISP_CTRL & 2;

    /* DE (Data Enable) mode for LCD video — CRITICAL!
     * Value 5 = DE mode: DISP actively PULLS data from Mixer FIFO
     * Value 6 = free-running: generates sync but does NOT pull data
     * Using 6 caused CLCD stall (bit 2) in v1-v12 because DISP
     * never consumed data from the pipeline. */
    DISP_OUTPUT = 5;
    DISP_ENABLE = 1;

    /* Identity color matrix (S0.11 FP, 0x800 = 1.0) — 6 entries */
    for (int i = 0x01C; i <= 0x030; i += 4)
        DISP_REG(i) = 0x800;

    /* Timing/sync */
    DISP_REG(0x038) = 0;
    DISP_REG(0x03C) = 0x01000700;  /* critical config register */

    /* Gamma passthrough — zero the LUT area */
    DISP_REG(0x0F0) = 0;
    for (int i = 0x100; i <= 0x15C; i += 4)
        DISP_REG(i) = 0;

    /* CSC bypass (bit 4) */
    DISP_CSC_MODE = (DISP_CSC_MODE & 0xE0) | 0x10;
    DISP_CSC_Y = 0x800000;     /* 1.0 in 8.24 FP */
    DISP_CSC_CBCR = 0x800000;
    DISP_CSC_OFS = 0x80;       /* 128 for unsigned chroma */
    DISP_REG(0x190) = 0;

    /* CSC range (BT.601 Y:16-235, CbCr:16-240) */
    DISP_REG(0x194) = 0x0000EB10;
    DISP_REG(0x198) = 0x02000000;
    DISP_REG(0x19C) = 0x03FF0200;
    DISP_REG(0x1A0) = 0x1FF;
    DISP_REG(0x1A4) = 0x03FF0000;
    DISP_REG(0x1A8) = 0x1FF;

    /* Output pixel format */
    DISP_OUT_FMT = 0x11;

    /* Display filter coefficients (+0x200 through +0x234) */
    for (int i = 0; i < 14; i++)
        DISP_REG(0x200 + i * 4) = disp_filter_coeff[i];

    /* Zero remaining filter area */
    for (int i = 0x238; i <= 0x26C; i += 4)
        DISP_REG(i) = 0;

    /* Pipeline fundamental config (from FUN_00069994) */
    DISP_REG(0x014) = 0x0000440C;

    /* Clear pending, enable LCD select */
    DISP_REG(0x280) = 0;
    DISP_LATCH = 0;      /* +0x3C0 = 0 */
    DISP_LCD_SEL = 1;     /* +0x3D0 = 1 */

    /* Pipeline control registers (from FUN_00069994) */
    DISP_REG(0x3C4) = 0x00018000;
    DISP_REG(0x3C8) = 0x00000008;
    DISP_REG(0x3CC) = 0x00018000;
    DISP_REG(0x3D4) = 0x00000008;

    /* Gamma LUT (FUN_000c9fe0 param=2) — the LAST missing gap */
    /* Zero 3 banks first */
    for (int i = 0x044; i <= 0x06C; i += 4) DISP_REG(i) = 0;  /* 11 words */
    for (int i = 0x080; i <= 0x090; i += 4) DISP_REG(i) = 0;  /* 5 words */
    for (int i = 0x0C0; i <= 0x0D0; i += 4) DISP_REG(i) = 0;  /* 5 words */
    /* Gamma control register */
    DISP_REG(0x070) = 0x281;
    /* Gamma curve 1 (7 entries) */
    DISP_REG(0x094) = 1;
    DISP_REG(0x098) = 7;
    DISP_REG(0x09C) = 0x15;
    DISP_REG(0x0A0) = 0x2A;
    DISP_REG(0x0A4) = 0x44;
    DISP_REG(0x0A8) = 0x57;
    DISP_REG(0x0AC) = 0x5F;
    /* Gamma curve 2 (7 entries) */
    DISP_REG(0x0D4) = 2;
    DISP_REG(0x0D8) = 0x0A;
    DISP_REG(0x0DC) = 0x1D;
    DISP_REG(0x0E0) = 0x3C;
    DISP_REG(0x0E4) = 0x5F;
    DISP_REG(0x0E8) = 0x7B;
    DISP_REG(0x0EC) = 0x86;

    /* Apply gamma (commit) */
    DISP_GAMMA_COMMIT = 0;
}

static void disp_go_lcd(void)
{
    /* ===== FUN_001682cc replication: the GO sequence ===== */

    /* DISP_MODE bits (from 30 xrefs to 0x39300008 in ROM):
     *   bits 0-3: output format — FUN_00168180 (ROM 0x168194): bic #0xf, orr #0x2
     *   bit 4:    postproc type — FUN_00168240 (ROM 0x168258): bic #0x10 (0=LCD)
     *   bit 5:    NEVER SET by any Apple function. Stale boot values vary.
     *             v16 log: bit5=1 → DISP never went busy. bit5=0 → DISP busy.
     *   bit 9:    sync enable — FUN_001682cc (GO)
     *   bit 12:   pipeline enable — FUN_001682cc (GO)
     *
     * Apple's GO does: &= 0x3F (preserve 0-5), |= 0x200, |= 0x1000.
     * Problem: stale bit 5 is indeterminate. Use &= 0x1F to clear it.
     * We replicate FUN_00168180 (bit 1) + FUN_00168240 (bit 4 clear) inline. */
    DISP_MODE = (DISP_MODE & 0xFFFFFFF0) | 0x02;  /* FUN_00168180: set bit 1 */
    DISP_MODE &= ~0x10;          /* FUN_00168240: clear bit 4 (LCD mode) */
    DISP_MODE &= 0x1F;           /* GO step 1: clear bits 5-31 (NOT 0x3F — bit 5 must be 0) */
    DISP_MODE |= 0x200;          /* GO step 2: bit 9 sync enable */
    DISP_MODE |= 0x1000;         /* GO step 3: bit 12 pipeline enable */

    /* Interlace field select: 0 for progressive LCD, 0x200 for interlaced NTSC.
     * ROM 0x168378-0x168390: only writes 0x200 when state[2]==1 && state[3]==1.
     * For LCD: always 0. v13 incorrectly set 0x200 (reverted in v16). */
    DISP_REG(0x034) = 0;

    /* Color correction coefficients: CHIPID_REG_TWO bit 8 selects variant.
     * ROM 0x168354: ldr r2,[r7,#0x4] (CHIPID+4), ROM 0x168358: tst r2,#0x100.
     * Bytes: 042097e5 / 010c12e3. Two sets for chip silicon variants. */
    {
        uint32_t chipid2 = *(volatile uint32_t *)0x3D100004;
        if (chipid2 & 0x100) {
            /* CHIPID bit 8 set: variant B */
            DISP_REG(0x028) = 0x7B5;  /* base + 0x23 */
            DISP_REG(0x02C) = 0x7FF;  /* base + 0x6D */
            DISP_REG(0x030) = 0x792;  /* base */
        } else {
            /* CHIPID bit 8 clear: variant A */
            DISP_REG(0x028) = 0x79F;  /* base | (base >> 7) */
            DISP_REG(0x02C) = 0x7B9;  /* base + 0x27 */
            DISP_REG(0x030) = 0x79A;  /* base + 8 */
        }
    }

    /* GO: clear then enable. ROM 0x1683dc-0x1683e8. */
    DISP_CTRL = 0;
    DISP_CTRL |= 1;
}

/* ---- Test Pattern Generation ---- */

static void fill_yuv420_gradient(uint8_t *y, uint8_t *cb, uint8_t *cr,
                                  int w, int h)
{
    /* Horizontal luma gradient, neutral chroma */
    for (int row = 0; row < h; row++)
        for (int col = 0; col < w; col++)
            y[row * w + col] = (col * 255) / (w - 1);

    rb->memset(cb, 0x80, (w / 2) * (h / 2));
    rb->memset(cr, 0x80, (w / 2) * (h / 2));
}

/* ---- Main Test ---- */

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;

    log_open();
    vlog("=== VPP Pipeline Test ===");

    /* Get audio buffer for test frame data */
    size_t buf_size;
    uint8_t *buf = rb->plugin_get_audio_buffer(&buf_size);
    if (buf_size < 256 * 1024) {
        rb->splash(HZ * 2, "Buffer too small");
        log_close();
        return PLUGIN_ERROR;
    }

    /* Test dimensions */
    int src_w = 320, src_h = 240;  /* start with 1:1, no scaling */
    int out_w = 320, out_h = 240;

    /* Allocate planes (cacheline-aligned) */
    uint8_t *y_plane  = (uint8_t *)(((uintptr_t)buf + 31) & ~31);
    uint8_t *cb_plane = (uint8_t *)(((uintptr_t)(y_plane + src_w * src_h) + 31) & ~31);
    uint8_t *cr_plane = (uint8_t *)(((uintptr_t)(cb_plane + (src_w/2) * (src_h/2)) + 31) & ~31);

    vlog("Y  plane: 0x%08lx (%dx%d)", (unsigned long)y_plane, src_w, src_h);
    vlog("Cb plane: 0x%08lx (%dx%d)", (unsigned long)cb_plane, src_w/2, src_h/2);
    vlog("Cr plane: 0x%08lx (%dx%d)", (unsigned long)cr_plane, src_w/2, src_h/2);

    /* Generate test pattern */
    fill_yuv420_gradient(y_plane, cb_plane, cr_plane, src_w, src_h);
    rb->commit_dcache();
    vlog("Test pattern generated (gradient)");

    /* === Phase 1: Show splash, then take over LCD === */
    rb->splashf(HZ, "VPP Test: powering on...");
    rb->sleep(HZ / 5);  /* ensure splash DMA completes */

    /* === Phase 2: Enable VPP clocks + GPIO 7.1 === */
    vlog("Phase 2: Enabling clocks");

    uint32_t pwrcon_before = PWRCON(0);
    vlog("PWRCON0 before: 0x%08lx", (unsigned long)pwrcon_before);

    vpp_svid_enable(true);
    rb->sleep(HZ / 5);  /* PLL2 stabilize */
    vpp_clocks_enable(true);

    uint32_t pwrcon_after = PWRCON(0);
    vlog("PWRCON0 after:  0x%08lx", (unsigned long)pwrcon_after);
    vlog("Bits 14-16 cleared: %s",
         ((pwrcon_after & 0x1C000) == 0) ? "YES" : "NO");

    vlog("GPIO 7.1 SKIPPED (breaks ATA)");
    vlog("CG16_SVID = 0x%04x (expect 0x3003 = PLL2/4 = 54MHz)",
         (unsigned)CG16_SVID);

    /* Send panel RGB interface command (0xB0) — enables RGB input mode.
     * Type-1 panels get this during boot; type-0 may not.
     * Sending it unconditionally is harmless (idempotent). */
    static const uint8_t rgb_cfg[] = {
        0x3a, 0x3a, 0x80, 0x80, 0x0a, 0x0a, 0x0a, 0x0a,
        0x0a, 0x0a, 0x0a, 0x0a, 0x3c, 0x30, 0x0f, 0x00,
        0x01, 0x54, 0x06, 0x66, 0x66,
    };
    vpp_lcd_config(LCD_CMD_MODE);
    vpp_lcd_cmd(0xB0);
    for (int i = 0; i < 21; i++)
        vpp_lcd_data(rgb_cfg[i]);
    vlog("Panel 0xB0 (RGB interface) sent: 21 bytes");

    /* === Phase 3: Read back VPP registers (verify alive) === */
    vlog("Phase 3: Register readback");

    uint32_t clcd_ctrl = CLCD_CTRL;
    uint32_t mixer_ctrl = MIXER_CTRL;
    uint32_t disp_ctrl = DISP_CTRL;

    vlog("CLCD  CTRL (0x39100000): 0x%08lx %s",
         (unsigned long)clcd_ctrl,
         (clcd_ctrl == 0xFFFFFFFF) ? "DEAD!" : "alive");
    vlog("MIXER CTRL (0x39200000): 0x%08lx %s",
         (unsigned long)mixer_ctrl,
         (mixer_ctrl == 0xFFFFFFFF) ? "DEAD!" : "alive");
    vlog("DISP  CTRL (0x39300000): 0x%08lx %s",
         (unsigned long)disp_ctrl,
         (disp_ctrl == 0xFFFFFFFF) ? "DEAD!" : "alive");
    vlog("DISP  MODE (stale from Apple boot): 0x%08lx (bit5=%d)",
         (unsigned long)DISP_MODE, (DISP_MODE >> 5) & 1);

    if (clcd_ctrl == 0xFFFFFFFF || mixer_ctrl == 0xFFFFFFFF
        || disp_ctrl == 0xFFFFFFFF)
    {
        vlog("ERROR: One or more VPP blocks not responding!");
        vlog("Check clock gates. Aborting.");
        vpp_clocks_enable(false);
        vpp_svid_enable(false);
        log_close();
        rb->splashf(HZ * 3, "VPP blocks DEAD! Check log.");
        return PLUGIN_ERROR;
    }

    /* === Phase 4: Initialize all three blocks === */
    vlog("Phase 4: Initializing VPP blocks");

    clcd_init();
    vlog("  CLCD init done");

    mixer_init();
    vlog("  Mixer init done");

    disp_init_lcd();
    vlog("  Display engine init done");

    /* === Phase 4b: DISP GO (Apple does this during startup, BEFORE first frame) === */
    /* Apple's order: init blocks → modes → display config → layer enable → gamma → GO */
    /* Then per-frame: panel cmds → buffers → CLCD enable → mixer commit → latch */
    /* The GO must happen FIRST, with time for DISP state machine to start */
    MIXER_L5_EN = 0x07;  /* all layer enables (Apple: vtable[0x20] + FUN_00168180 + FUN_00168240) */
    DISP_GAMMA_COMMIT = 0;  /* Apple: FUN_000b3cf4 right before GO */
    disp_go_lcd();
    {
        uint32_t chipid2 = *(volatile uint32_t *)0x3D100004;
        vlog("  DISP GO done (DISP_CTRL=0x%08lx DISP_MODE=0x%08lx)",
             (unsigned long)DISP_CTRL, (unsigned long)DISP_MODE);
        vlog("  CHIPID_REG_TWO=0x%08lx (bit8=%d → variant %c)",
             (unsigned long)chipid2, (chipid2 >> 8) & 1,
             (chipid2 & 0x100) ? 'B' : 'A');
    }
    /* DISP_CTRL=0x01 is the normal running state (agent 3, ROM 0x166e00:
     * bit 1 = "stopped/drained" confirmation, NOT "busy"). DISP generates
     * sync continuously. Apple has no explicit delay before first frame. */

    /* Readback key registers to verify init */
    vlog("  CLCD  scale[0x28]=0x%08lx (expect 0x8000000)",
         (unsigned long)CLCD_REG(0x028));
    vlog("  CLCD  H_STEP=0x%08lx V_STEP=0x%08lx (expect 0x200)",
         (unsigned long)CLCD_H_STEP, (unsigned long)CLCD_V_STEP);
    vlog("  MIXER bias=0x%08lx (expect 0x00108080)",
         (unsigned long)MIXER_YUV_BIAS);
    vlog("  DISP  CSC_MODE=0x%08lx (expect bit4 set)",
         (unsigned long)DISP_CSC_MODE);
    vlog("  DISP  LCD_SEL=0x%08lx (expect 1)",
         (unsigned long)DISP_LCD_SEL);

    /* === Phase 5: Configure buffer addresses and scaling === */
    vlog("Phase 5: Configuring buffers and scaling");

    /* Pixel format: YUV 4:2:0 */
    MIXER_PIXFMT = (MIXER_PIXFMT & 0xFFFFF0FF) | 0x200;

    /* Buffer addresses */
    CLCD_Y_ADDR  = (uint32_t)y_plane;
    CLCD_CB_ADDR = (uint32_t)cb_plane;
    CLCD_CR_ADDR = (uint32_t)cr_plane;
    CLCD_CLEAR34 = 0;

    /* Source buffer dimensions */
    CLCD_SRC_BUF_W = src_w;
    CLCD_SRC_BUF_H = src_h;

    /* YUV mode and strides */
    CLCD_YUV_MODE    = 1;          /* planar 4:2:0 */
    CLCD_LUMA_STRIDE = src_w;
    CLCD_CHROMA_STRIDE = src_w / 2;
    CLCD_YUV_ENABLE  = 1;

    /* Scaler dimensions */
    CLCD_X_OFFSET = 0;
    CLCD_Y_OFFSET = 0;
    CLCD_OUT_W    = out_w;
    CLCD_OUT_H    = out_h;
    CLCD_CROP_X   = 0;
    CLCD_CROP_Y   = 0;
    CLCD_SRC_W    = src_w;
    CLCD_SRC_H    = src_h;

    /* Scale ratios (1:1 for this test) */
    CLCD_H_STEP = ((out_w << 12) / src_w) >> 3;  /* 512 for 1:1 */
    CLCD_V_STEP = ((out_h << 12) / src_h) >> 4;  /* 256 for 1:1 */

    vlog("  H_STEP=%lu V_STEP=%lu",
         (unsigned long)CLCD_H_STEP, (unsigned long)CLCD_V_STEP);
    vlog("  Y=0x%08lx Cb=0x%08lx Cr=0x%08lx",
         (unsigned long)CLCD_Y_ADDR,
         (unsigned long)CLCD_CB_ADDR,
         (unsigned long)CLCD_CR_ADDR);

    /* Layer enables already set in Phase 4b */

    /* === Phase 6: Per-frame trigger (DISP GO already done in Phase 4b) === */
    vlog("Phase 6: Per-frame trigger");

    /* Step A: vtable[0x58] equivalent — enable video layer + set alpha */
    /* vtable[0x58] does two things:
     * 1. MIXER+0x008 |= 0x10000 (bit 16 = video layer alpha enable)
     * 2. MIXER+0x008 = (val & ~0xFF) | 0xFF (alpha = fully opaque)
     * Net result: MIXER+0x008 = 0x100FF */
    MIXER_REG(0x008) |= 0x10000;
    MIXER_REG(0x008) = (MIXER_REG(0x008) & ~0xFF) | 0xFF;
    vlog("  MIXER+0x008 = 0x%08lx (expect 0x100FF)",
         (unsigned long)MIXER_REG(0x008));

    /* Step B: Disable MCU LCD controller before ENVID takes over LCD pins.
     * Apple keeps LCD clock gated OFF except when sending commands (ROM
     * 0x000bf0ec enables temporarily, 0x000c36bc restores = disabled).
     * Rockbox enables it permanently in lcd_init_device(). We must gate
     * it off before VPP's CLCD drives the shared LCD data pins.
     * CLOCKGATE_LCD = 1 → PWRCON(0) bit 1. Set = disabled. */
    while (!(LCD_STATUS & 0x2))
        ;  /* wait for MCU LCD idle */
    vlog("  LCD_CON before=0x%08lx", (unsigned long)LCD_CON);
    PWRCON(0) |= (1 << 1);  /* disable MCU LCD clock gate */
    vlog("  MCU LCD clock disabled (PWRCON bit 1 set)");

    /* Step C: Per-frame trigger — Apple's exact order from FUN_001669c8 case 5 */
    CLCD_CTRL |= 1;          /* input enable (starts reading YUV) */
    vlog("  After CLCD enable: CLCD_CTRL=0x%08lx", (unsigned long)CLCD_CTRL);

    MIXER_CTRL = 7;           /* mixer commit (ENVID=1, takes over LCD pins) */
    vlog("  After mixer commit: MIXER_CTRL=0x%08lx", (unsigned long)MIXER_CTRL);

    DISP_REG(0x03C) |= 1;    /* latch config */
    DISP_REG(0x03C) |= 2;    /* latch buffers */
    DISP_REG(0x03C) |= 4;    /* latch output */
    vlog("  After latch: DISP+03C=0x%08lx", (unsigned long)DISP_REG(0x03C));

    /* Dump all block states immediately after trigger */
    vlog("  === POST-TRIGGER STATE ===");
    vlog("  CLCD: CTRL=0x%08lx Y=0x%08lx Cb=0x%08lx Cr=0x%08lx",
         (unsigned long)CLCD_CTRL,
         (unsigned long)CLCD_Y_ADDR,
         (unsigned long)CLCD_CB_ADDR,
         (unsigned long)CLCD_CR_ADDR);
    vlog("  CLCD: SRC=%dx%d OUT=%dx%d H=%lu V=%lu",
         (int)CLCD_SRC_W, (int)CLCD_SRC_H,
         (int)CLCD_OUT_W, (int)CLCD_OUT_H,
         (unsigned long)CLCD_H_STEP, (unsigned long)CLCD_V_STEP);
    vlog("  CLCD: YUV_MODE=0x%08lx LSTRIDE=0x%08lx CSTRIDE=0x%08lx",
         (unsigned long)CLCD_YUV_MODE,
         (unsigned long)CLCD_LUMA_STRIDE,
         (unsigned long)CLCD_CHROMA_STRIDE);
    vlog("  MIXER: CTRL=0x%08lx L5EN=0x%08lx PIXFMT=0x%08lx",
         (unsigned long)MIXER_CTRL,
         (unsigned long)MIXER_L5_EN,
         (unsigned long)MIXER_PIXFMT);
    vlog("  DISP: CTRL=0x%08lx MODE=0x%08lx OUT=0x%08lx EN=0x%08lx",
         (unsigned long)DISP_CTRL,
         (unsigned long)DISP_MODE,
         (unsigned long)DISP_OUTPUT,
         (unsigned long)DISP_ENABLE);
    /* Scan first 16 regs of each block for anything unexpected */
    vlog("  --- CLCD regs 0x00-0x3C ---");
    for (int i = 0; i <= 0x3C; i += 4)
        vlog("    +0x%02x = 0x%08lx", i, (unsigned long)CLCD_REG(i));
    vlog("  --- MIXER regs 0x00-0x0C ---");
    for (int i = 0; i <= 0x0C; i += 4)
        vlog("    +0x%02x = 0x%08lx", i, (unsigned long)MIXER_REG(i));
    vlog("  --- DISP regs 0x00-0x18 ---");
    for (int i = 0; i <= 0x18; i += 4)
        vlog("    +0x%02x = 0x%08lx", i, (unsigned long)DISP_REG(i));

    /* === Phase 7: Check pipeline is running === */
    vlog("Phase 7: Pipeline status check");

    /* CLCD_CTRL bit 2 = "actively processing" (NOT stall/error).
     * MIXER_CTRL bit 2 = config mode bit (Apple writes 6 init, 7 run).
     * RE evidence: Apple NEVER checks bit 2. Only polls bit 1 for
     * shutdown drain (ROM 0x166E98). CTRL=0x05 is NORMAL running state.
     * Pipeline runs continuously — never goes idle on its own. */
    rb->sleep(HZ / 10);  /* 100ms for pipeline to start flowing */
    uint32_t clcd_s = CLCD_CTRL;
    uint32_t mixer_s = MIXER_CTRL;
    uint32_t disp_s = DISP_CTRL;
    vlog("  CLCD_CTRL=0x%08lx (bit0=%d bit2=%d) %s",
         (unsigned long)clcd_s, clcd_s & 1, (clcd_s >> 2) & 1,
         (clcd_s == 0x05) ? "RUNNING" :
         (clcd_s == 0x01) ? "enabled-no-activity" : "unexpected");
    vlog("  MIXER_CTRL=0x%08lx %s",
         (unsigned long)mixer_s,
         (mixer_s == 0x05 || mixer_s == 0x07) ? "RUNNING" : "unexpected");
    vlog("  DISP_CTRL=0x%08lx %s",
         (unsigned long)disp_s,
         (disp_s == 0x01) ? "RUNNING(sync)" : "unexpected");

    /* Hold VPP output visible for 5 seconds — look at the LCD! */
    vlog("  Holding display for 5 seconds — check LCD for gradient...");
    rb->sleep(HZ * 5);

    /* === Phase 8: Dump diagnostic registers === */
    vlog("Phase 8: Post-trigger register dump");
    vlog("  CLCD_CTRL  = 0x%08lx", (unsigned long)CLCD_CTRL);
    vlog("  MIXER_CTRL = 0x%08lx", (unsigned long)MIXER_CTRL);
    vlog("  DISP_CTRL  = 0x%08lx", (unsigned long)DISP_CTRL);
    vlog("  DISP_MODE  = 0x%08lx", (unsigned long)DISP_MODE);

    /* === Phase 9: Shutdown VPP pipeline === */
    vlog("Phase 9: Shutting down VPP");

    /* Soft reset display engine */
    DISP_SOFT_RST = 1;
    DISP_GAMMA_COMMIT |= 1;

    /* Stop display engine */
    DISP_REG(0x03C) &= ~0xF;  /* clear latch bits */
    DISP_CTRL &= ~1;
    for (int i = 0; i < 10; i++) {
        if (DISP_CTRL & 2) break;  /* idle */
        rb->sleep(1);
    }
    vlog("  Display engine stopped: 0x%08lx", (unsigned long)DISP_CTRL);

    /* Stop mixer (ENVID=0, MCU controller regains LCD pins) */
    MIXER_CTRL &= ~1;
    for (int i = 0; i < 10; i++) {
        if (MIXER_CTRL & 2) break;
        rb->sleep(1);
    }
    vlog("  Mixer stopped: 0x%08lx", (unsigned long)MIXER_CTRL);

    /* Stop CLCD */
    CLCD_CTRL &= ~1;
    for (int i = 0; i < 10; i++) {
        if (CLCD_CTRL & 2) break;
        rb->sleep(1);
    }
    vlog("  CLCD stopped: 0x%08lx", (unsigned long)CLCD_CTRL);

    /* Disable clocks */
    vpp_clocks_enable(false);
    vpp_svid_enable(false);
    vlog("  Clocks disabled");

    /* === Phase 10: Restore Rockbox LCD === */
    vlog("Phase 10: Restoring LCD");

    /* Re-enable MCU LCD clock gate (disabled in Phase 6) */
    PWRCON(0) &= ~(1 << 1);  /* clear bit 1 = enable LCD clock */
    vlog("  MCU LCD clock re-enabled (PWRCON bit 1 cleared)");

    vlog("=== Test complete ===");
    log_close();

    /* Force full LCD redraw to restore Rockbox UI */
    rb->lcd_update();

    rb->splashf(HZ * 3, "VPP test done! Check /vpp_test.log");

    return PLUGIN_OK;
}

#else /* !IPOD_6G */

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    rb->splash(HZ * 2, "VPP test: iPod 6G only");
    return PLUGIN_ERROR;
}

#endif
