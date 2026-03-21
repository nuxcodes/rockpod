/***************************************************************************
 * PCM ring buffer for video player audio output.
 *
 * Simplified version of mpegplayer's pcm_output.c.
 * Audio-master clock: DMA callback advances clock_samples monotonically.
 ****************************************************************************/
#ifndef VIDEO_PCM_H
#define VIDEO_PCM_H

#include <stdint.h>
#include <stdbool.h>

/* Initialize PCM output at the given sample rate.
 * Saves and changes mixer frequency. */
void video_pcm_init(uint32_t sample_rate);

/* Write interleaved stereo int16 PCM to the ring buffer.
 * Returns number of stereo samples actually written (may be less if full). */
int video_pcm_write(const int16_t *pcm, int stereo_samples);

/* Get audio master clock: total stereo samples played by DMA. */
uint64_t video_pcm_get_clock(void);

/* Flush ring buffer and reset clock (for seek). */
void video_pcm_flush(void);

/* Pause/resume PCM output. */
void video_pcm_pause(bool pause);

/* Stop PCM output and restore original mixer frequency. */
void video_pcm_stop(void);

/* Check if ring buffer has space for at least n stereo samples. */
bool video_pcm_has_space(int stereo_samples);

/* Check if ring buffer is empty (all data played). */
bool video_pcm_empty(void);

#endif /* VIDEO_PCM_H */
