/***************************************************************************
 * S5L8702 VPU-B → VPP Pipeline Integration Test
 *
 * Decodes one H.264 I-frame using VPU-B, then feeds the decoded YUV frame
 * through the VPP pipeline (CLCD → MIXER → DISP → Compositor → LCD) to
 * display the video frame on the iPod's LCD panel.
 *
 * All register values are ROM-verified (500+ RE agents, 2026-03-24).
 * v31: I420 format fix + bandwidth enable + comp mask (2026-03-30).
 * v32: REVERT v31 format regression — restore ROM-exact format 8 (3-plane planar):
 *      VP+0x30=Cr restored, VP+0x3C0=1, chroma stride=luma/2. Raw-ROM verified
 *      (dispatch 0x167690; compositor comp+0x28=(fmt==8)?0x100:0 at 0x14ce5c).
 *      + read-only clock/domain dump (0x3C700000 x8, CG16, PWRCON, VP) to diagnose
 *      whether VP DMA is clock-gated. (2026-07-08)
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

/* ---- Logging (RAM-buffered for VPP phase when disk I/O is unsafe) ---- */

static int log_fd = -1;
#define RLOG_SIZE 8192   /* v32: enlarged — 3 clock/domain dumps add ~2KB of RAM-mode log */
static char rlog_buf[RLOG_SIZE];
static int rlog_pos = 0;
static bool rlog_mode = false;  /* true = buffer to RAM, false = write to file */

static void vlog(const char *fmt, ...)
{
    static char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int len = rb->vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (rlog_mode) {
        /* RAM buffer mode — no disk I/O (GPIO7.1 = VPP, ATA unsafe) */
        if (rlog_pos + len + 1 < RLOG_SIZE) {
            rb->memcpy(rlog_buf + rlog_pos, buf, len);
            rlog_buf[rlog_pos + len] = '\n';
            rlog_pos += len + 1;
        }
    } else if (log_fd >= 0) {
        rb->fdprintf(log_fd, "%s\n", buf);
    }
}

static void rlog_flush(void)
{
    if (log_fd >= 0 && rlog_pos > 0) {
        rb->write(log_fd, rlog_buf, rlog_pos);
        rlog_pos = 0;
    }
}

/* ---- v32 diagnostic: dump clock/power + VP DMA-relevant state ----
 * Read-only. Captures the data needed to prove/disprove whether the VP DMA is
 * clock-gated. Covers:
 *  - 0x3C700000 per-domain clock controller (8 domains x 0x20) — UNMODELED by Rockbox,
 *    programmed by Apple's SRAM clock manager. Each domain: +0x00 ctrl, +0x04 status,
 *    +0x08 divisor, +0x10 count (driver FUN_0036e130). Enable command writes 0x40 to +0x00.
 *  - CLK block: 0x3C500008 (CG16_2L|SVID), PWRCON(0)=0x3C500048, 0x3CF00200 (VP route)
 *  - VP DMA plane/format regs + CTRL. */
static void dump_clock_vp_state(const char *tag)
{
    vlog("== CLKDUMP [%s] ==", tag);
    vlog("  CG16(3C500008)=%08lx PWRCON0(3C500048)=%08lx VProute(3CF00200)=%08lx",
         (unsigned long)(*(volatile uint32_t *)0x3C500008),
         (unsigned long)(*(volatile uint32_t *)0x3C500048),
         (unsigned long)(*(volatile uint32_t *)0x3CF00200));
    for (int d = 0; d < 8; d++) {
        volatile uint32_t *dom = (volatile uint32_t *)(0x3C700000 + d * 0x20);
        vlog("  DOM%d(%08lx): +00=%08lx +04=%08lx +08=%08lx +10=%08lx", d,
             (unsigned long)(0x3C700000 + d * 0x20),
             (unsigned long)dom[0], (unsigned long)dom[1],
             (unsigned long)dom[2], (unsigned long)dom[4]);
    }
    vlog("  VP: 000=%08lx 028=%08lx 02C=%08lx 030=%08lx 034=%08lx",
         (unsigned long)CLCD_REG(0x000), (unsigned long)CLCD_REG(0x028),
         (unsigned long)CLCD_REG(0x02C), (unsigned long)CLCD_REG(0x030),
         (unsigned long)CLCD_REG(0x034));
    vlog("  VP: 3C0=%08lx 3C4=%08lx 3C8=%08lx 3CC=%08lx 200=%08lx",
         (unsigned long)CLCD_REG(0x3C0), (unsigned long)CLCD_REG(0x3C4),
         (unsigned long)CLCD_REG(0x3C8), (unsigned long)CLCD_REG(0x3CC),
         (unsigned long)CLCD_REG(0x200));
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

    /* VP_IMG_SIZE: DMA buffer geometry. Apple never writes these because iBoot
     * pre-sets them for the boot logo. Rockbox bootloader doesn't use VPP,
     * so these contain STALE values (0x3F, 0xA00 = garbage).
     * Samsung VP uses these for DMA address calculation:
     *   HSIZE (bits 29:16) = stride in pixels
     *   VSIZE (bits 13:0)  = height in lines */
    CLCD_REG(0x014) = (src_w << 16) | src_h;          /* VP_IMG_SIZE_Y */
    CLCD_REG(0x018) = ((src_w / 2) << 16) | (src_h / 2); /* VP_IMG_SIZE_C */

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
    /* DISP_MODE: target 0x1200 (bits 9+12. V13-3: bit 1 is TV-out only, NOT internal LCD.
     * v13 had bit 1 set BEFORE mask which cleared it — it was already dead code.) */
    DISP_REG(0x008) &= 0xFFFFFFF0;     /* clear format bits 0-3 */
    DISP_REG(0x008) &= ~0x10;          /* clear stale bit 4 = deinterlace */
    DISP_REG(0x008) &= 0x3F;           /* J5: preserve bits 0-5 (ROM 0x1682e4, was 0x1F) */
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

    /* DISP ENABLE — starts the display data pump (ROM 0x1683DC-0x1683E8).
     * Without this, DISP never pulls from MIXER, MIXER never generates vsync,
     * VP shadow registers never commit, DMA never starts.
     * THIS WAS THE ROOT CAUSE — missing since v8. */
    DISP_REG(0x000) = 0;                   /* clear */
    { uint32_t tmp = DISP_REG(0x000); }    /* readback (HW sync) */
    DISP_REG(0x000) = DISP_REG(0x000) | 1; /* ENABLE */

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
    c[0x024/4] = 0x00FFFFFF;    /* Fix 2: color mask — ROM 0x14D410 MVN r1,#0xFF000000 */
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
    /* Wait for bus idle — no timeout (Apple ROM 0xbe7fc has none) */
    while (LCD_REG(0x8C) & 3);

    /* CPU takes bus (ROM 0xa5094) */
    LCD_REG(0x80) = 1;

    /* GRAM commands — RAW LCD_CON writes, NO lcd_set_con() polling.
     * OOB-7: lcd_set_con() adds STATUS poll+delay inside bracket,
     * corrupting GRAM command timing. Apple uses raw writes. */
    LCD_CON = 0x80000DA9;  /* P18 cmd mode — RAW, no poll */
    if (panel_type >= 2) {
        lcd_cmd(0x210); lcd_data(0);
        lcd_cmd(0x211); lcd_data(319);
        lcd_cmd(0x212); lcd_data(0);
        lcd_cmd(0x213); lcd_data(239);
        lcd_cmd(0x200); lcd_data(0);
        lcd_cmd(0x201); lcd_data(0);
        lcd_cmd(0x202);  /* panel enters GRAM-write state */
    }
    /* OOB-8: Apple polls LCD_STATUS bit 1 BEFORE restoring LCD_CON.
     * Without poll, P9 restore corrupts 0x202 still on the bus. */
    while (!(LCD_STATUS & 0x2));
    LCD_CON = 0x81100DB9;  /* P9 restore — RAW, no poll */

    /* OOB-7: LCD+0x80=0 STARTS autonomous push. MCU controller pushes
     * full frame (~12ms for 76800px). Apple polls 0x8C with no timeout. */
    LCD_REG(0x80) = 0;
    while (LCD_REG(0x8C) & 3);  /* wait FULL frame transfer */
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
    vlog("=== VPU-B → VPP Integration Test v32 ===");
    vlog("File: %s", test_path);

    /* Detect panel type via GPIO (B6-1: matches lcd-6g.c:265) */
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

    /* ---- Phase 2b: Test pattern — prove CLCD DMA reads buffer ---- */
    vlog("Phase 2b: Y=0xFF solid white test pattern");
    {
        uint8_t *y_buf = (uint8_t *)y_out;
        uint8_t *cb_buf = (uint8_t *)cb_out;
        uint8_t *cr_buf = (uint8_t *)cr_out;
        int chroma_sz = (frame_w / 2) * (frame_h / 2);
        int row, col;
        for (row = 0; row < frame_h; row++)
            for (col = 0; col < frame_w; col++)
                y_buf[row * frame_w + col] = 0xFF; /* solid white */
        rb->memset(cb_buf, 128, chroma_sz);
        rb->memset(cr_buf, 128, chroma_sz);
        rb->commit_discard_dcache();
        vlog("  Y[0,0]=%d Y[0,160]=%d Cb[0]=%d Cr[0]=%d",
             y_buf[0], y_buf[160], cb_buf[0], cr_buf[0]);
    }

    /* ---- GPIO7.1 bracket: switch pin from ATA D1 to VPP function ----
     * SRST-5: Apple sets GPIOCMD 0x0007010F (GPIO7.1 = function 0xF) for VPP DMA.
     * Rockbox's ATA driver sets PCON(7)=0x44444444 (all pins = ATA function 4).
     * Without function 0xF, CLCD DMA cannot access DRAM.
     * ALL file I/O must happen BEFORE this point. Logging switches to RAM buffer. */
    vlog("Switching GPIO7.1 to VPP mode (ATA D1 disconnected — NO DISK I/O)");
    /* Flush log to disk before disabling ATA D1 pin */
    rb->close(log_fd);
    log_fd = rb->open("/vpu_vpp_test.log", O_WRONLY|O_APPEND, 0666);
    rb->close(log_fd);
    log_fd = -1;
    rlog_mode = true;  /* all vlog() now buffers to RAM */

    /* GPIO7.1 = function 0xF for VPP DMA bus access.
     * v28b/v29b had WRONG address (0x3CF0001C instead of 0x3CF000E0)!
     * PCON(x) = GPIO_BASE + x*0x20, NOT x*4. */
    {
        uint32_t old = PCON(7);
        PCON(7) = (old & ~0xF0) | 0xF0;  /* pin 1 nibble = 0xF, preserve others */
        vlog("PCON(7): before=%08lx after=%08lx (expect nibble 1 = F)",
             (unsigned long)old, (unsigned long)PCON(7));
    }

    /* ---- Phase 3: VPP pipeline init ---- */

    vlog("Phase 3: VPP pipeline init");
    rb->backlight_on();

    /* NOTE (v32 RE): the old "Fix 4" below clears PWRCON(0) bit 6 based on a MIS-DECODE of
     * SRAM 0x220043e8(1,0x40) — that fn actually sets bit 0x40 in an SRAM software-vote word
     * at 0x2200f8c0, NOT a PWRCON bit. Kept UNCHANGED this build to isolate the format fix;
     * flagged for removal once the dumps confirm it's inert. */
    PWRCON(0) &= ~(1 << 6);
    vlog("  PWRCON(0) bit 6 ungated: %08lx", (unsigned long)PWRCON(0));

    /* Apple step (d): VP device clock routing — FUN_0036d3f0(0x39,1,1) writes
     * 0x0007010F to 0x3CF00200 (verified: (0x39>>3)<<16|(0x39&7)<<8|0xf). */
    *(volatile uint32_t *)0x3CF00200 = 0x0007010F;

    /* Enable VPP clocks (steps 2-4 of Apple's sequence) */
    vpp_svid_enable(true);
    vpp_clocks_enable(true);
    for (volatile int d = 0; d < 10000; d++);

    /* Pre-init register dump: see what iBoot/HW defaults are BEFORE we touch anything.
     * Clocks just ungated — registers retain state from last power cycle. */
    vlog("Phase 3a: Pre-init CLCD dump (iBoot defaults)");
    {
        volatile uint32_t *c = (volatile uint32_t *)0x39100000;
        vlog("  000=%08lx 004=%08lx 008=%08lx 00C=%08lx 010=%08lx",
             (unsigned long)c[0], (unsigned long)c[1], (unsigned long)c[2],
             (unsigned long)c[3], (unsigned long)c[4]);
        vlog("  014=%08lx 018=%08lx 01C=%08lx 020=%08lx 024=%08lx",
             (unsigned long)c[5], (unsigned long)c[6], (unsigned long)c[7],
             (unsigned long)c[8], (unsigned long)c[9]);
        vlog("  028=%08lx 02C=%08lx 030=%08lx 034=%08lx 038=%08lx",
             (unsigned long)c[0x28/4], (unsigned long)c[0x2C/4], (unsigned long)c[0x30/4],
             (unsigned long)c[0x34/4], (unsigned long)c[0x38/4]);
        vlog("  03C=%08lx 040=%08lx 044=%08lx 048=%08lx 04C=%08lx",
             (unsigned long)c[0x3C/4], (unsigned long)c[0x40/4], (unsigned long)c[0x44/4],
             (unsigned long)c[0x48/4], (unsigned long)c[0x4C/4]);
        vlog("  050=%08lx 054=%08lx 058=%08lx 05C=%08lx 060=%08lx",
             (unsigned long)c[0x50/4], (unsigned long)c[0x54/4], (unsigned long)c[0x58/4],
             (unsigned long)c[0x5C/4], (unsigned long)c[0x60/4]);
        vlog("  064=%08lx 068=%08lx 200=%08lx 3C0=%08lx 3C4=%08lx",
             (unsigned long)c[0x64/4], (unsigned long)c[0x68/4], (unsigned long)c[0x200/4],
             (unsigned long)c[0x3C0/4], (unsigned long)c[0x3C4/4]);
        vlog("  3C8=%08lx 3CC=%08lx", (unsigned long)c[0x3C8/4], (unsigned long)c[0x3CC/4]);
        /* MIXER pre-init */
        c = (volatile uint32_t *)0x39200000;
        vlog("  MXR: 000=%08lx 004=%08lx 008=%08lx 00C=%08lx",
             (unsigned long)c[0], (unsigned long)c[1], (unsigned long)c[2], (unsigned long)c[3]);
    }

    /* v32: clock/power domain state right after our clock pokes (Apple SRAM path not run) */
    dump_clock_vp_state("post-clock-enable");

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
    vlog("  iBoot timing: %08lx %08lx %08lx %08lx %08lx",
         (unsigned long)iboot_timing[0], (unsigned long)iboot_timing[1],
         (unsigned long)iboot_timing[2], (unsigned long)iboot_timing[3],
         (unsigned long)iboot_timing[4]);

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

    /* v32: FORMAT 8 = 3-plane planar YUV420 (Apple's H.264 video format).
     * ROM-verified (raw capstone decode of per-frame dispatch at 0x167690):
     *   format 8 handler (0x1676bc): VP+0x28=Y, VP+0x2C=Cb, VP+0x30=Cr, VP+0x34=0,
     *                                VP+0x3C4=luma_stride, VP+0x3C8=luma_stride/2, VP+0x3C0=1
     *   format 9 handler (0x1676fc): 2-plane NV12, VP+0x3C0=0 (NOT what VPU-B outputs)
     * Cross-confirmed: compositor sets comp+0x28=(fmt==8)?0x100:0 (ROM 0x14ce5c-0x14ce64),
     * i.e. Apple only enables the video layer for format 8. The type-8 H.264 codec reports
     * format 8 (ROM 0x1c052c: mov r6,#8; strb r6,[r8]).
     * v31 "I420 fix" was BACKWARDS (dropped VP+0x30, set 0x3C0=0, chroma stride full). */
    CLCD_REG(0x028) = PHYS(y_out);      /* Y  plane (struct[0]) */
    CLCD_REG(0x02C) = PHYS(cb_out);     /* Cb plane (struct[2] -> VP+0x2C) */
    CLCD_REG(0x030) = PHYS(cr_out);     /* Cr plane (struct[1] -> VP+0x30)  RESTORED */
    CLCD_REG(0x034) = 0;                /* 0 for 4:2:0 (ROM: str lr(=0),[r1,#0x34]) */
    CLCD_REG(0x3C4) = frame_w;          /* luma stride = width (ctx+0x19c) */
    CLCD_REG(0x3C8) = frame_w / 2;      /* chroma stride = luma/2 (ROM: asr #1) */
    CLCD_REG(0x3C0) = 1;                /* 1 = planar 4:2:0 (ROM: mov r0,#1; str [r1,#0x3c0]) */

    /* CLCD+0x008 = 0 (VP_SHADOW_UPDATE cleared, matches Apple init) */

    /* MIXER+0x004: D7 trace gives 0x03 (bits 0+1). DS1 found bit 3 = REG_VIDEO_EN
     * (ROM 0x166d24 layer enable dispatcher, S5PC100 p.1461). Without bit 3, mixer
     * DISCARDS all video data. 0x0B = bits 0+1+3. */
    MIXER_REG(0x004) = 0x1F;   /* bits 0+1+2+3+4. Bit 3 = Samsung VP enable, bit 4 = video layer 5 */
    MIXER_REG(0x008) = 0x100FF; /* DEEP-1: alpha=0 makes layer invisible. 0x100FF = opaque. */

    vlog("  Buffers set: Y=%08lx Cb=%08lx",
         (unsigned long)CLCD_REG(0x028),
         (unsigned long)CLCD_REG(0x02C));
    vlog("  +stride: Y2=%08lx Cb2=%08lx mode=%08lx",
         (unsigned long)CLCD_REG(0x034),
         (unsigned long)CLCD_REG(0x038),
         (unsigned long)CLCD_REG(0x3C0));

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

    /* Per-frame dimension update (Apple FUN_001669c8 case 5 writes these) */
    CLCD_REG(0x044) = frame_w << 4;    /* src_width << 4 (fractional, was 0!) */
    CLCD_REG(0x048) = frame_h;         /* src_height */

    /* Full CLCD register dump for comparison with Apple's ROM */
    vlog("Phase 4b: CLCD register dump");
    for (int off = 0; off <= 0x068; off += 4) {
        vlog("  CLCD+0x%03x=%08lx", off, (unsigned long)CLCD_REG(off));
    }
    vlog("  CLCD+0x200=%08lx +0x3C0=%08lx +0x3C4=%08lx +0x3C8=%08lx +0x3CC=%08lx",
         (unsigned long)CLCD_REG(0x200), (unsigned long)CLCD_REG(0x3C0),
         (unsigned long)CLCD_REG(0x3C4), (unsigned long)CLCD_REG(0x3C8),
         (unsigned long)CLCD_REG(0x3CC));

    /* ---- Phase 5: Trigger pipeline ---- */

    vlog("Phase 5: Pipeline trigger");

    CLCD_REG(0x03C) = frame_w;
    CLCD_REG(0x040) = frame_h;

    dump_clock_vp_state("pre-trigger");

    /* FF2 VERIFIED: 3 separate RMW ops for DISP trigger */
    CLCD_REG(0x000) |= 1;              /* CLCD enable (shadow) */
    MIXER_REG(0x000) = 7;              /* MIXER GO */
    { uint32_t t = DISP_REG(0x03C); DISP_REG(0x03C) = t | 1; }  /* latch config */
    { uint32_t t = DISP_REG(0x03C); DISP_REG(0x03C) = t | 2; }  /* latch buffer */
    { uint32_t t = DISP_REG(0x03C); DISP_REG(0x03C) = t | 4; }  /* latch output */

    /* Note: MIXER+0x008 bit 16 (video layer alpha) already set in Phase 4.
     * CLCD+0x008 (VP_SHADOW_UPDATE) = 0 per Apple ROM — no shadow commit needed. */

    vlog("  Trigger fired");
    vlog("  CLCD_CTRL: %08lx", (unsigned long)CLCD_REG(0x000));
    vlog("  MIXER: 000=%08lx 004=%08lx 008=%08lx 00C=%08lx",
         (unsigned long)MIXER_REG(0x000), (unsigned long)MIXER_REG(0x004),
         (unsigned long)MIXER_REG(0x008), (unsigned long)MIXER_REG(0x00C));
    vlog("  DISP: 000=%08lx 008=%08lx 00C=%08lx 03C=%08lx",
         (unsigned long)DISP_REG(0x000), (unsigned long)DISP_REG(0x008),
         (unsigned long)DISP_REG(0x00C), (unsigned long)DISP_REG(0x03C));

    /* Vsync diagnostic: poll MIXER status for 100ms to see if vsync events occur */
    {
        uint32_t m0 = MIXER_REG(0x000);
        uint32_t m10 = MIXER_REG(0x010);  /* possible interrupt status */
        uint32_t d0 = DISP_REG(0x000);
        uint32_t d3c = DISP_REG(0x03C);
        uint32_t t0 = USEC_TIMER;
        /* Poll for 50ms — check if any status bits change */
        uint32_t m0_changed = 0, m10_changed = 0;
        while ((USEC_TIMER - t0) < 50000) {
            m0_changed |= MIXER_REG(0x000) ^ m0;
            m10_changed |= MIXER_REG(0x010) ^ m10;
        }
        vlog("  VSYNC diag: MXR_000 changed=%08lx MXR_010 changed=%08lx",
             (unsigned long)m0_changed, (unsigned long)m10_changed);
        /* Also read DISP status registers */
        vlog("  DISP: 010=%08lx 014=%08lx 038=%08lx 280=%08lx 284=%08lx",
             (unsigned long)DISP_REG(0x010), (unsigned long)DISP_REG(0x014),
             (unsigned long)DISP_REG(0x038), (unsigned long)DISP_REG(0x280),
             (unsigned long)DISP_REG(0x284));
        /* MIXER extended status */
        vlog("  MXR: 010=%08lx 014=%08lx 018=%08lx 01C=%08lx",
             (unsigned long)MIXER_REG(0x010), (unsigned long)MIXER_REG(0x014),
             (unsigned long)MIXER_REG(0x018), (unsigned long)MIXER_REG(0x01C));
    }

    /* Wait for pipeline to process */
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 100000); }

    dump_clock_vp_state("post-trigger");

    /* ---- Phase 6: Per-frame push loop (K1+K2: gate cycling required) ---- */

    vlog("Phase 6: Push loop (10 frames)");

    for (int push = 0; push < 10; push++) {
        uint32_t t0 = USEC_TIMER;
        lcd_push_frame(panel_type);
        uint32_t dt = USEC_TIMER - t0;
        vlog("  Push %d: %lu us", push, (unsigned long)dt);
    }

    /* DIRECT LCD WRITE TEST — bypass EVERYTHING (VPP, compositor, passthrough).
     * Write pixels directly to panel GRAM via MCU interface.
     * If this shows color → LCD works, issue is VPP pipeline.
     * If still blue → LCD path itself is broken. */
    vlog("  Direct LCD write test (5s)");
    LCD_REG(0x70) = 0;               /* disable passthrough */
    LCD_REG(0x88) = 0;               /* disable RGB DMA */
    for (volatile int d = 0; d < 10000; d++);

    while (LCD_REG(0x8C) & 3);
    LCD_REG(0x80) = 1;               /* CPU takes bus */
    LCD_CON = 0x80000DA9;            /* cmd mode */
    if (panel_type >= 2) {
        lcd_cmd(0x210); lcd_data(0);
        lcd_cmd(0x211); lcd_data(319);
        lcd_cmd(0x212); lcd_data(0);
        lcd_cmd(0x213); lcd_data(239);
        lcd_cmd(0x200); lcd_data(0);
        lcd_cmd(0x201); lcd_data(0);
        lcd_cmd(0x202);
        /* Write 320x240 = 76800 pixels of RED (RGB565: 0xF800) */
        for (int p = 0; p < 76800; p++) {
            lcd_data(0xF800);
        }
    }
    while (!(LCD_STATUS & 0x2));
    LCD_CON = 0x81100DB9;
    LCD_REG(0x80) = 0;
    vlog("  Wrote 76800 red pixels directly to GRAM");
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 5000000) rb->backlight_on(); }

    /* Re-enable passthrough for VPP test */
    LCD_REG(0x70) = 1;

    /* DMA ADDRESS TEST: point at Rockbox code (0x08000000) instead of our buffer.
     * If output CHANGES from blue → DMA IS reading, just wrong color/format.
     * If output STAYS blue → DMA truly doesn't read from ANY address.
     * v32: format-8 3-plane layout (0x28/0x2C/0x30), matching Phase 4. */
    vlog("  DMA test: buf=0x08000000 (Rockbox code) for 5s");
    CLCD_REG(0x028) = 0x08000000;
    CLCD_REG(0x02C) = 0x08010000;
    CLCD_REG(0x030) = 0x08018000;
    CLCD_REG(0x034) = 0;
    for (int vt = 0; vt < 3; vt++) {
        lcd_push_frame(panel_type);
    }
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 5000000) rb->backlight_on(); }

    /* Now point at our actual white buffer (format-8 3-plane) */
    vlog("  DMA test: buf=Y(white) for 5s");
    CLCD_REG(0x028) = PHYS(y_out);
    CLCD_REG(0x02C) = PHYS(cb_out);
    CLCD_REG(0x030) = PHYS(cr_out);
    CLCD_REG(0x034) = 0;
    for (int vt = 0; vt < 3; vt++) {
        lcd_push_frame(panel_type);
    }
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 5000000) rb->backlight_on(); }

    /* Push one more frame to get CURRENT compositor output into GRAM */
    CLCD_REG(0x008) |= 0x10000;  /* shadow commit */
    lcd_push_frame(panel_type);

    /* GRAM readback — 5-point X scan at Y=120 (test gradient) */
    vlog("Phase 6b: GRAM gradient scan (Y=120)");
    {
        static const int test_x[] = {0, 64, 128, 192, 256};
        LCD_REG(0x70) = 0;
        for (volatile int d = 0; d < 10000; d++);
        lcd_set_con(0x80000DA8);
        for (int ti = 0; ti < 5; ti++) {
            lcd_cmd(0x200); lcd_data(test_x[ti]);
            lcd_cmd(0x201); lcd_data(120);
            lcd_cmd(0x202);
            while (!(LCD_STATUS & 0x2));
            LCD_RDATA = 0; while (!(LCD_STATUS & 1)); (void)LCD_DBUFF;
            LCD_RDATA = 0; while (!(LCD_STATUS & 1));
            uint32_t px = LCD_DBUFF;
            vlog("  GRAM(%d,120)=%08lx", test_x[ti], (unsigned long)px);
        }
        LCD_CON = 0x81100DB9;
        LCD_REG(0x70) = 1;
    }

    /* ---- Phase 7: Layer 5 ON/OFF diagnostic ---- */

    vlog("Phase 7: Layer 5 diagnostics");
    vlog("  CLCD+0x000=%08lx MIXER+0x00C=%08lx comp+0x028=%08lx",
         (unsigned long)CLCD_REG(0x000), (unsigned long)MIXER_REG(0x00C),
         (unsigned long)COMP_REG(0x028));

    /* Helper macro: set GRAM window + observe + readback */
#define GRAM_TEST(label, obs_us) do { \
    { int _t = 100000; while ((LCD_REG(0x8C) & 3) && --_t > 0); } \
    LCD_REG(0x80) = 1; \
    lcd_set_con(0x80000DA9); \
    if (panel_type >= 2) { \
        lcd_cmd(0x210); lcd_data(0); lcd_cmd(0x211); lcd_data(319); \
        lcd_cmd(0x212); lcd_data(0); lcd_cmd(0x213); lcd_data(239); \
        lcd_cmd(0x200); lcd_data(0); lcd_cmd(0x201); lcd_data(0); \
        lcd_cmd(0x202); \
    } \
    LCD_CON = 0x81100DB9; LCD_REG(0x80) = 0; \
    { uint32_t _t = USEC_TIMER; while ((USEC_TIMER - _t) < (obs_us)) rb->backlight_on(); } \
    LCD_REG(0x70) = 0; \
    for (volatile int _d = 0; _d < 10000; _d++); \
    lcd_set_con(0x80000DA8); \
    lcd_cmd(0x200); lcd_data(160); lcd_cmd(0x201); lcd_data(120); lcd_cmd(0x202); \
    while (!(LCD_STATUS & 0x2)); \
    LCD_RDATA = 0; while (!(LCD_STATUS & 1)); (void)LCD_DBUFF; \
    LCD_RDATA = 0; while (!(LCD_STATUS & 1)); \
    { uint32_t _g = LCD_DBUFF; vlog("  %s: GRAM=%08lx", label, (unsigned long)_g); } \
    LCD_CON = 0x81100DB9; LCD_REG(0x70) = 1; \
} while(0)

    /* Test A: Layer 5 OFF, BG_COLOR = gray */
    COMP_REG(0x028) = 0;
    COMP_REG(0x000) = 1;
    GRAM_TEST("TestA(L5=OFF,gray)", 3000000);

    /* Test B: YCbCr gray hypothesis (G2) — if compositor uses YCbCr internally,
     * 0x00808080 = Y=128,Cb=128,Cr=128 = neutral gray in YCbCr */
    COMP_REG(0x00C) = 0x00808080;
    COMP_REG(0x000) = 1;
    GRAM_TEST("TestB(L5=OFF,YCbCr_gray)", 3000000);

    /* Test C: Pure channels — R=255 */
    COMP_REG(0x00C) = 0x000000FF;
    COMP_REG(0x000) = 1;
    GRAM_TEST("TestC(L5=OFF,R=255)", 2000000);

    /* Test D: Pure channels — G=255 */
    COMP_REG(0x00C) = 0x0000FF00;
    COMP_REG(0x000) = 1;
    GRAM_TEST("TestD(L5=OFF,G=255)", 2000000);

    /* Test E: Pure channels — B=255 */
    COMP_REG(0x00C) = 0x00FF0000;
    COMP_REG(0x000) = 1;
    GRAM_TEST("TestE(L5=OFF,B=255)", 2000000);

    /* Test F: White */
    COMP_REG(0x00C) = 0x00FFFFFF;
    COMP_REG(0x000) = 1;
    GRAM_TEST("TestF(L5=OFF,white)", 2000000);

#undef GRAM_TEST

    /* ---- Phase 8: Shutdown ---- */

    vlog("Phase 8: Shutdown");

    /* v10: LCD clockgate toggle to reset controller state machine (R3 fallback) */
    { int t = 100000; while ((LCD_REG(0x8C) & 3) && --t > 0); }
    LCD_REG(0x88) = saved_lcd_88;
    LCD_REG(0x20) = saved_lcd_20;
    LCD_REG(0x7C) = saved_lcd_7c;
    LCD_REG(0x70) = 0;
    PWRCON(0) |= (1 << 1);          /* gate LCD clock */
    for (volatile int d = 0; d < 10000; d++);
    PWRCON(0) &= ~(1 << 1);         /* ungate LCD clock */
    for (volatile int d = 0; d < 10000; d++);
    LCD_PHTIME = 0x33;               /* re-init phase timing */
    LCD_CON = saved_lcd_con;
    { int t = 100000; while ((LCD_REG(0x8C) & 3) && --t > 0); }
    rb->lcd_update();

    /* ---- Restore GPIO7.1 to ATA before disk I/O ---- */
    PCON(7) = (PCON(7) & ~0xF0) | 0x40;  /* pin 1 = function 4 (ATA) */
    rlog_mode = false;  /* vlog() back to file mode */
    log_fd = rb->open("/vpu_vpp_test.log", O_WRONLY|O_APPEND, 0666);
    rlog_flush();        /* write RAM-buffered logs to disk */
    vlog("GPIO7.1 restored to ATA mode");

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
