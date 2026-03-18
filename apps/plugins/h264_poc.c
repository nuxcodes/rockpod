/***************************************************************************
 * S5L8702 H.264 Hardware Video Decoder - v36b
 *
 * v36b: Fix HW deblock artifacts — remove wait_and_clear() between sub-calls.
 *       Apple's FUN_0009144c never clears SUB+00/MAIN+00 between submissions.
 *       Add SW/HW diff metrics and SW bypass fallback.
 *
 * v36: I-frame decode from raw .264 file. Ports CAVLC + 4x4 IDCT + intra
 *      prediction from synthesizable_h264 reference. SW decode bit-perfect
 *      (0/76800 diffs vs ffmpeg no-deblock reference).
 *
 * v35d: Forward DCT round-trip validated (0/64 errors).
 * v34: Confirmed XFORM does 8x8 IEEE IDCT. No bypass mode.
 * v33c: Full LCD (320x240), stride=32, chroma, deblock flush.
 ****************************************************************************/

#include "plugin.h"
#include "s5l87xx.h"

#define LOG_PATH "/vdec_poc.log"
#define REG32(addr) (*(volatile uint32_t *)(addr))

/* Decoder sub-block base addresses */
#define VDEC_MAIN   0x39600000
#define VDEC_CORE   0x39610000
#define VDEC_DMA    0x39630000
#define VDEC_XFORM  0x39641000
#define VDEC_DEBLK  0x39650000
#define VDEC_SUB    0x39660000

/* XFORM+800 register group (IDCT command interface) */
#define XFORM_800   (*(volatile uint32_t *)0x39641800)
#define XFORM_808   (*(volatile uint32_t *)0x39641808)
#define DMA_10C     (*(volatile uint32_t *)0x3963010C)

/* IDCT command constant from Apple's DAT_00091530 */
#define XFORM_CMD_BASE  0x00020341

#define TEST_WIDTH_MBS    20
#define TEST_HEIGHT_MBS   15
#define TEST_WIDTH        320
#define TEST_HEIGHT       240

/* Buffer sizes */
#define DMA_WORK_SIZE     0x400       /* 1 KB */
#define WORK_BUF_SIZE     0x20000     /* 128 KB each */
#define COEFF_BUF_SIZE    0x200       /* 512 bytes per MB */
#define SMALL_BUF_SIZE    0x400       /* 1 KB — Apple's allocation (8 rows × 32 = 256 active) */
#define FRAME_Y_SIZE      (TEST_WIDTH * TEST_HEIGHT)           /* 76800 */
#define FRAME_CB_SIZE     (TEST_WIDTH / 2 * TEST_HEIGHT / 2)   /* 9600 */
#define FRAME_CR_SIZE     FRAME_CB_SIZE
#define HW_SRC_STRIDE     32  /* stride = 1 * 32 = 32 (MAIN+10 = 0x00010100 = 1×1 MBs) */

/* Alignment macros */
/* Set to 0 to bypass HW pipeline and display SW decode output directly */
#define USE_HW_PIPELINE 1

#define ALIGN32(x)   (((uintptr_t)(x) + 31) & ~31)
#define ALIGN4K(x)   (((uintptr_t)(x) + 0xFFF) & ~0xFFF)

static int log_fd = -1;

static void lflush(void)
{
    if (log_fd >= 0) {
        rb->close(log_fd);
        log_fd = rb->open(LOG_PATH, O_WRONLY|O_APPEND, 0666);
    }
}

static void poc_log(const char *fmt, ...)
{
    static char buf[200];
    va_list ap;
    va_start(ap, fmt);
    rb->vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (log_fd >= 0) rb->fdprintf(log_fd, "%s\n", buf);
}

/* ===================== POWER ON ===================== */
static void vdec_power_on(void)
{
    uint32_t cg, pw;

    cg = REG32(CLK_BASE + 0x08);
    cg &= ~0x80000000;
    cg |= 0x30000000;
    REG32(CLK_BASE + 0x08) = cg;
    rb->sleep(HZ/5);

    pw = PWRCON(0);
    PWRCON(0) = pw & ~((7 << 14) | (1 << 18));
    rb->sleep(HZ/5);

    REG32(0x38100000 + 0x314) &= ~1;

    REG32(VDEC_MAIN + 0x1C) = 0xFFFFFFFF;
    REG32(VDEC_CORE)        = 0xFFFFFFFF;
    REG32(VDEC_CORE)        = 0xFFFFFFFF;
    REG32(VDEC_DMA + 0x100) = 0xFFFFFFFF;
    REG32(VDEC_DEBLK)       = 0xFFFFFFFF;
    REG32(VDEC_SUB)         = 0xFFFFFFFF;
    REG32(VDEC_MAIN)        = 0xFFFFFFFF;

    REG32(VDEC_MAIN + 0x04) = 0x40;
    REG32(VDEC_MAIN + 0x0C) = 0;
    REG32(VDEC_MAIN + 0x10) = 0x10100;
    REG32(VDEC_SUB  + 0x04) = 2;
    REG32(VDEC_SUB  + 0x10) = 0x182;
    REG32(VDEC_DMA + 0x110) = 0x800;
    REG32(VDEC_XFORM+ 0x804)= 0x40;
    REG32(VDEC_DEBLK+ 0x10) = 0x10;
    REG32(VDEC_SUB  + 0x6C) = 0x10001;
}

static void vdec_power_off(void)
{
    uint32_t cg;

    REG32(VDEC_MAIN + 0x1C) = 0xFFFFFFFF;
    REG32(VDEC_CORE)        = 0xFFFFFFFF;
    REG32(VDEC_DMA + 0x100) = 0xFFFFFFFF;
    REG32(VDEC_DEBLK)       = 0xFFFFFFFF;
    REG32(VDEC_SUB)         = 0xFFFFFFFF;
    REG32(VDEC_MAIN)        = 0xFFFFFFFF;

    PWRCON(0) |= (7 << 14) | (1 << 18);
    REG32(0x38100000 + 0x314) |= 1;

    cg = REG32(CLK_BASE + 0x08);
    cg |= 0x80000000;
    cg &= ~0x30000000;
    REG32(CLK_BASE + 0x08) = cg;
}

/* ===================== Reset ===================== */
static void vdec_reset(void)
{
    REG32(VDEC_MAIN + 0x2C) = 2;
    REG32(VDEC_MAIN + 0x1C) = 0xFFFFFFFF;
    REG32(VDEC_MAIN + 0x0C) = 0;
    REG32(VDEC_CORE)         = 0xFFFFFFFF;
    REG32(VDEC_CORE)         = 0xFFFFFFFF;
    REG32(VDEC_DEBLK)        = 0xFFFFFFFF;
    REG32(VDEC_DMA + 0x100)  = 0xFFFFFFFF;
    REG32(VDEC_SUB)          = 0xFFFFFFFF;
    REG32(VDEC_SUB)          = 0xFFFFFFFF;
}

/* ===================== H.264 Init ===================== */
static void vdec_h264_init(uint32_t dma_addr, uint32_t work1_addr)
{
    int i;

    REG32(VDEC_MAIN + 0x1C) = 0xFFFFFFFF;
    REG32(VDEC_MAIN + 0x0C) = 0;
    REG32(VDEC_CORE)         = 0xFFFFFFFF;
    REG32(VDEC_CORE)         = 0xFFFFFFFF;
    REG32(VDEC_DMA + 0x100)  = 0xFFFFFFFF;
    REG32(VDEC_DEBLK)        = 0xFFFFFFFF;
    REG32(VDEC_SUB)          = 0xFFFFFFFF;
    REG32(VDEC_MAIN)         = 0xFFFFFFFF;

    {
        /* Apple's DAT_0007f478 = 0x00010100 (1×1 MBs).
         * H.264 hybrid path: HW outputs one MB at a time to small buffer.
         * MAIN+10 = 1×1 gives stride = 32 bytes. Frame dimensions handled by SW. */
        uint32_t main10 = 0x00010100;
        REG32(VDEC_MAIN + 0x04) = 0x40;
        REG32(VDEC_SUB  + 0x04) = 2;
        REG32(VDEC_SUB  + 0x10) = 0x182;
        REG32(VDEC_MAIN + 0x10) = main10;
        REG32(VDEC_DMA + 0x110) = 0x800;
        REG32(VDEC_XFORM+ 0x804)= 0x40;
        REG32(VDEC_DEBLK+ 0x10) = 0x10;
        REG32(VDEC_SUB  + 0x6C) = main10 - 0xFF;  /* = 0x00010001 */
    }

    REG32(VDEC_SUB + 0x20) = dma_addr;
    REG32(VDEC_SUB + 0x24) = DMA_WORK_SIZE;
    REG32(VDEC_SUB + 0x78) = work1_addr;
    REG32(VDEC_SUB + 0x7C) = WORK_BUF_SIZE;
    REG32(VDEC_SUB + 0x80) = 0;

    for (i = 0; i < 64; i++) {
        REG32(VDEC_XFORM + 0x200 + i * 4) = 16;
        REG32(VDEC_XFORM + 0x300 + i * 4) = 16;
    }
}

/* ===================== PER-MB HARDWARE IDCT ===================== */
static int hw_mb_submit(uint32_t coeff_buf_phys,
                        uint32_t ref_addr, uint32_t out_addr,
                        int frame_toggle, int is_chroma)
{
    int timeout;

    REG32(VDEC_SUB + 0x18) = coeff_buf_phys;
    REG32(VDEC_SUB + 0x1C) = coeff_buf_phys + COEFF_BUF_SIZE;
    REG32(VDEC_SUB + 0x0C) = 3;

    REG32(VDEC_SUB + 0x2C) = ref_addr;
    REG32(VDEC_SUB + 0x3C) = out_addr;

    timeout = 100000;
    while ((REG32(VDEC_DEBLK + 0x14) & 0x10000) && --timeout > 0) {}
    if (timeout == 0) return -1;
    REG32(VDEC_DEBLK + 0x0C) = ((uint32_t)frame_toggle << 30) | 0x80;

    {
        int p;
        for (p = 0; p < 2; p++) {
            timeout = 100000;
            while ((XFORM_808 & 2) && --timeout > 0) {}
            if (timeout == 0) return -1;
            XFORM_800 = XFORM_CMD_BASE | ((uint32_t)is_chroma << 19);
            DMA_10C = ((uint32_t)is_chroma << 3) | 0x31;
        }
    }
    return 0;
}

static void fill_coeff_pair(uint32_t *buf, uint32_t dc0, uint32_t dc1)
{
    int i;
    for (i = 0; i < 128; i++)
        buf[i] = 0;
    buf[0]  = __builtin_bswap32(dc0);
    buf[64] = __builtin_bswap32(dc1);
}

static int wait_and_clear(void)
{
    int wait = 50000;
    while (!(REG32(VDEC_SUB) & 0x7F) && --wait > 0) {}
    REG32(VDEC_SUB)  = 0xFFFFFFFF;
    REG32(VDEC_MAIN) = 0xFFFFFFFF;
    return (wait > 0) ? 0 : -1;
}

/* ===================== FORWARD 8×8 DCT ===================== */

/* Cosine lookup: round(cos(π*(2x+1)*u/16) * 1024), u=0..7, x=0..7 */
static const int dct_c[8][8] = {
    { 1024, 1024, 1024, 1024, 1024, 1024, 1024, 1024 },
    { 1004,  851,  569,  200, -200, -569, -851,-1004 },
    {  946,  392, -392, -946, -946, -392,  392,  946 },
    {  851, -200,-1004, -569,  569, 1004,  200, -851 },
    {  724, -724, -724,  724,  724, -724, -724,  724 },
    {  569,-1004,  200,  851, -851, -200, 1004, -569 },
    {  392, -946,  946, -392, -392,  946, -946,  392 },
    {  200, -569,  851,-1004, 1004, -851,  569, -200 },
};

/* α(u) * 1024: α(0)=1/√8*1024≈362, α(k>0)=1/2*1024=512 */
static const int dct_a[8] = { 362, 512, 512, 512, 512, 512, 512, 512 };

/* 8×8 zigzag scan: zigzag position → raster position (row*8+col) */
static const unsigned char zigzag8x8[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

/* Forward 8×8 DCT (orthonormal DCT-II, integer with 64-bit intermediates).
 * in[row][col]: pixel-128 values.
 * out[row][col]: DCT coefficients in raster order.
 * Uses FP10 cosine table (×1024) and FP10 α factors. */
static void forward_dct8x8(const int in[8][8], int out[8][8])
{
    int tmp[8][8];
    int i, u, v;

    /* 1D DCT on each row → tmp[row][freq] (FP10) */
    for (i = 0; i < 8; i++) {
        for (u = 0; u < 8; u++) {
            int x, sum = 0;
            for (x = 0; x < 8; x++)
                sum += in[i][x] * dct_c[u][x];
            tmp[i][u] = sum; /* FP10, max ≈ 8*127*1024 = 1,040,384 */
        }
    }

    /* 1D DCT on each column of tmp, apply α normalization.
     * out[v][u] = F(v,u): v=vertical freq, u=horizontal freq (JPEG convention).
     * Column sum is FP20, × α²(FP20) = FP40, >> 40 = actual. */
    for (u = 0; u < 8; u++) {
        for (v = 0; v < 8; v++) {
            long long sum = 0;
            for (i = 0; i < 8; i++)
                sum += (long long)tmp[i][u] * dct_c[v][i];
            long long F = sum * dct_a[u] * dct_a[v];
            out[v][u] = (int)((F + (1LL << 39)) >> 40);
        }
    }
}

/* Forward DCT on 8×8 pixel block, pack into one half of DMA coefficient buffer.
 * pixels[row][col]: uint8_t source pixels.
 * buf: destination (64 uint32), packed in 8×8 zigzag order.
 * scale: scaling matrix value (16 for flat). */
static void fill_coeff_from_pixels(const uint8_t pixels[8][8],
                                    uint32_t *buf, int scale)
{
    int block[8][8], dct_out[8][8];
    int i, j;

    for (i = 0; i < 8; i++)
        for (j = 0; j < 8; j++)
            block[i][j] = (int)pixels[i][j] - 128;

    forward_dct8x8(block, dct_out);

    for (i = 0; i < 64; i++) {
        int pos = zigzag8x8[i];
        int coeff = dct_out[pos >> 3][pos & 7] / scale;
        buf[i] = __builtin_bswap32((uint32_t)(int32_t)coeff);
    }
}

/* ===================== READBACK ===================== */

/* Luma: 8 rows × 16 bytes (2 blocks side-by-side), stride=32 */
static void readback_luma(const uint8_t *src, uint8_t *frame,
                          int mb_col, int mb_row, int row_offset)
{
    int row;
    for (row = 0; row < 8; row++) {
        const uint32_t *s = (const uint32_t *)(src + row * HW_SRC_STRIDE);
        uint32_t *d = (uint32_t *)(frame
                      + (mb_row * 16 + row_offset + row) * TEST_WIDTH
                      + mb_col * 16);
        d[0] = __builtin_bswap32(s[0]);
        d[1] = __builtin_bswap32(s[1]);
        d[2] = __builtin_bswap32(s[2]);
        d[3] = __builtin_bswap32(s[3]);
    }
}

/* Chroma: block 0 = Cb (src[0:7]), block 1 = Cr (src[8:15]), stride=32.
 * 4:2:0 = half resolution: 8×8 per 16×16 MB. */
static void readback_chroma(const uint8_t *src,
                            uint8_t *cb_frame, uint8_t *cr_frame,
                            int mb_col, int mb_row)
{
    int row;
    int cb_stride = TEST_WIDTH / 2;
    for (row = 0; row < 8; row++) {
        const uint32_t *s = (const uint32_t *)(src + row * HW_SRC_STRIDE);
        uint32_t *dcb = (uint32_t *)(cb_frame
                        + (mb_row * 8 + row) * cb_stride + mb_col * 8);
        dcb[0] = __builtin_bswap32(s[0]);
        dcb[1] = __builtin_bswap32(s[1]);
        uint32_t *dcr = (uint32_t *)(cr_frame
                        + (mb_row * 8 + row) * cb_stride + mb_col * 8);
        dcr[0] = __builtin_bswap32(s[2]);
        dcr[1] = __builtin_bswap32(s[3]);
    }
}

/* Dump small buffer at stride 32 (8 rows × 32 bytes) */
static void dump_small_buf(const uint8_t *buf, const char *label)
{
    int row;
    const uint32_t *w;
    poc_log("--- small_buf %s (stride-%d, 8 rows) ---", label, HW_SRC_STRIDE);
    for (row = 0; row < 8; row++) {
        int off = row * HW_SRC_STRIDE;
        w = (const uint32_t *)(buf + off);
        poc_log("  %03x: %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx",
                off,
                (unsigned long)w[0], (unsigned long)w[1],
                (unsigned long)w[2], (unsigned long)w[3],
                (unsigned long)w[4], (unsigned long)w[5],
                (unsigned long)w[6], (unsigned long)w[7]);
    }
}

/* ===================== H.264 SW DECODE (v36) ===================== */

#define H264_TEST_PATH "/test_iframe.264"
#define MAX_NALU_BUF   200000

/* Macro helpers from H.264 spec */
#define KTOX(a) ((((a)&0x4)!=0)*2+(((a)&0x1)!=0))
#define KTOY(a) ((((a)&0x8)!=0)*2+(((a)&0x2)!=0))
#define ABSS(a) ((a)>0?(a):-(a))
#ifndef MAX
#define MAX(a,b) ((a)>(b)?(a):(b))
#endif
#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#endif

/* Frame dimensions (set from SPS) */
static int pic_w, pic_h, pic_cw, pic_ch, pic_wmb, pic_hmb;

/* Row-major HW output frame buffers (set in plugin_start) */
static uint8_t *frame_y, *frame_cb, *frame_cr;

/* Col-major SW frame buffers */
static uint8_t *sw_y, *sw_cb, *sw_cr;
#define SY(x,y)   sw_y[(x)*pic_h+(y)]
#define SCB(x,y)  sw_cb[(x)*pic_ch+(y)]
#define SCR(x,y)  sw_cr[(x)*pic_ch+(y)]

/* Per-frame state (col-major) */
static int8_t  *ipm_buf;
static uint8_t *nzl_buf, *nzc0_buf, *nzc1_buf, *mbim_buf;
#define IPM(x,y)   ipm_buf[(x)*(pic_h/4)+(y)]
#define NZL(x,y)   nzl_buf[(x)*(pic_h/4)+(y)]
#define NZC0(x,y)  nzc0_buf[(x)*(pic_ch/4)+(y)]
#define NZC1(x,y)  nzc1_buf[(x)*(pic_ch/4)+(y)]
#define IMODE(x,y) mbim_buf[(x)*pic_hmb+(y)]

/* ---- NALU ---- */
typedef struct {
    uint8_t *buf;
    unsigned long bit_offset;
    unsigned long bit_length;
    unsigned int len;
    int nal_unit_type;
    int nal_reference_idc;
} nalu_t;

/* ---- SPS ---- */
typedef struct {
    uint8_t profile_idc, level_idc;
    unsigned int seq_parameter_set_id;
    unsigned int log2_max_frame_num_minus4;
    unsigned int pic_order_cnt_type;
    unsigned int log2_max_pic_order_cnt_lsb_minus4;
    unsigned int max_num_ref_frames;
    unsigned int pic_width_in_mbs_minus1;
    unsigned int pic_height_in_map_units_minus1;
    uint8_t frame_mbs_only_flag;
    uint8_t direct_8x8_inference_flag;
    uint8_t frame_cropping_flag;
    uint8_t delta_pic_order_always_zero_flag;
    int offset_for_non_ref_pic;
    int offset_for_top_to_bottom_field;
    unsigned int num_ref_frames_in_pic_order_cnt_cycle;
    int offset_for_ref_frame[256];
} sps_t;

/* ---- PPS ---- */
typedef struct {
    unsigned int pic_parameter_set_id;
    unsigned int seq_parameter_set_id;
    uint8_t entropy_coding_mode_flag;
    uint8_t bottom_field_pic_order_in_frame_present_flag;
    unsigned int num_ref_idx_l0_active_minus1;
    unsigned int num_ref_idx_l1_active_minus1;
    int pic_init_qp_minus26;
    int chroma_qp_index_offset;
    uint8_t deblocking_filter_control_present_flag;
    uint8_t constrained_intra_pred_flag;
    uint8_t weighted_pred_flag;
    uint8_t weighted_bipred_idc;
} pps_t;

/* ---- Image parameters ---- */
typedef struct {
    int sliceQPY;
    int chroma_offset;
    int mem_idx;
} img_t;

/* ---- Math utilities ---- */
static int Clip1y(int x) { return x < 0 ? 0 : (x > 255 ? 255 : x); }
static int Clip3(int lo, int hi, int x) { return x < lo ? lo : (x > hi ? hi : x); }

/* ---- Bitstream reader (ARM-safe, no unaligned access) ---- */
static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}

static uint8_t bs_u1(nalu_t *n)
{
    int bytepos = n->bit_offset / 8;
    int bitpos = 7 - (n->bit_offset % 8);
    n->bit_offset++;
    return (n->buf[bytepos] >> bitpos) & 1;
}

static uint8_t bs_u8(nalu_t *n)
{
    int bytepos = n->bit_offset / 8;
    n->bit_offset += 8;
    return n->buf[bytepos];
}

static uint32_t bs_un(int nbits, nalu_t *n)
{
    int first_byte = n->bit_offset / 8;
    int last_byte = (n->bit_offset + nbits) / 8;
    int num_bytes = last_byte - first_byte + 1;
    int last_bit = 7 - (n->bit_offset + nbits) % 8;
    uint32_t temp0 = read_be32(&n->buf[first_byte]);
    uint32_t temp1 = temp0 >> (8 * (4 - num_bytes));
    uint32_t temp2 = temp1 >> (last_bit + 1);
    int ret = temp2 & ((1 << nbits) - 1);
    n->bit_offset += nbits;
    return ret;
}

static uint32_t bs_ue(nalu_t *n)
{
    int lz = 0;
    while (!bs_u1(n)) lz++;
    return (1u << lz) - 1 + bs_un(lz, n);
}

static int bs_se(nalu_t *n)
{
    int r = bs_ue(n);
    return (r % 2) ? (r + 1) / 2 : -(r + 1) / 2;
}

static int bs_showbits(int nbits, uint32_t temp0, int offset)
{
    if (nbits == 0) return -1;
    int first_byte = offset / 8;
    int last_byte = (offset + nbits) / 8;
    int num_bytes = last_byte - first_byte + 1;
    int last_bit = 7 - (offset + nbits) % 8;
    uint32_t temp1 = temp0 >> (8 * (4 - num_bytes));
    uint32_t temp2 = temp1 >> (last_bit + 1);
    return temp2 & ((1 << nbits) - 1);
}

static const uint8_t NCBP[48][2] = {
    {47, 0},{31,16},{15, 1},{ 0, 2},{23, 4},{27, 8},{29,32},{30, 3},
    { 7, 5},{11,10},{13,12},{14,15},{39,47},{43, 7},{45,11},{46,13},
    {16,14},{ 3, 6},{ 5, 9},{10,31},{12,35},{19,37},{21,42},{26,44},
    {28,33},{35,34},{37,36},{42,40},{44,39},{ 1,43},{ 2,45},{ 4,46},
    { 8,17},{17,18},{18,20},{20,24},{24,19},{ 6,21},{ 9,26},{22,28},
    {25,23},{32,27},{33,29},{34,30},{36,22},{40,25},{38,38},{41,41},
};

static uint8_t bs_me(int pred_type, nalu_t *n)
{
    int idx = bs_ue(n);
    return NCBP[idx][pred_type];
}

/* ---- SPS/PPS parsing ---- */
static void parse_sps(sps_t *sps, nalu_t *n)
{
    unsigned int i;
    n->bit_offset = 8;
    sps->profile_idc = bs_u8(n);
    bs_u8(n); /* constraint flags + reserved */
    sps->level_idc = bs_u8(n);
    sps->seq_parameter_set_id = bs_ue(n);
    sps->log2_max_frame_num_minus4 = bs_ue(n);
    sps->pic_order_cnt_type = bs_ue(n);
    if (sps->pic_order_cnt_type == 0) {
        sps->log2_max_pic_order_cnt_lsb_minus4 = bs_ue(n);
    } else if (sps->pic_order_cnt_type == 1) {
        sps->delta_pic_order_always_zero_flag = bs_u1(n);
        sps->offset_for_non_ref_pic = bs_se(n);
        sps->offset_for_top_to_bottom_field = bs_se(n);
        sps->num_ref_frames_in_pic_order_cnt_cycle = bs_ue(n);
        for (i = 0; i < sps->num_ref_frames_in_pic_order_cnt_cycle; i++)
            sps->offset_for_ref_frame[i] = bs_se(n);
    }
    sps->max_num_ref_frames = bs_ue(n);
    bs_u1(n); /* gaps_in_frame_num_value_allowed_flag */
    sps->pic_width_in_mbs_minus1 = bs_ue(n);
    sps->pic_height_in_map_units_minus1 = bs_ue(n);
    sps->frame_mbs_only_flag = bs_u1(n);
    if (!sps->frame_mbs_only_flag) bs_u1(n); /* mb_adaptive */
    sps->direct_8x8_inference_flag = bs_u1(n);
    sps->frame_cropping_flag = bs_u1(n);
    if (sps->frame_cropping_flag) {
        bs_ue(n); bs_ue(n); bs_ue(n); bs_ue(n); /* crop offsets */
    }
    bs_u1(n); /* vui_parameters_present_flag */
}

static void parse_pps(pps_t *pps, nalu_t *n)
{
    n->bit_offset = 8;
    pps->pic_parameter_set_id = bs_ue(n);
    pps->seq_parameter_set_id = bs_ue(n);
    pps->entropy_coding_mode_flag = bs_u1(n);
    pps->bottom_field_pic_order_in_frame_present_flag = bs_u1(n);
    bs_ue(n); /* num_slice_groups_minus1 */
    pps->num_ref_idx_l0_active_minus1 = bs_ue(n);
    pps->num_ref_idx_l1_active_minus1 = bs_ue(n);
    pps->weighted_pred_flag = bs_u1(n);
    pps->weighted_bipred_idc = bs_un(2, n);
    pps->pic_init_qp_minus26 = bs_se(n);
    bs_se(n); /* pic_init_qs_minus26 */
    pps->chroma_qp_index_offset = bs_se(n);
    pps->deblocking_filter_control_present_flag = bs_u1(n);
    pps->constrained_intra_pred_flag = bs_u1(n);
    bs_u1(n); /* redundant_pic_cnt_present_flag */
}

static void parse_sh_idr(sps_t *sps, pps_t *pps, nalu_t *n,
                         int *slice_qp, int *chroma_off)
{
    n->bit_offset = 8;
    bs_ue(n); /* first_mb_in_slice */
    unsigned int slice_type = bs_ue(n) % 5;
    (void)slice_type;
    bs_ue(n); /* pic_parameter_set_id */
    bs_un(sps->log2_max_frame_num_minus4 + 4, n); /* frame_num */
    /* IDR */
    bs_ue(n); /* idr_pic_id */
    if (sps->pic_order_cnt_type == 0)
        bs_un(sps->log2_max_pic_order_cnt_lsb_minus4 + 4, n);
    /* dec_ref_pic_marking for IDR */
    if (n->nal_reference_idc != 0) {
        bs_u1(n); /* no_output_of_prior_pics_flag */
        bs_u1(n); /* long_term_reference_flag */
    }
    int qp_delta = bs_se(n);
    *slice_qp = 26 + pps->pic_init_qp_minus26 + qp_delta;
    *chroma_off = pps->chroma_qp_index_offset;
    if (pps->deblocking_filter_control_present_flag) {
        unsigned int dd = bs_ue(n); /* disable_deblocking_filter_idc */
        if (dd != 1) { bs_se(n); bs_se(n); }
    }
}

/* ---- CAVLC tables ---- */
static const uint8_t cavlc_lentab[3][4][17] = {
    {{ 1, 6, 8, 9,10,11,13,13,13,14,14,15,15,16,16,16,16},
     { 0, 2, 6, 8, 9,10,11,13,13,14,14,15,15,15,16,16,16},
     { 0, 0, 3, 7, 8, 9,10,11,13,13,14,14,15,15,16,16,16},
     { 0, 0, 0, 5, 6, 7, 8, 9,10,11,13,14,14,15,15,16,16}},
    {{ 2, 6, 6, 7, 8, 8, 9,11,11,12,12,12,13,13,13,14,14},
     { 0, 2, 5, 6, 6, 7, 8, 9,11,11,12,12,13,13,14,14,14},
     { 0, 0, 3, 6, 6, 7, 8, 9,11,11,12,12,13,13,13,14,14},
     { 0, 0, 0, 4, 4, 5, 6, 6, 7, 9,11,11,12,13,13,13,14}},
    {{ 4, 6, 6, 6, 7, 7, 7, 7, 8, 8, 9, 9, 9,10,10,10,10},
     { 0, 4, 5, 5, 5, 5, 6, 6, 7, 8, 8, 9, 9, 9,10,10,10},
     { 0, 0, 4, 5, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9,10,10,10},
     { 0, 0, 0, 4, 4, 4, 4, 4, 5, 6, 7, 8, 8, 9,10,10,10}},
};
static const uint8_t cavlc_codtab[3][4][17] = {
    {{ 1, 5, 7, 7, 7, 7,15,11, 8,15,11,15,11,15,11, 7, 4},
     { 0, 1, 4, 6, 6, 6, 6,14,10,14,10,14,10, 1,14,10, 6},
     { 0, 0, 1, 5, 5, 5, 5, 5,13, 9,13, 9,13, 9,13, 9, 5},
     { 0, 0, 0, 3, 3, 4, 4, 4, 4, 4,12,12, 8,12, 8,12, 8}},
    {{ 3,11, 7, 7, 7, 4, 7,15,11,15,11, 8,15,11, 7, 9, 7},
     { 0, 2, 7,10, 6, 6, 6, 6,14,10,14,10,14,10,11, 8, 6},
     { 0, 0, 3, 9, 5, 5, 5, 5,13, 9,13, 9,13, 9, 6,10, 5},
     { 0, 0, 0, 5, 4, 6, 8, 4, 4, 4,12, 8,12,12, 8, 1, 4}},
    {{15,15,11, 8,15,11, 9, 8,15,11,15,11, 8,13, 9, 5, 1},
     { 0,14,15,12,10, 8,14,10,14,14,10,14,10, 7,12, 8, 4},
     { 0, 0,13,14,11, 9,13, 9,13,10,13, 9,13, 9,11, 7, 3},
     { 0, 0, 0,12,11,10, 9, 8,13,12,12,12, 8,12,10, 6, 2}},
};
static const uint8_t cavlc_lentabDC[4][5] = {
    { 2, 6, 6, 6, 6},{ 0, 1, 6, 7, 8},{ 0, 0, 3, 7, 8},{ 0, 0, 0, 6, 7},
};
static const uint8_t cavlc_codtabDC[4][5] = {
    {1,7,4,3,2},{0,1,6,3,3},{0,0,1,2,2},{0,0,0,5,0},
};
static const uint8_t tzlentab[15][16] = {
    { 1,3,3,4,4,5,5,6,6,7,7,8,8,9,9,9},
    { 3,3,3,3,3,4,4,4,4,5,5,6,6,6,6,0},
    { 4,3,3,3,4,4,3,3,4,5,5,6,5,6,0,0},
    { 5,3,4,4,3,3,3,4,3,4,5,5,5,0,0,0},
    { 4,4,4,3,3,3,3,3,4,5,4,5,0,0,0,0},
    { 6,5,3,3,3,3,3,3,4,3,6,0,0,0,0,0},
    { 6,5,3,3,3,2,3,4,3,6,0,0,0,0,0,0},
    { 6,4,5,3,2,2,3,3,6,0,0,0,0,0,0,0},
    { 6,6,4,2,2,3,2,5,0,0,0,0,0,0,0,0},
    { 5,5,3,2,2,2,4,0,0,0,0,0,0,0,0,0},
    { 4,4,3,3,1,3,0,0,0,0,0,0,0,0,0,0},
    { 4,4,2,1,3,0,0,0,0,0,0,0,0,0,0,0},
    { 3,3,1,2,0,0,0,0,0,0,0,0,0,0,0,0},
    { 2,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0},
    { 1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
};
static const uint8_t tzcodtab[15][16] = {
    {1,3,2,3,2,3,2,3,2,3,2,3,2,3,2,1},
    {7,6,5,4,3,5,4,3,2,3,2,3,2,1,0,0},
    {5,7,6,5,4,3,4,3,2,3,2,1,1,0,0,0},
    {3,7,5,4,6,5,4,3,3,2,2,1,0,0,0,0},
    {5,4,3,7,6,5,4,3,2,1,1,0,0,0,0,0},
    {1,1,7,6,5,4,3,2,1,1,0,0,0,0,0,0},
    {1,1,5,4,3,3,2,1,1,0,0,0,0,0,0,0},
    {1,1,1,3,3,2,2,1,0,0,0,0,0,0,0,0},
    {1,0,1,3,2,1,1,1,0,0,0,0,0,0,0,0},
    {1,0,1,3,2,1,1,0,0,0,0,0,0,0,0,0},
    {0,1,1,2,1,3,0,0,0,0,0,0,0,0,0,0},
    {0,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0},
    {0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
};
static const uint8_t tzlentabDC[3][4] = {{1,2,3,3},{1,2,2,0},{1,1,0,0}};
static const uint8_t tzcodtabDC[3][4] = {{1,1,1,0},{1,1,0,0},{1,0,0,0}};
static const uint8_t rblentab[7][16] = {
    {1,1,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,2,2,0,0,0,0,0,0,0,0,0,0,0,0},
    {2,2,2,2,0,0,0,0,0,0,0,0,0,0,0},
    {2,2,2,3,3,0,0,0,0,0,0,0,0,0,0},
    {2,2,3,3,3,3,0,0,0,0,0,0,0,0,0},
    {2,3,3,3,3,3,3,0,0,0,0,0,0,0,0},
    {3,3,3,3,3,3,3,4,5,6,7,8,9,10,11},
};
static const uint8_t rbcodtab[7][16] = {
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {3,2,1,0,0,0,0,0,0,0,0,0,0,0,0},
    {3,2,1,1,0,0,0,0,0,0,0,0,0,0,0},
    {3,2,3,2,1,0,0,0,0,0,0,0,0,0,0},
    {3,0,1,3,2,5,4,0,0,0,0,0,0,0,0},
    {7,6,5,4,3,2,1,1,1,1,1,1,1,1,1},
};
static const uint8_t framescan[16][2] = {
    {0,0},{1,0},{0,1},{0,2},{1,1},{2,0},{3,0},{2,1},
    {1,2},{0,3},{1,3},{2,2},{3,1},{3,2},{2,3},{3,3}
};

/* ---- CAVLC decoder ---- */
static uint8_t cavlc_unary(nalu_t *n)
{
    uint8_t i = 0;
    while (bs_u1(n) == 0) i++;
    return i;
}

static void cavlc_trailing_total(nalu_t *n, uint8_t *tc, uint8_t *to,
                                  uint8_t nC_range)
{
    int i, j;
    if (nC_range == 3) {
        int cod = bs_un(6, n);
        *to = cod & 3;
        *tc = (cod >> 2) + 1;
        if (*to > *tc) { *to = 0; *tc = 0; }
        return;
    }
    int a = 0, b = 0, c = 0;
    int offset = n->bit_offset;
    uint32_t temp0 = read_be32(&n->buf[offset / 8]);
    for (j = 0; j < 4; j++)
        for (i = 0; i < 17; i++) {
            int len = cavlc_lentab[nC_range][j][i];
            int cod = cavlc_codtab[nC_range][j][i];
            uint8_t test = (bs_showbits(len, temp0, offset) == cod);
            a += j * test; b += i * test; c += len * test;
        }
    *to = a; *tc = b;
    n->bit_offset += c;
}

static void cavlc_trailing_total_dc(nalu_t *n, uint8_t *tc, uint8_t *to)
{
    int i, j;
    int a = 0, b = 0, c = 0;
    int offset = n->bit_offset;
    uint32_t temp0 = read_be32(&n->buf[offset / 8]);
    for (i = 0; i < 4; i++)
        for (j = 0; j < 5; j++) {
            int len = cavlc_lentabDC[i][j];
            int cod = cavlc_codtabDC[i][j];
            uint8_t test = (bs_showbits(len, temp0, offset) == cod);
            a += j * test; b += i * test; c += len * test;
        }
    *to = b; *tc = a;
    n->bit_offset += c;
}

static uint8_t cavlc_total_zeros(nalu_t *n, uint8_t tzVLC)
{
    int i, a = 0, b = 0;
    int offset = n->bit_offset;
    uint32_t temp0 = read_be32(&n->buf[offset / 8]);
    for (i = 0; i < 15; i++) {
        int len = tzlentab[tzVLC - 1][i];
        int cod = tzcodtab[tzVLC - 1][i];
        uint8_t test = (bs_showbits(len, temp0, offset) == cod);
        a += len * test; b += i * test;
    }
    n->bit_offset += a;
    return b;
}

static uint8_t cavlc_total_zeros_dc(nalu_t *n, uint8_t tzVLC)
{
    int i, a = 0, b = 0;
    int offset = n->bit_offset;
    uint32_t temp0 = read_be32(&n->buf[offset / 8]);
    for (i = 0; i < 4; i++) {
        int len = tzlentabDC[tzVLC - 1][i];
        int cod = tzcodtabDC[tzVLC - 1][i];
        uint8_t test = (bs_showbits(len, temp0, offset) == cod);
        a += len * test; b += i * test;
    }
    n->bit_offset += a;
    return b;
}

static uint8_t cavlc_run_before(nalu_t *n, uint8_t tzVLC)
{
    int i, a = 0, b = 0;
    uint8_t tmp = (tzVLC > 7) ? 6 : tzVLC - 1;
    int offset = n->bit_offset;
    uint32_t temp0 = read_be32(&n->buf[offset / 8]);
    for (i = 0; i < 15; i++) {
        int len = rblentab[tmp][i];
        int cod = rbcodtab[tmp][i];
        uint8_t test = (bs_showbits(len, temp0, offset) == cod);
        a += len * test; b += i * test;
    }
    n->bit_offset += a;
    return b;
}

static uint8_t cavlc_decode_16(int cl[4][4], nalu_t *n,
                                int startIdx, int endIdx, int nC)
{
    int i;
    uint8_t to, tc, suffLen;
    uint8_t nC_range = nC / 2;
    if (nC_range > 3) nC_range = 3;
    else if (nC_range == 3) nC_range = 2;
    for (i = 0; i < 16; i++) cl[i/4][i%4] = 0;
    cavlc_trailing_total(n, &tc, &to, nC_range);
    if (tc == 0) return 0;
    int levelVal[16];
    uint8_t runVal[16];
    suffLen = (tc > 10 && to < 3) ? 1 : 0;
    for (i = 0; i < tc; i++) {
        if (i < to) {
            levelVal[i] = 1 - 2 * bs_u1(n);
        } else {
            uint8_t lp = cavlc_unary(n);
            int lc = lp << suffLen;
            if (suffLen > 0 || lp >= 14) {
                uint8_t ssl;
                if (lp == 14 && suffLen == 0) ssl = 4;
                else if (lp >= 15) ssl = lp - 3;
                else ssl = suffLen;
                lc += bs_un(ssl, n);
            }
            if (lp >= 15 && suffLen == 0) lc += 15;
            if (lp >= 16) lc += (1 << (lp - 3)) - 4096;
            if (i == to && to < 3) lc += 2;
            levelVal[i] = (lc % 2 == 0) ? (lc + 2) >> 1 : (-lc - 1) >> 1;
            if (suffLen == 0) suffLen = 1;
            if (ABSS(levelVal[i]) > (3 << (suffLen - 1)) && suffLen < 6)
                suffLen++;
        }
    }
    uint8_t zeroLeft = 0;
    if (tc < endIdx - startIdx + 1)
        zeroLeft = cavlc_total_zeros(n, tc);
    for (i = 0; i < tc - 1; i++) {
        runVal[i] = (zeroLeft > 0) ? cavlc_run_before(n, zeroLeft) : 0;
        zeroLeft -= runVal[i];
    }
    runVal[tc - 1] = zeroLeft;
    int cn = -1;
    for (i = tc - 1; i >= 0; i--) {
        cn += runVal[i] + 1;
        cl[framescan[startIdx + cn][0]][framescan[startIdx + cn][1]] = levelVal[i];
    }
    return tc;
}

static uint8_t cavlc_decode_4(int cl[2][2], nalu_t *n,
                               int startIdx, int endIdx)
{
    int i;
    uint8_t to, tc, suffLen;
    for (i = 0; i < 4; i++) cl[i/2][i%2] = 0;
    cavlc_trailing_total_dc(n, &tc, &to);
    if (tc == 0) return 0;
    int levelVal[4];
    uint8_t runVal[4];
    suffLen = (tc > 10 && to < 3) ? 1 : 0;
    for (i = 0; i < tc; i++) {
        if (i < to) {
            levelVal[i] = 1 - 2 * bs_u1(n);
        } else {
            uint8_t lp = cavlc_unary(n);
            int lc = lp << suffLen;
            if (suffLen > 0 || lp >= 14) {
                uint8_t ssl;
                if (lp == 14 && suffLen == 0) ssl = 4;
                else if (lp >= 15) ssl = lp - 3;
                else ssl = suffLen;
                lc += bs_un(ssl, n);
            }
            if (lp >= 15 && suffLen == 0) lc += 15;
            if (lp >= 16) lc += (1 << (lp - 3)) - 4096;
            if (i == to && to < 3) lc += 2;
            levelVal[i] = (lc % 2 == 0) ? (lc + 2) >> 1 : (-lc - 1) >> 1;
            if (suffLen == 0) suffLen = 1;
            if (ABSS(levelVal[i]) > (3 << (suffLen - 1)) && suffLen < 6)
                suffLen++;
        }
    }
    uint8_t zeroLeft = 0;
    if (tc < endIdx - startIdx + 1)
        zeroLeft = cavlc_total_zeros_dc(n, tc);
    for (i = 0; i < tc - 1; i++) {
        runVal[i] = (zeroLeft > 0) ? cavlc_run_before(n, zeroLeft) : 0;
        zeroLeft -= runVal[i];
    }
    runVal[tc - 1] = zeroLeft;
    int cn = -1;
    for (i = tc - 1; i >= 0; i--) {
        cn += runVal[i] + 1;
        cl[(startIdx + cn) % 2][(startIdx + cn) / 2] = levelVal[i];
    }
    return tc;
}

static uint8_t nc_luma(int xoff, int yoff)
{
    uint8_t nA = 0, nB = 0;
    if (xoff > 0) {
        uint8_t im = IMODE((xoff-1)/4, yoff/4);
        nA = (im == 25) * 16 + (im != 3) * NZL(xoff-1, yoff);
    }
    if (yoff > 0) {
        uint8_t im = IMODE(xoff/4, (yoff-1)/4);
        nB = (im == 25) * 16 + (im != 3) * NZL(xoff, yoff-1);
    }
    return (nA + nB + (yoff > 0) * (xoff > 0)) >> ((yoff > 0) * (xoff > 0));
}

static uint8_t nc_chroma(int ch, int xoff, int yoff)
{
    uint8_t nA = 0, nB = 0;
    if (xoff > 0) {
        uint8_t im = IMODE((xoff-1)/2, yoff/2);
        uint8_t nz = ch ? NZC1(xoff-1, yoff) : NZC0(xoff-1, yoff);
        nA = (im == 25) * 16 + (im != 3) * nz;
    }
    if (yoff > 0) {
        uint8_t im = IMODE(xoff/2, (yoff-1)/2);
        uint8_t nz = ch ? NZC1(xoff, yoff-1) : NZC0(xoff, yoff-1);
        nB = (im == 25) * 16 + (im != 3) * nz;
    }
    return (nA + nB + (yoff > 0) * (xoff > 0)) >> ((yoff > 0) * (xoff > 0));
}

/* ---- 4x4 IDCT + dequant ---- */
static const int vt_table[6][2][2] = {
    {{160,208},{208,256}},{{208,224},{224,288}},{{208,256},{256,320}},
    {{224,288},{288,368}},{{256,320},{320,400}},{{288,368},{368,464}},
};

static void idct4x4(int qP, int qPm6, int t1, int t2, int t3,
                     int c[4][4], int r[4][4], int DC_comp, uint8_t flag)
{
    int i, j, i1, j1;
    int temp[4][4], tmp[4][4], f[4];
    if (qP >= 24)
        for (i = 0; i < 4; i++)
            for (j = 0; j < 4; j++)
                temp[i][j] = (c[i][j] * vt_table[qPm6][i&1][j&1]) << t1;
    else
        for (i = 0; i < 4; i++)
            for (j = 0; j < 4; j++)
                temp[i][j] = (c[i][j] * vt_table[qPm6][i&1][j&1] + t3) >> t2;
    if (flag) temp[0][0] = DC_comp;
    for (j = 0; j < 4; j++) {
        f[0] = temp[0][j] + temp[2][j];
        f[1] = temp[0][j] - temp[2][j];
        f[2] = (temp[1][j] >> 1) - temp[3][j];
        f[3] = temp[1][j] + (temp[3][j] >> 1);
        for (i = 0; i < 2; i++) {
            i1 = 3 - i;
            tmp[i][j] = f[i] + f[i1];
            tmp[i1][j] = f[i] - f[i1];
        }
    }
    for (i = 0; i < 4; i++) {
        f[0] = tmp[i][0] + tmp[i][2];
        f[1] = tmp[i][0] - tmp[i][2];
        f[2] = (tmp[i][1] >> 1) - tmp[i][3];
        f[3] = tmp[i][1] + (tmp[i][3] >> 1);
        for (j = 0; j < 2; j++) {
            j1 = 3 - j;
            r[i][j]  = (f[j] + f[j1] + 32) >> 6;
            r[i][j1] = (f[j] - f[j1] + 32) >> 6;
        }
    }
}

static void hadamard16x16dc(int qP, int c[4][4], int qPm6,
                              int s1, int s2, int s3)
{
    int i, j;
    int inv[4][4] = {{1,1,1,1},{1,1,-1,-1},{1,-1,-1,1},{1,-1,1,-1}};
    int f[4][4], temp[4][4];
    int ls = vt_table[qPm6][0][0];
    /* inv1 * c */
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            int sum = 0, k;
            for (k = 0; k < 4; k++) sum += inv[i][k] * c[k][j];
            temp[i][j] = sum;
        }
    /* temp * inv1 */
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            int sum = 0, k;
            for (k = 0; k < 4; k++) sum += temp[i][k] * inv[k][j];
            f[i][j] = sum;
        }
    if (qP >= 36)
        for (i = 0; i < 4; i++)
            for (j = 0; j < 4; j++)
                c[i][j] = (f[i][j] * ls) << s1;
    else
        for (i = 0; i < 4; i++)
            for (j = 0; j < 4; j++)
                c[i][j] = (f[i][j] * ls + s3) >> s2;
}

static void hadamard_chroma2x2(int c[2][2], int qP, int qPcm6)
{
    int temp = vt_table[qPcm6][0][0];
    int t00 = c[0][0]+c[1][0]+c[0][1]+c[1][1];
    int t10 = c[0][0]-c[1][0]+c[0][1]-c[1][1];
    int t01 = c[0][0]+c[1][0]-c[0][1]-c[1][1];
    int t11 = c[0][0]-c[1][0]-c[0][1]+c[1][1];
    c[0][0] = ((t00 * temp) << (qP / 6)) >> 5;
    c[1][0] = ((t10 * temp) << (qP / 6)) >> 5;
    c[0][1] = ((t01 * temp) << (qP / 6)) >> 5;
    c[1][1] = ((t11 * temp) << (qP / 6)) >> 5;
}

/* ---- Intra prediction (col-major via SY/SCB/SCR macros) ---- */
static void intra_info_parse(nalu_t *n, uint8_t cip, int startx, int starty,
                              char tmpmode[16])
{
    int k, x, y, xoff, yoff;
    int8_t predA, predB;
    for (k = 0; k < 16; k++) {
        x = KTOX(k); y = KTOY(k);
        xoff = startx + x; yoff = starty + y;
        predA = 2; predB = 2;
        if (xoff > 0 && cip == 0 && yoff > 0) {
            predA = IPM(xoff - 1, yoff);
            predB = IPM(xoff, yoff - 1);
        }
        int modetmp = MIN(predA, predB);
        uint8_t prev = bs_u1(n);
        if (prev) {
            tmpmode[k] = modetmp;
        } else {
            tmpmode[k] = bs_un(3, n);
            if (tmpmode[k] >= modetmp) tmpmode[k]++;
        }
        IPM(xoff, yoff) = tmpmode[k];
    }
}

static void intra4x4_predict(uint8_t predL[4][4], uint8_t mode,
                               uint8_t avail, int sx, int sy, unsigned int blk)
{
    uint8_t PA, PB, PC, PD, PE, PF, PG, PH, PI, PJ, PK, PL, PX;
    int i, j;
    if (avail & 1) { PA=SY(sx,sy-1); PB=SY(sx+1,sy-1); PC=SY(sx+2,sy-1); PD=SY(sx+3,sy-1); }
    else { PA=PB=PC=PD=128; }
    if (blk==3||blk==11||blk==13||blk==7||blk==15||!(avail&1)||sx+4>=pic_w||sy==0)
        { PE=PF=PG=PH=PD; }
    else { PE=SY(sx+4,sy-1); PF=SY(sx+5,sy-1); PG=SY(sx+6,sy-1); PH=SY(sx+7,sy-1); }
    if (avail & 2) { PI=SY(sx-1,sy); PJ=SY(sx-1,sy+1); PK=SY(sx-1,sy+2); PL=SY(sx-1,sy+3); }
    else { PI=PJ=PK=PL=128; }
    PX = (avail == 3) ? SY(sx-1,sy-1) : 128;

    switch (mode) {
    case 0: /* VERT */
        for (i=0;i<4;i++) { predL[0][i]=PA; predL[1][i]=PB; predL[2][i]=PC; predL[3][i]=PD; }
        break;
    case 1: /* HOR */
        for (i=0;i<4;i++) { predL[i][0]=PI; predL[i][1]=PJ; predL[i][2]=PK; predL[i][3]=PL; }
        break;
    case 2: /* DC */ {
        uint8_t s0 = 128;
        if (avail == 3) s0 = (PA+PB+PC+PD+PI+PJ+PK+PL+4)>>3;
        else if (avail == 2) s0 = (PI+PJ+PK+PL+2)>>2;
        else if (avail == 1) s0 = (PA+PB+PC+PD+2)>>2;
        for (j=0;j<4;j++) for (i=0;i<4;i++) predL[i][j]=s0;
        break; }
    case 3: /* DIAG_DOWN_LEFT */
        predL[0][0]=(PA+PC+2*PB+2)/4; predL[1][0]=predL[0][1]=(PB+PD+2*PC+2)/4;
        predL[2][0]=predL[1][1]=predL[0][2]=(PC+PE+2*PD+2)/4;
        predL[3][0]=predL[2][1]=predL[1][2]=predL[0][3]=(PD+PF+2*PE+2)/4;
        predL[3][1]=predL[2][2]=predL[1][3]=(PE+PG+2*PF+2)/4;
        predL[3][2]=predL[2][3]=(PF+PH+2*PG+2)/4;
        predL[3][3]=(PG+3*PH+2)/4;
        break;
    case 4: /* DIAG_DOWN_RIGHT */
        predL[0][3]=(PL+2*PK+PJ+2)/4; predL[0][2]=predL[1][3]=(PK+2*PJ+PI+2)/4;
        predL[0][1]=predL[1][2]=predL[2][3]=(PJ+2*PI+PX+2)/4;
        predL[0][0]=predL[1][1]=predL[2][2]=predL[3][3]=(PI+2*PX+PA+2)/4;
        predL[1][0]=predL[2][1]=predL[3][2]=(PX+2*PA+PB+2)/4;
        predL[2][0]=predL[3][1]=(PA+2*PB+PC+2)/4;
        predL[3][0]=(PB+2*PC+PD+2)/4;
        break;
    case 5: /* VERT_RIGHT */
        predL[0][0]=predL[1][2]=(PX+PA+1)/2; predL[1][0]=predL[2][2]=(PA+PB+1)/2;
        predL[2][0]=predL[3][2]=(PB+PC+1)/2; predL[3][0]=(PC+PD+1)/2;
        predL[0][1]=predL[1][3]=(PI+2*PX+PA+2)/4; predL[1][1]=predL[2][3]=(PX+2*PA+PB+2)/4;
        predL[2][1]=predL[3][3]=(PA+2*PB+PC+2)/4; predL[3][1]=(PB+2*PC+PD+2)/4;
        predL[0][2]=(PX+2*PI+PJ+2)/4; predL[0][3]=(PI+2*PJ+PK+2)/4;
        break;
    case 6: /* HOR_DOWN */
        predL[0][0]=predL[2][1]=(PX+PI+1)/2; predL[1][0]=predL[3][1]=(PI+2*PX+PA+2)/4;
        predL[2][0]=(PX+2*PA+PB+2)/4; predL[3][0]=(PA+2*PB+PC+2)/4;
        predL[0][1]=predL[2][2]=(PI+PJ+1)/2; predL[1][1]=predL[3][2]=(PX+2*PI+PJ+2)/4;
        predL[0][2]=predL[2][3]=(PJ+PK+1)/2; predL[1][2]=predL[3][3]=(PI+2*PJ+PK+2)/4;
        predL[0][3]=(PK+PL+1)/2; predL[1][3]=(PJ+2*PK+PL+2)/4;
        break;
    case 7: /* VERT_LEFT */
        predL[0][0]=(PA+PB+1)/2; predL[1][0]=predL[0][2]=(PB+PC+1)/2;
        predL[2][0]=predL[1][2]=(PC+PD+1)/2; predL[3][0]=predL[2][2]=(PD+PE+1)/2;
        predL[3][2]=(PE+PF+1)/2;
        predL[0][1]=(PA+2*PB+PC+2)/4; predL[1][1]=predL[0][3]=(PB+2*PC+PD+2)/4;
        predL[2][1]=predL[1][3]=(PC+2*PD+PE+2)/4; predL[3][1]=predL[2][3]=(PD+2*PE+PF+2)/4;
        predL[3][3]=(PE+2*PF+PG+2)/4;
        break;
    case 8: /* HOR_UP */
        predL[0][0]=(PI+PJ+1)/2; predL[1][0]=(PI+2*PJ+PK+2)/4;
        predL[2][0]=predL[0][1]=(PJ+PK+1)/2; predL[3][0]=predL[1][1]=(PJ+2*PK+PL+2)/4;
        predL[2][1]=predL[0][2]=(PK+PL+1)/2; predL[3][1]=predL[1][2]=(PK+2*PL+PL+2)/4;
        predL[3][2]=predL[1][3]=predL[0][3]=predL[2][2]=predL[2][3]=predL[3][3]=PL;
        break;
    }
}

static void intra16x16_predict(uint8_t predL[16][4][4], uint8_t mode,
                                 uint8_t avail, int sx, int sy)
{
    int i, j, k, x, y;
    uint8_t v[16], h[16];
    if (avail & 2) for (i=0;i<16;i++) h[i]=SY(sx-1,sy+i); else for (i=0;i<16;i++) h[i]=128;
    if (avail & 1) for (i=0;i<16;i++) v[i]=SY(sx+i,sy-1); else for (i=0;i<16;i++) v[i]=128;
    uint8_t X = (avail==3) ? SY(sx-1,sy-1) : 128;
    switch (mode) {
    case 0: for (k=0;k<16;k++) for (i=0;i<4;i++) for (j=0;j<4;j++)
                predL[k][i][j]=v[KTOX(k)*4+i]; break;
    case 1: for (k=0;k<16;k++) for (i=0;i<4;i++) for (j=0;j<4;j++)
                predL[k][i][j]=h[KTOY(k)*4+j]; break;
    case 2: {
        int sumx=0,sumy=0,tmp;
        if (avail&1) for (x=0;x<16;x++) sumx+=v[x];
        if (avail&2) for (y=0;y<16;y++) sumy+=h[y];
        if (avail==3) tmp=(sumx+sumy+16)>>5;
        else if (avail==2) tmp=(sumy+8)>>4;
        else if (avail==1) tmp=(sumx+8)>>4;
        else tmp=128;
        for (k=0;k<16;k++) for (i=0;i<4;i++) for (j=0;j<4;j++) predL[k][i][j]=tmp;
        break; }
    default: { /* plane */
        int H=v[8]-v[6]+2*(v[9]-v[5])+3*(v[10]-v[4])+4*(v[11]-v[3])
              +5*(v[12]-v[2])+6*(v[13]-v[1])+7*(v[14]-v[0])+8*(v[15]-X);
        int V=h[8]-h[6]+2*(h[9]-h[5])+3*(h[10]-h[4])+4*(h[11]-h[3])
              +5*(h[12]-h[2])+6*(h[13]-h[1])+7*(h[14]-h[0])+8*(h[15]-X);
        H=(5*H+32)>>6; V=(5*V+32)>>6;
        int a=16*(v[15]+h[15]);
        for (k=0;k<16;k++) for (i=0;i<4;i++) for (j=0;j<4;j++) {
            x=KTOX(k)*4+i; y=KTOY(k)*4+j;
            int tmp=(a+H*(x-7)+V*(y-7)+16)>>5;
            predL[k][i][j]=Clip1y(tmp);
        }
        break; }
    }
}

static void chroma_predict(uint8_t predC[4][4][4], int ch,
                             uint8_t avail, int sx, int sy, uint8_t mode)
{
    int i, j, k, x, y;
    uint8_t v[8], h[8];
    if (avail&2) for (i=0;i<8;i++) h[i]=(ch?SCR(sx-1,sy+i):SCB(sx-1,sy+i));
    else for (i=0;i<8;i++) h[i]=128;
    if (avail&1) for (i=0;i<8;i++) v[i]=(ch?SCR(sx+i,sy-1):SCB(sx+i,sy-1));
    else for (i=0;i<8;i++) v[i]=128;
    uint8_t X=(avail==3)?(ch?SCR(sx-1,sy-1):SCB(sx-1,sy-1)):128;
    switch (mode) {
    case 0: { /* DC */
        int js0=0,js1=0,js2=0,js3=0;
        if (avail&1) for (x=0;x<4;x++) { js0+=v[x]; js1+=v[x+4]; }
        if (avail&2) for (y=0;y<4;y++) { js2+=h[y]; js3+=h[y+4]; }
        int t[2][2];
        if (avail==0) { t[0][0]=t[0][1]=t[1][0]=t[1][1]=128; }
        else if (avail==1) { t[0][0]=(js0+2)>>2; t[0][1]=(js1+2)>>2; t[1][0]=(js0+2)>>2; t[1][1]=(js1+2)>>2; }
        else if (avail==2) { t[0][0]=(js2+2)>>2; t[0][1]=(js2+2)>>2; t[1][0]=(js3+2)>>2; t[1][1]=(js3+2)>>2; }
        else { t[0][0]=(js2+js0+4)>>3; t[0][1]=(js1+2)>>2; t[1][0]=(js3+2)>>2; t[1][1]=(js1+js3+4)>>3; }
        for (i=0;i<2;i++) for (j=0;j<2;j++) for (x=0;x<4;x++) for (y=0;y<4;y++)
            predC[j+i*2][x][y]=t[i][j];
        break; }
    case 1: for (k=0;k<4;k++) for (i=0;i<4;i++) for (j=0;j<4;j++)
                predC[k][i][j]=h[(k/2)*4+j]; break;
    case 2: for (k=0;k<4;k++) for (i=0;i<4;i++) for (j=0;j<4;j++)
                predC[k][i][j]=v[(k%2)*4+i]; break;
    default: { /* plane */
        int H=v[4]-v[2]+2*(v[5]-v[1])+3*(v[6]-v[0])+4*(v[7]-X);
        int V=h[4]-h[2]+2*(h[5]-h[1])+3*(h[6]-h[0])+4*(h[7]-X);
        H=(17*H+16)>>5; V=(17*V+16)>>5;
        int a=16*(v[7]+h[7]);
        for (k=0;k<4;k++) for (i=0;i<4;i++) for (j=0;j<4;j++) {
            int tmp=(a+H*((k%2)*4+i-3)+V*((k/2)*4+j-3)+16)>>5;
            predC[k][i][j]=Clip1y(tmp);
        }
        break; }
    }
}

/* Write reconstructed pixels to col-major frame */
static void write_luma_sw(uint8_t pred[4][4], int rmb[4][4],
                            int sx, int sy, uint8_t has_res)
{
    int i, j;
    for (i=0;i<4;i++) for (j=0;j<4;j++)
        SY(sx+i,sy+j) = Clip1y(has_res * rmb[i][j] + pred[i][j]);
}

static void write_chroma_sw(uint8_t pred[4][4], int rmb[4][4],
                              int ch, int sx, int sy, uint8_t is_skip)
{
    int i, j;
    for (i=0;i<4;i++) for (j=0;j<4;j++) {
        int pix = Clip1y((is_skip == 0) * rmb[i][j] + pred[i][j]);
        if (ch) SCR(sx+i,sy+j) = pix; else SCB(sx+i,sy+j) = pix;
    }
}

/* ---- Annex B NALU scanner ---- */
static int find_start_code(const uint8_t *buf, int len, int *sc_len)
{
    int i;
    for (i = 0; i < len - 3; i++) {
        if (buf[i] == 0 && buf[i+1] == 0) {
            if (buf[i+2] == 1) { *sc_len = 3; return i; }
            if (i < len - 4 && buf[i+2] == 0 && buf[i+3] == 1) { *sc_len = 4; return i; }
        }
    }
    return -1;
}

static int ebsp_to_rbsp(uint8_t *dst, const uint8_t *src, int src_len)
{
    int i, j = 0, zeros = 0;
    for (i = 0; i < src_len; i++) {
        if (zeros == 2 && src[i] == 3) { zeros = 0; continue; }
        if (src[i] == 0) zeros++; else zeros = 0;
        dst[j++] = src[i];
    }
    return j;
}

/* ---- I-slice decode ---- */
static void decode_i_slice(nalu_t *n, int slice_qp, int chroma_off,
                            uint8_t *coeff_buf, uint8_t *small_a,
                            uint8_t *small_b, int *frame_toggle_p)
{
    int qPprev = slice_qp;
    const int qPCtable[22] = {29,30,31,32,32,33,34,34,35,35,36,36,37,37,37,38,38,38,39,39,39,39};
    const int power2[6] = {1,2,4,8,16,32};
    int mbx, mby, k, i, j, x, y;
    uint32_t coeff_phys = (uint32_t)coeff_buf;
    uint32_t sa_phys = (uint32_t)small_a;
    uint32_t sb_phys = (uint32_t)small_b;
    int frame_toggle = *frame_toggle_p;

    for (mby = 0; mby < pic_hmb; mby++)
    for (mbx = 0; mbx < pic_wmb; mbx++) {
        uint8_t MbType = bs_ue(n);
        uint8_t tmpImode = (MbType == 0 || MbType == 25) ? MbType : 1;
        IMODE(mbx, mby) = tmpImode;

        uint8_t CodedPatternLuma = 0, CodedPatternChroma = 0;
        uint8_t IntraChromaPredMode = 0, Intra16x16PredMode = 0;
        int mb_qp_delta;
        char i4predmode[16];
        int coeffDCL[4][4];
        uint8_t predL[16][4][4];
        int8_t qPm6 = 0, qPy = 26, qPc = 26, qPcm6 = 0;
        int8_t t1l = 0, t2l = 0, t3l = 0, t1c = 0, t2c = 0, t3c = 0;
        int8_t s1 = 0, s2 = 0, s3 = 0;

        if (tmpImode == 0) { /* INTRA4x4 */
            intra_info_parse(n, 0, mbx*4, mby*4, i4predmode);
        } else if (tmpImode == 1) { /* INTRA16x16 */
            for (i=0;i<4;i++) { IPM(mbx*4+3,mby*4+i)=2; IPM(mbx*4+i,mby*4+3)=2; }
            Intra16x16PredMode = (MbType - 1) % 4;
        }
        if (tmpImode == 0 || tmpImode == 1)
            IntraChromaPredMode = bs_ue(n);

        if (tmpImode != 1) { /* not INTRA16x16: parse CBP */
            uint8_t cbp = bs_me((tmpImode != 0), n);
            CodedPatternLuma = cbp % 16;
            CodedPatternChroma = cbp / 16;
        } else {
            CodedPatternChroma = (MbType - 1) / 4 % 3;
            CodedPatternLuma = MbType > 12 ? 15 : 0;
        }

        if (CodedPatternChroma > 0 || CodedPatternLuma > 0 || tmpImode == 1) {
            mb_qp_delta = bs_se(n);
            qPprev += mb_qp_delta;
            qPy = qPprev; qPm6 = qPy % 6;
            t1l = qPy/6 - 4; t2l = 4 - qPy/6;
            t3l = (t1l < 0) ? power2[3-qPy/6] : 0;
            s1 = qPy/6 - 6; s2 = 6 - qPy/6;
            s3 = (s1 < 0) ? power2[5-qPy/6] : 0;
            int qPi = Clip3(0, 51, qPy + chroma_off);
            qPc = (qPi < 30) ? qPi : qPCtable[qPi - 30];
            qPcm6 = qPc % 6;
            t1c = qPc/6 - 4; t2c = 4 - qPc/6;
            t3c = 1 << (3 - qPc/6);
        }

        /* Intra16x16 DC + prediction */
        if (tmpImode == 1) {
            int nC = nc_luma(mbx*4, mby*4);
            cavlc_decode_16(coeffDCL, n, 0, 15, nC);
            hadamard16x16dc(qPy, coeffDCL, qPm6, s1, s2, s3);
            intra16x16_predict(predL, Intra16x16PredMode,
                               (mbx>0)*2+(mby>0), mbx*16, mby*16);
        }

        /* 16 luma sub-blocks */
        for (k = 0; k < 16; k++) {
            x = KTOX(k); y = KTOY(k);
            int coeffACL[4][4];
            int rMbL[4][4];
            if (CodedPatternLuma & (1 << (k/4))) {
                int nC = nc_luma(mbx*4+x, mby*4+y);
                NZL(mbx*4+x,mby*4+y) = cavlc_decode_16(coeffACL, n,
                    (tmpImode==1), 15, nC);
            } else if (tmpImode == 1) {
                NZL(mbx*4+x,mby*4+y) = 0;
                for (i=0;i<4;i++) for (j=0;j<4;j++) coeffACL[i][j]=0;
            } else {
                NZL(mbx*4+x,mby*4+y) = 0;
            }
            uint8_t has_res = (CodedPatternLuma & (1<<(k/4))) || tmpImode==1;
            if (has_res)
                idct4x4(qPy, qPm6, t1l, t2l, t3l, coeffACL, rMbL,
                         coeffDCL[x][y], (tmpImode==1));
            if (tmpImode == 0) {
                uint8_t avail = ((mbx*4+x)>0)*2 + ((mby*4+y)>0);
                uint8_t p4[4][4];
                intra4x4_predict(p4, i4predmode[k], avail,
                                  (mbx*4+x)*4, (mby*4+y)*4, k);
                write_luma_sw(p4, rMbL, (mbx*4+x)*4, (mby*4+y)*4, has_res);
            } else {
                write_luma_sw(predL[k], rMbL, (mbx*4+x)*4, (mby*4+y)*4, has_res);
            }
        }

        /* Chroma DC + AC + prediction */
        int coeffDCC0[4][2], coeffDCC1[4][2];
        if (CodedPatternChroma & 3) {
            cavlc_decode_4((int(*)[2])coeffDCC0, n, 0, 3);
            hadamard_chroma2x2((int(*)[2])coeffDCC0, qPc, qPc%6);
            cavlc_decode_4((int(*)[2])coeffDCC1, n, 0, 3);
            hadamard_chroma2x2((int(*)[2])coeffDCC1, qPc, qPc%6);
        }

        uint8_t predC0[4][4][4], predC1[4][4][4];
        chroma_predict(predC0, 0, (mbx>0)*2+(mby>0), mbx*8, mby*8, IntraChromaPredMode);
        chroma_predict(predC1, 1, (mbx>0)*2+(mby>0), mbx*8, mby*8, IntraChromaPredMode);

        /* Chroma AC: decode ALL Cb blocks first, then ALL Cr blocks
         * (H.264 bitstream order: all channel 0 AC, then all channel 1 AC) */
        {
            int coeffACC0[2][2][4][4], coeffACC1[2][2][4][4];
            int rMbC0[2][2][4][4], rMbC1[2][2][4][4];
            /* Cb AC (all 4 blocks) */
            for (y=0;y<2;y++) for (x=0;x<2;x++) {
                if (CodedPatternChroma & 2) {
                    int nC0 = nc_chroma(0, mbx*2+x, mby*2+y);
                    NZC0(mbx*2+x,mby*2+y) = cavlc_decode_16(coeffACC0[x][y], n, 1, 15, nC0);
                } else {
                    NZC0(mbx*2+x,mby*2+y) = 0;
                    for (i=0;i<4;i++) for (j=0;j<4;j++) coeffACC0[x][y][i][j] = 0;
                }
            }
            /* Cr AC (all 4 blocks) */
            for (y=0;y<2;y++) for (x=0;x<2;x++) {
                if (CodedPatternChroma & 2) {
                    int nC1 = nc_chroma(1, mbx*2+x, mby*2+y);
                    NZC1(mbx*2+x,mby*2+y) = cavlc_decode_16(coeffACC1[x][y], n, 1, 15, nC1);
                } else {
                    NZC1(mbx*2+x,mby*2+y) = 0;
                    for (i=0;i<4;i++) for (j=0;j<4;j++) coeffACC1[x][y][i][j] = 0;
                }
            }
            /* Scale + write both channels */
            for (y=0;y<2;y++) for (x=0;x<2;x++) {
                if (CodedPatternChroma & 3) {
                    idct4x4(qPc, qPcm6, t1c, t2c, t3c, coeffACC0[x][y], rMbC0[x][y], coeffDCC0[x][y], 1);
                    idct4x4(qPc, qPcm6, t1c, t2c, t3c, coeffACC1[x][y], rMbC1[x][y], coeffDCC1[x][y], 1);
                } else {
                    for (i=0;i<4;i++) for (j=0;j<4;j++) { rMbC0[x][y][i][j]=0; rMbC1[x][y][i][j]=0; }
                }
                write_chroma_sw(predC0[x+y*2], rMbC0[x][y], 0, (mbx*2+x)*4, (mby*2+y)*4, 0);
                write_chroma_sw(predC1[x+y*2], rMbC1[x][y], 1, (mbx*2+x)*4, (mby*2+y)*4, 0);
            }
        }

#if USE_HW_PIPELINE
        /* Forward DCT on reconstructed MB and submit to HW.
         * Apple's FUN_0009144c never clears SUB/MAIN status between sub-calls.
         * DEBLK+14 poll at start of next hw_mb_submit handles synchronization. */
        {
            uint32_t *cb = (uint32_t *)(void *)coeff_buf;
            uint8_t mb_pix[8][8];
            /* Y-top: rows 0-7 (two 8x8 blocks) */
            for (i=0;i<128;i++) cb[i]=0;
            for (int blk = 0; blk < 2; blk++) {
                for (i=0;i<8;i++) for (j=0;j<8;j++)
                    mb_pix[i][j] = SY(mbx*16+blk*8+j, mby*16+i);
                fill_coeff_from_pixels(mb_pix, cb + blk*64, 1);
            }
            rb->commit_dcache();
            hw_mb_submit(coeff_phys, sa_phys, sb_phys, frame_toggle, 0);
            rb->commit_discard_dcache();
            readback_luma((frame_toggle?small_b:small_a), frame_y, mbx, mby, 0);
            frame_toggle ^= 1;

            /* Y-bottom: rows 8-15 */
            for (i=0;i<128;i++) cb[i]=0;
            for (int blk = 0; blk < 2; blk++) {
                for (i=0;i<8;i++) for (j=0;j<8;j++)
                    mb_pix[i][j] = SY(mbx*16+blk*8+j, mby*16+8+i);
                fill_coeff_from_pixels(mb_pix, cb + blk*64, 1);
            }
            rb->commit_dcache();
            hw_mb_submit(coeff_phys, sa_phys, sb_phys, frame_toggle, 0);
            rb->commit_discard_dcache();
            readback_luma((frame_toggle?small_b:small_a), frame_y, mbx, mby, 8);
            frame_toggle ^= 1;

            /* Chroma */
            for (i=0;i<128;i++) cb[i]=0;
            /* block0 = Cb */
            for (i=0;i<8;i++) for (j=0;j<8;j++)
                mb_pix[i][j] = SCB(mbx*8+j, mby*8+i);
            fill_coeff_from_pixels(mb_pix, cb, 1);
            /* block1 = Cr */
            for (i=0;i<8;i++) for (j=0;j<8;j++)
                mb_pix[i][j] = SCR(mbx*8+j, mby*8+i);
            fill_coeff_from_pixels(mb_pix, cb + 64, 1);
            rb->commit_dcache();
            hw_mb_submit(coeff_phys, sa_phys, sb_phys, frame_toggle, 1);
            rb->commit_discard_dcache();
            readback_chroma((frame_toggle?small_b:small_a), frame_cb, frame_cr, mbx, mby);
            frame_toggle ^= 1;
        }
#endif /* USE_HW_PIPELINE */

        if ((mbx % 40) == 0)
            poc_log("  MB[%d](%d,%d) imode=%d", mby*pic_wmb+mbx, mbx, mby, tmpImode);
    }

#if USE_HW_PIPELINE
    /* deblock flush: push held deblock data through pipeline */
    {
        rb->commit_dcache();
        hw_mb_submit((uint32_t)coeff_buf, sa_phys, sb_phys, frame_toggle, 0);
        rb->commit_discard_dcache();
        frame_toggle ^= 1;
    }
#else
    /* SW bypass: copy col-major SW output to row-major frame buffers */
    {
        int row, col;
        for (row = 0; row < pic_h; row++)
            for (col = 0; col < pic_w; col++)
                frame_y[row * pic_w + col] = SY(col, row);
        for (row = 0; row < pic_ch; row++)
            for (col = 0; col < pic_cw; col++) {
                frame_cb[row * pic_cw + col] = SCB(col, row);
                frame_cr[row * pic_cw + col] = SCR(col, row);
            }
    }
#endif
    *frame_toggle_p = frame_toggle;
    poc_log("  I-slice decode complete (%d MBs)", pic_wmb * pic_hmb);
}

/* ===================== MAIN ===================== */
enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    size_t buf_size;
    uint8_t *buf, *p;
    uint8_t *dma_work, *work_buf1, *work_buf2;
    uint8_t *small_a, *small_b;
    uint8_t *coeff_buf;
    uint8_t *file_buf;
    uint8_t *nalu_buf;
    uint32_t v;
    int i;
    int total_mbs = TEST_WIDTH_MBS * TEST_HEIGHT_MBS;

    rb->splash(HZ/2, "v36b START");

    log_fd = rb->open(LOG_PATH, O_WRONLY|O_CREAT|O_TRUNC, 0666);
    poc_log("=== v36b — I-frame decode, deblock fix (%dx%d) ===",
            TEST_WIDTH, TEST_HEIGHT);

    /* ---- Allocate buffers from audio buffer ---- */
    buf = rb->plugin_get_audio_buffer(&buf_size);
    poc_log("Audio buffer: %08lx size=%lu",
            (unsigned long)(uintptr_t)buf, (unsigned long)buf_size);

    if (buf_size < DMA_WORK_SIZE + 2 * WORK_BUF_SIZE +
                   2 * SMALL_BUF_SIZE + FRAME_Y_SIZE +
                   FRAME_CB_SIZE + FRAME_CR_SIZE +
                   COEFF_BUF_SIZE + 0x6000) {
        poc_log("ERROR: buffer too small");
        if (log_fd >= 0) rb->close(log_fd);
        rb->splash(HZ*3, "Buffer too small!");
        return PLUGIN_ERROR;
    }

    p = (uint8_t *)ALIGN32(buf);

    dma_work  = p;                        p += DMA_WORK_SIZE;
    work_buf1 = (uint8_t *)ALIGN32(p);    p = work_buf1 + WORK_BUF_SIZE;
    work_buf2 = (uint8_t *)ALIGN32(p);    p = work_buf2 + WORK_BUF_SIZE;
    small_a   = (uint8_t *)ALIGN4K(p);    p = small_a + SMALL_BUF_SIZE;
    small_b   = (uint8_t *)ALIGN4K(p);    p = small_b + SMALL_BUF_SIZE;
    frame_y   = (uint8_t *)ALIGN4K(p);    p = frame_y + FRAME_Y_SIZE;
    frame_cb  = (uint8_t *)ALIGN32(p);    p = frame_cb + FRAME_CB_SIZE;
    frame_cr  = (uint8_t *)ALIGN32(p);    p = frame_cr + FRAME_CR_SIZE;
    coeff_buf = (uint8_t *)ALIGN32(p);    p = coeff_buf + COEFF_BUF_SIZE;

    /* v36: SW decode buffers (col-major frame + state arrays + NALU buf) */
    sw_y   = (uint8_t *)ALIGN32(p);  p = sw_y  + FRAME_Y_SIZE;
    sw_cb  = (uint8_t *)ALIGN32(p);  p = sw_cb + FRAME_CB_SIZE;
    sw_cr  = (uint8_t *)ALIGN32(p);  p = sw_cr + FRAME_CR_SIZE;
    nalu_buf = (uint8_t *)ALIGN32(p); p = nalu_buf + MAX_NALU_BUF;
    ipm_buf  = (int8_t *)ALIGN32(p);
    p = (uint8_t *)ipm_buf + (TEST_WIDTH/4)*(TEST_HEIGHT/4);
    nzl_buf  = (uint8_t *)ALIGN32(p);
    p = nzl_buf + (TEST_WIDTH/4)*(TEST_HEIGHT/4);
    nzc0_buf = (uint8_t *)ALIGN32(p);
    p = nzc0_buf + (TEST_WIDTH/8)*(TEST_HEIGHT/8);
    nzc1_buf = (uint8_t *)ALIGN32(p);
    p = nzc1_buf + (TEST_WIDTH/8)*(TEST_HEIGHT/8);
    mbim_buf = (uint8_t *)ALIGN32(p);
    p = mbim_buf + TEST_WIDTH_MBS * TEST_HEIGHT_MBS;
    file_buf = (uint8_t *)ALIGN32(p);

    rb->memset(dma_work, 0, DMA_WORK_SIZE);
    rb->memset(work_buf1, 0xBB, WORK_BUF_SIZE);
    rb->memset(work_buf2, 0, WORK_BUF_SIZE);
    rb->memset(small_a, 0xCC, SMALL_BUF_SIZE);
    rb->memset(small_b, 0xCC, SMALL_BUF_SIZE);
    rb->memset(frame_y, 0, FRAME_Y_SIZE);
    rb->memset(frame_cb, 0x80, FRAME_CB_SIZE);
    rb->memset(frame_cr, 0x80, FRAME_CR_SIZE);
    rb->memset(coeff_buf, 0, COEFF_BUF_SIZE);

    poc_log("Buffers allocated:");
    poc_log("  dma_work=%08lx", (unsigned long)(uintptr_t)dma_work);
    poc_log("  work1=%08lx work2=%08lx",
            (unsigned long)(uintptr_t)work_buf1,
            (unsigned long)(uintptr_t)work_buf2);
    poc_log("  small_a=%08lx small_b=%08lx",
            (unsigned long)(uintptr_t)small_a,
            (unsigned long)(uintptr_t)small_b);
    poc_log("  frame_y=%08lx frame_cb=%08lx frame_cr=%08lx",
            (unsigned long)(uintptr_t)frame_y,
            (unsigned long)(uintptr_t)frame_cb,
            (unsigned long)(uintptr_t)frame_cr);
    poc_log("  coeff_buf=%08lx (%d bytes)",
            (unsigned long)(uintptr_t)coeff_buf, COEFF_BUF_SIZE);
    lflush();

    /* ---- Power on decoder ---- */
    poc_log("--- Power on ---");
    rb->splash(HZ/4, "Power on...");
    vdec_power_on();

    v = REG32(VDEC_MAIN + 0x08);
    poc_log("MAIN+08 = %08lx (hw status)", (unsigned long)v);
    if (v == 0)
        poc_log("WARNING: MAIN+08 is 0, decoder may not be powered");
    lflush();

    rb->commit_dcache();

    poc_log("--- Status BEFORE decode ---");
    poc_log("  MAIN+00=%08lx SUB+00=%08lx",
            (unsigned long)REG32(VDEC_MAIN),
            (unsigned long)REG32(VDEC_SUB));
    lflush();

    rb->splash(HZ/4, "Decoding...");

    /* Phase 1: Reset */
    poc_log("Phase 1: Reset");
    vdec_reset();

    /* Phase 2: H.264 init */
    poc_log("Phase 2: H.264 init");
    vdec_h264_init(
        (uint32_t)(uintptr_t)dma_work,
        (uint32_t)(uintptr_t)work_buf1
    );
    poc_log("  MAIN+04=%08lx SUB+10=%08lx MAIN+10=%08lx SUB+6C=%08lx",
            (unsigned long)REG32(VDEC_MAIN + 0x04),
            (unsigned long)REG32(VDEC_SUB + 0x10),
            (unsigned long)REG32(VDEC_MAIN + 0x10),
            (unsigned long)REG32(VDEC_SUB + 0x6C));
    lflush();

    /* ---- Common physical addresses for diagnostics + decode ---- */
    {
    uint32_t dma_phys   = (uint32_t)(uintptr_t)dma_work;
    uint32_t work1_phys = (uint32_t)(uintptr_t)work_buf1;
    uint32_t coeff_phys = (uint32_t)(uintptr_t)coeff_buf;
    uint32_t sa_phys    = (uint32_t)(uintptr_t)small_a;
    uint32_t sb_phys    = (uint32_t)(uintptr_t)small_b;

    /* ---- Phase 2.5: Stride test (non-uniform DC) ---- */
    poc_log("--- Stride test (dc0=10 -> 0x94, dc1=50 -> 0xE4) ---");
    {
        uint32_t *cb = (uint32_t *)(void *)coeff_buf;
        const uint32_t *w;

        fill_coeff_pair(cb, 10, 50);
        rb->memset(small_a, 0xBB, SMALL_BUF_SIZE);
        rb->memset(small_b, 0x00, SMALL_BUF_SIZE);
        rb->commit_dcache();
        hw_mb_submit(coeff_phys, sa_phys, sb_phys, 0, 0);
        wait_and_clear();
        rb->commit_discard_dcache();

        /* Dump row 0, then check candidate stride offsets */
        w = (const uint32_t *)small_a;
        poc_log("  +000: %08lx %08lx %08lx %08lx",
                (unsigned long)w[0], (unsigned long)w[1],
                (unsigned long)w[2], (unsigned long)w[3]);
        w = (const uint32_t *)(small_a + 32);
        poc_log("  +020: %08lx %08lx %08lx %08lx (stride=32)",
                (unsigned long)w[0], (unsigned long)w[1],
                (unsigned long)w[2], (unsigned long)w[3]);
        w = (const uint32_t *)(small_a + 64);
        poc_log("  +040: %08lx %08lx %08lx %08lx (row2@s32)",
                (unsigned long)w[0], (unsigned long)w[1],
                (unsigned long)w[2], (unsigned long)w[3]);
        {
            uint32_t w_at_32 = *(uint32_t *)(small_a + 32);
            if (w_at_32 != 0xBBBBBBBB && w_at_32 != 0) {
                poc_log("Stride: 32 confirmed (MAIN+10=0x00010100, row1=%08lx)",
                        (unsigned long)w_at_32);
            } else {
                poc_log("Stride: NOT 32! +020=%08lx (sentinel)",
                        (unsigned long)w_at_32);
            }
        }
        vdec_reset();
        vdec_h264_init(dma_phys, work1_phys);
    }
    lflush();

    /* ---- Phase 2.6: Forward DCT round-trip validation ---- */
    /* Proves: pixels → forward 8×8 DCT → HW IDCT → same pixels (±1).
     * Uses scale_matrix=1 for maximum precision (no quantization loss).
     * Apple uses scale=16, but for our round-trip we control both sides. */
    poc_log("--- Round-trip test (scale=1): pixels -> fwd DCT -> HW IDCT ---");
    {
        uint8_t test_pix[8][8];
        uint32_t *cb = (uint32_t *)(void *)coeff_buf;
        int j, row, errors;

        /* Set XFORM scaling matrices to 1 for precision */
        for (i = 0; i < 64; i++) {
            REG32(VDEC_XFORM + 0x200 + i * 4) = 1;
            REG32(VDEC_XFORM + 0x300 + i * 4) = 1;
        }

        /* Test 1: Uniform block (200) — DC only */
        poc_log("  Test 1: uniform block (200), scale=1");
        for (i = 0; i < 8; i++)
            for (j = 0; j < 8; j++)
                test_pix[i][j] = 200;
        for (i = 0; i < 128; i++)
            cb[i] = 0;
        fill_coeff_from_pixels(test_pix, cb, 1);
        poc_log("    coeff DC=%ld", (long)(int32_t)__builtin_bswap32(cb[0]));

        rb->memset(small_a, 0xCC, SMALL_BUF_SIZE);
        rb->commit_dcache();
        hw_mb_submit(coeff_phys, sa_phys, sb_phys, 0, 0);
        wait_and_clear();
        rb->commit_discard_dcache();

        errors = 0;
        for (row = 0; row < 8; row++) {
            const uint32_t *s = (const uint32_t *)(small_a + row * 32);
            uint32_t w0 = __builtin_bswap32(s[0]);
            uint32_t w1 = __builtin_bswap32(s[1]);
            uint8_t g[8];
            g[0] = w0 & 0xff; g[1] = (w0 >> 8) & 0xff;
            g[2] = (w0 >> 16) & 0xff; g[3] = w0 >> 24;
            g[4] = w1 & 0xff; g[5] = (w1 >> 8) & 0xff;
            g[6] = (w1 >> 16) & 0xff; g[7] = w1 >> 24;
            if (row < 2)
                poc_log("    r%d: got %d %d %d %d %d %d %d %d (want 200)",
                        row, g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7]);
            for (j = 0; j < 8; j++) {
                int diff = (int)g[j] - 200;
                if (diff < -2 || diff > 2) errors++;
            }
        }
        poc_log("    errors (|diff|>2): %d/64", errors);
        vdec_reset();
        vdec_h264_init(dma_phys, work1_phys);
        for (i = 0; i < 64; i++) {
            REG32(VDEC_XFORM + 0x200 + i * 4) = 1;
            REG32(VDEC_XFORM + 0x300 + i * 4) = 1;
        }

        /* Test 2: Diagonal gradient (128 + row*8 + col*4) */
        poc_log("  Test 2: diagonal gradient, scale=1");
        for (i = 0; i < 8; i++)
            for (j = 0; j < 8; j++)
                test_pix[i][j] = (uint8_t)(128 + i * 8 + j * 4);
        for (i = 0; i < 128; i++)
            cb[i] = 0;
        fill_coeff_from_pixels(test_pix, cb, 1);
        poc_log("    coeff[0..3]: %08lx %08lx %08lx %08lx",
                (unsigned long)cb[0], (unsigned long)cb[1],
                (unsigned long)cb[2], (unsigned long)cb[3]);

        rb->memset(small_a, 0xCC, SMALL_BUF_SIZE);
        rb->commit_dcache();
        hw_mb_submit(coeff_phys, sa_phys, sb_phys, 0, 0);
        wait_and_clear();
        rb->commit_discard_dcache();

        errors = 0;
        for (row = 0; row < 8; row++) {
            const uint32_t *s = (const uint32_t *)(small_a + row * 32);
            uint32_t w0 = __builtin_bswap32(s[0]);
            uint32_t w1 = __builtin_bswap32(s[1]);
            uint8_t g[8];
            g[0] = w0 & 0xff; g[1] = (w0 >> 8) & 0xff;
            g[2] = (w0 >> 16) & 0xff; g[3] = w0 >> 24;
            g[4] = w1 & 0xff; g[5] = (w1 >> 8) & 0xff;
            g[6] = (w1 >> 16) & 0xff; g[7] = w1 >> 24;
            poc_log("    r%d: want %3d %3d %3d %3d %3d %3d %3d %3d",
                    row,
                    test_pix[row][0], test_pix[row][1],
                    test_pix[row][2], test_pix[row][3],
                    test_pix[row][4], test_pix[row][5],
                    test_pix[row][6], test_pix[row][7]);
            poc_log("        got  %3d %3d %3d %3d %3d %3d %3d %3d",
                    g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7]);
            for (j = 0; j < 8; j++) {
                int diff = (int)g[j] - (int)test_pix[row][j];
                if (diff < -2 || diff > 2) errors++;
            }
        }
        poc_log("    errors (|diff|>2): %d/64", errors);
        vdec_reset();
        vdec_h264_init(dma_phys, work1_phys);
    }
    lflush();

    /* ---- Phase 3: Per-MB decode with toggle + chroma readback ---- */
    poc_log("Phase 3: per-MB decode (%d MBs, toggle + chroma)", total_mbs);
    {
        int mb;
        int timeouts = 0;
        int frame_toggle = 0;
        uint32_t *cb = (uint32_t *)(void *)coeff_buf;
        uint8_t *active;

        poc_log("  small_a=%08lx small_b=%08lx",
                (unsigned long)sa_phys, (unsigned long)sb_phys);

        rb->commit_discard_dcache();

        for (mb = 0; mb < total_mbs; mb++) {
            int mb_col = mb % TEST_WIDTH_MBS;
            int mb_row = mb / TEST_WIDTH_MBS;
            uint32_t dc = (uint32_t)((mb_col + 1) * 3);

            /* Sub-call 1: Y-top (rows 0-7) */
            active = (frame_toggle == 0) ? small_a : small_b;
            fill_coeff_pair(cb, dc, dc);
            rb->memset(active, 0xCC, SMALL_BUF_SIZE);
            rb->commit_dcache();
            if (hw_mb_submit(coeff_phys, sa_phys, sb_phys,
                             frame_toggle, 0) < 0)
                timeouts++;
            rb->commit_discard_dcache();

            if (mb == 0)
                dump_small_buf(active, "Y-top");
            readback_luma(active, frame_y, mb_col, mb_row, 0);
            frame_toggle ^= 1;

            /* Sub-call 2: Y-bottom (rows 8-15) */
            active = (frame_toggle == 0) ? small_a : small_b;
            fill_coeff_pair(cb, dc, dc);
            rb->memset(active, 0xCC, SMALL_BUF_SIZE);
            rb->commit_dcache();
            if (hw_mb_submit(coeff_phys, sa_phys, sb_phys,
                             frame_toggle, 0) < 0)
                timeouts++;
            rb->commit_discard_dcache();

            if (mb == 0)
                dump_small_buf(active, "Y-bot");
            readback_luma(active, frame_y, mb_col, mb_row, 8);
            frame_toggle ^= 1;

            /* Sub-call 3: Chroma — DC=0 for neutral gray (Cb=Cr=128) */
            active = (frame_toggle == 0) ? small_a : small_b;
            fill_coeff_pair(cb, 0, 0);
            rb->memset(active, 0xCC, SMALL_BUF_SIZE);
            rb->commit_dcache();
            if (hw_mb_submit(coeff_phys, sa_phys, sb_phys,
                             frame_toggle, 1) < 0)
                timeouts++;
            rb->commit_discard_dcache();

            if (mb == 0)
                dump_small_buf(active, "Chroma");
            readback_chroma(active, frame_cb, frame_cr, mb_col, mb_row);
            frame_toggle ^= 1;

            /* Log first 2 + last MB */
            if (mb < 2 || mb == total_mbs - 1) {
                poc_log("  MB[%d](%d,%d): dc=%lu toggle=%d SUB=%08lx DEBLK+14=%08lx",
                        mb, mb_col, mb_row,
                        (unsigned long)dc, frame_toggle,
                        (unsigned long)REG32(VDEC_SUB),
                        (unsigned long)REG32(VDEC_DEBLK + 0x14));
            }

            if (timeouts >= 3) {
                poc_log("  ABORT: %d timeouts at MB %d", timeouts, mb);
                lflush();
                break;
            }
        }

        poc_log("  %d/%d MBs submitted (%d timeouts)", mb, total_mbs, timeouts);
        lflush();

        /* Deblock flush: extra submit after last MB (Apple's end-of-frame) */
        {
            uint32_t last_dc = (uint32_t)(TEST_WIDTH_MBS * 3);
            active = (frame_toggle == 0) ? small_a : small_b;
            fill_coeff_pair(cb, last_dc, last_dc);
            rb->memset(active, 0xCC, SMALL_BUF_SIZE);
            rb->commit_dcache();
            if (hw_mb_submit(coeff_phys, sa_phys, sb_phys,
                             frame_toggle, 0) < 0)
                poc_log("Deblock flush: TIMEOUT");
            else {
                rb->commit_discard_dcache();
                poc_log("Deblock flush: OK (toggle=%d)", frame_toggle);
            }
        }
    }
    lflush();

    /* ---- Phase 4: Real I-frame decode from .264 file ---- */
    poc_log("Phase 4: I-frame decode from %s", H264_TEST_PATH);
    {
        int fd = rb->open(H264_TEST_PATH, O_RDONLY);
        if (fd < 0) {
            poc_log("  File not found, skipping Phase 4");
        } else {
            off_t fsize = rb->filesize(fd);
            if (fsize > 500000) fsize = 500000;
            int fread_n = rb->read(fd, file_buf, fsize);
            rb->close(fd);
            poc_log("  Read %d bytes from file", fread_n);

            /* Set XFORM scaling to 1 for lossless round-trip */
            for (i = 0; i < 64; i++) {
                REG32(VDEC_XFORM + 0x200 + i * 4) = 1;
                REG32(VDEC_XFORM + 0x300 + i * 4) = 1;
            }

            /* Initialize SW decode frame dimensions */
            pic_w = TEST_WIDTH; pic_h = TEST_HEIGHT;
            pic_cw = TEST_WIDTH/2; pic_ch = TEST_HEIGHT/2;
            pic_wmb = TEST_WIDTH_MBS; pic_hmb = TEST_HEIGHT_MBS;
            rb->memset(sw_y, 128, FRAME_Y_SIZE);
            rb->memset(sw_cb, 128, FRAME_CB_SIZE);
            rb->memset(sw_cr, 128, FRAME_CR_SIZE);
            rb->memset(ipm_buf, 2, (pic_w/4)*(pic_h/4));
            rb->memset(nzl_buf, 0, (pic_w/4)*(pic_h/4));
            rb->memset(nzc0_buf, 0, (pic_cw/4)*(pic_ch/4));
            rb->memset(nzc1_buf, 0, (pic_cw/4)*(pic_ch/4));
            rb->memset(mbim_buf, 0, pic_wmb*pic_hmb);
            rb->memset(frame_y, 0, FRAME_Y_SIZE);
            rb->memset(frame_cb, 0x80, FRAME_CB_SIZE);
            rb->memset(frame_cr, 0x80, FRAME_CR_SIZE);

            /* Scan Annex B for NALUs */
            sps_t sps;
            pps_t pps;
            nalu_t nalu;
            int have_sps = 0, have_pps = 0;
            int pos = 0, sc_len;
            int slice_qp = 26, chroma_off = 0;

            rb->memset(&sps, 0, sizeof(sps));
            rb->memset(&pps, 0, sizeof(pps));

            while (pos < fread_n) {
                int sc_pos = find_start_code(file_buf + pos, fread_n - pos, &sc_len);
                if (sc_pos < 0) break;
                int nalu_start = pos + sc_pos + sc_len;
                /* Find next start code to determine NALU length */
                int sc2_len;
                int sc2_pos = find_start_code(file_buf + nalu_start,
                                               fread_n - nalu_start, &sc2_len);
                int nalu_len_raw = (sc2_pos >= 0) ? sc2_pos : (fread_n - nalu_start);

                /* EBSP → RBSP */
                int rbsp_len = ebsp_to_rbsp(nalu_buf, file_buf + nalu_start, nalu_len_raw);
                nalu.buf = nalu_buf;
                nalu.len = rbsp_len;
                nalu.bit_offset = 0;
                nalu.bit_length = rbsp_len * 8;
                uint8_t hdr = nalu.buf[0];
                nalu.nal_reference_idc = (hdr >> 5) & 3;
                nalu.nal_unit_type = hdr & 0x1F;

                poc_log("  NALU type=%d ref=%d len=%d",
                        nalu.nal_unit_type, nalu.nal_reference_idc, rbsp_len);

                if (nalu.nal_unit_type == 7) { /* SPS */
                    parse_sps(&sps, &nalu);
                    pic_wmb = sps.pic_width_in_mbs_minus1 + 1;
                    pic_hmb = sps.pic_height_in_map_units_minus1 + 1;
                    pic_w = pic_wmb * 16; pic_h = pic_hmb * 16;
                    pic_cw = pic_w / 2; pic_ch = pic_h / 2;
                    poc_log("  SPS: %dx%d (%dx%d MBs) profile=%d level=%d",
                            pic_w, pic_h, pic_wmb, pic_hmb,
                            sps.profile_idc, sps.level_idc);
                    have_sps = 1;
                } else if (nalu.nal_unit_type == 8) { /* PPS */
                    parse_pps(&pps, &nalu);
                    poc_log("  PPS: qp=%d chroma_off=%d deblk=%d",
                            26 + pps.pic_init_qp_minus26,
                            pps.chroma_qp_index_offset,
                            pps.deblocking_filter_control_present_flag);
                    have_pps = 1;
                } else if (nalu.nal_unit_type == 5 && have_sps && have_pps) {
                    /* IDR slice */
                    poc_log("  IDR slice: decoding %dx%d I-frame...", pic_w, pic_h);
                    lflush();

                    /* Re-init HW for actual frame dims */
                    vdec_reset();
                    vdec_h264_init(dma_phys, work1_phys);
                    for (i = 0; i < 64; i++) {
                        REG32(VDEC_XFORM + 0x200 + i * 4) = 1;
                        REG32(VDEC_XFORM + 0x300 + i * 4) = 1;
                    }

                    parse_sh_idr(&sps, &pps, &nalu, &slice_qp, &chroma_off);
                    poc_log("  slice_qp=%d chroma_off=%d bit_offset=%lu",
                            slice_qp, chroma_off, nalu.bit_offset);

                    int ft = 0;
                    decode_i_slice(&nalu, slice_qp, chroma_off,
                                    coeff_buf, small_a, small_b,
                                    &ft);
                    poc_log("  IDR decode done, toggle=%d", ft);

                    /* Dump SW decode output + checksums for verification */
                    {
                        uint32_t crc_sw = 0xFFFFFFFF;
                        uint32_t crc_hw = 0xFFFFFFFF;
                        int sz = pic_w * pic_h;
                        for (i = 0; i < sz; i++) {
                            crc_sw ^= sw_y[i];
                            crc_sw = (crc_sw >> 1) ^ (0xEDB88320 & -(crc_sw & 1));
                        }
                        for (i = 0; i < sz; i++) {
                            crc_hw ^= frame_y[i];
                            crc_hw = (crc_hw >> 1) ^ (0xEDB88320 & -(crc_hw & 1));
                        }
                        poc_log("  SW Y crc32=%08lx  HW Y crc32=%08lx",
                                (unsigned long)(crc_sw^0xFFFFFFFF),
                                (unsigned long)(crc_hw^0xFFFFFFFF));
                        /* Log first row of SW decode (col-major → row order) */
                        poc_log("  sw_y row0: %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",
                                SY(0,0),SY(1,0),SY(2,0),SY(3,0),
                                SY(4,0),SY(5,0),SY(6,0),SY(7,0),
                                SY(8,0),SY(9,0),SY(10,0),SY(11,0),
                                SY(12,0),SY(13,0),SY(14,0),SY(15,0));
                        /* SW vs HW diff metrics */
                        {
                            int diff_count = 0, max_diff = 0;
                            long sum_abs = 0;
                            int row, col;
                            for (row = 0; row < pic_h; row++) {
                                for (col = 0; col < pic_w; col++) {
                                    int sw_val = SY(col, row);
                                    int hw_val = frame_y[row * pic_w + col];
                                    int d = sw_val - hw_val;
                                    if (d < 0) d = -d;
                                    if (d > 0) diff_count++;
                                    if (d > max_diff) max_diff = d;
                                    sum_abs += d;
                                }
                            }
                            poc_log("  SW vs HW Y: %d/%d diffs, max=%d, MAE=%ld",
                                    diff_count, sz, max_diff,
                                    (long)(sum_abs / sz));
                        }
                        /* Dump sw_y to file (convert col-major to row-major) */
                        int dfd = rb->open("/vdec_sw_y.bin", O_WRONLY|O_CREAT|O_TRUNC, 0666);
                        if (dfd >= 0) {
                            int row;
                            for (row = 0; row < pic_h; row++) {
                                int col;
                                for (col = 0; col < pic_w; col++)
                                    nalu_buf[col] = SY(col, row);
                                rb->write(dfd, nalu_buf, pic_w);
                            }
                            rb->close(dfd);
                            poc_log("  sw_y dumped to /vdec_sw_y.bin (%d bytes)", sz);
                        }
                    }

                    lflush();
                    break;
                }
                pos = nalu_start + nalu_len_raw;
            }
        }
    }
    lflush();

    /* ---- Readback result ---- */
    poc_log("--- Readback result ---");
    {
        int nz_y = 0, nz_cb = 0, nz_cr = 0;
        for (i = 0; i < FRAME_Y_SIZE; i++)
            if (frame_y[i] != 0) nz_y++;
        for (i = 0; i < FRAME_CB_SIZE; i++) {
            if (frame_cb[i] != 0x80) nz_cb++;
            if (frame_cr[i] != 0x80) nz_cr++;
        }
        poc_log("frame_y: %d/%d non-zero", nz_y, FRAME_Y_SIZE);
        poc_log("frame_cb: %d/%d non-0x80, frame_cr: %d/%d non-0x80",
                nz_cb, FRAME_CB_SIZE, nz_cr, FRAME_CR_SIZE);
    }

    /* Hex dump sample rows */
    poc_log("--- frame_y hex (rows 0,1,15,16,239 stride=%d) ---", TEST_WIDTH);
    {
        int row, off;
        static const int check_rows[] = {0, 1, 15, 16, 239};
        for (i = 0; i < 5; i++) {
            row = check_rows[i];
            if (row >= TEST_HEIGHT) continue;
            off = row * TEST_WIDTH;
            poc_log("  row%d: %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx",
                    row,
                    (unsigned long)*(uint32_t *)(frame_y + off),
                    (unsigned long)*(uint32_t *)(frame_y + off + 4),
                    (unsigned long)*(uint32_t *)(frame_y + off + 8),
                    (unsigned long)*(uint32_t *)(frame_y + off + 12),
                    (unsigned long)*(uint32_t *)(frame_y + off + 16),
                    (unsigned long)*(uint32_t *)(frame_y + off + 20),
                    (unsigned long)*(uint32_t *)(frame_y + off + 24),
                    (unsigned long)*(uint32_t *)(frame_y + off + 28));
        }
    }
    lflush();

    /* ---- Binary dump ---- */
    poc_log("--- Dumping frame_y to file ---");
    {
        int dump_fd;
        ssize_t written;
        dump_fd = rb->open("/vdec_framey.bin", O_WRONLY|O_CREAT|O_TRUNC, 0666);
        if (dump_fd >= 0) {
            written = rb->write(dump_fd, frame_y, FRAME_Y_SIZE);
            rb->close(dump_fd);
            poc_log("  frame_y dump: %ld bytes", (long)written);
        }
    }
    lflush();

    /* ---- Post-decode register dump ---- */
    poc_log("--- Post-decode register dump ---");
    poc_log("MAIN: +00=%08lx +04=%08lx +08=%08lx +10=%08lx +2C=%08lx",
            (unsigned long)REG32(VDEC_MAIN),
            (unsigned long)REG32(VDEC_MAIN + 0x04),
            (unsigned long)REG32(VDEC_MAIN + 0x08),
            (unsigned long)REG32(VDEC_MAIN + 0x10),
            (unsigned long)REG32(VDEC_MAIN + 0x2C));
    poc_log("SUB: +00=%08lx +10=%08lx +6C=%08lx +74=%08lx",
            (unsigned long)REG32(VDEC_SUB),
            (unsigned long)REG32(VDEC_SUB + 0x10),
            (unsigned long)REG32(VDEC_SUB + 0x6C),
            (unsigned long)REG32(VDEC_SUB + 0x74));
    poc_log("DEBLK: +00=%08lx +14=%08lx",
            (unsigned long)REG32(VDEC_DEBLK),
            (unsigned long)REG32(VDEC_DEBLK + 0x14));
    lflush();

    /* ---- YCbCr → RGB565 tiled LCD display ---- */
    poc_log("--- YCbCr -> LCD (tiled, BT.601) ---");
    lflush();
    {
        fb_data *tile = (fb_data *)(void *)work_buf1;
        int ty, py, px;

        rb->lcd_clear_display();

        for (ty = 0; ty < TEST_HEIGHT; ty += 16) {
            int rows = (ty + 16 <= TEST_HEIGHT) ? 16 : TEST_HEIGHT - ty;
            for (py = 0; py < rows; py++) {
                for (px = 0; px < TEST_WIDTH; px++) {
                    uint8_t y_val  = frame_y[(ty + py) * TEST_WIDTH + px];
                    uint8_t cb_val = frame_cb[((ty + py) / 2) * (TEST_WIDTH / 2)
                                              + px / 2];
                    uint8_t cr_val = frame_cr[((ty + py) / 2) * (TEST_WIDTH / 2)
                                              + px / 2];
                    int r = y_val + (((int)cr_val - 128) * 359 >> 8);
                    int g = y_val - (((int)cb_val - 128) * 88 >> 8)
                                  - (((int)cr_val - 128) * 183 >> 8);
                    int b = y_val + (((int)cb_val - 128) * 454 >> 8);
                    if (r < 0) r = 0;
                    if (r > 255) r = 255;
                    if (g < 0) g = 0;
                    if (g > 255) g = 255;
                    if (b < 0) b = 0;
                    if (b > 255) b = 255;
                    tile[py * TEST_WIDTH + px] =
                        ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
                }
            }
            rb->lcd_bitmap(tile, 0, ty, TEST_WIDTH, rows);
        }
        rb->lcd_update();

        poc_log("  LCD updated, 5s timeout...");
        lflush();
        {
            int btn_wait = 50;
            while (--btn_wait > 0) {
                if (rb->button_get(false) != BUTTON_NONE) break;
                rb->sleep(HZ/10);
            }
        }
    }

    } /* end common phys-addr scope */

    /* ---- Cleanup ---- */
    vdec_power_off();
    poc_log("=== v36b done ===");
    lflush();
    if (log_fd >= 0) rb->close(log_fd);

    rb->splashf(HZ*3, "v36b done");
    return PLUGIN_OK;
}
