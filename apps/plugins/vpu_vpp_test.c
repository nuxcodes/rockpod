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
    /* MIXER+0x00C: format bits [11:8] stay at 0 for video layer 5 (B8-3: overlay only).
     * BUT bit 16 (0x10000) = alpha processing enable IS set for layer 5
     * (B24a: vtable[0x58] at ROM 0x167924: ORR #0x10000). */
    MIXER_REG(0x00C) |= 0x10000;
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
    DISP_REG(0x00C) = 6;    /* Non-progressive free-running (B14 DEFINITIVE: progressive path is TV-out only. Internal LCD always uses 6. FUN_001773b8 skips progressive setter for no TV-out.) */
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
    /* DISP sync registers: FUN_00069994, struct+7=0 for iPod 6G internal LCD (B7-1).
     * FUN_000dcc14(0)=6, uVar6 = 0 | (6 << 14) = 0x18000. DISP+0x3C8 = 8 (progressive).
     * struct+7 (sync type) ≠ struct+2 (display type=1 for mode/gamma). Independent fields. */
    DISP_REG(0x3C4) = 0x00018000;
    DISP_REG(0x3C8) = 0x00000008;
    DISP_REG(0x3CC) = 0x00018000;
    DISP_REG(0x3D4) = 0x00000008;

    /* Gamma LCD mode */
    for (int i = 0x044; i <= 0x06C; i += 4) DISP_REG(i) = 0;
    for (int i = 0x080; i <= 0x090; i += 4) DISP_REG(i) = 0;
    for (int i = 0x0C0; i <= 0x0D0; i += 4) DISP_REG(i) = 0;
    /* Gamma Mode 1 (B14 DEFINITIVE: internal LCD = non-progressive, struct+2=0
     * → FUN_000c9fe0(1). Mode 2 is TV-out only. Pairs with DISP+0x00C=6.) */
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
    /* DISP_MODE: target 0x1200 (B14: bits 9+12, NO bit 1 for internal LCD)
     * Apple sequence: FUN_00168180 does bic#0xF then orr#0x2
     *                 FUN_001682cc does and#0x3F (preserves bit 1) then orr#0x200, orr#0x1000 */
    DISP_REG(0x008) &= 0xFFFFFFF0;     /* clear bits 0-3 (Apple: bic #0xF at ROM 0x168194) */
    /* B14: bit 1 NOT set for internal LCD (non-progressive). Apple only sets it for TV-out.
     * FUN_00168180 param_2=0: clears bits 0-3 but does NOT orr #0x2. DISP_MODE = 0x1200. */
    DISP_REG(0x008) &= ~0x30;          /* clear bits 4+5 (stale Rockbox state, Apple starts clean) */
    DISP_REG(0x008) &= 0x3F;           /* keep bits 0-5, clear 6+ (Apple: and #0x3F at ROM 0x1682e4) */
    { uint32_t tmp = DISP_REG(0x008); DISP_REG(0x008) = tmp; }  /* fence (Apple does 2) */
    { uint32_t tmp = DISP_REG(0x008); DISP_REG(0x008) = tmp; }
    DISP_REG(0x008) |= 0x200;          /* bit 9 (Apple: orr #0x200 at ROM 0x1683a8) */
    DISP_REG(0x008) |= 0x1000;         /* bit 12 (Apple: orr #0x1000 at ROM 0x1683b4) */
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

static uint32_t iboot_timing[5];  /* saved iBoot timing for logging */

static void compositor_init(void)
{
    /* Ungate compositor clocks — Rockbox gates them during boot.
     * Skip the gate step (unlike v9-v11) since there's nothing to reset. */
    PWRCON(0) &= ~0x2080;           /* ungate compositor clocks */
    for (volatile int d = 0; d < 10000; d++);

    volatile uint32_t *c = (volatile uint32_t *)COMP_BASE;

    /* Log compositor register state after ungate */
    for (int i = 0; i < 5; i++)
        iboot_timing[i] = c[(0x1EC + i*4)/4];

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

    /* i80 bus timing (v54 iBoot capture — only known values) */
    c[0x1EC/4] = 0x0C;
    c[0x1F0/4] = 0x26;
    c[0x1F4/4] = 0x10;
    c[0x1F8/4] = 0x82;
    c[0x1FC/4] = 0x4E;

    /* Set bit 30 LAST (master output enable) */
    c[0x008/4] |= 0x40000000;
    c[0x024/4] = 0x00FFFFFF;    /* M2: ROM 0x14D410 mvn r1,#0xFF000000 → vtable[4]→ROM 0x14D900 */
    c[0x000/4] = 1;             /* GO */
    c[0x3AC/4] = 0x04004003;    /* pipeline config */
}

/* ---- LCD Passthrough Init ---- */

static void lcd_passthrough_init(int panel_type, uint32_t *saved_con)
{
    *saved_con = LCD_CON;

    /* Wait for bus idle */
    while (LCD_REG(0x8C) & 3);

    /* Defensive: ensure compositor has bus (B21e: POR=0, Rockbox never writes,
     * but a previous plugin crash could leave LCD+0x80=1) */
    LCD_REG(0x80) = 0;

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
    vlog("=== VPU-B → VPP Integration Test v16 ===");
    vlog("File: %s", test_path);

    /* Detect panel type via GPIO strap pins (B6-1: matches lcd-6g.c:265) */
    panel_type = (PDAT(6) & 0x30) >> 4;
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

    /* ---- Phase 3: VPP pipeline init (B2-3: Apple Phase A→B order) ---- */

    vlog("Phase 3: VPP pipeline init");
    rb->backlight_on();

    int out_w = 320, out_h = 240;

    /* === Phase A: Compositor + LCD passthrough (ROM FUN_0014deec) === */

    /* Init compositor FIRST — downstream must be ready before VPP data flows.
     * Apple's Phase A: compositor_init → comp+0x3AC → GRAM → LCD passthrough.
     * compositor_init ungates its own clocks (PWRCON bits 7,13). */
    compositor_init();

    /* Clear compositor layers 0-4 to prevent stale iBoot UI data (B6-5 medium).
     * Apple calls vtable[0x3c](obj, i, 0) for i=0..5 in FUN_0014d240. */
    for (int i = 0; i < 5; i++)
        COMP_REG(0x05C + i * 0x18) = 0;

    /* Layer 5 setup (VPP video input to compositor)
     * L2: FUN_0014cc90 at ROM 0x14cc90 writes comp+0x028 = 0x100 for layer 5.
     * Without this, compositor ignores VPP data and only shows BG_COLOR. */
    COMP_REG(0x028) = 0x100;                            /* layer 5 enable, YUV format 8 */
    COMP_REG(0x02C) = frame_w | ((frame_w / 2) << 16); /* Y=320 | UV=160<<16 = 0x00A00140 */
    COMP_REG(0x030) = 0;                                /* source origin (0,0) */
    COMP_REG(0x034) = frame_h | ((uint32_t)frame_w << 16);  /* source rect end: [15:0]=H, [31:16]=W (B5-2 verified) */
    COMP_REG(0x04C) = 0x10001000;                       /* 1:1 scale (Q4.12: [15:0]=H, [31:16]=V) */
    COMP_REG(0x050) = 0;                                /* dest origin (0,0) */
    COMP_REG(0x054) = ((uint32_t)out_h << 16) | out_w;  /* dest size */
    /* DO NOT write comp+0x038-0x044 (kills DMA in bypass mode — v66 proved) */

    vlog("  Compositor initialized + Layer 5 enabled");
    vlog("  iBoot timing: %08lx %08lx %08lx %08lx %08lx",
         (unsigned long)iboot_timing[0], (unsigned long)iboot_timing[1],
         (unsigned long)iboot_timing[2], (unsigned long)iboot_timing[3],
         (unsigned long)iboot_timing[4]);

    /* No settle delay needed — Apple uses ZERO delay after compositor GO (B25d).
     * MMIO writes are strongly-ordered, compositor latches config synchronously. */

    /* Save LCD registers BEFORE modifying (for shutdown restore) */
    uint32_t saved_lcd_7c = LCD_REG(0x7C);
    uint32_t saved_lcd_88 = LCD_REG(0x88);
    uint32_t saved_lcd_20 = LCD_REG(0x20);

    /* LCD passthrough (Phase A final step) */
    lcd_passthrough_init(panel_type, &saved_lcd_con);
    vlog("  LCD passthrough initialized");

    /* === Phase B: VPP blocks (ROM FUN_00168450) === */

    /* Enable VPP clocks (Apple: inside vpp_enable_and_init FUN_00166d9c) */
    vpp_svid_enable(true);
    vpp_clocks_enable(true);
    for (volatile int d = 0; d < 10000; d++);

    /* Zero stale VPP state */
    CLCD_REG(0x000) &= 2;
    MIXER_REG(0x000) = 0;
    DISP_REG(0x000) &= 2;
    for (volatile int d = 0; d < 10000; d++);

    /* Init VPP blocks (Apple: clcd_init + mixer_init inside vpp_enable_and_init,
     * then disp_init via mixer_deinterlace) */
    clcd_init(frame_w, frame_h, out_w, out_h);
    mixer_init();
    disp_init();
    vlog("  VPP blocks initialized");

    /* Compositor GO already fired inside compositor_init() — no second fire needed.
     * WW2 verified: Apple fires comp[0]=1 exactly ONCE (ROM 0x14d420). */

    /* DISP GO (Phase B final step, ROM FUN_001682cc) */
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

    /* MIXER+0x004 = 0x13: bits 0+1+4 for LCD non-progressive video (B14 DEFINITIVE).
     *   Bit 0: FUN_00167880 via vtable[0x20] (ROM 0x1678B0: ORR #1, CLCD input enable)
     *   Bit 1: FUN_00168240 at ROM 0x168294 (ORR #2, layer control)
     *   Bit 2: NOT SET for internal LCD (B14: FUN_00168180 param_2=0 → BIC only, no ORR)
     *   Bit 4: FUN_00166D24 case 5 at ROM 0x166D6C (ORR #0x10, VIDEO layer)
     * Progressive mode (bit 2) is TV-out only. Internal LCD always non-progressive. */
    MIXER_REG(0x004) = 0x13;
    /* MIXER+0x008: leave at 0 (B23a DEFINITIVE: 0x100FF was fabricated by merging
     * two separate operations — bit 16 goes to +0x008, 0xFF goes to vtable[0x44]
     * which writes a DIFFERENT register. The constant 0x100FF does NOT exist in ROM.
     * MIXER+0x008 = 0 does NOT block video — it controls alpha blending, not data flow.
     * Apple NEVER calls alpha enable during VPP startup.) */

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

    /* Wait for pipeline to process (~15-20ms for 320x240, compositor auto-pushes
     * via LCD+0x70=1 hardware passthrough — no LCD+0x80 push needed.
     * TRACE-lcd-push DEFINITIVE: Apple NEVER uses LCD+0x80 for video.
     * All 4 ROM writes are UI-only. Video uses continuous passthrough.) */
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 100000); }

    /* ---- Phase 5b: COMPREHENSIVE register dump ---- */
    vlog("Phase 5b: Full register dump after trigger");

    /* CLCD (VP) state */
    vlog("  CLCD: 000=%08lx 004=%08lx 008=%08lx 00C=%08lx",
         (unsigned long)CLCD_REG(0x000), (unsigned long)CLCD_REG(0x004),
         (unsigned long)CLCD_REG(0x008), (unsigned long)CLCD_REG(0x00C));
    vlog("  CLCD: 028=%08lx 02C=%08lx 030=%08lx 034=%08lx",
         (unsigned long)CLCD_REG(0x028), (unsigned long)CLCD_REG(0x02C),
         (unsigned long)CLCD_REG(0x030), (unsigned long)CLCD_REG(0x034));
    vlog("  CLCD: 03C=%08lx 040=%08lx 044=%08lx 048=%08lx",
         (unsigned long)CLCD_REG(0x03C), (unsigned long)CLCD_REG(0x040),
         (unsigned long)CLCD_REG(0x044), (unsigned long)CLCD_REG(0x048));
    vlog("  CLCD: 04C=%08lx 050=%08lx 054=%08lx 058=%08lx",
         (unsigned long)CLCD_REG(0x04C), (unsigned long)CLCD_REG(0x050),
         (unsigned long)CLCD_REG(0x054), (unsigned long)CLCD_REG(0x058));
    vlog("  CLCD: 3C0=%08lx 3C4=%08lx 3C8=%08lx 3CC=%08lx",
         (unsigned long)CLCD_REG(0x3C0), (unsigned long)CLCD_REG(0x3C4),
         (unsigned long)CLCD_REG(0x3C8), (unsigned long)CLCD_REG(0x3CC));

    /* MIXER state */
    vlog("  MIXER: 000=%08lx 004=%08lx 008=%08lx 00C=%08lx",
         (unsigned long)MIXER_REG(0x000), (unsigned long)MIXER_REG(0x004),
         (unsigned long)MIXER_REG(0x008), (unsigned long)MIXER_REG(0x00C));
    vlog("  MIXER: 010=%08lx 048=%08lx 080=%08lx 084=%08lx",
         (unsigned long)MIXER_REG(0x010), (unsigned long)MIXER_REG(0x048),
         (unsigned long)MIXER_REG(0x080), (unsigned long)MIXER_REG(0x084));
    vlog("  MIXER: 088=%08lx 800=%08lx",
         (unsigned long)MIXER_REG(0x088), (unsigned long)MIXER_REG(0x800));

    /* DISP state */
    vlog("  DISP: 000=%08lx 008=%08lx 00C=%08lx 010=%08lx",
         (unsigned long)DISP_REG(0x000), (unsigned long)DISP_REG(0x008),
         (unsigned long)DISP_REG(0x00C), (unsigned long)DISP_REG(0x010));
    vlog("  DISP: 03C=%08lx 180=%08lx 1C0=%08lx 3D0=%08lx",
         (unsigned long)DISP_REG(0x03C), (unsigned long)DISP_REG(0x180),
         (unsigned long)DISP_REG(0x1C0), (unsigned long)DISP_REG(0x3D0));

    /* Compositor state */
    vlog("  COMP: 000=%08lx 004=%08lx 008=%08lx 00C=%08lx",
         (unsigned long)COMP_REG(0x000), (unsigned long)COMP_REG(0x004),
         (unsigned long)COMP_REG(0x008), (unsigned long)COMP_REG(0x00C));
    vlog("  COMP: 020=%08lx 024=%08lx 028=%08lx 02C=%08lx",
         (unsigned long)COMP_REG(0x020), (unsigned long)COMP_REG(0x024),
         (unsigned long)COMP_REG(0x028), (unsigned long)COMP_REG(0x02C));
    vlog("  COMP: 030=%08lx 034=%08lx 04C=%08lx 050=%08lx",
         (unsigned long)COMP_REG(0x030), (unsigned long)COMP_REG(0x034),
         (unsigned long)COMP_REG(0x04C), (unsigned long)COMP_REG(0x050));
    vlog("  COMP: 054=%08lx 0D4=%08lx 200=%08lx 3AC=%08lx",
         (unsigned long)COMP_REG(0x054), (unsigned long)COMP_REG(0x0D4),
         (unsigned long)COMP_REG(0x200), (unsigned long)COMP_REG(0x3AC));
    vlog("  COMP: 1EC=%08lx 1F0=%08lx 1F4=%08lx 1F8=%08lx 1FC=%08lx",
         (unsigned long)COMP_REG(0x1EC), (unsigned long)COMP_REG(0x1F0),
         (unsigned long)COMP_REG(0x1F4), (unsigned long)COMP_REG(0x1F8),
         (unsigned long)COMP_REG(0x1FC));

    /* LCD MCU state */
    vlog("  LCD: CON=%08lx 070=%08lx 074=%08lx 078=%08lx",
         (unsigned long)LCD_CON, (unsigned long)LCD_REG(0x70),
         (unsigned long)LCD_REG(0x74), (unsigned long)LCD_REG(0x78));
    vlog("  LCD: 07C=%08lx 080=%08lx 088=%08lx 08C=%08lx",
         (unsigned long)LCD_REG(0x7C), (unsigned long)LCD_REG(0x80),
         (unsigned long)LCD_REG(0x88), (unsigned long)LCD_REG(0x8C));

    /* ---- Phase 6: Wait for auto-push (B1-4: no LCD+0x80 cycling for video) ---- */
    /* Pipeline auto-pushes via LCD+0x70=1 passthrough. No CPU intervention needed.
     * B1-4 verified: all 4 LCD+0x80 writes in ROM are UI-only (0xA5094, 0xA50C4,
     * 0xBB4C4, 0xBB4F4). Apple NEVER does gate cycling for video frames.
     * GRAM window set once in lcd_passthrough_init, persists across frames. */

    vlog("Phase 6: Waiting for auto-push (no push loop)");
    vlog("  Observing 3s...");
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 3000000) rb->backlight_on(); }

    /* GRAM readback */
    vlog("Phase 6b: GRAM readback");
    {
        LCD_REG(0x70) = 0;
        for (volatile int d = 0; d < 10000; d++);
        lcd_set_con(0x80000DA8);
        lcd_cmd(0x200); lcd_data(160);
        lcd_cmd(0x201); lcd_data(120);
        lcd_cmd(0x202);
        while (!(LCD_STATUS & 0x2));
        LCD_RDATA = 0; while (!(LCD_STATUS & 1)); (void)LCD_DBUFF;
        LCD_RDATA = 0; while (!(LCD_STATUS & 1)); uint32_t px0 = LCD_DBUFF;
        lcd_cmd(0x200); lcd_data(0);
        lcd_cmd(0x201); lcd_data(0);
        lcd_cmd(0x202);
        while (!(LCD_STATUS & 0x2));
        LCD_RDATA = 0; while (!(LCD_STATUS & 1)); (void)LCD_DBUFF;
        LCD_RDATA = 0; while (!(LCD_STATUS & 1)); uint32_t px1 = LCD_DBUFF;
        vlog("  GRAM center(160,120)=%08lx corner(0,0)=%08lx",
             (unsigned long)px0, (unsigned long)px1);
        lcd_set_con(0x81100DB9);  /* B10-5: wait for bus idle before mode switch */
        LCD_REG(0x70) = 1;
    }

    /* Phase 7: removed (was Layer 5 diagnostic — interfered with shutdown) */

    /* ---- Phase 8: Shutdown (B2-2: ROM FUN_00166d9c sequence) ---- */

    vlog("Phase 8: Shutdown");

    /* Step 1: VPP pipeline stop — DISP → MIXER → CLCD order (Q4, ROM 0x166df0)
     * Each block: clear bit 0 (stop), poll bit 1 (idle), with 10x 10us timeout */
    DISP_REG(0x280) = 1;               /* soft reset (ROM 0x166dec) */
    DISP_REG(0x284) |= 1;              /* commit reset */
    DISP_REG(0x03C) &= ~0xF;           /* clear trigger bits */
    DISP_REG(0x000) &= ~1;             /* stop DISP */
    for (int i = 0; i < 10; i++) {
        { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 10000); }  /* 10ms (B15e: Apple uses 10ms, not 10us) */
        if (DISP_REG(0x000) & 2) break;
    }
    MIXER_REG(0x000) &= ~1;            /* stop MIXER */
    for (int i = 0; i < 10; i++) {
        { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 10000); }  /* 10ms */
        if (MIXER_REG(0x000) & 2) break;
    }
    CLCD_REG(0x000) &= ~1;             /* stop CLCD */
    for (int i = 0; i < 10; i++) {
        { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 10000); }  /* 10ms */
        if (CLCD_REG(0x000) & 2) break;
    }
    vlog("  VPP pipeline stopped");

    /* Step 2: Gate VPP clocks (ROM 0x167e98) */
    vpp_clocks_enable(false);
    vpp_svid_enable(false);

    /* Step 3: LCD restore — ORDER IS CRITICAL (B2-2):
     * LCD+0x88 (RGB DMA) off BEFORE LCD+0x70 (passthrough) off.
     * Disabling passthrough while RGB DMA is active leaves LCD in undefined state. */
    LCD_REG(0x80) = 0;                 /* release bus if held */
    while (LCD_REG(0x8C) & 3);         /* wait bus idle */
    LCD_REG(0x88) = saved_lcd_88;      /* RGB DMA off FIRST */
    LCD_REG(0x70) = 0;                 /* passthrough off SECOND */
    LCD_REG(0x7C) = saved_lcd_7c;
    LCD_REG(0x74) = 0;
    LCD_REG(0x78) = 0;
    COMP_REG(0x000) = 0;               /* stop compositor */
    PWRCON(0) |= 0x2080;               /* gate compositor clocks (bits 7+13) */

    /* Step 4: LCD controller restore (B5-5: Apple does NOT toggle LCD clock gate) */
    LCD_PHTIME = 0x33;
    LCD_REG(0x20) = saved_lcd_20;
    LCD_CON = saved_lcd_con;
    while (LCD_REG(0x8C) & 3);
    vlog("  LCD restored");
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
