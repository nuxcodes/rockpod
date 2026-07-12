/* VPP Pre-Shear Test — works with ORIGINAL ILI9326 firmware (no DCS changes)
 * Uses pre-shear to correct compositor 240/scan → 320-wide GRAM mapping.
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

/* ILI9326 register commands (P18 mode) */
static void ili_cmd(uint16_t c){while(LCD_STATUS&0x10);LCD_WCMD=c;}
static void ili_data(uint16_t d){while(LCD_STATUS&0x10);LCD_WDATA=d;}

/* Pre-shear: rearrange YUV so compositor's 240/scan output fills 320-wide GRAM correctly.
 *
 * Compositor with rotation ON reads source[g] for output position g.
 * Output position g goes to source[p*W + s] where s=g/240, p=g%240 (rotation transpose).
 * GRAM position g displays at (g%320, g/320).
 * For correct display: source[(g%240)*W + (g/240)] must equal original[g].
 * So: presheared[(g%240)*W + (g/240)] = original[g] for all g in [0, W*H).
 * Proven bijective: if (g1%240)*W+(g1/240) = (g2%240)*W+(g2/240), then g1=g2. */
static void preshear_y(const uint8_t *src, uint8_t *dst, int w, int h)
{
    int total = w * h;
    rb->memset(dst, 0, total);
    for (int g = 0; g < total; g++) {
        int s = g / 240;   /* scan */
        int p = g % 240;   /* pixel in scan */
        int target = p * w + s;
        if (target < total)
            dst[target] = src[g];
    }
}

static void preshear_c(const uint8_t *src, uint8_t *dst, int cw, int ch)
{
    int total = cw * ch;
    rb->memset(dst, 128, total);
    for (int g = 0; g < total; g++) {
        int s = g / 120;   /* chroma scan = 240/2 */
        int p = g % 120;
        int target = p * cw + s;
        if (target < total)
            dst[target] = src[g];
    }
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
    {uint32_t v=c[0x008/4];v|=0x40000000;c[0x008/4]=v;}
    c[0x200/4]|=0x10080;c[0x204/4]=2;c[0x208/4]=0;c[0x20C/4]=2;
    c[0x210/4]=0x00010110;c[0x214/4]=0x00EF013F;c[0x024/4]=0x00FFFFFF;
}

enum plugin_status plugin_start(const void *parameter)
{
    const char*path=parameter?(const char*)parameter:"/test_iframe.264";
    if(!*path)return PLUGIN_ERROR;
    rb->cpu_boost(true);rb->audio_stop();

    log_fd=rb->open("/vpu_vpp_preshear.log",O_WRONLY|O_CREAT|O_TRUNC,0666);
    vlog("=== VPP Pre-Shear Test ===");
    vlog("Panel type: %d",(PDAT(6)&0x30)>>4);

    /* Decode H.264 */
    uint8_t*ab;size_t as;
    ab=rb->plugin_get_audio_buffer(&as);
    size_t ds=vpu_h264_buf_size(640,480);
    struct vpu_h264*dec=vpu_h264_open(ab,ds,640,480);
    if(!dec){vlog("ERROR: decoder");rb->close(log_fd);return PLUGIN_ERROR;}
    uint8_t*fb=ab+ds;
    int fd=rb->open(path,O_RDONLY);
    if(fd<0){vlog("ERROR: file");rb->close(log_fd);return PLUGIN_ERROR;}
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
    if(!yo){vlog("ERROR: no frame");vpu_h264_close(dec);rb->close(log_fd);return PLUGIN_ERROR;}
    vlog("Decoded %dx%d",fw,fh);

    /* Allocate pre-sheared buffers */
    size_t y_size = fw * fh;
    size_t c_size = (fw/2) * (fh/2);
    uint8_t *ps_y  = fb + fl + 1024;  /* some margin */
    uint8_t *ps_cb = ps_y + y_size;
    uint8_t *ps_cr = ps_cb + c_size;

    /* Build pre-sheared planes */
    uint32_t t0 = USEC_TIMER;
    preshear_y(yo, ps_y, fw, fh);
    preshear_c(cbo, ps_cb, fw/2, fh/2);
    preshear_c(cro, ps_cr, fw/2, fh/2);
    uint32_t t1 = USEC_TIMER;
    vlog("Pre-shear took %lu us", (unsigned long)(t1-t0));
    rb->commit_discard_dcache();

    /* Save LCD state */
    uint32_t sc=LCD_CON,s7=LR(0x7C),s8=LR(0x88),s2=LR(0x20),s4=LR(0x74),s5=LR(0x78);

    /* Enable compositor clock */
    PWRCON(0)&=~0x2080;
    for(volatile int d=0;d<10000;d++);

    comp_init();

    /* Layer 5 — point to PRE-SHEARED buffers */
    for(int o=0x024;o<=0x044;o+=4)CR(o)=0;
    for(int o=0x04C;o<=0x058;o+=4)CR(o)=0;
    CR(0x028)=0x100;CR(0x02C)=fw|((fw/2)<<16);
    CR(0x034)=fh|((uint32_t)fw<<16);
    CR(0x04C)=0x10001000;CR(0x054)=0x00F00140;
    CR(0x038)=PH(ps_y);
    CR(0x03C)=PH(ps_cr);
    CR(0x040)=0;
    CR(0x044)=PH(ps_cb);
    CR(0x3AC)=0x04004003;  /* rotation ON */
    CR(0x0D4)=1;
    {uint32_t v=CR(0x008);v&=~0x100;CR(0x008)=v;}

    /* LCD passthrough — ILI9326 P18 mode */
    LCD_CON=0x80100DB0;
    LR(0x88)=0x01000000;LR(0x20)=0x33;LR(0x7C)=0x00000402;
    LR(0x78)=0x000A000A;LR(0x74)=0x00F00140;

    /* Set ILI9326 GRAM window (P18 commands) */
    while(!(LCD_STATUS&0x2)); LCD_CON=0x80000DA9;
    ili_cmd(0x003);ili_data(0x1030);  /* Entry Mode: AM=0, I/D=11, landscape */
    ili_cmd(0x210);ili_data(0);       /* H start = 0 */
    ili_cmd(0x211);ili_data(319);     /* H end = 319 */
    ili_cmd(0x212);ili_data(0);       /* V start = 0 */
    ili_cmd(0x213);ili_data(239);     /* V end = 239 */
    ili_cmd(0x200);ili_data(0);       /* GRAM x = 0 */
    ili_cmd(0x201);ili_data(0);       /* GRAM y = 0 */
    ili_cmd(0x202);                   /* Write to GRAM */
    while(!(LCD_STATUS&0x2)); LCD_CON=0x80100DB0;

    /* Enable passthrough + compositor */
    LR(0x70)=1;LR(0x80)=0;
    CR(0x000)=0;{volatile int d=0;while(d++<50000);}
    CR(0x000)=1;{uint32_t t=USEC_TIMER;while((USEC_TIMER-t)<500000);}

    /* Push multiple frames to ensure GRAM fills */
    for(int i=0;i<10;i++){
        {int t=100000;while((LR(0x8C)&3)&&--t>0);}
        LR(0x80)=1; LCD_CON=0x80000DA9;
        ili_cmd(0x200);ili_data(0);ili_cmd(0x201);ili_data(0);ili_cmd(0x202);
        while(!(LCD_STATUS&0x2)); LCD_CON=0x80100DB0;
        LR(0x80)=0;{int t=500000;while((LR(0x8C)&3)&&--t>0);}
    }
    vlog("Compositor running with pre-sheared buffers");

    while(rb->button_get(true)==BUTTON_NONE) rb->backlight_on();

    /* Shutdown */
    LR(0x70)=0;LR(0x80)=0;CR(0x000)=0;
    while(!(LCD_STATUS&0x2)); LCD_CON=0x80000DA9;
    ili_cmd(0x003);ili_data(0x0230);  /* restore Rockbox Entry Mode */
    while(!(LCD_STATUS&0x2)); LCD_CON=sc;
    LR(0x88)=s8;LR(0x20)=s2;LR(0x7C)=s7;LR(0x74)=s4;LR(0x78)=s5;
    LCD_PHTIME=0x33;
    {int t=100000;while((LR(0x8C)&3)&&--t>0);}
    rb->lcd_update();

    vlog("Done");
    rb->close(log_fd);
    vpu_h264_close(dec);rb->cpu_boost(false);
    return PLUGIN_OK;
}
#else
enum plugin_status plugin_start(const void*p){(void)p;rb->splash(HZ*2,"iPod 6G only");return PLUGIN_ERROR;}
#endif
