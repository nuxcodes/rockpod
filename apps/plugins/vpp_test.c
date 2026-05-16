/***************************************************************************
 * S5L8702 Compositor BG_COLOR Isolation Diagnostic (v19)
 *
 * Tests ONLY the compositor→LCD passthrough path. NO VPP pipeline at all.
 * Purpose: isolate whether the VPP pipeline (CLCD/MIXER/DISP) or the
 * compositor/LCD output path is the problem.
 *
 * Architecture (from 500+ agent marathon):
 *   Signal path: CLCD(0x391) → Mixer(0x392) → DISP(0x393)
 *                → Compositor(0x389) → LCD MCU passthrough(0x383) → Panel
 *
 *   Compositor has NO independent DMA (B1 confirmed). All pixel data must
 *   be pushed into it via CLCD→MIXER→DISP. BUT it can output BG_COLOR
 *   without any pixel data — just needs GO + LCD+0x80 push.
 *
 *   If BG_COLOR appears: compositor→LCD works. Problem is VPP pipeline.
 *   If BG_COLOR does NOT appear: compositor→LCD is broken. Fix that first.
 *
 * See ipod-re/vpp_marathon_v91.md for full register documentation.
 ****************************************************************************/

#include "plugin.h"

#ifdef IPOD_6G
#include "s5l87xx.h"

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

static void vpp_lcd_cmd(uint16_t cmd)
{
    while (LCD_STATUS & 0x10);
    LCD_WCMD = cmd;
}

static void vpp_lcd_data(uint16_t data)
{
    while (LCD_STATUS & 0x10);
    LCD_WDATA = data;
}

/* ---- Main Test ---- */

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;

    log_open();
    vlog("=== Compositor BG_COLOR Isolation v19 ===");
    vlog("=== NO VPP pipeline — compositor + LCD passthrough ONLY ===");

    uint32_t saved_lcd_con = 0;

    /* === Phase 1: Show splash, then take over LCD === */
    rb->splashf(HZ, "v19 BG_COLOR");
    rb->sleep(HZ / 2);

    rb->lcd_scroll_stop();
    rb->backlight_on();

    int panel_type = (PDAT(6) & 0x30) >> 4;
    vlog("Panel type: %d (0/1=8bit ILI9340, 2/3=16bit ILI9320)", panel_type);

    /* === Phase 2: Enable ONLY compositor clocks ===
     * NO VPP clocks (bits 14-16). NO SVID clock.
     * Only bits 7+13 for compositor. */
    vlog("Phase 2: Enabling compositor clocks ONLY");
    uint32_t pwrcon_before = PWRCON(0);
    vlog("PWRCON0 before: 0x%08lx", (unsigned long)pwrcon_before);

    /* PWRCON(2) gate 64 — may be shared VPP/compositor fabric clock.
     * Keep enabling it as before. */
    {
        volatile uint32_t *pwrcon2 = (volatile uint32_t *)0x3C500058;
        uint32_t pc2_before = *pwrcon2;
        *pwrcon2 &= ~1;
        vlog("PWRCON2: 0x%08lx -> 0x%08lx (gate64=%s)",
             (unsigned long)pc2_before, (unsigned long)*pwrcon2,
             (pc2_before & 1) ? "was GATED, now ENABLED" : "already enabled");
    }

    /* Gate→ungate compositor for clean POR state */
    PWRCON(0) |= 0x2080;    /* gate bits 7+13 */
    { volatile int d; for (d = 0; d < 10000; d++); }
    PWRCON(0) &= ~0x2080;   /* ungate */
    { volatile int d; for (d = 0; d < 10000; d++); }
    vlog("  Compositor clocks reset (gate->ungate)");

    /* VPP clocks (bits 14-16) deliberately LEFT GATED.
     * This is the key difference from v136: NO VPP at all. */
    vlog("  VPP clocks REMAIN GATED (bits 14-16 = %s)",
         (PWRCON(0) & 0x1C000) ? "gated" : "ERROR: ungated!");

    /* === Phase 3: Compositor state dump (POR after gate cycle) === */
    vlog("Phase 3: Compositor POR state");
    {
        volatile uint32_t *comp = (volatile uint32_t *)0x38900000;
        vlog("  CTRL=%08lx CFG=%08lx MODE=%08lx BG=%08lx",
             (unsigned long)comp[0], (unsigned long)comp[0x008/4],
             (unsigned long)comp[0x00C/4], (unsigned long)comp[0x00C/4]);
        vlog("  +010=%08lx +024=%08lx +200=%08lx",
             (unsigned long)comp[0x010/4], (unsigned long)comp[0x024/4],
             (unsigned long)comp[0x200/4]);
        vlog("  timing: 1EC=%08lx 1F0=%08lx 1F4=%08lx 1F8=%08lx 1FC=%08lx",
             (unsigned long)comp[0x1EC/4], (unsigned long)comp[0x1F0/4],
             (unsigned long)comp[0x1F4/4], (unsigned long)comp[0x1F8/4],
             (unsigned long)comp[0x1FC/4]);
    }

    /* === Phase 4: Full compositor init (identical to v136 Phase 6) ===
     * All values from ROM trace (FUN_0014d240).
     * NO Layer 5 config — Layer 5 does not exist on compositor (G8 proved).
     * NO comp+0x038-0x044 writes — those kill DMA (v66 proved). */
    rb->backlight_on();
    vlog("Phase 4: Compositor init (ROM-matched, no VPP)");
    {
        volatile uint32_t *comp = (volatile uint32_t *)0x38900000;

        /* Step 1: Clear pipeline bit */
        comp[0x200/4] &= ~1;

        /* Step 2-3: Basic config */
        comp[0x004/4] = 1;
        comp[0x020/4] = 1;
        comp[0x0D4/4] = 1;  /* panel type = LCD */

        /* Per-channel identity gain (0x1000 = 1.0 in 4.12 FP) */
        comp[0x0D8/4] = 0x00001000;
        comp[0x0DC/4] = 0;
        comp[0x0E0/4] = 0x00001000;
        comp[0x0E4/4] = 0;
        comp[0x0E8/4] = 0x00001000;
        comp[0x0EC/4] = 0;

        /* Mode config WITHOUT bit 30 (display output enable deferred).
         * bit 0=enable, bit 8=bypass, bit 15=video overlay enable,
         * bits[17:16]=01 output fmt, bits[21:20]=01 input fmt B,
         * bits[25:24]=01 input fmt A. */
        comp[0x008/4] = 0x01118101;

        /* BG_COLOR — start with RED (XBGR format per v106 agent) */
        comp[0x00C/4] = 0x000000FF;

        /* Pipeline enable */
        comp[0x200/4] |= 0x10080;

        /* DMA config */
        comp[0x204/4] = 2;
        comp[0x208/4] = 0;
        comp[0x20C/4] = 2;

        /* Viewport: full screen */
        comp[0x210/4] = 0x00010110;
        comp[0x214/4] = 0x00EF013F;  /* (239<<16)|319 */

        /* Gamma LUTs — identity. Without these, all pixels map to BLACK.
         * ROM evidence: FUN_00088d8c writes to 0x38900400/0x800/0xC00. */
        for (int i = 0; i < 256; i++) {
            comp[0x400/4 + i] = i * 4;  /* R channel */
            comp[0x800/4 + i] = i * 4;  /* G channel */
            comp[0xC00/4 + i] = i * 4;  /* B channel */
        }

        /* Timing from iBoot (only values that ever produced DMA=0x034d) */
        comp[0x1EC/4] = 0x0C;
        comp[0x1F0/4] = 0x26;
        comp[0x1F4/4] = 0x10;
        comp[0x1F8/4] = 0x82;
        comp[0x1FC/4] = 0x4E;
        vlog("  Timing: %08lx %08lx %08lx %08lx %08lx",
             (unsigned long)comp[0x1EC/4], (unsigned long)comp[0x1F0/4],
             (unsigned long)comp[0x1F4/4], (unsigned long)comp[0x1F8/4],
             (unsigned long)comp[0x1FC/4]);

        /* NOW set bit 30 (display output enable) — Apple does LAST */
        comp[0x008/4] |= 0x40000000;

        /* IRQ mask clear */
        comp[0x024/4] = 0x00FFFFFF;

        vlog("  Compositor init complete: CFG=%08lx +200=%08lx",
             (unsigned long)comp[0x008/4], (unsigned long)comp[0x200/4]);
    }

    /* === Phase 5: LCD passthrough setup ===
     * Two-function ordering from batch 4 agent 5:
     *   1. lcd_mcu_passthrough_init (ROM 0xca178)
     *   2. FUN_0014deec passthrough enable (ROM 0x14deec) */
    rb->backlight_on();
    vlog("Phase 5: LCD passthrough setup");

    /* Poll LCD+0x8C for bus idle */
    {
        int timeout = 100000;
        while ((*(volatile uint32_t *)(0x3830008C) & 3) && --timeout > 0);
        vlog("  LCD+0x8C = 0x%08lx (bus %s)",
             (unsigned long)*(volatile uint32_t *)(0x3830008C),
             timeout > 0 ? "idle" : "TIMEOUT");
    }

    /* Part 1: Static LCD config */
    saved_lcd_con = LCD_CON;
    vlog("  LCD_CON before=0x%08lx", saved_lcd_con);
    LCD_CON = 0x81100DB9;  /* Apple's P9 passthrough */
    *(volatile uint32_t *)(0x38300088) = 0x01000000;
    *(volatile uint32_t *)(0x38300020) = 0x33;
    *(volatile uint32_t *)(0x3830007C) = 0x00000402;
    vlog("  LCD_CON=0x%08lx +0x20=0x33 +0x7C=0x402 +0x88=0x01000000",
         (unsigned long)LCD_CON);

    /* Part 2: Passthrough enable */
    /* Step 1: Porch timing */
    *(volatile uint32_t *)(0x38300078) = 0x000A000A;

    /* Step 2: COMP+0x3AC */
    {
        volatile uint32_t *comp = (volatile uint32_t *)0x38900000;
        comp[0x3AC/4] = 0x04004003;
    }

    /* Step 3: Panel GRAM commands */
    {
        uint32_t saved_con = LCD_CON;
        vpp_lcd_config(0x80000DA9);  /* P18 */
        if (panel_type >= 2) {
            vpp_lcd_cmd(0x210); vpp_lcd_data(0);
            vpp_lcd_cmd(0x211); vpp_lcd_data(319);
            vpp_lcd_cmd(0x212); vpp_lcd_data(0);
            vpp_lcd_cmd(0x213); vpp_lcd_data(239);
            vpp_lcd_cmd(0x200); vpp_lcd_data(0);
            vpp_lcd_cmd(0x201); vpp_lcd_data(0);
            vpp_lcd_cmd(0x202);
        } else {
            vpp_lcd_config(0x80000C21);
            vpp_lcd_cmd(0x2A);
            vpp_lcd_data(0x00); vpp_lcd_data(0x00);
            vpp_lcd_data(0x01); vpp_lcd_data(0x3F);
            vpp_lcd_cmd(0x2B);
            vpp_lcd_data(0x00); vpp_lcd_data(0x00);
            vpp_lcd_data(0x00); vpp_lcd_data(0xEF);
            vpp_lcd_cmd(0x2C);
        }
        vpp_lcd_config(saved_con);  /* back to P9 */
    }
    vlog("  Panel GRAM done");

    /* Step 4: Resolution */
    *(volatile uint32_t *)(0x38300074) = 0x00F00140;

    /* Step 5: Enable passthrough — LAST */
    *(volatile uint32_t *)(0x38300070) = 1;
    vlog("  Passthrough enabled: LCD+0x70=%08lx",
         (unsigned long)*(volatile uint32_t *)(0x38300070));

    /* === Phase 6: Compositor GO + initial settle === */
    vlog("Phase 6: Compositor GO");
    {
        volatile uint32_t *comp = (volatile uint32_t *)0x38900000;
        comp[0x000/4] = 1;  /* GO strobe */
        vlog("  GO fired: CTRL=%08lx", (unsigned long)comp[0]);
        vlog("  +010 before delay: %08lx", (unsigned long)comp[0x010/4]);

        /* Wait 200ms for compositor initial output */
        { uint32_t start = USEC_TIMER; while ((USEC_TIMER - start) < 200000); }

        vlog("  +010 after 200ms: %08lx", (unsigned long)comp[0x010/4]);
        vlog("  +200 after 200ms: %08lx", (unsigned long)comp[0x200/4]);

        /* Re-fire i80 strobe */
        comp[0x200/4] |= 0x80;
        { volatile int d; for (d = 0; d < 10000; d++); }
        vlog("  +200 after re-strobe: %08lx", (unsigned long)comp[0x200/4]);
    }

    /* === Phase 7: BG_COLOR cycle with per-color GRAM readback ===
     * Uses the v134 minimal working pattern:
     *   1. GRAM_SETUP_OUTSIDE (panel in write mode)
     *   2. LCD+0x80 = 1 (open output gate)
     *   3. LCD_CON self-write (WR# strobe trigger)
     *   4. 50000-delay hold
     *   5. LCD+0x80 = 0 (close gate)
     *   6. Bus-idle wait
     *   7. GRAM readback to verify pixel values */
    vlog("Phase 7: BG_COLOR cycle (6 colors, 3s each)");
    {
        volatile uint32_t *comp_c = (volatile uint32_t *)0x38900000;
        /* XBGR format per v106 agent (or XRGB — P9 masks the difference) */
        uint32_t colors[] = {
            0x00000000,  /* BLACK */
            0x000000FF,  /* RED   (XBGR: R=FF) */
            0x0000FF00,  /* GREEN (XBGR: G=FF) */
            0x00FF0000,  /* BLUE  (XBGR: B=FF) */
            0x00FFFFFF,  /* WHITE */
            0x000F0F0F,  /* DARK GRAY */
        };
        const char *names[] = {"BLACK","RED","GREEN","BLUE","WHITE","GRAY"};

        for (int c = 0; c < 6; c++) {
            rb->backlight_on();

            /* Button abort */
            if (rb->button_get(false) & BUTTON_MENU)
            {
                vlog("  Aborted by user at color %d", c);
                break;
            }

            /* Write BG_COLOR + latch + GO */
            comp_c[0x00C/4] = colors[c];
            { uint32_t c200 = comp_c[0x200/4];
              comp_c[0x200/4] = c200 | 0x10080; }
            comp_c[0] = 1;

            /* 100ms settle */
            { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 100000); }

            /* Wait bus idle */
            { int t = 100000;
              while ((*(volatile uint32_t *)(0x3830008C) & 3) && --t > 0); }

            /* GRAM_SETUP_OUTSIDE (v135 proved: REQUIRED per-frame) */
            vpp_lcd_config(0x80000DA9);  /* P18 */
            if (panel_type >= 2) {
                vpp_lcd_cmd(0x210); vpp_lcd_data(0);
                vpp_lcd_cmd(0x211); vpp_lcd_data(319);
                vpp_lcd_cmd(0x212); vpp_lcd_data(0);
                vpp_lcd_cmd(0x213); vpp_lcd_data(239);
                vpp_lcd_cmd(0x200); vpp_lcd_data(0);
                vpp_lcd_cmd(0x201); vpp_lcd_data(0);
                vpp_lcd_cmd(0x202);
            } else {
                vpp_lcd_config(0x80000C21);
                vpp_lcd_cmd(0x2A); vpp_lcd_data(0); vpp_lcd_data(0);
                vpp_lcd_data(0x01); vpp_lcd_data(0x3F);
                vpp_lcd_cmd(0x2B); vpp_lcd_data(0); vpp_lcd_data(0);
                vpp_lcd_data(0); vpp_lcd_data(0xEF);
                vpp_lcd_cmd(0x2C);
            }
            vpp_lcd_config(0x81100DB9);  /* back to P9 */

            /* Self-write + delay push (v134 minimal pattern) */
            *(volatile uint32_t *)(0x38300080) = 1;
            { uint32_t v = LCD_CON; LCD_CON = v; }
            { volatile int d; for (d = 0; d < 50000; d++); }
            *(volatile uint32_t *)(0x38300080) = 0;

            /* Wait bus idle */
            { int t = 100000;
              while ((*(volatile uint32_t *)(0x3830008C) & 3) && --t > 0); }

            /* Per-color GRAM readback (passthrough off for valid read) */
            *(volatile uint32_t *)(0x38300070) = 0;
            { volatile int d; for (d = 0; d < 10000; d++); }

            if (panel_type >= 2) {
                vpp_lcd_config(0x80000DA8);
                vpp_lcd_cmd(0x200); vpp_lcd_data(0);
                vpp_lcd_cmd(0x201); vpp_lcd_data(0);
                vpp_lcd_cmd(0x202);
                while (!(LCD_STATUS & 0x2));
                LCD_RDATA = 0; while (!(LCD_STATUS & 1));
                uint32_t dummy = LCD_DBUFF;
                LCD_RDATA = 0; while (!(LCD_STATUS & 1));
                uint32_t px0 = LCD_DBUFF;
                LCD_RDATA = 0; while (!(LCD_STATUS & 1));
                uint32_t px1 = LCD_DBUFF;
                vlog("  %s(0x%06lx): GRAM dummy=%08lx px0=%08lx px1=%08lx C010=%08lx",
                     names[c], (unsigned long)colors[c],
                     (unsigned long)dummy, (unsigned long)px0,
                     (unsigned long)px1,
                     (unsigned long)comp_c[0x010/4]);
            } else {
                vpp_lcd_config(0x80000c20);
                vpp_lcd_cmd(0x2A);
                vpp_lcd_data(0); vpp_lcd_data(0); vpp_lcd_data(0); vpp_lcd_data(0);
                vpp_lcd_cmd(0x2B);
                vpp_lcd_data(0); vpp_lcd_data(0); vpp_lcd_data(0); vpp_lcd_data(0);
                vpp_lcd_cmd(0x2E);
                while (!(LCD_STATUS & 0x2));
                LCD_RDATA = 0; while (!(LCD_STATUS & 1));
                uint32_t dummy = LCD_DBUFF;
                LCD_RDATA = 0; while (!(LCD_STATUS & 1));
                uint32_t r = LCD_DBUFF >> 1;
                LCD_RDATA = 0; while (!(LCD_STATUS & 1));
                uint32_t g = LCD_DBUFF >> 1;
                LCD_RDATA = 0; while (!(LCD_STATUS & 1));
                uint32_t b = LCD_DBUFF >> 1;
                vlog("  %s(0x%06lx): GRAM dummy=%08lx R=%02lx G=%02lx B=%02lx C010=%08lx",
                     names[c], (unsigned long)colors[c],
                     (unsigned long)dummy, (unsigned long)r,
                     (unsigned long)g, (unsigned long)b,
                     (unsigned long)comp_c[0x010/4]);
            }

            /* Re-enable passthrough for next color */
            vpp_lcd_config(0x81100DB9);
            *(volatile uint32_t *)(0x38300070) = 1;

            /* 3s visual observation */
            { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 3000000); }
        }
    }

    /* === Phase 8: Final status === */
    vlog("Phase 8: Final state");
    {
        volatile uint32_t *comp = (volatile uint32_t *)0x38900000;
        vlog("  Compositor: CTRL=%08lx CFG=%08lx +010=%08lx +200=%08lx",
             (unsigned long)comp[0], (unsigned long)comp[0x008/4],
             (unsigned long)comp[0x010/4], (unsigned long)comp[0x200/4]);
        vlog("  LCD: CON=%08lx STATUS=%08lx +0x70=%08lx +0x80=%08lx +0x8C=%08lx",
             (unsigned long)LCD_CON, (unsigned long)LCD_STATUS,
             (unsigned long)*(volatile uint32_t *)(0x38300070),
             (unsigned long)*(volatile uint32_t *)(0x38300080),
             (unsigned long)*(volatile uint32_t *)(0x3830008C));
        /* Confirm VPP was NEVER touched */
        vlog("  PWRCON0=%08lx (VPP bits 14-16 should be SET=gated)",
             (unsigned long)PWRCON(0));
        vlog("  VPP gated: %s", (PWRCON(0) & 0x1C000) ? "YES (correct)" : "NO (ERROR!)");
    }

    /* === Phase 9: Shutdown compositor + restore LCD === */
    vlog("Phase 9: Shutdown");

    /* Disable RGB passthrough + compositor */
    *(volatile uint32_t *)(0x38300080) = 0;
    *(volatile uint32_t *)(0x38300070) = 0;
    { int t = 100000; while ((*(volatile uint32_t *)(0x3830008C) & 3) && --t > 0); }
    *(volatile uint32_t *)0x38900000 = 0;    /* compositor off */
    PWRCON(0) |= 0x2080;  /* re-gate compositor clocks */

    /* Clear passthrough registers */
    *(volatile uint32_t *)(0x3830007C) = 0;
    *(volatile uint32_t *)(0x38300088) = 0;
    *(volatile uint32_t *)(0x38300074) = 0;
    *(volatile uint32_t *)(0x38300078) = 0;
    vpp_lcd_config(saved_lcd_con);
    if (panel_type >= 2) {
        vpp_lcd_config(0x80000DA8);
        vpp_lcd_cmd(0x001); vpp_lcd_data(0x0110);
        vpp_lcd_cmd(0x003); vpp_lcd_data(0x0230);
        vpp_lcd_config(saved_lcd_con);
    }
    vlog("  LCD restored (CON=%08lx)", (unsigned long)LCD_CON);

    vlog("=== v19 complete ===");
    log_close();

    rb->lcd_update();
    rb->splashf(HZ * 3, "v19 done! Check /vpp_test.log");

    return PLUGIN_OK;
}

#else /* !IPOD_6G */

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    rb->splash(HZ*3, "iPod 6G only");
    return PLUGIN_ERROR;
}

#endif
