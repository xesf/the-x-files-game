/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* WAVE sound files of the earlier HyperBole games: PCM, 8-bit unsigned or
 * 16-bit signed, mono or stereo. */
#ifndef XF_WAV_H
#define XF_WAV_H

#include "xf_types.h"

typedef struct {
    i16 *samples;   /* interleaved */
    u32 frames;
    int channels;
    u32 rate;       /* Hz */
} xf_wav;

/* Returns 0, or -1 for data this reader does not handle. */
int xf_wav_decode(const u8 *data, size_t size, xf_wav *out);
int xf_wav_load(const char *path, xf_wav *out);
void xf_wav_free(xf_wav *w);

#endif
