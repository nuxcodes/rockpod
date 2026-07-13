/* VPP ILI9326 Test -- P9 vs P16 passthrough experiment
 * Config 0 (default): P16 passthrough (0x80100DB0), landscape, no rotation
 * Config 1 (MENU):    P9 Apple passthrough (0x81100DB9), Apple regs, rotation ON
 * Tests whether passthrough mechanism is P9-only.
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

static int fsc(const uint8_t*b,int l,int*s){
    for(int i=0;i<l-3;i++){if(b[i]==0&&b[i+1]==0){
        if(b[i+2]==1){*s=3;return i;}
        if(i+3<l&&b[i+2]==0&&b[i+3]==1){*s=4;return i;}}}return-1;}

static void ili_cmd(uint16_t c){while(LCD_STATUS&0x10);LCD_WCMD=c;}
static void ili_data(uint16_t d){while(LCD_STATUS&0x10);LCD_WDATA=d;}

/* Active passthrough mode: 0 = P16 (0x80100DB0), 1 = P9 Apple (0x81100DB9) */
static int pt_mode = 0;

static uint32_t get_pixel_con(void)
{
    return pt_mode ? 0x81100DB9 : 0x80100DB0;
}

static void ili_set_entry_mode(uint16_t val)
{
    while(!(LCD_STATUS&0x2)); LCD_CON=0x80000DA9;  /* P18 for ILI9326 cmds */
    ili_cmd(0x003);ili_data(val);
    while(!(LCD_STATUS&0x2)); LCD_CON=get_pixel_con();
}

static void ili_set_gram_window(void)
{
    while(!(LCD_STATUS&0x2)); LCD_CON=0x80000DA9;  /* P18 for ILI9326 cmds */
    ili_cmd(0x210);ili_data(0);      /* H start = 0 */
    ili_cmd(0x211);ili_data(319);    /* H end = 319 */
    ili_cmd(0x212);ili_data(0);      /* V start = 0 */
    ili_cmd(0x213);ili_data(239);    /* V end = 239 */
    ili_cmd(0x200);ili_data(0);      /* GRAM x = 0 */
    ili_cmd(0x201);ili_data(0);      /* GRAM y = 0 */
    ili_cmd(0x202);                  /* Write to GRAM */
    while(!(LCD_STATUS&0x2)); LCD_CON=get_pixel_con();
}

/* push_one_frame: LCD+0x80 bracket
 * CPU takes bus, sends ILI9326 Entry Mode + GRAM window via P18,
 * then releases bus so compositor pushes pixels.
 * Pixel data phase uses P16 or P9 depending on pt_mode. */
static void push_one_frame(void)
{
    { int t = 100000; while ((LR(0x8C) & 3) && --t > 0); }

    LR(0x80) = 1;  /* CPU takes bus */

    while(!(LCD_STATUS&0x2)); LCD_CON = 0x80000DA9;  /* P18 for ILI9326 */
    ili_cmd(0x003); ili_data(0x1238);  /* AM=1, I/D=11, BGR=1 — fixes shearing */
    ili_cmd(0x210); ili_data(0);
    ili_cmd(0x211); ili_data(319);    /* HE=319 */
    ili_cmd(0x212); ili_data(0);
    ili_cmd(0x213); ili_data(239);    /* VE=239 */
    ili_cmd(0x200); ili_data(0);
    ili_cmd(0x201); ili_data(0);
    ili_cmd(0x202);
    while(!(LCD_STATUS&0x2)); LCD_CON = get_pixel_con();

    LR(0x80) = 0;  /* release bus to compositor */

    { int t = 500000; while ((LR(0x8C) & 3) && --t > 0); }
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
    {uint32_t v=c[0x008/4];v&=~0x20000000;v&=~0x10000000;
     v&=~0x03000000;v|=0x01000000;v&=~0x00300000;v|=0x00100000;
     v&=~0x00030000;v|=0x00010000;v&=~1;v|=1;c[0x008/4]=v;}
    c[0x00C/4]=0x000F0F0F;
    {uint32_t v=c[0x008/4];v|=0x8000;c[0x008/4]=v;}
    {uint32_t v=c[0x008/4];v&=~2;c[0x008/4]=v;}
    {uint32_t v=c[0x008/4];v|=0x100;c[0x008/4]=v;}
    {uint32_t v=c[0x008/4];v|=0x80;c[0x008/4]=v;}
    {uint32_t v=c[0x008/4];v|=0x40000000;c[0x008/4]=v;}
    c[0x200/4]|=0x10080;c[0x204/4]=2;c[0x208/4]=0;c[0x20C/4]=2;
    c[0x210/4]=0x00010110;c[0x214/4]=0x013F00EF;c[0x024/4]=0x00FFFFFF;
}


enum plugin_status plugin_start(const void *parameter)
{
    const char*path=parameter?(const char*)parameter:"/test_iframe.264";
    if(!*path)return PLUGIN_ERROR;
    rb->cpu_boost(true);rb->audio_stop();

    log_fd=rb->open("/vpu_vpp_ili.log",O_WRONLY|O_CREAT|O_TRUNC,0666);
    vlog("=== VPP P9 vs P16 Passthrough Test ===");
    vlog("Panel type: %d",(PDAT(6)&0x30)>>4);

    uint8_t*ab;size_t as;
    ab=rb->plugin_get_audio_buffer(&as);
    int fw=0,fh=0;
    const uint8_t*yo=NULL,*cbo=NULL,*cro=NULL;
    struct vpu_h264*dec=NULL;

    int fd=rb->open(path,O_RDONLY);
    if(fd>=0) {
        size_t ds=vpu_h264_buf_size(640,480);
        dec=vpu_h264_open(ab,ds,640,480);
        if(!dec){vlog("ERROR: decoder");rb->close(fd);rb->close(log_fd);return PLUGIN_ERROR;}
        uint8_t*fb=ab+ds;
        int fl=rb->read(fd,fb,320*240*2);rb->close(fd);
        int pos=0;
        while(pos<fl-4){
            int sl,sp=fsc(fb+pos,fl-pos,&sl);if(sp<0)break;
            int ns=pos+sp+sl,nx=fsc(fb+ns,fl-ns,&sl),nl=(nx>=0)?nx:fl-ns;
            if(vpu_h264_decode_nalu(dec,fb+ns,nl)==1)
                vpu_h264_get_frame(dec,&yo,&cbo,&cro,&fw,&fh);
            pos=ns+nl;
        }
        if(!yo) { vlog("No frame decoded, test pattern"); vpu_h264_close(dec); dec=NULL; }
        else vlog("Decoded %dx%d",fw,fh);
    } else {
        vlog("No %s, test pattern",path);
    }

    if(!yo) {
        fw=320; fh=240;
        uint8_t *ty=ab, *tcb=ab+fw*fh, *tcr=tcb+(fw/2)*(fh/2);
        for(int r=0;r<fh;r++) for(int c=0;c<fw;c++) {
            int i=r*fw+c;
            if(c<fw/3) ty[i]=81; else if(c<2*fw/3) ty[i]=145; else ty[i]=41;
        }
        for(int r=0;r<fh/2;r++) for(int c=0;c<fw/2;c++) {
            int i=r*(fw/2)+c;
            if(c<fw/6)       { tcb[i]=90; tcr[i]=240; }
            else if(c<fw/3)  { tcb[i]=54; tcr[i]=34;  }
            else             { tcb[i]=240;tcr[i]=110; }
        }
        yo=ty; cbo=tcb; cro=tcr;
        vlog("Test pattern: %dx%d RGB bars",fw,fh);
    }

    uint32_t sc=LCD_CON,s7=LR(0x7C),s8=LR(0x88),s2=LR(0x20),s4=LR(0x74),s5=LR(0x78);

    PWRCON(0)&=~0x2080;
    for(volatile int d=0;d<10000;d++);

    comp_init();

    /* Layer 5 -- YUV planes */
    for(int o=0x024;o<=0x044;o+=4)CR(o)=0;
    for(int o=0x04C;o<=0x058;o+=4)CR(o)=0;
    CR(0x028)=0x100;CR(0x02C)=fw|((fw/2)<<16);
    CR(0x034)=fh|((uint32_t)fw<<16);
    CR(0x04C)=0x10001000;
    CR(0x054)=0x014000F0;  /* Config 0 default: landscape 320x240 */
    CR(0x038)=PH(yo);CR(0x03C)=PH(cro);CR(0x040)=0;CR(0x044)=PH(cbo);
    CR(0x3AC)=0;           /* Config 0 default: rotation OFF */
    CR(0x0D4)=1;
    {uint32_t v=CR(0x008);v&=~0x100;CR(0x008)=v;}
    rb->commit_discard_dcache();

    /* LCD passthrough -- Config 0 (P16) defaults */
    pt_mode = 0;
    LCD_CON=0x80100DB0;    /* P16 mode */
    LR(0x88)=0x01000000;LR(0x20)=0x33;
    LR(0x7C)=0x00000402;
    LR(0x78)=0x000A000A;
    LR(0x74)=0x014000F0;  /* landscape: 320/scan, 240 scans */
    vlog("Config 0 (P16): CON=0x%08lx 74=0x%08lx 7C=0x%08lx",
         (unsigned long)LCD_CON,(unsigned long)LR(0x74),
         (unsigned long)LR(0x7C));

    /* Entry Mode cycling -- ALL must have AM=1 (bit 3)
     * Bit layout: bit12=BGR, bit5=I/D1, bit4=I/D0, bit3=AM */
    static const uint16_t em_vals[] = {
        0x0038,  /* AM=1, I/D=11, BGR=0 */
        0x0028,  /* AM=1, I/D=10, BGR=0 */
        0x0018,  /* AM=1, I/D=01, BGR=0 */
        0x0008,  /* AM=1, I/D=00, BGR=0 */
        0x1038,  /* AM=1, I/D=11, BGR=1 */
        0x1028,  /* AM=1, I/D=10, BGR=1 */
        0x1018,  /* AM=1, I/D=01, BGR=1 */
        0x1008,  /* AM=1, I/D=00, BGR=1 */
    };
    int em_idx = 0;

    /* Compositor configuration table */
    struct comp_cfg {
        uint32_t comp_054;   /* compositor layer size */
        uint32_t lcd_074;    /* LCD passthrough size */
        uint32_t rotation;   /* 0 = identity, 0x04004003 = Apple rotation */
        uint32_t comp_214;   /* compositor output size */
        const char *desc;
    };
    static const struct comp_cfg cfgs[] = {
        { 0x014000F0, 0x014000F0, 0,          0x013F00EF,
          "cfg0: landscape, no rot" },
        { 0x00F00140, 0x00F00140, 0x04004003, 0x00EF013F,
          "cfg1: Apple + rot ON" },
        { 0x014000F0, 0x014000F0, 0x04004003, 0x013F00EF,
          "cfg2: landscape + rot ON" },
        { 0x00F00140, 0x014000F0, 0,          0x013F00EF,
          "cfg3: comp portrait, LCD landscape" },
        { 0x014000F0, 0x00F00140, 0x04004003, 0x00EF013F,
          "cfg4: comp landscape, LCD portrait + rot" },
        { 0x00F00140, 0x00F00140, 0,          0x00EF013F,
          "cfg5: Apple no rot" },
    };
    int cfg_idx = 0;
    int lcd_7c_val = 0x402;

    ili_set_entry_mode(em_vals[em_idx]);
    ili_set_gram_window();
    vlog("Entry Mode: 0x%04x",em_vals[em_idx]);

    /* GO first, then passthrough enable */
    CR(0x000)=0;{volatile int d=0;while(d++<50000);}
    CR(0x000)=1;{uint32_t t=USEC_TIMER;while((USEC_TIMER-t)<200000);}
    LR(0x70)=1;LR(0x80)=0;

    for(int i=0;i<10;i++) push_one_frame();
    vlog("Compositor GO=1, passthrough on, 10 frames pushed");
    vlog("MENU=P9/P16, RIGHT=cfg, LEFT=entry, PLAY=7C, SELECT=exit");

    int btn;
    while(1) {
        btn=rb->button_get(true);
        if(btn==BUTTON_SELECT) break;

        /* MENU -- toggle P9 Apple (Config 1) vs P16 (Config 0) */
        if(btn==BUTTON_MENU) {
            pt_mode = !pt_mode;
            LR(0x70)=0;  /* disable passthrough */

            if(pt_mode) {
                /* Config 1: P9 Apple passthrough */
                LCD_CON = 0x81100DB9;             /* P9 mode */
                LR(0x74) = 0x00F00140;            /* Apple portrait: 240x320 */
                CR(0x054) = 0x00F00140;            /* comp layer: portrait */
                CR(0x3AC) = 0x04004003;            /* rotation ON */
                CR(0x214) = 0x00EF013F;            /* comp output: portrait */
                /* Apple rotation matrix */
                CR(0x0D8)=0x0400; CR(0x0DC)=0x4003;
                CR(0x0E0)=0x0400; CR(0x0E4)=0x4003;
                CR(0x0E8)=0x0400; CR(0x0EC)=0x4003;
                vlog("=== Config 1: P9 Apple mode ===");
                vlog("  LCD_CON=0x81100DB9 LCD+074=0x00F00140");
                vlog("  comp+054=0x00F00140 comp+3AC=0x04004003");
                vlog("  comp+214=0x00EF013F rotation=ON");
            } else {
                /* Config 0: P16 passthrough (original) */
                LCD_CON = 0x80100DB0;             /* P16 mode */
                LR(0x74) = 0x014000F0;            /* landscape: 320x240 */
                CR(0x054) = 0x014000F0;            /* comp layer: landscape */
                CR(0x3AC) = 0;                     /* rotation OFF */
                CR(0x214) = 0x013F00EF;            /* comp output: landscape */
                /* Identity rotation matrix */
                CR(0x0D8)=0x1000; CR(0x0DC)=0;
                CR(0x0E0)=0x1000; CR(0x0E4)=0;
                CR(0x0E8)=0x1000; CR(0x0EC)=0;
                vlog("=== Config 0: P16 mode (original) ===");
                vlog("  LCD_CON=0x80100DB0 LCD+074=0x014000F0");
                vlog("  comp+054=0x014000F0 comp+3AC=0x00000000");
                vlog("  comp+214=0x013F00EF rotation=OFF");
            }

            LR(0x70)=1; LR(0x80)=0;  /* re-enable passthrough */
            for(int i=0;i<10;i++) push_one_frame();
            vlog("  readback: LCD_CON=0x%08lx comp+054=0x%08lx LCD+074=0x%08lx",
                 (unsigned long)LCD_CON, (unsigned long)CR(0x054),
                 (unsigned long)LR(0x74));
        }

        /* RIGHT -- cycle compositor configuration */
        if(btn==BUTTON_RIGHT) {
            cfg_idx=(cfg_idx+1)%6;
            const struct comp_cfg *c = &cfgs[cfg_idx];

            LR(0x70)=0;

            CR(0x054) = c->comp_054;
            CR(0x214) = c->comp_214;

            if(c->rotation == 0) {
                CR(0x0D8)=0x1000; CR(0x0DC)=0;
                CR(0x0E0)=0x1000; CR(0x0E4)=0;
                CR(0x0E8)=0x1000; CR(0x0EC)=0;
            } else {
                CR(0x0D8)=0x0400; CR(0x0DC)=0x4003;
                CR(0x0E0)=0x0400; CR(0x0E4)=0x4003;
                CR(0x0E8)=0x0400; CR(0x0EC)=0x4003;
            }

            LR(0x74) = c->lcd_074;

            LR(0x70)=1; LR(0x80)=0;
            for(int i=0;i<10;i++) push_one_frame();

            vlog("%s (pt=%s)", c->desc, pt_mode ? "P9" : "P16");
            vlog("  comp+054=0x%08lx LCD+074=0x%08lx rot=0x%08lx comp+214=0x%08lx",
                 (unsigned long)c->comp_054, (unsigned long)c->lcd_074,
                 (unsigned long)c->rotation, (unsigned long)c->comp_214);
        }

        /* LEFT -- cycle entry mode */
        if(btn==BUTTON_LEFT) {
            em_idx=(em_idx+1)%8;
            LR(0x70)=0;
            ili_set_entry_mode(em_vals[em_idx]);
            ili_set_gram_window();
            LR(0x70)=1;LR(0x80)=0;
            for(int i=0;i<10;i++) push_one_frame();
            vlog("Entry Mode: 0x%04x idx=%d (pt=%s)",
                 em_vals[em_idx],em_idx,pt_mode?"P9":"P16");
        }

        /* PLAY -- toggle LCD+0x7C between 0x402 and 0x401 */
        if(btn==BUTTON_PLAY) {
            lcd_7c_val = (lcd_7c_val == 0x402) ? 0x401 : 0x402;
            LR(0x70)=0;
            LR(0x7C) = lcd_7c_val;
            LR(0x70)=1; LR(0x80)=0;
            for(int i=0;i<10;i++) push_one_frame();
            vlog("LCD+07C=0x%08lx (pt=%s)",
                 (unsigned long)lcd_7c_val, pt_mode?"P9":"P16");
        }

        rb->backlight_on();
    }

    /* Restore state */
    pt_mode = 0;
    LR(0x70)=0;LR(0x80)=0;CR(0x000)=0;
    ili_set_entry_mode(0x0230);
    while(!(LCD_STATUS&0x2));LCD_CON=sc;
    LR(0x88)=s8;LR(0x20)=s2;LR(0x7C)=s7;LR(0x74)=s4;LR(0x78)=s5;
    LCD_PHTIME=0x33;
    {int t=100000;while((LR(0x8C)&3)&&--t>0);}
    rb->lcd_update();

    vlog("Done");
    rb->close(log_fd);
    if(dec) vpu_h264_close(dec);
    rb->cpu_boost(false);
    return PLUGIN_OK;
}
#else
enum plugin_status plugin_start(const void*p){(void)p;rb->splash(HZ*2,"iPod 6G only");return PLUGIN_ERROR;}
#endif
