/***************************************************************************
 * S5L8702 VPP MVP Test — Clean compositor-only pipeline
 *
 * Stripped to minimum: compositor (0x38900000) + LCD (0x38300000) only.
 * No VP/MIXER/DISP (TV-out chain, agent-verified irrelevant for LCD).
 * Comprehensive diagnostics at every stage.
 *
 * CSC block at 0x39A00000 dumped but NOT initialized (format unknown).
 * VPP block at 0x38E00000 dumped but NOT initialized.
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
#define CSC_BASE   0x39A00000
#define VPP_HW_BASE 0x38E00000

#define COMP_REG(off) (*(volatile uint32_t *)(COMP_BASE + (off)))
#define LCD_REG(off)  (*(volatile uint32_t *)(LCD_BASE + (off)))
#define CSC_REG(off)  (*(volatile uint32_t *)(CSC_BASE + (off)))
#define VPP_REG(off)  (*(volatile uint32_t *)(VPP_HW_BASE + (off)))

#define PHYS(x) ((uint32_t)((uintptr_t)(x) & 0x7FFFFFFF))

static int log_fd = -1;
#define RLOG_SIZE 16384
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

/* ---- Annex B NALU parser ---- */
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

/* ---- Compositor Init (ROM FUN_0014D240, compositor-only) ---- */
static uint32_t iboot_timing[5];

static void compositor_init(void)
{
    volatile uint32_t *c = (volatile uint32_t *)COMP_BASE;

    c[0x200/4] &= ~1;           /* clear SWTRGCMD */
    c[0x004/4] = 1;
    c[0x020/4] = 1;

    /* Gamma LUT — identity */
    for (int i = 0; i < 256; i++) {
        c[0x400/4 + i] = i * 4;
        c[0x800/4 + i] = i * 4;
        c[0xC00/4 + i] = i * 4;
    }

    /* Gain/offset — identity */
    c[0x0D8/4] = 0x00001000; c[0x0DC/4] = 0;
    c[0x0E0/4] = 0x00001000; c[0x0E4/4] = 0;
    c[0x0E8/4] = 0x00001000; c[0x0EC/4] = 0;

    /* i80 timing — try iBoot DRAM, fall back to hardcoded */
    {
        volatile uint32_t *src = (volatile uint32_t *)0x0890D2DC;
        uint32_t t[5];
        for (int i = 0; i < 5; i++) t[i] = src[i];
        if (t[0] > 0 && t[0] < 0x1000 && t[4] > 0 && t[4] < 0x1000) {
            for (int i = 0; i < 5; i++) { c[(0x1EC + i*4)/4] = t[i]; iboot_timing[i] = t[i]; }
            vlog("  Using LIVE iBoot timing");
        } else {
            uint32_t hc[] = {0x0C, 0x26, 0x10, 0x82, 0x4E};
            for (int i = 0; i < 5; i++) { c[(0x1EC + i*4)/4] = hc[i]; iboot_timing[i] = hc[i]; }
            vlog("  Using HARDCODED timing");
        }
    }

    /* comp+0x008: 5-step RMW (ROM order) */
    {
        uint32_t v = c[0x008/4];
        v &= ~0x20000000;  /* clear bit 29 */
        v &= ~0x10000000;  /* clear bit 28 */
        v &= ~0x03000000; v |= 0x01000000;  /* bits 25:24 = 01 */
        v &= ~0x00300000; v |= 0x00100000;  /* bit 20 */
        v &= ~0x00030000; v |= 0x00010000;  /* bits 17:16 = 01 */
        v &= ~1; v |= 1;  /* bit 0 */
        c[0x008/4] = v;
    }
    c[0x00C/4] = 0x000F0F0F;  /* BG_COLOR */
    { uint32_t v = c[0x008/4]; v |= 0x8000; c[0x008/4] = v; }    /* bit 15 */
    { uint32_t v = c[0x008/4]; v &= ~2; c[0x008/4] = v; }         /* clear bit 1 */
    { uint32_t v = c[0x008/4]; v |= 0x100; c[0x008/4] = v; }      /* bit 8 */
    { uint32_t v = c[0x008/4]; v |= 0x80; c[0x008/4] = v; }       /* bit 7 = Layer 5 */
    { uint32_t v = c[0x008/4]; v |= 0x40000000; c[0x008/4] = v; } /* bit 30 */

    c[0x200/4] |= 0x10080;      /* TRIGCON: bits 16+7 */
    c[0x204/4] = 2;
    c[0x208/4] = 0;
    c[0x20C/4] = 2;
    c[0x210/4] = 0x00010110;
    c[0x214/4] = 0x00EF013F;    /* 239<<16 | 319 */
    c[0x024/4] = 0x00FFFFFF;    /* color mask */
}

/* ---- Main ---- */

enum plugin_status plugin_start(const void *parameter)
{
    const char *test_path = (const char *)parameter;
    if (!test_path || !*test_path) return PLUGIN_ERROR;

    rb->cpu_boost(true);
    rb->audio_stop();

    log_fd = rb->open("/vpu_vpp_test.log", O_WRONLY|O_CREAT|O_TRUNC, 0666);
    vlog("=== VPP MVP Test v63m ===");
    vlog("File: %s", test_path);

    /* Detect panel type */
    int panel_type = (PDAT(6) & 0x30) >> 4;
    vlog("Panel type: %d", panel_type);

    /* ---- Phase 1: Allocate + decode ---- */
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
                 frame_w, frame_h, (unsigned long)PHYS(y_out),
                 (unsigned long)PHYS(cb_out), (unsigned long)PHYS(cr_out));
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

    /* ---- Phase 2: Solid Y=128 test pattern (uncached) ---- */
    vlog("Phase 2: SOLID Y=128 Cb=128 Cr=128");
    {
        uint8_t *y_unc = (uint8_t *)((uintptr_t)y_out | 0x40000000);
        uint8_t *cb_unc = (uint8_t *)((uintptr_t)cb_out | 0x40000000);
        uint8_t *cr_unc = (uint8_t *)((uintptr_t)cr_out | 0x40000000);
        for (int i = 0; i < frame_w * frame_h; i++) y_unc[i] = 128;
        for (int i = 0; i < (frame_w/2)*(frame_h/2); i++) { cb_unc[i] = 128; cr_unc[i] = 128; }
        rb->commit_discard_dcache();
    }

    /* Switch to RAM logging (no disk I/O after this) */
    rb->close(log_fd);
    log_fd = rb->open("/vpu_vpp_test.log", O_WRONLY|O_APPEND, 0666);
    rb->close(log_fd);
    log_fd = -1;
    rlog_mode = true;

    /* ---- Phase 3: Dump pre-init state ---- */
    vlog("Phase 3: Pre-init dumps");
    PWRCON(0) &= ~0x2080;  /* ungate compositor clocks (bits 7+13) */
    PWRCON(0) &= ~((1<<14)|(1<<15)|(1<<16));  /* ungate VPP clocks for CSC/VPP reads */
    for (volatile int d = 0; d < 10000; d++);

    /* CSC block (0x39A00000) — the missing piece */
    vlog("  CSC(0x39A00000):");
    vlog("    +080: %08lx %08lx %08lx %08lx %08lx %08lx %08lx",
         (unsigned long)CSC_REG(0x080), (unsigned long)CSC_REG(0x084),
         (unsigned long)CSC_REG(0x088), (unsigned long)CSC_REG(0x08C),
         (unsigned long)CSC_REG(0x090), (unsigned long)CSC_REG(0x094),
         (unsigned long)CSC_REG(0x098));
    vlog("    +0A0: %08lx %08lx %08lx %08lx %08lx %08lx %08lx",
         (unsigned long)CSC_REG(0x0A0), (unsigned long)CSC_REG(0x0A4),
         (unsigned long)CSC_REG(0x0A8), (unsigned long)CSC_REG(0x0AC),
         (unsigned long)CSC_REG(0x0B0), (unsigned long)CSC_REG(0x0B4),
         (unsigned long)CSC_REG(0x0B8));
    vlog("    +0C0: %08lx %08lx %08lx %08lx %08lx %08lx %08lx",
         (unsigned long)CSC_REG(0x0C0), (unsigned long)CSC_REG(0x0C4),
         (unsigned long)CSC_REG(0x0C8), (unsigned long)CSC_REG(0x0CC),
         (unsigned long)CSC_REG(0x0D0), (unsigned long)CSC_REG(0x0D4),
         (unsigned long)CSC_REG(0x0D8));
    vlog("    +0E0: %08lx %08lx %08lx %08lx %08lx %08lx %08lx",
         (unsigned long)CSC_REG(0x0E0), (unsigned long)CSC_REG(0x0E4),
         (unsigned long)CSC_REG(0x0E8), (unsigned long)CSC_REG(0x0EC),
         (unsigned long)CSC_REG(0x0F0), (unsigned long)CSC_REG(0x0F4),
         (unsigned long)CSC_REG(0x0F8));

    /* VPP block (0x38E00000) */
    vlog("  VPP(0x38E00000): +010=%08lx +1010=%08lx",
         (unsigned long)VPP_REG(0x010), (unsigned long)VPP_REG(0x1010));

    /* Compositor pre-init */
    vlog("  COMP pre-init: 008=%08lx 028=%08lx 034=%08lx 038=%08lx 3C4=%08lx 3C8=%08lx",
         (unsigned long)COMP_REG(0x008), (unsigned long)COMP_REG(0x028),
         (unsigned long)COMP_REG(0x034), (unsigned long)COMP_REG(0x038),
         (unsigned long)COMP_REG(0x3C4), (unsigned long)COMP_REG(0x3C8));

    /* ---- Phase 4: Compositor init ---- */
    vlog("Phase 4: Compositor init");
    compositor_init();
    vlog("  comp+0x008=%08lx", (unsigned long)COMP_REG(0x008));

    /* Layer 5 config */
    int out_w = 320, out_h = 240;
    COMP_REG(0x028) = 0x100;  /* format 8 = YUV420 3-plane */
    COMP_REG(0x02C) = frame_w | ((frame_w / 2) << 16);
    COMP_REG(0x030) = 0;
    COMP_REG(0x034) = frame_h | ((uint32_t)frame_w << 16);
    COMP_REG(0x04C) = 0x10001000;  /* 1:1 scale */
    COMP_REG(0x050) = 0;
    COMP_REG(0x054) = out_h | ((uint32_t)out_w << 16);

    /* Buffer addresses */
    COMP_REG(0x038) = PHYS(y_out);
    COMP_REG(0x03C) = PHYS(cr_out);  /* Cr at 03C (ROM-verified) */
    COMP_REG(0x040) = 0;
    COMP_REG(0x044) = PHYS(cb_out);  /* Cb at 044 (ROM-verified) */

    /* Verify DRAM */
    {
        volatile uint8_t *dy = (volatile uint8_t *)(PHYS(y_out) | 0x40000000);
        volatile uint8_t *dcb = (volatile uint8_t *)(PHYS(cb_out) | 0x40000000);
        volatile uint8_t *dcr = (volatile uint8_t *)(PHYS(cr_out) | 0x40000000);
        vlog("  DRAM: Y[0]=%d Y[mid]=%d Cb[0]=%d Cr[0]=%d",
             dy[0], dy[120*frame_w+160], dcb[0], dcr[0]);
    }

    /* GO */
    /* comp+0x3C4: from runtime data table (agent-found, not in decompiled functions) */
    COMP_REG(0x3C4) = 2;

    /* GO — do NOT set SWTRGCMD (bit 0). Apple clears it; auto-commit via VSYNC. */
    COMP_REG(0x200) |= 0x80;  /* bit 7 only, NOT 0x81 */
    COMP_REG(0x000) = 1;
    vlog("  Compositor GO fired");

    /* ---- Phase 5: LCD passthrough ---- */
    vlog("Phase 5: LCD passthrough");
    uint32_t saved_lcd_con = LCD_CON;
    uint32_t saved_7c = LCD_REG(0x7C);
    uint32_t saved_88 = LCD_REG(0x88);
    uint32_t saved_20 = LCD_REG(0x20);
    uint32_t saved_74 = LCD_REG(0x74);
    uint32_t saved_78 = LCD_REG(0x78);

    LCD_CON = 0x81100DB9;
    LCD_REG(0x88) = 0x01000000;
    LCD_REG(0x20) = 0x33;
    LCD_REG(0x7C) = 0x00000402;
    LCD_REG(0x78) = 0x000A000A;
    LCD_REG(0x74) = 0x00F00140;

    /* GRAM window (ILI9326 via P18) */
    lcd_set_con(0x80000DA9);
    lcd_cmd(0x3A); lcd_data(0x66);    /* COLMOD 18-bit */
    lcd_cmd(0x003); lcd_data(0x1230); /* Entry Mode BGR */
    lcd_cmd(0x210); lcd_data(0);
    lcd_cmd(0x211); lcd_data(319);
    lcd_cmd(0x212); lcd_data(0);
    lcd_cmd(0x213); lcd_data(239);
    lcd_cmd(0x200); lcd_data(0);
    lcd_cmd(0x201); lcd_data(0);
    lcd_cmd(0x202);
    lcd_set_con(0x81100DB9);

    LCD_REG(0x70) = 1;  /* passthrough ON */
    LCD_REG(0x80) = 0;  /* release bus to compositor */
    vlog("  Passthrough enabled, LCD+0x70=%08lx", (unsigned long)LCD_REG(0x70));

    /* Wait for first render */
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 200000); }

    /* ---- Phase 6: Push loop ---- */
    vlog("Phase 6: Push loop (10 frames)");
    for (int push = 0; push < 10; push++) {
        uint32_t t0 = USEC_TIMER;
        { int t = 100000; while ((LCD_REG(0x8C) & 3) && --t > 0); }
        LCD_REG(0x80) = 1;
        LCD_CON = 0x80000DA9;
        lcd_cmd(0x210); lcd_data(0); lcd_cmd(0x211); lcd_data(319);
        lcd_cmd(0x212); lcd_data(0); lcd_cmd(0x213); lcd_data(239);
        lcd_cmd(0x200); lcd_data(0); lcd_cmd(0x201); lcd_data(0);
        lcd_cmd(0x202);
        while (!(LCD_STATUS & 0x2));
        LCD_CON = 0x81100DB9;
        LCD_REG(0x80) = 0;
        { int t = 100000; while ((LCD_REG(0x8C) & 3) && --t > 0); }
        uint32_t dt = USEC_TIMER - t0;
        if (push == 0)
            vlog("  Push 0: %lu us 8C=%08lx", (unsigned long)dt, (unsigned long)LCD_REG(0x8C));
        else
            vlog("  Push %d: %lu us", push, (unsigned long)dt);
    }

    /* ---- Phase 7: GRAM scan ---- */
    LCD_REG(0x70) = 0;
    LCD_REG(0x80) = 1;
    vlog("Phase 7: GRAM scan");
    {
        while (!(LCD_STATUS & 0x2));
        LCD_CON = 0x80000DA8;  /* P18 read mode */
        static const struct { int x, y; } pts[] = {
            {0,0},{160,0},{319,0},{0,60},{160,60},
            {0,120},{160,120},{319,120},{0,180},{160,180},
            {0,239},{160,239},{319,239}
        };
        for (int i = 0; i < 13; i++) {
            lcd_cmd(0x200); lcd_data(pts[i].x);
            lcd_cmd(0x201); lcd_data(pts[i].y);
            lcd_cmd(0x202);
            while (!(LCD_STATUS & 0x2));
            LCD_RDATA = 0; { int t=100000; while(!(LCD_STATUS&1)&&--t>0); } (void)LCD_DBUFF;
            LCD_RDATA = 0; { int t=100000; while(!(LCD_STATUS&1)&&--t>0); }
            uint32_t g = LCD_DBUFF, gs = g >> 1;
            vlog("  (%3d,%3d): raw=%05lx >>1: R=%2lu G=%2lu B=%2lu",
                 pts[i].x, pts[i].y, (unsigned long)(g&0x3FFFF),
                 (unsigned long)((gs>>12)&0x3F), (unsigned long)((gs>>6)&0x3F),
                 (unsigned long)(gs&0x3F));
        }
        while (!(LCD_STATUS & 0x2));
        LCD_CON = 0x81100DB9;
    }

    /* Hold for 10s */
    vlog("  Holding 10s...");
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 10000000) rb->backlight_on(); }

    /* ---- Phase 8: Shutdown ---- */
    vlog("Phase 8: Shutdown");
    LCD_REG(0x70) = 0;
    LCD_REG(0x80) = 0;
    COMP_REG(0x000) = 0;

    lcd_set_con(0x80000DA9);
    lcd_cmd(0x003); lcd_data(0x0230);
    lcd_cmd(0x3A); lcd_data(0x06);
    lcd_set_con(0x81100DB9);

    LCD_REG(0x88) = saved_88;
    LCD_REG(0x20) = saved_20;
    LCD_REG(0x7C) = saved_7c;
    LCD_REG(0x74) = saved_74;
    LCD_REG(0x78) = saved_78;
    LCD_PHTIME = 0x33;
    LCD_CON = saved_lcd_con;
    { int t = 100000; while ((LCD_REG(0x8C) & 3) && --t > 0); }
    rb->lcd_update();

    vlog("=== Test complete ===");

    /* Flush RAM log */
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
