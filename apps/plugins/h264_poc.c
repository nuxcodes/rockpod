/***************************************************************************
 * S5L8702 H.264 Hardware Video Decoder — v42f
 *
 * P-FRAME SUPPORT: Decodes I+P frame sequences via VPU-B (0x39800000).
 *
 * v42f fix: Apple cycles VPU-B clock gate between every frame (disable
 * after each decode, re-enable before next). Apple also calls vtable+0x48
 * before every decode (unresolvable — vtable is in runtime RAM). We
 * substitute with VPU_MODE pulse + register zeroing.
 *
 * VPU-B reference registers (from RE of FUN_001c06ac):
 *   +0x00..+0x0B  = L0 ref[0] Y/Cb/Cr addresses
 *   +0x120..+0x12C = primary ref Y/Cb/Cr + valid flag (P/B frames)
 *
 * See jpeg_poc.c for the VPU-A JPEG IDCT engine (album art decoding).
 ****************************************************************************/

#include "plugin.h"
#include "s5l87xx.h"

#define LOG_PATH "/vdec_poc.log"
#define H264_TEST_PATH "/test_ip.264"
#define FRAME_DUMP_PATH "/vdec_framey_%d.bin"
#define REG32(addr) (*(volatile uint32_t *)(addr))

/* ---- VPU-B H.264 hardware registers (0x39800000) ---- */
#define VPU_B_BASE      0x39800000
#define VPU_B(off)      (*(volatile uint32_t *)(VPU_B_BASE + (off)))

#define VPU_OUT_Y       VPU_B(0xCC)
#define VPU_OUT_CB      VPU_B(0xD0)
#define VPU_OUT_CR      VPU_B(0xD4)
#define VPU_CTRL_BUF    VPU_B(0xD8)
#define VPU_SLICE_DESC  VPU_B(0xDC)
#define VPU_DIMS        VPU_B(0xE0)
#define VPU_STRIDES     VPU_B(0xE4)
#define VPU_CTRL        VPU_B(0xE8)
#define VPU_STATUS0     VPU_B(0xF0)
#define VPU_STATUS1     VPU_B(0xF4)
#define VPU_CONFIG      VPU_B(0x118)
#define VPU_REF_Y       VPU_B(0x120)
#define VPU_REF_CB      VPU_B(0x124)
#define VPU_REF_CR      VPU_B(0x128)
#define VPU_REF_FLAG    VPU_B(0x12C)

/* Constants from Apple firmware (verified from Ghidra) */
#define VPU_CONFIG_CONST    0x82625A00
#define VPU_SLICE_CONST     0x00110C85
#define VPU_TRIGGER_BITS    0x88003001

/* Clock gate */
#define VPU_MODE_REG    (*(volatile uint32_t *)0x38100314)

#define ALIGN32(x)  (((uintptr_t)(x) + 31) & ~31)
#define ALIGN4K(x)  (((uintptr_t)(x) + 0xFFF) & ~0xFFF)
#define PHYS(x)     ((uint32_t)((uintptr_t)(x) & 0x7FFFFFFF))

#define MAX_FILE_SIZE   500000
#define CTRL_BUF_SIZE   4096
#define SLICE_DESC_SIZE 320
#define BS_DMA_SIZE     131072
#define MAX_FRAMES      16

#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#endif

/* ---- Logging ---- */
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

/* ---- CRC-32 (standard, 8 bits per byte) ---- */

static uint32_t crc32_calc(const uint8_t *data, int len)
{
    uint32_t crc = 0xFFFFFFFF;
    int i, j;
    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
    return crc ^ 0xFFFFFFFF;
}

/* ---- Bitstream reader (exp-Golomb) ---- */

typedef struct {
    const uint8_t *buf;
    unsigned long bit_offset;
    unsigned long bit_length;
} bs_t;

static uint32_t bs_un(bs_t *b, int n)
{
    uint32_t val = 0;
    int i;
    for (i = 0; i < n; i++) {
        unsigned long byte_pos = b->bit_offset >> 3;
        unsigned int bit_pos = 7 - (b->bit_offset & 7);
        val = (val << 1) | ((b->buf[byte_pos] >> bit_pos) & 1);
        b->bit_offset++;
    }
    return val;
}

static uint32_t bs_u1(bs_t *b) { return bs_un(b, 1); }
static uint32_t bs_u8(bs_t *b) { return bs_un(b, 8); }

static uint32_t bs_ue(bs_t *b)
{
    int lz = 0;
    while (bs_u1(b) == 0 && lz < 31) lz++;
    if (lz == 0) return 0;
    return (1 << lz) - 1 + bs_un(b, lz);
}

static int bs_se(bs_t *b)
{
    uint32_t v = bs_ue(b);
    return (v & 1) ? (int)((v + 1) >> 1) : -(int)(v >> 1);
}

/* ---- Annex B parsing ---- */

static int find_start_code(const uint8_t *buf, int len, int *sc_len)
{
    int i;
    for (i = 0; i + 2 < len; i++) {
        if (buf[i] == 0 && buf[i+1] == 0) {
            if (buf[i+2] == 1) { *sc_len = 3; return i; }
            if (i + 3 < len && buf[i+2] == 0 && buf[i+3] == 1) {
                *sc_len = 4; return i;
            }
        }
    }
    return -1;
}

static int ebsp_to_rbsp(uint8_t *dst, const uint8_t *src, int src_len)
{
    int di = 0, si;
    for (si = 0; si < src_len; si++) {
        if (si + 2 < src_len && src[si] == 0 && src[si+1] == 0 &&
            src[si+2] == 3) {
            dst[di++] = 0; dst[di++] = 0;
            si += 2;
        } else {
            dst[di++] = src[si];
        }
    }
    return di;
}

/* ---- H.264 header parsers ---- */

typedef struct {
    int profile_idc, level_idc;
    int pic_width_in_mbs_minus1, pic_height_in_map_units_minus1;
    int log2_max_frame_num_minus4;
    int pic_order_cnt_type, log2_max_pic_order_cnt_lsb_minus4;
    int max_num_ref_frames;
} sps_t;

typedef struct {
    int pic_init_qp_minus26, chroma_qp_index_offset;
    int deblocking_filter_control_present_flag, weighted_pred_flag;
    int num_ref_idx_l0_default_active_minus1;
} pps_t;

typedef struct {
    int first_mb_in_slice, slice_type, slice_qp_delta;
    int disable_deblocking_filter_idc;
    int alpha_c0_offset_div2, beta_offset_div2;
    int frame_num;
    int num_ref_idx_l0_active_minus1;
    unsigned long bits_consumed;
} slice_hdr_t;

static void parse_sps(sps_t *sps, bs_t *b)
{
    b->bit_offset = 8;
    sps->profile_idc = bs_u8(b);
    bs_un(b, 8);
    sps->level_idc = bs_u8(b);
    bs_ue(b);

    if (sps->profile_idc == 100 || sps->profile_idc == 110 ||
        sps->profile_idc == 122 || sps->profile_idc == 244) {
        int cfi = bs_ue(b);
        if (cfi == 3) bs_u1(b);
        bs_ue(b); bs_ue(b); bs_u1(b);
        if (bs_u1(b)) {
            int cnt = (cfi != 3) ? 8 : 12, j;
            for (j = 0; j < cnt; j++)
                if (bs_u1(b)) {
                    int sz = (j < 6) ? 16 : 64, k;
                    int last = 8, next = 0;
                    for (k = 0; k < sz; k++) {
                        if (next != 0) next = (last + bs_se(b) + 256) % 256;
                        last = (next == 0) ? last : next;
                    }
                }
        }
    }

    sps->log2_max_frame_num_minus4 = bs_ue(b);
    sps->pic_order_cnt_type = bs_ue(b);
    if (sps->pic_order_cnt_type == 0) {
        sps->log2_max_pic_order_cnt_lsb_minus4 = bs_ue(b);
    } else if (sps->pic_order_cnt_type == 1) {
        bs_u1(b); bs_se(b); bs_se(b);
        int nrf = bs_ue(b);
        int j;
        for (j = 0; j < nrf; j++) bs_se(b);
    }
    sps->max_num_ref_frames = bs_ue(b);
    bs_u1(b);
    sps->pic_width_in_mbs_minus1 = bs_ue(b);
    sps->pic_height_in_map_units_minus1 = bs_ue(b);
}

static void parse_pps(pps_t *pps, bs_t *b)
{
    b->bit_offset = 8;
    bs_ue(b); bs_ue(b);
    bs_u1(b); /* entropy_coding_mode */
    bs_u1(b); /* bottom_field */
    int nsg = bs_ue(b);
    (void)nsg;
    pps->num_ref_idx_l0_default_active_minus1 = bs_ue(b);
    bs_ue(b);
    pps->weighted_pred_flag = bs_u1(b);
    bs_un(b, 2);
    pps->pic_init_qp_minus26 = bs_se(b);
    bs_se(b);
    pps->chroma_qp_index_offset = bs_se(b);
    pps->deblocking_filter_control_present_flag = bs_u1(b);
}

static void parse_slice_header(sps_t *sps, pps_t *pps, bs_t *b,
                                int nal_type, int nal_ref_idc,
                                slice_hdr_t *sh)
{
    b->bit_offset = 8;
    sh->first_mb_in_slice = bs_ue(b);
    sh->slice_type = bs_ue(b);
    if (sh->slice_type >= 5) sh->slice_type -= 5;
    bs_ue(b);  /* pic_parameter_set_id */
    sh->frame_num = bs_un(b, sps->log2_max_frame_num_minus4 + 4);

    if (nal_type == 5) {
        bs_ue(b);  /* idr_pic_id */
    }
    if (sps->pic_order_cnt_type == 0) {
        bs_un(b, sps->log2_max_pic_order_cnt_lsb_minus4 + 4);
    }

    /* num_ref_idx_active_override (P/B slices only) */
    sh->num_ref_idx_l0_active_minus1 = pps->num_ref_idx_l0_default_active_minus1;
    if (sh->slice_type == 0 || sh->slice_type == 1) {
        /* P-slice (0) or B-slice (1) */
        if (bs_u1(b)) {
            sh->num_ref_idx_l0_active_minus1 = bs_ue(b);
            if (sh->slice_type == 1)
                bs_ue(b);  /* num_ref_idx_l1_active_minus1 */
        }
    }

    /* ref_pic_list_modification (P/B slices only) */
    if (sh->slice_type == 0 || sh->slice_type == 1) {
        /* ref_pic_list_modification_flag_l0 */
        if (bs_u1(b)) {
            uint32_t idc;
            do {
                idc = bs_ue(b);
                if (idc == 0 || idc == 1)
                    bs_ue(b);  /* abs_diff_pic_num_minus1 */
                else if (idc == 2)
                    bs_ue(b);  /* long_term_pic_num */
            } while (idc != 3);
        }
        if (sh->slice_type == 1) {
            /* ref_pic_list_modification_flag_l1 */
            if (bs_u1(b)) {
                uint32_t idc;
                do {
                    idc = bs_ue(b);
                    if (idc == 0 || idc == 1)
                        bs_ue(b);
                    else if (idc == 2)
                        bs_ue(b);
                } while (idc != 3);
            }
        }
    }

    /* dec_ref_pic_marking */
    if (nal_ref_idc != 0) {
        if (nal_type == 5) {
            bs_u1(b);  /* no_output_of_prior_pics_flag */
            bs_u1(b);  /* long_term_reference_flag */
        } else {
            /* adaptive_ref_pic_marking_mode_flag */
            if (bs_u1(b)) {
                uint32_t mmco;
                do {
                    mmco = bs_ue(b);
                    if (mmco == 1 || mmco == 3)
                        bs_ue(b);  /* difference_of_pic_nums_minus1 */
                    if (mmco == 2)
                        bs_ue(b);  /* long_term_pic_num */
                    if (mmco == 3 || mmco == 6)
                        bs_ue(b);  /* long_term_frame_idx */
                    if (mmco == 4)
                        bs_ue(b);  /* max_long_term_frame_idx_plus1 */
                } while (mmco != 0);
            }
        }
    }

    sh->slice_qp_delta = bs_se(b);
    sh->disable_deblocking_filter_idc = 0;
    sh->alpha_c0_offset_div2 = 0;
    sh->beta_offset_div2 = 0;

    if (pps->deblocking_filter_control_present_flag) {
        sh->disable_deblocking_filter_idc = bs_ue(b);
        if (sh->disable_deblocking_filter_idc != 1) {
            sh->alpha_c0_offset_div2 = bs_se(b);
            sh->beta_offset_div2 = bs_se(b);
        }
    }

    sh->bits_consumed = b->bit_offset;
}

/* ---- VPU-B power management ---- */

/* One-time initialization: enable video subsystem clocks and VPU-B power.
 * Per-frame clock gating is handled by vpub_decode(). */
static void vpub_power_on(void)
{
    uint32_t cg, pw;

    /* Force FULL power OFF: VPU mode, PWRCON, and clocks */
    VPU_MODE_REG &= ~1;
    PWRCON(0) |= (1 << 17) | (7 << 14);
    rb->sleep(HZ/2);

    /* Enable video subsystem clocks */
    cg = REG32(CLK_BASE + 0x08);
    cg &= ~0x80000000;
    cg |= 0x30000000;
    REG32(CLK_BASE + 0x08) = cg;
    rb->sleep(HZ/5);

    /* Power ON video subsystem (bits 14-16) but leave VPU-B clock OFF.
     * vpub_decode() will enable/disable bit 17 per-frame, matching Apple. */
    pw = PWRCON(0);
    PWRCON(0) = pw & ~(7 << 14);
    rb->sleep(HZ/5);

    /* Set VPU_MODE for H.264 */
    VPU_MODE_REG |= 1;
    rb->sleep(HZ/10);
}

static void vpub_power_off(void)
{
    VPU_MODE_REG &= ~1;
    PWRCON(0) |= (1 << 17) | (7 << 14);
}

/* ---- Slice descriptor builder ---- */

static void build_slice_descriptor(uint32_t *desc,
                                    int slice_index,
                                    int first_mb_in_slice,
                                    int slice_type,
                                    int slice_qp_y,
                                    int chroma_qp_off,
                                    int weighted_pred,
                                    int num_ref_l0,
                                    int deblk_idc,
                                    int alpha_off,
                                    int beta_off,
                                    int bit_offset,
                                    uint32_t bs_phys_addr,
                                    int bs_length)
{
    desc[0] = ((slice_index & 0x7FF) << 21)
            | ((first_mb_in_slice & 0x7FF) << 10)
            | ((slice_type & 0xF) << 6)
            | (slice_qp_y & 0x3F);

    desc[1] = ((chroma_qp_off & 0xFF) << 15)
            | ((weighted_pred & 1) << 14)
            | ((num_ref_l0 & 0xF) << 10)
            | ((deblk_idc & 0x3) << 8)
            | ((alpha_off & 0xF) << 4)
            | (beta_off & 0xF);

    desc[2] = VPU_SLICE_CONST;
    desc[3] = 0;
    desc[4] = bit_offset & 0xFF;
    desc[5] = bs_phys_addr;
    desc[6] = bs_length;
    desc[7] = 0;
}

/* ---- VPU-B decode trigger ---- */

/* Matches Apple's FUN_001c06ac sequence:
 * 1. Enable clock (PWRCON bit 17 clear)
 * 2. VPU_MODE pulse + zero regs (substitute for vtable+0x48)
 * 3. Program registers (Apple's exact order, RMW)
 * 4. Write ref list + ref registers
 * 5. Trigger + wait
 * 6. Disable clock (PWRCON bit 17 set) */
static int vpub_decode(uint32_t ctrl_phys, uint32_t desc_phys,
                        uint32_t y_phys, uint32_t cb_phys, uint32_t cr_phys,
                        uint32_t ref_y, uint32_t ref_cb, uint32_t ref_cr,
                        int has_ref,
                        int width_mbs, int height_mbs,
                        int pic_w, int num_slices)
{
    uint32_t val;
    volatile uint32_t *base = (volatile uint32_t *)VPU_B_BASE;
    int i, ret;

    /* Step 1: Enable VPU-B clock (Apple: thunk_EXT_FUN_22000318(0x20000,0,1)) */
    PWRCON(0) = PWRCON(0) & ~(1 << 17);

    /* Step 2: VPU_MODE pulse (substitute for vtable+0x48). */
    VPU_MODE_REG &= ~1;
    VPU_MODE_REG |= 1;

    /* Dump ALL VPU-B registers BEFORE zeroing — find non-zero defaults */
    {
        int any_nz = 0;
        for (i = 0; i < 0x130/4; i++)
            if (base[i] != 0) any_nz++;
        poc_log("  post-pulse: %d non-zero regs (of %d)", any_nz, 0x130/4);
        if (any_nz > 0) {
            for (i = 0; i < 0x130/4; i++) {
                if (base[i] != 0)
                    poc_log("    +%02x = %08lx", i*4, (unsigned long)base[i]);
            }
        }
    }

    for (i = 0; i < 0x130/4; i++)
        base[i] = 0;

    poc_log("  pre: +F0=%08lx +F4=%08lx +E8=%08lx",
            (unsigned long)VPU_STATUS0, (unsigned long)VPU_STATUS1,
            (unsigned long)VPU_CTRL);

    /* Steps 3-9: Program registers (Apple's order from FUN_001c06ac asm) */
    VPU_CTRL_BUF = ctrl_phys;                              /* +0xD8 */

    val = VPU_DIMS;                                         /* +0xE0 RMW */
    val = (val & ~0x3F00) | ((height_mbs & 0x3F) << 8);
    VPU_DIMS = val;
    val = VPU_DIMS;
    val = (val & ~0x003F) | (width_mbs & 0x3F);
    VPU_DIMS = val;

    val = VPU_CTRL;                                         /* +0xE8 RMW */
    val = (val & ~0x0FFE) | ((width_mbs * height_mbs * 2) & 0xFFE);
    VPU_CTRL = val;

    VPU_SLICE_DESC = desc_phys;                             /* +0xDC */

    val = VPU_STRIDES;                                      /* +0xE4 RMW */
    val = (val & ~0x01FF0000) | (((pic_w / 2) & 0x1FF) << 16);
    VPU_STRIDES = val;
    val = VPU_STRIDES;
    val = (val & ~0x000003FF) | (pic_w & 0x3FF);
    VPU_STRIDES = val;

    VPU_OUT_Y  = y_phys;                                    /* +0xCC */
    VPU_OUT_CB = cb_phys;                                   /* +0xD0 */
    VPU_OUT_CR = cr_phys;                                   /* +0xD4 */
    VPU_CONFIG = VPU_CONFIG_CONST;                          /* +0x118 */

    /* +0x120-0x12C are B-frame L1 ref registers — NOT written for Baseline.
     * Apple's FUN_001c0fd0 clears has_ref for Baseline before calling
     * FUN_001c06ac, so the +0x120-0x12C block never executes.
     * Writing +0x12C=1 incorrectly enables B-frame mode, causing errors. */

    /* Write L0 ref list to +0x00+ (Apple: DPB iterator loop) */
    if (has_ref) {
        base[0] = ref_y;
        base[1] = ref_cb;
        base[2] = ref_cr;
        poc_log("  refs: +00=%08lx +04=%08lx +08=%08lx +12C=%d",
                (unsigned long)ref_y, (unsigned long)ref_cb,
                (unsigned long)ref_cr, has_ref);
    }

    poc_log("  regs: +E0=%08lx +E4=%08lx +E8=%08lx +CC=%08lx +D0=%08lx +D4=%08lx",
            (unsigned long)VPU_DIMS, (unsigned long)VPU_STRIDES,
            (unsigned long)VPU_CTRL,
            (unsigned long)VPU_OUT_Y, (unsigned long)VPU_OUT_CB,
            (unsigned long)VPU_OUT_CR);

    /* Step 13: Slice count */
    val = VPU_CTRL;
    val = (val & ~0x07FF0000) | ((num_slices & 0x7FF) << 16);
    VPU_CTRL = val;

    /* Step 14: TRIGGER DECODE */
    VPU_CTRL = VPU_CTRL | VPU_TRIGGER_BITS;

    /* Step 15: Wait for decode completion (Apple: IRQ semaphore, 0x84ms) */
    rb->sleep(HZ/5);  /* 200ms */

    val = VPU_STATUS1;
    poc_log("  done: +F0=%08lx +F4=%08lx",
            (unsigned long)VPU_STATUS0, (unsigned long)val);

    ret = 0;
    if ((val << 3) >> 21)
        ret = -2;  /* HW error bits [28:18] set */

    /* Step 18: Disable VPU-B clock (Apple: thunk_EXT_FUN_22000318(0x20000,0,0)) */
    PWRCON(0) = PWRCON(0) | (1 << 17);

    return ret;
}

/* ---- YCbCr to LCD display ---- */

static void display_frame(const uint8_t *y, const uint8_t *cb,
                           const uint8_t *cr, int pic_w, int pic_h,
                           uint8_t *tile_buf)
{
    fb_data *tile = (fb_data *)(void *)tile_buf;
    int ty, py, px;

    rb->lcd_clear_display();
    for (ty = 0; ty < pic_h; ty += 16) {
        int rows = MIN(16, pic_h - ty);
        for (py = 0; py < rows; py++) {
            for (px = 0; px < pic_w; px++) {
                uint8_t yv = y[(ty+py)*pic_w+px];
                uint8_t cbv = cb[((ty+py)/2)*(pic_w/2)+px/2];
                uint8_t crv = cr[((ty+py)/2)*(pic_w/2)+px/2];
                int r = yv + (((int)crv-128)*359>>8);
                int g = yv - (((int)cbv-128)*88>>8)
                          - (((int)crv-128)*183>>8);
                int bv = yv + (((int)cbv-128)*454>>8);
                if (r < 0) r = 0;
                if (r > 255) r = 255;
                if (g < 0) g = 0;
                if (g > 255) g = 255;
                if (bv < 0) bv = 0;
                if (bv > 255) bv = 255;
                tile[py*pic_w+px] = ((r>>3)<<11)|((g>>2)<<5)|(bv>>3);
            }
        }
        rb->lcd_bitmap(tile, 0, ty, pic_w, rows);
    }
    rb->lcd_update();
}

/* ---- Main ---- */

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    size_t buf_size;
    uint8_t *buf, *p;
    uint8_t *ctrl_buf, *slice_desc, *bs_dma;
    uint8_t *frame_y[2], *frame_cb[2], *frame_cr[2];
    uint8_t *file_buf, *nalu_buf;
    int pic_w = 320, pic_h = 240, pic_wmb = 20, pic_hmb = 15;
    int frame_y_size, frame_cb_size, frame_cr_size;
    int cur_buf = 0, frame_count = 0;

    rb->splash(HZ/2, "v42f VPU-B P-frame");

    log_fd = rb->open(LOG_PATH, O_WRONLY|O_CREAT|O_TRUNC, 0666);
    poc_log("=== v42f — H.264 HW I+P via VPU-B (clock gate per frame) ===");

    /* ---- Allocate buffers ---- */
    buf = rb->plugin_get_audio_buffer(&buf_size);
    poc_log("Audio buffer: %08lx size=%lu",
            (unsigned long)(uintptr_t)buf, (unsigned long)buf_size);

    p = (uint8_t *)ALIGN32(buf);
    ctrl_buf   = (uint8_t *)ALIGN4K(p);  p = ctrl_buf + CTRL_BUF_SIZE;
    slice_desc = (uint8_t *)ALIGN32(p);  p = slice_desc + SLICE_DESC_SIZE;
    bs_dma     = (uint8_t *)ALIGN32(p);  p = bs_dma + BS_DMA_SIZE;

    frame_y_size = 320 * 240;
    frame_cb_size = 160 * 120;
    frame_cr_size = 160 * 120;

    /* Double-buffered frame memory */
    frame_y[0]  = (uint8_t *)ALIGN4K(p);  p = frame_y[0] + frame_y_size;
    frame_cb[0] = (uint8_t *)ALIGN32(p);  p = frame_cb[0] + frame_cb_size;
    frame_cr[0] = (uint8_t *)ALIGN32(p);  p = frame_cr[0] + frame_cr_size;
    frame_y[1]  = (uint8_t *)ALIGN4K(p);  p = frame_y[1] + frame_y_size;
    frame_cb[1] = (uint8_t *)ALIGN32(p);  p = frame_cb[1] + frame_cb_size;
    frame_cr[1] = (uint8_t *)ALIGN32(p);  p = frame_cr[1] + frame_cr_size;

    file_buf = (uint8_t *)ALIGN32(p);  p = file_buf + MAX_FILE_SIZE;
    nalu_buf = (uint8_t *)ALIGN32(p);  p = nalu_buf + MAX_FILE_SIZE;

    if ((uintptr_t)(p - buf) > buf_size) {
        poc_log("ERROR: buffer too small");
        if (log_fd >= 0) rb->close(log_fd);
        rb->splash(HZ*3, "Buffer too small!");
        return PLUGIN_ERROR;
    }

    rb->memset(ctrl_buf, 0, CTRL_BUF_SIZE);
    rb->memset(slice_desc, 0, SLICE_DESC_SIZE);
    rb->memset(bs_dma, 0, BS_DMA_SIZE);
    rb->memset(frame_y[0], 0, frame_y_size);
    rb->memset(frame_cb[0], 0x80, frame_cb_size);
    rb->memset(frame_cr[0], 0x80, frame_cr_size);
    rb->memset(frame_y[1], 0, frame_y_size);
    rb->memset(frame_cb[1], 0x80, frame_cb_size);
    rb->memset(frame_cr[1], 0x80, frame_cr_size);

    poc_log("Buffers: ctrl=%08lx desc=%08lx bs=%08lx",
            (unsigned long)(uintptr_t)ctrl_buf,
            (unsigned long)(uintptr_t)slice_desc,
            (unsigned long)(uintptr_t)bs_dma);
    poc_log("  Y[0]=%08lx Cb[0]=%08lx Cr[0]=%08lx",
            (unsigned long)(uintptr_t)frame_y[0],
            (unsigned long)(uintptr_t)frame_cb[0],
            (unsigned long)(uintptr_t)frame_cr[0]);
    poc_log("  Y[1]=%08lx Cb[1]=%08lx Cr[1]=%08lx",
            (unsigned long)(uintptr_t)frame_y[1],
            (unsigned long)(uintptr_t)frame_cb[1],
            (unsigned long)(uintptr_t)frame_cr[1]);
    lflush();

    /* ---- Power on VPU-B ---- */
    poc_log("--- Power on VPU-B ---");
    poc_log("  PWRCON0 before: %08lx", (unsigned long)PWRCON(0));
    poc_log("  VPU_MODE before: %08lx", (unsigned long)VPU_MODE_REG);

    vpub_power_on();

    poc_log("  PWRCON0 after: %08lx", (unsigned long)PWRCON(0));
    poc_log("  VPU_MODE after: %08lx", (unsigned long)VPU_MODE_REG);
    poc_log("  VPU_B status: +F0=%08lx +F4=%08lx +E8=%08lx +118=%08lx",
            (unsigned long)VPU_STATUS0,
            (unsigned long)VPU_STATUS1,
            (unsigned long)VPU_CTRL,
            (unsigned long)VPU_CONFIG);
    lflush();

    /* ---- Parse H.264 file ---- */
    poc_log("--- Parsing %s ---", H264_TEST_PATH);
    {
        int fd = rb->open(H264_TEST_PATH, O_RDONLY);
        if (fd < 0) {
            poc_log("  ERROR: file not found");
            vpub_power_off();
            if (log_fd >= 0) rb->close(log_fd);
            rb->splash(HZ*3, "File not found!");
            return PLUGIN_ERROR;
        }
        off_t fsize = rb->filesize(fd);
        if (fsize > MAX_FILE_SIZE) fsize = MAX_FILE_SIZE;
        int fread_n = rb->read(fd, file_buf, fsize);
        rb->close(fd);
        poc_log("  Read %d bytes", fread_n);

        sps_t sps;
        pps_t pps;
        slice_hdr_t sh;
        bs_t bs;
        int have_sps = 0, have_pps = 0;
        int pos = 0, sc_len;

        rb->memset(&sps, 0, sizeof(sps));
        rb->memset(&pps, 0, sizeof(pps));

        while (pos < fread_n && frame_count < MAX_FRAMES) {
            int sc_pos = find_start_code(file_buf + pos, fread_n - pos,
                                          &sc_len);
            if (sc_pos < 0) break;
            int nalu_start = pos + sc_pos + sc_len;

            int sc2_len;
            int sc2_pos = find_start_code(file_buf + nalu_start,
                                           fread_n - nalu_start, &sc2_len);
            int nalu_len = (sc2_pos >= 0) ? sc2_pos
                                          : (fread_n - nalu_start);

            int rbsp_len = ebsp_to_rbsp(nalu_buf, file_buf + nalu_start,
                                         nalu_len);
            uint8_t nal_hdr = nalu_buf[0];
            int nal_type = nal_hdr & 0x1F;
            int nal_ref_idc = (nal_hdr >> 5) & 3;

            poc_log("  NALU type=%d ref=%d len=%d",
                    nal_type, nal_ref_idc, rbsp_len);

            bs.buf = nalu_buf;
            bs.bit_offset = 0;
            bs.bit_length = rbsp_len * 8;

            if (nal_type == 7) {
                parse_sps(&sps, &bs);
                pic_wmb = sps.pic_width_in_mbs_minus1 + 1;
                pic_hmb = sps.pic_height_in_map_units_minus1 + 1;
                pic_w = pic_wmb * 16;
                pic_h = pic_hmb * 16;
                frame_y_size = pic_w * pic_h;
                frame_cb_size = (pic_w / 2) * (pic_h / 2);
                frame_cr_size = frame_cb_size;
                poc_log("  SPS: %dx%d (%dx%d MBs) profile=%d level=%d "
                        "max_ref=%d log2_fn=%d poc_type=%d",
                        pic_w, pic_h, pic_wmb, pic_hmb,
                        sps.profile_idc, sps.level_idc,
                        sps.max_num_ref_frames,
                        sps.log2_max_frame_num_minus4 + 4,
                        sps.pic_order_cnt_type);
                have_sps = 1;

            } else if (nal_type == 8) {
                parse_pps(&pps, &bs);
                poc_log("  PPS: qp=%d cqp_off=%d deblk=%d wpred=%d "
                        "l0_default=%d",
                        26 + pps.pic_init_qp_minus26,
                        pps.chroma_qp_index_offset,
                        pps.deblocking_filter_control_present_flag,
                        pps.weighted_pred_flag,
                        pps.num_ref_idx_l0_default_active_minus1);
                have_pps = 1;

            } else if ((nal_type == 5 || nal_type == 1) &&
                       have_sps && have_pps) {
                int is_idr = (nal_type == 5);
                int has_ref = !is_idr;
                int ref_buf = cur_buf ^ 1;

                poc_log("--- Frame %d: %s slice ---",
                        frame_count, is_idr ? "IDR" : "P");
                lflush();

                parse_slice_header(&sps, &pps, &bs, nal_type,
                                    nal_ref_idc, &sh);

                int slice_qp = 26 + pps.pic_init_qp_minus26
                             + sh.slice_qp_delta;
                poc_log("  hdr: type=%d qp=%d frame_num=%d "
                        "nref_l0=%d deblk=%d alpha=%d beta=%d bits=%lu",
                        sh.slice_type, slice_qp, sh.frame_num,
                        sh.num_ref_idx_l0_active_minus1,
                        sh.disable_deblocking_filter_idc,
                        sh.alpha_c0_offset_div2, sh.beta_offset_div2,
                        sh.bits_consumed);

                /* Compact RBSP: strip slice header, keep MB data */
                int hdr_bytes = sh.bits_consumed / 8;
                int bit_off = sh.bits_consumed & 7;
                int dma_len = rbsp_len - hdr_bytes;

                if (dma_len <= 0 || dma_len > BS_DMA_SIZE) {
                    poc_log("  ERROR: bad RBSP len %d", dma_len);
                    break;
                }

                poc_log("  DMA: %d RBSP bytes from hdr_bytes=%d, bit_off=%d",
                        dma_len, hdr_bytes, bit_off);

                rb->memcpy(bs_dma, nalu_buf + hdr_bytes,
                           MIN(dma_len, BS_DMA_SIZE));

                poc_log("  RBSP[0..7]: %02x %02x %02x %02x %02x %02x %02x %02x",
                        bs_dma[0], bs_dma[1], bs_dma[2], bs_dma[3],
                        bs_dma[4], bs_dma[5], bs_dma[6], bs_dma[7]);

                /* Build slice descriptor */
                build_slice_descriptor(
                    (uint32_t *)slice_desc,
                    0, sh.first_mb_in_slice, sh.slice_type, slice_qp,
                    pps.chroma_qp_index_offset, pps.weighted_pred_flag,
                    has_ref ? sh.num_ref_idx_l0_active_minus1 : 0,
                    sh.disable_deblocking_filter_idc,
                    sh.alpha_c0_offset_div2, sh.beta_offset_div2,
                    bit_off, PHYS(bs_dma), dma_len);

                {
                    uint32_t *d = (uint32_t *)slice_desc;
                    poc_log("  desc: %08lx %08lx %08lx %08lx",
                            (unsigned long)d[0], (unsigned long)d[1],
                            (unsigned long)d[2], (unsigned long)d[3]);
                    poc_log("        %08lx %08lx %08lx %08lx",
                            (unsigned long)d[4], (unsigned long)d[5],
                            (unsigned long)d[6], (unsigned long)d[7]);
                }

                /* Clear output buffers */
                rb->memset(frame_y[cur_buf], 0, frame_y_size);
                rb->memset(frame_cb[cur_buf], 0x80, frame_cb_size);
                rb->memset(frame_cr[cur_buf], 0x80, frame_cr_size);
                rb->commit_dcache();

                poc_log("  Triggering VPU-B decode...");
                lflush();

                int ret = vpub_decode(
                    PHYS(ctrl_buf), PHYS(slice_desc),
                    PHYS(frame_y[cur_buf]),
                    PHYS(frame_cb[cur_buf]),
                    PHYS(frame_cr[cur_buf]),
                    has_ref ? PHYS(frame_y[ref_buf]) : 0,
                    has_ref ? PHYS(frame_cb[ref_buf]) : 0,
                    has_ref ? PHYS(frame_cr[ref_buf]) : 0,
                    has_ref,
                    pic_wmb, pic_hmb, pic_w, 1);

                rb->commit_discard_dcache();

                if (ret == 0)
                    poc_log("  VPU-B decode SUCCESS!");
                else if (ret == -1)
                    poc_log("  VPU-B decode TIMEOUT");
                else
                    poc_log("  VPU-B decode ERROR (%d)", ret);

                poc_log("  Post: +F0=%08lx +F4=%08lx +E8=%08lx",
                        (unsigned long)VPU_STATUS0,
                        (unsigned long)VPU_STATUS1,
                        (unsigned long)VPU_CTRL);

                /* Frame analysis */
                {
                    int nz = 0, i;
                    for (i = 0; i < frame_y_size; i++)
                        if (frame_y[cur_buf][i] != 0) nz++;
                    poc_log("  frame_y: %d/%d non-zero", nz, frame_y_size);
                    poc_log("  row0: %08lx %08lx %08lx %08lx",
                            (unsigned long)*(uint32_t *)(frame_y[cur_buf]),
                            (unsigned long)*(uint32_t *)(frame_y[cur_buf]+4),
                            (unsigned long)*(uint32_t *)(frame_y[cur_buf]+8),
                            (unsigned long)*(uint32_t *)(frame_y[cur_buf]+12));
                }

                /* CRC */
                {
                    uint32_t crc = crc32_calc(frame_y[cur_buf], frame_y_size);
                    poc_log("  Y crc32=%08lx", (unsigned long)crc);
                }

                /* Dump Y plane */
                {
                    static char dump_path[40];
                    rb->snprintf(dump_path, sizeof(dump_path),
                                 FRAME_DUMP_PATH, frame_count);
                    int dfd = rb->open(dump_path,
                                        O_WRONLY|O_CREAT|O_TRUNC, 0666);
                    if (dfd >= 0) {
                        rb->write(dfd, frame_y[cur_buf], frame_y_size);
                        rb->close(dfd);
                        poc_log("  dumped %s (%d bytes)",
                                dump_path, frame_y_size);
                    }
                }
                lflush();

                /* LCD display */
                {
                    poc_log("--- Display frame %d ---", frame_count);
                    display_frame(frame_y[cur_buf], frame_cb[cur_buf],
                                  frame_cr[cur_buf], pic_w, pic_h,
                                  nalu_buf);

                    poc_log("  LCD updated, 3s...");
                    lflush();
                    {
                        int btn = 30;
                        while (--btn > 0) {
                            if (rb->button_get(false) != BUTTON_NONE)
                                break;
                            rb->sleep(HZ/10);
                        }
                    }
                }

                /* --- DIAGNOSTIC: after first IDR, re-decode same I to buf[1] --- */
                if (frame_count == 0 && is_idr && ret == 0) {
                    int diag_buf = cur_buf ^ 1;
                    poc_log("--- DIAG: I-I re-decode (same IDR to buf[%d]) ---",
                            diag_buf);

                    /* Dump VPU-B regs before clock-off (read while still on) */
                    {
                        volatile uint32_t *vb = (volatile uint32_t *)VPU_B_BASE;
                        /* Re-enable clock to read regs (vpub_decode disabled it) */
                        PWRCON(0) = PWRCON(0) & ~(1 << 17);
                        poc_log("  VPU post-IDR: +00=%08lx +04=%08lx +08=%08lx",
                                (unsigned long)vb[0], (unsigned long)vb[1],
                                (unsigned long)vb[2]);
                        poc_log("  +CC=%08lx +D0=%08lx +D4=%08lx +D8=%08lx",
                                (unsigned long)VPU_B(0xCC),
                                (unsigned long)VPU_B(0xD0),
                                (unsigned long)VPU_B(0xD4),
                                (unsigned long)VPU_B(0xD8));
                        poc_log("  +E0=%08lx +E4=%08lx +E8=%08lx +F0=%08lx +F4=%08lx",
                                (unsigned long)VPU_B(0xE0),
                                (unsigned long)VPU_B(0xE4),
                                (unsigned long)VPU_B(0xE8),
                                (unsigned long)VPU_B(0xF0),
                                (unsigned long)VPU_B(0xF4));
                        poc_log("  +118=%08lx +120=%08lx +12C=%08lx",
                                (unsigned long)VPU_B(0x118),
                                (unsigned long)VPU_B(0x120),
                                (unsigned long)VPU_B(0x12C));
                        PWRCON(0) = PWRCON(0) | (1 << 17);
                    }

                    /* Re-decode same IDR to second buffer */
                    rb->memset(frame_y[diag_buf], 0, frame_y_size);
                    rb->memset(frame_cb[diag_buf], 0x80, frame_cb_size);
                    rb->memset(frame_cr[diag_buf], 0x80, frame_cr_size);
                    rb->commit_dcache();

                    poc_log("  Triggering I-I re-decode...");
                    lflush();

                    int ret2 = vpub_decode(
                        PHYS(ctrl_buf), PHYS(slice_desc),
                        PHYS(frame_y[diag_buf]),
                        PHYS(frame_cb[diag_buf]),
                        PHYS(frame_cr[diag_buf]),
                        0, 0, 0, 0,
                        pic_wmb, pic_hmb, pic_w, 1);

                    rb->commit_discard_dcache();

                    if (ret2 == 0)
                        poc_log("  I-I re-decode SUCCESS!");
                    else
                        poc_log("  I-I re-decode ERROR (%d)", ret2);

                    poc_log("  Post: +F0=%08lx +F4=%08lx",
                            (unsigned long)VPU_STATUS0,
                            (unsigned long)VPU_STATUS1);

                    {
                        int nz = 0, ii;
                        for (ii = 0; ii < frame_y_size; ii++)
                            if (frame_y[diag_buf][ii] != 0) nz++;
                        poc_log("  diag_y: %d/%d non-zero", nz, frame_y_size);
                        uint32_t crc2 = crc32_calc(frame_y[diag_buf],
                                                    frame_y_size);
                        poc_log("  diag Y crc32=%08lx", (unsigned long)crc2);
                    }
                    /* DIAG B: Same I-frame RBSP but descriptor says type=0 (P).
                     * Tests if HW rejects P-mode setup vs bitstream content. */
                    poc_log("--- DIAG-B: I-RBSP with P-descriptor (type=0) ---");
                    {
                        uint32_t *dd = (uint32_t *)slice_desc;
                        uint32_t saved_w0 = dd[0];
                        /* Change slice_type from 2(I) to 0(P) in word 0 */
                        dd[0] = (saved_w0 & ~(0xF << 6)) | (0 << 6);
                        poc_log("  desc[0]: %08lx (was %08lx)",
                                (unsigned long)dd[0], (unsigned long)saved_w0);

                        rb->memset(frame_y[diag_buf], 0, frame_y_size);
                        rb->memset(frame_cb[diag_buf], 0x80, frame_cb_size);
                        rb->memset(frame_cr[diag_buf], 0x80, frame_cr_size);
                        rb->commit_dcache();

                        int ret3 = vpub_decode(
                            PHYS(ctrl_buf), PHYS(slice_desc),
                            PHYS(frame_y[diag_buf]),
                            PHYS(frame_cb[diag_buf]),
                            PHYS(frame_cr[diag_buf]),
                            0, 0, 0, 0,
                            pic_wmb, pic_hmb, pic_w, 1);

                        rb->commit_discard_dcache();
                        poc_log("  DIAG-B result: %d  STATUS1=%08lx",
                                ret3, (unsigned long)VPU_STATUS1);
                        {
                            int nz = 0, ii;
                            for (ii = 0; ii < frame_y_size; ii++)
                                if (frame_y[diag_buf][ii] != 0) nz++;
                            poc_log("  diag_y: %d/%d non-zero", nz, frame_y_size);
                        }

                        /* Restore descriptor for subsequent use */
                        dd[0] = saved_w0;
                        rb->commit_dcache();
                    }

                    /* DIAG C: Same I-frame RBSP, type=0, WITH ref addrs at +0x00 */
                    poc_log("--- DIAG-C: I-RBSP + P-desc + refs at +0x00 ---");
                    {
                        uint32_t *dd = (uint32_t *)slice_desc;
                        uint32_t saved_w0 = dd[0];
                        dd[0] = (saved_w0 & ~(0xF << 6)) | (0 << 6);

                        rb->memset(frame_y[diag_buf], 0, frame_y_size);
                        rb->memset(frame_cb[diag_buf], 0x80, frame_cb_size);
                        rb->memset(frame_cr[diag_buf], 0x80, frame_cr_size);
                        rb->commit_dcache();

                        int ret4 = vpub_decode(
                            PHYS(ctrl_buf), PHYS(slice_desc),
                            PHYS(frame_y[diag_buf]),
                            PHYS(frame_cb[diag_buf]),
                            PHYS(frame_cr[diag_buf]),
                            PHYS(frame_y[cur_buf]),
                            PHYS(frame_cb[cur_buf]),
                            PHYS(frame_cr[cur_buf]),
                            1,
                            pic_wmb, pic_hmb, pic_w, 1);

                        rb->commit_discard_dcache();
                        poc_log("  DIAG-C result: %d  STATUS1=%08lx",
                                ret4, (unsigned long)VPU_STATUS1);
                        {
                            int nz = 0, ii;
                            for (ii = 0; ii < frame_y_size; ii++)
                                if (frame_y[diag_buf][ii] != 0) nz++;
                            poc_log("  diag_y: %d/%d non-zero", nz, frame_y_size);
                        }

                        dd[0] = saved_w0;
                        rb->commit_dcache();
                    }
                    lflush();
                }

                /* Swap buffers: current becomes reference for next frame */
                cur_buf ^= 1;
                frame_count++;
            }

            pos = nalu_start + nalu_len;
        }

        poc_log("--- Decoded %d frames ---", frame_count);
    }

    vpub_power_off();
    poc_log("=== v42f done ===");
    lflush();
    if (log_fd >= 0) rb->close(log_fd);
    rb->splashf(HZ*3, "v42f: %d frames", frame_count);
    return PLUGIN_OK;
}
