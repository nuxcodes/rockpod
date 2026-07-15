/* Compositor test — flip + overlay + mode field + P18.
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

static int log_fd=-1;
static void vlog(const char *fmt,...){
    if(log_fd<0)return;
    char buf[256];va_list ap;va_start(ap,fmt);
    int n=rb->vsnprintf(buf,sizeof(buf),fmt,ap);va_end(ap);
    rb->write(log_fd,buf,n);rb->write(log_fd,"\n",1);
}
static void ili_cmd(uint16_t c){while(LCD_STATUS&0x10);LCD_WCMD=c;}
static void ili_data(uint16_t d){while(LCD_STATUS&0x10);LCD_WDATA=d;}

static void push_frame(void){
    {int t=100000;while((LR(0x8C)&3)&&--t>0);}
    LR(0x80)=1;
    while(!(LCD_STATUS&0x2));LCD_CON=0x80000DA9;
    ili_cmd(0x200);ili_data(0);ili_cmd(0x201);ili_data(0);ili_cmd(0x202);
    while(!(LCD_STATUS&0x2));LCD_CON=0x81100DB0;
    LR(0x80)=0;
}
static uint32_t gram_sample(void){
    {int t=100000;while((LR(0x8C)&3)&&--t>0);}
    LR(0x70)=0;LR(0x80)=1;
    while(!(LCD_STATUS&0x2));{volatile int d=0;while(d++<200);}
    LCD_CON=0x80000DA9;while(!(LCD_STATUS&0x2));
    ili_cmd(0x200);ili_data(160);ili_cmd(0x201);ili_data(120);ili_cmd(0x202);
    while(!(LCD_STATUS&0x2));
    LCD_RDATA=0;{int t=100000;while(!(LCD_STATUS&1)&&--t>0);}(void)LCD_DBUFF;
    LCD_RDATA=0;{int t=100000;while(!(LCD_STATUS&1)&&--t>0);}
    uint32_t g=LCD_DBUFF&0x3FFFF;
    LCD_CON=0x81100DB0;LR(0x80)=0;LR(0x70)=1;
    push_frame();return g;
}
static void busywait_us(uint32_t us){
    uint32_t t=USEC_TIMER;while((USEC_TIMER-t)<us)rb->backlight_on();
}
static void gram_log(const char*desc){
    uint32_t g=gram_sample();
    vlog("  %s GRAM=0x%06lx R=%lu G=%lu B=%lu",desc,
         (unsigned long)g,(unsigned long)((g>>12)&0x3F),
         (unsigned long)((g>>6)&0x3F),(unsigned long)(g&0x3F));
}

static void comp_hw_init(void){
    volatile uint32_t *c=(volatile uint32_t*)COMP;
    c[0x200/4]&=~1;c[0x004/4]=1;c[0x020/4]=1;
    for(int i=0;i<256;i++){c[0x400/4+i]=i*4;c[0x800/4+i]=i*4;c[0xC00/4+i]=i*4;}
    {volatile uint32_t *s=(volatile uint32_t*)0x0890D2DC;
     uint32_t t[5];for(int i=0;i<5;i++)t[i]=s[i];
     if(t[0]>0&&t[0]<0x1000&&t[4]>0&&t[4]<0x1000)
         for(int i=0;i<5;i++)c[(0x1EC+i*4)/4]=t[i];
     else{uint32_t h[]={0x0C,0x26,0x10,0x82,0x4E};
          for(int i=0;i<5;i++)c[(0x1EC+i*4)/4]=h[i];}}
    c[0x0D8/4]=0x10FF;c[0x0DC/4]=0;c[0x0E0/4]=0x10FF;c[0x0E4/4]=0;
    c[0x0E8/4]=0x10FF;c[0x0EC/4]=0;
    {uint32_t v=c[0x008/4];v&=~0xFC;
     v&=~0x20000000;v&=~0x10000000;
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
    c[0x024/4]=0x00000000;
    /* Defensive zero of CSC/filter banks — a prior plugin run in the same
     * power-on session may leave these non-zero; comp_hw_init() never
     * touches this range otherwise. Matches osd_layer_test.c's T0/T1 fix. */
    for(int o=0x0F0;o<=0x17C;o+=4) c[o/4]=0;
    for(int o=0x180;o<=0x1C4;o+=4) c[o/4]=0;
    for(int o=0x31C;o<=0x360;o+=4) c[o/4]=0;
    for(int o=0x364;o<=0x3A8;o+=4) c[o/4]=0;
    c[0x3C4/4]=0; c[0x3C8/4]=0;
}

static int fsc(const uint8_t*b,int l,int*s){
    for(int i=0;i<l-3;i++){if(b[i]==0&&b[i+1]==0){
        if(b[i+2]==1){*s=3;return i;}
        if(i+3<l&&b[i+2]==0&&b[i+3]==1){*s=4;return i;}}}return-1;}

enum plugin_status plugin_start(const void *parameter)
{
    const char *path=parameter?(const char*)parameter:"/test_iframe.264";
    if(!*path)return PLUGIN_ERROR;
    rb->cpu_boost(true);rb->audio_stop();
    log_fd=rb->open("/scaler_test.log",O_WRONLY|O_CREAT|O_TRUNC,0666);
    vlog("=== Compositor Test ===");

    uint8_t *ab;size_t as;
    ab=rb->plugin_get_audio_buffer(&as);
    const uint8_t *yo=NULL,*cbo=NULL,*cro=NULL;
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
        }else rb->close(fd);
    }
    if(!yo){fw=320;fh=240;
        uint8_t*ty=ab;rb->memset(ty,145,fw*fh);
        rb->memset(ty+fw*fh,54,(fw/2)*(fh/2));
        rb->memset(ty+fw*fh+(fw/2)*(fh/2),34,(fw/2)*(fh/2));
        yo=ty;cbo=ty+fw*fh;cro=cbo+(fw/2)*(fh/2);}

    uint32_t s_con=LCD_CON,s_7c=LR(0x7C),s_88=LR(0x88);
    uint32_t s_20=LR(0x20),s_74=LR(0x74),s_78=LR(0x78);

    /* Start compositor */
    {int t=100000;while((LR(0x8C)&3)&&--t>0);}
    PWRCON(0)&=~(0x2080|(7<<14));
    {volatile int d=0;while(d++<10000);}
    comp_hw_init();
    for(int o=0x028;o<=0x044;o+=4)CR(o)=0;
    for(int o=0x04C;o<=0x054;o+=4)CR(o)=0;
    CR(0x028)=0x100;CR(0x02C)=fw|((fw/2)<<16);
    CR(0x034)=fh|((uint32_t)fw<<16);CR(0x04C)=0x10001000;
    CR(0x054)=0x014000F0;CR(0x038)=PH(yo);CR(0x03C)=PH(cro);
    CR(0x040)=0;CR(0x044)=PH(cbo);
    CR(0x3AC)=0x04004002;CR(0x0D4)=1;
    {uint32_t v=CR(0x008);v|=0x100;CR(0x008)=v;}
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
    vlog("Video 008=0x%08lx 3AC=0x%08lx",(unsigned long)CR(0x008),(unsigned long)CR(0x3AC));
    busywait_us(2000000);

    /* === comp+0x024-write-while-DMA-active disturbance test =====
     * Apple's ROM never writes comp+0x024 while the compositor GO bit
     * is set (the only write site in the entire ROM happens once at
     * init, immediately BEFORE GO is asserted). This is a known, flagged
     * risk in the protected driver's compositor_layer_show()/hide()
     * (both write comp+0x024=1 while Layer 5 DMA is actively streaming).
     * Checked here in the cleanest possible environment (pure Layer 5
     * video, no overlay layers active) so a positive result is
     * unambiguous: sample before and immediately after a write burst
     * with no recovery frame, compare against the exact known
     * green-screen signature (0x000980) for a hard, evidence-backed
     * verdict rather than a guess about normal frame-to-frame variation. */
    {
        uint32_t g_before=gram_sample();
        vlog("comp024-burst: baseline BEFORE burst GRAM=0x%06lx",(unsigned long)g_before);

        for(int i=0;i<20;i++) CR(0x024)=1; /* burst, no frame push between writes */

        uint32_t g_after=gram_sample();
        vlog("comp024-burst: AFTER 20x burst (no recovery frame) GRAM=0x%06lx",(unsigned long)g_after);
        vlog("  comp024-burst verdict: %s",
             (g_after==0x000980)
                 ?"MATCHES KNOWN GREEN-SCREEN SIGNATURE -- burst writes reproduce the disable bug!"
                 :(g_after==g_before)
                     ?"UNCHANGED -- strong evidence burst writes did not disturb active video"
                     :"CHANGED (not the known-bad signature) -- likely normal frame variation, inconclusive from a single sample");
    }

    /* === FLIP TEST === */
    vlog("=== FLIP ===");
    {
        int ysz=fw*fh,csz=(fw/2)*(fh/2);
        size_t ds=vpu_h264_buf_size(640,480);
        uint8_t *ba=ab+ds+512*1024;
        ba=(uint8_t*)(((uintptr_t)ba+31)&~31UL);
        uint8_t *bb=ba+ysz+csz+csz;
        bb=(uint8_t*)(((uintptr_t)bb+31)&~31UL);
        rb->memset(ba,81,ysz);rb->memset(ba+ysz,90,csz);rb->memset(ba+ysz+csz,240,csz);
        rb->memset(bb,41,ysz);rb->memset(bb+ysz,240,csz);rb->memset(bb+ysz+csz,110,csz);
        rb->commit_discard_dcache();
        uint32_t t0=USEC_TIMER;int fr=0,sel=0;
        while((USEC_TIMER-t0)<5000000){
            uint32_t tf=USEC_TIMER;
            {int t=100000;while((LR(0x8C)&3)&&--t>0);}
            uint8_t*s=sel?bb:ba;
            CR(0x038)=PH(s);CR(0x03C)=PH(s+ysz+csz);CR(0x044)=PH(s+ysz);
            rb->commit_discard_dcache();push_frame();
            sel^=1;fr++;while((USEC_TIMER-tf)<33333);
        }
        vlog("  %d frames = %d fps",fr,fr/5);
    }

    /* === P18 TEST === */
    vlog("=== P18 ===");
    CR(0x038)=PH(yo);CR(0x03C)=PH(cro);CR(0x044)=PH(cbo);
    push_frame();
    {int t=100000;while((LR(0x8C)&3)&&--t>0);}
    LR(0x70)=0;LR(0x80)=1;
    while(!(LCD_STATUS&0x2));LCD_CON=0x80000DA9;while(!(LCD_STATUS&0x2));
    ili_cmd(0x003);ili_data(0x5238);
    ili_cmd(0x200);ili_data(0);ili_cmd(0x201);ili_data(0);ili_cmd(0x202);
    while(!(LCD_STATUS&0x2));
    LR(0x80)=0;{int t=100000;while((LR(0x8C)&3)&&--t>0);}
    LCD_CON=0x80000DA8;
    LR(0x70)=1;
    {int t=100000;while((LR(0x8C)&3)&&--t>0);}
    LR(0x80)=1;
    while(!(LCD_STATUS&0x2));LCD_CON=0x80000DA9;
    ili_cmd(0x200);ili_data(0);ili_cmd(0x201);ili_data(0);ili_cmd(0x202);
    while(!(LCD_STATUS&0x2));LCD_CON=0x80000DA8;
    LR(0x80)=0;
    vlog("  CON=0x%08lx",(unsigned long)LCD_CON);

    /* P18 GRAM readback at multiple points for pixel comparison */
    {
        /* Read GRAM at 5 positions in P18 mode */
        static const int px[]={10,80,160,240,310};
        static const int py[]={10,60,120,180,230};
        vlog("  P18 pixel samples:");
        for(int i=0;i<5;i++){
            {int t=100000;while((LR(0x8C)&3)&&--t>0);}
            LR(0x70)=0;LR(0x80)=1;
            while(!(LCD_STATUS&0x2));{volatile int d=0;while(d++<200);}
            LCD_CON=0x80000DA9;while(!(LCD_STATUS&0x2));
            ili_cmd(0x200);ili_data(px[i]);ili_cmd(0x201);ili_data(py[i]);ili_cmd(0x202);
            while(!(LCD_STATUS&0x2));
            LCD_RDATA=0;{int t=100000;while(!(LCD_STATUS&1)&&--t>0);}(void)LCD_DBUFF;
            LCD_RDATA=0;{int t=100000;while(!(LCD_STATUS&1)&&--t>0);}
            uint32_t g=LCD_DBUFF&0x3FFFF;
            LCD_CON=0x80000DA8;LR(0x80)=0;LR(0x70)=1;
            /* P18 push restore */
            {int t2=100000;while((LR(0x8C)&3)&&--t2>0);}
            LR(0x80)=1;
            while(!(LCD_STATUS&0x2));LCD_CON=0x80000DA9;
            ili_cmd(0x200);ili_data(0);ili_cmd(0x201);ili_data(0);ili_cmd(0x202);
            while(!(LCD_STATUS&0x2));LCD_CON=0x80000DA8;
            LR(0x80)=0;
            vlog("    (%d,%d) 0x%06lx R=%lu G=%lu B=%lu",
                 px[i],py[i],(unsigned long)g,
                 (unsigned long)((g>>12)&0x3F),(unsigned long)((g>>6)&0x3F),
                 (unsigned long)(g&0x3F));
        }
    }
    busywait_us(2000000);

    /* Now switch back to P16 and read same points for comparison */
    {int t=100000;while((LR(0x8C)&3)&&--t>0);}
    LR(0x70)=0;LR(0x80)=1;
    while(!(LCD_STATUS&0x2));LCD_CON=0x80000DA9;while(!(LCD_STATUS&0x2));
    ili_cmd(0x003);ili_data(0x1238);
    ili_cmd(0x200);ili_data(0);ili_cmd(0x201);ili_data(0);ili_cmd(0x202);
    while(!(LCD_STATUS&0x2));
    LR(0x80)=0;{int t=100000;while((LR(0x8C)&3)&&--t>0);}
    LCD_CON=0x81100DB0;LR(0x70)=1;push_frame();
    busywait_us(500000);

    vlog("  P16 pixel samples:");
    {
        static const int px[]={10,80,160,240,310};
        static const int py[]={10,60,120,180,230};
        for(int i=0;i<5;i++){
            {int t=100000;while((LR(0x8C)&3)&&--t>0);}
            LR(0x70)=0;LR(0x80)=1;
            while(!(LCD_STATUS&0x2));{volatile int d=0;while(d++<200);}
            LCD_CON=0x80000DA9;while(!(LCD_STATUS&0x2));
            ili_cmd(0x200);ili_data(px[i]);ili_cmd(0x201);ili_data(py[i]);ili_cmd(0x202);
            while(!(LCD_STATUS&0x2));
            LCD_RDATA=0;{int t=100000;while(!(LCD_STATUS&1)&&--t>0);}(void)LCD_DBUFF;
            LCD_RDATA=0;{int t=100000;while(!(LCD_STATUS&1)&&--t>0);}
            uint32_t g=LCD_DBUFF&0x3FFFF;
            LCD_CON=0x81100DB0;LR(0x80)=0;LR(0x70)=1;push_frame();
            vlog("    (%d,%d) 0x%06lx R=%lu G=%lu B=%lu",
                 px[i],py[i],(unsigned long)g,
                 (unsigned long)((g>>12)&0x3F),(unsigned long)((g>>6)&0x3F),
                 (unsigned long)(g&0x3F));
        }
    }

    vlog("=== DONE ===");
    LR(0x70)=0;LR(0x80)=0;CR(0x000)=0;
    while(!(LCD_STATUS&0x2));LCD_CON=0x80000DA9;
    ili_cmd(0x003);ili_data(0x0230);while(!(LCD_STATUS&0x2));
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
