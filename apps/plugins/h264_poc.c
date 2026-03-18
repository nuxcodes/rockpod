/***************************************************************************
 * S5L8702 H.264 Hardware Video Decoder — v38
 *
 * TRUE HARDWARE H.264 DECODE via VPU-B (0x39800000).
 * Apple's real H.264 decoder is at a SEPARATE hardware block from the
 * JPEG/MPEG-4 VPU-A (0x39600000). Previous versions (v33-v36) accidentally
 * used the JPEG IDCT engine. This version feeds raw H.264 bitstream to
 * VPU-B. The HW does CAVLC, dequant, IDCT, intra pred, AND deblocking.
 *
 * Discovery: FUN_0007e9e0 = JPEG decoder (not H.264)
 *            FUN_0001b388 = bswap32 (not forward DCT)
 *            FUN_001c06ac = H.264 HW trigger via VPU-B
 *
 * See jpeg_poc.c for the VPU-A JPEG IDCT engine (album art decoding).
 ****************************************************************************/

#include "plugin.h"
#include "s5l87xx.h"

#define LOG_PATH "/vdec_poc.log"
#define H264_TEST_PATH "/test_iframe.264"
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
                                int nal_type, slice_hdr_t *sh)
{
    b->bit_offset = 8;
    sh->first_mb_in_slice = bs_ue(b);
    sh->slice_type = bs_ue(b);
    if (sh->slice_type >= 5) sh->slice_type -= 5;
    bs_ue(b);
    bs_un(b, sps->log2_max_frame_num_minus4 + 4);

    if (nal_type == 5) {
        bs_ue(b);
    }
    if (sps->pic_order_cnt_type == 0) {
        bs_un(b, sps->log2_max_pic_order_cnt_lsb_minus4 + 4);
    }

    if (nal_type == 5) {
        bs_u1(b); bs_u1(b);
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

static void vpub_power_on(void)
{
    uint32_t cg, pw;

    /* Enable video subsystem clocks (same as VPU-A but bit 17 for VPU-B) */
    cg = REG32(CLK_BASE + 0x08);
    cg &= ~0x80000000;
    cg |= 0x30000000;
    REG32(CLK_BASE + 0x08) = cg;
    rb->sleep(HZ/5);

    /* Clear PWRCON bits: 14-16 (video subsystem) + 17 (VPU-B) */
    pw = PWRCON(0);
    PWRCON(0) = pw & ~((7 << 14) | (1 << 17));
    rb->sleep(HZ/5);

    /* Set H.264 mode (bit 0 = 1) */
    VPU_MODE_REG |= 1;
    rb->sleep(HZ/100);
}

static void vpub_power_off(void)
{
    VPU_MODE_REG &= ~1;
    PWRCON(0) |= (1 << 17);
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
    /* Word 0: verified from ARM disasm at 0x1c0840-0x1c0868 */
    desc[0] = ((slice_index & 0x7FF) << 21)
            | ((first_mb_in_slice & 0x7FF) << 10)
            | ((slice_type & 0xF) << 6)
            | (slice_qp_y & 0x3F);

    /* Word 1: verified from ARM disasm at 0x1c086c-0x1c08ac */
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

static int vpub_decode(uint32_t ctrl_phys, uint32_t desc_phys,
                        uint32_t y_phys, uint32_t cb_phys, uint32_t cr_phys,
                        int width_mbs, int height_mbs,
                        int pic_w, int num_slices)
{
    uint32_t val;
    int timeout;

    /* Attempt software reset: toggle config, clear control */
    VPU_CONFIG = 0;
    VPU_CTRL = 0;
    rb->sleep(HZ/100);

    /* Clear reference picture area (I-frame: no refs) */
    {
        volatile uint32_t *base = (volatile uint32_t *)VPU_B_BASE;
        int i;
        for (i = 0; i < 48; i++)
            base[i] = 0;
    }

    /* Buffer addresses */
    VPU_CTRL_BUF  = ctrl_phys;
    VPU_SLICE_DESC = desc_phys;

    /* Frame dimensions (read-modify-write, two writes like Apple) */
    val = VPU_DIMS;
    val = (val & ~0x3F00) | ((height_mbs & 0x3F) << 8);
    VPU_DIMS = val;
    val = VPU_DIMS;
    val = (val & ~0x003F) | (width_mbs & 0x3F);
    VPU_DIMS = val;

    /* Frame size in control register (before strides, like Apple) */
    val = VPU_CTRL;
    val = (val & ~0x0FFE) | ((width_mbs * height_mbs * 2) & 0xFFE);
    VPU_CTRL = val;

    /* Strides */
    val = VPU_STRIDES;
    val = (val & ~0x01FF0000) | (((pic_w / 2) & 0x1FF) << 16);
    VPU_STRIDES = val;
    val = VPU_STRIDES;
    val = (val & ~0x000003FF) | (pic_w & 0x3FF);
    VPU_STRIDES = val;

    /* Output frame addresses */
    VPU_OUT_Y  = y_phys;
    VPU_OUT_CB = cb_phys;
    VPU_OUT_CR = cr_phys;

    /* Config */
    VPU_CONFIG = VPU_CONFIG_CONST;

    poc_log("  regs: +118=%08lx +E0=%08lx +E4=%08lx +E8=%08lx",
            (unsigned long)VPU_CONFIG, (unsigned long)VPU_DIMS,
            (unsigned long)VPU_STRIDES, (unsigned long)VPU_CTRL);
    poc_log("  bufs: +D8=%08lx +DC=%08lx +CC=%08lx +D0=%08lx +D4=%08lx",
            (unsigned long)VPU_CTRL_BUF, (unsigned long)VPU_SLICE_DESC,
            (unsigned long)VPU_OUT_Y, (unsigned long)VPU_OUT_CB,
            (unsigned long)VPU_OUT_CR);

    /* Slice count (separate RMW on +0xE8) */
    val = VPU_CTRL;
    val = (val & ~0x07FF0000) | ((num_slices & 0x7FF) << 16);
    VPU_CTRL = val;

    /* TRIGGER DECODE */
    VPU_CTRL = VPU_CTRL | VPU_TRIGGER_BITS;

    /* Poll for completion or error */
    timeout = 5000000;
    while (--timeout > 0) {
        uint32_t s1 = VPU_STATUS1;
        if ((s1 << 3) >> 21)
            break;              /* error bits [28:18] set */
        if (VPU_STATUS0 & 1)
            break;              /* completion: bit 0 set */
    }

    if (timeout <= 0)
        return -1;  /* timeout */

    val = VPU_STATUS1;
    if ((val << 3) >> 21)
        return -2;  /* HW error */

    return 0;
}

/* ---- Main ---- */

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    size_t buf_size;
    uint8_t *buf, *p;
    uint8_t *ctrl_buf, *slice_desc, *bs_dma;
    uint8_t *frame_y, *frame_cb, *frame_cr;
    uint8_t *file_buf, *nalu_buf;
    int pic_w = 320, pic_h = 240, pic_wmb = 20, pic_hmb = 15;
    int frame_y_size, frame_cb_size, frame_cr_size;
    int i;

    rb->splash(HZ/2, "v38 VPU-B H.264");

    log_fd = rb->open(LOG_PATH, O_WRONLY|O_CREAT|O_TRUNC, 0666);
    poc_log("=== v38 — H.264 HW decode via VPU-B (0x39800000) ===");

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
    frame_y  = (uint8_t *)ALIGN4K(p);  p = frame_y + frame_y_size;
    frame_cb = (uint8_t *)ALIGN32(p);  p = frame_cb + frame_cb_size;
    frame_cr = (uint8_t *)ALIGN32(p);  p = frame_cr + frame_cr_size;
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
    rb->memset(frame_y, 0, frame_y_size);
    rb->memset(frame_cb, 0x80, frame_cb_size);
    rb->memset(frame_cr, 0x80, frame_cr_size);

    poc_log("Buffers: ctrl=%08lx desc=%08lx bs=%08lx",
            (unsigned long)(uintptr_t)ctrl_buf,
            (unsigned long)(uintptr_t)slice_desc,
            (unsigned long)(uintptr_t)bs_dma);
    poc_log("  Y=%08lx Cb=%08lx Cr=%08lx",
            (unsigned long)(uintptr_t)frame_y,
            (unsigned long)(uintptr_t)frame_cb,
            (unsigned long)(uintptr_t)frame_cr);
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
        int have_sps = 0, have_pps = 0, decoded = 0;
        int pos = 0, sc_len;

        rb->memset(&sps, 0, sizeof(sps));
        rb->memset(&pps, 0, sizeof(pps));

        while (pos < fread_n && !decoded) {
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

            poc_log("  NALU type=%d ref=%d len=%d",
                    nal_type, (nal_hdr >> 5) & 3, rbsp_len);

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
                poc_log("  SPS: %dx%d (%dx%d MBs) profile=%d level=%d",
                        pic_w, pic_h, pic_wmb, pic_hmb,
                        sps.profile_idc, sps.level_idc);
                have_sps = 1;

            } else if (nal_type == 8) {
                parse_pps(&pps, &bs);
                poc_log("  PPS: qp=%d cqp_off=%d deblk=%d wpred=%d",
                        26 + pps.pic_init_qp_minus26,
                        pps.chroma_qp_index_offset,
                        pps.deblocking_filter_control_present_flag,
                        pps.weighted_pred_flag);
                have_pps = 1;

            } else if ((nal_type == 5 || nal_type == 1) &&
                       have_sps && have_pps) {
                poc_log("  %s slice: decoding %dx%d...",
                        nal_type == 5 ? "IDR" : "non-IDR", pic_w, pic_h);
                lflush();

                parse_slice_header(&sps, &pps, &bs, nal_type, &sh);

                int slice_qp = 26 + pps.pic_init_qp_minus26
                             + sh.slice_qp_delta;
                poc_log("  slice: type=%d qp=%d deblk=%d alpha=%d beta=%d "
                        "bits=%lu",
                        sh.slice_type, slice_qp,
                        sh.disable_deblocking_filter_idc,
                        sh.alpha_c0_offset_div2, sh.beta_offset_div2,
                        sh.bits_consumed);

                /* Compact RBSP: strip slice header, keep MB data.
                 * bits_consumed includes NAL header (8 bits) + slice header.
                 * hdr_bytes = byte offset into RBSP where MB data begins.
                 * Apple strips EBSP emulation prevention bytes BEFORE DMA.
                 * nalu_buf already has RBSP (0x000003 removed). */
                int hdr_bytes = sh.bits_consumed / 8;
                int bit_off = sh.bits_consumed & 7;
                int dma_len = rbsp_len - hdr_bytes;

                if (dma_len <= 0 || dma_len > BS_DMA_SIZE) {
                    poc_log("  ERROR: bad RBSP len %d", dma_len);
                    break;
                }

                poc_log("  DMA: %d RBSP bytes from hdr_bytes=%d, bit_off=%d",
                        dma_len, hdr_bytes, bit_off);

                /* Copy RBSP macroblock data (emulation prevention stripped) */
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
                    0, /* num_ref_l0 = 0 for I-frame */
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

                poc_log("  dims: wmb=%d hmb=%d, cur=(h<<8)|w=%04lx, alt=(w<<8)|h=%04lx",
                        pic_wmb, pic_hmb,
                        (unsigned long)((pic_hmb << 8) | pic_wmb),
                        (unsigned long)((pic_wmb << 8) | pic_hmb));

                rb->memset(frame_y, 0, frame_y_size);
                rb->memset(frame_cb, 0x80, frame_cb_size);
                rb->memset(frame_cr, 0x80, frame_cr_size);
                rb->commit_dcache();

                poc_log("  Triggering VPU-B decode...");
                lflush();

                int ret = vpub_decode(
                    PHYS(ctrl_buf), PHYS(slice_desc),
                    PHYS(frame_y), PHYS(frame_cb), PHYS(frame_cr),
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
                    int nz = 0;
                    for (i = 0; i < frame_y_size; i++)
                        if (frame_y[i] != 0) nz++;
                    poc_log("  frame_y: %d/%d non-zero", nz, frame_y_size);
                    poc_log("  row0: %08lx %08lx %08lx %08lx",
                            (unsigned long)*(uint32_t *)(frame_y),
                            (unsigned long)*(uint32_t *)(frame_y + 4),
                            (unsigned long)*(uint32_t *)(frame_y + 8),
                            (unsigned long)*(uint32_t *)(frame_y + 12));
                }

                /* CRC */
                {
                    uint32_t crc = 0xFFFFFFFF;
                    for (i = 0; i < frame_y_size; i++) {
                        crc ^= frame_y[i];
                        crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
                    }
                    poc_log("  Y crc32=%08lx",
                            (unsigned long)(crc ^ 0xFFFFFFFF));
                }

                /* Dump */
                {
                    int dfd = rb->open("/vdec_framey.bin",
                                        O_WRONLY|O_CREAT|O_TRUNC, 0666);
                    if (dfd >= 0) {
                        rb->write(dfd, frame_y, frame_y_size);
                        rb->close(dfd);
                        poc_log("  frame_y dumped (%d bytes)", frame_y_size);
                    }
                }
                lflush();

                /* LCD display */
                if (ret == 0 || 1) { /* display even on error for debugging */
                    poc_log("--- YCbCr -> LCD ---");
                    {
                        fb_data *tile = (fb_data *)(void *)nalu_buf;
                        int ty, py, px;

                        rb->lcd_clear_display();
                        for (ty = 0; ty < pic_h; ty += 16) {
                            int rows = MIN(16, pic_h - ty);
                            for (py = 0; py < rows; py++) {
                                for (px = 0; px < pic_w; px++) {
                                    uint8_t yv = frame_y[(ty+py)*pic_w+px];
                                    uint8_t cbv = frame_cb[((ty+py)/2)*(pic_w/2)+px/2];
                                    uint8_t crv = frame_cr[((ty+py)/2)*(pic_w/2)+px/2];
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
                                    tile[py*pic_w+px] =
                                        ((r>>3)<<11)|((g>>2)<<5)|(bv>>3);
                                }
                            }
                            rb->lcd_bitmap(tile, 0, ty, pic_w, rows);
                        }
                        rb->lcd_update();

                        poc_log("  LCD updated, 5s...");
                        lflush();
                        {
                            int btn = 50;
                            while (--btn > 0) {
                                if (rb->button_get(false) != BUTTON_NONE)
                                    break;
                                rb->sleep(HZ/10);
                            }
                        }
                    }
                }

                decoded = 1;
            }

            pos = nalu_start + nalu_len;
        }

        if (!decoded)
            poc_log("  No slice found to decode!");
    }

    vpub_power_off();
    poc_log("=== v38 done ===");
    lflush();
    if (log_fd >= 0) rb->close(log_fd);
    rb->splashf(HZ*3, "v38 done");
    return PLUGIN_OK;
}
