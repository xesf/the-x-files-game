/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
#include "audio.h"

#include <stdlib.h>
#include <string.h>

#include "xf_endian.h"

#define IMA_PACKET_BYTES 34
#define IMA_PACKET_FRAMES 64

static const i16 g_ima_steps[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552,
    1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
    7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385,
    24623, 27086, 29794, 32767,
};
static const i8 g_ima_index[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

static i16 ima_nibble(xf_ima_state *st, int nibble)
{
    int step = g_ima_steps[st->step_index];
    int diff = step >> 3, pred;
    if (nibble & 4)
        diff += step;
    if (nibble & 2)
        diff += step >> 1;
    if (nibble & 1)
        diff += step >> 2;
    pred = st->predictor + ((nibble & 8) ? -diff : diff);
    st->predictor = XF_CLAMP(pred, -32768, 32767);
    st->step_index = XF_CLAMP(st->step_index + g_ima_index[nibble], 0, 88);
    return (i16)st->predictor;
}

void xf_ima4_packet(xf_ima_state *st, const u8 *packet, i16 *out, int stride)
{
    int header = (i16)xf_be16(packet), index = header & 0x7F, pred = header & ~0x7F, i;

    /* The header stores the predictor with 9 bits of precision. Keep the
     * running value when it is close, to avoid a click at each packet. */
    if (st->step_index != index || abs(pred - st->predictor) > 0x7F) {
        st->step_index = XF_MIN(index, 88);
        st->predictor = pred;
    }
    for (i = 0; i < 32; i++) {
        u8 b = packet[2 + i];
        out[(i * 2) * stride] = ima_nibble(st, b & 0x0F);
        out[(i * 2 + 1) * stride] = ima_nibble(st, b >> 4);
    }
}

int xf_audio_supported(const xf_mov_track *t)
{
    if (t->kind != XF_TRACK_AUDIO || t->channels < 1 || t->channels > 2)
        return 0;
    if (t->codec == XF_FOURCC('i', 'm', 'a', '4'))
        return 1;
    if (t->codec == XF_FOURCC('t', 'w', 'o', 's'))
        return t->bits == 8 || t->bits == 16;
    if (t->codec == XF_FOURCC('Q', 'D', 'M', 'C')) {
        xf_qdmc *q = xf_qdmc_open(t->desc, t->desc_size);
        int ok = q && xf_qdmc_channels(q) == t->channels;
        xf_qdmc_close(q);
        return ok;
    }
    return 0;
}

int xf_audio_open(xf_audio_stream *s, xf_mov *mov, const xf_mov_track *t)
{
    memset(s, 0, sizeof(*s));
    if (!xf_audio_supported(t))
        return -1;
    s->mov = mov;
    s->track = t;
    if (t->codec == XF_FOURCC('Q', 'D', 'M', 'C') && !(s->qdmc = xf_qdmc_open(t->desc, t->desc_size)))
        return -1;
    return 0;
}

void xf_audio_close(xf_audio_stream *s)
{
    xf_qdmc_close(s->qdmc);
    free(s->buf);
    memset(s, 0, sizeof(*s));
}

static size_t chunk_bytes(const xf_audio_stream *s, u32 frames)
{
    const xf_mov_track *t = s->track;
    if (s->qdmc) /* whole packets of a fixed size */
        return (size_t)(frames / xf_qdmc_frame_samples(s->qdmc)) * xf_qdmc_packet_bytes(s->qdmc);
    if (t->codec == XF_FOURCC('i', 'm', 'a', '4'))
        return (size_t)(frames / IMA_PACKET_FRAMES) * IMA_PACKET_BYTES * t->channels;
    return (size_t)frames * t->channels * (t->bits / 8);
}

u32 xf_audio_max_chunk_frames(const xf_audio_stream *s)
{
    u32 i, m = s->max_chunk;
    if (m)
        return m;
    for (i = 0; i < s->track->chunk_count; i++)
        m = XF_MAX(m, s->track->chunk_samples[i]);
    ((xf_audio_stream *)s)->max_chunk = m; /* worked out once */
    return m;
}

int xf_audio_next(xf_audio_stream *s, i16 *out)
{
    const xf_mov_track *t = s->track;
    u32 frames, i;
    size_t n;
    int ch = t->channels;

    if (s->chunk >= t->chunk_count)
        return 0;
    frames = t->chunk_samples[s->chunk];
    n = chunk_bytes(s, frames);
    if (n > s->buf_size) {
        u8 *b = realloc(s->buf, n);
        if (!b)
            return -1;
        s->buf = b;
        s->buf_size = n;
    }
    if (xf_read_at(s->mov->file, t->chunk_offset[s->chunk], s->buf, n) != 0)
        return -1;
    s->chunk++;

    if (s->qdmc) {
        u32 fs = xf_qdmc_frame_samples(s->qdmc), pb = xf_qdmc_packet_bytes(s->qdmc), p;
        for (p = 0; p < frames / fs; p++)
            xf_qdmc_decode(s->qdmc, s->buf + (size_t)p * pb, pb, out + (size_t)p * fs * ch);
        return (int)(frames / fs * fs);
    }
    if (t->codec == XF_FOURCC('i', 'm', 'a', '4')) {
        u32 packets = frames / IMA_PACKET_FRAMES, p;
        int c;
        /* Stereo packets alternate: left packet, right packet. */
        for (p = 0; p < packets; p++)
            for (c = 0; c < ch; c++)
                xf_ima4_packet(&s->ima[c], s->buf + (p * ch + c) * IMA_PACKET_BYTES,
                               out + p * IMA_PACKET_FRAMES * ch + c, ch);
        return (int)(packets * IMA_PACKET_FRAMES);
    }
    if (t->bits == 16) {
        for (i = 0; i < frames * ch; i++)
            out[i] = (i16)xf_be16(s->buf + i * 2);
    } else {
        for (i = 0; i < frames * ch; i++)
            out[i] = (i16)((i8)s->buf[i] * 256);
    }
    return (int)frames;
}

u32 xf_audio_seek(xf_audio_stream *s, u32 frame)
{
    const xf_mov_track *t = s->track;
    s->chunk = xf_mov_chunk_at(t, frame);
    memset(s->ima, 0, sizeof(s->ima));
    if (s->qdmc)
        xf_qdmc_reset(s->qdmc);
    return t->chunk_count ? frame - t->chunk_first[s->chunk] : 0;
}
