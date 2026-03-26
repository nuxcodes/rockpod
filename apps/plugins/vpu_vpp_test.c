/***************************************************************************
 * S5L8702 VPU-B → VPP Pipeline Integration Test
 *
 * Decodes one H.264 I-frame using VPU-B, then feeds the decoded YUV frame
 * through the VPP pipeline (CLCD → MIXER → DISP → Compositor → LCD) to
 * display the video frame on the iPod's LCD panel.
 *
 * All register values are ROM-verified (500+ RE agents, 2026-03-24).
 *
 * Usage: open a .264 (Annex B) file from the file browser.
 ****************************************************************************/

#include "plugin.h"

#ifdef IPOD_6G
#include "s5l87xx.h"

/* ---- VPU-B decode API (via plugin interface) ---- */

#define vpu_h264_buf_size    rb->vpu_h264_buf_size
#define vpu_h264_open        rb->vpu_h264_open
#define vpu_h264_configure   rb->vpu_h264_configure
#define vpu_h264_decode_nalu rb->vpu_h264_decode_nalu
#define vpu_h264_get_frame   rb->vpu_h264_get_frame
#define vpu_h264_close       rb->vpu_h264_close

/* ---- VPP Register Definitions ---- */

#define CLCD_BASE       0x39100000
#define CLCD_REG(off)   (*(volatile uint32_t *)(CLCD_BASE + (off)))

#define MIXER_BASE      0x39200000
#define MIXER_REG(off)  (*(volatile uint32_t *)(MIXER_BASE + (off)))

#define DISP_BASE       0x39300000
#define DISP_REG(off)   (*(volatile uint32_t *)(DISP_BASE + (off)))

#define COMP_BASE       0x38900000
#define COMP_REG(off)   (*(volatile uint32_t *)(COMP_BASE + (off)))

#define LCD_BASE        0x38300000
#define LCD_REG(off)    (*(volatile uint32_t *)(LCD_BASE + (off)))

#define PHYS(x)         ((uint32_t)((uintptr_t)(x) & 0x7FFFFFFF))

/* ---- Logging ---- */

static int log_fd = -1;

static void vlog(const char *fmt, ...)
{
    static char buf[256];
    va_list ap;
    va_start(ap, fmt);
    rb->vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (log_fd >= 0) rb->fdprintf(log_fd, "%s\n", buf);
}

/* ---- LCD command helpers ---- */

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
    for (int i = 0; i + 2 < len; i++) {
        if (buf[i] == 0 && buf[i+1] == 0) {
            if (buf[i+2] == 1) { *sc_len = 3; return i; }
            if (i + 3 < len && buf[i+2] == 0 && buf[i+3] == 1)
                { *sc_len = 4; return i; }
        }
    }
    return -1;
}

/* ---- VPP Clock Control ---- */

static void vpp_clocks_enable(bool enable)
{
    uint32_t pwrcon = PWRCON(0);
    uint32_t mask = (1 << 14) | (1 << 15) | (1 << 16);
    if (enable) pwrcon &= ~mask;
    else        pwrcon |= mask;
    PWRCON(0) = pwrcon;
}

static void vpp_svid_enable(bool enable)
{
    if (enable) {
        volatile uint32_t *cg32 = (volatile uint32_t *)(0x3C500008);
        uint32_t val = *cg32;
        val = (val & 0x0000FFFF) | (0x3003 << 16);
        *cg32 = val;
        for (volatile int i = 0; i < 10000; i++);
    } else {
        CG16_SVID |= (1 << 15);
    }
}

/* ---- CLCD Init (ROM FUN_00167288) ---- */

static void clcd_init(int src_w, int src_h, int out_w, int out_h)
{
    CLCD_REG(0x000) = CLCD_REG(0x000) & 2;  /* reset, preserve busy */

    CLCD_REG(0x004) = 0;
    CLCD_REG(0x008) = 0;
    CLCD_REG(0x00C) = 0;

    /* Scale coefficients (init value, overwritten per-frame with buf addrs) */
    for (int i = 0x028; i <= 0x038; i += 4)
        CLCD_REG(i) = 0x8000000;

    /* Filter config */
    CLCD_REG(0x03C) = src_w;   /* source buffer width */
    CLCD_REG(0x040) = src_h;   /* source buffer height */
    CLCD_REG(0x044) = 0;
    CLCD_REG(0x048) = 0;       /* VP_SRC_V_POSITION = 0 (VF1+C2: ROM 0x1672D4) */
    CLCD_REG(0x04C) = out_w;
    CLCD_REG(0x050) = out_h;
    CLCD_REG(0x054) = 0;       /* FF1: POS_X (was src_w!) */
    CLCD_REG(0x058) = 0;       /* FF1: POS_Y (was src_h!) */
    CLCD_REG(0x05C) = src_w;
    CLCD_REG(0x060) = src_h;

    /* Scale ratios (1:1 for same-size) */
    CLCD_REG(0x064) = ((out_w << 12) / src_w) >> 3;
    CLCD_REG(0x068) = ((out_h << 12) / src_h) >> 4;

    /* Polyphase filter coefficients (from ROM data pool) */
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

    CLCD_REG(0x3C0) = 1;    /* YUV planar mode */
    CLCD_REG(0x3CC) = 1;    /* YUV enable */

    /* Filter/DMA config */
    CLCD_REG(0x200) = 1;
    CLCD_REG(0x20C) = 0;
    CLCD_REG(0x210) = 0;
    CLCD_REG(0x218) = 0x80;
    CLCD_REG(0x21C) = 0x80000080;
    for (int i = 0x220; i <= 0x234; i += 4)
        CLCD_REG(i) = 0x80;
    CLCD_REG(0x238) = 0;
}

/* ---- MIXER Init (ROM FUN_00168510) ---- */

static void mixer_init(void)
{
    for (int i = 0x004; i <= 0x044; i += 4)
        MIXER_REG(i) = 0;
    for (int i = 0x04C; i <= 0x058; i += 4)
        MIXER_REG(i) = 0;

    MIXER_REG(0x048) = 0x00108080;  /* YCbCr bias BT.601 */
    MIXER_REG(0x080) = 0x08440832;  /* color matrix */
    MIXER_REG(0x084) = 0x3B4DACE1;
    MIXER_REG(0x088) = 0x0E1D13DC;
    MIXER_REG(0x800) = 1;           /* global enable */
    MIXER_REG(0x00C) |= 0x200;     /* YUV420 format (FUN_001680e8, ROM 0x1680e8) */
    MIXER_REG(0x000) = 6;           /* pipeline active + data path (no GO yet) */
}

/* ---- DISP Init (ROM FUN_00167c34) ---- */

static const uint32_t disp_regs_200[28] = {
    0x00FD00FE, 0x00000000, 0x00050004, 0x000000FF,
    0x00F700FA, 0x00000001, 0x000E000A, 0x000001FF,
    0x01EC01F2, 0x00000001, 0x001D0014, 0x000001FE,
    0x03D803E4, 0x00000002, 0x00380028, 0x000003FD,
    0x03B003C7, 0x00000005, 0x00790056, 0x000003F6,
    0x072C0766, 0x0000001B, 0x028B0265, 0x04000ECC,
    0x00000000, 0x00000000, 0x00000000, 0x00011A00,
};

static void disp_init(void)
{
    DISP_REG(0x000) = DISP_REG(0x000) & 2;
    DISP_REG(0x00C) = 6;    /* progressive free-running */
    DISP_REG(0x010) = 1;    /* enable */

    for (int i = 0x01C; i <= 0x030; i += 4)
        DISP_REG(i) = 0x800;  /* identity scaling */

    DISP_REG(0x038) = 0;
    DISP_REG(0x03C) = 0x01000700;  /* DD8: critical trigger base */

    DISP_REG(0x0F0) = 0;
    for (int i = 0x100; i <= 0x15C; i += 4)
        DISP_REG(i) = 0;

    DISP_REG(0x180) = 0x10;           /* CSC bypass */
    DISP_REG(0x184) = 0x800000;
    DISP_REG(0x188) = 0x800000;
    DISP_REG(0x18C) = 0x80;
    DISP_REG(0x190) = 0;
    DISP_REG(0x194) = 0x0000EB10;
    DISP_REG(0x198) = 0x02000000;
    DISP_REG(0x19C) = 0x03FF0200;
    DISP_REG(0x1A0) = 0x1FF;
    DISP_REG(0x1A4) = 0x03FF0000;
    DISP_REG(0x1A8) = 0x1FF;
    DISP_REG(0x1C0) = 0x11;           /* DD8: output format */

    for (int i = 0; i < 28; i++)
        DISP_REG(0x200 + i * 4) = disp_regs_200[i];

    DISP_REG(0x014) = 0x0000440C;     /* DD8: timing */

    DISP_REG(0x280) = 0;
    DISP_REG(0x3C0) = 0;
    DISP_REG(0x3D0) = 1;              /* LCD select */
    DISP_REG(0x3C4) = 0x00018000;
    DISP_REG(0x3C8) = 0x00000008;
    DISP_REG(0x3CC) = 0x00018000;
    DISP_REG(0x3D4) = 0x00000008;

    /* Gamma LCD mode */
    for (int i = 0x044; i <= 0x06C; i += 4) DISP_REG(i) = 0;
    for (int i = 0x080; i <= 0x090; i += 4) DISP_REG(i) = 0;
    for (int i = 0x0C0; i <= 0x0D0; i += 4) DISP_REG(i) = 0;
    DISP_REG(0x070) = 0x25d;
    DISP_REG(0x094) = 1;
    DISP_REG(0x098) = 7;
    DISP_REG(0x09C) = 0x14;
    DISP_REG(0x0A0) = 0x28;
    DISP_REG(0x0A4) = 0x3f;
    DISP_REG(0x0A8) = 0x52;
    DISP_REG(0x0AC) = 0x5a;
    DISP_REG(0x0D4) = 1;
    DISP_REG(0x0D8) = 0x09;
    DISP_REG(0x0DC) = 0x1c;
    DISP_REG(0x0E0) = 0x39;
    DISP_REG(0x0E4) = 0x5a;
    DISP_REG(0x0E8) = 0x74;
    DISP_REG(0x0EC) = 0x7e;
    DISP_REG(0x284) = 0;              /* FF2: clear before GO */
}

/* ---- DISP GO (ROM FUN_001682cc) ---- */

static void disp_go(void)
{
    /* DISP_MODE: target 0x1200 (Q3 definitive: bits 9+12 for LCD progressive) */
    DISP_REG(0x008) &= 0xFFFFFFF0;    /* clear format bits 0-3 */
    DISP_REG(0x008) &= ~0x10;          /* clear stale bit 4 = deinterlace (Q4) */
    DISP_REG(0x008) &= 0x1F;           /* clear bits 5-31 including stale bit 5 (Q4) */
    { uint32_t tmp = DISP_REG(0x008); DISP_REG(0x008) = tmp; }
    { uint32_t tmp = DISP_REG(0x008); DISP_REG(0x008) = tmp; }
    DISP_REG(0x008) |= 0x200;
    DISP_REG(0x008) |= 0x1000;
    DISP_REG(0x034) = 0;               /* progressive = 0 (Q3) */

    /* Color correction per chip variant */
    uint32_t chipid2 = *(volatile uint32_t *)0x3D100004;
    if (chipid2 & 0x100) {
        DISP_REG(0x028) = 0x7B5;
        DISP_REG(0x02C) = 0x7FF;
        DISP_REG(0x030) = 0x792;
    } else {
        DISP_REG(0x028) = 0x79F;
        DISP_REG(0x02C) = 0x7B9;
        DISP_REG(0x030) = 0x79A;
    }

    /* FF2: DISP+0x284 = 0 immediately before GO */
    DISP_REG(0x284) = 0;

    /* GO: clear then enable with read-fence */
    DISP_REG(0x000) = 0;
    { uint32_t tmp = DISP_REG(0x000); DISP_REG(0x000) = tmp | 1; }
}

/* ---- Compositor Init (ROM FUN_0014d240) ---- */

static void compositor_init(void)
{
    /* Gate → ungate for clean state */
    PWRCON(0) |= 0x2080;
    for (volatile int d = 0; d < 10000; d++);
    PWRCON(0) &= ~0x2080;
    for (volatile int d = 0; d < 10000; d++);

    volatile uint32_t *c = (volatile uint32_t *)COMP_BASE;

    c[0x200/4] &= ~1;           /* clear SWTRGCMD */
    c[0x004/4] = 1;
    c[0x020/4] = 1;
    c[0x0D4/4] = 1;             /* panel type = LCD */

    /* Per-channel gain (0x1000 = 1.0) */
    c[0x0D8/4] = 0x00001000;
    c[0x0DC/4] = 0;
    c[0x0E0/4] = 0x00001000;
    c[0x0E4/4] = 0;
    c[0x0E8/4] = 0x00001000;
    c[0x0EC/4] = 0;

    /* Mode config (without bit 30 — set last) */
    c[0x008/4] = 0x01118101;
    c[0x00C/4] = 0x000F0F0F;    /* BG_COLOR = Apple's gray (XBGR, ROM 0x14d324) */
    c[0x200/4] |= 0x10080;      /* TRIGCON: bits 16+7 */
    c[0x204/4] = 2;
    c[0x208/4] = 0;
    c[0x20C/4] = 2;
    c[0x210/4] = 0x00010110;    /* DMA dimensions */
    c[0x214/4] = 0x00EF013F;    /* 239<<16 | 319 */

    /* Identity gamma LUT (ROM FUN_00088d8c) */
    for (int i = 0; i < 256; i++) {
        c[0x400/4 + i] = i * 4;
        c[0x800/4 + i] = i * 4;
        c[0xC00/4 + i] = i * 4;
    }

    /* Timing (from v54 iBoot capture) */
    c[0x1EC/4] = 0x0C;
    c[0x1F0/4] = 0x26;
    c[0x1F4/4] = 0x10;
    c[0x1F8/4] = 0x82;
    c[0x1FC/4] = 0x4E;

    /* Set bit 30 LAST (master output enable) */
    c[0x008/4] |= 0x40000000;
    c[0x000/4] = 1;             /* GO */
    c[0x3AC/4] = 0x04004003;    /* pipeline config */
}

/* ---- LCD Passthrough Init ---- */

static void lcd_passthrough_init(int panel_type, uint32_t *saved_con)
{
    *saved_con = LCD_CON;

    /* Wait for bus idle */
    { int t = 100000; while ((LCD_REG(0x8C) & 3) && --t > 0); }

    /* LCD controller config (ROM FUN_000ca178) */
    LCD_CON = 0x81100DB9;
    LCD_REG(0x88) = 0x01000000;
    LCD_REG(0x20) = 0x33;
    LCD_REG(0x7C) = 0x00000402;

    /* Passthrough setup (ROM FUN_0014deec) */
    LCD_REG(0x78) = 0x000A000A;

    /* GRAM window for panel type 2 (ILI9326) */
    if (panel_type >= 2) {
        lcd_set_con(0x80000DA9);
        lcd_cmd(0x210); lcd_data(0);
        lcd_cmd(0x211); lcd_data(319);
        lcd_cmd(0x212); lcd_data(0);
        lcd_cmd(0x213); lcd_data(239);
        lcd_cmd(0x200); lcd_data(0);
        lcd_cmd(0x201); lcd_data(0);
        lcd_cmd(0x202);
        lcd_set_con(0x81100DB9);
    }

    LCD_REG(0x74) = 0x00F00140;
    LCD_REG(0x70) = 1;          /* passthrough enable */
}

/* ---- LCD Push (v134 minimal working pattern) ---- */

static void lcd_push_frame(int panel_type)
{
    /* Wait for bus idle (ROM 0xbe7fc: lcd_wait_ready) */
    { int t = 100000; while ((LCD_REG(0x8C) & 3) && --t > 0); }

    /* Open gate: CPU takes bus (ROM 0xa5094) */
    LCD_REG(0x80) = 1;

    /* Fix 4: GRAM commands + LCD_CON mode transition inside bracket
     * (F1: WR# restart, V2: panel gate, R02: Apple's exact sequence) */
    lcd_set_con(0x80000DA9);  /* P18 for ILI9326 commands */
    if (panel_type >= 2) {
        lcd_cmd(0x210); lcd_data(0);
        lcd_cmd(0x211); lcd_data(319);
        lcd_cmd(0x212); lcd_data(0);
        lcd_cmd(0x213); lcd_data(239);
        lcd_cmd(0x200); lcd_data(0);
        lcd_cmd(0x201); lcd_data(0);
        lcd_cmd(0x202);  /* opens panel GRAM write gate */
    }
    /* v7 Fix 3: Raw P9 restore (W1+W2: Apple NEVER uses polled write with LCD+0x80=1.
     * Poll kills compositor trigger by adding gap after GRAM cmd 0x202.) */
    LCD_CON = 0x81100DB9;

    /* v7 Fix 4: Close gate IMMEDIATELY — no delay (W3: Apple ROM 0xa50c4 has zero delay.
     * Gate-open is ONLY for GRAM commands. Compositor pushes when gate CLOSED.) */
    LCD_REG(0x80) = 0;

    /* Wait for compositor to push frame */
    { int t = 100000; while ((LCD_REG(0x8C) & 3) && --t > 0); }
}

/* ---- Main ---- */

enum plugin_status plugin_start(const void *parameter)
{
    const char *test_path = parameter ? (const char *)parameter
                                       : "/test_iframe.264";
    size_t audio_size;
    uint8_t *audio_buf, *p;
    struct vpu_h264 *dec;
    uint32_t saved_lcd_con = 0;
    int panel_type;

    rb->cpu_boost(true);

    log_fd = rb->open("/vpu_vpp_test.log", O_WRONLY|O_CREAT|O_TRUNC, 0666);
    vlog("=== VPU-B → VPP Integration Test v9 ===");
    vlog("File: %s", test_path);

    /* Detect panel type */
    panel_type = (LCD_CON >> 4) & 3;
    if (panel_type < 2) panel_type = LCD_CON & 1;
    vlog("Panel type: %d", panel_type);

    /* ---- Phase 1: Allocate buffers ---- */

    audio_buf = rb->plugin_get_audio_buffer(&audio_size);
    p = audio_buf;

    size_t dec_size = vpu_h264_buf_size(640, 480);
    if (dec_size + 500000 > audio_size) {
        vlog("ERROR: buffer too small (%lu need %lu)",
             (unsigned long)audio_size, (unsigned long)(dec_size + 500000));
        rb->close(log_fd);
        rb->splash(HZ*3, "Buffer too small!");
        rb->cpu_boost(false);
        return PLUGIN_ERROR;
    }

    /* ---- Phase 2: VPU-B decode ---- */

    vlog("Phase 2: VPU-B decode");
    dec = vpu_h264_open(p, dec_size, 640, 480);
    p += dec_size;
    if (!dec) {
        vlog("ERROR: vpu_h264_open failed");
        rb->close(log_fd);
        rb->splash(HZ*3, "Decoder init failed!");
        rb->cpu_boost(false);
        return PLUGIN_ERROR;
    }
    vlog("  Decoder opened");

    /* Load test file */
    int fd = rb->open(test_path, O_RDONLY);
    if (fd < 0) {
        vlog("ERROR: file not found: %s", test_path);
        vpu_h264_close(dec);
        rb->close(log_fd);
        rb->splash(HZ*3, "File not found!");
        rb->cpu_boost(false);
        return PLUGIN_ERROR;
    }

    uint8_t *file_buf = p;
    int fsize = rb->read(fd, file_buf, 500000);
    rb->close(fd);
    vlog("  Loaded %d bytes", fsize);

    /* Parse Annex B and decode until first frame */
    const uint8_t *y_out = NULL, *cb_out = NULL, *cr_out = NULL;
    int frame_w = 0, frame_h = 0;
    int pos = 0, sc_len;
    bool got_frame = false;

    while (pos < fsize && !got_frame) {
        int sc_pos = find_start_code(file_buf + pos, fsize - pos, &sc_len);
        if (sc_pos < 0) break;
        int nalu_start = pos + sc_pos + sc_len;
        int sc2_len;
        int sc2_pos = find_start_code(file_buf + nalu_start,
                                       fsize - nalu_start, &sc2_len);
        int nalu_len = (sc2_pos >= 0) ? sc2_pos : (fsize - nalu_start);
        int nal_type = file_buf[nalu_start] & 0x1F;

        vlog("  NALU type=%d len=%d", nal_type, nalu_len);

        int ret = vpu_h264_decode_nalu(dec, file_buf + nalu_start, nalu_len);
        if (ret == 1) {
            vpu_h264_get_frame(dec, &y_out, &cb_out, &cr_out,
                                &frame_w, &frame_h);
            vlog("  DECODED: %dx%d Y=%08lx Cb=%08lx Cr=%08lx",
                 frame_w, frame_h,
                 (unsigned long)PHYS(y_out),
                 (unsigned long)PHYS(cb_out),
                 (unsigned long)PHYS(cr_out));
            got_frame = true;
        } else if (ret < 0) {
            vlog("  DECODE ERROR on NALU type=%d", nal_type);
        }

        pos = nalu_start + nalu_len;
    }

    if (!got_frame) {
        vlog("ERROR: no frame decoded");
        vpu_h264_close(dec);
        rb->close(log_fd);
        rb->splash(HZ*3, "No frame decoded!");
        rb->cpu_boost(false);
        return PLUGIN_ERROR;
    }

    rb->splashf(HZ/2, "Decoded %dx%d", frame_w, frame_h);

    /* ---- Phase 3: VPP pipeline init ---- */

    vlog("Phase 3: VPP pipeline init");
    rb->backlight_on();

    /* Enable VPP clocks */
    vpp_svid_enable(true);
    vpp_clocks_enable(true);
    for (volatile int d = 0; d < 10000; d++);

    /* Zero stale VPP state */
    CLCD_REG(0x000) &= 2;
    MIXER_REG(0x000) = 0;
    DISP_REG(0x000) &= 2;
    for (volatile int d = 0; d < 10000; d++);

    /* Init VPP blocks */
    int out_w = 320, out_h = 240;
    clcd_init(frame_w, frame_h, out_w, out_h);
    mixer_init();
    disp_init();
    vlog("  VPP blocks initialized");

    /* Init compositor */
    compositor_init();

    /* Fix 1: Enable compositor Layer 5 (VPP input, YUV420 format)
     * L2: FUN_0014cc90 at ROM 0x14cc90 writes comp+0x028 = 0x100 for layer 5.
     * Without this, compositor ignores VPP data and only shows BG_COLOR. */
    COMP_REG(0x028) = 0x100;                            /* layer 5 enable, YUV format 8 */
    /* Fix 1: Layer 5 stride (N1+N2: ROM 0x14cebc, stale 0x01E001E0 from iBoot) */
    COMP_REG(0x02C) = frame_w | ((frame_w / 2) << 16); /* Y=320 | UV=160<<16 = 0x00A00140 */
    COMP_REG(0x030) = 0;                                /* source origin (0,0) */
    COMP_REG(0x034) = ((uint32_t)frame_h << 16) | frame_w; /* source rect end */
    COMP_REG(0x04C) = 0x10001000;                       /* 1:1 scale (Q16.16) */
    COMP_REG(0x050) = 0;                                /* dest origin (0,0) */
    COMP_REG(0x054) = ((uint32_t)out_h << 16) | out_w;  /* dest size */
    /* DO NOT write comp+0x038-0x044 (kills DMA in bypass mode — v66 proved) */

    vlog("  Compositor initialized + Layer 5 enabled");

    /* Wait for compositor to settle */
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 200000); }

    /* Save LCD registers BEFORE modifying (for shutdown restore) */
    uint32_t saved_lcd_7c = LCD_REG(0x7C);
    uint32_t saved_lcd_88 = LCD_REG(0x88);
    uint32_t saved_lcd_20 = LCD_REG(0x20);

    /* LCD passthrough */
    lcd_passthrough_init(panel_type, &saved_lcd_con);
    vlog("  LCD passthrough initialized");

    /* Compositor GO already fired inside compositor_init() — no second fire needed.
     * WW2 verified: Apple fires comp[0]=1 exactly ONCE (ROM 0x14d420). */

    /* DISP GO */
    disp_go();
    vlog("  DISP GO fired");

    /* Diagnostics: verify register state after init */
    vlog("  DISP_MODE=%08lx MIXER_004=%08lx",
         (unsigned long)DISP_REG(0x008), (unsigned long)MIXER_REG(0x004));
    vlog("  comp+0x008=%08lx comp+0x00C=%08lx comp+0x220=%08lx",
         (unsigned long)COMP_REG(0x008), (unsigned long)COMP_REG(0x00C),
         (unsigned long)COMP_REG(0x220));
    vlog("  LCD_CON=%08lx +0x70=%08lx +0x7C=%08lx +0x88=%08lx",
         (unsigned long)LCD_CON, (unsigned long)LCD_REG(0x70),
         (unsigned long)LCD_REG(0x7C), (unsigned long)LCD_REG(0x88));

    /* ---- Phase 4: Feed decoded frame to CLCD ---- */

    vlog("Phase 4: Feed frame to CLCD");

    /* HH1 VERIFIED: CLCD+0x02C = Cr, CLCD+0x030 = Cb (labels swapped!) */
    CLCD_REG(0x028) = PHYS(y_out);     /* Y plane */
    CLCD_REG(0x02C) = PHYS(cr_out);    /* Cr (NOT Cb!) — HH1 verified */
    CLCD_REG(0x030) = PHYS(cb_out);    /* Cb (NOT Cr!) — HH1 verified */
    CLCD_REG(0x034) = 0;
    CLCD_REG(0x3C4) = frame_w;         /* luma stride */
    CLCD_REG(0x3C8) = frame_w / 2;     /* chroma stride */
    CLCD_REG(0x3C0) = 1;               /* YUV planar mode */

    /* MIXER+0x004: D7 trace gives 0x03 (bits 0+1). DS1 found bit 3 = REG_VIDEO_EN
     * (ROM 0x166d24 layer enable dispatcher, S5PC100 p.1461). Without bit 3, mixer
     * DISCARDS all video data. 0x0B = bits 0+1+3. */
    MIXER_REG(0x004) = 0x0B;
    /* MIXER+0x008: Apple writes 0 (C1 verified). MIXER+0x010: Apple writes 0 (R2+C2). */

    vlog("  Buffers set: Y=%08lx Cr=%08lx Cb=%08lx",
         (unsigned long)CLCD_REG(0x028),
         (unsigned long)CLCD_REG(0x02C),
         (unsigned long)CLCD_REG(0x030));

    /* v5: Comprehensive register dump BEFORE trigger */
    vlog("  REGISTERS BEFORE TRIGGER:");
    vlog("    comp+0x028=%08lx comp+0x02C=%08lx comp+0x00C=%08lx comp+0x220=%08lx",
         (unsigned long)COMP_REG(0x028), (unsigned long)COMP_REG(0x02C),
         (unsigned long)COMP_REG(0x00C), (unsigned long)COMP_REG(0x220));
    vlog("    comp+0x030=%08lx comp+0x034=%08lx comp+0x04C=%08lx",
         (unsigned long)COMP_REG(0x030), (unsigned long)COMP_REG(0x034),
         (unsigned long)COMP_REG(0x04C));
    vlog("    comp+0x050=%08lx comp+0x054=%08lx comp+0x008=%08lx",
         (unsigned long)COMP_REG(0x050), (unsigned long)COMP_REG(0x054),
         (unsigned long)COMP_REG(0x008));
    vlog("    MIXER+0x004=%08lx MIXER+0x008=%08lx MIXER+0x000=%08lx",
         (unsigned long)MIXER_REG(0x004), (unsigned long)MIXER_REG(0x008),
         (unsigned long)MIXER_REG(0x000));
    vlog("    CLCD+0x03C=%08lx +0x040=%08lx +0x048=%08lx +0x000=%08lx",
         (unsigned long)CLCD_REG(0x03C), (unsigned long)CLCD_REG(0x040),
         (unsigned long)CLCD_REG(0x048), (unsigned long)CLCD_REG(0x000));

    /* ---- Phase 5: Trigger pipeline ---- */

    vlog("Phase 5: Pipeline trigger");

    CLCD_REG(0x03C) = frame_w;
    CLCD_REG(0x040) = frame_h;

    /* FF2 VERIFIED: 3 separate RMW ops for DISP trigger */
    CLCD_REG(0x000) |= 1;              /* CLCD enable */
    MIXER_REG(0x000) = 7;              /* MIXER GO */
    { uint32_t t = DISP_REG(0x03C); DISP_REG(0x03C) = t | 1; }  /* latch config */
    { uint32_t t = DISP_REG(0x03C); DISP_REG(0x03C) = t | 2; }  /* latch buffer */
    { uint32_t t = DISP_REG(0x03C); DISP_REG(0x03C) = t | 4; }  /* latch output */

    vlog("  Trigger fired");
    vlog("  CLCD_CTRL: %08lx", (unsigned long)CLCD_REG(0x000));
    vlog("  MIXER_CTRL: %08lx", (unsigned long)MIXER_REG(0x000));
    vlog("  MIXER_00C: %08lx", (unsigned long)MIXER_REG(0x00C));
    vlog("  DISP_CTRL: %08lx", (unsigned long)DISP_REG(0x000));

    /* Wait for pipeline to process */
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 100000); }

    /* ---- Phase 6: Push to LCD ---- */

    vlog("Phase 6: LCD push");

    /* Push frames with timing diagnostic */
    for (int push = 0; push < 10; push++) {
        uint32_t t0 = USEC_TIMER;
        lcd_push_frame(panel_type);
        uint32_t dt = USEC_TIMER - t0;
        vlog("  Push %d: %lu us COMP+0x10=%08lx",
             push, (unsigned long)dt,
             (unsigned long)COMP_REG(0x010));
    }

    /* v5: GRAM readback after pushes */
    vlog("Phase 6b: GRAM readback");
    {
        LCD_REG(0x70) = 0;  /* disable passthrough */
        for (volatile int d = 0; d < 10000; d++);
        uint32_t save_con = LCD_CON;
        lcd_set_con(0x80000DA8);  /* P18 read mode */
        /* Read center pixel (160, 120) */
        lcd_cmd(0x200); lcd_data(160);
        lcd_cmd(0x201); lcd_data(120);
        lcd_cmd(0x202);
        while (!(LCD_STATUS & 0x2));
        LCD_RDATA = 0;
        while (!(LCD_STATUS & 1));
        uint32_t dummy = LCD_DBUFF;
        LCD_RDATA = 0;
        while (!(LCD_STATUS & 1));
        uint32_t px0 = LCD_DBUFF;
        /* Read corner pixel (0, 0) */
        lcd_cmd(0x200); lcd_data(0);
        lcd_cmd(0x201); lcd_data(0);
        lcd_cmd(0x202);
        while (!(LCD_STATUS & 0x2));
        LCD_RDATA = 0;
        while (!(LCD_STATUS & 1));
        dummy = LCD_DBUFF;
        LCD_RDATA = 0;
        while (!(LCD_STATUS & 1));
        uint32_t px1 = LCD_DBUFF;
        vlog("  GRAM center(160,120)=%08lx corner(0,0)=%08lx",
             (unsigned long)px0, (unsigned long)px1);
        LCD_CON = save_con;
        LCD_REG(0x70) = 1;  /* re-enable passthrough */
    }

    /* ---- Phase 7: Post-push register dump ---- */

    vlog("Phase 7: Register state");
    vlog("  CLCD+0x000=%08lx +0x03C=%08lx +0x040=%08lx +0x048=%08lx",
         (unsigned long)CLCD_REG(0x000), (unsigned long)CLCD_REG(0x03C),
         (unsigned long)CLCD_REG(0x040), (unsigned long)CLCD_REG(0x048));
    vlog("  MIXER+0x004=%08lx +0x00C=%08lx +0x000=%08lx",
         (unsigned long)MIXER_REG(0x004), (unsigned long)MIXER_REG(0x00C),
         (unsigned long)MIXER_REG(0x000));
    vlog("  comp+0x008=%08lx +0x00C=%08lx +0x028=%08lx +0x200=%08lx",
         (unsigned long)COMP_REG(0x008), (unsigned long)COMP_REG(0x00C),
         (unsigned long)COMP_REG(0x028), (unsigned long)COMP_REG(0x200));

    /* 5s observe window */
    vlog("  Observing 5s...");
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 5000000) rb->backlight_on(); }

    /* ---- Phase 8: Shutdown ---- */

    vlog("Phase 8: Shutdown");

    /* v9: Restore ALL LCD registers before disabling passthrough (R3).
     * lcd_update() hangs if LCD controller state machine is stuck. */
    { int t = 100000; while ((LCD_REG(0x8C) & 3) && --t > 0); }
    LCD_REG(0x88) = saved_lcd_88;
    LCD_REG(0x20) = saved_lcd_20;
    LCD_REG(0x7C) = saved_lcd_7c;
    LCD_REG(0x70) = 0;
    { int t = 100000; while ((LCD_REG(0x8C) & 3) && --t > 0); }
    LCD_CON = saved_lcd_con;
    { int t = 100000; while ((LCD_REG(0x8C) & 3) && --t > 0); }
    rb->lcd_update();

    /* Close VPU-B */
    vpu_h264_close(dec);

    vlog("=== Test complete ===");
    if (log_fd >= 0) rb->close(log_fd);

    rb->cpu_boost(false);
    return PLUGIN_OK;
}

#else /* !IPOD_6G */

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    rb->splash(HZ*3, "iPod 6G only!");
    return PLUGIN_ERROR;
}

#endif
