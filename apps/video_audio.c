/***************************************************************************
 * Video player audio decode thread.
 *
 * Decodes AAC audio from MP4 files via libfaad, outputs to video_pcm ring
 * buffer. Runs on a dedicated thread created via create_thread().
 *
 * Architecture:
 * - Main thread sends messages (play/pause/seek/quit) via event queue
 * - Audio thread decodes AAC frames, converts Q17.14 to int16, writes PCM
 * - DMA callback in video_pcm pulls from ring buffer (ISR context)
 * - Audio clock in video_pcm is the master for A/V sync
 ****************************************************************************/
#include "config.h"

#ifdef IPOD_6G

#include "system.h"
#include "kernel.h"
#include "thread.h"
#include "file.h"
#include "video_audio.h"
#include "video_pcm.h"
#include "mp4_demux.h"

/* libfaad headers */
#include "common.h"
#include "structs.h"
#include "decoder.h"

#include <string.h>

#define CLIP(x, lo, hi) (MAX(MIN((x), (hi)), (lo)))

/* Thread stack: 12KB — libfaad decode uses 8-16KB of stack */
#define AUDIO_STACK_SIZE  (12 * 1024)
static uint8_t audio_stack[AUDIO_STACK_SIZE];
static unsigned int audio_thread_id;

/* Event queue for main→audio thread communication */
static struct event_queue audio_queue;

enum {
    AUDIO_MSG_PLAY = 1,
    AUDIO_MSG_PAUSE,
    AUDIO_MSG_RESUME,
    AUDIO_MSG_SEEK,
    AUDIO_MSG_QUIT
};

/* Shared state */
static const struct mp4v_demux_res *audio_demux;
static int audio_fd;
static uint32_t audio_sample_idx;
static uint32_t audio_sample_rate;
static uint32_t audio_lead_trim;
static volatile bool audio_is_ready;
static volatile bool audio_is_playing;

/* Read buffer for AAC frames (max AAC frame ~2KB, 8KB generous) */
#define AUDIO_READ_BUF_SIZE  8192
static uint8_t audio_read_buf[AUDIO_READ_BUF_SIZE];

/* PCM conversion buffer: 2048 stereo samples max (AAC-LC=1024, HE-AAC=2048) */
#define PCM_CONV_BUF_SIZE  4096
static int16_t pcm_conv_buf[PCM_CONV_BUF_SIZE];

/* Convert one decoded AAC frame from FAAD Q17.14 to interleaved int16 PCM.
 * FAAD outputs non-interleaved int32 (real_t) in time_out[ch][sample].
 * REAL_BITS=14 (fractional bits). Conversion: >> 14 with rounding.
 * Verified against output.c:499, DSP output_scale=14, Q format definition. */
static int convert_frame_to_pcm(NeAACDecStruct *decoder,
                                NeAACDecFrameInfo *info,
                                uint32_t skip)
{
    uint32_t frame_samples = info->samples / info->channels;
    int32_t *left, *right;
    uint32_t i;
    int out_samples;

    if (skip >= frame_samples)
        return 0;

    out_samples = (int)(frame_samples - skip);
    if (out_samples > PCM_CONV_BUF_SIZE / 2)
        out_samples = PCM_CONV_BUF_SIZE / 2;

    left = decoder->time_out[0] + skip;
    right = (info->channels > 1)
          ? decoder->time_out[1] + skip : left;

    for (i = 0; i < (uint32_t)out_samples; i++)
    {
        int32_t l = (left[i] + (1 << 13)) >> 14;
        int32_t r = (right[i] + (1 << 13)) >> 14;
        pcm_conv_buf[i * 2]     = (int16_t)CLIP(l, -32768, 32767);
        pcm_conv_buf[i * 2 + 1] = (int16_t)CLIP(r, -32768, 32767);
    }

    return out_samples;
}

/* Find audio sample index for a given time in milliseconds */
static uint32_t audio_sample_for_time(uint32_t target_ms)
{
    uint64_t target_ticks = (uint64_t)target_ms
                          * audio_demux->audio_timescale / 1000;
    uint64_t acc = 0;
    uint32_t sample = 0;
    uint32_t i;

    for (i = 0; i < audio_demux->audio_num_stts
                && sample < audio_demux->audio_num_samples; i++)
    {
        uint64_t run = (uint64_t)audio_demux->audio_stts[i].sample_count
                     * audio_demux->audio_stts[i].sample_delta;
        if (acc + run > target_ticks)
        {
            sample += (uint32_t)((target_ticks - acc)
                                / audio_demux->audio_stts[i].sample_delta);
            break;
        }
        acc += run;
        sample += audio_demux->audio_stts[i].sample_count;
    }

    if (sample >= audio_demux->audio_num_samples)
        sample = audio_demux->audio_num_samples > 0
               ? audio_demux->audio_num_samples - 1 : 0;

    return sample;
}

static void audio_decode_thread(void)
{
    NeAACDecHandle decoder;
    NeAACDecFrameInfo frame_info;
    uint32_t sample_rate_out;
    uint8_t channels_out;
    struct queue_event ev;
    bool running = true;
    bool playing = false;

    /* Init libfaad (uses static allocation — no heap needed) */
    decoder = NeAACDecOpen();
    if (!decoder)
        goto thread_exit;

    if (NeAACDecInit2(decoder,
                      (uint8_t *)audio_demux->audio_codecdata,
                      audio_demux->audio_codecdata_len,
                      &sample_rate_out, &channels_out) < 0)
    {
        goto thread_exit;
    }

    audio_sample_rate = sample_rate_out;

    while (running)
    {
        /* Wait for messages (blocking when paused, polling when playing) */
        if (playing)
        {
            /* Non-blocking: check for message, default to decode */
            if (queue_peek(&audio_queue, &ev))
                queue_wait(&audio_queue, &ev); /* consume it */
            else
                ev.id = SYS_TIMEOUT;
        }
        else
        {
            queue_wait(&audio_queue, &ev);
        }

        switch (ev.id)
        {
            case AUDIO_MSG_PLAY:
                playing = true;
                audio_is_playing = true;
                break;

            case AUDIO_MSG_PAUSE:
                playing = false;
                audio_is_playing = false;
                break;

            case AUDIO_MSG_RESUME:
                playing = true;
                audio_is_playing = true;
                break;

            case AUDIO_MSG_SEEK:
            {
                uint32_t target_ms = (uint32_t)ev.data;
                audio_sample_idx = audio_sample_for_time(target_ms);
                video_pcm_flush(target_ms, audio_sample_rate);
                audio_is_ready = false;
                /* Re-apply lead_trim when seeking to start */
                if (target_ms == 0)
                    audio_lead_trim = audio_demux->audio_lead_trim;
                else
                    audio_lead_trim = 0;
                NeAACDecPostSeekReset(decoder, (int32_t)audio_sample_idx);
                break;
            }

            case AUDIO_MSG_QUIT:
                running = false;
                continue;

            case SYS_TIMEOUT:
                /* Fall through to decode */
                break;

            default:
                break;
        }

        if (!playing || !running)
            continue;

        /* Check if we've reached the end of audio samples */
        if (audio_sample_idx >= audio_demux->audio_num_samples)
        {
            /* Audio EOF — keep thread alive but stop decoding */
            playing = false;
            audio_is_playing = false;
            continue;
        }

        /* Wait if PCM buffer is full */
        if (!video_pcm_has_space(1024))
        {
            sleep(1); /* ~10ms */
            continue;
        }

        /* Read one AAC frame from MP4 */
        {
            uint32_t offset, size;
            if (mp4v_get_audio_sample_offset(audio_demux,
                                             audio_sample_idx,
                                             &offset, &size) < 0)
            {
                audio_sample_idx++;
                continue;
            }

            if (size > AUDIO_READ_BUF_SIZE)
                size = AUDIO_READ_BUF_SIZE;

            lseek(audio_fd, offset, SEEK_SET);
            if (read(audio_fd, audio_read_buf, size) != (ssize_t)size)
            {
                audio_sample_idx++;
                continue;
            }

            /* Decode AAC frame */
            NeAACDecDecode(decoder, &frame_info,
                           audio_read_buf, size);

            if (frame_info.error != 0 || frame_info.samples == 0)
            {
                audio_sample_idx++;
                continue;
            }

            /* Convert and write to PCM ring buffer */
            {
                uint32_t skip = 0;
                int pcm_samples;

                /* Handle lead_trim (encoder priming) */
                if (audio_lead_trim > 0)
                {
                    uint32_t frame_samples = frame_info.samples
                                           / frame_info.channels;
                    if (audio_lead_trim >= frame_samples)
                    {
                        audio_lead_trim -= frame_samples;
                        audio_sample_idx++;
                        continue;
                    }
                    skip = audio_lead_trim;
                    audio_lead_trim = 0;
                }

                pcm_samples = convert_frame_to_pcm(
                    (NeAACDecStruct *)decoder, &frame_info, skip);

                if (pcm_samples > 0)
                    video_pcm_write(pcm_conv_buf, pcm_samples);
            }

            audio_sample_idx++;

            /* Signal ready after pre-filling ~200ms */
            if (!audio_is_ready && !video_pcm_has_space(
                    (int)(audio_sample_rate * 200 / 1000)))
            {
                audio_is_ready = true;
            }
        }

        yield();
    }

thread_exit:
    thread_exit();
}

int video_audio_init(const char *filepath,
                     const struct mp4v_demux_res *demux)
{
    if (demux->audio_format == 0 || demux->audio_codecdata_len == 0)
        return -1;

    audio_demux = demux;
    audio_sample_idx = 0;
    audio_sample_rate = demux->audio_sample_rate;
    audio_lead_trim = demux->audio_lead_trim;
    audio_is_ready = false;
    audio_is_playing = false;

    /* Open separate fd for independent seeks */
    audio_fd = open(filepath, O_RDONLY);
    if (audio_fd < 0)
        return -1;

    /* Init PCM output */
    video_pcm_init(audio_sample_rate);

    /* Create event queue (private, no system broadcasts) */
    queue_init(&audio_queue, false);

    /* Create decode thread */
    audio_thread_id = create_thread(
        audio_decode_thread, audio_stack, sizeof(audio_stack),
        0, "video_audio"
        IF_PRIO(, PRIORITY_PLAYBACK)
        IF_COP(, CPU));

    return 0;
}

void video_audio_play(void)
{
    queue_post(&audio_queue, AUDIO_MSG_PLAY, 0);
}

void video_audio_pause(void)
{
    queue_post(&audio_queue, AUDIO_MSG_PAUSE, 0);
}

void video_audio_resume(void)
{
    queue_post(&audio_queue, AUDIO_MSG_RESUME, 0);
}

void video_audio_seek(uint32_t target_ms)
{
    queue_post(&audio_queue, AUDIO_MSG_SEEK, (intptr_t)target_ms);
}

void video_audio_stop(void)
{
    queue_post(&audio_queue, AUDIO_MSG_QUIT, 0);
    thread_wait(audio_thread_id);

    video_pcm_stop();

    if (audio_fd >= 0)
    {
        close(audio_fd);
        audio_fd = -1;
    }

    queue_delete(&audio_queue);
    audio_demux = NULL;
}

bool video_audio_ready(void)
{
    return audio_is_ready;
}

#endif /* IPOD_6G */
