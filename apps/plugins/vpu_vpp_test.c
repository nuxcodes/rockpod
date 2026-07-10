/***************************************************************************
 * S5L8702 VPU-B → VPP Pipeline Integration Test
 *
 * Decodes one H.264 I-frame using VPU-B, then feeds the decoded YUV frame
 * through the VPP pipeline (CLCD → MIXER → DISP → Compositor → LCD) to
 * display the video frame on the iPod's LCD panel.
 *
 * All register values are ROM-verified (500+ RE agents, 2026-03-24).
 * v31: I420 format fix + bandwidth enable + comp mask (2026-03-30).
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
#define RLOG_SIZE 8192
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

/* SPI mode (MIPI DCS) command/data — 8-bit values */
static void spi_cmd(uint8_t cmd)
{
    while (LCD_STATUS & 0x10);
    LCD_WCMD = cmd;
}

static void spi_data(uint8_t data)
{
    while (LCD_STATUS & 0x10);
    LCD_WDATA = data;
}

/* Switch LCD_CON to SPI command mode (ROM FUN_000d16a8 pattern) */
static uint32_t lcd_enter_spi_cmd(void)
{
    while (!(LCD_STATUS & 0x2));
    uint32_t saved = LCD_CON;
    LCD_CON = (saved & 0x80000007) | 0x01000C20;
    return saved;
}

/* Restore LCD_CON (ROM FUN_000d16e8 pattern) */
static void lcd_leave_spi_cmd(uint32_t saved)
{
    while (!(LCD_STATUS & 0x2));
    LCD_CON = saved;
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

    CLCD_REG(0x004) = 0;  /* Apple writes 0 (ROM 0x16729c). NOT pulsing 1→0. */

    CLCD_REG(0x008) = 0;
    CLCD_REG(0x00C) = 0;
    /* VP+0x010/0x014: Apple relies on iBoot to set these, but Rockbox's
     * bootloader doesn't use VPP so iBoot values vary wildly between boots
     * (v35h: 0x000/0x070, v35i: 0x200/0x03D). We must set them explicitly.
     * Apple's iBoot uses 0x200 for VP+0x010 and 0x70 for VP+0x014 when
     * displaying the boot logo at 320x240. Use those values. */
    /* VP+0x014: NOT frame dimensions (Samsung interpretation wrong for Apple variant).
     * iBoot values (0x70=112, 0x3D=61) are small → likely DMA prefetch threshold.
     * Apple NEVER writes this register. Use iBoot's common value 0x70.
     * VP+0x018: similarly unclear. Use 0 (neutral). */
    CLCD_REG(0x010) = 0;  /* progressive mode (v35g value) */
    CLCD_REG(0x014) = 0x70;      /* DMA threshold (iBoot common value) */
    CLCD_REG(0x018) = 0;         /* chroma threshold/config */

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
    MIXER_REG(0x080) = 0x08440832;  /* color matrix coeff Y */
    MIXER_REG(0x084) = 0x3B4DACE1;  /* color matrix coeff Cb */
    MIXER_REG(0x088) = 0x0E1D13DC;  /* color matrix coeff Cr */
    MIXER_REG(0x800) = 1;           /* global enable */
    /* MIXER+0x00C: ROM init = 0. FUN_001680E8 format selector is ONLY called
     * from graphic layer path (cases 0-3). Video layer 5 never calls it.
     * v42 had |= 0x200 from v38 agent — ROM-disproven, removed. */
    MIXER_REG(0x000) = 6;           /* pipeline active (bits 1+2). Bit 2 = SYNC_ENABLE
                                     * (S5PC100: values applied at VSYNC when set). */
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
    /* DISP+0x00C: ROM context[2]==1→5, else→6. Rockbox panel_type=2 → context≠1 → 6.
     * v35h set 5 which BROKE compositor push. v35g had 6 and push worked. */
    DISP_REG(0x00C) = 6;
    DISP_REG(0x010) = 1;    /* enable */

    for (int i = 0x01C; i <= 0x030; i += 4)
        DISP_REG(i) = 0x800;  /* identity scaling */

    DISP_REG(0x038) = 0;
    DISP_REG(0x03C) = 0x01000700;  /* DD8: critical trigger base */

    DISP_REG(0x0F0) = 0;
    for (int i = 0x100; i <= 0x15C; i += 4)
        DISP_REG(i) = 0;

    DISP_REG(0x180) = (DISP_REG(0x180) & ~0x1F) | 0x10;  /* RMW, ROM exact */
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

    /* DISP+0x014: Apple NEVER writes this (zero ROM references).
     * iBoot sets it. v42 wrote 0x777FF from log observation — removed. */

    DISP_REG(0x280) = 0;
    DISP_REG(0x3C0) = 0;              /* ROM 0x167E30 */
    DISP_REG(0x3D0) = 1;              /* LCD select — ROM 0x167E34 */
    /* DISP+0x3C4/3C8/3CC/3D4: iBoot residuals (v35f: 0x18000/0x08/0x18000/0x08).
     * Apple never writes these. Leave at iBoot defaults. */

    /* Gamma LCD mode — zero-fill first (ROM 0x0C9FEC-0x0CA03C)
     * Apple zeros 0x044-0x06C, 0x080-0x090, 0x0C0-0x0D0 (confirmed from disasm). */
    for (int i = 0x044; i <= 0x06C; i += 4) DISP_REG(i) = 0;
    for (int i = 0x080; i <= 0x090; i += 4) DISP_REG(i) = 0;
    for (int i = 0x0C0; i <= 0x0D0; i += 4) DISP_REG(i) = 0;
    /* Gamma: DISP+0x00C=6 → FUN_000c9fe0(2) → type 2 gamma.
     * v42 had type 1 values (0x25D). ROM-verified type 2 from 0x0CA154. */
    DISP_REG(0x070) = 0x281;
    DISP_REG(0x094) = 1;
    DISP_REG(0x098) = 7;
    DISP_REG(0x09C) = 0x15;
    DISP_REG(0x0A0) = 0x2A;
    DISP_REG(0x0A4) = 0x44;
    DISP_REG(0x0A8) = 0x57;
    DISP_REG(0x0AC) = 0x5F;
    DISP_REG(0x0D4) = 2;
    DISP_REG(0x0D8) = 0x0A;
    DISP_REG(0x0DC) = 0x1D;
    DISP_REG(0x0E0) = 0x3C;
    DISP_REG(0x0E4) = 0x5F;
    DISP_REG(0x0E8) = 0x7B;
    DISP_REG(0x0EC) = 0x86;
    DISP_REG(0x284) = 0;              /* FF2: clear before GO */
}

/* ---- DISP GO (ROM FUN_001682cc, case 2 = internal LCD) ---- */

static void disp_go(void)
{
    /* ROM 0x1682E4: mask DISP+0x008 to bits 0-5 only */
    DISP_REG(0x008) &= 0x3F;

    /* ROM 0x168324: progressive mode */
    DISP_REG(0x034) = 0;

    /* ROM 0x16832C-0x168350: DISP_MODE enable sequence.
     * Case 2 (internal LCD) sets bits 6, 18, 21 with self-writes between.
     * Prior versions WRONGLY set bits 9+12 (0x1200) — different bits entirely.
     * ROM confirmed: 0x40 (bit 6), 0x40000 (bit 18), 0x200000 (bit 21). */
    { uint32_t t = DISP_REG(0x008); DISP_REG(0x008) = t | 0x40; }
    { uint32_t t = DISP_REG(0x008); DISP_REG(0x008) = t; }  /* self-write commit */
    { uint32_t t = DISP_REG(0x008); DISP_REG(0x008) = t | 0x40000; }
    { uint32_t t = DISP_REG(0x008); DISP_REG(0x008) = t | 0x200000; }

    /* Color correction per chip variant (ROM 0x16835C-0x168370).
     * Writes to DISP+0x01C/0x020/0x024 — overwrites disp_init's 0x800 scaling.
     * Prior versions WRONGLY wrote to 0x028/0x02C/0x030 (wrong offsets). */
    uint32_t chipid2 = *(volatile uint32_t *)0x3D100004;
    if (chipid2 & 0x100) {
        DISP_REG(0x01C) = 0x7FF;
        DISP_REG(0x020) = 0x792;
        DISP_REG(0x024) = 0x7B5;
    } else {
        DISP_REG(0x01C) = 0x7B9;
        DISP_REG(0x020) = 0x79A;
        DISP_REG(0x024) = 0x79F;
    }

    /* DISP ENABLE (ROM 0x1683DC-0x1683E8): write 0, read back, set bit 0.
     * Also set bit 1 (ENVID_F) for continuous VSYNC generation.
     * Samsung: ENVID=1 + ENVID_F=0 stops after current frame.
     * Without VSYNC, compositor render never triggers. */
    DISP_REG(0x000) = 0;
    { uint32_t t = DISP_REG(0x000); DISP_REG(0x000) = t | 3; }  /* bits 0+1 */
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
    /* Capture iBoot residuals before we overwrite */
    for (int i = 0; i < 5; i++)
        iboot_timing[i] = c[(0x1EC + i*4)/4];
    vlog("  comp residuals: 1E0=%08lx 1E4=%08lx 1E8=%08lx",
         (unsigned long)c[0x1E0/4], (unsigned long)c[0x1E4/4],
         (unsigned long)c[0x1E8/4]);

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

    /* Identity gamma LUT — MUST be before comp+0x008 (ROM order: step 4 before step 7).
     * If gamma loads AFTER comp+0x008, the pipeline renders with empty gamma → 0x03FF. */
    for (int i = 0; i < 256; i++) {
        c[0x400/4 + i] = i * 4;
        c[0x800/4 + i] = i * 4;
        c[0xC00/4 + i] = i * 4;
    }

    /* Mode config — ROM sets comp+0x008 in FIVE separate RMW steps.
     * Writing all bits at once (0x01118101) may not work — HW state machine
     * might need sequential enables. Match ROM order exactly:
     * Step 7: FUN_BF820(0,0,0x010101,1) → bits 0,16,20,24 */
    {
        uint32_t v = c[0x008/4];
        v &= ~0x20000000;  /* clear bit 29 */
        v &= ~0x10000000;  /* clear bit 28 */
        v &= ~0x03000000; v |= 0x01000000;  /* bits 25:24 = 01 (Apple ROM value, 24-bit) (18-bit output, was 01=24-bit) (ROM-only value) */
        v &= ~0x00300000; v |= 0x00100000;  /* bit 20 = 1 (ROM-only value) */
        v &= ~0x00030000; v |= 0x00010000;  /* bits 17:16 = 01 (Apple ROM value) (18-bit, was 01=24-bit) (ROM-only value) */
        v &= ~1; v |= 1;  /* bit 0 */
        c[0x008/4] = v;
    }
    /* Step 8: BG_COLOR */
    c[0x00C/4] = 0x000F0F0F;
    /* Step 9: FUN_A7F98(1) → bit 15 */
    { uint32_t v = c[0x008/4]; v |= 0x8000; c[0x008/4] = v; }
    /* Step 10: disable all layers (bits 2-7 = 0) — already 0 from above */
    /* Step 11: FUN_BF8B0(0) → clear bit 1 */
    { uint32_t v = c[0x008/4]; v &= ~2; c[0x008/4] = v; }
    /* Step 12: FUN_9B6D4(0) → set bit 8 (inverted logic) */
    { uint32_t v = c[0x008/4]; v |= 0x100; c[0x008/4] = v; }
    /* Enable Layer 5 (bit 7). ROM init disables all layers, but Apple
     * enables Layer 5 via vtable[0x3C](ctx, 5, 1) during video start.
     * Without bit 7, the compositor has NO active layers → no render. */
    { uint32_t v = c[0x008/4]; v |= 0x80; c[0x008/4] = v; }
    /* FUN_000D8920(1): set bit 30 during init (ROM 0x14D408).
     * Apple sets this in compositor_init, NOT per-frame trigger. */
    { uint32_t v = c[0x008/4]; v |= 0x40000000; c[0x008/4] = v; }
    c[0x200/4] |= 0x10080;      /* TRIGCON: bits 16+7 */
    c[0x204/4] = 2;
    c[0x208/4] = 0;
    c[0x20C/4] = 2;
    c[0x210/4] = 0x00010110;    /* DMA dimensions */
    c[0x214/4] = 0x00EF013F;    /* 239<<16 | 319 */

    /* i80 bus timing — ROM FUN_0014D240 copies 5 values from DRAM 0x0890D2DC
     * to comp+0x1EC-0x1FC (via memcpy at 0x14D254, writer at 0x0D972C).
     * Source is a runtime struct populated by iBoot. Try reading it live;
     * fall back to captured values if DRAM was overwritten by Rockbox. */
    {
        volatile uint32_t *iboot_src = (volatile uint32_t *)0x0890D2DC;
        uint32_t t0 = iboot_src[0], t1 = iboot_src[1], t2 = iboot_src[2];
        uint32_t t3 = iboot_src[3], t4 = iboot_src[4];
        vlog("  iBoot DRAM 0x0890D2DC: %08lx %08lx %08lx %08lx %08lx",
             (unsigned long)t0, (unsigned long)t1, (unsigned long)t2,
             (unsigned long)t3, (unsigned long)t4);
        /* Use DRAM values if they look valid (non-zero, reasonable range).
         * iBoot values: {0x0C, 0x26, 0x10, 0x82, 0x4E}. All < 0x100. */
        if (t0 > 0 && t0 < 0x1000 && t4 > 0 && t4 < 0x1000) {
            c[0x1EC/4] = t0;
            c[0x1F0/4] = t1;
            c[0x1F4/4] = t2;
            c[0x1F8/4] = t3;
            c[0x1FC/4] = t4;
            vlog("  Using LIVE iBoot timing from DRAM");
        } else {
            c[0x1EC/4] = 0x0C;
            c[0x1F0/4] = 0x26;
            c[0x1F4/4] = 0x10;
            c[0x1F8/4] = 0x82;
            c[0x1FC/4] = 0x4E;
            vlog("  Using HARDCODED timing (DRAM overwritten)");
        }
    }
    /* comp+0x1E0/0x1E4/0x1E8: Apple init does NOT write these.
     * Previously wrote iBoot residuals (0x4A8, 0x812, 0) — removed
     * to match ROM. Leave at POR/iBoot defaults. */

    /* Set bit 30 LAST (master output enable) */
    c[0x008/4] |= 0x40000000;
    c[0x024/4] = 0x00FFFFFF;    /* Fix 2: color mask — ROM 0x14D410 MVN r1,#0xFF000000 */
    /* DO NOT fire GO here — must wait until LCD passthrough is active.
     * vpp_test.c v137 fires GO in Phase 6 AFTER passthrough, and that works. */
    /* comp+0x3AC: v49 did NOT have this and rendered full screen.
     * Apple writes 0x04004003 for landscape→portrait rotation with
     * MIPI DCS mode. With ILI9326/P18 GRAM window in landscape,
     * rotation causes portrait/landscape mismatch → partial rendering.
     * Leave at default (no rotation) for ILI9326 mode. */
}

/* ---- LCD Passthrough Init ---- */

static void lcd_passthrough_init(int panel_type, uint32_t *saved_con)
{
    *saved_con = LCD_CON;

    /* Wait for bus idle */
    { int t = 100000; while ((LCD_REG(0x8C) & 3) && --t > 0); }

    /* DISP register bulk-zero (ROM 0xC9FEC-0xCA03C). Apple zeros DISP+0x44-0xD0
     * (base 0x39300000, NOT LCD 0x38300000!) during display pipeline init.
     * ROM literal at 0xCA14C = 0x39300000 confirms base register. */
    for (int i = 0x44; i <= 0x6C; i += 4) DISP_REG(i) = 0;
    for (int i = 0x80; i <= 0x90; i += 4) DISP_REG(i) = 0;
    for (int i = 0xC0; i <= 0xD0; i += 4) DISP_REG(i) = 0;

    /* LCD controller config (ROM FUN_000ca178) */
    LCD_CON = 0x81100DB9;
    LCD_REG(0x88) = 0x01000000;
    LCD_REG(0x20) = 0x33;
    LCD_REG(0x7C) = 0x00000402;

    /* Passthrough setup (ROM FUN_0014deec) */
    LCD_REG(0x78) = 0x000A000A;

    LCD_REG(0x74) = 0x00F00140;

    /* GRAM window via ILI9326 16-bit registers through P18 mode.
     * Apple ROM uses MIPI DCS via P8/SPI mode, but that requires Apple's
     * panel init (PixoOS). Rockbox initialized the panel in P18/ILI9326
     * mode — panel only responds to P18 commands from this state.
     * v60 proved: P8/MIPI DCS produces NO output on Rockbox-init'd panel. */
    if (panel_type >= 2) {
        lcd_set_con(0x80000DA9);  /* P18 command mode (Rockbox LCD_MODE_P18+1) */
        lcd_cmd(0x210); lcd_data(0);
        lcd_cmd(0x211); lcd_data(319);
        lcd_cmd(0x212); lcd_data(0);
        lcd_cmd(0x213); lcd_data(239);
        lcd_cmd(0x200); lcd_data(0);
        lcd_cmd(0x201); lcd_data(0);
        lcd_cmd(0x202);
        lcd_set_con(0x81100DB9);  /* restore P9 passthrough */
    }

    /* Panel config (v49 had these — needed for compositor P9 pixel format) */
    lcd_set_con(0x80000DA9);
    lcd_cmd(0x3A); lcd_data(0x66);    /* COLMOD: DBI=18-bit for compositor pixels */
    lcd_cmd(0x003); lcd_data(0x1230); /* Entry Mode: BGR + increment */
    lcd_set_con(0x81100DB9);

    LCD_REG(0x70) = 1;          /* LCD MCU passthrough enable */
    /* LCD+0x80 = 0: release bus to compositor. Without this, compositor
     * can't drive the i80 bus. Old code zeroed LCD+0x80 via bulk-zero
     * (which targeted LCD before v34h). After v34h moved bulk-zero to DISP,
     * LCD+0x80 was never cleared → compositor could never push. */
    LCD_REG(0x80) = 0;
    /* DISP+0x70 = 0x281 — panel type 2 routing config.
     * ROM 0x0ca0dc: writes to DISP base 0x39300000 (literal at 0xCA14C),
     * NOT LCD base 0x38300000! All prior versions wrote 0x281 to LCD+0x70
     * instead of DISP+0x70 — WRONG register entirely. */
    DISP_REG(0x70) = 0x281;
    /* Panel type 2 gamma/timing regs — also DISP block, not LCD.
     * ROM 0x0ca0e0-0xca140, base r0 = 0x39300000 */
    DISP_REG(0x94) = 0x01;
    DISP_REG(0x98) = 0x07;
    DISP_REG(0x9C) = 0x15;
    DISP_REG(0xA0) = 0x2A;
    DISP_REG(0xA4) = 0x44;
    DISP_REG(0xA8) = 0x57;
    DISP_REG(0xAC) = 0x5F;
    DISP_REG(0xD4) = 0x02;
    DISP_REG(0xD8) = 0x0A;
    DISP_REG(0xDC) = 0x1D;
    DISP_REG(0xE0) = 0x3C;
    DISP_REG(0xE4) = 0x5F;
    DISP_REG(0xE8) = 0x7B;
    DISP_REG(0xEC) = 0x86;
}

/* ---- LCD Push (v134 minimal working pattern) ---- */

static void lcd_push_frame(int panel_type)
{
    /* Apple per-frame trigger FIRST (render compositor output) */
    CLCD_REG(0x000) |= 1;
    MIXER_REG(0x000) = 7;
    { uint32_t t = DISP_REG(0x03C); DISP_REG(0x03C) = t | 1; }
    { uint32_t t = DISP_REG(0x03C); DISP_REG(0x03C) = t | 2; }
    { uint32_t t = DISP_REG(0x03C); DISP_REG(0x03C) = t | 4; }
    for (volatile int d = 0; d < 5000; d++);

    /* Wait for bus idle */
    while (LCD_REG(0x8C) & 3);

    /* CPU takes bus (ROM 0xa5094) */
    LCD_REG(0x80) = 1;

    /* GRAM commands — match v35g EXACTLY: RAW LCD_CON switch, dummy pixel */
    LCD_CON = 0x80000DA9;  /* v35g cmd mode — RAW write, no poll */
    if (panel_type >= 2) {
        lcd_cmd(0x210); lcd_data(0);
        lcd_cmd(0x211); lcd_data(319);
        lcd_cmd(0x212); lcd_data(0);
        lcd_cmd(0x213); lcd_data(239);
        lcd_cmd(0x200); lcd_data(0);
        lcd_cmd(0x201); lcd_data(0);
        lcd_cmd(0x202);
        /* No dummy pixel — v35g relied on LCD+0x7C bit 1 for passthrough DC=1 */
    }
    /* Wait for command bus completion before releasing */
    while (!(LCD_STATUS & 0x2));
    LCD_CON = 0x81100DB9;  /* restore — RAW write, no poll */

    /* OOB-7: LCD+0x80=0 STARTS autonomous push. MCU controller pushes
     * full frame (~12ms for 76800px). Apple polls 0x8C with no timeout. */
    LCD_REG(0x80) = 0;
    while (LCD_REG(0x8C) & 3);
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
    rb->audio_stop();

    log_fd = rb->open("/vpu_vpp_test.log", O_WRONLY|O_CREAT|O_TRUNC, 0666);
    vlog("=== VPU-B → VPP Integration Test v62 ===");
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

    /* ---- Phase 2b: Test pattern selection ---- */
#if 1 /* SOLID COLOR test — verify compositor reads uniform data correctly */
    vlog("Phase 2b: SOLID Y=128 Cb=128 Cr=128 (should be uniform gray)");
    {
        uint8_t *y_unc = (uint8_t *)((uintptr_t)y_out | 0x40000000);
        uint8_t *cb_unc = (uint8_t *)((uintptr_t)cb_out | 0x40000000);
        uint8_t *cr_unc = (uint8_t *)((uintptr_t)cr_out | 0x40000000);
        for (int i = 0; i < frame_w * frame_h; i++)
            y_unc[i] = 128;
        for (int i = 0; i < (frame_w/2)*(frame_h/2); i++) {
            cb_unc[i] = 128;
            cr_unc[i] = 128;
        }
        rb->commit_discard_dcache();
        uint8_t *y_buf = (uint8_t *)y_out;
        vlog("  Y[0]=%d Y[mid]=%d Y[last]=%d", y_buf[0], y_buf[120*frame_w], y_buf[frame_w*frame_h-1]);
    }
#else
    vlog("Phase 2b: Using ACTUAL decoded frame (not white override)");
    {
        uint8_t *y_buf = (uint8_t *)y_out;
        uint8_t *cb_buf = (uint8_t *)cb_out;
        vlog("  Y[0,0]=%d Y[0,160]=%d Y[120*320]=%d Cb[0]=%d",
             y_buf[0], y_buf[160], y_buf[120*frame_w], cb_buf[0]);
        rb->commit_discard_dcache();
    }
#endif /* gradient vs decoded frame */

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
     * v34l: DISABLED — Finding 3 confirmed Apple NEVER writes PCON(7).
     * Zero references to 0x3CF000E0 in entire retailos.bin.
     * VPP DMA is internal AHB bus master, doesn't need GPIO pins.
     * v34 log showed PCON(7) write gave 44444414 not 444444F4 — HW rejected 0xF. */
#if 0
    {
        uint32_t old = PCON(7);
        PCON(7) = (old & ~0xF0) | 0xF0;  /* pin 1 nibble = 0xF, preserve others */
        vlog("PCON(7): before=%08lx after=%08lx (expect nibble 1 = F)",
             (unsigned long)old, (unsigned long)PCON(7));
    }
#endif
    vlog("PCON(7) = %08lx (untouched — Apple never writes PCON(7))",
         (unsigned long)PCON(7));

    /* ---- Phase 3: VPP pipeline init ---- */

    vlog("Phase 3: VPP pipeline init");
    rb->backlight_on();

    /* Fix 4: iBoot bandwidth enable — Apple calls thunk_EXT_FUN_220043e8(1, 0x40)
     * as step 1 of VPP init. The SRAM function sets bit 6 in a power management
     * struct processed by a bytecode VM. Best guess: PWRCON(0) bit 6 (Clk_UNK).
     * Rockbox system_init gates this bit, we never ungate it. */
    PWRCON(0) &= ~(1 << 6);
    vlog("  PWRCON(0) bit 6 ungated: %08lx", (unsigned long)PWRCON(0));

    /* Fix 3: GPIOCMD IRQ 57 routing — Apple step 5: FUN_0036d3f0(0x39, 1, 1)
     * writes 0x0007010F to GPIOCMD for VPP completion interrupt. */
    *(volatile uint32_t *)0x3CF00200 = 0x0007010F;

    /* Enable VPP clocks (steps 2-4 of Apple's sequence) */
    vpp_svid_enable(true);
    vpp_clocks_enable(true);
    for (volatile int d = 0; d < 10000; d++);

    /* v34e: 0x3C700000 clock domain dump — never touched by any version.
     * 8 domains at stride 0x20. If a domain is enabled: +00 has 0x40.
     * If all zero: SRAM clock manager never ran. */
    vlog("Phase 3 clock domains (0x3C700000):");
    vlog("  CG16=%08lx PWRCON0=%08lx DEVRT=%08lx",
         (unsigned long)(*(volatile uint32_t *)0x3C500008),
         (unsigned long)PWRCON(0),
         (unsigned long)(*(volatile uint32_t *)0x3CF00200));
    for (int dom = 0; dom < 8; dom++) {
        volatile uint32_t *dr = (volatile uint32_t *)(0x3C700000 + dom * 0x20);
        vlog("  D%d: %08lx %08lx %08lx %08lx", dom,
             (unsigned long)dr[0], (unsigned long)dr[1],
             (unsigned long)dr[2], (unsigned long)dr[4]);
    }
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
    /* DISP pre-init dump (iBoot defaults for 0x3Cx range) */
    vlog("  DISP: 008=%08lx 014=%08lx 070=%08lx 3C0=%08lx",
         (unsigned long)DISP_REG(0x008), (unsigned long)DISP_REG(0x014),
         (unsigned long)DISP_REG(0x070), (unsigned long)DISP_REG(0x3C0));
    vlog("  DISP: 3C4=%08lx 3C8=%08lx 3CC=%08lx 3D0=%08lx 3D4=%08lx",
         (unsigned long)DISP_REG(0x3C4), (unsigned long)DISP_REG(0x3C8),
         (unsigned long)DISP_REG(0x3CC), (unsigned long)DISP_REG(0x3D0),
         (unsigned long)DISP_REG(0x3D4));

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
    vlog("  comp pre-init: 008=%08lx 028=%08lx 034=%08lx 038=%08lx",
         (unsigned long)COMP_REG(0x008), (unsigned long)COMP_REG(0x028),
         (unsigned long)COMP_REG(0x034), (unsigned long)COMP_REG(0x038));
    compositor_init();

    /* Layer 5 config (ROM FUN_0014CC90 case 5, format 8) */
    COMP_REG(0x028) = 0x100;
    COMP_REG(0x02C) = frame_w | ((frame_w / 2) << 16);
    COMP_REG(0x030) = 0;
    COMP_REG(0x034) = frame_h | ((uint32_t)frame_w << 16);
    COMP_REG(0x04C) = 0x10001000;
    COMP_REG(0x050) = 0;
    COMP_REG(0x054) = out_h | ((uint32_t)out_w << 16);

    /* v47: Initialize scaler coefficient tables.
     * comp+0x0F0-0x17C (9x4=36 dwords): vertical 4-tap polyphase filter
     * comp+0x180-0x1C4 (9x2=18 dwords): horizontal 2-tap
     * comp+0x31C-0x360 (9x2=18 dwords): scaler table 2
     * comp+0x364-0x3A8 (9x2=18 dwords): scaler table 3
     * Apple loads these from DRAM 0x08A18700 (via FUN_0014D678).
     * Rockbox likely overwrote that DRAM region. Without valid
     * coefficients, YUV420 chroma upsampling produces garbage.
     * Use identity filter: phase 0 = pass-through [0,0,256,0],
     * all other phases = linear interpolation. */
    {
        volatile uint32_t *c = (volatile uint32_t *)COMP_BASE;
        /* Dump current scaler coefficients — iBoot values */
        vlog("  comp scaler iBoot (0x0F0-0x10C, first 8):");
        vlog("    %08lx %08lx %08lx %08lx",
             (unsigned long)c[0x0F0/4], (unsigned long)c[0x0F4/4],
             (unsigned long)c[0x0F8/4], (unsigned long)c[0x0FC/4]);
        vlog("    %08lx %08lx %08lx %08lx",
             (unsigned long)c[0x100/4], (unsigned long)c[0x104/4],
             (unsigned long)c[0x108/4], (unsigned long)c[0x10C/4]);
        /* Also dump 0x180 and 0x31C iBoot values */
        vlog("  comp 0x180 iBoot: %08lx %08lx %08lx %08lx",
             (unsigned long)c[0x180/4], (unsigned long)c[0x184/4],
             (unsigned long)c[0x188/4], (unsigned long)c[0x18C/4]);
        vlog("  comp 0x31C iBoot: %08lx %08lx %08lx %08lx",
             (unsigned long)c[0x31C/4], (unsigned long)c[0x320/4],
             (unsigned long)c[0x324/4], (unsigned long)c[0x328/4]);
        /* v50: Do NOT overwrite scaler coefficients. iBoot initialized
         * them for boot logo display. v49 overwrites caused a red band
         * artifact. Leave iBoot defaults intact. */
    }

    /* v46: Write buffer addresses BEFORE compositor GO.
     * v45 wrote them in Phase 4 AFTER GO — compositor rendered
     * iBoot's boot logo from stale addresses instead of our gradient.
     * ROM 0x14D794: format 8 order: struct[0]→Y, struct[2]→03C, struct[1]→044. */
    COMP_REG(0x038) = PHYS(y_out);                  /* Layer 5 Y */
    COMP_REG(0x03C) = PHYS(cr_out);                 /* Cr (ROM: struct[2]→0x03C) */
    COMP_REG(0x040) = 0;                            /* unused for 3-plane */
    COMP_REG(0x044) = PHYS(cb_out);                 /* Cb (ROM: struct[1]→0x044) */

    vlog("  Compositor initialized + Layer 5 enabled");
    vlog("  iBoot timing: %08lx %08lx %08lx %08lx %08lx",
         (unsigned long)iboot_timing[0], (unsigned long)iboot_timing[1],
         (unsigned long)iboot_timing[2], (unsigned long)iboot_timing[3],
         (unsigned long)iboot_timing[4]);
    {
        volatile uint32_t *ct = (volatile uint32_t *)COMP_BASE;
        vlog("  POST-WRITE timing: %08lx %08lx %08lx %08lx %08lx",
             (unsigned long)ct[0x1EC/4], (unsigned long)ct[0x1F0/4],
             (unsigned long)ct[0x1F4/4], (unsigned long)ct[0x1F8/4],
             (unsigned long)ct[0x1FC/4]);
    }

    /* Wait for compositor to settle — reduced from 200ms */
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 50000) rb->backlight_on(); }

    /* Save LCD registers BEFORE modifying (for shutdown restore) */
    uint32_t saved_lcd_7c = LCD_REG(0x7C);
    uint32_t saved_lcd_88 = LCD_REG(0x88);
    uint32_t saved_lcd_20 = LCD_REG(0x20);
    uint32_t saved_lcd_74 = LCD_REG(0x74);
    uint32_t saved_lcd_78 = LCD_REG(0x78);

    /* LCD passthrough */
    lcd_passthrough_init(panel_type, &saved_lcd_con);
    vlog("  LCD passthrough initialized");

    /* DISP GO */
    disp_go();
    vlog("  DISP GO fired");

    /* Wait for DISP to generate at least one internal frame cycle.
     * At 30fps, one frame = 33ms. Wait 100ms to be safe.
     * Without this, compositor GO might fire before DISP VSYNC is ready. */
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 50000) rb->backlight_on(); }

    /* Fire initial VP/MIXER/DISP trigger (v49 had this, v58 removed it) */
    CLCD_REG(0x000) |= 1;
    MIXER_REG(0x000) = 7;
    { uint32_t t = DISP_REG(0x03C); DISP_REG(0x03C) = t | 1; }
    { uint32_t t = DISP_REG(0x03C); DISP_REG(0x03C) = t | 2; }
    { uint32_t t = DISP_REG(0x03C); DISP_REG(0x03C) = t | 4; }
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 50000) rb->backlight_on(); }

    /* Compositor GO — MUST be AFTER passthrough (LCD+0x70=1).
     * vpp_test.c v137 proved: GO before passthrough = no output.
     * GO after passthrough = BG_COLOR visible. */
    /* ROM init: bit 7 at step 14, then steps 15-17, then GO at step 18.
     * Steps 15-17 provide ~10 register writes of timing gap between bit 7 and GO.
     * Our earlier back-to-back bit7+GO may not give HW enough time. */
    /* Clear compositor status/interrupts before GO — pending flags
     * might prevent render engine from starting */
    COMP_REG(0x010) = 0x003FEFFE;  /* write-to-clear all pending bits */
    COMP_REG(0x200) |= 0x81;   /* bit 7 + bit 0 (SWTRGCMD) — v49 had this */
    COMP_REG(0x204) = 2;      /* step 15: re-assert DMA config */
    COMP_REG(0x20C) = 2;
    COMP_REG(0x208) = 0;
    COMP_REG(0x008) |= 0x40000000;  /* step 16: re-assert bit 30 */
    COMP_REG(0x024) = 0x00FFFFFF;   /* step 17: re-assert color mask */
    COMP_REG(0x000) = 1;      /* step 18: GO! */
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 200000); }
    /* Re-fire i80 strobe (from vpp_test.c v137 working pattern) */
    COMP_REG(0x200) |= 0x80;
    for (volatile int d = 0; d < 10000; d++);
    vlog("  Compositor GO + i80 re-strobe fired");

#if 0 /* Disable BG-only test — modifies comp+0x008 and fires extra GO */
     * compositor produces clean solid color without any layer data.
     * If BG is clean → compositor output works, issue is Layer 5.
     * If BG has bands → compositor output stage is broken. */
    {
        uint32_t saved_008 = COMP_REG(0x008);
        COMP_REG(0x008) = saved_008 & ~0x80;  /* clear bit 7 = disable Layer 5 */
        COMP_REG(0x00C) = 0x00FF0000;          /* BG = bright RED */
        COMP_REG(0x200) |= 0x80; /* no SWTRGCMD — compositor is free-running */
        vlog("  BG-ONLY test: comp008=%08lx (Layer 5 OFF, BG=RED)",
             (unsigned long)COMP_REG(0x008));
        /* Push to LCD */
        LCD_REG(0x80) = 0;
        { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 5000000) rb->backlight_on(); }
        /* Re-enable Layer 5 */
        COMP_REG(0x008) = saved_008;
        COMP_REG(0x00C) = 0x000F0F0F;  /* restore BG */
        vlog("  Layer 5 re-enabled");
    }
    vlog("  comp: 000=%08lx 010=%08lx 200=%08lx LCD70=%08lx DISP70=%08lx",
         (unsigned long)COMP_REG(0x000), (unsigned long)COMP_REG(0x010),
         (unsigned long)COMP_REG(0x200), (unsigned long)LCD_REG(0x70),
         (unsigned long)DISP_REG(0x70));
#endif /* BG-only test disabled */

    /* Diagnostics: verify register state after init */
    vlog("  DISP: 000=%08lx 008=%08lx",
         (unsigned long)DISP_REG(0x000), (unsigned long)DISP_REG(0x008));
    vlog("  MXR_004=%08lx comp008=%08lx comp00C=%08lx",
         (unsigned long)MIXER_REG(0x004),
         (unsigned long)COMP_REG(0x008), (unsigned long)COMP_REG(0x00C));
    vlog("  comp: 014=%08lx 018=%08lx 01C=%08lx 020=%08lx",
         (unsigned long)COMP_REG(0x014), (unsigned long)COMP_REG(0x018),
         (unsigned long)COMP_REG(0x01C), (unsigned long)COMP_REG(0x020));
    vlog("  LCD_CON=%08lx LCD70=%08lx DISP70=%08lx +0x7C=%08lx +0x88=%08lx",
         (unsigned long)LCD_CON, (unsigned long)LCD_REG(0x70),
         (unsigned long)DISP_REG(0x70),
         (unsigned long)LCD_REG(0x7C), (unsigned long)LCD_REG(0x88));

    /* ---- Phase 4: Feed decoded frame to CLCD ---- */

    vlog("Phase 4: Feed frame to CLCD");

    /* v34: Format 8 = 3-plane planar YUV420 (ROM-verified at dispatch 0x167690).
     * v31 had format 9 (2-plane) which was wrong — format 8 is Apple's H.264 format.
     * Compositor only enables Layer 5 when format==8 (ROM 0x14ce5c).
     *
     * v35g: VP must be DISABLED when writing plane addresses to bypass the
     * shadow register mechanism. On i80 panels, VSYNC never occurs so
     * VP+0x008 shadow commit never triggers. With VP disabled, register
     * writes go directly to active registers (no shadow). */
    /* v35h: ROM-verified VP register setup.
     * Apple FUN_00167288 NEVER writes VP+0x010 or VP+0x014 — these are
     * iBoot values preserved across RetailOS lifecycle. ROM disasm confirms:
     * init writes 0x004,0x008,0x00C=0, then 0x028-0x068, 0x200, 0x3C0-0x3CC.
     * VP+0x010=0x200 and VP+0x014=0x70 are iBoot POR values NEVER changed.
     * Per-frame geometry uses VP+0x03C-0x060 (ROM 0x166B40-0x166BA8). */
    CLCD_REG(0x000) = CLCD_REG(0x000) & 2;  /* disable VP, preserve bit 1 (ROM exact) */
    for (volatile int d = 0; d < 1000; d++);

    /* ROM-exact init: 0x004,0x008,0x00C = 0 (no reset pulse, no shadow commit) */
    CLCD_REG(0x004) = 0;
    CLCD_REG(0x008) = 0;
    CLCD_REG(0x00C) = 0;
    /* Write VP+0x010/0x014/0x018 — iBoot values vary between boots */
    CLCD_REG(0x010) = 0;  /* progressive mode (v35g value) */
    CLCD_REG(0x014) = 0x70;   /* DMA threshold (iBoot common value, NOT frame size) */
    CLCD_REG(0x018) = 0;      /* chroma threshold/config */

    /* Plane addresses (ROM format 8 = 3-plane planar) */
    CLCD_REG(0x028) = PHYS(y_out);                  /* Y  plane */
    CLCD_REG(0x02C) = PHYS(cb_out);                 /* Cb plane (standard) */
    CLCD_REG(0x030) = PHYS(cr_out);                 /* Cr plane (standard) */
    CLCD_REG(0x034) = 0;
    CLCD_REG(0x038) = 0;

    /* Per-frame geometry (ROM 0x166B40-0x166BA8 writes these) */
    CLCD_REG(0x03C) = frame_w;
    CLCD_REG(0x040) = frame_h;
    CLCD_REG(0x044) = 0;  /* pan offset = 0 (agent: NOT source width!) */  /* src_width << 4 (fractional, ROM 0x166B3C) */
    CLCD_REG(0x048) = 0;             /* VP_SRC_V_POSITION = 0 (ROM 0x166B48) */
    CLCD_REG(0x04C) = frame_w;       /* out_w */
    CLCD_REG(0x050) = frame_h;       /* out_h */
    CLCD_REG(0x054) = 0;             /* pos_x */
    CLCD_REG(0x058) = 0;             /* pos_y */
    CLCD_REG(0x05C) = frame_w;       /* src_display_w (ROM 0x166B70) */
    CLCD_REG(0x060) = frame_h;       /* src_display_h (ROM 0x166B78) */
    CLCD_REG(0x064) = ((frame_w << 12) / frame_w) >> 3;  /* h_ratio (ROM 0x166B84-0x166B90) */
    CLCD_REG(0x068) = ((frame_h << 12) / frame_h) >> 4;  /* v_ratio (ROM 0x166B9C-0x166BA8) */
    CLCD_REG(0x3C4) = frame_w;                      /* luma stride */
    CLCD_REG(0x3C8) = frame_w / 2;                  /* chroma stride */
    CLCD_REG(0x3C0) = 1;                            /* planar mode */
    CLCD_REG(0x3CC) = 1;                            /* YUV enable */

    /* Diagnostics: verify iBoot values preserved */
    vlog("  VP+0x010=%08lx (iBoot POR, expect 0x200)",
         (unsigned long)CLCD_REG(0x010));
    vlog("  VP+0x014=%08lx (iBoot, expect 0x70 — Apple never changes)",
         (unsigned long)CLCD_REG(0x014));
    vlog("  VP+0x000=%08lx +0x004=%08lx +0x008=%08lx",
         (unsigned long)CLCD_REG(0x000), (unsigned long)CLCD_REG(0x004),
         (unsigned long)CLCD_REG(0x008));

    /* Buffer addresses already set in Phase 3 (before compositor GO).
     * Re-assert here for per-frame update (ROM writes these per-frame). */
    COMP_REG(0x038) = PHYS(y_out);
    COMP_REG(0x03C) = PHYS(cr_out);
    COMP_REG(0x040) = 0;
    COMP_REG(0x044) = PHYS(cb_out);

    /* Re-enable VP — all registers written with VP disabled (ROM pattern) */
    CLCD_REG(0x000) |= 1;

    /* Track VP DMA status: bit 2 is read-only HW status (auto-sets when DMA active).
     * 0x01 = VP enabled, DMA not started. 0x05 = VP enabled, DMA running. */
    vlog("  VP+0x000 after enable: %08lx (0x05=DMA running, 0x01=DMA idle)",
         (unsigned long)CLCD_REG(0x000));
    vlog("  VP diag: +0x008=%08lx +0x010=%08lx +0x3C0=%08lx",
         (unsigned long)CLCD_REG(0x008), (unsigned long)CLCD_REG(0x010),
         (unsigned long)CLCD_REG(0x3C0));

    /* v50: Verify DRAM at the EXACT physical address the compositor reads */
    {
        uint32_t comp_y_addr = COMP_REG(0x038);
        uint32_t comp_cb_addr = COMP_REG(0x044);
        uint32_t comp_cr_addr = COMP_REG(0x03C);
        volatile uint8_t *dram_y = (volatile uint8_t *)(comp_y_addr | 0x40000000);
        volatile uint8_t *dram_cb = (volatile uint8_t *)(comp_cb_addr | 0x40000000);
        volatile uint8_t *dram_cr = (volatile uint8_t *)(comp_cr_addr | 0x40000000);
        vlog("  DRAM@comp038(0x%08lx): [0]=%d [160]=%d [mid]=%d [last]=%d",
             (unsigned long)comp_y_addr,
             dram_y[0], dram_y[160],
             dram_y[120*320 + 160], dram_y[320*240 - 1]);
        vlog("  DRAM@comp044(Cb=0x%08lx): [0]=%d [1]=%d [2]=%d [3]=%d",
             (unsigned long)comp_cb_addr,
             dram_cb[0], dram_cb[1], dram_cb[2], dram_cb[3]);
        vlog("  DRAM@comp03C(Cr=0x%08lx): [0]=%d [1]=%d [2]=%d [3]=%d",
             (unsigned long)comp_cr_addr,
             dram_cr[0], dram_cr[1], dram_cr[2], dram_cr[3]);
    }

    /* MIXER+0x004: D7 trace gives 0x03 (bits 0+1). DS1 found bit 3 = REG_VIDEO_EN
     * (ROM 0x166d24 layer enable dispatcher, S5PC100 p.1461). Without bit 3, mixer
     * DISCARDS all video data. 0x0B = bits 0+1+3. */
    MIXER_REG(0x004) = 0x13;   /* ROM-verified: bits 0+1+4.
                                 * Bit 0 = MXR_EN, bit 1 = SYNC_EN, bit 4 = video
                                 * layer 5 enable (ROM 0x166D60: case 5 → ORR #0x10).
                                 * NOT bit 5 — ROM jump table is layer-inverted. */
    MIXER_REG(0x008) = 0;      /* v43: Apple init sets 0, never changes. v42 had
                                 * 0x100FF from unverified 'DEEP-1' finding. */

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

    /* Per-frame dimension update removed — Phase 4 now writes ALL VP regs */

    /* Full CLCD register dump for comparison with Apple's ROM */
    vlog("Phase 4b: CLCD register dump");
    for (int off = 0; off <= 0x068; off += 4) {
        vlog("  CLCD+0x%03x=%08lx", off, (unsigned long)CLCD_REG(off));
    }
    vlog("  CLCD+0x200=%08lx +0x3C0=%08lx +0x3C4=%08lx +0x3C8=%08lx +0x3CC=%08lx",
         (unsigned long)CLCD_REG(0x200), (unsigned long)CLCD_REG(0x3C0),
         (unsigned long)CLCD_REG(0x3C4), (unsigned long)CLCD_REG(0x3C8),
         (unsigned long)CLCD_REG(0x3CC));

    /* v43: Verify gradient buffer contents before pipeline reads them */
    {
        const uint8_t *yb = (const uint8_t *)y_out;
        const uint8_t *cbb = (const uint8_t *)cb_out;
        const uint8_t *crb = (const uint8_t *)cr_out;
        vlog("Phase 4c: Buffer verify Y[0]=%d Y[120*%d]=%d Y[last]=%d Cb[0]=%d Cr[0]=%d",
             yb[0], frame_w, yb[120*frame_w], yb[(frame_h-1)*frame_w], cbb[0], crb[0]);
        /* Also verify via uncached alias to confirm DRAM content */
        const uint8_t *yu = (const uint8_t *)((uintptr_t)y_out | 0x40000000);
        vlog("  Uncached: Y[0]=%d Y[mid]=%d Y[last]=%d",
             yu[0], yu[120*frame_w], yu[(frame_h-1)*frame_w]);
        vlog("Phase 4c: Buffer verify Y[0]=%d Y[120*%d]=%d Y[last]=%d Cb[0]=%d Cr[0]=%d",
             yb[0], frame_w, yb[120*frame_w], yb[(frame_h-1)*frame_w], cbb[0], crb[0]);
        vlog("  Y phys=%08lx Cb phys=%08lx Cr phys=%08lx",
             (unsigned long)PHYS(y_out), (unsigned long)PHYS(cb_out),
             (unsigned long)PHYS(cr_out));
    }

    /* ---- Phase 5: Trigger pipeline ---- */

    vlog("Phase 5: Pipeline trigger");

    /* v35h: ROM-exact per-frame trigger (0x166C28-0x166C64).
     * VP is already enabled from Phase 4. Apple's trigger re-asserts VP|=1. */
    CLCD_REG(0x000) |= 1;              /* VP enable (ROM 0x166C2C-0x166C30) */
    MIXER_REG(0x000) = 7;              /* MIXER GO (ROM 0x166C38-0x166C3C) */
    { uint32_t t = DISP_REG(0x03C); DISP_REG(0x03C) = t | 1; }  /* ROM 0x166C44-0x166C4C */
    { uint32_t t = DISP_REG(0x03C); DISP_REG(0x03C) = t | 2; }  /* ROM 0x166C50-0x166C58 */
    { uint32_t t = DISP_REG(0x03C); DISP_REG(0x03C) = t | 4; }  /* ROM 0x166C5C-0x166C64 */

    /* Note: MIXER+0x008 bit 16 (video layer alpha) already set in Phase 4.
     * CLCD+0x008 (VP_SHADOW_UPDATE) = 0 per Apple ROM — no shadow commit needed. */

    vlog("  Trigger fired");
    vlog("  CLCD_CTRL: %08lx", (unsigned long)CLCD_REG(0x000));

    /* v35h: Apple NEVER writes VP+0x008 = 1. ROM FUN_00167288 writes
     * VP+0x008 = 0 in init (0x1672A0) and NEVER touches it again.
     * Our previous VP+0x008 = 1 was wrong Samsung assumption.
     * Just read for diagnostics. */
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 50000); }
    vlog("  VP+008 after 50ms: %08lx (Apple always 0)",
         (unsigned long)CLCD_REG(0x008));
    vlog("  CLCD_CTRL after 50ms: %08lx (expect 0x05 if DMA started)",
         (unsigned long)CLCD_REG(0x000));

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

#if 0 /* Disable Phase 5c — LCD_CON switching corrupts compositor stream */
    /* Phase 5b: Passive observation — compositor should push automatically
     * via passthrough. No lcd_push_frame, no LCD+0x80 toggle.
     * Apple's per-frame trigger never touches LCD+0x80.
     * If screen shows color here → compositor pushes autonomously.
     * If still blue → compositor needs explicit push trigger. */
    vlog("Phase 5b: Passive 3s (no push, compositor auto)");
    vlog("  comp+0x200=%08lx LCD_8C=%08lx",
         (unsigned long)COMP_REG(0x200), (unsigned long)LCD_REG(0x8C));
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 3000000) rb->backlight_on(); }
    vlog("  After 3s: comp+0x200=%08lx LCD_8C=%08lx CLCD=%08lx",
         (unsigned long)COMP_REG(0x200), (unsigned long)LCD_REG(0x8C),
         (unsigned long)CLCD_REG(0x000));

    /* Phase 5b2: Strobe test — fire compositor strobe and immediately
     * sample LCD+0x8C to detect bus activity. If compositor pushes,
     * LCD_8C should show non-zero (bus busy) within microseconds. */
    vlog("Phase 5b2: Strobe bus-activity test");
    {
        uint32_t pre_8c = LCD_REG(0x8C);
        uint32_t pre_200 = COMP_REG(0x200);
        COMP_REG(0x200) |= 0x80;  /* strobe */
        uint32_t post_200 = COMP_REG(0x200);
        /* Sample LCD_8C rapidly 10 times over ~10µs */
        uint32_t samples[10];
        for (int i = 0; i < 10; i++) {
            samples[i] = LCD_REG(0x8C);
            for (volatile int d = 0; d < 100; d++);
        }
        vlog("  pre: 8C=%08lx 200=%08lx  post: 200=%08lx",
             (unsigned long)pre_8c, (unsigned long)pre_200,
             (unsigned long)post_200);
        vlog("  8C samples: %08lx %08lx %08lx %08lx %08lx",
             (unsigned long)samples[0], (unsigned long)samples[1],
             (unsigned long)samples[2], (unsigned long)samples[3],
             (unsigned long)samples[4]);
        vlog("  8C samples: %08lx %08lx %08lx %08lx %08lx",
             (unsigned long)samples[5], (unsigned long)samples[6],
             (unsigned long)samples[7], (unsigned long)samples[8],
             (unsigned long)samples[9]);
    }

    /* Phase 5c: Layer 0 RGB test — fill buffer with GREEN RGB565,
     * use compositor Layer 0 (single-buffer RGB) instead of Layer 5 (YUV) */
    vlog("Phase 5c: Color channel test (layers ON, varying BG)");
    /* Keep Layer 5 (bit 7) enabled — compositor needs active layer to render.
     * BG_COLOR only fills gaps; with no layers, output = default 0x03FF. */
    COMP_REG(0x028) = 0x100;  /* format 8 = YUV420 (hardwired CSC?) */

    static const struct { uint32_t bg; const char *name; } ctests[] = {
        {0x000000FF, "R=0xFF"},
        {0x0000FF00, "G=0xFF"},
        {0x00FF0000, "B=0xFF"},
        {0x00FFFFFF, "WHITE"},
    };
    for (int ci = 0; ci < 4; ci++) {
        COMP_REG(0x00C) = ctests[ci].bg;
        /* Full trigger → render → push → readback */
        CLCD_REG(0x000) |= 1; MIXER_REG(0x000) = 7;
        { uint32_t t = DISP_REG(0x03C); DISP_REG(0x03C) = t | 1; }
        { uint32_t t = DISP_REG(0x03C); DISP_REG(0x03C) = t | 2; }
        { uint32_t t = DISP_REG(0x03C); DISP_REG(0x03C) = t | 4; }
        /* compositor is free-running — no per-frame GO needed */
        { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 200000); }
        /* Push — clear bit 20 during GRAM commands (with STATUS poll) */
        { int t = 100000; while ((LCD_REG(0x8C) & 3) && --t > 0); }
        LCD_REG(0x80) = 1;
        LCD_CON = 0x80000DA9;  /* RAW write — v35g exact */
        if (panel_type >= 2) {
            lcd_cmd(0x210); lcd_data(0); lcd_cmd(0x211); lcd_data(319);
            lcd_cmd(0x212); lcd_data(0); lcd_cmd(0x213); lcd_data(239);
            lcd_cmd(0x200); lcd_data(0); lcd_cmd(0x201); lcd_data(0);
            lcd_cmd(0x202); lcd_data(0x0000);  /* dummy pixel — v35g had this */
        }
        while (!(LCD_STATUS & 0x2));
        LCD_CON = 0x81100DB9;  /* RAW restore — v35g exact */
        LCD_REG(0x80) = 0;
        { int t = 100000; while ((LCD_REG(0x8C) & 3) && --t > 0); }
        /* Readback at (160,120) — disable passthrough for CPU GRAM access */
        LCD_REG(0x70) = 0;
        LCD_REG(0x80) = 1;
        /* Switch to P18 command mode with proper status wait */
        while (!(LCD_STATUS & 0x2));
        for (volatile int _d = 0; _d < 100; _d++);
        LCD_CON = 0x80000DA8;  /* P18 mode (Rockbox LCD_MODE_P18) */
        while (!(LCD_STATUS & 0x2));
        lcd_cmd(0x200); lcd_data(160); lcd_cmd(0x201); lcd_data(120); lcd_cmd(0x202);
        while (!(LCD_STATUS & 0x2));
        /* ILI9326 18-bit read: 1 dummy cycle, then pixel data */
        LCD_RDATA = 0; { int t = 100000; while (!(LCD_STATUS & 1) && --t > 0); } (void)LCD_DBUFF;
        LCD_RDATA = 0; { int t = 100000; while (!(LCD_STATUS & 1) && --t > 0); }
        uint32_t g = LCD_DBUFF;
        uint32_t g_shifted = g >> 1;
        uint32_t r6 = (g>>12)&0x3F, g6 = (g>>6)&0x3F, b6 = g&0x3F;
        uint32_t r6s = (g_shifted>>12)&0x3F, g6s = (g_shifted>>6)&0x3F, b6s = g_shifted&0x3F;
        vlog("  BG=%s: GRAM=%08lx R=%lu G=%lu B=%lu (>>1: R=%lu G=%lu B=%lu)",
             ctests[ci].name, (unsigned long)g,
             (unsigned long)r6, (unsigned long)g6, (unsigned long)b6,
             (unsigned long)r6s, (unsigned long)g6s, (unsigned long)b6s);
        LCD_CON = 0x81100DB9; LCD_REG(0x80) = 0;
        LCD_REG(0x70) = 1; DISP_REG(0x70) = 0x281;
    }

    /* Apple per-frame trigger FIRST (ROM order: trigger → push) */
    CLCD_REG(0x000) |= 1;
    MIXER_REG(0x000) = 7;
    { uint32_t t = DISP_REG(0x03C); DISP_REG(0x03C) = t | 1; }
    { uint32_t t = DISP_REG(0x03C); DISP_REG(0x03C) = t | 2; }
    { uint32_t t = DISP_REG(0x03C); DISP_REG(0x03C) = t | 4; }
    /* Wait for compositor to render after trigger */
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 100000); }
    /* THEN LCD push — keep LCD_CON at 0x81100DB9 */
    { int t = 100000; while ((LCD_REG(0x8C) & 3) && --t > 0); }
    LCD_REG(0x80) = 1;
    if (panel_type >= 2) {
        lcd_cmd(0x210); lcd_data(0);
        lcd_cmd(0x211); lcd_data(319);
        lcd_cmd(0x212); lcd_data(0);
        lcd_cmd(0x213); lcd_data(239);
        lcd_cmd(0x200); lcd_data(0);
        lcd_cmd(0x201); lcd_data(0);
        lcd_cmd(0x202);
    }
    while (!(LCD_STATUS & 0x2));
    LCD_REG(0x80) = 0;  /* compositor pushes on 1→0 transition */
    vlog("  LCD_8C=%08lx (nonzero=bus busy=compositor pushing)",
         (unsigned long)LCD_REG(0x8C));
    /* Wait for push to finish, then check GRAM at (0,0) */
    { int t = 500000; while ((LCD_REG(0x8C) & 3) && --t > 0);
      vlog("  bus idle after %s", t > 0 ? "transfer" : "timeout"); }
    /* Quick GRAM spot check: read pixel at (0,0) */
    LCD_REG(0x70) = 0;  /* passthrough off for read */
    LCD_REG(0x80) = 1;
    LCD_CON = 0x80000DA8;  /* P18 read mode (Rockbox LCD_MODE_P18) */  /* read mode: bit 0=0 for read direction */
    lcd_cmd(0x200); lcd_data(0);
    lcd_cmd(0x201); lcd_data(0);
    lcd_cmd(0x202);
    { int t = 100000; while (!(LCD_STATUS & 0x2) && --t > 0); }
    LCD_RDATA = 0; { int t = 100000; while (!(LCD_STATUS & 1) && --t > 0); } (void)LCD_DBUFF;
    LCD_RDATA = 0; { int t = 100000; while (!(LCD_STATUS & 1) && --t > 0); }
    vlog("  GRAM(0,0) after comp push: %08lx", (unsigned long)LCD_DBUFF);
    LCD_CON = 0x81100DB9;
    LCD_REG(0x80) = 0;
    LCD_REG(0x70) = 1;
    DISP_REG(0x70) = 0x281;
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 3000000) rb->backlight_on(); }
    vlog("  After 3s: LCD_8C=%08lx comp+0x200=%08lx",
         (unsigned long)LCD_REG(0x8C), (unsigned long)COMP_REG(0x200));
#endif /* skip diagnostic phases */

    /* ---- Phase 6: Per-frame push loop (K1+K2: gate cycling required) ---- */

    vlog("Phase 6: Push loop (Apple pattern: LCD+0x80 toggle, NO strobe)");

    for (int push = 0; push < 10; push++) {
        uint32_t t0 = USEC_TIMER;
        /* ROM FUN_00086754 push pattern (agent-verified):
         * 1. Wait DMA idle (LCD+0x8C & 3 == 0)
         * 2. LCD+0x80 = 1 (hold DMA)
         * 3. Switch LCD_CON to SPI mode, send MIPI DCS window cmds
         * 4. Restore LCD_CON
         * 5. LCD+0x80 = 0 (release → compositor pushes)
         * 6. Wait DMA idle */
        { int t = 100000; while ((LCD_REG(0x8C) & 3) && --t > 0); }
        LCD_REG(0x80) = 1;
        {
            /* ILI9326 GRAM window via P18 mode (panel is in P18/ILI9326 state) */
            LCD_CON = 0x80000DA9;
            if (panel_type >= 2) {
                lcd_cmd(0x210); lcd_data(0);
                lcd_cmd(0x211); lcd_data(319);
                lcd_cmd(0x212); lcd_data(0);
                lcd_cmd(0x213); lcd_data(239);
                lcd_cmd(0x200); lcd_data(0);
                lcd_cmd(0x201); lcd_data(0);
                lcd_cmd(0x202);
            }
            while (!(LCD_STATUS & 0x2));
            LCD_CON = 0x81100DB9;
        }
        LCD_REG(0x80) = 0;
        { int t = 100000; while ((LCD_REG(0x8C) & 3) && --t > 0); }
        uint32_t dt = USEC_TIMER - t0;
        if (push == 0) {
            /* First push: monitor LCD STATUS to detect incoming compositor data */
            vlog("  Push %d: %lu us STATUS=%08lx 8C=%08lx 70=%08lx",
                 push, (unsigned long)dt,
                 (unsigned long)LCD_STATUS, (unsigned long)LCD_REG(0x8C),
                 (unsigned long)LCD_REG(0x70));
        } else {
            vlog("  Push %d: %lu us", push, (unsigned long)dt);
        }
    }

    /* Hold decoded frame on screen for 10 seconds.
     * Disable passthrough AND take bus to completely freeze display. */
    LCD_REG(0x70) = 0;  /* passthrough OFF — no more compositor pushes */
    LCD_REG(0x80) = 1;  /* CPU takes bus */

    /* Comprehensive GRAM scan — read pixels at multiple positions */
    vlog("  GRAM scan (P18 readback):");
    {
        /* Switch to P18 command mode for GRAM readback */
        while (!(LCD_STATUS & 0x2));
        LCD_CON = 0x80000DA8;  /* Rockbox LCD_MODE_P18 */

        static const struct { int x, y; } scan_pts[] = {
            {0,0}, {160,0}, {319,0},
            {0,60}, {160,60},
            {0,120}, {160,120}, {319,120},
            {0,180}, {160,180},
            {0,239}, {160,239}, {319,239},
        };

        for (int i = 0; i < 13; i++) {
            int x = scan_pts[i].x, y = scan_pts[i].y;
            lcd_cmd(0x200); lcd_data(x);
            lcd_cmd(0x201); lcd_data(y);
            lcd_cmd(0x202);
            while (!(LCD_STATUS & 0x2));
            /* dummy read */
            LCD_RDATA = 0;
            { int t = 100000; while (!(LCD_STATUS & 1) && --t > 0); }
            (void)LCD_DBUFF;
            /* real read */
            LCD_RDATA = 0;
            { int t = 100000; while (!(LCD_STATUS & 1) && --t > 0); }
            uint32_t g = LCD_DBUFF;
            uint32_t gs = g >> 1;
            vlog("    (%3d,%3d): raw=%05lx >>1: R=%2lu G=%2lu B=%2lu",
                 x, y, (unsigned long)(g & 0x3FFFF),
                 (unsigned long)((gs>>12)&0x3F),
                 (unsigned long)((gs>>6)&0x3F),
                 (unsigned long)(gs&0x3F));
        }

        /* Restore P9 passthrough mode */
        while (!(LCD_STATUS & 0x2));
        LCD_CON = 0x81100DB9;
    }

    /* Also dump scaler coefficients to verify they're clean */
    {
        volatile uint32_t *c = (volatile uint32_t *)COMP_BASE;
        vlog("  Scaler post-push (0x0F0): %08lx %08lx %08lx %08lx",
             (unsigned long)c[0x0F0/4], (unsigned long)c[0x0F4/4],
             (unsigned long)c[0x0F8/4], (unsigned long)c[0x0FC/4]);
        vlog("  Scaler (0x100): %08lx %08lx %08lx %08lx",
             (unsigned long)c[0x100/4], (unsigned long)c[0x104/4],
             (unsigned long)c[0x108/4], (unsigned long)c[0x10C/4]);
        vlog("  Scaler (0x110): %08lx %08lx %08lx %08lx",
             (unsigned long)c[0x110/4], (unsigned long)c[0x114/4],
             (unsigned long)c[0x118/4], (unsigned long)c[0x11C/4]);
    }

    vlog("  Holding decoded frame for 10s (passthrough OFF)...");
    vlog("  comp: 028=%08lx 038=%08lx 03C=%08lx 044=%08lx",
         (unsigned long)COMP_REG(0x028), (unsigned long)COMP_REG(0x038),
         (unsigned long)COMP_REG(0x03C), (unsigned long)COMP_REG(0x044));
    vlog("  VP: 000=%08lx 010=%08lx 014=%08lx 028=%08lx",
         (unsigned long)CLCD_REG(0x000), (unsigned long)CLCD_REG(0x010),
         (unsigned long)CLCD_REG(0x014), (unsigned long)CLCD_REG(0x028));
    vlog("  DISP: 000=%08lx 008=%08lx 00C=%08lx 070=%08lx",
         (unsigned long)DISP_REG(0x000), (unsigned long)DISP_REG(0x008),
         (unsigned long)DISP_REG(0x00C), (unsigned long)DISP_REG(0x070));
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 10000000) rb->backlight_on(); }

#if 0 /* Skip diagnostic tests — VPP pipeline confirmed working */
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
    if (panel_type >= 2) {
        lcd_cmd(0x210); lcd_data(0);
        lcd_cmd(0x211); lcd_data(319);
        lcd_cmd(0x212); lcd_data(0);
        lcd_cmd(0x213); lcd_data(239);
        lcd_cmd(0x200); lcd_data(0);
        lcd_cmd(0x201); lcd_data(0);
        lcd_cmd(0x202);
        for (int p = 0; p < 76800; p++) {
            lcd_data(0xF800);
        }
    }
    while (!(LCD_STATUS & 0x2));
    LCD_REG(0x80) = 0;
    vlog("  Wrote 76800 red pixels directly to GRAM");
    /* GRAM readback sanity: verify the RED pixels are readable.
     * ROM 0xB0B68: Apple shifts LCD_DBUFF >> 1 (data on D[8:1] for P8).
     * ILI9326: 1-2 dummy reads before real data.
     * Try both 1-dummy and 2-dummy, log raw + shifted values. */
    {
        LCD_REG(0x80) = 1;  /* CPU must own bus for GRAM read commands */
        LCD_CON = 0x80000DA8;  /* P18 read mode (Rockbox LCD_MODE_P18) */  /* P18 read mode (bit 0=0 = read direction) */
        lcd_cmd(0x200); lcd_data(160);
        lcd_cmd(0x201); lcd_data(120);
        lcd_cmd(0x202);
        { int t = 100000; while (!(LCD_STATUS & 0x2) && --t > 0); }
        uint32_t d0 = 0xDEAD, d1 = 0xDEAD, d2 = 0xDEAD;
        LCD_RDATA = 0; { int t = 100000; while (!(LCD_STATUS & 1) && --t > 0); } d0 = LCD_DBUFF;  /* dummy */
        LCD_RDATA = 0; { int t = 100000; while (!(LCD_STATUS & 1) && --t > 0); } d1 = LCD_DBUFF;  /* pixel 1 */
        LCD_RDATA = 0; { int t = 100000; while (!(LCD_STATUS & 1) && --t > 0); } d2 = LCD_DBUFF;  /* pixel 2 */
        vlog("  RED GRAM: dummy=%08lx px1=%08lx px2=%08lx",
             (unsigned long)d0, (unsigned long)d1, (unsigned long)d2);
        vlog("  RED >>1:  d0=%08lx d1=%08lx d2=%08lx",
             (unsigned long)(d0>>1), (unsigned long)(d1>>1), (unsigned long)(d2>>1));
        LCD_CON = 0x81100DB9;
        LCD_REG(0x80) = 0;
    }
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 5000000) rb->backlight_on(); }

    /* Re-enable passthrough for VPP test */
    LCD_REG(0x88) = 0x01000000;  /* restore compositor data input */
    LCD_REG(0x70) = 1;
    DISP_REG(0x70) = 0x281;

    /* DMA ADDRESS TEST: point at Rockbox code (0x08000000) instead of our buffer.
     * If output CHANGES from blue → DMA IS reading, just wrong color/format.
     * If output STAYS blue → DMA truly doesn't read from ANY address. */
    vlog("  DMA test: buf=0x08000000 (Rockbox code) for 5s");
    CLCD_REG(0x028) = 0x08000000;
    CLCD_REG(0x02C) = 0x08010000;
    CLCD_REG(0x034) = 0x08000000 + frame_w;
    CLCD_REG(0x038) = 0x08010000 + frame_w/2;
    lcd_push_frame(panel_type);
    COMP_REG(0x200) |= 0x80;  /* strobe after GRAM setup + bus release */
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 5000000) rb->backlight_on(); }

    /* Now point at our actual decoded frame buffer */
    vlog("  DMA test: buf=Y(decoded) for 5s");
    CLCD_REG(0x028) = PHYS(y_out);
    CLCD_REG(0x02C) = PHYS(cb_out);
    CLCD_REG(0x034) = 0;  /* no second field (match Phase 4) */
    CLCD_REG(0x038) = 0;
    lcd_push_frame(panel_type);
    COMP_REG(0x200) |= 0x80;  /* strobe after GRAM setup + bus release */
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 5000000) rb->backlight_on(); }

    /* Push one more frame to get CURRENT compositor output into GRAM */
    CLCD_REG(0x008) |= 0x10000;  /* shadow commit */
    lcd_push_frame(panel_type);
    COMP_REG(0x200) |= 0x80;  /* strobe to push current frame to GRAM */
    { int t = 100000; while ((LCD_REG(0x8C) & 3) && --t > 0); }  /* wait push */

    /* GRAM readback — 5-point X scan at Y=120. 1 dummy + 1 data per point. */
    vlog("Phase 6b: GRAM gradient scan (Y=120)");
    {
        static const int test_x[] = {0, 64, 128, 192, 256};
        LCD_REG(0x70) = 0;
        for (volatile int d = 0; d < 10000; d++);
        LCD_REG(0x80) = 1;
        lcd_set_con(0x80000DA8);  /* P18 read mode */
        for (int ti = 0; ti < 5; ti++) {
            lcd_cmd(0x200); lcd_data(test_x[ti]);
            lcd_cmd(0x201); lcd_data(120);
            lcd_cmd(0x202);
            { int t = 100000; while (!(LCD_STATUS & 0x2) && --t > 0); }
            LCD_RDATA = 0; { int t = 100000; while (!(LCD_STATUS & 1) && --t > 0); } (void)LCD_DBUFF;
            LCD_RDATA = 0; { int t = 100000; while (!(LCD_STATUS & 1) && --t > 0); }
            uint32_t px = LCD_DBUFF;
            vlog("  GRAM(%d,120)=%08lx >>1=%08lx", test_x[ti], (unsigned long)px, (unsigned long)(px>>1));
        }
        LCD_CON = 0x81100DB9;
        LCD_REG(0x70) = 1;
        DISP_REG(0x70) = 0x281;
    }

    /* ---- Phase 7: Layer 5 ON/OFF diagnostic ---- */

    vlog("Phase 7: Layer 5 diagnostics");
    vlog("  CLCD+0x000=%08lx MIXER+0x00C=%08lx comp+0x028=%08lx",
         (unsigned long)CLCD_REG(0x000), (unsigned long)MIXER_REG(0x00C),
         (unsigned long)COMP_REG(0x028));

    /* Helper macro: set GRAM window + observe + readback */
#define GRAM_TEST(label, obs_us) do { \
    /* Step 1: CPU takes bus, set GRAM window on panel */ \
    /* Step 1: Trigger FIRST (prepare compositor output) */ \
    CLCD_REG(0x000) |= 1; \
    MIXER_REG(0x000) = 7; \
    { uint32_t _d3c = DISP_REG(0x03C); DISP_REG(0x03C) = _d3c | 1; } \
    { uint32_t _d3c = DISP_REG(0x03C); DISP_REG(0x03C) = _d3c | 2; } \
    { uint32_t _d3c = DISP_REG(0x03C); DISP_REG(0x03C) = _d3c | 4; } \
    for (volatile int _rd = 0; _rd < 10000; _rd++); \
    /* Step 2: LCD push — no LCD_CON switch */ \
    { int _t = 100000; while ((LCD_REG(0x8C) & 3) && --_t > 0); } \
    LCD_REG(0x80) = 1; \
    if (panel_type >= 2) { \
        lcd_cmd(0x210); lcd_data(0); lcd_cmd(0x211); lcd_data(319); \
        lcd_cmd(0x212); lcd_data(0); lcd_cmd(0x213); lcd_data(239); \
        lcd_cmd(0x200); lcd_data(0); lcd_cmd(0x201); lcd_data(0); \
        lcd_cmd(0x202); \
    } \
    while (!(LCD_STATUS & 0x2)); \
    /* Step 3: Release bus */ \
    LCD_REG(0x80) = 0; \
    /* Step 3: Wait for compositor to push frame via i80 */ \
    { int _t = 100000; while ((LCD_REG(0x8C) & 3) && --_t > 0); } \
    /* Observe */ \
    { uint32_t _t = USEC_TIMER; while ((USEC_TIMER - _t) < (obs_us)) rb->backlight_on(); } \
    /* GRAM readback (passthrough off). 3 reads with timeouts. */ \
    LCD_REG(0x70) = 0; \
    for (volatile int _d = 0; _d < 10000; _d++); \
    LCD_REG(0x80) = 1; \
    LCD_CON = 0x80000DA8;  /* P18 read mode (Rockbox LCD_MODE_P18) */ \
    lcd_cmd(0x200); lcd_data(160); lcd_cmd(0x201); lcd_data(120); lcd_cmd(0x202); \
    { int _w = 100000; while (!(LCD_STATUS & 0x2) && --_w > 0); } \
    LCD_RDATA = 0; { int _w = 100000; while (!(LCD_STATUS & 1) && --_w > 0); } (void)LCD_DBUFF; \
    LCD_RDATA = 0; { int _w = 100000; while (!(LCD_STATUS & 1) && --_w > 0); } \
    { uint32_t _g = LCD_DBUFF; vlog("  %s: GRAM=%08lx >>1=%08lx", label, (unsigned long)_g, (unsigned long)(_g>>1)); } \
    LCD_CON = 0x81100DB9; LCD_REG(0x80) = 0; LCD_REG(0x70) = 1; DISP_REG(0x70) = 0x281; \
} while(0)

    /* Test A: Layer 5 OFF, BG_COLOR = gray */
    COMP_REG(0x028) = 0;
    COMP_REG(0x200) |= 0x80; COMP_REG(0x000) = 1; for (volatile int _gd = 0; _gd < 50000; _gd++);
    GRAM_TEST("TestA(L5=OFF,gray)", 3000000);

    /* Test B: YCbCr gray hypothesis (G2) — if compositor uses YCbCr internally,
     * 0x00808080 = Y=128,Cb=128,Cr=128 = neutral gray in YCbCr */
    COMP_REG(0x00C) = 0x00808080;
    COMP_REG(0x200) |= 0x80; COMP_REG(0x000) = 1; for (volatile int _gd = 0; _gd < 50000; _gd++);
    GRAM_TEST("TestB(L5=OFF,YCbCr_gray)", 3000000);

    /* Test C: Pure channels — R=255 */
    COMP_REG(0x00C) = 0x000000FF;
    COMP_REG(0x200) |= 0x80; COMP_REG(0x000) = 1; for (volatile int _gd = 0; _gd < 50000; _gd++);
    GRAM_TEST("TestC(L5=OFF,R=255)", 2000000);

    /* Test D: Pure channels — G=255 */
    COMP_REG(0x00C) = 0x0000FF00;
    COMP_REG(0x200) |= 0x80; COMP_REG(0x000) = 1; for (volatile int _gd = 0; _gd < 50000; _gd++);
    GRAM_TEST("TestD(L5=OFF,G=255)", 2000000);

    /* Test E: Pure channels — B=255 */
    COMP_REG(0x00C) = 0x00FF0000;
    COMP_REG(0x200) |= 0x80; COMP_REG(0x000) = 1; for (volatile int _gd = 0; _gd < 50000; _gd++);
    GRAM_TEST("TestE(L5=OFF,B=255)", 2000000);

    /* Test F: White */
    COMP_REG(0x00C) = 0x00FFFFFF;
    COMP_REG(0x200) |= 0x80; COMP_REG(0x000) = 1; for (volatile int _gd = 0; _gd < 50000; _gd++);
    GRAM_TEST("TestF(L5=OFF,white)", 2000000);

#undef GRAM_TEST
#endif /* skip diagnostic tests */

    /* Phase 7b REMOVED — software render was giving false positives.
     * Only VPP hardware pipeline output should be visible. */

    /* ---- Phase 8: Shutdown ---- */

    vlog("Phase 8: Shutdown");

    /* Restore LCD+0x70=1 and LCD+0x80=0 to match v35g's entry state
     * (hold disabled passthrough and took bus — undo that first) */
    LCD_REG(0x70) = 1;
    LCD_REG(0x80) = 0;
    { int t = 100000; while ((LCD_REG(0x8C) & 3) && --t > 0); }

    /* v35g EXACT shutdown from here */
    { int t = 100000; while ((LCD_REG(0x8C) & 3) && --t > 0); }
    /* Disable passthrough and VPP output BEFORE restoring LCD */
    LCD_REG(0x70) = 0;               /* passthrough OFF */
    LCD_REG(0x80) = 0;               /* release bus */
    DISP_REG(0x70) = 0;              /* clear DISP panel routing */
    DISP_REG(0x000) = 0;             /* stop DISP */
    COMP_REG(0x000) = 0;             /* stop compositor */
    for (volatile int d = 0; d < 10000; d++);
    /* v35g EXACT shutdown — panel commands BEFORE clock cycle, NO LCD+0x80=1 */
    lcd_set_con(0x80000DA9);
    lcd_cmd(0x003); lcd_data(0x0230);  /* Entry Mode: restore RGB for Rockbox */
    lcd_cmd(0x3A); lcd_data(0x06);     /* COLMOD: restore Rockbox default */
    lcd_set_con(0x81100DB9);
    /* Restore LCD registers */
    LCD_REG(0x88) = saved_lcd_88;
    LCD_REG(0x20) = saved_lcd_20;
    LCD_REG(0x7C) = saved_lcd_7c;
    LCD_REG(0x74) = saved_lcd_74;
    LCD_REG(0x78) = saved_lcd_78;
    LCD_PHTIME = 0x33;               /* re-init phase timing */
    LCD_CON = saved_lcd_con;
    { int t = 100000; while ((LCD_REG(0x8C) & 3) && --t > 0); }
    rb->lcd_update();

    /* ---- Restore GPIO7.1 to ATA (v34 always did this) ---- */
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
