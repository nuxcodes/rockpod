/* VPP Minimal — bare-bones landscape compositor display
 * NO diagnostic tests. NO GRAM scans. NO TEST_SW.
 * Just: decode → landscape config → passthrough → display.
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

static void lc(uint16_t c){while(LCD_STATUS&0x10);LCD_WCMD=c;}
static void ld(uint16_t d){while(LCD_STATUS&0x10);LCD_WDATA=d;}
static int fsc(const uint8_t*b,int l,int*s){
    for(int i=0;i<l-3;i++){if(b[i]==0&&b[i+1]==0){
        if(b[i+2]==1){*s=3;return i;}
        if(i+3<l&&b[i+2]==0&&b[i+3]==1){*s=4;return i;}}}return-1;}

enum plugin_status plugin_start(const void *parameter)
{
    const char*path=parameter?(const char*)parameter:"/test_iframe.264";
    if(!*path)return PLUGIN_ERROR;
    rb->cpu_boost(true); rb->audio_stop();

    uint8_t*ab;size_t as;
    ab=rb->plugin_get_audio_buffer(&as);
    size_t ds=vpu_h264_buf_size(640,480);
    struct vpu_h264*dec=vpu_h264_open(ab,ds,640,480);
    if(!dec)return PLUGIN_ERROR;
    uint8_t*fb=ab+ds;
    int fd=rb->open(path,O_RDONLY);if(fd<0)return PLUGIN_ERROR;
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
    if(!yo){vpu_h264_close(dec);return PLUGIN_ERROR;}

    uint32_t sc=LCD_CON,s7=LR(0x7C),s8=LR(0x88),s2=LR(0x20),s4=LR(0x74),s5=LR(0x78);

    PWRCON(0)&=~0x2080;
    for(volatile int d=0;d<10000;d++);

    /* Compositor init */
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
    c[0x214/4]=0x013F00EF;  /* LANDSCAPE */
    c[0x024/4]=0x00FFFFFF;

    /* Layer 5 */
    for(int o=0x024;o<=0x044;o+=4)CR(o)=0;
    for(int o=0x04C;o<=0x058;o+=4)CR(o)=0;
    CR(0x028)=0x100; CR(0x02C)=fw|((fw/2)<<16);
    CR(0x034)=fh|((uint32_t)fw<<16);
    CR(0x04C)=0x10001000; CR(0x054)=0x014000F0;
    CR(0x038)=PH(yo); CR(0x03C)=PH(cro); CR(0x040)=0; CR(0x044)=PH(cbo);
    CR(0x3AC)=0; CR(0x0D4)=1;
    {uint32_t v=CR(0x008);v&=~0x100;CR(0x008)=v;}
    rb->commit_discard_dcache();

    /* LCD passthrough */
    LCD_CON=0x80100DB0;
    LR(0x88)=0x01000000; LR(0x20)=0x33; LR(0x7C)=0x00000402;
    LR(0x78)=0x000A000A; LR(0x74)=0x014000F0;
    while(!(LCD_STATUS&0x2)); LCD_CON=0x80000DA9;
    lc(0x003);ld(0x1030); lc(0x210);ld(0); lc(0x211);ld(319);
    lc(0x212);ld(0); lc(0x213);ld(239); lc(0x200);ld(0); lc(0x201);ld(0); lc(0x202);
    while(!(LCD_STATUS&0x2)); LCD_CON=0x80100DB0;
    LR(0x70)=1; LR(0x80)=0;

    /* Start */
    CR(0x000)=0;{volatile int d=0;while(d++<50000);}
    CR(0x000)=1;{uint32_t t=USEC_TIMER;while((USEC_TIMER-t)<200000);}
    for(int i=0;i<10;i++){
        {int t=100000;while((LR(0x8C)&3)&&--t>0);}
        LR(0x80)=1; LCD_CON=0x80000DA9;
        lc(0x200);ld(0);lc(0x201);ld(0);lc(0x202);
        while(!(LCD_STATUS&0x2)); LCD_CON=0x80100DB0;
        LR(0x80)=0;{int t=500000;while((LR(0x8C)&3)&&--t>0);}
    }

    while(rb->button_get(true)==BUTTON_NONE) rb->backlight_on();

    /* Shutdown */
    LR(0x70)=0; LR(0x80)=0; CR(0x000)=0;
    while(!(LCD_STATUS&0x2)); LCD_CON=0x80000DA9;
    lc(0x003);ld(0x0230);
    while(!(LCD_STATUS&0x2)); LCD_CON=sc;
    LR(0x88)=s8;LR(0x20)=s2;LR(0x7C)=s7;LR(0x74)=s4;LR(0x78)=s5;
    LCD_PHTIME=0x33;
    {int t=100000;while((LR(0x8C)&3)&&--t>0);}
    rb->lcd_update();
    vpu_h264_close(dec); rb->cpu_boost(false);
    return PLUGIN_OK;
}
#else
enum plugin_status plugin_start(const void*p){(void)p;rb->splash(HZ*2,"iPod 6G only");return PLUGIN_ERROR;}
#endif
