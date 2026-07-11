/***************************************************************************
 * S5L8702 VPP MVP Test v122m — DEFINITIVE FIX: P16 mode for ILI9326 panel
 *
 * ROOT CAUSE CONFIRMED: Apple's ROM has ZERO ILI9326 panels — all are
 * MIPI DCS. P9 mode (0x81100DB9) was designed exclusively for DCS panels.
 * Our ILI9326 panel needs P16 mode (0x80100DB0) — confirmed by Rockbox
 * LCD driver which uses P16 for all PAR18-interface panels.
 * LCD+0x7C changed from 0x402 (2 transfers/P9) to 0x401 (1 transfer/P16).
 *
 * KEY FIXES from v113m investigation:
 * 1. GO bit cycling: COMP_REG(0x000) = 0 then 1 between tests
 *    (v113m never cleared GO, causing stale GRAM data in T4-T8)
 * 2. Test H.264 frame with BOTH AM=0 and AM=1 to resolve agent conflict
 * 3. Fixed GRAM readback: 18-bit direct decode (no >>1 truncation)
 * 4. All compositor registers ROM-verified correct (scaler, planes, stride)
 ****************************************************************************/

#include "plugin.h"

#ifdef IPOD_6G
#include "s5l87xx.h"

#define vpu_h264_buf_size    rb->vpu_h264_buf_size
#define vpu_h264_open        rb->vpu_h264_open
#define vpu_h264_configure   rb->vpu_h264_configure
#define vpu_h264_decode_nalu rb->vpu_h264_decode_nalu
#define vpu_h264_get_frame   rb->vpu_h264_get_frame
#define vpu_h264_close       rb->vpu_h264_close

#define COMP_BASE  0x38900000

#define COMP_REG(off) (*(volatile uint32_t *)(COMP_BASE + (off)))
#define LCD_REG(off)  (*(volatile uint32_t *)(LCD_BASE + (off)))

#define PHYS(x) ((uint32_t)((uintptr_t)(x) & 0x7FFFFFFF))

static int log_fd = -1;
#define RLOG_SIZE 49152
static char rlog_buf[RLOG_SIZE];
static int rlog_pos = 0;
static bool rlog_mode = false;

static void vlog(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int len = rb->vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (rlog_mode) {
        if (rlog_pos + len + 1 < RLOG_SIZE) {
            rb->memcpy(rlog_buf + rlog_pos, buf, len);
            rlog_buf[rlog_pos + len] = '\n';
            rlog_pos += len + 1;
        }
    } else if (log_fd >= 0) {
        rb->write(log_fd, buf, len);
        rb->write(log_fd, "\n", 1);
    }
}

static void rlog_flush(void)
{
    if (log_fd >= 0 && rlog_pos > 0) {
        rb->write(log_fd, rlog_buf, rlog_pos);
        rlog_pos = 0;
    }
}

static void lcd_wait(void)
{
    while (!(LCD_STATUS & 0x2));
    for (volatile int i = 0; i < 100; i++);
}

static void lcd_set_con(uint32_t config)
{
    lcd_wait();
    LCD_CON = config;
}

static void lcd_cmd(uint16_t cmd)
{
    while (LCD_STATUS & 0x10);
    LCD_WCMD = cmd;
}

static void lcd_data(uint16_t data)
{
    while (LCD_STATUS & 0x10);
    LCD_WDATA = data;
}

static int find_start_code(const uint8_t *buf, int len, int *sc_len)
{
    for (int i = 0; i < len - 3; i++) {
        if (buf[i] == 0 && buf[i+1] == 0) {
            if (buf[i+2] == 1) { *sc_len = 3; return i; }
            if (i + 3 < len && buf[i+2] == 0 && buf[i+3] == 1)
            { *sc_len = 4; return i; }
        }
    }
    return -1;
}

static void push_one_frame(void)
{
    { int t = 100000; while ((LCD_REG(0x8C) & 3) && --t > 0); }

    LCD_REG(0x80) = 1;  /* CPU takes bus */

    LCD_CON = 0x80000DA9;  /* P18 for ILI9326 commands */
    lcd_cmd(0x003); lcd_data(0x1030);  /* AM=0, I/D=11, BGR=1, HWM=1 */
    lcd_cmd(0x210); lcd_data(0);
    lcd_cmd(0x211); lcd_data(319);    /* HE=319: full landscape */
    lcd_cmd(0x212); lcd_data(0);
    lcd_cmd(0x213); lcd_data(239);    /* VE=239: full landscape */
    lcd_cmd(0x200); lcd_data(0);
    lcd_cmd(0x201); lcd_data(0);
    lcd_cmd(0x202);
    while (!(LCD_STATUS & 0x2));
    LCD_CON = 0x80100DB0;  /* P16 mode — correct for ILI9326 */

    LCD_REG(0x80) = 0;

    { int t = 500000; while ((LCD_REG(0x8C) & 3) && --t > 0); }
}

/* Apple's exact push: P8 DCS (0x81000C21, bit 24 SET) inside LCD+0x80 bracket.
 * Previous attempts used 0x80000C20 (wrong bus pins). */
static void push_one_frame_dcs(void)
{
    { int t = 100000; while ((LCD_REG(0x8C) & 3) && --t > 0); }

    LCD_REG(0x80) = 1;

    /* Switch to P8 with bit 24 SET — routes to D[17:10] where panel listens */
    lcd_wait();
    LCD_CON = 0x81000C21;

    /* DCS 0x36 MADCTL = 0x00 (normal scan, no exchange) */
    lcd_cmd(0x36); lcd_data(0x00);
    /* DCS 0x2A CASET = 0-239 (240 columns) */
    lcd_cmd(0x2A);
    lcd_data(0x00); lcd_data(0x00); lcd_data(0x00); lcd_data(0xEF);
    /* DCS 0x2B PASET = 0-319 (320 pages) */
    lcd_cmd(0x2B);
    lcd_data(0x00); lcd_data(0x00); lcd_data(0x01); lcd_data(0x3F);
    /* DCS 0x2C RAMWR */
    lcd_cmd(0x2C);

    /* Restore P9 for compositor pixel push */
    lcd_wait();
    LCD_CON = 0x80100DB0;

    LCD_REG(0x80) = 0;

    { int t = 500000; while ((LCD_REG(0x8C) & 3) && --t > 0); }
}

static void push_one_frame_am1(void)
{
    { int t = 100000; while ((LCD_REG(0x8C) & 3) && --t > 0); }

    LCD_REG(0x80) = 1;  /* CPU takes bus */

    LCD_CON = 0x80000DA9;  /* P18 for ILI9326 commands */
    lcd_cmd(0x003); lcd_data(0x1238);  /* AM=1: vertical auto-increment, HWM=1, BGR=1 */
    lcd_cmd(0x210); lcd_data(0);
    lcd_cmd(0x211); lcd_data(319);    /* HE=319: full horizontal */
    lcd_cmd(0x212); lcd_data(0);
    lcd_cmd(0x213); lcd_data(239);    /* VE=239: full vertical */
    lcd_cmd(0x200); lcd_data(0);
    lcd_cmd(0x201); lcd_data(0);
    lcd_cmd(0x202);
    while (!(LCD_STATUS & 0x2));
    LCD_CON = 0x80100DB0;  /* P16 for ILI9326 */

    LCD_REG(0x80) = 0;

    { int t = 500000; while ((LCD_REG(0x8C) & 3) && --t > 0); }
}

static void compositor_retrigger(void)
{
    COMP_REG(0x000) = 0;
    { volatile int d = 0; while (d++ < 50000); }
    COMP_REG(0x000) = 1;
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 200000); }
}

static void push_one_frame_p16(void)
{
    { int t = 100000; while ((LCD_REG(0x8C) & 3) && --t > 0); }

    LCD_REG(0x80) = 1;

    LCD_CON = 0x80000DA9;
    lcd_cmd(0x003); lcd_data(0x1238);
    lcd_cmd(0x210); lcd_data(0);
    lcd_cmd(0x211); lcd_data(319);
    lcd_cmd(0x212); lcd_data(0);
    lcd_cmd(0x213); lcd_data(239);
    lcd_cmd(0x200); lcd_data(0);
    lcd_cmd(0x201); lcd_data(0);
    lcd_cmd(0x202);
    while (!(LCD_STATUS & 0x2));
    LCD_CON = 0x80100DB0;  /* P16 mode — Rockbox's native pixel format */

    LCD_REG(0x80) = 0;

    { int t = 500000; while ((LCD_REG(0x8C) & 3) && --t > 0); }
}

/* KEY FIX: Set COLMOD=0x05 (16-bit RGB565) so panel expects 2 P9 transfers.
 * iBoot sets COLMOD=0x06 (18-bit RGB666) which needs 3 P9 transfers per pixel.
 * With 2 transfers + 18-bit mode, the B channel transfer is NEVER SENT → B=0!
 * COLMOD is a DCS command (0x3A) — must be sent via P8 mode, not P18. */
static void push_one_frame_colmod(void)
{
    { int t = 100000; while ((LCD_REG(0x8C) & 3) && --t > 0); }

    LCD_REG(0x80) = 1;

    /* Send DCS COLMOD via P8 (Apple's command mode with bit 24) */
    lcd_wait();
    LCD_CON = 0x81000C21;  /* P8 with bit 24 — routes to D[17:10] */
    lcd_cmd(0x3A); lcd_data(0x05);  /* COLMOD = 16-bit RGB565 */
    lcd_wait();

    /* Now send ILI9326 GRAM setup via P18 */
    LCD_CON = 0x80000DA9;
    lcd_cmd(0x003); lcd_data(0x1238);
    lcd_cmd(0x210); lcd_data(0);
    lcd_cmd(0x211); lcd_data(319);
    lcd_cmd(0x212); lcd_data(0);
    lcd_cmd(0x213); lcd_data(239);
    lcd_cmd(0x200); lcd_data(0);
    lcd_cmd(0x201); lcd_data(0);
    lcd_cmd(0x202);
    while (!(LCD_STATUS & 0x2));
    LCD_CON = 0x80100DB0;  /* P9 mode */

    LCD_REG(0x80) = 0;

    { int t = 500000; while ((LCD_REG(0x8C) & 3) && --t > 0); }
}

static void gram_scan(const char *label)
{
    LCD_REG(0x70) = 0;
    LCD_REG(0x80) = 1;
    while (!(LCD_STATUS & 0x2));
    LCD_CON = 0x80000DA8;

    static const struct { int x, y; } pts[] = {
        {160, 0}, {160, 60}, {160, 120}, {160, 180}, {160, 239}
    };
    vlog("  GRAM[%s]:", label);
    for (int i = 0; i < 5; i++) {
        lcd_cmd(0x200); lcd_data(pts[i].x);
        lcd_cmd(0x201); lcd_data(pts[i].y);
        lcd_cmd(0x202);
        while (!(LCD_STATUS & 0x2));
        LCD_RDATA = 0; { int t = 100000; while (!(LCD_STATUS & 1) && --t > 0); } (void)LCD_DBUFF;
        LCD_RDATA = 0; { int t = 100000; while (!(LCD_STATUS & 1) && --t > 0); }
        uint32_t g = LCD_DBUFF & 0x3FFFF;
        vlog("    y=%3d: R6=%2lu G6=%2lu B6=%2lu (raw=%05lx)",
             pts[i].y,
             (unsigned long)((g >> 12) & 0x3F),
             (unsigned long)((g >> 6) & 0x3F),
             (unsigned long)(g & 0x3F),
             (unsigned long)g);
    }

    while (!(LCD_STATUS & 0x2));
    LCD_CON = 0x80100DB0;
    LCD_REG(0x80) = 0;
    LCD_REG(0x70) = 1;
}

/* Enhanced GRAM scan: sample a 2D grid of positions.
 * 5 x-positions x 5 y-positions = 25 samples covering the full LCD.
 * This reveals the compositor-to-GRAM coordinate mapping. */
static void gram_scan_2d(const char *label)
{
    static const int xs[] = {0, 80, 160, 240, 319};
    static const int ys[] = {0, 60, 120, 180, 239};

    LCD_REG(0x70) = 0;
    LCD_REG(0x80) = 1;
    while (!(LCD_STATUS & 0x2));
    LCD_CON = 0x80000DA8;

    vlog("  GRAM2D[%s]:", label);
    for (int yi = 0; yi < 5; yi++) {
        for (int xi = 0; xi < 5; xi++) {
            lcd_cmd(0x200); lcd_data(xs[xi]);
            lcd_cmd(0x201); lcd_data(ys[yi]);
            lcd_cmd(0x202);
            while (!(LCD_STATUS & 0x2));
            LCD_RDATA = 0;
            { int t = 100000; while (!(LCD_STATUS & 1) && --t > 0); }
            (void)LCD_DBUFF;
            LCD_RDATA = 0;
            { int t = 100000; while (!(LCD_STATUS & 1) && --t > 0); }
            uint32_t g = LCD_DBUFF & 0x3FFFF;
            vlog("    x=%3d y=%3d: R6=%2lu G6=%2lu B6=%2lu (raw=%05lx)",
                 xs[xi], ys[yi],
                 (unsigned long)((g >> 12) & 0x3F),
                 (unsigned long)((g >> 6) & 0x3F),
                 (unsigned long)(g & 0x3F),
                 (unsigned long)g);
        }
    }

    while (!(LCD_STATUS & 0x2));
    LCD_CON = 0x80100DB0;
    LCD_REG(0x80) = 0;
    LCD_REG(0x70) = 1;
}

/* Fill YUV planes via uncached writes */
static void fill_yuv(const uint8_t *y_out, const uint8_t *cb_out,
                     const uint8_t *cr_out, int w, int h,
                     uint8_t yval, uint8_t cbval, uint8_t crval)
{
    uint8_t *y_unc = (uint8_t *)((uintptr_t)y_out | 0x40000000);
    uint8_t *cb_unc = (uint8_t *)((uintptr_t)cb_out | 0x40000000);
    uint8_t *cr_unc = (uint8_t *)((uintptr_t)cr_out | 0x40000000);
    for (int i = 0; i < w * h; i++) y_unc[i] = yval;
    for (int i = 0; i < (w/2) * (h/2); i++) {
        cb_unc[i] = cbval;
        cr_unc[i] = crval;
    }
    rb->commit_discard_dcache();
}

/* Fill Y-plane with vertical stripes (left/right split at column boundary).
 * Cb/Cr set to neutral 128 everywhere. */
static void fill_yuv_vstripe(const uint8_t *y_out, const uint8_t *cb_out,
                             const uint8_t *cr_out, int w, int h,
                             int split_col, uint8_t y_left, uint8_t y_right)
{
    uint8_t *y_unc = (uint8_t *)((uintptr_t)y_out | 0x40000000);
    uint8_t *cb_unc = (uint8_t *)((uintptr_t)cb_out | 0x40000000);
    uint8_t *cr_unc = (uint8_t *)((uintptr_t)cr_out | 0x40000000);
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            y_unc[r * w + c] = (c < split_col) ? y_left : y_right;
    for (int i = 0; i < (w/2) * (h/2); i++) {
        cb_unc[i] = 128;
        cr_unc[i] = 128;
    }
    rb->commit_discard_dcache();
}

/* Fill Y-plane with horizontal stripes (top/bottom split at row boundary).
 * Cb/Cr set to neutral 128 everywhere. */
static void fill_yuv_hstripe(const uint8_t *y_out, const uint8_t *cb_out,
                             const uint8_t *cr_out, int w, int h,
                             int split_row, uint8_t y_top, uint8_t y_bot)
{
    uint8_t *y_unc = (uint8_t *)((uintptr_t)y_out | 0x40000000);
    uint8_t *cb_unc = (uint8_t *)((uintptr_t)cb_out | 0x40000000);
    uint8_t *cr_unc = (uint8_t *)((uintptr_t)cr_out | 0x40000000);
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            y_unc[r * w + c] = (r < split_row) ? y_top : y_bot;
    for (int i = 0; i < (w/2) * (h/2); i++) {
        cb_unc[i] = 128;
        cr_unc[i] = 128;
    }
    rb->commit_discard_dcache();
}

/* Fill Y-plane with a single bright pixel at (px,py), rest black.
 * Cb/Cr neutral 128. */
static void fill_yuv_single_pixel(const uint8_t *y_out, const uint8_t *cb_out,
                                  const uint8_t *cr_out, int w, int h,
                                  int px, int py, uint8_t y_bright)
{
    uint8_t *y_unc = (uint8_t *)((uintptr_t)y_out | 0x40000000);
    uint8_t *cb_unc = (uint8_t *)((uintptr_t)cb_out | 0x40000000);
    uint8_t *cr_unc = (uint8_t *)((uintptr_t)cr_out | 0x40000000);
    for (int i = 0; i < w * h; i++) y_unc[i] = 16;
    y_unc[py * w + px] = y_bright;
    /* Also light up a small 3x3 block around the pixel for visibility */
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            int r = py + dy, c = px + dx;
            if (r >= 0 && r < h && c >= 0 && c < w)
                y_unc[r * w + c] = y_bright;
        }
    for (int i = 0; i < (w/2) * (h/2); i++) {
        cb_unc[i] = 128;
        cr_unc[i] = 128;
    }
    rb->commit_discard_dcache();
}

/* Fill Y-plane with horizontal gradient: Y[row][col] = col & 0xFF.
 * This creates vertical bands that cycle through 0-255 across columns.
 * Cb/Cr neutral 128. */
static void fill_yuv_hgradient(const uint8_t *y_out, const uint8_t *cb_out,
                               const uint8_t *cr_out, int w, int h)
{
    uint8_t *y_unc = (uint8_t *)((uintptr_t)y_out | 0x40000000);
    uint8_t *cb_unc = (uint8_t *)((uintptr_t)cb_out | 0x40000000);
    uint8_t *cr_unc = (uint8_t *)((uintptr_t)cr_out | 0x40000000);
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            y_unc[r * w + c] = (uint8_t)(c & 0xFF);
    for (int i = 0; i < (w/2) * (h/2); i++) {
        cb_unc[i] = 128;
        cr_unc[i] = 128;
    }
    rb->commit_discard_dcache();
}

/* Fill Y-plane with VERTICAL gradient: Y[row][col] = row & 0xFF.
 * This creates horizontal bands. Since rotation maps source rows to GRAM x,
 * the gradient should appear along GRAM x (the axis T7 couldn't test). */
static void fill_yuv_vgradient(const uint8_t *y_out, const uint8_t *cb_out,
                               const uint8_t *cr_out, int w, int h)
{
    uint8_t *y_unc = (uint8_t *)((uintptr_t)y_out | 0x40000000);
    uint8_t *cb_unc = (uint8_t *)((uintptr_t)cb_out | 0x40000000);
    uint8_t *cr_unc = (uint8_t *)((uintptr_t)cr_out | 0x40000000);
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            y_unc[r * w + c] = (uint8_t)(r & 0xFF);
    for (int i = 0; i < (w/2) * (h/2); i++) {
        cb_unc[i] = 128;
        cr_unc[i] = 128;
    }
    rb->commit_discard_dcache();
}

static void compositor_init(void)
{
    volatile uint32_t *c = (volatile uint32_t *)COMP_BASE;

    c[0x200/4] &= ~1;
    c[0x004/4] = 1;
    c[0x020/4] = 1;

    for (int i = 0; i < 256; i++) {
        c[0x400/4 + i] = i * 4;  /* 10-bit identity LUT — Apple ROM-verified (0x14D288) */
        c[0x800/4 + i] = i * 4;
        c[0xC00/4 + i] = i * 4;
    }

    /* Scaler coefficients: POR = 0x80 = 128 = unity gain (128/128 = 1.0).
     * Agent confirmed: signed 12-bit, 2 per register, divisor=128.
     * POR is CORRECT — do NOT overwrite. Apple Lanczos tables at
     * ROM 0xA287DC (4-tap) and 0xA28AAC (2-tap) for proper filtering. */

    c[0x0D8/4] = 0x00001000; c[0x0DC/4] = 0;
    c[0x0E0/4] = 0x00001000; c[0x0E4/4] = 0;
    c[0x0E8/4] = 0x00001000; c[0x0EC/4] = 0;

    {
        volatile uint32_t *src = (volatile uint32_t *)0x0890D2DC;
        uint32_t t[5];
        for (int i = 0; i < 5; i++) t[i] = src[i];
        if (t[0] > 0 && t[0] < 0x1000 && t[4] > 0 && t[4] < 0x1000) {
            for (int i = 0; i < 5; i++) c[(0x1EC + i*4)/4] = t[i];
            vlog("  Using LIVE iBoot timing");
        } else {
            uint32_t hc[] = {0x0C, 0x26, 0x10, 0x82, 0x4E};
            for (int i = 0; i < 5; i++) c[(0x1EC + i*4)/4] = hc[i];
            vlog("  Using HARDCODED timing");
        }
    }

    {
        uint32_t v = c[0x008/4];
        v &= ~0x20000000;
        v &= ~0x10000000;
        v &= ~0x03000000; v |= 0x01000000;
        v &= ~0x00300000; v |= 0x00100000;
        v &= ~0x00030000; v |= 0x00010000;
        v &= ~1; v |= 1;
        c[0x008/4] = v;
    }
    c[0x00C/4] = 0x000F0F0F;  /* BG_COLOR (Apple default) */
    { uint32_t v = c[0x008/4]; v |= 0x8000; c[0x008/4] = v; }    /* bit 15 */
    { uint32_t v = c[0x008/4]; v &= ~2; c[0x008/4] = v; }       /* clear bit 1 */
    { uint32_t v = c[0x008/4]; v |= 0x100; c[0x008/4] = v; }    /* bit 8 */
    { uint32_t v = c[0x008/4]; v |= 0x80; c[0x008/4] = v; }     /* bit 7 — REQUIRED for Layer 5 output */
    { uint32_t v = c[0x008/4]; v |= 0x40000000; c[0x008/4] = v; } /* bit 30 */

    c[0x200/4] |= 0x10080;
    c[0x204/4] = 2;
    c[0x208/4] = 0;
    c[0x20C/4] = 2;
    c[0x210/4] = 0x00010110;
    c[0x214/4] = 0x013F00EF;  /* LANDSCAPE: (319<<16)|239 = 320/scan, 240 scans */
    c[0x024/4] = 0x00FFFFFF;
}

enum plugin_status plugin_start(const void *parameter)
{
    const char *test_path = parameter ? (const char *)parameter
                                      : "/test_iframe.264";
    if (!*test_path) return PLUGIN_ERROR;

    rb->cpu_boost(true);
    rb->audio_stop();

    log_fd = rb->open("/vpu_vpp_test.log", O_WRONLY|O_CREAT|O_TRUNC, 0666);
    vlog("=== VPP MVP Test v141m ===");
    vlog("File: %s", test_path);
    vlog("Panel type: %d", (PDAT(6) & 0x30) >> 4);

    /* ---- Phase 1: Decode ---- */
    uint8_t *audio_buf;
    size_t audio_sz;
    audio_buf = rb->plugin_get_audio_buffer(&audio_sz);
    size_t dec_size = vpu_h264_buf_size(640, 480);
    if (audio_sz < dec_size + 320*240*2) {
        vlog("ERROR: buffer too small");
        rb->close(log_fd);
        return PLUGIN_ERROR;
    }

    struct vpu_h264 *dec = vpu_h264_open(audio_buf, dec_size, 640, 480);
    if (!dec) { vlog("ERROR: vpu_h264_open failed"); rb->close(log_fd); return PLUGIN_ERROR; }
    uint8_t *file_buf = audio_buf + dec_size;

    int fd = rb->open(test_path, O_RDONLY);
    if (fd < 0) { vlog("ERROR: can't open file"); rb->close(log_fd); return PLUGIN_ERROR; }
    int file_len = rb->read(fd, file_buf, 320*240*2);
    rb->close(fd);
    vlog("  Loaded %d bytes", file_len);

    int frame_w = 0, frame_h = 0;
    const uint8_t *y_out = NULL, *cb_out = NULL, *cr_out = NULL;
    bool got_frame = false;
    int pos = 0;
    while (pos < file_len - 4) {
        int sc_len, sc_pos = find_start_code(file_buf + pos, file_len - pos, &sc_len);
        if (sc_pos < 0) break;
        int nalu_start = pos + sc_pos + sc_len;
        int next_sc = find_start_code(file_buf + nalu_start, file_len - nalu_start, &sc_len);
        int nalu_len = (next_sc >= 0) ? next_sc : file_len - nalu_start;
        int ret = vpu_h264_decode_nalu(dec, file_buf + nalu_start, nalu_len);
        if (ret == 1) {
            vpu_h264_get_frame(dec, &y_out, &cb_out, &cr_out, &frame_w, &frame_h);
            vlog("  DECODED: %dx%d Y=%08lx Cb=%08lx Cr=%08lx",
                 frame_w, frame_h,
                 (unsigned long)(uintptr_t)y_out,
                 (unsigned long)(uintptr_t)cb_out,
                 (unsigned long)(uintptr_t)cr_out);
            got_frame = true;
        }
        pos = nalu_start + nalu_len;
    }
    if (!got_frame || !y_out || !cb_out || !cr_out) {
        vlog("ERROR: no frame decoded or null buffers (y=%p cb=%p cr=%p)",
             y_out, cb_out, cr_out);
        vpu_h264_close(dec);
        rb->close(log_fd);
        return PLUGIN_ERROR;
    }

    /* Switch to RAM logging */
    rb->close(log_fd);
    log_fd = rb->open("/vpu_vpp_test.log", O_WRONLY|O_APPEND, 0666);
    rb->close(log_fd);
    log_fd = -1;
    rlog_mode = true;

    /* ---- Phase 2: Init compositor ---- */
    vlog("Phase 2: Init");
    PWRCON(0) &= ~0x2080;
    for (volatile int d = 0; d < 10000; d++);

    /* Dump ALL Layer 5 and surrounding regs BEFORE init */
    vlog("  Pre-init regs:");
    for (int off = 0x028; off <= 0x058; off += 4)
        vlog("    +%03x=%08lx", off, (unsigned long)COMP_REG(off));

    /* Dump CSC matrix registers (comp+0x0F0-0x17C) BEFORE init */
    vlog("  Pre-init CSC matrix (0F0-17C):");
    for (int off = 0x0F0; off <= 0x17C; off += 4)
        vlog("    +%03x=%08lx", off, (unsigned long)COMP_REG(off));
    /* Dump second matrix bank (comp+0x31C-0x360) */
    vlog("  Pre-init matrix2 (31C-360):");
    for (int off = 0x31C; off <= 0x360; off += 4)
        vlog("    +%03x=%08lx", off, (unsigned long)COMP_REG(off));

    compositor_init();
    vlog("  comp+0x008=%08lx 0D4=%08lx 3AC=%08lx",
         (unsigned long)COMP_REG(0x008),
         (unsigned long)COMP_REG(0x0D4),
         (unsigned long)COMP_REG(0x3AC));
    /* Verify scaler coefficients took (0x7F not POR 0x80) */
    vlog("  Scaler: 0F4=%08lx 180=%08lx (expect 0x7F)",
         (unsigned long)COMP_REG(0x0F4), (unsigned long)COMP_REG(0x180));

    /* Verify LUT readback — are our identity writes intact? */
    vlog("  LUT readback: ch0[0]=%08lx [64]=%08lx [128]=%08lx [255]=%08lx",
         (unsigned long)COMP_REG(0x400), (unsigned long)COMP_REG(0x500),
         (unsigned long)COMP_REG(0x600), (unsigned long)COMP_REG(0x7FC));
    vlog("  LUT readback: ch1[0]=%08lx [128]=%08lx ch2[0]=%08lx [128]=%08lx",
         (unsigned long)COMP_REG(0x800), (unsigned long)COMP_REG(0xA00),
         (unsigned long)COMP_REG(0xC00), (unsigned long)COMP_REG(0xE00));
    /* Gain/offset readback */
    vlog("  Gain: D8=%08lx DC=%08lx E0=%08lx E4=%08lx E8=%08lx EC=%08lx",
         (unsigned long)COMP_REG(0x0D8), (unsigned long)COMP_REG(0x0DC),
         (unsigned long)COMP_REG(0x0E0), (unsigned long)COMP_REG(0x0E4),
         (unsigned long)COMP_REG(0x0E8), (unsigned long)COMP_REG(0x0EC));

    /* Zero Layer 5 registers EXCEPT comp+0x048 (hardware-auto-computed).
     * comp+0x048 is the chroma dimension register, never written by Apple's ROM.
     * The vtable[9] agent confirmed it's derived by hardware from format/dimensions. */
    for (int off = 0x024; off <= 0x044; off += 4)
        COMP_REG(off) = 0;
    /* Skip 0x048 — hardware auto-computed */
    for (int off = 0x04C; off <= 0x058; off += 4)
        COMP_REG(off) = 0;

    /* Layer 5 config */
    COMP_REG(0x028) = 0x100;
    COMP_REG(0x02C) = frame_w | ((frame_w / 2) << 16);
    COMP_REG(0x030) = 0;
    COMP_REG(0x034) = frame_h | ((uint32_t)frame_w << 16);
    COMP_REG(0x04C) = 0x10001000;  /* Scaler step 1.0/1.0 — CSC-enabled path uses hardcoded 320,240 */
    COMP_REG(0x050) = 0;
    COMP_REG(0x054) = ((uint32_t)240 << 16) | 320;

    COMP_REG(0x038) = PHYS(y_out);
    COMP_REG(0x03C) = PHYS(cr_out);
    COMP_REG(0x040) = 0;
    COMP_REG(0x044) = PHYS(cb_out);
    COMP_REG(0x3AC) = 0;  /* rotation OFF — landscape source → landscape display */
    COMP_REG(0x0D4) = 1;           /* ROM-verified: vtable[0x6c](obj,1,0) → 1|(0<<8)=1 */

    /* CRITICAL: Clear bit 8 to activate BT.601 CSC for video mode.
     * Apple init SETS bit 8 (graphics bypass). Video-enable at ROM 0x14DEC4
     * CLEARS it via FUN_0009B6D4(1). Without clearing, CSC is BYPASSED. */
    { uint32_t v = COMP_REG(0x008); v &= ~0x100; COMP_REG(0x008) = v; }
    vlog("  comp+0x008 after CSC enable: %08lx", (unsigned long)COMP_REG(0x008));

    /* Verify buffer addresses and DRAM content */
    vlog("  Ptrs: Y=%08lx Cb=%08lx Cr=%08lx (PHYS: Y=%08lx Cb=%08lx Cr=%08lx)",
         (unsigned long)(uintptr_t)y_out,
         (unsigned long)(uintptr_t)cb_out,
         (unsigned long)(uintptr_t)cr_out,
         (unsigned long)PHYS(y_out),
         (unsigned long)PHYS(cb_out),
         (unsigned long)PHYS(cr_out));
    /* Address range validation: all pointers must be in DRAM (0x08000000-0x0BFFFFFF) */
    {
        uint32_t py = PHYS(y_out), pcb = PHYS(cb_out), pcr = PHYS(cr_out);
        uint32_t y_end = py + frame_w * frame_h;
        uint32_t cb_end = pcb + (frame_w/2) * (frame_h/2);
        uint32_t cr_end = pcr + (frame_w/2) * (frame_h/2);
        vlog("  Addr ranges: Y=%08lx-%08lx Cb=%08lx-%08lx Cr=%08lx-%08lx",
             (unsigned long)py, (unsigned long)y_end,
             (unsigned long)pcb, (unsigned long)cb_end,
             (unsigned long)pcr, (unsigned long)cr_end);
        if (py < 0x08000000 || y_end > 0x0C000000 ||
            pcb < 0x08000000 || cb_end > 0x0C000000 ||
            pcr < 0x08000000 || cr_end > 0x0C000000) {
            vlog("  ERROR: buffer address outside DRAM range!");
        }
        if (pcb == pcr || py == pcb || py == pcr) {
            vlog("  ERROR: overlapping buffer addresses!");
        }
    }
    /* DMA reg readback — v113m showed these ARE readable (not write-only) */
    vlog("  DMA readback: 038=%08lx 03C=%08lx 044=%08lx",
         (unsigned long)COMP_REG(0x038),
         (unsigned long)COMP_REG(0x03C),
         (unsigned long)COMP_REG(0x044));

    /* Dump ALL Layer 5 regs AFTER config */
    vlog("  Post-config regs:");
    for (int off = 0x028; off <= 0x058; off += 4)
        vlog("    +%03x=%08lx", off, (unsigned long)COMP_REG(off));

    /* LCD passthrough setup */
    uint32_t saved_lcd_con = LCD_CON;
    uint32_t saved_7c = LCD_REG(0x7C);
    uint32_t saved_88 = LCD_REG(0x88);
    uint32_t saved_20 = LCD_REG(0x20);
    uint32_t saved_74 = LCD_REG(0x74);
    uint32_t saved_78 = LCD_REG(0x78);

    /* LCD passthrough setup — ADAPTED FOR ILI9326 PANEL (type 2/3)
     *
     * ROOT CAUSE FOUND: Apple's ROM only has MIPI DCS panels (no ILI9326).
     * Apple uses P9 mode (0x81100DB9) for DCS panels. But our ILI9326 panel
     * needs P16 mode (0x80100DB0) — confirmed by Rockbox LCD driver which
     * uses P16 for all PAR18-interface panels. P9 causes B=0 because the
     * ILI9326 in 18-bit COLMOD mode expects 3 P9 transfers per pixel but
     * only receives 2, losing the B channel entirely.
     *
     * FIX: Use P16 (0x80100DB0) as the passthrough LCD_CON.
     * Also try LCD+0x7C=0x401 (1 transfer for P16 vs 0x402 for 2-transfer P9).
     */
    LCD_CON = 0x80100DB0;  /* P16 mode — correct for ILI9326 panel */
    LCD_REG(0x88) = 0x01000000;
    LCD_REG(0x20) = 0x33;
    LCD_REG(0x7C) = 0x00000401;  /* 1 transfer per pixel for P16 mode */

    /* Passthrough registers — LANDSCAPE for ILI9326 */
    LCD_REG(0x78) = 0x000A000A;
    LCD_REG(0x74) = 0x014000F0;  /* per_scan=320, num_scans=240 */

    /* Prime panel for DCS mode: send ILI9326 "interface control" via P18
     * to enable the DCS command decoder alongside ILI9326 registers.
     * Then send initial DCS GRAM setup via P8 to verify DCS works. */
    lcd_set_con(0x80000DA9);
    lcd_cmd(0x003); lcd_data(0x1030);  /* AM=0, landscape GRAM */
    lcd_cmd(0x210); lcd_data(0);
    lcd_cmd(0x211); lcd_data(319);    /* HE=319 */
    lcd_cmd(0x212); lcd_data(0);
    lcd_cmd(0x213); lcd_data(239);    /* VE=239 */
    lcd_cmd(0x200); lcd_data(0);
    lcd_cmd(0x201); lcd_data(0);
    lcd_cmd(0x202);
    lcd_set_con(0x81100DB9);

    /* ---- Rockbox pixel calibration: paint known color via Rockbox, read GRAM ---- */
    vlog("CALIB: Rockbox white pixel at (160,120)");
    {
        /* Use Rockbox's framebuffer to paint white at center */
        rb->lcd_set_foreground(LCD_WHITE);
        rb->lcd_fillrect(158, 118, 5, 5);
        rb->lcd_set_foreground(LCD_BLACK);
        rb->lcd_fillrect(153, 118, 5, 5);
        rb->lcd_update();
        /* Read GRAM at both spots */
        while (!(LCD_STATUS & 0x2));
        LCD_CON = 0x80000DA8;
        lcd_cmd(0x200); lcd_data(160); lcd_cmd(0x201); lcd_data(120);
        lcd_cmd(0x202);
        while (!(LCD_STATUS & 0x2));
        LCD_RDATA = 0; { int t=100000; while(!(LCD_STATUS&1)&&--t>0); } (void)LCD_DBUFF;
        LCD_RDATA = 0; { int t=100000; while(!(LCD_STATUS&1)&&--t>0); }
        uint32_t gw = LCD_DBUFF & 0x3FFFF;
        vlog("  White(160,120): R6=%lu G6=%lu B6=%lu raw=%05lx",
             (unsigned long)((gw>>12)&0x3F), (unsigned long)((gw>>6)&0x3F),
             (unsigned long)(gw&0x3F), (unsigned long)gw);

        lcd_cmd(0x200); lcd_data(155); lcd_cmd(0x201); lcd_data(120);
        lcd_cmd(0x202);
        while (!(LCD_STATUS & 0x2));
        LCD_RDATA = 0; { int t=100000; while(!(LCD_STATUS&1)&&--t>0); } (void)LCD_DBUFF;
        LCD_RDATA = 0; { int t=100000; while(!(LCD_STATUS&1)&&--t>0); }
        uint32_t gb = LCD_DBUFF & 0x3FFFF;
        vlog("  Black(155,120): R6=%lu G6=%lu B6=%lu raw=%05lx",
             (unsigned long)((gb>>12)&0x3F), (unsigned long)((gb>>6)&0x3F),
             (unsigned long)(gb&0x3F), (unsigned long)gb);
        while (!(LCD_STATUS & 0x2));
        LCD_CON = 0x80100DB0;
    }

    /* ---- TEST SW: Software YCbCr→RGB display via Rockbox lcd_update ---- */
    /* Bypasses compositor entirely. Shows what the decoded frame SHOULD look like. */
    vlog("TEST_SW: Software YCbCr→RGB via Rockbox lcd_update");
    {
        /* Disable passthrough for CPU painting */
        LCD_REG(0x70) = 0;
        LCD_REG(0x80) = 0;

        /* Restore Rockbox LCD mode for CPU painting */
        lcd_set_con(saved_lcd_con);
        LCD_PHTIME = 0x33;

        for (int sy = 0; sy < frame_h && sy < LCD_HEIGHT; sy++) {
            for (int sx = 0; sx < frame_w && sx < LCD_WIDTH; sx++) {
                const uint8_t *y_unc = (const uint8_t *)((uintptr_t)y_out | 0x40000000);
                const uint8_t *cb_unc = (const uint8_t *)((uintptr_t)cb_out | 0x40000000);
                const uint8_t *cr_unc = (const uint8_t *)((uintptr_t)cr_out | 0x40000000);
                int Y = y_unc[sy * frame_w + sx];
                int Cb = cb_unc[(sy/2) * (frame_w/2) + (sx/2)];
                int Cr = cr_unc[(sy/2) * (frame_w/2) + (sx/2)];
                int C = Y - 16;
                int D = Cb - 128;
                int E = Cr - 128;
                int R = (298*C + 409*E + 128) >> 8;
                int G = (298*C - 100*D - 208*E + 128) >> 8;
                int B = (298*C + 516*D + 128) >> 8;
                if (R < 0) R = 0; if (R > 255) R = 255;
                if (G < 0) G = 0; if (G > 255) G = 255;
                if (B < 0) B = 0; if (B > 255) B = 255;
                unsigned short rgb565 = ((R >> 3) << 11) | ((G >> 2) << 5) | (B >> 3);
                rb->lcd_set_foreground(rgb565);
                rb->lcd_drawpixel(sx, sy);
            }
        }
        rb->lcd_update();
        vlog("  SW display done (%dx%d)", frame_w, frame_h);
        { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 5000000) rb->backlight_on(); }

        /* Re-setup LCD for compositor passthrough */
        LCD_CON = 0x80100DB0;
        LCD_REG(0x88) = 0x01000000;
        LCD_REG(0x20) = 0x33;
        LCD_REG(0x7C) = 0x00000402;
        LCD_REG(0x78) = 0x000A000A;
        LCD_REG(0x74) = 0x014000F0;
        lcd_set_con(0x80000DA9);
        lcd_cmd(0x003); lcd_data(0x1238);
        lcd_cmd(0x210); lcd_data(0);
        lcd_cmd(0x211); lcd_data(319);
        lcd_cmd(0x212); lcd_data(0);
        lcd_cmd(0x213); lcd_data(239);
        lcd_cmd(0x200); lcd_data(0);
        lcd_cmd(0x201); lcd_data(0);
        lcd_cmd(0x202);
        lcd_set_con(0x81100DB9);
    }

    /* Enable passthrough + release bus */
    LCD_REG(0x70) = 1;
    LCD_REG(0x80) = 0;

    /* ---- TEST 00: KITCHEN SINK — swap ALL dimension registers ---- */
    /* Swap LCD+0x74 (per-scan), comp+0x214 (viewport), AND comp+0x054 (output)
     * to landscape. If per-scan comes from ANY of these, this will work. */
    vlog("TEST00: ALL dims landscape + rotation OFF (kitchen sink)");
    rb->commit_discard_dcache();
    COMP_REG(0x000) = 0;
    LCD_REG(0x70) = 0;
    LCD_REG(0x80) = 0;
    COMP_REG(0x3AC) = 0;
    COMP_REG(0x214) = 0x013F00EF;
    COMP_REG(0x054) = 0x014000F0;  /* ALSO swap output dims */
    LCD_REG(0x74) = 0x014000F0;
    COMP_REG(0x000) = 0;
    LCD_REG(0x70) = 0;
    LCD_REG(0x80) = 0;
    COMP_REG(0x3AC) = 0;            /* rotation OFF */
    LCD_REG(0x74) = 0x014000F0;     /* per_scan=320, num_scans=240 */
    {
        LCD_REG(0x80) = 1;
        LCD_CON = 0x80000DA9;
        lcd_cmd(0x003); lcd_data(0x1030);
        lcd_cmd(0x210); lcd_data(0);
        lcd_cmd(0x211); lcd_data(319);
        lcd_cmd(0x212); lcd_data(0);
        lcd_cmd(0x213); lcd_data(239);
        lcd_cmd(0x200); lcd_data(0);
        lcd_cmd(0x201); lcd_data(0);
        lcd_cmd(0x202);
        while (!(LCD_STATUS & 0x2));
        LCD_CON = 0x80100DB0;
        LCD_REG(0x70) = 1;
        LCD_REG(0x80) = 0;
    }
    COMP_REG(0x000) = 0;
    { volatile int d = 0; while (d++ < 50000); }
    COMP_REG(0x000) = 1;
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 300000); }
    for (int i = 0; i < 10; i++) push_one_frame();
    gram_scan("T00-THE-FIX");
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 5000000) rb->backlight_on(); }
    /* Restore for next tests */
    COMP_REG(0x000) = 0;
    LCD_REG(0x70) = 0;
    COMP_REG(0x3AC) = 0x04004003;
    LCD_REG(0x74) = 0x014000F0;
    {
        LCD_REG(0x80) = 1;
        LCD_CON = 0x80000DA9;
        lcd_cmd(0x003); lcd_data(0x1030);
        lcd_cmd(0x210); lcd_data(0);
        lcd_cmd(0x211); lcd_data(319);
        lcd_cmd(0x212); lcd_data(0);
        lcd_cmd(0x213); lcd_data(239);
        lcd_cmd(0x200); lcd_data(0);
        lcd_cmd(0x201); lcd_data(0);
        lcd_cmd(0x202);
        while (!(LCD_STATUS & 0x2));
        LCD_CON = 0x80100DB0;
        LCD_REG(0x70) = 1;
        LCD_REG(0x80) = 0;
    }

    /* ---- TEST 0a: rotation OFF + porch=0 + re-enable passthrough ---- */
    vlog("TEST0a: rotation OFF + porch=0 + passthrough re-enable");
    rb->commit_discard_dcache();
    COMP_REG(0x000) = 0;       /* stop compositor */
    LCD_REG(0x70) = 0;         /* disable passthrough */
    LCD_REG(0x80) = 0;
    COMP_REG(0x3AC) = 0;       /* rotation OFF */
    LCD_REG(0x78) = 0;          /* NO porch gaps */
    LCD_REG(0x74) = 0x014000F0; /* (320<<16)|240 = 320/scan */
    /* Re-setup GRAM and re-enable */
    {
        LCD_REG(0x80) = 1;
        LCD_CON = 0x80000DA9;
        lcd_cmd(0x003); lcd_data(0x1030);
        lcd_cmd(0x210); lcd_data(0);
        lcd_cmd(0x211); lcd_data(319);
        lcd_cmd(0x212); lcd_data(0);
        lcd_cmd(0x213); lcd_data(239);
        lcd_cmd(0x200); lcd_data(0);
        lcd_cmd(0x201); lcd_data(0);
        lcd_cmd(0x202);
        while (!(LCD_STATUS & 0x2));
        LCD_CON = 0x80100DB0;
        LCD_REG(0x70) = 1;     /* re-enable passthrough with new settings */
        LCD_REG(0x80) = 0;
    }
    COMP_REG(0x000) = 0;
    { volatile int d = 0; while (d++ < 50000); }
    COMP_REG(0x000) = 1;
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 300000); }
    for (int i = 0; i < 10; i++) push_one_frame();
    gram_scan("T0a-noporch");
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 5000000) rb->backlight_on(); }

    /* ---- TEST 0b: same but with comp+0x054 swapped too ---- */
    vlog("TEST0b: rotation OFF + porch=0 + comp054=0x014000F0");
    COMP_REG(0x000) = 0;
    LCD_REG(0x70) = 0;
    COMP_REG(0x054) = 0x014000F0;  /* swap: fast=240, slow=320 */
    LCD_REG(0x74) = 0x014000F0;
    LCD_REG(0x78) = 0;
    {
        LCD_REG(0x80) = 1;
        LCD_CON = 0x80000DA9;
        lcd_cmd(0x003); lcd_data(0x1030);
        lcd_cmd(0x210); lcd_data(0);
        lcd_cmd(0x211); lcd_data(319);
        lcd_cmd(0x212); lcd_data(0);
        lcd_cmd(0x213); lcd_data(239);
        lcd_cmd(0x200); lcd_data(0);
        lcd_cmd(0x201); lcd_data(0);
        lcd_cmd(0x202);
        while (!(LCD_STATUS & 0x2));
        LCD_CON = 0x80100DB0;
        LCD_REG(0x70) = 1;
        LCD_REG(0x80) = 0;
    }
    COMP_REG(0x000) = 0;
    { volatile int d = 0; while (d++ < 50000); }
    COMP_REG(0x000) = 1;
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 300000); }
    for (int i = 0; i < 10; i++) push_one_frame();
    gram_scan("T0b-054swap");
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 5000000) rb->backlight_on(); }

    /* ---- TEST 0c: SCALER FIX — scale 240→320 per scan ---- */
    /* The compositor always sends 240/scan with rotation ON. The scaler
     * can upscale 240→320 per scan (step 0x0C00 = 0.75 = 240/320).
     * AND downscale 320→240 scans (step 0x1555 = 1.33 = 320/240).
     * Result: 320/scan × 240 scans = matches 320×240 GRAM perfectly!
     * This is EXACTLY Apple's UI scaler value (0x0C001555). */
    vlog("TEST0c: SCALER 0x0C001555 + rotation ON + 054=0x014000F0");
    COMP_REG(0x000) = 0;
    LCD_REG(0x70) = 0;
    COMP_REG(0x3AC) = 0x04004003;  /* rotation ON */
    COMP_REG(0x04C) = 0x0C001555;  /* scale 240→320 per scan, 320→240 scans */
    COMP_REG(0x054) = 0x014000F0;  /* output: fast=240(per-scan), slow=320 → BUT scaler makes output 320×240 */
    LCD_REG(0x74) = 0x014000F0;
    LCD_REG(0x78) = 0x000A000A;    /* restore porch */
    {
        LCD_REG(0x80) = 1;
        LCD_CON = 0x80000DA9;
        lcd_cmd(0x003); lcd_data(0x1030);
        lcd_cmd(0x210); lcd_data(0);
        lcd_cmd(0x211); lcd_data(319);
        lcd_cmd(0x212); lcd_data(0);
        lcd_cmd(0x213); lcd_data(239);
        lcd_cmd(0x200); lcd_data(0);
        lcd_cmd(0x201); lcd_data(0);
        lcd_cmd(0x202);
        while (!(LCD_STATUS & 0x2));
        LCD_CON = 0x80100DB0;
        LCD_REG(0x70) = 1;
        LCD_REG(0x80) = 0;
    }
    COMP_REG(0x000) = 0;
    { volatile int d = 0; while (d++ < 50000); }
    COMP_REG(0x000) = 1;
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 300000); }
    for (int i = 0; i < 10; i++) push_one_frame();
    gram_scan("T0c-scaler");
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 5000000) rb->backlight_on(); }

    /* Restore landscape defaults for remaining tests */
    COMP_REG(0x000) = 0;
    LCD_REG(0x70) = 0;
    COMP_REG(0x3AC) = 0;  /* rotation OFF */
    COMP_REG(0x04C) = 0x10001000;
    COMP_REG(0x054) = ((uint32_t)240 << 16) | 320;
    COMP_REG(0x214) = 0x013F00EF;  /* landscape viewport */
    LCD_REG(0x78) = 0x000A000A;
    LCD_REG(0x74) = 0x014000F0;  /* landscape per-scan */

    /* ---- TEST 0d: SWAP comp+0x214 (the SIMPLEST fix) ---- */
    /* comp+0x214 = 0x00EF013F = (239<<16)|319. Upper+1=240=per-scan!
     * Swap to (319<<16)|239 = 0x013F00EF → per-scan=320. */
    vlog("TEST0d: comp+0x214=0x013F00EF (swap viewport → 320/scan)");
    COMP_REG(0x000) = 0;
    LCD_REG(0x70) = 0;
    COMP_REG(0x3AC) = 0;  /* rotation OFF for landscape */
    COMP_REG(0x214) = 0x013F00EF;  /* SWAP: per-scan=320, scans=240 */
    {
        LCD_REG(0x80) = 1;
        LCD_CON = 0x80000DA9;
        lcd_cmd(0x003); lcd_data(0x1030);
        lcd_cmd(0x210); lcd_data(0);
        lcd_cmd(0x211); lcd_data(319);
        lcd_cmd(0x212); lcd_data(0);
        lcd_cmd(0x213); lcd_data(239);
        lcd_cmd(0x200); lcd_data(0);
        lcd_cmd(0x201); lcd_data(0);
        lcd_cmd(0x202);
        while (!(LCD_STATUS & 0x2));
        LCD_CON = 0x80100DB0;
        LCD_REG(0x70) = 1;
        LCD_REG(0x80) = 0;
    }
    COMP_REG(0x000) = 0;
    { volatile int d = 0; while (d++ < 50000); }
    COMP_REG(0x000) = 1;
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 300000); }
    for (int i = 0; i < 10; i++) push_one_frame();
    gram_scan("T0d-214swap");
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 5000000) rb->backlight_on(); }

    /* Restore */
    COMP_REG(0x214) = 0x013F00EF;
    COMP_REG(0x3AC) = 0x04004003;

    /* ---- TEST 0e: rotation ON + 0x214 swapped (rotated landscape) ---- */
    vlog("TEST0e: rotation ON + 0x214=0x013F00EF");
    COMP_REG(0x000) = 0;
    LCD_REG(0x70) = 0;
    COMP_REG(0x3AC) = 0x04004003;
    COMP_REG(0x214) = 0x013F00EF;
    {
        LCD_REG(0x80) = 1;
        LCD_CON = 0x80000DA9;
        lcd_cmd(0x003); lcd_data(0x1030);
        lcd_cmd(0x210); lcd_data(0);
        lcd_cmd(0x211); lcd_data(319);
        lcd_cmd(0x212); lcd_data(0);
        lcd_cmd(0x213); lcd_data(239);
        lcd_cmd(0x200); lcd_data(0);
        lcd_cmd(0x201); lcd_data(0);
        lcd_cmd(0x202);
        while (!(LCD_STATUS & 0x2));
        LCD_CON = 0x80100DB0;
        LCD_REG(0x70) = 1;
        LCD_REG(0x80) = 0;
    }
    COMP_REG(0x000) = 0;
    { volatile int d = 0; while (d++ < 50000); }
    COMP_REG(0x000) = 1;
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 300000); }
    for (int i = 0; i < 10; i++) push_one_frame();
    gram_scan("T0e-rot214");
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 5000000) rb->backlight_on(); }
    COMP_REG(0x214) = 0x013F00EF;

    /* ---- TEST 0f: baseline (known shearing) ---- */
    vlog("TEST0f: rotation ON, original (baseline shearing)");
    COMP_REG(0x3AC) = 0x04004003;
    compositor_retrigger();
    for (int i = 0; i < 10; i++) push_one_frame();
    gram_scan("T0c-baseline");
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 3000000) rb->backlight_on(); }

    /* ---- TEST 1: Y=128 gray ---- */
    vlog("TEST1: Y=128 Cb=128 Cr=128");
    fill_yuv(y_out, cb_out, cr_out, frame_w, frame_h, 128, 128, 128);
    {
        volatile uint8_t *dy = (volatile uint8_t *)(PHYS(y_out) | 0x40000000);
        volatile uint8_t *dcb = (volatile uint8_t *)(PHYS(cb_out) | 0x40000000);
        vlog("  DRAM verify: Y[0]=%d Y[mid]=%d Cb[0]=%d", dy[0], dy[frame_w*120+160], dcb[0]);
    }
    compositor_retrigger();
    for (int i = 0; i < 10; i++) push_one_frame();
    gram_scan("T1");

    /* ---- TEST 2: Y=16 black ---- */
    vlog("TEST2: Y=16 black");
    fill_yuv(y_out, cb_out, cr_out, frame_w, frame_h, 16, 128, 128);
    compositor_retrigger();
    for (int i = 0; i < 10; i++) push_one_frame();
    gram_scan("T2");

    /* ---- TEST 3: Y=255 white ---- */
    vlog("TEST3: Y=255 white");
    fill_yuv(y_out, cb_out, cr_out, frame_w, frame_h, 255, 128, 128);
    compositor_retrigger();
    for (int i = 0; i < 10; i++) push_one_frame();
    gram_scan("T3");

    /* ---- TEST 4: Vertical stripe (left black, right white) ---- */
    /* Y-plane: columns 0-159 = Y=16 (black), columns 160-319 = Y=235 (white).
     * If compositor outputs 320 px/row (landscape): GRAM left half dark, right bright.
     * If compositor outputs 240 px/row (portrait): boundary at different GRAM position.
     * The boundary's location in GRAM (x,y) directly reveals the mapping. */
    vlog("TEST4: Vertical stripe col<160=black col>=160=white");
    COMP_REG(0x000) = 0;  /* clear before filling */
    fill_yuv_vstripe(y_out, cb_out, cr_out, frame_w, frame_h,
                     160, 16, 235);
    {
        volatile uint8_t *dy = (volatile uint8_t *)(PHYS(y_out) | 0x40000000);
        vlog("  DRAM verify: Y[0,0]=%d Y[0,159]=%d Y[0,160]=%d Y[0,319]=%d",
             dy[0], dy[159], dy[160], dy[319]);
    }
    compositor_retrigger();
    for (int i = 0; i < 10; i++) push_one_frame();
    gram_scan_2d("T4-vstripe");
    /* Also test AM=1 with the vstripe */
    compositor_retrigger();
    for (int i = 0; i < 10; i++) push_one_frame_am1();
    gram_scan_2d("T4-am1");
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 3000000) rb->backlight_on(); }

    /* ---- TEST 5: Horizontal stripe (top black, bottom white) ---- */
    /* Y-plane: rows 0-119 = Y=16 (black), rows 120-239 = Y=235 (white).
     * Boundary at row 120 in source. Where it appears in GRAM reveals scan direction.
     * If row maps to GRAM y: boundary at y=120.
     * If row maps to GRAM x: boundary at x=120 (rotation). */
    vlog("TEST5: Horizontal stripe row<120=black row>=120=white");
    COMP_REG(0x000) = 0;  /* clear GO before filling */
    fill_yuv_hstripe(y_out, cb_out, cr_out, frame_w, frame_h,
                     120, 16, 235);
    {
        volatile uint8_t *dy = (volatile uint8_t *)(PHYS(y_out) | 0x40000000);
        vlog("  DRAM verify: Y[0,160]=%d Y[119,160]=%d Y[120,160]=%d Y[239,160]=%d",
             dy[0 * frame_w + 160], dy[119 * frame_w + 160],
             dy[120 * frame_w + 160], dy[239 * frame_w + 160]);
    }
    compositor_retrigger();
    for (int i = 0; i < 10; i++) push_one_frame();
    gram_scan_2d("T5-hstripe");
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 3000000) rb->backlight_on(); }

    /* ---- TEST 6: Single bright pixel at (0,0) ---- */
    /* 3x3 bright block at top-left corner of source (Y=235), rest Y=16.
     * Scan the full 2D grid to find where the bright spot lands in GRAM.
     * This directly reveals the (0,0) mapping from source to GRAM. */
    vlog("TEST6: Single bright pixel at (0,0)");
    COMP_REG(0x000) = 0;
    fill_yuv_single_pixel(y_out, cb_out, cr_out, frame_w, frame_h,
                          0, 0, 235);
    {
        volatile uint8_t *dy = (volatile uint8_t *)(PHYS(y_out) | 0x40000000);
        vlog("  DRAM verify: Y[0,0]=%d Y[0,1]=%d Y[1,0]=%d Y[120,160]=%d",
             dy[0], dy[1], dy[frame_w], dy[120 * frame_w + 160]);
    }
    compositor_retrigger();
    for (int i = 0; i < 10; i++) push_one_frame();
    gram_scan_2d("T6-pixel00");
    /* Also do a fine scan around GRAM corners to pinpoint the bright pixel */
    {
        static const struct { int x, y; } corners[] = {
            {0,0}, {1,0}, {2,0}, {0,1}, {0,2},
            {319,0}, {318,0}, {319,1},
            {0,239}, {0,238}, {1,239},
            {319,239}, {318,239}, {319,238}
        };
        LCD_REG(0x70) = 0;
        LCD_REG(0x80) = 1;
        while (!(LCD_STATUS & 0x2));
        LCD_CON = 0x80000DA8;
        vlog("  GRAM corners[T6]:");
        for (int i = 0; i < 14; i++) {
            lcd_cmd(0x200); lcd_data(corners[i].x);
            lcd_cmd(0x201); lcd_data(corners[i].y);
            lcd_cmd(0x202);
            while (!(LCD_STATUS & 0x2));
            LCD_RDATA = 0;
            { int t = 100000; while (!(LCD_STATUS & 1) && --t > 0); }
            (void)LCD_DBUFF;
            LCD_RDATA = 0;
            { int t = 100000; while (!(LCD_STATUS & 1) && --t > 0); }
            uint32_t g = LCD_DBUFF & 0x3FFFF;
            vlog("    x=%3d y=%3d: R6=%2lu G6=%2lu B6=%2lu (raw=%05lx)",
                 corners[i].x, corners[i].y,
                 (unsigned long)((g >> 12) & 0x3F),
                 (unsigned long)((g >> 6) & 0x3F),
                 (unsigned long)(g & 0x3F),
                 (unsigned long)g);
        }
        while (!(LCD_STATUS & 0x2));
        LCD_CON = 0x80100DB0;
        LCD_REG(0x80) = 0;
        LCD_REG(0x70) = 1;
    }
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 3000000) rb->backlight_on(); }

    /* ---- TEST 7: Horizontal gradient ---- */
    /* Y[row][col] = col & 0xFF. Creates a horizontal gradient that repeats
     * every 256 columns (only 1 full cycle + 64 extra for 320-wide).
     * The GRAM readback pattern reveals exactly how source columns map
     * to GRAM positions:
     *   - If GRAM x matches source col: gradient increases with GRAM x
     *   - If rotated 90 CW: gradient increases with GRAM y
     *   - If rotated 90 CCW: gradient DEcreases with GRAM y
     *   - If the pattern repeats: confirms the tiling/cropping issue
     * The RGB values encode the Y luma, so we can read the original
     * column index back from the brightness. */
    vlog("TEST7: Horizontal gradient Y=col&0xFF");
    fill_yuv_hgradient(y_out, cb_out, cr_out, frame_w, frame_h);
    {
        volatile uint8_t *dy = (volatile uint8_t *)(PHYS(y_out) | 0x40000000);
        vlog("  DRAM verify: Y[0,0]=%d Y[0,64]=%d Y[0,128]=%d Y[0,255]=%d Y[0,319]=%d",
             dy[0], dy[64], dy[128], dy[255], dy[319]);
    }
    compositor_retrigger();
    for (int i = 0; i < 10; i++) push_one_frame();
    gram_scan_2d("T7-grad");
    /* Fine-grained horizontal scan at y=120 to trace the gradient */
    {
        static const int xfine[] = {0, 16, 32, 48, 64, 80, 96, 112,
                                    128, 144, 160, 176, 192, 208,
                                    224, 240, 256, 272, 288, 304, 319};
        LCD_REG(0x70) = 0;
        LCD_REG(0x80) = 1;
        while (!(LCD_STATUS & 0x2));
        LCD_CON = 0x80000DA8;
        vlog("  GRAM xsweep y=120 [T7]:");
        for (int i = 0; i < 21; i++) {
            lcd_cmd(0x200); lcd_data(xfine[i]);
            lcd_cmd(0x201); lcd_data(120);
            lcd_cmd(0x202);
            while (!(LCD_STATUS & 0x2));
            LCD_RDATA = 0;
            { int t = 100000; while (!(LCD_STATUS & 1) && --t > 0); }
            (void)LCD_DBUFF;
            LCD_RDATA = 0;
            { int t = 100000; while (!(LCD_STATUS & 1) && --t > 0); }
            uint32_t g = LCD_DBUFF & 0x3FFFF;
            vlog("    x=%3d: R6=%2lu G6=%2lu B6=%2lu (raw=%05lx)",
                 xfine[i],
                 (unsigned long)((g >> 12) & 0x3F),
                 (unsigned long)((g >> 6) & 0x3F),
                 (unsigned long)(g & 0x3F),
                 (unsigned long)g);
        }
        /* Also sweep vertically at x=160 to check if gradient appears there */
        vlog("  GRAM ysweep x=160 [T7]:");
        for (int y = 0; y < 240; y += 16) {
            lcd_cmd(0x200); lcd_data(160);
            lcd_cmd(0x201); lcd_data(y);
            lcd_cmd(0x202);
            while (!(LCD_STATUS & 0x2));
            LCD_RDATA = 0;
            { int t = 100000; while (!(LCD_STATUS & 1) && --t > 0); }
            (void)LCD_DBUFF;
            LCD_RDATA = 0;
            { int t = 100000; while (!(LCD_STATUS & 1) && --t > 0); }
            uint32_t g = LCD_DBUFF & 0x3FFFF;
            vlog("    y=%3d: R6=%2lu G6=%2lu B6=%2lu (raw=%05lx)",
                 y,
                 (unsigned long)((g >> 12) & 0x3F),
                 (unsigned long)((g >> 6) & 0x3F),
                 (unsigned long)(g & 0x3F),
                 (unsigned long)g);
        }
        while (!(LCD_STATUS & 0x2));
        LCD_CON = 0x80100DB0;
        LCD_REG(0x80) = 0;
        LCD_REG(0x70) = 1;
    }
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 3000000) rb->backlight_on(); }

    /* ---- TEST 8: Vertical gradient (Y = row & 0xFF) ---- */
    vlog("TEST8: Vertical gradient Y=row&0xFF");
    fill_yuv_vgradient(y_out, cb_out, cr_out, frame_w, frame_h);
    compositor_retrigger();
    for (int i = 0; i < 10; i++) push_one_frame();
    gram_scan_2d("T8-vgrad");
    {
        static const int xfine[] = {0, 16, 32, 48, 64, 80, 96, 112,
                                    128, 144, 160, 176, 192, 208,
                                    224, 240, 256, 272, 288, 304, 319};
        LCD_REG(0x70) = 0;
        LCD_REG(0x80) = 1;
        while (!(LCD_STATUS & 0x2));
        LCD_CON = 0x80000DA8;
        vlog("  GRAM xsweep y=120 [T8]:");
        for (int i = 0; i < 21; i++) {
            lcd_cmd(0x200); lcd_data(xfine[i]);
            lcd_cmd(0x201); lcd_data(120);
            lcd_cmd(0x202);
            while (!(LCD_STATUS & 0x2));
            LCD_RDATA = 0;
            { int t = 100000; while (!(LCD_STATUS & 1) && --t > 0); }
            (void)LCD_DBUFF;
            LCD_RDATA = 0;
            { int t = 100000; while (!(LCD_STATUS & 1) && --t > 0); }
            uint32_t g = LCD_DBUFF & 0x3FFFF;
            vlog("    x=%3d: R6=%2lu G6=%2lu B6=%2lu (raw=%05lx)",
                 xfine[i],
                 (unsigned long)((g >> 12) & 0x3F),
                 (unsigned long)((g >> 6) & 0x3F),
                 (unsigned long)(g & 0x3F),
                 (unsigned long)g);
        }
        while (!(LCD_STATUS & 0x2));
        LCD_CON = 0x80100DB0;
        LCD_REG(0x80) = 0;
        LCD_REG(0x70) = 1;
    }
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 3000000) rb->backlight_on(); }

    /* ---- TEST_CSC: BT.601 offset-subtracted inputs ---- */
    /* The hardwired CSC may expect pre-offset-subtracted values:
     *   Y' = Y - 16   (range 0-219)
     *   Cb' = Cb - 128 (range -128..+127, stored as unsigned 0-255)
     *   Cr' = Cr - 128
     * If CSC expects Y'=0 for black instead of Y=16, subtracting offsets
     * in software before writing to the planes should fix the output. */

    /* TEST_CSC-A: Gray — Y=128,Cb=128,Cr=128 → write Y'=112,Cb'=0,Cr'=0 */
    vlog("TEST_CSC-A: Offset-sub gray (Y=128→112, Cb/Cr=128→0)");
    {
        int y_sub = 128 - 16;   /* 112 */
        int cb_sub = 128 - 128; /* 0 */
        int cr_sub = 128 - 128; /* 0 */
        fill_yuv(y_out, cb_out, cr_out, frame_w, frame_h,
                 (uint8_t)y_sub, (uint8_t)cb_sub, (uint8_t)cr_sub);
        volatile uint8_t *dy = (volatile uint8_t *)(PHYS(y_out) | 0x40000000);
        volatile uint8_t *dcb = (volatile uint8_t *)(PHYS(cb_out) | 0x40000000);
        volatile uint8_t *dcr = (volatile uint8_t *)(PHYS(cr_out) | 0x40000000);
        vlog("  DRAM verify: Y[0]=%d Cb[0]=%d Cr[0]=%d", dy[0], dcb[0], dcr[0]);
    }
    compositor_retrigger();
    for (int i = 0; i < 10; i++) push_one_frame();
    gram_scan("CSC-A-gray");

    /* TEST_CSC-B: Black — Y=16,Cb=128,Cr=128 → write Y'=0,Cb'=0,Cr'=0 */
    vlog("TEST_CSC-B: Offset-sub black (Y=16→0, Cb/Cr=128→0)");
    {
        int y_sub = 16 - 16;    /* 0 */
        int cb_sub = 128 - 128; /* 0 */
        int cr_sub = 128 - 128; /* 0 */
        fill_yuv(y_out, cb_out, cr_out, frame_w, frame_h,
                 (uint8_t)y_sub, (uint8_t)cb_sub, (uint8_t)cr_sub);
        volatile uint8_t *dy = (volatile uint8_t *)(PHYS(y_out) | 0x40000000);
        vlog("  DRAM verify: Y[0]=%d (expect 0)", dy[0]);
    }
    compositor_retrigger();
    for (int i = 0; i < 10; i++) push_one_frame();
    gram_scan("CSC-B-black");

    /* TEST_CSC-C: White — Y=235,Cb=128,Cr=128 → write Y'=219,Cb'=0,Cr'=0 */
    vlog("TEST_CSC-C: Offset-sub white (Y=235→219, Cb/Cr=128→0)");
    {
        int y_sub = 235 - 16;   /* 219 */
        int cb_sub = 128 - 128; /* 0 */
        int cr_sub = 128 - 128; /* 0 */
        fill_yuv(y_out, cb_out, cr_out, frame_w, frame_h,
                 (uint8_t)y_sub, (uint8_t)cb_sub, (uint8_t)cr_sub);
        volatile uint8_t *dy = (volatile uint8_t *)(PHYS(y_out) | 0x40000000);
        vlog("  DRAM verify: Y[0]=%d (expect 219)", dy[0]);
    }
    compositor_retrigger();
    for (int i = 0; i < 10; i++) push_one_frame();
    gram_scan("CSC-C-white");
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 3000000) rb->backlight_on(); }

    /* ---- TEST_CSC-D: Entry Mode BGR=0 test ---- */
    /* ILI9326 reg 0x003: 0x1230 has BGR=1 (bit 12). Try BGR=0 (0x0230)
     * to see if the R/B channel swap is from Entry Mode, not CSC.
     * Re-use offset-subtracted gray (Y'=112) to see channel balance. */
    vlog("TEST_CSC-D: BGR=0 Entry Mode (0x0230) + offset-sub gray");
    fill_yuv(y_out, cb_out, cr_out, frame_w, frame_h, 112, 0, 0);
    compositor_retrigger();
    /* Push with Entry Mode BGR=0 */
    {
        { int t = 100000; while ((LCD_REG(0x8C) & 3) && --t > 0); }
        LCD_REG(0x80) = 1;
        LCD_CON = 0x80000DA9;
        lcd_cmd(0x003); lcd_data(0x0230);  /* BGR=0, rest same as normal */
        lcd_cmd(0x210); lcd_data(0);
        lcd_cmd(0x211); lcd_data(319);
        lcd_cmd(0x212); lcd_data(0);
        lcd_cmd(0x213); lcd_data(239);
        lcd_cmd(0x200); lcd_data(0);
        lcd_cmd(0x201); lcd_data(0);
        lcd_cmd(0x202);
        while (!(LCD_STATUS & 0x2));
        LCD_CON = 0x80100DB0;
        LCD_REG(0x80) = 0;
        { int t = 500000; while ((LCD_REG(0x8C) & 3) && --t > 0); }
    }
    for (int i = 0; i < 9; i++) push_one_frame();
    gram_scan("CSC-D-bgr0");
    /* Restore Entry Mode BGR=1 for subsequent tests/shutdown */
    {
        { int t = 100000; while ((LCD_REG(0x8C) & 3) && --t > 0); }
        LCD_REG(0x80) = 1;
        LCD_CON = 0x80000DA9;
        lcd_cmd(0x003); lcd_data(0x1238);  /* BGR=1 restored */
        while (!(LCD_STATUS & 0x2));
        LCD_CON = 0x80100DB0;
        LCD_REG(0x80) = 0;
        { int t = 500000; while ((LCD_REG(0x8C) & 3) && --t > 0); }
    }
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 3000000) rb->backlight_on(); }

    /* ---- Phase 8: Shutdown ---- */
    vlog("Phase 8: Shutdown");
    LCD_REG(0x70) = 0;
    LCD_REG(0x80) = 0;
    COMP_REG(0x000) = 0;
    LCD_REG(0x80) = 0;
    COMP_REG(0x000) = 0;

    lcd_set_con(0x80000DA9);
    lcd_cmd(0x003); lcd_data(0x0230); /* restore Rockbox Entry Mode */
    lcd_set_con(saved_lcd_con);

    LCD_REG(0x88) = saved_88;
    LCD_REG(0x20) = saved_20;
    LCD_REG(0x7C) = saved_7c;
    LCD_REG(0x74) = saved_74;
    LCD_REG(0x78) = saved_78;
    LCD_PHTIME = 0x33;
    { int t = 100000; while ((LCD_REG(0x8C) & 3) && --t > 0); }
    rb->lcd_update();

    vlog("=== Test complete ===");

    rlog_mode = false;
    log_fd = rb->open("/vpu_vpp_test.log", O_WRONLY|O_APPEND, 0666);
    rlog_flush();
    rb->close(log_fd);

    vpu_h264_close(dec);
    rb->cpu_boost(false);
    return PLUGIN_OK;
}

#else
enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    rb->splash(HZ*2, "iPod 6G only");
    return PLUGIN_ERROR;
}
#endif
