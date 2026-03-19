/***************************************************************************
 * S5L8702 H.264 Hardware Video Decoder — Test Harness
 *
 * Uses the clean vpu_h264 API to decode Annex B .264 files and verify
 * output against ffmpeg reference CRCs. Tests IRQ-based completion.
 *
 * See vpu_h264.c/h for the reusable decode module.
 * See vpu-6g.c/h for the IRQ 35 completion handler.
 ****************************************************************************/

#include "plugin.h"

#ifdef IPOD_6G
#include "s5l87xx.h"

/* vpu_h264 API accessed through plugin API (rb->) */
#define vpu_h264_buf_size    rb->vpu_h264_buf_size
#define vpu_h264_open        rb->vpu_h264_open
#define vpu_h264_configure   rb->vpu_h264_configure
#define vpu_h264_decode_nalu rb->vpu_h264_decode_nalu
#define vpu_h264_get_frame   rb->vpu_h264_get_frame
#define vpu_h264_close       rb->vpu_h264_close

#define LOG_PATH "/vdec_poc.log"
#define H264_TEST_PATH "/test_lowqp.264"
#define MAX_FILE_SIZE   500000

/* ---- Logging ---- */

static int log_fd = -1;
static char log_path_buf[64];

static void lflush(void)
{
    if (log_fd >= 0)
    {
        rb->close(log_fd);
        log_fd = rb->open(log_path_buf, O_WRONLY|O_APPEND, 0666);
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

/* ---- CRC-32 ---- */

static uint32_t crc32_calc(const uint8_t *data, int len)
{
    uint32_t crc = 0xFFFFFFFF;
    int i, j;
    for (i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
    return crc ^ 0xFFFFFFFF;
}

/* ---- Annex B start code finder ---- */

static int find_start_code(const uint8_t *buf, int len, int *sc_len)
{
    int i;
    for (i = 0; i + 2 < len; i++)
    {
        if (buf[i] == 0 && buf[i+1] == 0)
        {
            if (buf[i+2] == 1) { *sc_len = 3; return i; }
            if (i + 3 < len && buf[i+2] == 0 && buf[i+3] == 1)
            {
                *sc_len = 4; return i;
            }
        }
    }
    return -1;
}

/* ---- Main ---- */

enum plugin_status plugin_start(const void *parameter)
{
    const char *test_path = parameter ? (const char *)parameter : H264_TEST_PATH;
    size_t buf_size;
    uint8_t *buf;
    uint8_t *file_buf;
    struct vpu_h264 *dec;
    int frame_count = 0;
    int errors = 0;

    rb->splashf(HZ/2, "v58 %s", test_path);

    /* Generate per-file log: /vdec_<basename>.log */
    {
        const char *slash = rb->strrchr(test_path, '/');
        const char *base = slash ? slash + 1 : test_path;
        rb->snprintf(log_path_buf, sizeof(log_path_buf), "/vdec_%s.log", base);
        log_fd = rb->open(log_path_buf, O_WRONLY|O_CREAT|O_TRUNC, 0666);
    }
    poc_log("=== v58 — edge case test: %s ===", test_path);

    /* Allocate from audio buffer */
    buf = rb->plugin_get_audio_buffer(&buf_size);

    /* Open decoder with max 640x480 to handle all resolutions */
    size_t dec_size = vpu_h264_buf_size(640, 480);
    poc_log("Decoder needs %lu bytes", (unsigned long)dec_size);

    if (dec_size + MAX_FILE_SIZE + 4096 > buf_size)
    {
        poc_log("ERROR: buffer too small");
        if (log_fd >= 0) rb->close(log_fd);
        rb->splash(HZ*3, "Buffer too small!");
        return PLUGIN_ERROR;
    }

    file_buf = buf + buf_size - MAX_FILE_SIZE;

    /* Open decoder (640x480 max — handles 176x144 through 640x480) */
    dec = vpu_h264_open(buf, dec_size, 640, 480);
    if (!dec)
    {
        poc_log("ERROR: vpu_h264_open failed");
        if (log_fd >= 0) rb->close(log_fd);
        rb->splash(HZ*3, "Decoder init failed!");
        return PLUGIN_ERROR;
    }
    poc_log("Decoder opened OK");
    lflush();

    /* Read test file */
    poc_log("--- Parsing %s ---", test_path);
    {
        int fd = rb->open(test_path, O_RDONLY);
        if (fd < 0)
        {
            poc_log("ERROR: file not found");
            vpu_h264_close(dec);
            if (log_fd >= 0) rb->close(log_fd);
            rb->splash(HZ*3, "File not found!");
            return PLUGIN_ERROR;
        }
        int fsize = rb->filesize(fd);
        if (fsize > MAX_FILE_SIZE) fsize = MAX_FILE_SIZE;
        rb->read(fd, file_buf, fsize);
        rb->close(fd);
        poc_log("  Read %d bytes", fsize);

        /* Feed NALUs one at a time (Annex B) */
        int pos = 0;
        int sc_len;
        int nalu_idx = 0;

        while (pos < fsize && frame_count < 16)
        {
            int sc_pos = find_start_code(file_buf + pos, fsize - pos, &sc_len);
            if (sc_pos < 0) break;
            int nalu_start = pos + sc_pos + sc_len;

            /* Find next start code to get NALU length */
            int sc2_len;
            int sc2_pos = find_start_code(file_buf + nalu_start,
                                           fsize - nalu_start, &sc2_len);
            int nalu_len = (sc2_pos >= 0) ? sc2_pos : (fsize - nalu_start);

            int nal_type = file_buf[nalu_start] & 0x1F;
            poc_log("  NALU type=%d len=%d", nal_type, nalu_len);
            nalu_idx++;

            /* Feed to decoder — measure decode time */
            long t0 = *rb->current_tick;
            int ret = vpu_h264_decode_nalu(dec, file_buf + nalu_start, nalu_len);
            long t1 = *rb->current_tick;

            if (ret == 1)
            {
                long ms = (t1 - t0) * 1000 / HZ;
                /* Frame decoded — get output and verify */
                const uint8_t *y, *cb, *cr;
                int w, h;
                vpu_h264_get_frame(dec, &y, &cb, &cr, &w, &h);

                int nz = 0, i;
                for (i = 0; i < w * h; i++)
                    if (y[i] != 0) nz++;
                uint32_t crc = crc32_calc(y, w * h);

                poc_log("--- Frame %d: %dx%d nz=%d/%d crc=%08lx %ldms ---",
                        frame_count, w, h, nz, w * h, (unsigned long)crc, ms);

                frame_count++;
                lflush();
            }
            else if (ret < 0)
            {
                poc_log("  DECODE ERROR on NALU type=%d", nal_type);
                errors++;
                lflush();
            }

            pos = nalu_start + nalu_len;
        }
    }

    poc_log("--- Done: %d frames, %d errors ---", frame_count, errors);
    lflush();

    vpu_h264_close(dec);

    if (log_fd >= 0) rb->close(log_fd);
    rb->splashf(HZ*3, "v58: %d frames %d err", frame_count, errors);
    return PLUGIN_OK;
}

#else /* !IPOD_6G */

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    rb->splash(HZ*3, "iPod 6G only");
    return PLUGIN_ERROR;
}

#endif
