/* VPP DCS Test — verifies DCS firmware + compositor passthrough
 * Phase 1: Rockbox lcd_update test (verifies DCS firmware display)
 * Phase 2: Compositor passthrough with MADCTL cycling
 * Copyright (C) 2025 Nux Li */

#include "plugin.h"
#ifdef IPOD_6G
#include "s5l87xx.h"

#define vpu_h264_buf_size    rb->vpu_h264_buf_size
#define vpu_h264_open        rb->vpu_h264_open
#define vpu_h264_decode_nalu rb->vpu_h264_decode_nalu
#define vpu_h264_get_frame   rb->vpu_h264_get_frame
#define vpu_h264_close       rb->vpu_h264_close

#undef COMP
#define COMP  0x38900000
#define CR(o) (*(volatile uint32_t*)(COMP+(o)))
#define LR(o) (*(volatile uint32_t*)(LCD_BASE+(o)))
#define PH(x) ((uint32_t)((uintptr_t)(x)&0x7FFFFFFF))

static int log_fd = -1;
static void vlog(const char *fmt, ...) {
    if(log_fd<0)return;
    char buf[256];va_list ap;va_start(ap,fmt);
    int len=rb->vsnprintf(buf,sizeof(buf),fmt,ap);va_end(ap);
    rb->write(log_fd,buf,len);rb->write(log_fd,"\n",1);
}
static void lc(uint16_t c){while(LCD_STATUS&0x10);LCD_WCMD=c;}
static void ld(uint16_t d){while(LCD_STATUS&0x10);LCD_WDATA=d;}

static int fsc(const uint8_t*b,int l,int*s){
    for(int i=0;i<l-3;i++){if(b[i]==0&&b[i+1]==0){
        if(b[i+2]==1){*s=3;return i;}
        if(i+3<l&&b[i+2]==0&&b[i+3]==1){*s=4;return i;}}}return-1;}

static void dcs_set_madctl(uint8_t val)
{
    while(!(LCD_STATUS&0x2));
    LCD_CON=0x81000C20;
    lc(0x36);ld(val);
    while(!(LCD_STATUS&0x2));
    LCD_CON=0x80100DB0;
}

static void dcs_set_window(int portrait)
{
    while(!(LCD_STATUS&0x2));
    LCD_CON=0x81000C20;
    if (portrait) {
        lc(0x2A);ld(0x00);ld(0x00);ld(0x00);ld(0xEF);  /* CASET=0-239 */
        lc(0x2B);ld(0x00);ld(0x00);ld(0x01);ld(0x3F);  /* PASET=0-319 */
    } else {
        lc(0x2A);ld(0x00);ld(0x00);ld(0x01);ld(0x3F);  /* CASET=0-319 */
        lc(0x2B);ld(0x00);ld(0x00);ld(0x00);ld(0xEF);  /* PASET=0-239 */
    }
    lc(0x2C);  /* RAMWR */
    while(!(LCD_STATUS&0x2));
    LCD_CON=0x80100DB0;
}

static void comp_init(void) {
    volatile uint32_t*c=(volatile uint32_t*)COMP;
    c[0x200/4]&=~1;c[0x004/4]=1;c[0x020/4]=1;
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
    c[0x200/4]|=0x10080;c[0x204/4]=2;c[0x208/4]=0;c[0x20C/4]=2;
    c[0x210/4]=0x00010110;c[0x214/4]=0x00EF013F;c[0x024/4]=0x00FFFFFF;
}

static void comp_retrigger(void)
{
    CR(0x000)=0;
    {volatile int d=0;while(d++<50000);}
    CR(0x000)=1;
    {uint32_t t=USEC_TIMER;while((USEC_TIMER-t)<200000);}
}

enum plugin_status plugin_start(const void *parameter)
{
    const char*path=parameter?(const char*)parameter:"/test_iframe.264";
    if(!*path)return PLUGIN_ERROR;
    rb->cpu_boost(true);rb->audio_stop();

    log_fd=rb->open("/vpu_vpp_test.log",O_WRONLY|O_CREAT|O_TRUNC,0666);
    int panel_type = (PDAT(6)&0x30)>>4;
    vlog("=== VPP DCS Test v10 ===");
    vlog("Panel type: %d",panel_type);
    vlog("LCD state at plugin start:");
    vlog("  CON=0x%08lx STATUS=0x%08lx PHTIME=0x%08lx",
         (unsigned long)LCD_CON,(unsigned long)LCD_STATUS,(unsigned long)LCD_PHTIME);
    vlog("  +20=0x%08lx +70=0x%08lx +74=0x%08lx +78=0x%08lx +7C=0x%08lx +88=0x%08lx",
         (unsigned long)LR(0x20),(unsigned long)LR(0x70),(unsigned long)LR(0x74),
         (unsigned long)LR(0x78),(unsigned long)LR(0x7C),(unsigned long)LR(0x88));
    vlog("  PWRCON0=0x%08lx",(unsigned long)PWRCON(0));

    /* DCS readback diagnostics */
    {
        while(!(LCD_STATUS&0x2));
        LCD_CON=0x81000C20;
        /* Read Display Status (0x09) — 4 bytes */
        while(LCD_STATUS&0x10); LCD_WCMD=0x09;
        uint8_t status[4];
        for(int i=0;i<4;i++){
            while(!(LCD_STATUS&0x2)); *(volatile uint32_t*)(LCD_BASE+0x10)=0;
            while(!(LCD_STATUS&1)); status[i]=(uint8_t)(*(volatile uint32_t*)(LCD_BASE+0x14)>>1);
        }
        vlog("DCS 0x09 status: %02x %02x %02x %02x",status[0],status[1],status[2],status[3]);
        /* Read MADCTL (0x0B) — 1 byte */
        while(LCD_STATUS&0x10); LCD_WCMD=0x0B;
        while(!(LCD_STATUS&0x2)); *(volatile uint32_t*)(LCD_BASE+0x10)=0;
        while(!(LCD_STATUS&1));
        uint8_t madctl_rb=(uint8_t)(*(volatile uint32_t*)(LCD_BASE+0x14)>>1);
        vlog("DCS 0x0B MADCTL readback: 0x%02x (expect 0x60 landscape)",madctl_rb);
        /* Read COLMOD (0x0C) — 1 byte */
        while(LCD_STATUS&0x10); LCD_WCMD=0x0C;
        while(!(LCD_STATUS&0x2)); *(volatile uint32_t*)(LCD_BASE+0x10)=0;
        while(!(LCD_STATUS&1));
        uint8_t colmod_rb=(uint8_t)(*(volatile uint32_t*)(LCD_BASE+0x14)>>1);
        vlog("DCS 0x0C COLMOD readback: 0x%02x (expect 0x05 RGB565)",colmod_rb);
        while(!(LCD_STATUS&0x2));
        LCD_CON=0x80100DB0;
    }

    /* === PHASE 1: Verify DCS firmware display works === */
    vlog("Phase 1: Rockbox display test");
    rb->lcd_set_foreground(LCD_RGBPACK(255,0,0));
    rb->lcd_fillrect(0,0,106,240);
    rb->lcd_set_foreground(LCD_RGBPACK(0,255,0));
    rb->lcd_fillrect(106,0,108,240);
    rb->lcd_set_foreground(LCD_RGBPACK(0,0,255));
    rb->lcd_fillrect(214,0,106,240);
    rb->lcd_set_foreground(LCD_WHITE);
    rb->lcd_putsxy(10,10,"DCS firmware OK?");
    rb->lcd_putsxy(10,25,"MENU=compositor test");
    rb->lcd_putsxy(10,40,"SELECT=exit");
    rb->lcd_putsxy(10,55,"(R/G/B bars visible = DCS works)");
    rb->lcd_update();
    vlog("  RGB bars displayed via lcd_update");

    int btn;
    while(1) {
        btn=rb->button_get(true);
        if(btn==BUTTON_MENU) break;
        if(btn==BUTTON_SELECT) {
            rb->close(log_fd);rb->cpu_boost(false);
            return PLUGIN_OK;
        }
        rb->backlight_on();
    }

    /* === PHASE 2: Get YUV frame (decode or test pattern) === */
    vlog("Phase 2: Decode + compositor");
    uint8_t*ab;size_t as;
    ab=rb->plugin_get_audio_buffer(&as);

    int fw=0,fh=0;
    const uint8_t*yo=NULL,*cbo=NULL,*cro=NULL;
    uint8_t *test_y=NULL, *test_cb=NULL, *test_cr=NULL;

    int fd=rb->open(path,O_RDONLY);
    if(fd>=0) {
        size_t ds=vpu_h264_buf_size(640,480);
        struct vpu_h264*dec=vpu_h264_open(ab,ds,640,480);
        if(!dec){vlog("ERROR: vpu_h264_open");rb->close(fd);rb->close(log_fd);return PLUGIN_ERROR;}
        uint8_t*fb=ab+ds;
        int fl=rb->read(fd,fb,320*240*2);rb->close(fd);
        vlog("  Read %d bytes from %s",fl,path);

        int pos=0,nalu_count=0;
        while(pos<fl-4){
            int sl,sp=fsc(fb+pos,fl-pos,&sl);if(sp<0)break;
            int ns=pos+sp+sl,nx=fsc(fb+ns,fl-ns,&sl),nl=(nx>=0)?nx:fl-ns;
            nalu_count++;
            int ret=vpu_h264_decode_nalu(dec,fb+ns,nl);
            vlog("  NALU %d: off=%d len=%d ret=%d",nalu_count,ns,nl,ret);
            if(ret==1)
                vpu_h264_get_frame(dec,&yo,&cbo,&cro,&fw,&fh);
            pos=ns+nl;
        }
        if(!yo) {
            vlog("  No decoded frame, using test pattern");
            vpu_h264_close(dec);
        }
    } else {
        vlog("  No %s, using test pattern",path);
    }

    if(!yo) {
        fw=320; fh=240;
        test_y  = ab;
        test_cb = test_y + fw*fh;
        test_cr = test_cb + (fw/2)*(fh/2);
        /* Red/Green/Blue vertical bars in YCbCr (BT.601) */
        for(int row=0;row<fh;row++) {
            for(int col=0;col<fw;col++) {
                int i = row*fw+col;
                if(col<fw/3)       test_y[i]=81;   /* red: Y=81 */
                else if(col<2*fw/3) test_y[i]=145;  /* green: Y=145 */
                else               test_y[i]=41;    /* blue: Y=41 */
            }
        }
        for(int row=0;row<fh/2;row++) {
            for(int col=0;col<fw/2;col++) {
                int i = row*(fw/2)+col;
                if(col<fw/6)        { test_cb[i]=90;  test_cr[i]=240; }  /* red */
                else if(col<2*fw/6) { test_cb[i]=54;  test_cr[i]=34;  }  /* green */
                else                { test_cb[i]=240; test_cr[i]=110; }  /* blue */
            }
        }
        yo=test_y; cbo=test_cb; cro=test_cr;
        vlog("  Test pattern: %dx%d RGB bars in YCbCr",fw,fh);
    } else {
        vlog("  Frame: %dx%d Y=0x%08lx Cb=0x%08lx Cr=0x%08lx",
             fw,fh,(unsigned long)yo,(unsigned long)cbo,(unsigned long)cro);
    }

    /* Save LCD state */
    uint32_t sc=LCD_CON,s7=LR(0x7C),s8=LR(0x88),s2=LR(0x20),s4=LR(0x74),s5=LR(0x78);
    vlog("  Saved LCD: CON=0x%08lx 70=0x%08lx 74=0x%08lx 78=0x%08lx 7C=0x%08lx 88=0x%08lx",
         (unsigned long)sc,(unsigned long)LR(0x70),(unsigned long)s4,
         (unsigned long)s5,(unsigned long)s7,(unsigned long)s8);

    /* Enable compositor clock */
    PWRCON(0)&=~0x2080;
    for(volatile int d=0;d<10000;d++);

    comp_init();
    vlog("  Compositor initialized");

    /* Layer 5 — Apple defaults */
    for(int o=0x024;o<=0x044;o+=4)CR(o)=0;
    for(int o=0x04C;o<=0x058;o+=4)CR(o)=0;
    CR(0x028)=0x100;CR(0x02C)=fw|((fw/2)<<16);
    CR(0x034)=fh|((uint32_t)fw<<16);
    CR(0x04C)=0x10001000;CR(0x054)=0x00F00140;
    CR(0x038)=PH(yo);CR(0x03C)=PH(cro);CR(0x040)=0;CR(0x044)=PH(cbo);
    CR(0x3AC)=0x04004003;  /* rotation ON */
    CR(0x0D4)=1;
    {uint32_t v=CR(0x008);v&=~0x100;CR(0x008)=v;}
    rb->commit_discard_dcache();
    vlog("  Layer 5: Y=0x%08lx Cb=0x%08lx Cr=0x%08lx stride=%d dim=%dx%d",
         (unsigned long)PH(yo),(unsigned long)PH(cbo),(unsigned long)PH(cro),fw,fw,fh);

    /* LCD passthrough mode */
    LCD_CON=0x80100DB0;  /* P16 frame mode */
    LR(0x88)=0x01000000;LR(0x20)=0x33;LR(0x7C)=0x00000402;
    LR(0x78)=0x000A000A;

    /* Set LCD+0x74 BEFORE enabling passthrough (it latches at 0x70=1 transition) */
    LR(0x74)=0x00F00140;
    vlog("  LCD passthrough registers set");

    /* Set DCS portrait window for compositor (240/scan matches portrait).
     * Start with MADCTL=0x40 (MX): Rockbox landscape uses 0x60 (MV+MX),
     * portrait = landscape minus MV = 0x40. Apple ROM confirms 0x40 wrapper. */
    dcs_set_madctl(0x40);
    dcs_set_window(1);  /* portrait: CASET=0-239, PASET=0-319 */
    vlog("  DCS portrait MADCTL=0x40 (MX), window 240x320");

    /* Enable passthrough */
    LR(0x70)=1;LR(0x80)=0;

    /* Trigger compositor */
    comp_retrigger();
    vlog("  Compositor running");

    /* MADCTL cycling table — 0x40 first (predicted correct from landscape 0x60) */
    static const uint8_t madctl_vals[] = {
        0x40,  /* portrait: MX (predicted correct — landscape 0x60 minus MV) */
        0x00,  /* portrait: no flags */
        0xC0,  /* portrait: MY+MX (both mirrored) */
        0x80,  /* portrait: MY (row mirrored) */
        0x20,  /* landscape: MV */
        0x60,  /* landscape: MV+MX */
        0xA0,  /* landscape: MV+MY */
        0xE0,  /* landscape: MV+MX+MY */
    };
    int madctl_idx = 0;
    int is_portrait = 1;
    vlog("  Controls: LEFT/RIGHT=cycle MADCTL, PLAY=toggle port/land, SELECT=exit");

    while(1) {
        btn=rb->button_get(true);
        if(btn==BUTTON_SELECT) break;

        if(btn==BUTTON_RIGHT) {
            madctl_idx = (madctl_idx + 1) % 8;
            uint8_t m = madctl_vals[madctl_idx];
            is_portrait = !(m & 0x20);
            LR(0x70)=0;
            dcs_set_madctl(m);
            dcs_set_window(is_portrait);
            LR(0x70)=1;LR(0x80)=0;
            vlog("  MADCTL=0x%02x (%s) idx=%d",m,is_portrait?"portrait":"landscape",madctl_idx);
        }
        if(btn==BUTTON_LEFT) {
            madctl_idx = (madctl_idx + 7) % 8;
            uint8_t m = madctl_vals[madctl_idx];
            is_portrait = !(m & 0x20);
            LR(0x70)=0;
            dcs_set_madctl(m);
            dcs_set_window(is_portrait);
            LR(0x70)=1;LR(0x80)=0;
            vlog("  MADCTL=0x%02x (%s) idx=%d",m,is_portrait?"portrait":"landscape",madctl_idx);
        }
        if(btn==BUTTON_PLAY) {
            /* Reset GRAM position (Apple's per-frame method) */
            {int t=100000;while((LR(0x8C)&3)&&--t>0);}
            LR(0x80)=1;
            dcs_set_window(is_portrait);
            LR(0x80)=0;
            vlog("  GRAM reset");
        }
        rb->backlight_on();
    }

    /* === SHUTDOWN === */
    vlog("Shutdown");
    LR(0x70)=0;LR(0x80)=0;CR(0x000)=0;

    /* Restore landscape MADCTL for Rockbox UI */
    dcs_set_madctl(0x60);
    /* Restore LCD state */
    while(!(LCD_STATUS&0x2));
    LCD_CON=sc;
    LR(0x88)=s8;LR(0x20)=s2;LR(0x7C)=s7;LR(0x74)=s4;LR(0x78)=s5;
    LCD_PHTIME=0x33;
    {int t=100000;while((LR(0x8C)&3)&&--t>0);}
    rb->lcd_update();

    vlog("Done — restored landscape");
    rb->close(log_fd);
    rb->cpu_boost(false);
    return PLUGIN_OK;
}
#else
enum plugin_status plugin_start(const void*p){(void)p;rb->splash(HZ*2,"iPod 6G only");return PLUGIN_ERROR;}
#endif
