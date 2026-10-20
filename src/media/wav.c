/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
#include "wav.h"

#include <stdlib.h>
#include <string.h>

#include "xf_endian.h"
#include "xf_file.h"

/* RIFF "WAVE": chunks of a four-letter name and a little-endian size,
 * padded to even sizes; "fmt " gives the format (1 = PCM), the channels,
 * the rate and the bits per sample, "data" the samples. */
int xf_wav_decode(const u8 *data, size_t size, xf_wav *w)
{
    size_t p = 12;
    const u8 *fmt = NULL, *pcm = NULL;
    u32 pcm_size = 0, i, n;
    int bits;

    memset(w, 0, sizeof(*w));
    if (size < 12 || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0)
        return -1;
    while (p + 8 <= size) {
        u32 len = xf_le32(data + p + 4);
        if (len > size - p - 8)
            len = (u32)(size - p - 8); /* a data chunk cut short */
        if (memcmp(data + p, "fmt ", 4) == 0 && len >= 16)
            fmt = data + p + 8;
        else if (memcmp(data + p, "data", 4) == 0) {
            pcm = data + p + 8;
            pcm_size = len;
        }
        p += 8 + (size_t)len + (len & 1);
    }
    if (!fmt || !pcm || xf_le16(fmt) != 1)
        return -1;
    w->channels = xf_le16(fmt + 2);
    w->rate = xf_le32(fmt + 4);
    bits = xf_le16(fmt + 14);
    if ((bits != 8 && bits != 16) || w->channels < 1 || w->channels > 2 || !w->rate)
        return -1;
    w->frames = pcm_size / (u32)(bits / 8 * w->channels);
    n = w->frames * (u32)w->channels;
    w->samples = malloc((size_t)(n ? n : 1) * sizeof(i16));
    if (!w->samples)
        return -1;
    for (i = 0; i < n; i++)
        w->samples[i] = bits == 8 ? (i16)(((int)pcm[i] - 128) * 256) : (i16)xf_le16(pcm + i * 2);
    return 0;
}

int xf_wav_load(const char *path, xf_wav *out)
{
    size_t size;
    u8 *data = xf_load(path, &size);
    int r = data ? xf_wav_decode(data, size, out) : -1;
    if (!data)
        memset(out, 0, sizeof(*out));
    free(data);
    return r;
}

void xf_wav_free(xf_wav *w)
{
    free(w->samples);
    memset(w, 0, sizeof(*w));
}
