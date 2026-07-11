/***************************************************************************
 * Video player audio decode thread.
 *
 * Decodes AAC audio via libfaad on a dedicated thread, feeds PCM ring buffer.
 * Uses audio-master clock from video_pcm for A/V sync.
 ********************************************************************* * Copyright (C) 2025 Nux Li
 *
 *******/
#ifndef VIDEO_AUDIO_H
#define VIDEO_AUDIO_H

#include <stdint.h>
#include <stdbool.h>
#include "mp4_demux.h"

/* Initialize audio decoder and thread.
 * filepath: MP4 file path (opens its own fd for independent seeks)
 * demux: parsed MP4 with audio track data (audio_format != 0)
 * Returns 0 on success, -1 on error. */
int video_audio_init(const char *filepath,
                     const struct mp4v_demux_res *demux);

/* Start audio playback (begins decoding and filling PCM buffer). */
void video_audio_play(void);

/* Pause audio decode (PCM buffer drains, then silence). */
void video_audio_pause(void);

/* Resume audio decode after pause. */
void video_audio_resume(void);

/* Seek audio to target time in milliseconds. Flushes PCM buffer. */
void video_audio_seek(uint32_t target_ms);

/* Stop audio thread and release resources. Blocks until thread exits. */
void video_audio_stop(void);

/* Check if audio thread has pre-filled enough PCM data to start. */
bool video_audio_ready(void);

/* Check if audio thread is actively decoding (false after EOF or error). */
bool video_audio_is_active(void);

#endif /* VIDEO_AUDIO_H */
