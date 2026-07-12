/* VPP Pre-Shear Test — Apple defaults + pre-distorted YUV
 * Fixes 240/scan shearing by rearranging YUV planes.
 * ALL compositor registers at Apple defaults. Zero HW risk.
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
    c[0x214/4]=0x00EF013F;
    c[0x024/4]=0x00FFFFFF;
}

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

    /* Allocate pre-sheared buffers after file data */
    size_t y_sz=fw*fh, c_sz=(fw/2)*(fh/2);
    uint8_t *ps_y=fb+fl, *ps_cb=ps_y+y_sz, *ps_cr=ps_cb+c_sz;

    /* === PRE-SHEAR Y PLANE ===
     * The compositor with rotation ON + 240/scan into 320 GRAM:
     * GRAM(x,y) at linear=y*320+x receives source(col=linear/240, row=linear%240)
     * We want GRAM(x,y) = original(x,y).
     * So place original(x,y) at source position (col=(y*320+x)/240, row=(y*320+x)%240).
     * In the Y plane: dst[row*fw + col] = src[y*fw + x] */
    {
        uint8_t *dst=(uint8_t*)((uintptr_t)ps_y|0x40000000);
        const uint8_t *src=(const uint8_t*)((uintptr_t)yo|0x40000000);
        rb->memset(dst, 0, y_sz);
        for(int y=0;y<fh;y++){
            for(int x=0;x<fw;x++){
                int linear=y*320+x;
                int col=linear/240;
                int row=linear%240;
                if(col<fw && row<fh)
                    dst[row*fw+col]=src[y*fw+x];
            }
        }
    }

    /* === PRE-SHEAR Cb PLANE (quarter res) === */
    {
        int cw=fw/2, ch=fh/2;
        uint8_t *dst=(uint8_t*)((uintptr_t)ps_cb|0x40000000);
        const uint8_t *src=(const uint8_t*)((uintptr_t)cbo|0x40000000);
        rb->memset(dst, 128, c_sz);
        for(int y=0;y<ch;y++){
            for(int x=0;x<cw;x++){
                int linear=y*160+x;
                int col=linear/120;
                int row=linear%120;
                if(col<cw && row<ch)
                    dst[row*cw+col]=src[y*cw+x];
            }
        }
    }

    /* === PRE-SHEAR Cr PLANE === */
    {
        int cw=fw/2, ch=fh/2;
        uint8_t *dst=(uint8_t*)((uintptr_t)ps_cr|0x40000000);
        const uint8_t *src=(const uint8_t*)((uintptr_t)cro|0x40000000);
        rb->memset(dst, 128, c_sz);
        for(int y=0;y<ch;y++){
            for(int x=0;x<cw;x++){
                int linear=y*160+x;
                int col=linear/120;
                int row=linear%120;
                if(col<cw && row<ch)
                    dst[row*cw+col]=src[y*cw+x];
            }
        }
    }

    rb->commit_discard_dcache();

    uint32_t sc=LCD_CON,s7=LR(0x7C),s8=LR(0x88),s2=LR(0x20),s4=LR(0x74),s5=LR(0x78);

    PWRCON(0)&=~0x2080;
    for(volatile int d=0;d<10000;d++);

    comp_init();

    /* Layer 5 — ALL APPLE DEFAULTS + pre-sheared buffers */
    for(int o=0x024;o<=0x044;o+=4)CR(o)=0;
    for(int o=0x04C;o<=0x058;o+=4)CR(o)=0;
    CR(0x028)=0x100; CR(0x02C)=fw|((fw/2)<<16);
    CR(0x034)=fh|((uint32_t)fw<<16);
    CR(0x04C)=0x10001000;
    CR(0x054)=0x00F00140;
    CR(0x038)=PH(ps_y); CR(0x03C)=PH(ps_cr); CR(0x040)=0; CR(0x044)=PH(ps_cb);
    CR(0x3AC)=0x04004003;  /* rotation ON — Apple default */
    CR(0x0D4)=1;
    {uint32_t v=CR(0x008);v&=~0x100;CR(0x008)=v;}

    /* LCD passthrough — P16 for ILI9326, Apple defaults */
    LCD_CON=0x80100DB0;
    LR(0x88)=0x01000000; LR(0x20)=0x33; LR(0x7C)=0x00000402;
    LR(0x78)=0x000A000A; LR(0x74)=0x00F00140;

    while(!(LCD_STATUS&0x2)); LCD_CON=0x80000DA9;
    lc(0x003);ld(0x1030);
    lc(0x210);ld(0); lc(0x211);ld(319);
    lc(0x212);ld(0); lc(0x213);ld(239);
    lc(0x200);ld(0); lc(0x201);ld(0); lc(0x202);
    while(!(LCD_STATUS&0x2)); LCD_CON=0x80100DB0;
    LR(0x70)=1; LR(0x80)=0;

    CR(0x000)=0;{volatile int d=0;while(d++<50000);}
    CR(0x000)=1;{uint32_t t=USEC_TIMER;while((USEC_TIMER-t)<300000);}

    for(int i=0;i<10;i++){
        {int t=100000;while((LR(0x8C)&3)&&--t>0);}
        LR(0x80)=1; LCD_CON=0x80000DA9;
        lc(0x200);ld(0);lc(0x201);ld(0);lc(0x202);
        while(!(LCD_STATUS&0x2)); LCD_CON=0x80100DB0;
        LR(0x80)=0;{int t=500000;while((LR(0x8C)&3)&&--t>0);}
    }

    while(rb->button_get(true)==BUTTON_NONE) rb->backlight_on();

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
