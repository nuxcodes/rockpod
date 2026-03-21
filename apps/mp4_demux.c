/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2025
 *
 * MP4 video demuxer - extracts H.264 video track data from MP4/M4V files.
 * Based on libm4a/demux.c box-walking pattern, with fd-based I/O and
 * video track support (vmhd + avc1/avcC parsing).
 *
 * Original demux.c copyright:
 * Copyright (c) 2005 David Hammerton (MIT license)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include "config.h"
#include "system.h"
#include "file.h"
#include "mp4_demux.h"

#include <string.h>

/* --- Simple fd-based stream I/O --- */

struct mp4_stream {
    int fd;
    off_t pos;
    bool eof;
};

static void stream_init(struct mp4_stream *s, int fd)
{
    s->fd = fd;
    s->pos = 0;
    s->eof = false;
}

static bool stream_read(struct mp4_stream *s, void *buf, size_t len)
{
    ssize_t n = read(s->fd, buf, len);
    if (n < (ssize_t)len)
    {
        s->eof = true;
        return false;
    }
    s->pos += n;
    return true;
}

static uint32_t stream_read_uint32(struct mp4_stream *s)
{
    uint8_t buf[4];
    if (!stream_read(s, buf, 4))
        return 0;
    /* big-endian */
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16)
         | ((uint32_t)buf[2] << 8)  | (uint32_t)buf[3];
}

static uint16_t stream_read_uint16(struct mp4_stream *s)
{
    uint8_t buf[2];
    if (!stream_read(s, buf, 2))
        return 0;
    return ((uint16_t)buf[0] << 8) | (uint16_t)buf[1];
}

static uint8_t stream_read_uint8(struct mp4_stream *s)
{
    uint8_t b;
    if (!stream_read(s, &b, 1))
        return 0;
    return b;
}

static void stream_skip(struct mp4_stream *s, size_t n)
{
    s->pos = lseek(s->fd, (off_t)n, SEEK_CUR);
    if (s->pos < 0)
        s->eof = true;
}

static void stream_seek(struct mp4_stream *s, off_t offset)
{
    s->pos = lseek(s->fd, offset, SEEK_SET);
    if (s->pos < 0)
        s->eof = true;
}

static off_t stream_tell(struct mp4_stream *s)
{
    return s->pos;
}

/* --- Box parsers --- */

struct mp4_parse_ctx {
    struct mp4_stream stream;
    struct mp4v_demux_res *res;
    bool found_video;    /* set when we find a video track */
    bool found_audio;    /* set when we find an audio track */
    bool in_video_trak;  /* currently inside a video trak */
    bool in_audio_trak;  /* currently inside an audio trak */
    bool want_audio;     /* caller provided audio buffers */
    uint16_t tkhd_dw;    /* tkhd display width (pending video confirm) */
    uint16_t tkhd_dh;    /* tkhd display height (pending video confirm) */
    /* Pending per-trak fields (committed at end of trak once type is known) */
    uint32_t pending_timescale;   /* from mdhd (parsed before hdlr) */
    uint32_t pending_lead_trim;   /* from edts/elst (parsed before mdia) */
};

/* Parse avcC box - H.264 decoder configuration record.
 * Layout: configurationVersion(1) + AVCProfileIndication(1) +
 *         profile_compatibility(1) + AVCLevelIndication(1) +
 *         lengthSizeMinusOne(1, lower 2 bits) +
 *         numSPS(1, lower 5 bits) + [spsLen(2)+spsData]... +
 *         numPPS(1) + [ppsLen(2)+ppsData]... */
static bool read_chunk_avcc(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t data_len = chunk_len - 8;
    uint8_t config_ver;

    if (data_len > MP4V_MAX_CODECDATA || data_len < 7)
        return false;

    /* Read the entire avcC payload into codecdata */
    ctx->res->codecdata_len = data_len;
    if (!stream_read(&ctx->stream, ctx->res->codecdata, data_len))
        return false;

    /* Parse key fields from the buffer */
    config_ver = ctx->res->codecdata[0];
    if (config_ver != 1)
        return false;

    ctx->res->avc_profile   = ctx->res->codecdata[1];
    ctx->res->avc_level     = ctx->res->codecdata[3];
    ctx->res->nalu_len_size = (ctx->res->codecdata[4] & 0x03) + 1;
    ctx->res->num_sps       = ctx->res->codecdata[5] & 0x1F;

    /* Count PPS: skip past SPS entries to find numPPS */
    {
        size_t offset = 6;
        uint8_t i;

        for (i = 0; i < ctx->res->num_sps && offset + 2 <= data_len; i++)
        {
            uint16_t sps_len = ((uint16_t)ctx->res->codecdata[offset] << 8)
                             | ctx->res->codecdata[offset + 1];
            offset += 2 + sps_len;
        }

        if (offset < data_len)
            ctx->res->num_pps = ctx->res->codecdata[offset];
    }

    return true;
}

/* Parse stsd for video: expects avc1 sample entry */
static bool read_chunk_stsd_video(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t size_remaining = chunk_len - 8;
    uint32_t numentries, i;

    /* version + flags */
    stream_read_uint32(&ctx->stream);
    size_remaining -= 4;

    numentries = stream_read_uint32(&ctx->stream);
    size_remaining -= 4;

    for (i = 0; i < numentries && size_remaining > 8; i++)
    {
        uint32_t entry_size = stream_read_uint32(&ctx->stream);
        uint32_t format = stream_read_uint32(&ctx->stream);
        uint32_t entry_remaining = entry_size - 8;

        if (entry_size <= 8 || entry_size > size_remaining)
            break;

        if (format == MAKEFOURCC('a','v','c','1')
            || format == MAKEFOURCC('a','v','c','3'))
        {
            ctx->res->format = format;

            /* reserved(6) + data_ref_index(2) */
            stream_skip(&ctx->stream, 8);
            entry_remaining -= 8;

            /* pre-defined(2) + reserved(2) + pre-defined(12) */
            stream_skip(&ctx->stream, 16);
            entry_remaining -= 16;

            ctx->res->width = stream_read_uint16(&ctx->stream);
            ctx->res->height = stream_read_uint16(&ctx->stream);
            entry_remaining -= 4;

            /* horiz_resolution(4) + vert_resolution(4) + reserved(4) +
               frame_count(2) + compressor_name(32) + depth(2) +
               pre_defined(2) */
            stream_skip(&ctx->stream, 50);
            entry_remaining -= 50;

            /* Walk sub-boxes looking for avcC */
            while (entry_remaining > 8)
            {
                uint32_t sub_len = stream_read_uint32(&ctx->stream);
                uint32_t sub_id  = stream_read_uint32(&ctx->stream);

                if (sub_len <= 8 || sub_len > entry_remaining)
                    break;

                if (sub_id == MAKEFOURCC('a','v','c','C'))
                {
                    if (!read_chunk_avcc(ctx, sub_len))
                        return false;
                }
                else
                {
                    stream_skip(&ctx->stream, sub_len - 8);
                }

                entry_remaining -= sub_len;
            }

            if (entry_remaining > 0)
                stream_skip(&ctx->stream, entry_remaining);
        }
        else
        {
            /* Unknown video format, skip */
            stream_skip(&ctx->stream, entry_remaining);
        }

        size_remaining -= entry_size;
    }

    if (size_remaining > 0)
        stream_skip(&ctx->stream, size_remaining);

    return ctx->res->format != 0;
}

/* Read MPEG-4 descriptor length (variable-length, 7 bits/byte, high bit=continue).
 * Ported from libm4a/demux.c:78-92. */
static uint32_t read_mp4_descr_length(struct mp4_stream *s)
{
    uint8_t b;
    uint8_t num_bytes = 0;
    uint32_t length = 0;

    do {
        b = stream_read_uint8(s);
        num_bytes++;
        length = (length << 7) | (b & 0x7F);
    } while ((b & 0x80) && num_bytes < 4);

    return length;
}

/* Parse esds box — extract AudioSpecificConfig for AAC decoder init.
 * Ported from libm4a/demux.c:95-154. */
static bool read_chunk_esds(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    off_t box_end = ctx->stream.pos + (off_t)(chunk_len - 8);
    uint8_t tag;
    uint32_t descr_len;

    /* version + flags */
    stream_read_uint32(&ctx->stream);

    /* ES_Descriptor (tag 0x03) */
    tag = stream_read_uint8(&ctx->stream);
    descr_len = read_mp4_descr_length(&ctx->stream);
    (void)descr_len;

    if (tag == 0x03)
    {
        /* ES_ID(2) + streamPriority(1) */
        stream_skip(&ctx->stream, 3);
    }
    else
    {
        /* Non-standard: skip 2 bytes instead */
        stream_skip(&ctx->stream, 2);
    }

    /* DecoderConfigDescriptor (tag 0x04) */
    tag = stream_read_uint8(&ctx->stream);
    descr_len = read_mp4_descr_length(&ctx->stream);
    (void)descr_len;

    if (tag != 0x04)
        return true; /* not fatal, just no codec data */

    /* objectTypeIndication(1) + streamType(1) + bufferSizeDB(3) +
       maxBitrate(4) + avgBitrate(4) = 13 bytes */
    stream_skip(&ctx->stream, 13);

    /* DecoderSpecificInfo (tag 0x05) = AudioSpecificConfig */
    tag = stream_read_uint8(&ctx->stream);
    descr_len = read_mp4_descr_length(&ctx->stream);

    if (tag == 0x05 && descr_len > 0)
    {
        if (descr_len > sizeof(ctx->res->audio_codecdata))
            descr_len = sizeof(ctx->res->audio_codecdata);

        ctx->res->audio_codecdata_len = descr_len;
        stream_read(&ctx->stream, ctx->res->audio_codecdata, descr_len);
    }

    /* Skip any trailing descriptors (SLConfigDescriptor etc.) */
    {
        off_t remaining = box_end - ctx->stream.pos;
        if (remaining > 0)
            stream_skip(&ctx->stream, (size_t)remaining);
    }

    return true;
}

/* Parse stsd for audio: expects mp4a sample entry.
 * mp4a layout: reserved(6) + data_ref_index(2) + reserved(8) +
 *   channels(2) + sample_size(2) + packet_size(2) + sample_rate(4, 16.16 FP) +
 *   reserved(2) + sub-boxes (esds, etc.) */
static bool read_chunk_stsd_audio(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t size_remaining = chunk_len - 8;
    uint32_t numentries, i;

    stream_read_uint32(&ctx->stream); /* version + flags */
    size_remaining -= 4;

    numentries = stream_read_uint32(&ctx->stream);
    size_remaining -= 4;

    for (i = 0; i < numentries && size_remaining > 8; i++)
    {
        uint32_t entry_size = stream_read_uint32(&ctx->stream);
        uint32_t format = stream_read_uint32(&ctx->stream);
        uint32_t entry_remaining = entry_size - 8;

        if (entry_size <= 8 || entry_size > size_remaining)
            break;

        if (format == MAKEFOURCC('m','p','4','a'))
        {
            ctx->res->audio_format = format;

            /* reserved(6) + data_ref_index(2) */
            stream_skip(&ctx->stream, 8);
            entry_remaining -= 8;

            /* reserved(8) */
            stream_skip(&ctx->stream, 8);
            entry_remaining -= 8;

            /* channelCount(2) + sampleSize(2) + compressionID(2) +
               packetSize(2) = 8 bytes */
            ctx->res->audio_channels = stream_read_uint16(&ctx->stream);
            stream_skip(&ctx->stream, 6); /* sampleSize+compressionID+packetSize */
            entry_remaining -= 8;

            /* sampleRate: 16.16 fixed-point, 4 bytes */
            {
                uint32_t sr_fp = stream_read_uint32(&ctx->stream);
                ctx->res->audio_sample_rate = sr_fp >> 16;
            }
            entry_remaining -= 4;

            /* Walk sub-boxes for esds */
            while (entry_remaining > 8)
            {
                uint32_t sub_len = stream_read_uint32(&ctx->stream);
                uint32_t sub_id  = stream_read_uint32(&ctx->stream);

                if (sub_len <= 8 || sub_len > entry_remaining)
                    break;

                if (sub_id == MAKEFOURCC('e','s','d','s'))
                {
                    if (!read_chunk_esds(ctx, sub_len))
                        return false;
                }
                else
                {
                    stream_skip(&ctx->stream, sub_len - 8);
                }

                entry_remaining -= sub_len;
            }

            if (entry_remaining > 0)
                stream_skip(&ctx->stream, entry_remaining);
        }
        else
        {
            stream_skip(&ctx->stream, entry_remaining);
        }

        size_remaining -= entry_size;
    }

    if (size_remaining > 0)
        stream_skip(&ctx->stream, size_remaining);

    return true;
}

/* Audio sample table parsers — write to audio_* fields in res */

static bool read_chunk_stts_audio(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t size_remaining = chunk_len - 8;
    uint32_t numentries, i;

    stream_read_uint32(&ctx->stream);
    size_remaining -= 4;

    numentries = stream_read_uint32(&ctx->stream);
    size_remaining -= 4;

    if (numentries > MP4V_MAX_STTS)
        numentries = MP4V_MAX_STTS;

    ctx->res->audio_num_stts = numentries;
    for (i = 0; i < numentries; i++)
    {
        ctx->res->audio_stts[i].sample_count = stream_read_uint32(&ctx->stream);
        ctx->res->audio_stts[i].sample_delta = stream_read_uint32(&ctx->stream);
        size_remaining -= 8;
    }

    if (size_remaining > 0)
        stream_skip(&ctx->stream, size_remaining);

    return true;
}

static bool read_chunk_stsz_audio(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t size_remaining = chunk_len - 8;
    uint32_t default_size, numsizes, i, cap;

    stream_read_uint32(&ctx->stream);
    size_remaining -= 4;

    default_size = stream_read_uint32(&ctx->stream);
    size_remaining -= 4;

    numsizes = stream_read_uint32(&ctx->stream);
    size_remaining -= 4;

    ctx->res->audio_num_samples = numsizes;
    cap = (numsizes < ctx->res->audio_sample_sizes_cap)
        ? numsizes : ctx->res->audio_sample_sizes_cap;

    if (default_size != 0)
    {
        for (i = 0; i < cap; i++)
            ctx->res->audio_sample_sizes[i] = default_size;
    }
    else
    {
        for (i = 0; i < cap; i++)
        {
            ctx->res->audio_sample_sizes[i] = stream_read_uint32(&ctx->stream);
            size_remaining -= 4;
        }
        if (numsizes > cap)
        {
            stream_skip(&ctx->stream, (numsizes - cap) * 4);
            size_remaining -= (numsizes - cap) * 4;
        }
    }

    if (size_remaining > 0)
        stream_skip(&ctx->stream, size_remaining);

    return true;
}

static bool read_chunk_stsc_audio(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t size_remaining = chunk_len - 8;
    uint32_t numentries, i;

    stream_read_uint32(&ctx->stream);
    size_remaining -= 4;

    numentries = stream_read_uint32(&ctx->stream);
    size_remaining -= 4;

    if (numentries > MP4V_MAX_STSC)
        numentries = MP4V_MAX_STSC;

    ctx->res->audio_num_stsc = numentries;
    for (i = 0; i < numentries; i++)
    {
        ctx->res->audio_stsc[i].first_chunk = stream_read_uint32(&ctx->stream);
        ctx->res->audio_stsc[i].samples_per_chunk = stream_read_uint32(&ctx->stream);
        ctx->res->audio_stsc[i].sample_desc_index = stream_read_uint32(&ctx->stream);
        size_remaining -= 12;
    }

    if (size_remaining > 0)
        stream_skip(&ctx->stream, size_remaining);

    return true;
}

static bool read_chunk_stco_audio(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t size_remaining = chunk_len - 8;
    uint32_t numentries, i, cap;

    stream_read_uint32(&ctx->stream);
    size_remaining -= 4;

    numentries = stream_read_uint32(&ctx->stream);
    size_remaining -= 4;

    ctx->res->audio_num_stco = numentries; /* real count from file */
    cap = (numentries < ctx->res->audio_chunk_offsets_cap)
        ? numentries : ctx->res->audio_chunk_offsets_cap;

    for (i = 0; i < cap; i++)
    {
        ctx->res->audio_chunk_offsets[i] = stream_read_uint32(&ctx->stream);
        size_remaining -= 4;
    }

    if (numentries > cap)
    {
        stream_skip(&ctx->stream, (numentries - cap) * 4);
        size_remaining -= (numentries - cap) * 4;
    }

    if (size_remaining > 0)
        stream_skip(&ctx->stream, size_remaining);

    return true;
}

static bool read_chunk_co64_audio(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t size_remaining = chunk_len - 8;
    uint32_t numentries, i, cap;

    stream_read_uint32(&ctx->stream);
    size_remaining -= 4;

    numentries = stream_read_uint32(&ctx->stream);
    size_remaining -= 4;

    ctx->res->audio_num_stco = numentries; /* real count from file */
    cap = (numentries < ctx->res->audio_chunk_offsets_cap)
        ? numentries : ctx->res->audio_chunk_offsets_cap;

    for (i = 0; i < cap; i++)
    {
        uint32_t hi = stream_read_uint32(&ctx->stream);
        uint32_t lo = stream_read_uint32(&ctx->stream);
        (void)hi;
        ctx->res->audio_chunk_offsets[i] = lo;
        size_remaining -= 8;
    }

    if (numentries > cap)
    {
        stream_skip(&ctx->stream, (numentries - cap) * 8);
        size_remaining -= (numentries - cap) * 8;
    }

    if (size_remaining > 0)
        stream_skip(&ctx->stream, size_remaining);

    return true;
}

/* Audio sample table container */
static bool read_chunk_stbl_audio(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t size_remaining = chunk_len - 8;

    while (size_remaining > 8)
    {
        uint32_t sub_len = stream_read_uint32(&ctx->stream);
        uint32_t sub_id  = stream_read_uint32(&ctx->stream);

        if (sub_len <= 1 || sub_len > size_remaining)
            return false;

        switch (sub_id)
        {
            case MAKEFOURCC('s','t','s','d'):
                if (!read_chunk_stsd_audio(ctx, sub_len))
                    return false;
                break;
            case MAKEFOURCC('s','t','t','s'):
                if (!read_chunk_stts_audio(ctx, sub_len))
                    return false;
                break;
            case MAKEFOURCC('s','t','s','z'):
                if (!read_chunk_stsz_audio(ctx, sub_len))
                    return false;
                break;
            case MAKEFOURCC('s','t','s','c'):
                if (!read_chunk_stsc_audio(ctx, sub_len))
                    return false;
                break;
            case MAKEFOURCC('s','t','c','o'):
                if (!read_chunk_stco_audio(ctx, sub_len))
                    return false;
                break;
            case MAKEFOURCC('c','o','6','4'):
                if (!read_chunk_co64_audio(ctx, sub_len))
                    return false;
                break;
            /* No stss for audio — all AAC frames are sync samples */
            default:
                stream_skip(&ctx->stream, sub_len - 8);
                break;
        }

        size_remaining -= sub_len;
    }

    return true;
}

/* stts - time-to-sample table */
static bool read_chunk_stts(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t size_remaining = chunk_len - 8;
    uint32_t numentries, i;

    stream_read_uint32(&ctx->stream); /* version + flags */
    size_remaining -= 4;

    numentries = stream_read_uint32(&ctx->stream);
    size_remaining -= 4;

    if (numentries > MP4V_MAX_STTS)
        numentries = MP4V_MAX_STTS;

    ctx->res->num_stts = numentries;
    for (i = 0; i < numentries; i++)
    {
        ctx->res->stts[i].sample_count = stream_read_uint32(&ctx->stream);
        ctx->res->stts[i].sample_delta = stream_read_uint32(&ctx->stream);
        size_remaining -= 8;
    }

    if (size_remaining > 0)
        stream_skip(&ctx->stream, size_remaining);

    return true;
}

/* stsz - sample sizes */
static bool read_chunk_stsz(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t size_remaining = chunk_len - 8;
    uint32_t default_size, numsizes, i;

    stream_read_uint32(&ctx->stream); /* version + flags */
    size_remaining -= 4;

    default_size = stream_read_uint32(&ctx->stream);
    size_remaining -= 4;

    numsizes = stream_read_uint32(&ctx->stream);
    size_remaining -= 4;

    ctx->res->num_samples = numsizes;

    if (default_size != 0)
    {
        /* All samples same size */
        uint32_t cap = (numsizes < ctx->res->sample_sizes_cap)
                     ? numsizes : ctx->res->sample_sizes_cap;
        for (i = 0; i < cap; i++)
            ctx->res->sample_sizes[i] = default_size;

        if (size_remaining > 0)
            stream_skip(&ctx->stream, size_remaining);
        return true;
    }

    /* Variable sample sizes */
    {
        uint32_t cap = (numsizes < ctx->res->sample_sizes_cap)
                     ? numsizes : ctx->res->sample_sizes_cap;
        for (i = 0; i < cap; i++)
        {
            ctx->res->sample_sizes[i] = stream_read_uint32(&ctx->stream);
            size_remaining -= 4;
        }
        /* Skip remaining if we hit our cap */
        if (numsizes > cap)
        {
            stream_skip(&ctx->stream, (numsizes - cap) * 4);
            size_remaining -= (numsizes - cap) * 4;
        }
    }

    if (size_remaining > 0)
        stream_skip(&ctx->stream, size_remaining);

    return true;
}

/* stsc - sample-to-chunk table */
static bool read_chunk_stsc(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t size_remaining = chunk_len - 8;
    uint32_t numentries, i;

    stream_read_uint32(&ctx->stream); /* version + flags */
    size_remaining -= 4;

    numentries = stream_read_uint32(&ctx->stream);
    size_remaining -= 4;

    if (numentries > MP4V_MAX_STSC)
        numentries = MP4V_MAX_STSC;

    ctx->res->num_stsc = numentries;
    for (i = 0; i < numentries; i++)
    {
        ctx->res->stsc[i].first_chunk = stream_read_uint32(&ctx->stream);
        ctx->res->stsc[i].samples_per_chunk = stream_read_uint32(&ctx->stream);
        ctx->res->stsc[i].sample_desc_index = stream_read_uint32(&ctx->stream);
        size_remaining -= 12;
    }

    if (size_remaining > 0)
        stream_skip(&ctx->stream, size_remaining);

    return true;
}

/* stco - chunk offset table */
static bool read_chunk_stco(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t size_remaining = chunk_len - 8;
    uint32_t numentries, i, cap;

    stream_read_uint32(&ctx->stream); /* version + flags */
    size_remaining -= 4;

    numentries = stream_read_uint32(&ctx->stream);
    size_remaining -= 4;

    ctx->res->num_stco = numentries; /* real count from file */
    cap = (numentries < ctx->res->chunk_offsets_cap)
        ? numentries : ctx->res->chunk_offsets_cap;

    for (i = 0; i < cap; i++)
    {
        ctx->res->chunk_offsets[i] = stream_read_uint32(&ctx->stream);
        size_remaining -= 4;
    }

    if (numentries > cap)
    {
        stream_skip(&ctx->stream, (numentries - cap) * 4);
        size_remaining -= (numentries - cap) * 4;
    }

    if (size_remaining > 0)
        stream_skip(&ctx->stream, size_remaining);

    return true;
}

/* co64 - 64-bit chunk offset table (for files >4GB).
 * Reads 64-bit offsets but truncates to 32-bit.
 * Files with chunk offsets >4GB are not supported. */
static bool read_chunk_co64(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t size_remaining = chunk_len - 8;
    uint32_t numentries, i, cap;

    stream_read_uint32(&ctx->stream); /* version + flags */
    size_remaining -= 4;

    numentries = stream_read_uint32(&ctx->stream);
    size_remaining -= 4;

    ctx->res->num_stco = numentries; /* real count from file */
    cap = (numentries < ctx->res->chunk_offsets_cap)
        ? numentries : ctx->res->chunk_offsets_cap;

    for (i = 0; i < cap; i++)
    {
        uint32_t hi = stream_read_uint32(&ctx->stream);
        uint32_t lo = stream_read_uint32(&ctx->stream);
        (void)hi; /* discard upper 32 bits */
        ctx->res->chunk_offsets[i] = lo;
        size_remaining -= 8;
    }

    if (numentries > cap)
    {
        stream_skip(&ctx->stream, (numentries - cap) * 8);
        size_remaining -= (numentries - cap) * 8;
    }

    if (size_remaining > 0)
        stream_skip(&ctx->stream, size_remaining);

    return true;
}

/* stss - sync sample (keyframe) table */
static bool read_chunk_stss(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t size_remaining = chunk_len - 8;
    uint32_t numentries, i, cap;

    stream_read_uint32(&ctx->stream); /* version + flags */
    size_remaining -= 4;

    numentries = stream_read_uint32(&ctx->stream);
    size_remaining -= 4;

    cap = (numentries < MP4V_MAX_STSS) ? numentries : MP4V_MAX_STSS;
    ctx->res->num_stss = cap;

    for (i = 0; i < cap; i++)
    {
        ctx->res->stss[i] = stream_read_uint32(&ctx->stream);
        size_remaining -= 4;
    }

    if (numentries > cap)
    {
        stream_skip(&ctx->stream, (numentries - cap) * 4);
        size_remaining -= (numentries - cap) * 4;
    }

    if (size_remaining > 0)
        stream_skip(&ctx->stream, size_remaining);

    return true;
}

/* stbl - sample table container */
static bool read_chunk_stbl(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t size_remaining = chunk_len - 8;

    while (size_remaining > 8)
    {
        uint32_t sub_len = stream_read_uint32(&ctx->stream);
        uint32_t sub_id  = stream_read_uint32(&ctx->stream);

        if (sub_len <= 1 || sub_len > size_remaining)
            return false;

        switch (sub_id)
        {
            case MAKEFOURCC('s','t','s','d'):
                if (!read_chunk_stsd_video(ctx, sub_len))
                    return false;
                break;
            case MAKEFOURCC('s','t','t','s'):
                if (!read_chunk_stts(ctx, sub_len))
                    return false;
                break;
            case MAKEFOURCC('s','t','s','z'):
                if (!read_chunk_stsz(ctx, sub_len))
                    return false;
                break;
            case MAKEFOURCC('s','t','s','c'):
                if (!read_chunk_stsc(ctx, sub_len))
                    return false;
                break;
            case MAKEFOURCC('s','t','c','o'):
                if (!read_chunk_stco(ctx, sub_len))
                    return false;
                break;
            case MAKEFOURCC('c','o','6','4'):
                if (!read_chunk_co64(ctx, sub_len))
                    return false;
                break;
            case MAKEFOURCC('s','t','s','s'):
                if (!read_chunk_stss(ctx, sub_len))
                    return false;
                break;
            default:
                stream_skip(&ctx->stream, sub_len - 8);
                break;
        }

        size_remaining -= sub_len;
    }

    return true;
}

/* minf - accepts vmhd (video) instead of smhd (audio) */
static bool read_chunk_minf(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t size_remaining = chunk_len - 8;

    /* Walk sub-boxes, look for vmhd to confirm video, then find stbl */
    while (size_remaining > 8)
    {
        uint32_t sub_len = stream_read_uint32(&ctx->stream);
        uint32_t sub_id  = stream_read_uint32(&ctx->stream);

        if (sub_len <= 1 || sub_len > size_remaining)
            return false;

        switch (sub_id)
        {
            case MAKEFOURCC('v','m','h','d'):
                /* Video media header — this confirms it's a video track */
                ctx->in_video_trak = true;
                stream_skip(&ctx->stream, sub_len - 8);
                break;
            case MAKEFOURCC('s','m','h','d'):
                /* Sound media header — this is an audio track */
                ctx->in_video_trak = false;
                ctx->in_audio_trak = true;
                stream_skip(&ctx->stream, sub_len - 8);
                break;
            case MAKEFOURCC('s','t','b','l'):
                if (ctx->in_video_trak)
                {
                    if (!read_chunk_stbl(ctx, sub_len))
                        return false;
                }
                else if (ctx->in_audio_trak && ctx->want_audio)
                {
                    if (!read_chunk_stbl_audio(ctx, sub_len))
                        return false;
                }
                else
                {
                    stream_skip(&ctx->stream, sub_len - 8);
                }
                break;
            default:
                stream_skip(&ctx->stream, sub_len - 8);
                break;
        }

        size_remaining -= sub_len;
    }

    return true;
}

/* mdhd - media header (extract timescale) */
static bool read_chunk_mdhd(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t size_remaining = chunk_len - 8;
    uint32_t version;

    version = stream_read_uint32(&ctx->stream); /* version + flags */
    size_remaining -= 4;

    {
        uint32_t ts;

        if ((version >> 24) == 0)
        {
            /* Version 0: 4-byte fields */
            stream_skip(&ctx->stream, 4); /* creation_time */
            stream_skip(&ctx->stream, 4); /* modification_time */
            ts = stream_read_uint32(&ctx->stream);
            stream_skip(&ctx->stream, 4); /* duration */
            size_remaining -= 16;
        }
        else
        {
            /* Version 1: 8-byte fields */
            stream_skip(&ctx->stream, 8); /* creation_time */
            stream_skip(&ctx->stream, 8); /* modification_time */
            ts = stream_read_uint32(&ctx->stream);
            stream_skip(&ctx->stream, 8); /* duration */
            size_remaining -= 28;
        }

        /* Save to pending — committed at end of trak once type is known.
         * mdhd comes before hdlr in mdia, so track type isn't known yet. */
        ctx->pending_timescale = ts;
    }

    if (size_remaining > 0)
        stream_skip(&ctx->stream, size_remaining);

    return true;
}

/* hdlr - handler reference (check for 'vide') */
static bool read_chunk_hdlr(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t size_remaining = chunk_len - 8;
    uint32_t handler_type;

    stream_read_uint32(&ctx->stream); /* version + flags */
    stream_read_uint32(&ctx->stream); /* pre_defined */
    handler_type = stream_read_uint32(&ctx->stream);
    size_remaining -= 12;

    ctx->in_video_trak = false;
    ctx->in_audio_trak = false;

    if (handler_type == MAKEFOURCC('v','i','d','e'))
        ctx->in_video_trak = true;
    else if (handler_type == MAKEFOURCC('s','o','u','n'))
        ctx->in_audio_trak = true;

    if (size_remaining > 0)
        stream_skip(&ctx->stream, size_remaining);

    return true;
}

/* mdia - media container */
static bool read_chunk_mdia(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t size_remaining = chunk_len - 8;

    while (size_remaining > 8)
    {
        uint32_t sub_len = stream_read_uint32(&ctx->stream);
        uint32_t sub_id  = stream_read_uint32(&ctx->stream);

        if (sub_len <= 1 || sub_len > size_remaining)
            return false;

        switch (sub_id)
        {
            case MAKEFOURCC('m','d','h','d'):
                if (!read_chunk_mdhd(ctx, sub_len))
                    return false;
                break;
            case MAKEFOURCC('h','d','l','r'):
                if (!read_chunk_hdlr(ctx, sub_len))
                    return false;
                break;
            case MAKEFOURCC('m','i','n','f'):
                if (!read_chunk_minf(ctx, sub_len))
                    return false;
                break;
            default:
                stream_skip(&ctx->stream, sub_len - 8);
                break;
        }

        size_remaining -= sub_len;
    }

    return true;
}

/* tkhd - track header (extract display width/height for PAR) */
static bool read_chunk_tkhd(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t size_remaining = chunk_len - 8;
    uint32_t version;
    uint32_t dw_fp, dh_fp; /* 16.16 fixed-point display dimensions */

    version = stream_read_uint32(&ctx->stream); /* version + flags */
    size_remaining -= 4;

    if ((version >> 24) == 0)
    {
        /* Version 0: creation(4)+modification(4)+track_id(4)+reserved(4)+
           duration(4) = 20 bytes, then reserved(8)+layer(2)+alt_group(2)+
           volume(2)+reserved(2)+matrix(36)+width(4)+height(4) = 56 bytes */
        stream_skip(&ctx->stream, 20 + 8 + 2 + 2 + 2 + 2 + 36);
        size_remaining -= 20 + 8 + 2 + 2 + 2 + 2 + 36;
    }
    else
    {
        /* Version 1: creation(8)+modification(8)+track_id(4)+reserved(4)+
           duration(8) = 32 bytes */
        stream_skip(&ctx->stream, 32 + 8 + 2 + 2 + 2 + 2 + 36);
        size_remaining -= 32 + 8 + 2 + 2 + 2 + 2 + 36;
    }

    dw_fp = stream_read_uint32(&ctx->stream); /* width in 16.16 */
    dh_fp = stream_read_uint32(&ctx->stream); /* height in 16.16 */
    size_remaining -= 8;

    /* Save temporarily — commit to res only after mdia confirms video */
    ctx->tkhd_dw = (uint16_t)(dw_fp >> 16);
    ctx->tkhd_dh = (uint16_t)(dh_fp >> 16);

    if (size_remaining > 0)
        stream_skip(&ctx->stream, size_remaining);

    return true;
}

/* edts/elst - edit list (extract media_time for encoder priming offset) */
static bool read_chunk_elst(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t size_remaining = chunk_len - 8;
    uint32_t version_flags;
    uint32_t entry_count, i;

    version_flags = stream_read_uint32(&ctx->stream);
    size_remaining -= 4;

    entry_count = stream_read_uint32(&ctx->stream);
    size_remaining -= 4;

    {
        size_t entry_size = ((version_flags >> 24) == 0) ? 12 : 20;

    for (i = 0; i < entry_count && size_remaining >= entry_size; i++)
    {
        int32_t media_time;

        if ((version_flags >> 24) == 0)
        {
            stream_skip(&ctx->stream, 4); /* segment_duration */
            media_time = (int32_t)stream_read_uint32(&ctx->stream);
            stream_skip(&ctx->stream, 4); /* media_rate (16.16) */
            size_remaining -= 12;
        }
        else
        {
            stream_skip(&ctx->stream, 8); /* segment_duration (64-bit) */
            stream_skip(&ctx->stream, 4); /* media_time high */
            media_time = (int32_t)stream_read_uint32(&ctx->stream);
            stream_skip(&ctx->stream, 4); /* media_rate */
            size_remaining -= 20;
        }

        /* First non-empty edit: media_time > 0 = priming offset */
        /* Save to pending — committed at end of trak once type is known.
         * edts comes before mdia, so track type isn't known yet. */
        if (i == 0 && media_time > 0)
            ctx->pending_lead_trim = (uint32_t)media_time;
    }
    }

    if (size_remaining > 0)
        stream_skip(&ctx->stream, size_remaining);

    return true;
}

/* edts container — walk for elst */
static bool read_chunk_edts(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t size_remaining = chunk_len - 8;

    while (size_remaining > 8)
    {
        uint32_t sub_len = stream_read_uint32(&ctx->stream);
        uint32_t sub_id  = stream_read_uint32(&ctx->stream);

        if (sub_len <= 1 || sub_len > size_remaining)
            return false;

        if (sub_id == MAKEFOURCC('e','l','s','t'))
        {
            if (!read_chunk_elst(ctx, sub_len))
                return false;
        }
        else
        {
            stream_skip(&ctx->stream, sub_len - 8);
        }

        size_remaining -= sub_len;
    }

    return true;
}

/* trak - track container */
static bool read_chunk_trak(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t size_remaining = chunk_len - 8;

    /* Reset per-track state (pending fields MUST be reset to prevent
     * stale values from one trak leaking into the next) */
    ctx->in_video_trak = false;
    ctx->in_audio_trak = false;
    ctx->pending_timescale = 0;
    ctx->pending_lead_trim = 0;

    while (size_remaining > 8)
    {
        uint32_t sub_len = stream_read_uint32(&ctx->stream);
        uint32_t sub_id  = stream_read_uint32(&ctx->stream);

        if (sub_len <= 1 || sub_len > size_remaining)
            return false;

        switch (sub_id)
        {
            case MAKEFOURCC('t','k','h','d'):
                if (!read_chunk_tkhd(ctx, sub_len))
                    return false;
                break;
            case MAKEFOURCC('m','d','i','a'):
                if (!read_chunk_mdia(ctx, sub_len))
                    return false;
                break;
            case MAKEFOURCC('e','d','t','s'):
                /* edts comes before mdia — track type unknown yet.
                 * Parse unconditionally, save to pending_lead_trim.
                 * Committed to audio_lead_trim at end of trak. */
                if (ctx->want_audio)
                {
                    if (!read_chunk_edts(ctx, sub_len))
                        return false;
                }
                else
                {
                    stream_skip(&ctx->stream, sub_len - 8);
                }
                break;
            default:
                stream_skip(&ctx->stream, sub_len - 8);
                break;
        }

        size_remaining -= sub_len;
    }

    /* Commit pending per-trak fields now that track type is known.
     * mdhd timescale and edts/elst are parsed before hdlr identifies
     * the track type, so they were saved to pending fields above. */
    if (ctx->in_video_trak && ctx->res->format != 0)
    {
        ctx->found_video = true;
        ctx->res->display_width = ctx->tkhd_dw;
        ctx->res->display_height = ctx->tkhd_dh;
        ctx->res->timescale = ctx->pending_timescale;
    }

    if (ctx->in_audio_trak && ctx->res->audio_format != 0)
    {
        ctx->found_audio = true;
        ctx->res->audio_timescale = ctx->pending_timescale;
        ctx->res->audio_lead_trim = ctx->pending_lead_trim;
    }

    return true;
}

/* Walk a generic box container, skipping all children except the target.
 * When found, records the file offset + size of the target's data payload. */
static void walk_for_covr(struct mp4_parse_ctx *ctx, size_t chunk_len,
                          int depth)
{
    size_t size_remaining = chunk_len - 8;

    /* depth 0 = udta, 1 = meta, 2 = ilst, 3 = covr */

    if (depth == 1)
    {
        /* meta has a 4-byte version/flags before children */
        stream_read_uint32(&ctx->stream);
        size_remaining -= 4;
    }

    while (size_remaining > 8)
    {
        uint32_t sub_len = stream_read_uint32(&ctx->stream);
        uint32_t sub_id  = stream_read_uint32(&ctx->stream);

        if (sub_len <= 1 || sub_len > size_remaining)
            break;

        bool descend = false;

        if (depth == 0 && sub_id == MAKEFOURCC('m','e','t','a'))
            descend = true;
        else if (depth == 1 && sub_id == MAKEFOURCC('i','l','s','t'))
            descend = true;
        else if (depth == 2 && sub_id == MAKEFOURCC('c','o','v','r'))
            descend = true;

        if (descend && depth < 3)
        {
            walk_for_covr(ctx, sub_len, depth + 1);
        }
        else if (depth == 3 && sub_id == MAKEFOURCC('d','a','t','a')
                 && sub_len >= 24)
        {
            /* covr/data: version+flags(4) + locale(4) + image_data */
            uint32_t data_type = stream_read_uint32(&ctx->stream);
            stream_skip(&ctx->stream, 4); /* locale */
            ctx->res->cover_type = (uint8_t)(data_type & 0xFF);
            ctx->res->cover_offset = (uint32_t)stream_tell(&ctx->stream);
            ctx->res->cover_size = sub_len - 8 - 8;
            stream_skip(&ctx->stream, sub_len - 8 - 8);
        }
        else
        {
            stream_skip(&ctx->stream, sub_len - 8);
        }

        size_remaining -= sub_len;
    }
}

/* moov - movie container */
static bool read_chunk_moov(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t size_remaining = chunk_len - 8;

    while (size_remaining > 8)
    {
        uint32_t sub_len = stream_read_uint32(&ctx->stream);
        uint32_t sub_id  = stream_read_uint32(&ctx->stream);

        if (sub_len <= 1 || sub_len > size_remaining)
            return false;

        switch (sub_id)
        {
            case MAKEFOURCC('t','r','a','k'):
                /* Parse until we have both video and audio (or all traks) */
                if (!ctx->found_video || (ctx->want_audio && !ctx->found_audio))
                {
                    if (!read_chunk_trak(ctx, sub_len))
                        return false;
                }
                else
                {
                    stream_skip(&ctx->stream, sub_len - 8);
                }
                break;
            case MAKEFOURCC('u','d','t','a'):
                /* Walk udta → meta → ilst → covr for cover art */
                walk_for_covr(ctx, sub_len, 0);
                break;
            default:
                stream_skip(&ctx->stream, sub_len - 8);
                break;
        }

        size_remaining -= sub_len;
    }

    return true;
}

int mp4v_demux_open(const char *filepath,
                    struct mp4v_demux_res *res,
                    uint32_t *sample_buf, uint32_t sample_cap,
                    uint32_t *chunk_buf, uint32_t chunk_cap,
                    uint32_t *audio_sample_buf, uint32_t audio_sample_cap,
                    uint32_t *audio_chunk_buf, uint32_t audio_chunk_cap)
{
    struct mp4_parse_ctx ctx;
    int fd;

    fd = open(filepath, O_RDONLY);
    if (fd < 0)
        return -1;

    memset(res, 0, sizeof(*res));
    res->sample_sizes = sample_buf;
    res->sample_sizes_cap = sample_cap;
    res->chunk_offsets = chunk_buf;
    res->chunk_offsets_cap = chunk_cap;
    res->audio_sample_sizes = audio_sample_buf;
    res->audio_sample_sizes_cap = audio_sample_cap;
    res->audio_chunk_offsets = audio_chunk_buf;
    res->audio_chunk_offsets_cap = audio_chunk_cap;

    memset(&ctx, 0, sizeof(ctx));
    stream_init(&ctx.stream, fd);
    ctx.res = res;
    ctx.want_audio = (audio_sample_buf != NULL && audio_chunk_buf != NULL);

    /* Walk top-level boxes */
    while (!ctx.stream.eof)
    {
        uint32_t chunk_len = stream_read_uint32(&ctx.stream);
        uint32_t chunk_id;

        if (ctx.stream.eof)
            break;

        if (chunk_len == 1)
        {
            /* 64-bit extended size — not supported */
            close(fd);
            return -1;
        }

        chunk_id = stream_read_uint32(&ctx.stream);

        switch (chunk_id)
        {
            case MAKEFOURCC('f','t','y','p'):
                stream_skip(&ctx.stream, chunk_len - 8);
                break;
            case MAKEFOURCC('m','o','o','v'):
                if (!read_chunk_moov(&ctx, chunk_len))
                {
                    close(fd);
                    return -1;
                }
                break;
            case MAKEFOURCC('m','d','a','t'):
                if (chunk_len > 8)
                {
                    res->mdat_offset = (uint32_t)stream_tell(&ctx.stream);
                    res->mdat_len = chunk_len - 8;
                }
                /* If we already found everything we need, we're done */
                if (ctx.found_video && (!ctx.want_audio || ctx.found_audio))
                    goto done;
                stream_skip(&ctx.stream, chunk_len - 8);
                break;
            default:
                stream_skip(&ctx.stream, chunk_len - 8);
                break;
        }
    }

done:
    close(fd);

    if (!ctx.found_video || res->format == 0)
        return -1;

    return 0;
}

int mp4v_get_sample_offset(const struct mp4v_demux_res *res,
                           uint32_t sample_index,
                           uint32_t *offset_out,
                           uint32_t *size_out)
{
    uint32_t chunk_index = 0;
    uint32_t sample_in_chunk = 0;
    uint64_t samples_so_far = 0;
    uint32_t i;
    uint32_t chunk_offset;

    if (sample_index >= res->num_samples ||
        sample_index >= res->sample_sizes_cap)
        return -1;

    *size_out = res->sample_sizes[sample_index];

    /* Walk stsc to find which chunk contains this sample */
    for (i = 0; i < res->num_stsc; i++)
    {
        uint32_t first_chunk = res->stsc[i].first_chunk - 1; /* 1-based → 0-based */
        uint32_t spc = res->stsc[i].samples_per_chunk;
        uint32_t next_first;

        if (i + 1 < res->num_stsc)
            next_first = res->stsc[i + 1].first_chunk - 1;
        else
            next_first = res->num_stco;

        uint32_t chunks_in_run = next_first - first_chunk;
        uint64_t samples_in_run = (uint64_t)chunks_in_run * spc;

        if (samples_so_far + samples_in_run > sample_index)
        {
            /* Sample is within this stsc run */
            uint32_t sample_offset_in_run = sample_index - samples_so_far;
            chunk_index = first_chunk + (sample_offset_in_run / spc);
            sample_in_chunk = sample_offset_in_run % spc;
            break;
        }

        samples_so_far += samples_in_run;
    }

    if (chunk_index >= res->chunk_offsets_cap)
        return -1;

    /* Get the chunk's file offset */
    chunk_offset = res->chunk_offsets[chunk_index];

    /* Add offsets for preceding samples within this chunk */
    {
        uint32_t first_sample_in_chunk = sample_index - sample_in_chunk;
        uint32_t j;

        for (j = 0; j < sample_in_chunk; j++)
        {
            if (first_sample_in_chunk + j < res->num_samples)
                chunk_offset += res->sample_sizes[first_sample_in_chunk + j];
        }
    }

    *offset_out = chunk_offset;
    return 0;
}

int mp4v_get_audio_sample_offset(const struct mp4v_demux_res *res,
                                 uint32_t sample_index,
                                 uint32_t *offset_out,
                                 uint32_t *size_out)
{
    uint32_t chunk_index = 0;
    uint32_t sample_in_chunk = 0;
    uint64_t samples_so_far = 0;
    uint32_t i;
    uint32_t chunk_offset;

    if (sample_index >= res->audio_num_samples ||
        sample_index >= res->audio_sample_sizes_cap)
        return -1;

    *size_out = res->audio_sample_sizes[sample_index];

    for (i = 0; i < res->audio_num_stsc; i++)
    {
        uint32_t first_chunk = res->audio_stsc[i].first_chunk - 1;
        uint32_t spc = res->audio_stsc[i].samples_per_chunk;
        uint32_t next_first;

        if (i + 1 < res->audio_num_stsc)
            next_first = res->audio_stsc[i + 1].first_chunk - 1;
        else
            next_first = res->audio_num_stco;

        uint32_t chunks_in_run = next_first - first_chunk;
        uint64_t samples_in_run = (uint64_t)chunks_in_run * spc;

        if (samples_so_far + samples_in_run > sample_index)
        {
            uint32_t sample_offset_in_run = sample_index - samples_so_far;
            chunk_index = first_chunk + (sample_offset_in_run / spc);
            sample_in_chunk = sample_offset_in_run % spc;
            break;
        }

        samples_so_far += samples_in_run;
    }

    if (chunk_index >= res->audio_chunk_offsets_cap)
        return -1;

    chunk_offset = res->audio_chunk_offsets[chunk_index];

    {
        uint32_t first_sample_in_chunk = sample_index - sample_in_chunk;
        uint32_t j;

        for (j = 0; j < sample_in_chunk; j++)
        {
            if (first_sample_in_chunk + j < res->audio_num_samples)
                chunk_offset += res->audio_sample_sizes[first_sample_in_chunk + j];
        }
    }

    *offset_out = chunk_offset;
    return 0;
}

bool mp4v_is_keyframe(const struct mp4v_demux_res *res,
                      uint32_t sample_index)
{
    uint32_t i;
    uint32_t sample_num = sample_index + 1; /* stss is 1-based */

    /* No stss table means all samples are sync samples */
    if (res->num_stss == 0)
        return true;

    for (i = 0; i < res->num_stss; i++)
    {
        if (res->stss[i] == sample_num)
            return true;
        if (res->stss[i] > sample_num)
            break;
    }

    return false;
}
