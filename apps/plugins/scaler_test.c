/* Compositor scaler test — replicates EXACT compositor_start() sequence,
 * then tests different comp+0x04C scale values.
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

/* push_frame — Apple ROM pattern: just LR(0x80) bracket, NO LCD_CON toggle */
static void push_frame(void)
{
    {int t=100000;while((LR(0x8C)&3)&&--t>0);}
    LR(0x80) = 1;
    LR(0x80) = 0;
}

/* GRAM readback — preserves PINMAP via Apple cmd mode formula */
static void gram_read(int x, int y, const char *label)
{
    {int t=100000;while((LR(0x8C)&3)&&--t>0);}
    LR(0x70)=0; LR(0x80)=1;
    while(!(LCD_STATUS&0x2));
    {volatile int d=0;while(d++<100);}
    LCD_CON=0x80000DA9;
    while(!(LCD_STATUS&0x2));
    ili_cmd(0x200);ili_data(x);ili_cmd(0x201);ili_data(y);ili_cmd(0x202);
    while(!(LCD_STATUS&0x2));
    LCD_RDATA=0;{int t=100000;while(!(LCD_STATUS&1)&&--t>0);}(void)LCD_DBUFF;
    LCD_RDATA=0;{int t=100000;while(!(LCD_STATUS&1)&&--t>0);}
    uint32_t g=LCD_DBUFF;
    uint32_t r6=(g>>12)&0x3F,g6=(g>>6)&0x3F,b6=g&0x3F;
    vlog("  %s (%d,%d) R=%lu G=%lu B=%lu raw=0x%06lx",
         label,x,y,(unsigned long)r6,(unsigned long)g6,(unsigned long)b6,
         (unsigned long)(g&0x3FFFF));
    LCD_CON=0x81100DB9; LR(0x80)=0; LR(0x70)=1;
    push_frame();
}

/* EXACT comp_hw_init from compositor-s5l8702.c */
static void comp_hw_init(void)
{
    volatile uint32_t *c = (volatile uint32_t*)COMP;
    c[0x200/4] &= ~1; c[0x004/4] = 1; c[0x020/4] = 1;
    for(int i=0;i<256;i++){c[0x400/4+i]=i*4;c[0x800/4+i]=i*4;c[0xC00/4+i]=i*4;}
    {volatile uint32_t *s=(volatile uint32_t*)0x0890D2DC;
     uint32_t t[5];for(int i=0;i<5;i++)t[i]=s[i];
     if(t[0]>0&&t[0]<0x1000&&t[4]>0&&t[4]<0x1000)
         for(int i=0;i<5;i++)c[(0x1EC+i*4)/4]=t[i];
     else{uint32_t h[]={0x0C,0x26,0x10,0x82,0x4E};
          for(int i=0;i<5;i++)c[(0x1EC+i*4)/4]=h[i];}}
    c[0x0D8/4]=0x1000;c[0x0DC/4]=0;
    c[0x0E0/4]=0x1000;c[0x0E4/4]=0;
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
    c[0x210/4]=0x00010110;c[0x214/4]=0x00EF013F; /* portrait viewport */
    c[0x024/4]=0x00FFFFFF;
}

/* EXACT compositor_start sequence — copy-paste from compositor-s5l8702.c */
static void comp_start(int fw, int fh,
    const uint8_t *y, const uint8_t *cb, const uint8_t *cr)
{
    {int t=100000;while((LR(0x8C)&3)&&--t>0);}
    PWRCON(0) &= ~0x2080;
    {volatile int d=0;while(d++<10000);}
    comp_hw_init();

    for(int o=0x024;o<=0x044;o+=4)CR(o)=0;
    for(int o=0x04C;o<=0x058;o+=4)CR(o)=0;
    CR(0x028)=0x100;
    CR(0x02C)=fw|((fw/2)<<16);
    CR(0x034)=fh|((uint32_t)fw<<16);
    CR(0x04C)=0x10001000;
    CR(0x054)=0x00F00140;    /* portrait output */
    CR(0x038)=PH(y); CR(0x03C)=PH(cr);
    CR(0x040)=0;     CR(0x044)=PH(cb);
    CR(0x3AC)=0x04004003;    /* rotation ON (Apple always uses this) */
    CR(0x0D4)=1;
    {uint32_t v=CR(0x008);v&=~0x100;CR(0x008)=v;}
    rb->commit_discard_dcache();

    /* P9 passthrough — Apple ROM values */
    LCD_CON=0x81100DB9;      /* P9 mode */
    LR(0x88)=0x01000000;
    LR(0x20)=0x33;
    LR(0x7C)=0x00000402;
    LR(0x78)=0x000A000A;
    LR(0x74)=0x00F00140;     /* portrait — Apple ROM value */

    /* ILI9326 GRAM setup via P18 — standard P18, PINMAP irrelevant for 18-bit */
    while(!(LCD_STATUS&0x2));
    LCD_CON=0x80000DA9;
    ili_cmd(0x003);ili_data(0x1238);
    ili_cmd(0x210);ili_data(0);ili_cmd(0x211);ili_data(319);
    ili_cmd(0x212);ili_data(0);ili_cmd(0x213);ili_data(239);
    ili_cmd(0x200);ili_data(0);ili_cmd(0x201);ili_data(0);ili_cmd(0x202);
    while(!(LCD_STATUS&0x2));
    LCD_CON=0x81100DB9;      /* restore P9 */

    CR(0x000)=1;
    LR(0x70)=1; LR(0x80)=0;
    push_frame();
}

static int fsc(const uint8_t*b,int l,int*s){
    for(int i=0;i<l-3;i++){if(b[i]==0&&b[i+1]==0){
        if(b[i+2]==1){*s=3;return i;}
        if(i+3<l&&b[i+2]==0&&b[i+3]==1){*s=4;return i;}}}return-1;}

enum plugin_status plugin_start(const void *parameter)
{
    const char *path = parameter?(const char*)parameter:"/test_iframe.264";
    if(!*path) return PLUGIN_ERROR;
    rb->cpu_boost(true); rb->audio_stop();

    log_fd=rb->open("/scaler_test.log",O_WRONLY|O_CREAT|O_TRUNC,0666);
    vlog("=== Compositor Scaler Test ===");

    uint8_t *ab; size_t as;
    ab = rb->plugin_get_audio_buffer(&as);
    const uint8_t *yo=NULL, *cbo=NULL, *cro=NULL;
    int fw=0, fh=0;

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
            if(!yo){vpu_h264_close(dec);}
            else vlog("Decoded %dx%d",fw,fh);
        } else rb->close(fd);
    }

    if(!yo){
        fw=320;fh=240;
        uint8_t*ty=ab,*tcb=ab+fw*fh,*tcr=tcb+(fw/2)*(fh/2);
        for(int r=0;r<fh;r++)for(int c=0;c<fw;c++){
            int i=r*fw+c;
            if(c<fw/3)ty[i]=81;else if(c<2*fw/3)ty[i]=145;else ty[i]=41;
        }
        for(int r=0;r<fh/2;r++)for(int c=0;c<fw/2;c++){
            int i=r*(fw/2)+c;
            if(c<fw/6){tcb[i]=90;tcr[i]=240;}
            else if(c<fw/3){tcb[i]=54;tcr[i]=34;}
            else{tcb[i]=240;tcr[i]=110;}
        }
        yo=ty;cbo=tcb;cro=tcr;
        vlog("Test pattern %dx%d",fw,fh);
    }

    uint32_t saved_con=LCD_CON, saved_7c=LR(0x7C), saved_88=LR(0x88);
    uint32_t saved_20=LR(0x20), saved_74=LR(0x74), saved_78=LR(0x78);

    /* Start compositor — EXACT copy of compositor_start() */
    comp_start(fw, fh, yo, cbo, cro);
    vlog("Compositor active, holding 3s");
    vlog("  008=0x%08lx 04C=0x%08lx 054=0x%08lx 3AC=0x%08lx 074=0x%08lx",
         (unsigned long)CR(0x008),(unsigned long)CR(0x04C),
         (unsigned long)CR(0x054),(unsigned long)CR(0x3AC),
         (unsigned long)LR(0x74));

    /* Hold 3s — busy-wait, no yield */
    {uint32_t t=USEC_TIMER;while((USEC_TIMER-t)<3000000)rb->backlight_on();}

    /* LCD+0x7C sweep — test transfer count per pixel */
    vlog("=== LCD+0x7C TEST (P9 transfer count) ===");
    static const uint32_t lcd7c_vals[] = {0x402, 0x403, 0x400, 0x401};
    static const char *lcd7c_names[] = {"0x402(2xfr)", "0x403(3xfr)", "0x400(0)", "0x401(1xfr)"};
    for(int i=0;i<4;i++){
        LR(0x7C) = lcd7c_vals[i];
        push_frame();
        vlog("[7C-%d] 0x%03lx %s", i, (unsigned long)lcd7c_vals[i], lcd7c_names[i]);
        gram_read(160,120,lcd7c_names[i]);
        {uint32_t t=USEC_TIMER;while((USEC_TIMER-t)<2000000)rb->backlight_on();}
    }
    LR(0x7C) = 0x402;
    vlog("=== LCD+0x7C TEST DONE ===");

    /* Baseline GRAM readback */
    gram_read(160,120,"baseline-center");
    gram_read(10,10,"baseline-TL");
    gram_read(310,230,"baseline-BR");

    /* Scaler test — change ONLY comp+0x04C */
    static const uint32_t scales[] = {
        0x10001000, 0x08000800, 0x20002000, 0x0C000C00, 0x04000400
    };
    static const char *names[] = {
        "unity", "0800", "2000", "0C00", "0400"
    };
    for(int i=0;i<5;i++){
        CR(0x04C) = scales[i];
        push_frame();
        vlog("[S%d] 04C=0x%08lx %s",i,(unsigned long)scales[i],names[i]);
        {uint32_t t=USEC_TIMER;while((USEC_TIMER-t)<1000000)rb->backlight_on();}
        gram_read(160,120,names[i]);
        gram_read(80,60,names[i]);
        gram_read(240,180,names[i]);
    }

    vlog("=== DONE ===");

    /* Cleanup */
    LR(0x70)=0; LR(0x80)=0; CR(0x000)=0;
    while(!(LCD_STATUS&0x2)); LCD_CON=0x80000DA9;
    ili_cmd(0x003);ili_data(0x0230);
    while(!(LCD_STATUS&0x2));
    LCD_CON=saved_con;
    LR(0x88)=saved_88;LR(0x20)=saved_20;
    LR(0x7C)=saved_7c;LR(0x74)=saved_74;LR(0x78)=saved_78;
    {int t=100000;while((LR(0x8C)&3)&&--t>0);}
    rb->close(log_fd);
    rb->cpu_boost(false);
    rb->lcd_set_viewport(NULL);
    rb->lcd_clear_display();
    rb->lcd_update();
    return PLUGIN_OK;
}
#else
enum plugin_status plugin_start(const void*p){(void)p;rb->splash(2*HZ,"iPod 6G only");return PLUGIN_ERROR;}
#endif
