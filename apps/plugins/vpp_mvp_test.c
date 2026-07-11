/***************************************************************************
 * S5L8702 VPP Test — clean compositor display path
 *
 * Decodes an H.264 I-frame and displays it via the hardware VPP pipeline:
 *   VPU-B decode → compositor (YCbCr→RGB CSC) → LCD controller → panel
 *
 * Key settings (all ROM-verified, v122m confirmed on hardware):
 *   - Compositor: format 8 (3-plane YUV420), rotation ON, scaler 1.0
 *   - LCD passthrough: P16 mode (0x80100DB0) for ILI9326 panels
 *   - LCD+0x7C = 0x401 (1 transfer/pixel for P16)
 *   - CSC: hardwired BT.601, enabled via comp+0x008 bit 8 clear
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

#define LCD_MODE_P16  0x80100DB0
#define LCD_MODE_P18  0x80000DA9

static int log_fd = -1;

static void vlog(const char *fmt, ...)
{
    if (log_fd < 0) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int len = rb->vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    rb->write(log_fd, buf, len);
    rb->write(log_fd, "\n", 1);
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

static void compositor_init(void)
{
    volatile uint32_t *c = (volatile uint32_t *)COMP_BASE;

    c[0x200/4] &= ~1;
    c[0x004/4] = 1;
    c[0x020/4] = 1;

    for (int i = 0; i < 256; i++) {
        c[0x400/4 + i] = i * 4;
        c[0x800/4 + i] = i * 4;
        c[0xC00/4 + i] = i * 4;
    }

    {
        volatile uint32_t *src = (volatile uint32_t *)0x0890D2DC;
        uint32_t t[5];
        for (int i = 0; i < 5; i++) t[i] = src[i];
        if (t[0] > 0 && t[0] < 0x1000 && t[4] > 0 && t[4] < 0x1000)
            for (int i = 0; i < 5; i++) c[(0x1EC + i*4)/4] = t[i];
        else {
            uint32_t hc[] = {0x0C, 0x26, 0x10, 0x82, 0x4E};
            for (int i = 0; i < 5; i++) c[(0x1EC + i*4)/4] = hc[i];
        }
    }

    c[0x0D8/4] = 0x00001000; c[0x0DC/4] = 0;
    c[0x0E0/4] = 0x00001000; c[0x0E4/4] = 0;
    c[0x0E8/4] = 0x00001000; c[0x0EC/4] = 0;

    {
        uint32_t v = c[0x008/4];
        v &= ~0x03000000; v |= 0x01000000;
        v &= ~0x00300000; v |= 0x00100000;
        v &= ~0x00030000; v |= 0x00010000;
        v &= ~1; v |= 1;
        c[0x008/4] = v;
    }
    c[0x00C/4] = 0x000F0F0F;
    { uint32_t v = c[0x008/4]; v |= 0x8000;      c[0x008/4] = v; }
    { uint32_t v = c[0x008/4]; v &= ~2;           c[0x008/4] = v; }
    { uint32_t v = c[0x008/4]; v |= 0x100;        c[0x008/4] = v; }
    { uint32_t v = c[0x008/4]; v |= 0x80;         c[0x008/4] = v; }
    { uint32_t v = c[0x008/4]; v |= 0x40000000;   c[0x008/4] = v; }

    c[0x200/4] |= 0x10080;
    c[0x204/4] = 2;
    c[0x208/4] = 0;
    c[0x20C/4] = 2;
    c[0x210/4] = 0x00010110;
    c[0x214/4] = 0x00EF013F;
    c[0x024/4] = 0x00FFFFFF;
}

static void compositor_configure_layer5(const uint8_t *y, const uint8_t *cb,
                                        const uint8_t *cr, int w, int h)
{
    for (int off = 0x024; off <= 0x044; off += 4)
        COMP_REG(off) = 0;
    for (int off = 0x04C; off <= 0x058; off += 4)
        COMP_REG(off) = 0;

    COMP_REG(0x028) = 0x100;
    COMP_REG(0x02C) = w | ((w / 2) << 16);
    COMP_REG(0x030) = 0;
    COMP_REG(0x034) = h | ((uint32_t)w << 16);
    COMP_REG(0x04C) = 0x10001000;
    COMP_REG(0x050) = 0;
    COMP_REG(0x054) = ((uint32_t)240 << 16) | 320;

    COMP_REG(0x038) = PHYS(y);
    COMP_REG(0x03C) = PHYS(cr);
    COMP_REG(0x040) = 0;
    COMP_REG(0x044) = PHYS(cb);
    COMP_REG(0x3AC) = 0x04004003;
    COMP_REG(0x0D4) = 1;

    { uint32_t v = COMP_REG(0x008); v &= ~0x100; COMP_REG(0x008) = v; }
}

static void lcd_passthrough_start(void)
{
    LCD_CON = LCD_MODE_P16;
    LCD_REG(0x88) = 0x01000000;
    LCD_REG(0x20) = 0x33;
    LCD_REG(0x7C) = 0x00000402;
    LCD_REG(0x78) = 0x000A000A;
    LCD_REG(0x74) = 0x00F00140;

    while (!(LCD_STATUS & 0x2));
    LCD_CON = LCD_MODE_P18;
    lcd_cmd(0x003); lcd_data(0x1238);
    lcd_cmd(0x210); lcd_data(0);
    lcd_cmd(0x211); lcd_data(319);
    lcd_cmd(0x212); lcd_data(0);
    lcd_cmd(0x213); lcd_data(239);
    lcd_cmd(0x200); lcd_data(0);
    lcd_cmd(0x201); lcd_data(0);
    lcd_cmd(0x202);
    while (!(LCD_STATUS & 0x2));
    LCD_CON = LCD_MODE_P16;

    LCD_REG(0x70) = 1;
    LCD_REG(0x80) = 0;
}

static void lcd_push_frame(void)
{
    { int t = 100000; while ((LCD_REG(0x8C) & 3) && --t > 0); }
    LCD_REG(0x80) = 1;

    LCD_CON = LCD_MODE_P18;
    lcd_cmd(0x003); lcd_data(0x1238);
    lcd_cmd(0x210); lcd_data(0);
    lcd_cmd(0x211); lcd_data(319);
    lcd_cmd(0x212); lcd_data(0);
    lcd_cmd(0x213); lcd_data(239);
    lcd_cmd(0x200); lcd_data(0);
    lcd_cmd(0x201); lcd_data(0);
    lcd_cmd(0x202);
    while (!(LCD_STATUS & 0x2));
    LCD_CON = LCD_MODE_P16;

    LCD_REG(0x80) = 0;
    { int t = 500000; while ((LCD_REG(0x8C) & 3) && --t > 0); }
}

enum plugin_status plugin_start(const void *parameter)
{
    const char *test_path = parameter ? (const char *)parameter
                                      : "/test_iframe.264";
    if (!*test_path) return PLUGIN_ERROR;

    rb->cpu_boost(true);
    rb->audio_stop();

    log_fd = rb->open("/vpu_vpp_test.log", O_WRONLY|O_CREAT|O_TRUNC, 0666);
    vlog("=== VPP Test v123 ===");
    vlog("File: %s", test_path);

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
    if (!got_frame || !y_out) {
        vlog("ERROR: no frame decoded");
        vpu_h264_close(dec);
        rb->close(log_fd);
        return PLUGIN_ERROR;
    }

    PWRCON(0) &= ~0x2080;
    for (volatile int d = 0; d < 10000; d++);

    uint32_t saved_lcd_con = LCD_CON;
    uint32_t saved_7c = LCD_REG(0x7C);
    uint32_t saved_88 = LCD_REG(0x88);
    uint32_t saved_20 = LCD_REG(0x20);
    uint32_t saved_74 = LCD_REG(0x74);
    uint32_t saved_78 = LCD_REG(0x78);

    compositor_init();
    compositor_configure_layer5(y_out, cb_out, cr_out, frame_w, frame_h);
    rb->commit_discard_dcache();

    /* Rockbox lcd_update resets LCD controller state — needed before passthrough */
    rb->lcd_update();

    lcd_passthrough_start();

    COMP_REG(0x000) = 0;
    { volatile int d = 0; while (d++ < 50000); }
    COMP_REG(0x000) = 1;
    { uint32_t t = USEC_TIMER; while ((USEC_TIMER - t) < 200000); }
    for (int i = 0; i < 10; i++) lcd_push_frame();

    vlog("  Display active — waiting for keypress");
    while (rb->button_get(true) == BUTTON_NONE)
        rb->backlight_on();

    LCD_REG(0x70) = 0;
    LCD_REG(0x80) = 0;
    COMP_REG(0x000) = 0;

    while (!(LCD_STATUS & 0x2));
    LCD_CON = LCD_MODE_P18;
    lcd_cmd(0x003); lcd_data(0x0230);
    while (!(LCD_STATUS & 0x2));
    LCD_CON = saved_lcd_con;

    LCD_REG(0x88) = saved_88;
    LCD_REG(0x20) = saved_20;
    LCD_REG(0x7C) = saved_7c;
    LCD_REG(0x74) = saved_74;
    LCD_REG(0x78) = saved_78;
    LCD_PHTIME = 0x33;

    { int t = 100000; while ((LCD_REG(0x8C) & 3) && --t > 0); }
    rb->lcd_update();

    vlog("=== Done ===");
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
