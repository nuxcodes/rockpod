/* v147 — DCS DRIVER POC
 * Tests switching ILI9326 panel to DCS mode at runtime.
 * SAFETY: saves all LCD state, restores on exit.
 * If display goes blank, just wait — it auto-restores on exit.
 *
 * Approach:
 * 1. Save current Rockbox LCD state
 * 2. Switch LCD controller to P8 with bit 24 (Apple's DCS mode)
 * 3. Send DCS init: Sleep Out, COLMOD, MADCTL, Normal Mode, Display On
 * 4. Send DCS CASET/PASET for portrait window (240×320)
 * 5. Enable compositor passthrough (Apple defaults, rotation ON)
 * 6. Compositor pushes 240/scan portrait → DCS portrait panel
 * 7. On exit: restore ILI9326 state via Rockbox lcd_update
 *
 * Copyright (C) 2025 Nux Li */

#include "plugin.h"
#ifdef IPOD_6G
#include "s5l87xx.h"

#define vpu_h264_buf_size    rb->vpu_h264_buf_size
#define vpu_h264_open        rb->vpu_h264_open
#define vpu_h264_decode_nalu rb->vpu_h264_decode_nalu
#define vpu_h264_get_frame   rb->vpu_h264_get_frame
#define vpu_h264_close       rb->vpu_h264_close

#define COMP  0x38900000
#define CR(o) (*(volatile uint32_t*)(COMP+(o)))
#define LR(o) (*(volatile uint32_t*)(LCD_BASE+(o)))
#define PH(x) ((uint32_t)((uintptr_t)(x)&0x7FFFFFFF))

#define P8_DCS  0x81000C21  /* P8 with bit 24 — Apple's DCS command mode */
#define P16     0x80100DB0  /* P16 — pixel data mode for PAR18 panels */
#define P18     0x80000DA9  /* P18 — ILI9326 register command mode */

static int log_fd = -1;
static void vlog(const char *fmt, ...) {
    if (log_fd < 0) return;
    char buf[256]; va_list ap;
    va_start(ap, fmt);
    int len = rb->vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    rb->write(log_fd, buf, len); rb->write(log_fd, "\n", 1);
}

static void lc(uint16_t c){while(LCD_STATUS&0x10);LCD_WCMD=c;}
static void ld(uint16_t d){while(LCD_STATUS&0x10);LCD_WDATA=d;}
static int fsc(const uint8_t*b,int l,int*s){
    for(int i=0;i<l-3;i++){if(b[i]==0&&b[i+1]==0){
        if(b[i+2]==1){*s=3;return i;}
        if(i+3<l&&b[i+2]==0&&b[i+3]==1){*s=4;return i;}}}return-1;}

static void dcs_cmd(uint8_t cmd, int ndata, const uint8_t *data)
{
    int t;
    t=100000; while(!(LCD_STATUS&0x2)&&--t>0);
    if(t<=0){vlog("  DCS HANG at pre-wait cmd=0x%02x",cmd);return;}
    LCD_CON = P8_DCS;
    lc(cmd);
    for (int i = 0; i < ndata; i++)
        ld(data[i]);
    t=100000; while(!(LCD_STATUS&0x2)&&--t>0);
    if(t<=0){vlog("  DCS HANG at post-wait cmd=0x%02x",cmd);return;}
    vlog("  DCS cmd 0x%02x OK (%d data)", cmd, ndata);
}

static void comp_init(void) {
    volatile uint32_t*c=(volatile uint32_t*)COMP;
    c[0x200/4]&=~1; c[0x004/4]=1; c[0x020/4]=1;
    for(int i=0;i<256;i++){c[0x400/4+i]=i*4;c[0x800/4+i]=i*4;c[0xC00/4+i]=i*4;}
    {volatile uint32_t*s=(volatile uint32_t*)0x0890D2DC;
     uint32_t t[5];for(int i=0;i<5;i++)t[i]=s[i];
     if(t[0]>0&&t[0]<0x1000&&t[4]>0&&t[4]<0x1000)
         for(int i=0;i<5;i++)c[(0x1EC+i*4)/4]=t[i];
     else{uint32_t h[]={0x0C,0x26,0x10,0x82,0x4E};for(int i=0;i<5;i++)c[(0x1EC+i*4)/4]=h[i];}}
    c[0x0D8/4]=0x1000;c[0x0DC/4]=0;c[0x0E0/4]=0x1000;c[0x0E4/4]=0;c[0x0E8/4]=0x1000;c[0x0EC/4]=0;
    {uint32_t v=c[0x008/4];v&=~0x03000000;v|=0x01000000;v&=~0x00300000;v|=0x00100000;
     v&=~0x00030000;v|=0x00010000;v&=~1;v|=1;c[0x008/4]=v;}
    c[0x00C/4]=0x000F0F0F;
    {uint32_t v=c[0x008/4];v|=0x8000;c[0x008/4]=v;}
    {uint32_t v=c[0x008/4];v&=~2;c[0x008/4]=v;}
    {uint32_t v=c[0x008/4];v|=0x100;c[0x008/4]=v;}
    {uint32_t v=c[0x008/4];v|=0x80;c[0x008/4]=v;}
    {uint32_t v=c[0x008/4];v|=0x40000000;c[0x008/4]=v;}
    c[0x200/4]|=0x10080; c[0x204/4]=2; c[0x208/4]=0; c[0x20C/4]=2;
    c[0x210/4]=0x00010110;
    c[0x214/4]=0x00EF013F;  /* Apple default portrait viewport */
    c[0x024/4]=0x00FFFFFF;
}

enum plugin_status plugin_start(const void *parameter)
{
    const char*path=parameter?(const char*)parameter:"/test_iframe.264";
    if(!*path)return PLUGIN_ERROR;
    rb->cpu_boost(true); rb->audio_stop();

    log_fd = rb->open("/vpu_vpp_test.log", O_WRONLY|O_CREAT|O_TRUNC, 0666);
    vlog("=== VPP v147 — DCS DRIVER POC ===");

    uint8_t*ab;size_t as;
    ab=rb->plugin_get_audio_buffer(&as);
    size_t ds=vpu_h264_buf_size(640,480);
    struct vpu_h264*dec=vpu_h264_open(ab,ds,640,480);
    if(!dec){rb->close(log_fd);return PLUGIN_ERROR;}
    uint8_t*fb=ab+ds;
    int fd=rb->open(path,O_RDONLY);if(fd<0){rb->close(log_fd);return PLUGIN_ERROR;}
    int fl=rb->read(fd,fb,320*240*2);rb->close(fd);

    int fw=0,fh=0;
    const uint8_t*yo=NULL,*cbo=NULL,*cro=NULL;
    int pos=0;
    while(pos<fl-4){
        int sl,sp=fsc(fb+pos,fl-pos,&sl);if(sp<0)break;
        int ns=pos+sp+sl,nx=fsc(fb+ns,fl-ns,&sl),nl=(nx>=0)?nx:fl-ns;
        if(vpu_h264_decode_nalu(dec,fb+ns,nl)==1)
            vpu_h264_get_frame(dec,&yo,&cbo,&cro,&fw,&fh);
        pos=ns+nl;
    }
    if(!yo){vpu_h264_close(dec);rb->close(log_fd);return PLUGIN_ERROR;}
    vlog("Decoded %dx%d", fw, fh);

    /* === SAVE ALL LCD STATE === */
    uint32_t sc=LCD_CON,s7=LR(0x7C),s8=LR(0x88),s2=LR(0x20),s4=LR(0x74),s5=LR(0x78);
    vlog("Saved LCD state: CON=%08lx", (unsigned long)sc);

    PWRCON(0)&=~0x2080;
    for(volatile int d=0;d<10000;d++);

    /* === COMPOSITOR INIT (ALL APPLE DEFAULTS) === */
    comp_init();

    /* Layer 5 — Apple defaults with rotation ON */
    for(int o=0x024;o<=0x044;o+=4)CR(o)=0;
    for(int o=0x04C;o<=0x058;o+=4)CR(o)=0;
    CR(0x028)=0x100; CR(0x02C)=fw|((fw/2)<<16);
    CR(0x034)=fh|((uint32_t)fw<<16);
    CR(0x04C)=0x10001000;
    CR(0x054)=0x00F00140;  /* Apple default portrait output */
    CR(0x038)=PH(yo); CR(0x03C)=PH(cro); CR(0x040)=0; CR(0x044)=PH(cbo);
    CR(0x3AC)=0x04004003;  /* Apple default rotation ON */
    CR(0x0D4)=1;
    {uint32_t v=CR(0x008);v&=~0x100;CR(0x008)=v;}  /* CSC enable */
    rb->commit_discard_dcache();

    /* === SWITCH PANEL TO DCS MODE === */
    vlog("Hardware reset + DCS init...");

    /* Hardware reset — forces panel back to default (DCS-receptive) state */
    LCD_RST_TIME = 0x7FFF;  /* set reset pulse duration */
    LCD_DRV_RST = 0;  /* assert reset (active LOW) */
    {uint32_t t=USEC_TIMER;while((USEC_TIMER-t)<50000);}  /* 50ms */
    LCD_DRV_RST = 1;  /* release reset */
    {uint32_t t=USEC_TIMER;while((USEC_TIMER-t)<150000);}  /* 150ms recovery */
    vlog("Panel reset done. LCD_STATUS=0x%08lx", (unsigned long)LCD_STATUS);

    /* DCS Software Reset (extra safety) */
    {uint8_t d[]={};dcs_cmd(0x01,0,d);}
    {uint32_t t=USEC_TIMER;while((USEC_TIMER-t)<120000);}

    /* DCS Sleep Out */
    {uint8_t d[]={};dcs_cmd(0x11,0,d);}
    {uint32_t t=USEC_TIMER;while((USEC_TIMER-t)<120000);}  /* 120ms delay */

    /* DCS Pixel Format = RGB565 (16-bit) */
    {uint8_t d[]={0x05};dcs_cmd(0x3A,1,d);}

    /* DCS MADCTL = 0x00 (normal scan, RGB order) */
    {uint8_t d[]={0x00};dcs_cmd(0x36,1,d);}

    /* DCS Normal Mode On */
    {uint8_t d[]={};dcs_cmd(0x13,0,d);}

    /* DCS Display On */
    {uint8_t d[]={};dcs_cmd(0x29,0,d);}

    vlog("DCS init sent");

    /* === LCD PASSTHROUGH SETUP — TRY P9 (Apple's DCS pixel mode) === */
    /* After DCS init, the panel is in DCS mode. Apple uses P9 for pixel data.
     * P9 with COLMOD=0x05 (16-bit) = 2 × 9-bit transfers per RGB565 pixel.
     * Try BOTH P9 and COLMOD=0x06 (18-bit, Apple's default). */
    LCD_CON=0x81100DB9;  /* P9 mode — Apple's exact passthrough mode */
    LR(0x88)=0x01000000; LR(0x20)=0x33; LR(0x7C)=0x00000402;
    LR(0x78)=0x000A000A;
    LR(0x74)=0x00F00140;  /* Apple default: 240/scan portrait */

    /* Set COLMOD to 18-bit (Apple's default for DCS panels) */
    {uint8_t d[]={0x06};dcs_cmd(0x3A,1,d);}
    vlog("Set COLMOD=0x06 (18-bit) for P9 mode");

    /* DCS GRAM window setup via P8 */
    {uint8_t caset[]={0x00,0x00,0x00,0xEF};dcs_cmd(0x2A,4,caset);}  /* CASET=0-239 */
    {uint8_t paset[]={0x00,0x00,0x01,0x3F};dcs_cmd(0x2B,4,paset);}  /* PASET=0-319 */
    {uint8_t d[]={};dcs_cmd(0x2C,0,d);}  /* RAMWR */

    /* Switch to P9 for pixel data, enable passthrough */
    while(!(LCD_STATUS&0x2));
    LCD_CON=0x81100DB9;  /* P9 — Apple's exact passthrough mode */
    LR(0x70)=1;  /* Enable passthrough */
    LR(0x80)=0;

    vlog("Passthrough enabled");

    /* === START COMPOSITOR === */
    CR(0x000)=0;{volatile int d=0;while(d++<50000);}
    CR(0x000)=1;
    {uint32_t t=USEC_TIMER;while((USEC_TIMER-t)<500000);}

    vlog("Compositor running — waiting for keypress");

    /* Wait for keypress */
    while(rb->button_get(true)==BUTTON_NONE) rb->backlight_on();

    /* === SAFE SHUTDOWN — RESTORE ILI9326 STATE === */
    vlog("Shutting down...");
    LR(0x70)=0; LR(0x80)=0; CR(0x000)=0;

    /* Restore all saved LCD registers */
    LCD_CON=sc;
    LR(0x88)=s8; LR(0x20)=s2; LR(0x7C)=s7; LR(0x74)=s4; LR(0x78)=s5;
    LCD_PHTIME=0x33;

    /* Force ILI9326 re-init by doing Rockbox lcd_update */
    {int t=100000;while((LR(0x8C)&3)&&--t>0);}
    rb->lcd_update();

    vlog("Restored. Done.");
    rb->close(log_fd);
    vpu_h264_close(dec); rb->cpu_boost(false);
    return PLUGIN_OK;
}
#else
enum plugin_status plugin_start(const void*p){(void)p;rb->splash(HZ*2,"iPod 6G only");return PLUGIN_ERROR;}
#endif
