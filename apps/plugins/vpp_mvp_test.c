/***************************************************************************
 * S5L8702 VPP MVP Test v102m — AM=1 vertical GRAM fix
 *
 * Key change: landscape GRAM window (0-319 H, 0-239 V) + AM=1 vertical
 * auto-increment. Compositor outputs 240 px/row (portrait due to rotation).
 * AM=1 sweeps 240 vertically per column = matches compositor output.
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
    lcd_cmd(0x003); lcd_data(0x1238);  /* AM=1: vertical auto-increment */
    lcd_cmd(0x210); lcd_data(0);
    lcd_cmd(0x211); lcd_data(319);    /* HE=319: full horizontal (320 source outputs) */
    lcd_cmd(0x212); lcd_data(0);
    lcd_cmd(0x213); lcd_data(239);    /* VE=239: full vertical (240 gate lines) */
    lcd_cmd(0x200); lcd_data(0);
    lcd_cmd(0x201); lcd_data(0);
    lcd_cmd(0x202);
    while (!(LCD_STATUS & 0x2));
    LCD_CON = 0x81100DB9;  /* back to P9 */

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
        uint32_t g = LCD_DBUFF, gs = g >> 1;
        vlog("    y=%3d: R=%2lu G=%2lu B=%2lu (raw=%05lx)",
             pts[i].y,
             (unsigned long)((gs >> 12) & 0x3F),
             (unsigned long)((gs >> 6) & 0x3F),
             (unsigned long)(gs & 0x3F),
             (unsigned long)(g & 0x3FFFF));
    }

    while (!(LCD_STATUS & 0x2));
    LCD_CON = 0x81100DB9;
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
    c[0x214/4] = 0x00EF013F;
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
    vlog("=== VPP MVP Test v102m ===");
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
            vlog("  DECODED: %dx%d", frame_w, frame_h);
            got_frame = true;
        }
        pos = nalu_start + nalu_len;
    }
    if (!got_frame) {
        vlog("ERROR: no frame decoded");
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

    /* Layer 5 config */
    COMP_REG(0x028) = 0x100;
    COMP_REG(0x02C) = frame_w | ((frame_w / 2) << 16);
    COMP_REG(0x030) = 0;
    COMP_REG(0x034) = frame_h | ((uint32_t)frame_w << 16);
    /* comp+0x048: Apple NEVER writes this — do not touch */
    COMP_REG(0x04C) = 0x10001000;
    COMP_REG(0x050) = 0;
    COMP_REG(0x054) = ((uint32_t)240 << 16) | 320;
    /* comp+0x058: Apple NEVER writes this — do not touch */

    COMP_REG(0x038) = PHYS(y_out);
    COMP_REG(0x03C) = PHYS(cr_out);
    COMP_REG(0x040) = 0;
    COMP_REG(0x044) = PHYS(cb_out);
    COMP_REG(0x3AC) = 0x04004003;  /* rotation — required for correct output */
    COMP_REG(0x0D4) = 1;           /* ROM-verified: vtable[0x6c](obj,1,0) → 1|(0<<8)=1 */

    /* CRITICAL: Clear bit 8 to activate BT.601 CSC for video mode.
     * Apple init SETS bit 8 (graphics bypass). Video-enable at ROM 0x14DEC4
     * CLEARS it via FUN_0009B6D4(1). Without clearing, CSC is BYPASSED. */
    { uint32_t v = COMP_REG(0x008); v &= ~0x100; COMP_REG(0x008) = v; }
    vlog("  comp+0x008 after CSC enable: %08lx", (unsigned long)COMP_REG(0x008));

    /* Verify buffer addresses and DRAM content */
    vlog("  Buf: Y=%08lx Cb=%08lx Cr=%08lx",
         (unsigned long)COMP_REG(0x038), (unsigned long)COMP_REG(0x044),
         (unsigned long)COMP_REG(0x03C));

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

    /* LCD init registers — Apple FUN_000ca178, ROM-verified */
    LCD_CON = 0x81100DB9;
    LCD_REG(0x88) = 0x01000000;
    LCD_REG(0x20) = 0x33;
    LCD_REG(0x7C) = 0x00000402;     /* RGB565, 2 P9 transfers (Apple ROM-verified) */

    /* Passthrough registers — Apple FUN_0014deec */
    LCD_REG(0x78) = 0x000A000A;
    LCD_REG(0x74) = 0x00F00140;

    /* Prime panel for DCS mode: send ILI9326 "interface control" via P18
     * to enable the DCS command decoder alongside ILI9326 registers.
     * Then send initial DCS GRAM setup via P8 to verify DCS works. */
    lcd_set_con(0x80000DA9);
    lcd_cmd(0x003); lcd_data(0x1238);  /* AM=1: vertical auto-increment */
    lcd_cmd(0x210); lcd_data(0);
    lcd_cmd(0x211); lcd_data(319);    /* HE=319: full horizontal */
    lcd_cmd(0x212); lcd_data(0);
    lcd_cmd(0x213); lcd_data(239);    /* VE=239: full vertical */
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
        uint32_t gw = LCD_DBUFF, gsw = gw >> 1;
        vlog("  White(160,120): hi=%lu mid=%lu lo=%lu raw=%05lx",
             (unsigned long)((gsw>>12)&0x3F), (unsigned long)((gsw>>6)&0x3F),
             (unsigned long)(gsw&0x3F), (unsigned long)(gw&0x3FFFF));

        lcd_cmd(0x200); lcd_data(155); lcd_cmd(0x201); lcd_data(120);
        lcd_cmd(0x202);
        while (!(LCD_STATUS & 0x2));
        LCD_RDATA = 0; { int t=100000; while(!(LCD_STATUS&1)&&--t>0); } (void)LCD_DBUFF;
        LCD_RDATA = 0; { int t=100000; while(!(LCD_STATUS&1)&&--t>0); }
        uint32_t gb = LCD_DBUFF, gsb = gb >> 1;
        vlog("  Black(155,120): hi=%lu mid=%lu lo=%lu raw=%05lx",
             (unsigned long)((gsb>>12)&0x3F), (unsigned long)((gsb>>6)&0x3F),
             (unsigned long)(gsb&0x3F), (unsigned long)(gb&0x3FFFF));
        while (!(LCD_STATUS & 0x2));
        LCD_CON = 0x81100DB9;
    }

    /* No GRAM re-send needed — push_one_frame does DCS per frame */

    /* Enable passthrough + release bus */
    LCD_REG(0x70) = 1;
    LCD_REG(0x80) = 0;

    /* ---- TEST 0: ACTUAL H.264 DECODED FRAME (CSC active) ---- */
    vlog("TEST0: H.264 frame, CSC active (bit8=0)");
    rb->commit_discard_dcache();
    COMP_REG(0x000) = 1;
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 200000); }
    for (int i = 0; i < 10; i++) push_one_frame();
    gram_scan("T0-csc");
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 3000000) rb->backlight_on(); }

    /* ---- TEST 0b: Same frame, CSC BYPASSED for comparison ---- */
    vlog("TEST0b: H.264 frame, CSC bypassed (bit8=1)");
    { uint32_t v = COMP_REG(0x008); v |= 0x100; COMP_REG(0x008) = v; }
    COMP_REG(0x000) = 1;
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 200000); }
    for (int i = 0; i < 10; i++) push_one_frame();
    gram_scan("T0b-bypass");
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 3000000) rb->backlight_on(); }
    /* Re-enable CSC for remaining tests */
    { uint32_t v = COMP_REG(0x008); v &= ~0x100; COMP_REG(0x008) = v; }

    /* ---- TEST 1: Y=128 gray ---- */
    vlog("TEST1: Y=128 Cb=128 Cr=128");
    fill_yuv(y_out, cb_out, cr_out, frame_w, frame_h, 128, 128, 128);
    {
        volatile uint8_t *dy = (volatile uint8_t *)(PHYS(y_out) | 0x40000000);
        volatile uint8_t *dcb = (volatile uint8_t *)(PHYS(cb_out) | 0x40000000);
        vlog("  DRAM verify: Y[0]=%d Y[mid]=%d Cb[0]=%d", dy[0], dy[frame_w*120+160], dcb[0]);
    }
    COMP_REG(0x000) = 1;
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 200000); }
    for (int i = 0; i < 10; i++) push_one_frame();
    gram_scan("T1");

    /* ---- TEST 2: Y=16 black ---- */
    vlog("TEST2: Y=16 black");
    fill_yuv(y_out, cb_out, cr_out, frame_w, frame_h, 16, 128, 128);
    COMP_REG(0x000) = 1;
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 200000); }
    for (int i = 0; i < 10; i++) push_one_frame();
    gram_scan("T2");

    /* ---- TEST 3: Y=255 white ---- */
    vlog("TEST3: Y=255 white");
    fill_yuv(y_out, cb_out, cr_out, frame_w, frame_h, 255, 128, 128);
    COMP_REG(0x000) = 1;
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 200000); }
    for (int i = 0; i < 10; i++) push_one_frame();
    gram_scan("T3");

    /* ---- Phase 8: Shutdown ---- */
    vlog("Phase 8: Shutdown");
    LCD_REG(0x70) = 0;
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
