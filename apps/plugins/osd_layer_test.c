/* OSD Layer Test — tests RGB overlay layers 0-4 on top of Layer 5 video.
 * Uses EXACT compositor_start() sequence from the working driver.
 * Auto-runs all tests, USEC_TIMER busy-wait (no yield).
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

#define OVL_W 160
#define OVL_H 40

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

static uint32_t gram_sample(void) {
    {int t=100000;while((LR(0x8C)&3)&&--t>0);}
    LR(0x70)=0; LR(0x80)=1;
    while(!(LCD_STATUS&0x2));
    {volatile int d=0;while(d++<200);}
    LCD_CON=0x80000DA9;
    while(!(LCD_STATUS&0x2));
    ili_cmd(0x200);ili_data(160);ili_cmd(0x201);ili_data(120);ili_cmd(0x202);
    while(!(LCD_STATUS&0x2));
    LCD_RDATA=0;{int t=100000;while(!(LCD_STATUS&1)&&--t>0);}(void)LCD_DBUFF;
    LCD_RDATA=0;{int t=100000;while(!(LCD_STATUS&1)&&--t>0);}
    uint32_t g=LCD_DBUFF & 0x3FFFF;
    LCD_CON=0x81100DB0; LR(0x80)=0; LR(0x70)=1;
    push_frame();
    return g;
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
    c[0x210/4]=0x00010110;c[0x214/4]=0x013F00EF;c[0x024/4]=0x00FFFFFF;
}

/* Layer enable bits (ROM-verified, reversed order) */
static const uint32_t layer_bits[]={0x040,0x020,0x010,0x008,0x004};

static void layer_show(int n, uint16_t *fb, int w, int h, int x, int y) {
    uint32_t base=0x058+(unsigned)n*0x18;
    uint32_t fmt=(n<=1)?0x10010100:0x00010100;
    int hw_y=240-h-y; if(hw_y<0)hw_y=0;
    CR(base+0x00)=(2*w)/2;
    CR(base+0x04)=fmt;
    CR(base+0x08)=PH(fb);
    CR(base+0x0C)=h|((uint32_t)w<<16);
    CR(base+0x10)=(2*w)/8;
    CR(base+0x14)=x|((uint32_t)hw_y<<16);
    if(n<5){uint32_t v=CR(0x008);v|=layer_bits[n];CR(0x008)=v;}
    vlog("L%d: fmt=0x%08lx fb=0x%08lx %dx%d@(%d,%d) hw_y=%d ctrl=0x%08lx",
         n,(unsigned long)fmt,(unsigned long)PH(fb),w,h,x,y,hw_y,
         (unsigned long)CR(0x008));
}

static void layer_hide(int n) {
    CR(0x058+(unsigned)n*0x18+0x04)=0;
    if(n<5){uint32_t v=CR(0x008);v&=~layer_bits[n];CR(0x008)=v;}
}

static int fsc(const uint8_t*b,int l,int*s){
    for(int i=0;i<l-3;i++){if(b[i]==0&&b[i+1]==0){
        if(b[i+2]==1){*s=3;return i;}
        if(i+3<l&&b[i+2]==0&&b[i+3]==1){*s=4;return i;}}}return-1;}

enum plugin_status plugin_start(const void *parameter)
{
    const char*path=parameter?(const char*)parameter:"/test_iframe.264";
    if(!*path)return PLUGIN_ERROR;
    rb->cpu_boost(true);rb->audio_stop();
    log_fd=rb->open("/osd_layer_test.log",O_WRONLY|O_CREAT|O_TRUNC,0666);
    vlog("=== OSD Layer Test ===");

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
    if(!yo){
        fw=320;fh=240;
        uint8_t*ty=ab,*tcb=ab+fw*fh,*tcr=tcb+(fw/2)*(fh/2);
        for(int r=0;r<fh;r++)for(int c=0;c<fw;c++){
            int i=r*fw+c;
            if(c<fw/3)ty[i]=81;else if(c<2*fw/3)ty[i]=145;else ty[i]=41;}
        for(int r=0;r<fh/2;r++)for(int c=0;c<fw/2;c++){
            int i=r*(fw/2)+c;
            if(c<fw/6){tcb[i]=90;tcr[i]=240;}
            else if(c<fw/3){tcb[i]=54;tcr[i]=34;}
            else{tcb[i]=240;tcr[i]=110;}}
        yo=ty;cbo=tcb;cro=tcr;
        vlog("Test pattern %dx%d",fw,fh);
    }

    uint32_t s_con=LCD_CON,s_7c=LR(0x7C),s_88=LR(0x88);
    uint32_t s_20=LR(0x20),s_74=LR(0x74),s_78=LR(0x78);

    /* OSD framebuffer — 32-byte aligned */
    uint16_t *ovl_fb;
    {
        size_t ds=vpu_h264_buf_size(640,480);
        uintptr_t p=(uintptr_t)(ab+ds+fw*fh*2+4096);
        ovl_fb=(uint16_t*)((p+31)&~(uintptr_t)31);
    }
    rb->memset(ovl_fb,0,OVL_W*OVL_H*2*4);

    /* Start compositor — EXACT compositor_start() */
    {int t=100000;while((LR(0x8C)&3)&&--t>0);}
    PWRCON(0)&=~(0x2080|(7<<14));  /* compositor core + overlay DMA clocks */
    {volatile int d=0;while(d++<10000);}
    comp_hw_init();
    for(int o=0x024;o<=0x044;o+=4)CR(o)=0;
    for(int o=0x04C;o<=0x058;o+=4)CR(o)=0;
    CR(0x028)=0x100;CR(0x02C)=fw|((fw/2)<<16);
    CR(0x034)=fh|((uint32_t)fw<<16);CR(0x04C)=0x10001000;
    CR(0x054)=0x014000F0;CR(0x038)=PH(yo);CR(0x03C)=PH(cro);
    CR(0x040)=0;CR(0x044)=PH(cbo);
    CR(0x3AC)=0x04004002;  /* pipeline config ON, rotation OFF (bit 0 clear) */
    CR(0x0D4)=1;
    {uint32_t v=CR(0x008);v&=~0x100;CR(0x008)=v;}
    rb->commit_discard_dcache();
    LCD_CON=0x81100DB0;LR(0x88)=0x01000000;LR(0x20)=0x33;
    LR(0x7C)=0x00000402;LR(0x78)=0x000A000A;LR(0x74)=0x014000F0;
    while(!(LCD_STATUS&0x2));
    LCD_CON=0x80000DA9;
    ili_cmd(0x003);ili_data(0x1238);
    ili_cmd(0x210);ili_data(0);ili_cmd(0x211);ili_data(319);
    ili_cmd(0x212);ili_data(0);ili_cmd(0x213);ili_data(239);
    ili_cmd(0x200);ili_data(0);ili_cmd(0x201);ili_data(0);ili_cmd(0x202);
    while(!(LCD_STATUS&0x2));LCD_CON=0x81100DB0;
    CR(0x000)=1;LR(0x70)=1;LR(0x80)=0;
    push_frame();

    vlog("Video baseline active, holding 2s");
    busywait_us(2000000);

    /* T0: Red ARGB1555 overlay on Layer 0 */
    for(int i=0;i<OVL_W*OVL_H;i++) ovl_fb[i]=0xFC00; /* A=1 R=31 G=0 B=0 */
    rb->commit_discard_dcache();
    layer_show(0, ovl_fb, OVL_W, OVL_H, 80, 100);
    push_frame();
    vlog("T0: RED overlay");
    busywait_us(2000000);
    {uint32_t g=gram_sample(); vlog("  GRAM=0x%06lx %s", (unsigned long)g, g>0x100?"VISIBLE":"black");}

    /* T1: Green */
    for(int i=0;i<OVL_W*OVL_H;i++) ovl_fb[i]=0x83E0; /* A=1 R=0 G=31 B=0 */
    rb->commit_discard_dcache();
    layer_show(0, ovl_fb, OVL_W, OVL_H, 80, 100);
    push_frame();
    vlog("T1: GREEN overlay");
    busywait_us(2000000);
    {uint32_t g=gram_sample(); vlog("  GRAM=0x%06lx %s", (unsigned long)g, g>0x100?"VISIBLE":"black");}

    /* T2: White (all bits set = max visibility test) */
    for(int i=0;i<OVL_W*OVL_H;i++) ovl_fb[i]=0xFFFF;
    rb->commit_discard_dcache();
    layer_show(0, ovl_fb, OVL_W, OVL_H, 80, 100);
    push_frame();
    vlog("T2: WHITE overlay");
    busywait_us(2000000);
    {uint32_t g=gram_sample(); vlog("  GRAM=0x%06lx %s", (unsigned long)g, g>0x100?"VISIBLE":"black");}

    /* T3: Hide layer */
    layer_hide(0);
    push_frame();
    vlog("T3: Layer hidden");
    busywait_us(1000000);

    vlog("=== DONE ===");

    /* Cleanup */
    layer_hide(0);layer_hide(1);layer_hide(2);
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
