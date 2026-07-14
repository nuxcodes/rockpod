/* OSD Sweep — brute-force test of overlay layer configurations.
 * Tests many combinations rapidly. User watches for non-black.
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

static int log_fd = -1;
static void vlog(const char *fmt, ...) {
    if(log_fd<0)return;
    char buf[256];va_list ap;va_start(ap,fmt);
    int n=rb->vsnprintf(buf,sizeof(buf),fmt,ap);va_end(ap);
    rb->write(log_fd,buf,n);rb->write(log_fd,"\n",1);
}

static void ili_cmd(uint16_t c){while(LCD_STATUS&0x10);LCD_WCMD=c;}
static void ili_data(uint16_t d){while(LCD_STATUS&0x10);LCD_WDATA=d;}

static void push_frame(void) {
    {int t=100000;while((LR(0x8C)&3)&&--t>0);}
    LR(0x80)=1;
    while(!(LCD_STATUS&0x2));LCD_CON=0x80000DA9;
    ili_cmd(0x200);ili_data(0);ili_cmd(0x201);ili_data(0);ili_cmd(0x202);
    while(!(LCD_STATUS&0x2));LCD_CON=0x81100DB0;
    LR(0x80)=0;
}

static void busywait_us(uint32_t us) {
    uint32_t t=USEC_TIMER;while((USEC_TIMER-t)<us)rb->backlight_on();
}

static void comp_hw_init(void) {
    volatile uint32_t *c=(volatile uint32_t*)COMP;
    c[0x200/4]&=~1;c[0x004/4]=1;c[0x020/4]=1;
    for(int i=0;i<256;i++){c[0x400/4+i]=i*4;c[0x800/4+i]=i*4;c[0xC00/4+i]=i*4;}
    {volatile uint32_t *s=(volatile uint32_t*)0x0890D2DC;
     uint32_t t[5];for(int i=0;i<5;i++)t[i]=s[i];
     if(t[0]>0&&t[0]<0x1000&&t[4]>0&&t[4]<0x1000)
         for(int i=0;i<5;i++)c[(0x1EC+i*4)/4]=t[i];
     else{uint32_t h[]={0x0C,0x26,0x10,0x82,0x4E};
          for(int i=0;i<5;i++)c[(0x1EC+i*4)/4]=h[i];}}
    c[0x0D8/4]=0x1000;c[0x0DC/4]=0;c[0x0E0/4]=0x1000;c[0x0E4/4]=0;
    c[0x0E8/4]=0x1000;c[0x0EC/4]=0;
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
    c[0x210/4]=0x00010110;c[0x214/4]=0x013F00EF;
}

static int fsc(const uint8_t*b,int l,int*s){
    for(int i=0;i<l-3;i++){if(b[i]==0&&b[i+1]==0){
        if(b[i+2]==1){*s=3;return i;}
        if(i+3<l&&b[i+2]==0&&b[i+3]==1){*s=4;return i;}}}return-1;}

static void setup_l0(uint16_t *fb, int w, int h, uint32_t fmt) {
    CR(0x058) = (2*w)/2;
    CR(0x05C) = fmt;
    CR(0x060) = PH(fb);
    CR(0x064) = h|((uint32_t)w<<16);
    CR(0x068) = ((2*w)+7)/8;
    CR(0x06C) = 0;
    {uint32_t v=CR(0x008);v|=0x040;CR(0x008)=v;}
}

enum plugin_status plugin_start(const void *parameter)
{
    const char*path=parameter?(const char*)parameter:"/test_iframe.264";
    if(!*path)return PLUGIN_ERROR;
    rb->cpu_boost(true);rb->audio_stop();
    log_fd=rb->open("/osd_sweep.log",O_WRONLY|O_CREAT|O_TRUNC,0666);
    vlog("=== OSD SWEEP TEST ===");

    uint8_t*ab;size_t as;
    ab=rb->plugin_get_audio_buffer(&as);
    const uint8_t*yo=NULL,*cbo=NULL,*cro=NULL;
    int fw=0,fh=0;

    int fd=rb->open(path,O_RDONLY);
    if(fd>=0){
        size_t ds=vpu_h264_buf_size(640,480);
        struct vpu_h264*dec=vpu_h264_open(ab,ds,640,480);
        if(dec){
            uint8_t*fb=ab+ds;
            int fl=rb->read(fd,fb,512*1024);rb->close(fd);
            int pos=0;
            while(pos<fl-4){
                int sl,sp=fsc(fb+pos,fl-pos,&sl);if(sp<0)break;
                int ns=pos+sp+sl,nx=fsc(fb+ns,fl-ns,&sl),nl=(nx>=0)?nx:fl-ns;
                if(vpu_h264_decode_nalu(dec,fb+ns,nl)==1)
                    vpu_h264_get_frame(dec,&yo,&cbo,&cro,&fw,&fh);
                pos=ns+nl;
            }
            if(!yo)vpu_h264_close(dec);
            else vlog("Decoded %dx%d",fw,fh);
        } else rb->close(fd);
    }
    if(!yo){fw=320;fh=240;
        uint8_t*ty=ab;rb->memset(ty,145,fw*fh);
        rb->memset(ty+fw*fh,54,(fw/2)*(fh/2));
        rb->memset(ty+fw*fh+(fw/2)*(fh/2),34,(fw/2)*(fh/2));
        yo=ty;cbo=ty+fw*fh;cro=cbo+(fw/2)*(fh/2);}

    uint32_t s_con=LCD_CON,s_7c=LR(0x7C),s_88=LR(0x88);
    uint32_t s_20=LR(0x20),s_74=LR(0x74),s_78=LR(0x78);

    /* Overlay FB */
    uint16_t *ovl;
    {size_t ds=vpu_h264_buf_size(640,480);
     uintptr_t p=(uintptr_t)(ab+ds+fw*fh*2+4096);
     ovl=(uint16_t*)((p+31)&~(uintptr_t)31);}

    /* Start compositor with L5 video */
    {int t=100000;while((LR(0x8C)&3)&&--t>0);}
    PWRCON(0)&=~(0x2080|(7<<14));
    {volatile int d=0;while(d++<10000);}
    comp_hw_init();
    for(int o=0x028;o<=0x044;o+=4)CR(o)=0;
    for(int o=0x04C;o<=0x058;o+=4)CR(o)=0;
    CR(0x028)=0x100;CR(0x02C)=fw|((fw/2)<<16);
    CR(0x034)=fh|((uint32_t)fw<<16);CR(0x04C)=0x10001000;
    CR(0x054)=0x014000F0;CR(0x038)=PH(yo);CR(0x03C)=PH(cro);
    CR(0x040)=0;CR(0x044)=PH(cbo);
    CR(0x3AC)=0;CR(0x0D4)=1;
    rb->commit_discard_dcache();
    LCD_CON=0x81100DB0;LR(0x88)=0x01000000;LR(0x20)=0x33;
    LR(0x7C)=0x00000402;LR(0x78)=0x000A000A;LR(0x74)=0x014000F0;
    while(!(LCD_STATUS&0x2));LCD_CON=0x80000DA9;
    ili_cmd(0x003);ili_data(0x1238);
    ili_cmd(0x210);ili_data(0);ili_cmd(0x211);ili_data(319);
    ili_cmd(0x212);ili_data(0);ili_cmd(0x213);ili_data(239);
    ili_cmd(0x200);ili_data(0);ili_cmd(0x201);ili_data(0);ili_cmd(0x202);
    while(!(LCD_STATUS&0x2));LCD_CON=0x81100DB0;
    CR(0x000)=1;LR(0x70)=1;LR(0x80)=0;
    push_frame();
    vlog("Video baseline 2s");
    busywait_us(2000000);

    /* Fill overlay FB with RED */
    for(int i=0;i<320*240;i++) ovl[i]=0xF800;
    rb->commit_discard_dcache();

    int test = 0;

    /* Sweep 1: comp+0x3AC values with L0 overlay */
    {
        static const uint32_t ac_vals[] = {0, 0x04004002, 0x04004003, 0x04004000};
        for(int a=0;a<4;a++) {
            CR(0x3AC) = ac_vals[a];
            /* L0 only (L5 off) */
            {uint32_t v=CR(0x008);v&=~0x080;v|=0x040;CR(0x008)=v;}
            setup_l0(ovl, 320, 240, 0x10010100);
            CR(0x024)=1;
            push_frame();
            vlog("T%d: 3AC=0x%08lx L0only fmt=RGB565",
                 test++, (unsigned long)ac_vals[a]);
            busywait_us(2000000);
            /* restore L5 */
            {uint32_t v=CR(0x008);v|=0x080;CR(0x008)=v;}
        }
        CR(0x3AC)=0;
    }

    /* Sweep 2: bit 8 (CSC scope) */
    {
        /* bit8=0 (CSC for all) + L0 only */
        {uint32_t v=CR(0x008);v&=~0x100;v&=~0x080;v|=0x040;CR(0x008)=v;}
        setup_l0(ovl, 320, 240, 0x10010100);
        CR(0x024)=1;
        push_frame();
        vlog("T%d: bit8=0 (CSC all) L0only RGB565", test++);
        busywait_us(2000000);
        /* restore */
        {uint32_t v=CR(0x008);v|=0x100;v|=0x080;v&=~0x040;CR(0x008)=v;}
    }

    /* Sweep 3: different format codes for L0 */
    {
        static const uint32_t fmts[] = {
            0x10010000, /* fmt 0: 8bpp indexed */
            0x10010100, /* fmt 1: RGB565 */
            0x10010200, /* fmt 2: ARGB8888 */
            0x10010400, /* fmt 4: ARGB with alpha */
            0x00000100, /* fmt 1 bare (no bit28/16) */
            0x00010100, /* fmt 1 L2-4 style */
        };
        static const char *fdesc[] = {
            "fmt0-8bpp", "fmt1-RGB565", "fmt2-ARGB",
            "fmt4-ARGBa", "fmt1-bare", "fmt1-L24"
        };
        /* Fill ARGB version too */
        {uint32_t *o32=(uint32_t*)ovl;
         for(int i=0;i<320*240;i++)o32[i]=0xFFFF0000;}
        rb->commit_discard_dcache();

        for(int f=0;f<6;f++) {
            {uint32_t v=CR(0x008);v&=~0x080;v|=0x040;CR(0x008)=v;}
            int bpp = (f>=2 && f<=3) ? 4 : 2;
            CR(0x058) = (bpp*320)/2;
            CR(0x05C) = fmts[f];
            CR(0x060) = PH(ovl);
            CR(0x064) = 240|(320U<<16);
            CR(0x068) = ((bpp*320)+7)/8;
            CR(0x06C) = 0;
            CR(0x024)=1;
            push_frame();
            vlog("T%d: %s L0only", test++, fdesc[f]);
            busywait_us(2000000);
            {uint32_t v=CR(0x008);v|=0x080;v&=~0x040;CR(0x008)=v;}
        }
    }

    /* Sweep 4: Layer 1 instead of Layer 0 */
    {
        for(int i=0;i<320*240;i++) ovl[i]=0xF800;
        rb->commit_discard_dcache();
        /* Use Layer 1 (bit 5, regs at 0x070) */
        {uint32_t v=CR(0x008);v&=~0x080;v|=0x020;CR(0x008)=v;}
        CR(0x070) = 160;
        CR(0x074) = 0x10010100;
        CR(0x078) = PH(ovl);
        CR(0x07C) = 240|(320U<<16);
        CR(0x080) = 80;
        CR(0x084) = 0;
        CR(0x024)=1;
        push_frame();
        vlog("T%d: Layer1 RED L5off", test++);
        busywait_us(2000000);
        {uint32_t v=CR(0x008);v&=~0x020;v|=0x080;CR(0x008)=v;}
    }

    /* Sweep 5: Layer 4 (topmost) */
    {
        {uint32_t v=CR(0x008);v&=~0x080;v|=0x004;CR(0x008)=v;}
        CR(0x0B8) = 160;
        CR(0x0BC) = 0x00010100; /* no bit28 for L4 */
        CR(0x0C0) = PH(ovl);
        CR(0x0C4) = 240|(320U<<16);
        CR(0x0C8) = 80;
        CR(0x0CC) = 0;
        CR(0x024)=1;
        push_frame();
        vlog("T%d: Layer4 RED L5off", test++);
        busywait_us(2000000);
        {uint32_t v=CR(0x008);v&=~0x004;v|=0x080;CR(0x008)=v;}
    }

    /* Sweep 6: L0+L5 together with different 3AC */
    {
        for(int i=0;i<320*240;i++) ovl[i]=0xF800;
        rb->commit_discard_dcache();
        {uint32_t v=CR(0x008);v|=0x040;CR(0x008)=v;} /* L0+L5 */
        setup_l0(ovl, 320, 240, 0x10010100);

        CR(0x3AC)=0x04004003;
        CR(0x024)=1;
        push_frame();
        vlog("T%d: L0+L5 3AC=0x04004003", test++);
        busywait_us(2000000);

        CR(0x3AC)=0;
        CR(0x024)=1;
        push_frame();
        vlog("T%d: L0+L5 3AC=0", test++);
        busywait_us(2000000);

        {uint32_t v=CR(0x008);v&=~0x040;CR(0x008)=v;}
    }

    /* Sweep 7: Enable PWRCON bits 14-16 (EV0/EV1/EV2 — possible overlay DMA) */
    {
        PWRCON(0) &= ~((7 << 14) | (1 << 18)); /* same as jpeg_hw.c */
        for(int i=0;i<320*240;i++) ovl[i]=0x07E0; /* GREEN */
        rb->commit_discard_dcache();

        {uint32_t v=CR(0x008);v&=~0x080;v|=0x040;CR(0x008)=v;}
        setup_l0(ovl, 320, 240, 0x10010100);
        CR(0x024)=1;
        push_frame();
        vlog("T%d: PWRCON bits14-16+18 enabled, L0only GREEN", test++);
        busywait_us(2000000);

        /* L0+L5 */
        {uint32_t v=CR(0x008);v|=0x080;CR(0x008)=v;}
        CR(0x024)=1;
        push_frame();
        vlog("T%d: PWRCON bits14-16+18 L0+L5 GREEN", test++);
        busywait_us(2000000);

        {uint32_t v=CR(0x008);v&=~0x040;CR(0x008)=v;}
    }

    vlog("=== DONE (%d tests) ===", test);

    /* Cleanup */
    LR(0x70)=0;LR(0x80)=0;CR(0x000)=0;
    while(!(LCD_STATUS&0x2));LCD_CON=0x80000DA9;
    ili_cmd(0x003);ili_data(0x0230);
    while(!(LCD_STATUS&0x2));
    LCD_CON=s_con;LR(0x88)=s_88;LR(0x20)=s_20;
    LR(0x7C)=s_7c;LR(0x74)=s_74;LR(0x78)=s_78;
    {int t=100000;while((LR(0x8C)&3)&&--t>0);}
    rb->close(log_fd);rb->cpu_boost(false);
    rb->lcd_set_viewport(NULL);rb->lcd_clear_display();rb->lcd_update();
    return PLUGIN_OK;
}
#else
enum plugin_status plugin_start(const void*p){(void)p;rb->splash(2*HZ,"iPod 6G only");return PLUGIN_ERROR;}
#endif
