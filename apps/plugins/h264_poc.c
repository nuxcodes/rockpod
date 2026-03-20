/***************************************************************************
 * S5L8702 H.264 Hardware Video Decoder — Test Harness
 *
 * Supports both Annex B .264 files and MP4/M4V containers.
 * For MP4: parses avcC, stsz, stco inline (no external demuxer needed).
 * Logs per-frame CRC-32 and decode time for validation against ffmpeg.
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

#define MAX_FILE_SIZE   500000
#define MAX_SAMPLES     8192
#define MAX_CHUNKS      8192
#define READ_BUF_SIZE   131072
#define MAX_FRAMES      30

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

/* ---- Big-endian helpers ---- */

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)  | p[3];
}

static uint16_t rd16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
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

/* ---- Minimal MP4 parser (inline, no external demuxer) ---- */

struct mp4_info {
    /* avcC */
    uint8_t avcc[512];
    int avcc_len;
    int nalu_len_size;
    /* Dimensions from avc1 */
    int width, height;
    /* Sample table */
    uint32_t num_samples;
    uint32_t *sample_sizes;  /* points into work buffer */
    uint32_t num_chunks;
    uint32_t *chunk_offsets;  /* points into work buffer */
};

static bool is_mp4_file(const char *path)
{
    const char *ext = rb->strrchr(path, '.');
    if (!ext) return false;
    return (rb->strcasecmp(ext, ".mp4") == 0 ||
            rb->strcasecmp(ext, ".m4v") == 0);
}

/* Scan the first few MB of an MP4 file for key atoms.
 * This is a brute-force search, not a proper box walker. */
static bool mp4_parse(int fd, struct mp4_info *info, uint8_t *scan_buf,
                       int scan_size)
{
    int n, i;

    rb->memset(info, 0, sizeof(*info));

    /* Read first chunk for metadata scanning */
    rb->lseek(fd, 0, SEEK_SET);
    n = rb->read(fd, scan_buf, scan_size);
    if (n < 256) return false;

    /* Find avcC */
    for (i = 0; i < n - 8; i++)
    {
        if (scan_buf[i] == 'a' && scan_buf[i+1] == 'v' &&
            scan_buf[i+2] == 'c' && scan_buf[i+3] == 'C')
        {
            uint32_t box_sz = rd32(scan_buf + i - 4);
            int data_len = box_sz - 8;
            if (data_len > 0 && data_len <= 512 && i + data_len <= n)
            {
                rb->memcpy(info->avcc, scan_buf + i + 4, data_len);
                info->avcc_len = data_len;
                info->nalu_len_size = (info->avcc[4] & 0x03) + 1;
            }
            break;
        }
    }

    /* Find avc1 for dimensions */
    for (i = 0; i < n - 32; i++)
    {
        if (scan_buf[i] == 'a' && scan_buf[i+1] == 'v' &&
            scan_buf[i+2] == 'c' && scan_buf[i+3] == '1')
        {
            /* avc1: +4(after fourcc) +6(reserved) +2(dri) +2+2+12 = 24,
               then width(2) + height(2) */
            int off = i + 4 + 6 + 2 + 2 + 2 + 12;
            if (off + 4 <= n)
            {
                info->width = rd16(scan_buf + off);
                info->height = rd16(scan_buf + off + 2);
            }
            break;
        }
    }

    /* Find stsz (after 'vide' handler to get video track's table) */
    {
        int vide_pos = -1;
        for (i = 0; i < n - 4; i++)
        {
            if (scan_buf[i] == 'v' && scan_buf[i+1] == 'i' &&
                scan_buf[i+2] == 'd' && scan_buf[i+3] == 'e')
            {
                vide_pos = i;
                break;
            }
        }
        if (vide_pos < 0) return false;

        for (i = vide_pos; i < n - 12; i++)
        {
            if (scan_buf[i] == 's' && scan_buf[i+1] == 't' &&
                scan_buf[i+2] == 's' && scan_buf[i+3] == 'z')
            {
                /* stsz: +4(type) +4(ver/flags) +4(default) +4(count) */
                int off = i + 4 + 4;
                uint32_t default_sz = rd32(scan_buf + off);
                uint32_t count = rd32(scan_buf + off + 4);
                off += 8;
                (void)default_sz;

                info->num_samples = (count > MAX_SAMPLES) ? MAX_SAMPLES : count;

                /* Read sample sizes directly from file if they extend past scan_buf */
                {
                    int file_off = off;  /* offset within scan_buf = file offset (we read from 0) */
                    uint32_t j;
                    for (j = 0; j < info->num_samples && file_off + 4 <= n; j++)
                    {
                        info->sample_sizes[j] = rd32(scan_buf + file_off);
                        file_off += 4;
                    }
                    /* If we ran out of scan buffer, read remaining from file */
                    if (j < info->num_samples)
                    {
                        rb->lseek(fd, file_off, SEEK_SET);
                        for (; j < info->num_samples; j++)
                        {
                            uint8_t tmp[4];
                            if (rb->read(fd, tmp, 4) != 4) break;
                            info->sample_sizes[j] = rd32(tmp);
                        }
                        info->num_samples = j;
                    }
                }
                break;
            }
        }

        /* Find stco */
        for (i = vide_pos; i < n - 8; i++)
        {
            if (scan_buf[i] == 's' && scan_buf[i+1] == 't' &&
                scan_buf[i+2] == 'c' && scan_buf[i+3] == 'o')
            {
                int off = i + 4 + 4; /* skip type + ver/flags */
                uint32_t count = rd32(scan_buf + off);
                off += 4;

                info->num_chunks = (count > MAX_CHUNKS) ? MAX_CHUNKS : count;

                {
                    uint32_t j;
                    for (j = 0; j < info->num_chunks && off + 4 <= n; j++)
                    {
                        info->chunk_offsets[j] = rd32(scan_buf + off);
                        off += 4;
                    }
                    if (j < info->num_chunks)
                    {
                        rb->lseek(fd, off, SEEK_SET);
                        for (; j < info->num_chunks; j++)
                        {
                            uint8_t tmp[4];
                            if (rb->read(fd, tmp, 4) != 4) break;
                            info->chunk_offsets[j] = rd32(tmp);
                        }
                        info->num_chunks = j;
                    }
                }
                break;
            }
        }
    }

    return (info->avcc_len > 0 && info->width > 0 &&
            info->num_samples > 0 && info->num_chunks > 0);
}

/* ---- Decode Annex B .264 file ---- */

static int decode_annexb(struct vpu_h264 *dec, uint8_t *file_buf, int fsize)
{
    int frame_count = 0, errors = 0;
    int pos = 0, sc_len, nalu_idx = 0;

    while (pos < fsize && frame_count < MAX_FRAMES)
    {
        int sc_pos = find_start_code(file_buf + pos, fsize - pos, &sc_len);
        if (sc_pos < 0) break;
        int nalu_start = pos + sc_pos + sc_len;
        int sc2_len;
        int sc2_pos = find_start_code(file_buf + nalu_start,
                                       fsize - nalu_start, &sc2_len);
        int nalu_len = (sc2_pos >= 0) ? sc2_pos : (fsize - nalu_start);
        int nal_type = file_buf[nalu_start] & 0x1F;

        poc_log("  NALU type=%d len=%d", nal_type, nalu_len);
        nalu_idx++;

        long t0 = *rb->current_tick;
        int ret = vpu_h264_decode_nalu(dec, file_buf + nalu_start, nalu_len);
        long t1 = *rb->current_tick;

        if (ret == 1)
        {
            long ms = (t1 - t0) * 1000 / HZ;
            const uint8_t *y, *cb, *cr;
            int w, h, nz, i;
            vpu_h264_get_frame(dec, &y, &cb, &cr, &w, &h);
            nz = 0;
            for (i = 0; i < w * h; i++)
                if (y[i] != 0) nz++;
            uint32_t crc_y = crc32_calc(y, w * h);
            uint32_t crc_cb = crc32_calc(cb, (w/2) * (h/2));
            uint32_t crc_cr = crc32_calc(cr, (w/2) * (h/2));
            poc_log("--- Frame %d: %dx%d nz=%d/%d Y=%08lx Cb=%08lx Cr=%08lx %ldms ---",
                    frame_count, w, h, nz, w * h,
                    (unsigned long)crc_y, (unsigned long)crc_cb,
                    (unsigned long)crc_cr, ms);
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

    poc_log("--- Annex B: %d frames, %d errors ---", frame_count, errors);
    return frame_count;
}

/* ---- Decode MP4/M4V file ---- */

static int decode_mp4(struct vpu_h264 *dec, int fd, struct mp4_info *info,
                       uint8_t *read_buf)
{
    int frame_count = 0, errors = 0;
    uint32_t s;

    /* Configure decoder with avcC */
    if (vpu_h264_configure(dec, info->avcc, info->avcc_len) < 0)
    {
        poc_log("ERROR: vpu_h264_configure failed");
        return -1;
    }
    poc_log("avcC configured: %dx%d nalu_len=%d",
            info->width, info->height, info->nalu_len_size);

    /* Decode samples (1 sample per chunk for simple stco layout) */
    for (s = 0; s < info->num_samples && s < info->num_chunks
                && frame_count < MAX_FRAMES; s++)
    {
        uint32_t offset = info->chunk_offsets[s];
        uint32_t size = info->sample_sizes[s];
        int pos;

        if ((int)size > READ_BUF_SIZE)
            size = READ_BUF_SIZE;

        rb->lseek(fd, offset, SEEK_SET);
        if (rb->read(fd, read_buf, size) != (ssize_t)size)
        {
            poc_log("  Sample %lu: read error", (unsigned long)s);
            errors++;
            continue;
        }

        /* Extract length-prefixed NALUs */
        pos = 0;
        while (pos + info->nalu_len_size <= (int)size)
        {
            uint32_t nalu_len = 0;
            int i;
            for (i = 0; i < info->nalu_len_size; i++)
                nalu_len = (nalu_len << 8) | read_buf[pos + i];
            pos += info->nalu_len_size;

            if (nalu_len == 0 || pos + (int)nalu_len > (int)size)
                break;

            int nal_type = read_buf[pos] & 0x1F;

            long t0 = *rb->current_tick;
            int ret = vpu_h264_decode_nalu(dec, read_buf + pos, nalu_len);
            long t1 = *rb->current_tick;

            if (ret == 1)
            {
                long ms = (t1 - t0) * 1000 / HZ;
                const uint8_t *y, *cb, *cr;
                int w, h, nz;
                vpu_h264_get_frame(dec, &y, &cb, &cr, &w, &h);
                nz = 0;
                for (i = 0; i < w * h; i++)
                    if (y[i] != 0) nz++;
                uint32_t crc_y = crc32_calc(y, w * h);
                uint32_t crc_cb = crc32_calc(cb, (w/2) * (h/2));
                uint32_t crc_cr = crc32_calc(cr, (w/2) * (h/2));
                poc_log("--- Frame %d (s%lu): %dx%d nz=%d/%d Y=%08lx Cb=%08lx Cr=%08lx %ldms type=%d ---",
                        frame_count, (unsigned long)s, w, h, nz, w * h,
                        (unsigned long)crc_y, (unsigned long)crc_cb,
                        (unsigned long)crc_cr, ms, nal_type);
                frame_count++;
                lflush();
            }
            else if (ret < 0)
            {
                poc_log("  Sample %lu DECODE ERROR type=%d len=%lu",
                        (unsigned long)s, nal_type, (unsigned long)nalu_len);
                errors++;
                lflush();
            }

            pos += nalu_len;
        }
    }

    poc_log("--- MP4: %d frames, %d errors ---", frame_count, errors);
    return frame_count;
}

/* ---- Main ---- */

enum plugin_status plugin_start(const void *parameter)
{
    const char *test_path = parameter ? (const char *)parameter : "/test_lowqp.264";
    size_t buf_size;
    uint8_t *buf, *p;
    struct vpu_h264 *dec;
    int frame_count = 0;

    rb->splashf(HZ/2, "v59 %s", test_path);

    /* Generate per-file log */
    {
        const char *slash = rb->strrchr(test_path, '/');
        const char *base = slash ? slash + 1 : test_path;
        rb->snprintf(log_path_buf, sizeof(log_path_buf), "/vdec_%s.log", base);
        log_fd = rb->open(log_path_buf, O_WRONLY|O_CREAT|O_TRUNC, 0666);
    }
    poc_log("=== v59 — MP4+AnnexB diagnostic: %s ===", test_path);

    /* Allocate from audio buffer */
    buf = rb->plugin_get_audio_buffer(&buf_size);
    p = buf;

    size_t dec_size = vpu_h264_buf_size(640, 480);
    poc_log("Decoder needs %lu bytes, have %lu",
            (unsigned long)dec_size, (unsigned long)buf_size);

    if (dec_size + MAX_SAMPLES * 4 + MAX_CHUNKS * 4 +
        READ_BUF_SIZE + MAX_FILE_SIZE + 4096 > buf_size)
    {
        poc_log("ERROR: buffer too small");
        if (log_fd >= 0) rb->close(log_fd);
        rb->splash(HZ*3, "Buffer too small!");
        return PLUGIN_ERROR;
    }

    /* Open decoder */
    dec = vpu_h264_open(p, dec_size, 640, 480);
    p += dec_size;
    if (!dec)
    {
        poc_log("ERROR: vpu_h264_open failed");
        if (log_fd >= 0) rb->close(log_fd);
        rb->splash(HZ*3, "Decoder init failed!");
        return PLUGIN_ERROR;
    }
    poc_log("Decoder opened OK");
    lflush();

    if (is_mp4_file(test_path))
    {
        /* MP4/M4V path */
        struct mp4_info info;
        uint8_t *scan_buf, *read_buf;
        int fd, scan_size;

        /* Allocate work buffers */
        info.sample_sizes = (uint32_t *)p;
        p += MAX_SAMPLES * 4;
        info.chunk_offsets = (uint32_t *)p;
        p += MAX_CHUNKS * 4;
        read_buf = p;
        p += READ_BUF_SIZE;
        scan_buf = p;
        scan_size = (int)(buf + buf_size - p);
        if (scan_size > 5 * 1024 * 1024)
            scan_size = 5 * 1024 * 1024;

        poc_log("--- MP4 mode: scan_size=%d ---", scan_size);

        fd = rb->open(test_path, O_RDONLY);
        if (fd < 0)
        {
            poc_log("ERROR: file not found");
            vpu_h264_close(dec);
            if (log_fd >= 0) rb->close(log_fd);
            rb->splash(HZ*3, "File not found!");
            return PLUGIN_ERROR;
        }

        if (!mp4_parse(fd, &info, scan_buf, scan_size))
        {
            poc_log("ERROR: MP4 parse failed");
            rb->close(fd);
            vpu_h264_close(dec);
            if (log_fd >= 0) rb->close(log_fd);
            rb->splash(HZ*3, "MP4 parse failed!");
            return PLUGIN_ERROR;
        }

        poc_log("MP4: %dx%d, %lu samples, %lu chunks, avcc=%d bytes",
                info.width, info.height,
                (unsigned long)info.num_samples,
                (unsigned long)info.num_chunks,
                info.avcc_len);

        frame_count = decode_mp4(dec, fd, &info, read_buf);
        rb->close(fd);
    }
    else
    {
        /* Annex B .264 path */
        uint8_t *file_buf = p;
        int fd, fsize;

        poc_log("--- Annex B mode ---");

        fd = rb->open(test_path, O_RDONLY);
        if (fd < 0)
        {
            poc_log("ERROR: file not found");
            vpu_h264_close(dec);
            if (log_fd >= 0) rb->close(log_fd);
            rb->splash(HZ*3, "File not found!");
            return PLUGIN_ERROR;
        }

        fsize = rb->filesize(fd);
        if (fsize > MAX_FILE_SIZE) fsize = MAX_FILE_SIZE;
        rb->read(fd, file_buf, fsize);
        rb->close(fd);
        poc_log("Read %d bytes", fsize);

        frame_count = decode_annexb(dec, file_buf, fsize);
    }

    lflush();
    vpu_h264_close(dec);

    if (log_fd >= 0) rb->close(log_fd);
    rb->splashf(HZ*3, "v59: %d frames", frame_count);
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
