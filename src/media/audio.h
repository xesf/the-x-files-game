/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* Decodes the sound track of a QuickTime movie to 16-bit PCM, one chunk
 * at a time. Supports 'ima4' (IMA ADPCM) and 'twos' (signed PCM). */
#ifndef XF_AUDIO_H
#define XF_AUDIO_H

#include "media/mov.h"

typedef struct {
    int predictor, step_index;
} xf_ima_state;

typedef struct {
    xf_mov *mov;
    const xf_mov_track *track;
    u32 chunk;          /* next chunk to decode */
    xf_ima_state ima[2];
    u8 *buf;
    size_t buf_size;
} xf_audio_stream;

int xf_audio_supported(const xf_mov_track *t);
int xf_audio_open(xf_audio_stream *s, xf_mov *mov, const xf_mov_track *t);
void xf_audio_close(xf_audio_stream *s);
/* Largest number of frames one chunk can produce. */
u32 xf_audio_max_chunk_frames(const xf_audio_stream *s);
/* Decodes the next chunk into `out` (interleaved, track channel count).
 * Returns the number of frames, 0 at the end, or -1 on error. */
int xf_audio_next(xf_audio_stream *s, i16 *out);
/* Restarts decoding at the chunk that holds `frame`. Returns the number
 * of frames to drop from the next chunk to reach `frame`. */
u32 xf_audio_seek(xf_audio_stream *s, u32 frame);

/* Decodes one 34-byte QuickTime IMA packet into 64 samples. */
void xf_ima4_packet(xf_ima_state *st, const u8 *packet, i16 *out, int stride);

#endif
