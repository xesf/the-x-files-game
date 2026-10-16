/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf
 * Based on FFmpeg's QDMC decoder, Copyright (c) 2017 Paul B Mahol
 * (LGPL-2.1-or-later). */
/* QDesign Music 1 ('QDMC') sound: the credits music, the establishing
 * shots' music and the songs on Willmore's stereo. A packet decodes to
 * one frame of 2048 to 8192 samples per channel, built from noise bands
 * and tones through an inverse FFT. */
#ifndef XF_QDMC_H
#define XF_QDMC_H

#include "xf_types.h"

typedef struct xf_qdmc xf_qdmc;

/* Reads the decoder settings (the 'QDCA' atom) from a sound sample
 * description. Returns NULL when they are missing or not supported. */
xf_qdmc *xf_qdmc_open(const u8 *desc, u32 desc_size);
void xf_qdmc_close(xf_qdmc *q);
int xf_qdmc_channels(const xf_qdmc *q);
/* Samples per channel in one packet. */
u32 xf_qdmc_frame_samples(const xf_qdmc *q);
/* Bytes in one packet. */
u32 xf_qdmc_packet_bytes(const xf_qdmc *q);
/* Decodes one packet into frame_samples interleaved samples. On a damaged
 * packet the output is silence, the decoder starts afresh and the result
 * is -1. */
int xf_qdmc_decode(xf_qdmc *q, const u8 *packet, u32 size, i16 *out);
/* Forgets the sound before a seek. */
void xf_qdmc_reset(xf_qdmc *q);

#endif
