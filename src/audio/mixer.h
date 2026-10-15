/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* Software mixer. Each channel has a ring buffer that the game thread
 * fills with 16-bit PCM at the source rate; the audio thread resamples
 * and mixes all channels to stereo at the output rate. */
#ifndef XF_MIXER_H
#define XF_MIXER_H

#include "xf_types.h"

#define XF_MIXER_CHANNELS 8

typedef void (*xf_lock_fn)(void);

void xf_mixer_init(int out_rate, xf_lock_fn lock, xf_lock_fn unlock);
/* Opens a channel for mono or stereo PCM at `rate`. Returns the channel
 * number, or -1 when all channels are busy. */
int xf_mixer_open(int rate, int channels);
/* Stops a channel at once and frees it. */
void xf_mixer_close(int ch);
/* Free space in the ring buffer, in source frames. */
u32 xf_mixer_space(int ch);
/* Queues frames; returns how many fit. */
u32 xf_mixer_write(int ch, const i16 *pcm, u32 frames);
/* Marks the end of the data: the channel frees itself once drained. */
void xf_mixer_finish(int ch);
/* Source frames played so far. */
u64 xf_mixer_played(int ch);
/* 1 while the channel still has data to play. */
int xf_mixer_active(int ch);
void xf_mixer_set_volume(int ch, int volume); /* 0 to 256 */

/* Called by the platform audio callback (with the audio lock held). */
void xf_mixer_render(void *user, i16 *out, int frames);

#endif
