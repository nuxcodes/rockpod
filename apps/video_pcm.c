/***************************************************************************
 * PCM ring buffer for video player audio output.
 *
 * Simplified version of mpegplayer's pcm_output.c.
 * Single-producer (audio decode thread), single-consumer (DMA ISR).
 * Audio-master clock via monotonic sample counter.
 *
 * CRITICAL: get_more() runs in DMA ISR context — no yield, no sleep,
 * no mutex, no file I/O. Only reads from pre-filled ring buffer.
 ****************************************************************************/
#include "config.h"

#ifdef IPOD_6G

#include "system.h"
#include "kernel.h"
#include "pcm.h"
#include "pcm_mixer.h"
#include "pcm_sampr.h"
#include "video_pcm.h"

#include <string.h>

/* Ring buffer: 64KB = ~0.74s at 44100 Hz stereo 16-bit */
#define PCM_BUF_SAMPLES  16384  /* stereo samples (= 64KB / 4 bytes) */

/* Silence clip for underrun (256 stereo samples = ~5.8ms at 44.1kHz) */
#define SILENCE_SAMPLES   256

static int16_t pcm_buffer[PCM_BUF_SAMPLES * 2] CACHEALIGN_ATTR;
static int16_t silence[SILENCE_SAMPLES * 2];

/* Read/write positions in stereo samples (wrap via modulo) */
static volatile uint32_t pcm_read_pos;
static volatile uint32_t pcm_write_pos;

/* Master clock: stereo samples consumed by DMA (uint32_t sufficient
 * for >27 hours at 44.1kHz; avoids torn reads on 32-bit ARM) */
static volatile uint32_t clock_samples;

/* Clock base: absolute time (ms) at last flush/init */
static uint32_t clock_base_ms;
static uint32_t clock_sample_rate;

/* Flush guard: prevents write during flush (set by main, checked by audio) */
static volatile bool flush_pending;

/* Saved mixer frequency for restore on stop */
static unsigned int saved_sampr;

static uint32_t pcm_used(void)
{
    return pcm_write_pos - pcm_read_pos;
}

static uint32_t pcm_free(void)
{
    return PCM_BUF_SAMPLES - pcm_used();
}

/* DMA callback — runs in ISR context.
 * Pulls from ring buffer, advances clock. On underrun, plays silence. */
static void video_pcm_get_more(const void **start, size_t *size)
{
    uint32_t avail = pcm_used();

    if (avail > 0)
    {
        uint32_t rd = pcm_read_pos % PCM_BUF_SAMPLES;
        uint32_t chunk = avail;

        /* Don't wrap around the buffer end */
        if (rd + chunk > PCM_BUF_SAMPLES)
            chunk = PCM_BUF_SAMPLES - rd;

        /* Limit chunk size to avoid hogging DMA */
        if (chunk > 2048)
            chunk = 2048;

        *start = &pcm_buffer[rd * 2];
        *size = chunk * 4; /* stereo 16-bit = 4 bytes per sample */
        pcm_read_pos += chunk;
        clock_samples += chunk;
    }
    else
    {
        /* Underrun: play silence. Do NOT advance clock —
         * advancing would create a permanent A/V offset because the
         * audio content hasn't actually been played yet. */
        *start = silence;
        *size = sizeof(silence);
    }
}

void video_pcm_init(uint32_t sample_rate)
{
    /* Save current frequency for restore */
    saved_sampr = mixer_get_frequency();

    /* Reset buffer state */
    pcm_read_pos = 0;
    pcm_write_pos = 0;
    clock_samples = 0;
    clock_base_ms = 0;
    clock_sample_rate = sample_rate;
    flush_pending = false;
    memset(silence, 0, sizeof(silence));

    /* Set sample rate BEFORE starting playback
     * (mixer_set_frequency calls mixer_reset which stops all channels) */
    mixer_set_frequency(sample_rate);

    /* Start playback with DMA callback */
    mixer_channel_set_amplitude(PCM_MIXER_CHAN_PLAYBACK, MIX_AMP_UNITY);
    mixer_channel_play_data(PCM_MIXER_CHAN_PLAYBACK,
                            video_pcm_get_more, NULL, 0);
}

int video_pcm_write(const int16_t *pcm, int stereo_samples)
{
    int written = 0;

    /* Bail if flush is in progress (prevents write/flush race) */
    if (flush_pending)
        return 0;

    while (written < stereo_samples)
    {
        uint32_t space = pcm_free();
        if (space == 0 || flush_pending)
            break;

        uint32_t wr = pcm_write_pos % PCM_BUF_SAMPLES;
        uint32_t chunk = stereo_samples - written;

        if (chunk > space)
            chunk = space;

        /* Don't wrap around the buffer end */
        if (wr + chunk > PCM_BUF_SAMPLES)
            chunk = PCM_BUF_SAMPLES - wr;

        memcpy(&pcm_buffer[wr * 2], &pcm[written * 2], chunk * 4);
        pcm_write_pos += chunk;
        written += chunk;
    }

    return written;
}

uint32_t video_pcm_get_clock_ms(void)
{
    uint32_t samples = clock_samples; /* atomic 32-bit read on ARM */
    if (clock_sample_rate == 0)
        return clock_base_ms;
    return clock_base_ms + (uint32_t)((uint64_t)samples * 1000
                                     / clock_sample_rate);
}

void video_pcm_flush(uint32_t base_ms, uint32_t sample_rate)
{
    /* Block the write side first to prevent race with audio thread */
    flush_pending = true;

    /* Stop mixer channel to invalidate any stale buffer pointer,
     * then reset state, then restart (mpegplayer pattern). */
    pcm_play_lock();
    mixer_channel_stop(PCM_MIXER_CHAN_PLAYBACK);
    pcm_play_unlock();

    pcm_read_pos = 0;
    pcm_write_pos = 0;
    clock_samples = 0;
    clock_base_ms = base_ms;
    clock_sample_rate = sample_rate;

    /* Restart mixer with fresh callback */
    mixer_channel_set_amplitude(PCM_MIXER_CHAN_PLAYBACK, MIX_AMP_UNITY);
    mixer_channel_play_data(PCM_MIXER_CHAN_PLAYBACK,
                            video_pcm_get_more, NULL, 0);

    flush_pending = false;
}

void video_pcm_pause(bool pause)
{
    mixer_channel_play_pause(PCM_MIXER_CHAN_PLAYBACK, !pause);
}

void video_pcm_stop(void)
{
    flush_pending = false;
    mixer_channel_stop(PCM_MIXER_CHAN_PLAYBACK);

    /* Restore original sample rate */
    if (saved_sampr != 0)
        mixer_set_frequency(saved_sampr);
}

bool video_pcm_has_space(int stereo_samples)
{
    return pcm_free() >= (uint32_t)stereo_samples;
}

bool video_pcm_empty(void)
{
    return pcm_used() == 0;
}

uint32_t video_pcm_buffered_samples(void)
{
    return pcm_used();
}

#endif /* IPOD_6G */
