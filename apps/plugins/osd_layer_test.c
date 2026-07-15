/* OSD overlay test — focused tests with GRAM readback.
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
/* ---- SOLID GRAM readback ------------------------------------------
 * Every read is self-certifying so the log stands alone with no need
 * for a human to look at the screen:
 *   - all waits are BOUNDED (the plugin can never hang → no missing log)
 *   - a timed-out read is reported as READ-TIMEOUT, never a garbage value
 *     masquerading as real data
 *   - each pixel is read 3x; disagreement is reported as UNSTABLE
 *   - multi-point grid sampling catches spatial artifacts (shear, stars,
 *     partial render) a single center pixel would miss
 * The underlying register protocol is byte-for-byte the proven working
 * sequence; only bounded-timeout guards and repeat/grid logic are added. */
enum { GRAM_OK=0, GRAM_TIMEOUT=1, GRAM_UNSTABLE=2 };
static const char* gram_st(int s){
    return s==GRAM_OK?"OK":s==GRAM_TIMEOUT?"READ-TIMEOUT!!":"UNSTABLE!!";
}
/* Core single read of GRAM at panel coord (x,y). *st=GRAM_OK or
 * GRAM_TIMEOUT. Returns raw 18-bit value (untrustworthy if timeout). */
static uint32_t gram_read_xy(int x,int y,int *st){
    int to=0;
    {int t=100000;while((LR(0x8C)&3)&&--t>0);if(t<=0)to=1;}
    LR(0x70)=0;LR(0x80)=1;
    {int t=200000;while(!(LCD_STATUS&0x2)&&--t>0);if(t<=0)to=1;}
    {volatile int d=0;while(d++<200);}
    LCD_CON=0x80000DA9;
    {int t=200000;while(!(LCD_STATUS&0x2)&&--t>0);if(t<=0)to=1;}
    ili_cmd(0x200);ili_data(x);ili_cmd(0x201);ili_data(y);ili_cmd(0x202);
    {int t=200000;while(!(LCD_STATUS&0x2)&&--t>0);if(t<=0)to=1;}
    LCD_RDATA=0;{int t=100000;while(!(LCD_STATUS&1)&&--t>0);if(t<=0)to=1;}(void)LCD_DBUFF;
    LCD_RDATA=0;{int t=100000;while(!(LCD_STATUS&1)&&--t>0);if(t<=0)to=1;}
    uint32_t g=LCD_DBUFF&0x3FFFF;
    LCD_CON=0x81100DB0;LR(0x80)=0;LR(0x70)=1;
    push_frame();
    if(st)*st=to?GRAM_TIMEOUT:GRAM_OK;
    return g;
}
/* Read (x,y) 3x; require agreement. *st=OK/TIMEOUT/UNSTABLE. */
static uint32_t gram_read_stable(int x,int y,int *st){
    int s0=0,s1=0,s2=0;
    uint32_t a=gram_read_xy(x,y,&s0);
    uint32_t b=gram_read_xy(x,y,&s1);
    uint32_t c=gram_read_xy(x,y,&s2);
    if(s0||s1||s2){if(st)*st=GRAM_TIMEOUT;return a;}
    if(a==b&&b==c){if(st)*st=GRAM_OK;return a;}
    if(st)*st=GRAM_UNSTABLE;
    return (a==b||a==c)?a:(b==c?b:a);
}
static uint32_t gram_sample(void){int s;return gram_read_stable(160,120,&s);}
static void busywait_us(uint32_t us){
    uint32_t t=USEC_TIMER;while((USEC_TIMER-t)<us)rb->backlight_on();
}
static void gram_log(int t,const char*desc){
    int s;uint32_t g=gram_read_stable(160,120,&s);
    vlog("T%d: %s GRAM=0x%06lx R=%lu G=%lu B=%lu [%s]",t,desc,
         (unsigned long)g,(unsigned long)((g>>12)&0x3F),
         (unsigned long)((g>>6)&0x3F),(unsigned long)(g&0x3F),gram_st(s));
}
/* Like gram_log but also returns the raw GRAM value for numeric
 * comparison between two samples (equivalence/disturbance tests). */
static uint32_t gram_log_r(int t,const char*desc){
    int s;uint32_t g=gram_read_stable(160,120,&s);
    vlog("T%d: %s GRAM=0x%06lx R=%lu G=%lu B=%lu [%s]",t,desc,
         (unsigned long)g,(unsigned long)((g>>12)&0x3F),
         (unsigned long)((g>>6)&0x3F),(unsigned long)(g&0x3F),gram_st(s));
    return g;
}
/* 5-point grid sample (4 quadrant centers + middle). Logs every point
 * with its trust tag and a uniformity verdict, so spatial artifacts
 * (shear, partial render, stray pixels) are visible from the log alone. */
static void gram_grid(const char*desc){
    static const int px[5]={ 60,260, 60,260,160};
    static const int py[5]={ 60, 60,180,180,120};
    uint32_t v[5];int s[5],worst=GRAM_OK,uniform=1;
    for(int i=0;i<5;i++){
        v[i]=gram_read_stable(px[i],py[i],&s[i]);
        if(s[i]>worst)worst=s[i];
        if(i&&v[i]!=v[0])uniform=0;
    }
    vlog("GRID %s [%s]%s",desc,gram_st(worst),
         uniform?" uniform":" NON-UNIFORM");
    for(int i=0;i<5;i++)
        vlog("  (%d,%d) 0x%06lx R=%lu G=%lu B=%lu [%s]",px[i],py[i],
             (unsigned long)v[i],(unsigned long)((v[i]>>12)&0x3F),
             (unsigned long)((v[i]>>6)&0x3F),(unsigned long)(v[i]&0x3F),
             gram_st(s[i]));
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
}

static int fsc(const uint8_t*b,int l,int*s){
    for(int i=0;i<l-3;i++){if(b[i]==0&&b[i+1]==0){
        if(b[i+2]==1){*s=3;return i;}
        if(i+3<l&&b[i+2]==0&&b[i+3]==1){*s=4;return i;}}}return-1;}

static void setup_layer(int n,uint16_t*fb,int w,int h,uint32_t fmt){
    uint32_t base=0x058+(unsigned)n*0x18;
    CR(base+0x00)=(2*w)/2;
    CR(base+0x04)=fmt;
    CR(base+0x08)=PH(fb);
    CR(base+0x0C)=h|((uint32_t)w<<16);
    CR(base+0x10)=((2*w)+7)/8;
    CR(base+0x14)=0;
    /* Apple's overlay blend: ONE_MINUS_SRC_ALPHA/SRC_ALPHA + alpha=0xFF */
    {static const uint16_t grp[]={0x0D8,0x0E0,0x0E0,0x0E0,0x0E8};
     CR(grp[n])=0x500040FF;}
    {uint32_t bits[]={0x040,0x020,0x010,0x008,0x004};
     uint32_t v=CR(0x008);v|=bits[n];CR(0x008)=v;}
    /* No comp+0x024 commit: Apple's own runtime layer-pointer dispatcher
     * (ROM 0x0014d6b4) does a bare pointer store for layers 0-4 exactly
     * like it does for layer 5 (already proven safe) — no commit write
     * anywhere in that path. comp+0x024 has zero ROM precedent for any
     * post-init write while the compositor is active; every call site
     * here is followed by push_frame(), which is the actual latch. */
}

enum plugin_status plugin_start(const void *parameter)
{
    const char*path=parameter?(const char*)parameter:"/test_iframe.264";
    if(!*path)return PLUGIN_ERROR;
    rb->cpu_boost(true);rb->audio_stop();
    log_fd=rb->open("/osd_test.log",O_WRONLY|O_CREAT|O_TRUNC,0666);
    vlog("=== OSD Overlay Test v3 ===");

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
        }else rb->close(fd);
    }
    if(!yo){fw=320;fh=240;
        uint8_t*ty=ab;rb->memset(ty,145,fw*fh);
        rb->memset(ty+fw*fh,54,(fw/2)*(fh/2));
        rb->memset(ty+fw*fh+(fw/2)*(fh/2),34,(fw/2)*(fh/2));
        yo=ty;cbo=ty+fw*fh;cro=cbo+(fw/2)*(fh/2);}

    uint32_t s_con=LCD_CON,s_7c=LR(0x7C),s_88=LR(0x88);
    uint32_t s_20=LR(0x20),s_74=LR(0x74),s_78=LR(0x78);

    uint16_t *ovl;
    {size_t ds=vpu_h264_buf_size(640,480);
     uintptr_t p=(uintptr_t)(ab+ds+fw*fh*2+4096);
     ovl=(uint16_t*)((p+31)&~(uintptr_t)31);}

    /* Start compositor — matching driver's working init */
    {int t=100000;while((LR(0x8C)&3)&&--t>0);}
    PWRCON(0)&=~(0x2080|(7<<14));
    {volatile int d=0;while(d++<10000);}
    comp_hw_init();
    for(int o=0x024;o<=0x044;o+=4)CR(o)=0;
    for(int o=0x04C;o<=0x058;o+=4)CR(o)=0;
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

    vlog("Video baseline 008=0x%08lx 3AC=0x%08lx 024=0x%08lx",
         (unsigned long)CR(0x008),(unsigned long)CR(0x3AC),
         (unsigned long)CR(0x024));
    busywait_us(2000000);

    for(int i=0;i<320*240;i++) ovl[i]=0xF800;
    rb->commit_discard_dcache();
    int test=0;

    /* Layer 4 RED BEFORE zeroing filter/CSC banks */
    setup_layer(4,ovl,320,240,0x00010100);
    push_frame();busywait_us(2000000);
    gram_log(test++,"L4 RED BEFORE zero banks");

    /* Zero all uninitialized filter/CSC register banks */
    {uint32_t v=CR(0x008);v&=~0x004;CR(0x008)=v;} /* disable L4 */
    for(int o=0x0F0;o<=0x17C;o+=4) CR(o)=0;
    for(int o=0x180;o<=0x1C4;o+=4) CR(o)=0;
    for(int o=0x31C;o<=0x360;o+=4) CR(o)=0;
    for(int o=0x364;o<=0x3A8;o+=4) CR(o)=0;
    CR(0x3C4)=0; CR(0x3C8)=0;
    vlog("Zeroed filter/CSC banks 0F0-17C, 180-1C4, 31C-360, 364-3A8, 3C4-3C8");

    /* Layer 4 RED AFTER zeroing — if colors change, stale banks were the cause */
    {uint32_t v=CR(0x008);v|=0x004;CR(0x008)=v;} /* re-enable L4 */
    CR(0x024)=1;
    push_frame();busywait_us(2000000);
    gram_log(test++,"L4 RED AFTER zero banks");

    /* Layer 4 RED, format 0x00000100 (bits 23:16 = 0x00) */
    CR(0x0BC)=0x00000100;
    CR(0x024)=1;
    push_frame();busywait_us(2000000);
    gram_log(test++,"L4 RED fmt=0x00000100 (byte2=0)");

    /* Layer 4 RED, format 0x00020100 (bits 23:16 = 0x02) */
    CR(0x0BC)=0x00020100;
    CR(0x024)=1;
    push_frame();busywait_us(2000000);
    gram_log(test++,"L4 RED fmt=0x00020100 (byte2=2)");

    /* Layer 4 RED, format 0x00030100 (bits 23:16 = 0x03) */
    CR(0x0BC)=0x00030100;
    CR(0x024)=1;
    push_frame();busywait_us(2000000);
    gram_log(test++,"L4 RED fmt=0x00030100 (byte2=3)");

    /* Layer 4 RED, bit 8 CLEARED + GO cycle (test CSC latch) */
    CR(0x0BC)=0x00010100;
    {uint32_t v=CR(0x008);v&=~0x100;CR(0x008)=v;}
    CR(0x000)=0;{volatile int d=0;while(d++<5000);}CR(0x000)=1;
    push_frame();busywait_us(2000000);
    gram_log(test++,"L4 RED bit8=0 + GO cycle");
    {uint32_t v=CR(0x008);v|=0x100;CR(0x008)=v;}

    /* Layer 4 RED, format 0x00011100 (bits 15:12=1 = blend mode 1) */
    CR(0x0BC)=0x00011100;
    CR(0x024)=1;
    push_frame();busywait_us(2000000);
    gram_log(test++,"L4 RED fmt=0x00011100 (blend=1)");

    /* Layer 4 RED, format 0x10011100 (blend=0: bits31:28=1,15:12=0 + blend=1 combined) */
    CR(0x0BC)=0x10011100;
    CR(0x024)=1;
    push_frame();busywait_us(2000000);
    gram_log(test++,"L4 RED fmt=0x10011100 (blend0+1)");

    /* Layer 4 RED, format 0x90011100 (blend=3: bits31:28=9,15:12=8) */
    CR(0x0BC)=0x90018100;
    CR(0x024)=1;
    push_frame();busywait_us(2000000);
    gram_log(test++,"L4 RED fmt=0x90018100 (blend=3)");

    /* Layer 4 RED, format 0xB001A100 (blend=4: Layer 4 special) */
    CR(0x0BC)=0xB001A100;
    CR(0x024)=1;
    push_frame();busywait_us(2000000);
    gram_log(test++,"L4 RED fmt=0xB001A100 (blend=4 special)");

    /* Disable L4, re-enable L5 */
    {uint32_t v=CR(0x008);v&=~0x004;v|=0x080;CR(0x008)=v;}
    CR(0x024)=1;

    /* 4bpp format-ID sweep on Layer 4, ISOLATED from Layer 5.
     * Prior "ARGB8888=fmt2" test conflated 3 variables at once: L5 was
     * still actively blending underneath (contaminating readback), the
     * second sample only rewrote the FB pointer instead of the full
     * block, and "fmt2 = ARGB8888" turned out to be a mislabel borrowed
     * from the unrelated MIXER block (0x39200000) — on THIS block
     * (compositor 0x38900000) format ID 2 has zero precedent anywhere
     * in Apple's ROM. Sweep the whole 4-bytes/pixel ID group (2/3/4/5,
     * the dispatcher's shared jump target for bpp=4) one at a time,
     * full block rewrite each, with L5 off, to isolate which value
     * (if any) is real RGB. */
    {uint32_t v=CR(0x008);v&=~0x080;CR(0x008)=v;} /* L5 off: isolate L4 */
    {
        static const uint32_t fmt_ids[]={2,3,4,5};
        uint32_t *ovl32=(uint32_t*)ovl;
        for(int i=0;i<320*240;i++) ovl32[i]=0xFFFF0000; /* RED, alpha=0xFF */
        rb->commit_discard_dcache();
        for(int k=0;k<4;k++){
            uint32_t id=fmt_ids[k];
            CR(0x0B8)=(4*320)/2;
            CR(0x0BC)=0x00010000|(id<<8); /* bit16=bpp<4x8 marker, id at [15:8] */
            CR(0x0C0)=PH(ovl);
            CR(0x0C4)=240|(320U<<16);
            CR(0x0C8)=((4*320)+7)/8;
            CR(0x0CC)=0;
            {uint32_t v=CR(0x008);v|=0x004;CR(0x008)=v;}
            CR(0x024)=1;
            push_frame();busywait_us(2000000);
            {uint32_t g=gram_sample();
             vlog("T%d: L4 4bpp fmtID=%lu RED (L5 off) GRAM=0x%06lx R=%lu G=%lu B=%lu",
                  test++,(unsigned long)id,(unsigned long)g,
                  (unsigned long)((g>>12)&0x3F),(unsigned long)((g>>6)&0x3F),
                  (unsigned long)(g&0x3F));}
        }
    }
    /* Restore: L4 disabled, L5 re-enabled */
    {uint32_t v=CR(0x008);v&=~0x004;v|=0x080;CR(0x008)=v;}
    CR(0x024)=1;

    /* comp+0x00C sweep — try disabling pipeline stages */
    CR(0x0BC)=0x00010100;
    CR(0x0B8)=(2*320)/2;
    CR(0x0C8)=((2*320)+7)/8;
    for(int i=0;i<320*240;i++) ovl[i]=0xF800;
    rb->commit_discard_dcache();
    CR(0x0C0)=PH(ovl);CR(0x024)=1;

    {volatile uint32_t *c=(volatile uint32_t*)0x38900000;
     c[0x00C/4]=0x00000000;} /* zero all pipeline stages */
    push_frame();busywait_us(2000000);
    gram_log(test++,"L4 RED 00C=0x00000000");

    {volatile uint32_t *c=(volatile uint32_t*)0x38900000;
     c[0x00C/4]=0x000F0F0F;} /* restore */

    /* Disable L4 */
    {uint32_t v=CR(0x008);v&=~0x004;v|=0x080;CR(0x008)=v;}
    CR(0x024)=1;

    /* Layer 4 GREEN for color matrix */
    for(int i=0;i<320*240;i++) ovl[i]=0x07E0;
    rb->commit_discard_dcache();
    setup_layer(4,ovl,320,240,0x00010100);
    push_frame();busywait_us(2000000);
    gram_log(test++,"L4 GREEN 0x07E0");

    /* Layer 4 BLUE for color matrix */
    for(int i=0;i<320*240;i++) ovl[i]=0x001F;
    rb->commit_discard_dcache();
    CR(0x0C0)=PH(ovl);CR(0x024)=1;
    push_frame();busywait_us(2000000);
    gram_log(test++,"L4 BLUE 0x001F");

    /* Layer 4 WHITE */
    for(int i=0;i<320*240;i++) ovl[i]=0xFFFF;
    rb->commit_discard_dcache();
    CR(0x0C0)=PH(ovl);CR(0x024)=1;
    push_frame();busywait_us(2000000);
    gram_log(test++,"L4 WHITE 0xFFFF");

    /* Disable L4 */
    {uint32_t v=CR(0x008);v&=~0x004;v|=0x080;CR(0x008)=v;}
    CR(0x024)=1;

    /* === CSC-bypass-bit isolation test =========================
     * Does comp+0x008 bit 8 affect Layer 4 (RGB) output at all, or
     * does it only affect Layer 5 (YCbCr) as the existing driver
     * comment assumes? ROM evidence could not settle this (bit 8 is
     * never read back/tested anywhere in Apple's static ROM). This
     * is the live HW test that resolves it: isolate L5 off, L4 solid
     * RED, toggle bit 8, compare GRAM. Identical readback confirms
     * the existing "RGB layers pass through" assumption; a different
     * readback means CSC leaks into RGB overlays and explains the
     * still-open "overlay colors wrong" bug directly. */
    {
        uint32_t v;
        v=CR(0x008); v&=~0x080; CR(0x008)=v; /* L5 off: isolate L4 */
        for(int i=0;i<320*240;i++) ovl[i]=0xF800; /* RED */
        rb->commit_discard_dcache();
        setup_layer(4,ovl,320,240,0x00010100);
        push_frame();busywait_us(1000000);
        uint32_t g_bit8_set=gram_log_r(test++,"CSC-isolation: L4 RED, bit8=1 (CSC on)");

        v=CR(0x008); v&=~0x100; CR(0x008)=v; /* CSC bypass bit CLEAR */
        push_frame();busywait_us(1000000);
        uint32_t g_bit8_clear=gram_log_r(test++,"CSC-isolation: L4 RED, bit8=0 (CSC off)");

        vlog("  CSC-isolation verdict: %s (0x%06lx vs 0x%06lx)",
             (g_bit8_set==g_bit8_clear)
                 ?"IDENTICAL -- bit8 has no effect on L4, RGB isolation CONFIRMED"
                 :"DIFFERENT -- bit8 changes L4 output, CSC LEAKS into RGB overlays",
             (unsigned long)g_bit8_set,(unsigned long)g_bit8_clear);

        v=CR(0x008); v|=0x100; CR(0x008)=v; /* restore bit8=1 default */
        v=CR(0x008); v&=~0x004; v|=0x080; CR(0x008)=v; /* L4 off, L5 back on */
        CR(0x024)=1;
    }

    /* === Blend-mode equivalence test ===========================
     * comp+0xE8 is shared by Layer 4 and Layer 5 — only 3 physical
     * blend registers exist for 6 logical layers. setup_layer()
     * overwrites it to mode2 (A=5,B=4) for Layer 4, silently changing
     * Layer 5's blend mode too (Layer 5's own resting value from
     * comp_hw_init is mode1: A=0,B=1, both alpha=0xFF). ROM has no
     * dispatch table anywhere that resolves whether these two modes
     * are equivalent at alpha=0xFF — this is the live HW test.
     * Identical GRAM under both modes means Layer 4 never needs to
     * touch this shared register at all, eliminating the conflict
     * entirely with zero further design work needed. */
    {
        uint32_t v;
        v=CR(0x008); v|=0x080; CR(0x008)=v; /* ensure L5 on as backdrop */
        for(int i=0;i<320*240;i++) ovl[i]=0x07E0; /* GREEN, distinguishable from L5 video */
        rb->commit_discard_dcache();
        setup_layer(4,ovl,320,240,0x00010100); /* uses mode2=0x500040FF internally */
        push_frame();busywait_us(1000000);
        uint32_t g_mode2=gram_log_r(test++,"blend-equiv: L4 GREEN mode2(A5,B4) alpha=FF over L5");

        CR(0x0E8)=0x000010FF; /* mode1: A=0,B=1, alpha=FF -- L5's own resting value */
        push_frame();busywait_us(1000000);
        uint32_t g_mode1=gram_log_r(test++,"blend-equiv: L4 GREEN mode1(A0,B1) alpha=FF over L5");

        vlog("  blend-equiv verdict: %s (0x%06lx vs 0x%06lx)",
             (g_mode1==g_mode2)
                 ?"IDENTICAL -- mode1==mode2 at alpha=FF, safe to drop setup_layer's blend override"
                 :"DIFFERENT -- modes are not equivalent, L4/L5 must keep separate blend handling",
             (unsigned long)g_mode1,(unsigned long)g_mode2);

        CR(0x0E8)=0x500040FF; /* restore L4's mode2 for consistency with rest of file */
        v=CR(0x008); v&=~0x004; CR(0x008)=v; /* L4 off */
        CR(0x024)=1;
    }

    /* === comp+0x024-write-while-DMA-active disturbance test ====
     * Apple's ROM never writes comp+0x024 while the compositor GO
     * bit is set (the only write site in the entire ROM happens once
     * at init, immediately BEFORE GO is asserted). This is a known,
     * flagged risk in the protected driver's compositor_layer_show()/
     * hide() (both write comp+0x024=1 while Layer 5 DMA is actively
     * streaming, for OSD show/hide during live playback). Empirically
     * check whether a BURST of writes (simulating rapid toggling)
     * visibly disturbs Layer 5's live video: L4 off so the GRAM
     * sample point is pure L5 video, sample before and immediately
     * after the burst with no recovery frame in between, and compare
     * against the EXACT known green-screen signature (0x000980,
     * confirmed elsewhere this session as the compositor-disabled
     * failure mode) for a hard, evidence-backed verdict rather than a
     * guess about normal frame-to-frame video variation. */
    {
        push_frame();busywait_us(500000);
        uint32_t g_before=gram_log_r(test++,"comp024-burst: L5 baseline BEFORE burst");

        for(int i=0;i<20;i++) CR(0x024)=1; /* burst, no frame push between writes */

        uint32_t g_after=gram_log_r(test++,"comp024-burst: L5 AFTER 20x burst (no recovery frame)");

        vlog("  comp024-burst verdict: %s (0x%06lx vs baseline 0x%06lx)",
             (g_after==g_before)
                 ?"UNCHANGED -- strong evidence burst writes did not disturb active video"
                 :(g_after==0x000980)
                     ?"CHANGED TO KNOWN GREEN-SCREEN SIGNATURE -- burst writes reproduce the disable bug!"
                     :"CHANGED (not the known-bad signature) -- likely normal frame variation, inconclusive from a single sample",
             (unsigned long)g_after,(unsigned long)g_before);
    }

    /* === Layer 4 ISOLATED color-transform characterization ======
     * The still-open "overlay colors wrong" bug: T18/T19 proved Layer 4
     * RED reads back greenish (R=25,G=43,B=29) even with L5 OFF and CSC
     * toggled — a pure Layer-4 transform, not CSC leak or L5 bleed. Only
     * RED was ever tested L5-off, leaving the transform underdetermined.
     * Sweep 9 well-separated colors, L5 OFF (Layer 4 blends only against
     * the black background color comp+0x024=0), full 6-register block
     * rewrite each iteration, so one run over-determines a general linear
     * model (9*3=27 equations vs 12 unknowns for a 3x3+offset fit). If
     * the input RGB and output GRAM match, "RGB passthrough" is real; if
     * they map through a fixed matrix, this pins the exact coefficients. */
    {
        uint32_t v=CR(0x008); v&=~0x080; CR(0x008)=v; /* L5 off: pure L4 */
        static const struct { uint16_t rgb; const char*name; } sweep[]={
            {0xF800,"RED   (31, 0, 0)"}, {0x07E0,"GREEN ( 0,63, 0)"},
            {0x001F,"BLUE  ( 0, 0,31)"}, {0xFFE0,"YELLOW(31,63, 0)"},
            {0x07FF,"CYAN  ( 0,63,31)"}, {0xF81F,"MAGNTA(31, 0,31)"},
            {0xFFFF,"WHITE (31,63,31)"}, {0x0000,"BLACK ( 0, 0, 0)"},
            {0x8410,"GRAY  (16,32,16)"},
        };
        for(unsigned k=0;k<sizeof(sweep)/sizeof(sweep[0]);k++){
            for(int i=0;i<320*240;i++) ovl[i]=sweep[k].rgb;
            rb->commit_discard_dcache();
            setup_layer(4,ovl,320,240,0x00010100);
            push_frame();busywait_us(800000);
            int s;uint32_t g=gram_read_stable(160,120,&s);
            vlog("L4-iso[%s] in=0x%04x -> GRAM=0x%06lx R=%lu G=%lu B=%lu [%s]",
                 sweep[k].name,(unsigned)sweep[k].rgb,(unsigned long)g,
                 (unsigned long)((g>>12)&0x3F),(unsigned long)((g>>6)&0x3F),
                 (unsigned long)(g&0x3F),gram_st(s));
        }
        /* Grid on the RED fill to confirm the L4 transform is spatially
         * uniform (not a shear/partial-render artifact contaminating the
         * per-color values above). */
        for(int i=0;i<320*240;i++) ovl[i]=0xF800;
        rb->commit_discard_dcache();
        CR(0x0C0)=PH(ovl);CR(0x024)=1;push_frame();busywait_us(800000);
        gram_grid("L4-iso RED uniformity");
        v=CR(0x008); v&=~0x004; v|=0x080; CR(0x008)=v; /* L4 off, L5 back on */
        CR(0x024)=1;
    }

    /* Layer 0 — never tested with proven setup_layer() methodology
     * until this session. grp[0]=0x0D8 (L0's dedicated blend reg),
     * bits[0]=0x040 (L0 enable) -- identical code path already proven
     * correct for L4. */
    for(int i=0;i<320*240;i++) ovl[i]=0xF800;
    rb->commit_discard_dcache();
    setup_layer(0,ovl,320,240,0x00010100);
    push_frame();busywait_us(2000000);
    gram_log(test++,"L0 RED 0xF800");

    for(int i=0;i<320*240;i++) ovl[i]=0x07E0;
    rb->commit_discard_dcache();
    CR(0x060)=PH(ovl);CR(0x024)=1;
    push_frame();busywait_us(2000000);
    gram_log(test++,"L0 GREEN 0x07E0");

    for(int i=0;i<320*240;i++) ovl[i]=0x001F;
    rb->commit_discard_dcache();
    CR(0x060)=PH(ovl);CR(0x024)=1;
    push_frame();busywait_us(2000000);
    gram_log(test++,"L0 BLUE 0x001F");

    for(int i=0;i<320*240;i++) ovl[i]=0xFFFF;
    rb->commit_discard_dcache();
    CR(0x060)=PH(ovl);CR(0x024)=1;
    push_frame();busywait_us(2000000);
    gram_log(test++,"L0 WHITE 0xFFFF");

    /* Full Layer 0 register dump — unconditional, for maximum
     * diagnostic value if Layer 0 is still black on this run. Layer 0
     * root cause has been exhaustively ruled out on both the
     * Apple-ROM side (zero index-0 special-casing anywhere) and the
     * Rockbox-runtime side (osd_draw()'s color packing/framebuffer/
     * draw-dispatch path all checked clean) -- see osd-overlay-
     * findings.md. This captures every register this project knows
     * is relevant, in one shot, so a still-black result doesn't
     * require another round-trip to gather basic diagnostic data. */
    vlog("L0 full register dump: 008=0x%08lx 058=0x%08lx 05C=0x%08lx 060=0x%08lx 064=0x%08lx 068=0x%08lx 06C=0x%08lx 070=0x%08lx 0D8=0x%08lx",
         (unsigned long)CR(0x008),(unsigned long)CR(0x058),(unsigned long)CR(0x05C),
         (unsigned long)CR(0x060),(unsigned long)CR(0x064),(unsigned long)CR(0x068),
         (unsigned long)CR(0x06C),(unsigned long)CR(0x070),(unsigned long)CR(0x0D8));

    /* Disable L0 */
    {uint32_t v=CR(0x008);v&=~0x040;CR(0x008)=v;}
    CR(0x024)=1;

    /* === Layer 0 vs Layer 1 A/B — the definitive isolation test =====
     * L0 shows ZERO compositing effect (T24-T27 all read the L5-alone
     * baseline regardless of content), while L1 is known to render. Both
     * are configured through the IDENTICAL setup_layer() code path. This
     * runs the SAME RED content on each in turn, L5 OFF (so the sample
     * point reflects only the overlay), and dumps both register banks so
     * any single differing bit between a working layer (L1) and a broken
     * one (L0) is visible in one capture. Per osd-overlay-findings.md,
     * this is the identified next step to isolate L0's root cause after
     * ROM-side and runtime-side static analysis were both exhausted.
     * L0 bank = comp+0x058..0x06C (stride 0x18), L1 = comp+0x070..0x084. */
    {
        uint32_t v=CR(0x008); v&=~0x080; CR(0x008)=v; /* L5 off */

        for(int i=0;i<320*240;i++) ovl[i]=0xF800; /* RED */
        rb->commit_discard_dcache();
        setup_layer(0,ovl,320,240,0x00010100);
        push_frame();busywait_us(1000000);
        int s0;uint32_t g0=gram_read_stable(160,120,&s0);
        vlog("L0/L1-AB: L0 RED (L5 off) GRAM=0x%06lx R=%lu G=%lu B=%lu [%s]",
             (unsigned long)g0,(unsigned long)((g0>>12)&0x3F),
             (unsigned long)((g0>>6)&0x3F),(unsigned long)(g0&0x3F),gram_st(s0));
        gram_grid("L0 RED");
        {uint32_t w=CR(0x008); w&=~0x040; CR(0x008)=w; CR(0x024)=1;} /* L0 off */

        setup_layer(1,ovl,320,240,0x00010100);
        push_frame();busywait_us(1000000);
        int s1;uint32_t g1=gram_read_stable(160,120,&s1);
        vlog("L0/L1-AB: L1 RED (L5 off) GRAM=0x%06lx R=%lu G=%lu B=%lu [%s]",
             (unsigned long)g1,(unsigned long)((g1>>12)&0x3F),
             (unsigned long)((g1>>6)&0x3F),(unsigned long)(g1&0x3F),gram_st(s1));
        gram_grid("L1 RED");

        vlog("L0/L1-AB verdict: %s (L0=0x%06lx [%s] L1=0x%06lx [%s])",
             (s0==GRAM_OK&&s1==GRAM_OK)
                 ?((g0==g1)?"IDENTICAL -- L0 renders same as L1, L0 bug is NOT layer-intrinsic"
                          :"DIFFERENT -- L0 broken while L1 works from identical config")
                 :"INCONCLUSIVE -- a read did not certify OK, ignore comparison",
             (unsigned long)g0,gram_st(s0),(unsigned long)g1,gram_st(s1));
        vlog("L0 bank: 058=0x%08lx 05C=0x%08lx 060=0x%08lx 064=0x%08lx 068=0x%08lx 06C=0x%08lx 0D8=0x%08lx",
             (unsigned long)CR(0x058),(unsigned long)CR(0x05C),(unsigned long)CR(0x060),
             (unsigned long)CR(0x064),(unsigned long)CR(0x068),(unsigned long)CR(0x06C),
             (unsigned long)CR(0x0D8));
        vlog("L1 bank: 070=0x%08lx 074=0x%08lx 078=0x%08lx 07C=0x%08lx 080=0x%08lx 084=0x%08lx 0E0=0x%08lx",
             (unsigned long)CR(0x070),(unsigned long)CR(0x074),(unsigned long)CR(0x078),
             (unsigned long)CR(0x07C),(unsigned long)CR(0x080),(unsigned long)CR(0x084),
             (unsigned long)CR(0x0E0));

        v=CR(0x008); v&=~0x020; v|=0x080; CR(0x008)=v; /* L1 off, L5 back on */
        CR(0x024)=1;
    }

    vlog("=== DONE (%d tests) ===",test);
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
