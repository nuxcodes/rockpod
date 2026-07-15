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
static void gram_log(int t,const char*desc){
    uint32_t g=gram_sample();
    vlog("T%d: %s GRAM=0x%06lx R=%lu G=%lu B=%lu",t,desc,
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
    c[0x0D8/4]=0x1000;c[0x0DC/4]=0;c[0x0E0/4]=0x1000;c[0x0E4/4]=0;
    c[0x0E8/4]=0x1000;c[0x0EC/4]=0;
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
    {uint32_t bits[]={0x040,0x020,0x010,0x008,0x004};
     uint32_t v=CR(0x008);v|=bits[n];CR(0x008)=v;}
    CR(0x024)=1;
}

enum plugin_status plugin_start(const void *parameter)
{
    const char*path=parameter?(const char*)parameter:"/test_iframe.264";
    if(!*path)return PLUGIN_ERROR;
    rb->cpu_boost(true);rb->audio_stop();
    log_fd=rb->open("/osd_test.log",O_WRONLY|O_CREAT|O_TRUNC,0666);
    vlog("=== OSD Overlay Test v2 ===");

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

    /* T0: Layer 4 RED BEFORE zeroing filter/CSC banks */
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

    /* T1: Layer 4 RED AFTER zeroing — if colors change, stale banks were the cause */
    {uint32_t v=CR(0x008);v|=0x004;CR(0x008)=v;} /* re-enable L4 */
    CR(0x024)=1;
    push_frame();busywait_us(2000000);
    gram_log(test++,"L4 RED AFTER zero banks");

    /* T2: Layer 4 RED, format 0x00000100 (bits 23:16 = 0x00) */
    CR(0x0BC)=0x00000100;
    CR(0x024)=1;
    push_frame();busywait_us(2000000);
    gram_log(test++,"L4 RED fmt=0x00000100 (byte2=0)");

    /* T2: Layer 4 RED, format 0x00020100 (bits 23:16 = 0x02) */
    CR(0x0BC)=0x00020100;
    CR(0x024)=1;
    push_frame();busywait_us(2000000);
    gram_log(test++,"L4 RED fmt=0x00020100 (byte2=2)");

    /* T3: Layer 4 RED, format 0x00030100 (bits 23:16 = 0x03) */
    CR(0x0BC)=0x00030100;
    CR(0x024)=1;
    push_frame();busywait_us(2000000);
    gram_log(test++,"L4 RED fmt=0x00030100 (byte2=3)");

    /* T4: Layer 4 RED, bit 8 CLEARED + GO cycle (test CSC latch) */
    CR(0x0BC)=0x00010100;
    {uint32_t v=CR(0x008);v&=~0x100;CR(0x008)=v;}
    CR(0x000)=0;{volatile int d=0;while(d++<5000);}CR(0x000)=1;
    push_frame();busywait_us(2000000);
    gram_log(test++,"L4 RED bit8=0 + GO cycle");
    {uint32_t v=CR(0x008);v|=0x100;CR(0x008)=v;}

    /* Disable L4, re-enable L5 */
    {uint32_t v=CR(0x008);v&=~0x004;v|=0x080;CR(0x008)=v;}
    CR(0x024)=1;

    /* T5: Layer 4 GREEN for color matrix */
    for(int i=0;i<320*240;i++) ovl[i]=0x07E0;
    rb->commit_discard_dcache();
    setup_layer(4,ovl,320,240,0x00010100);
    push_frame();busywait_us(2000000);
    gram_log(test++,"L4 GREEN 0x07E0");

    /* T6: Layer 4 BLUE for color matrix */
    for(int i=0;i<320*240;i++) ovl[i]=0x001F;
    rb->commit_discard_dcache();
    CR(0x0C0)=PH(ovl);CR(0x024)=1;
    push_frame();busywait_us(2000000);
    gram_log(test++,"L4 BLUE 0x001F");

    /* T7: Layer 4 WHITE */
    for(int i=0;i<320*240;i++) ovl[i]=0xFFFF;
    rb->commit_discard_dcache();
    CR(0x0C0)=PH(ovl);CR(0x024)=1;
    push_frame();busywait_us(2000000);
    gram_log(test++,"L4 WHITE 0xFFFF");

    /* Disable L4 */
    {uint32_t v=CR(0x008);v&=~0x004;v|=0x080;CR(0x008)=v;}
    CR(0x024)=1;

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
