/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
#include "video.h"

#include <stdlib.h>
#include <string.h>

#include "media/cinepak.h"
#include "xf_endian.h"
#include "media/jpeg.h"
#include "media/rpza.h"
#include "media/smc.h"

static int codec_supported(u32 codec)
{
    switch (codec) {
    case XF_FOURCC('c', 'v', 'i', 'd'):
    case XF_FOURCC('r', 'p', 'z', 'a'):
    case XF_FOURCC('j', 'p', 'e', 'g'):
    case XF_FOURCC('m', 'j', 'p', 'a'):
    case XF_FOURCC('s', 'm', 'c', ' '):
        return 1;
    }
    return 0;
}

/* An 8-bit track's colours: the colour table in its sample description
 * (colour table ID 0 at byte 76, the table at 78: seed, flags, entries -
 * 1, then index, red, green, blue as 16-bit values), else the Macintosh
 * system palette. */
static void read_palette(xf_video *v, const xf_mov_desc *d)
{
    u32 i, n;
    xf_mac_palette(v->palette);
    if (d->size < 86 || xf_be16(d->data + 76) != 0)
        return;
    n = xf_be16(d->data + 84) + 1u;
    for (i = 0; i < n && 86 + i * 8 + 8 <= d->size; i++) {
        const u8 *e = d->data + 86 + i * 8;
        u16 idx = xf_be16(e);
        if (idx < 256)
            v->palette[idx] = (u32)e[2] << 16 | (u32)e[4] << 8 | e[6];
        else if (n <= 256) /* tables that leave the index 0 count in order */
            v->palette[i] = (u32)e[2] << 16 | (u32)e[4] << 8 | e[6];
    }
}

int xf_video_supported(const xf_mov_track *t)
{
    u32 i;
    if (t->kind != XF_TRACK_VIDEO || !t->sample_size || !t->width || !t->height || !t->desc_count)
        return 0;
    for (i = 0; i < t->desc_count; i++)
        if (!codec_supported(t->descs[i].codec))
            return 0;
    return 1;
}

int xf_video_open(xf_video *v, xf_mov *mov, const xf_mov_track *t)
{
    memset(v, 0, sizeof(*v));
    if (!xf_video_supported(t))
        return -1;
    v->mov = mov;
    v->track = t;
    v->width = t->width;
    v->height = t->height;
    v->frame = calloc((size_t)v->width * v->height, 4);
    if (!v->frame)
        return -1;
    v->cinepak = xf_cinepak_create(v->width, v->height);
    v->smc = calloc(1, sizeof(*v->smc));
    if (!v->cinepak || !v->smc) {
        xf_video_close(v);
        return -1;
    }
    read_palette(v, &t->descs[0]);
    return 0;
}

void xf_video_close(xf_video *v)
{
    xf_cinepak_destroy(v->cinepak);
    free(v->smc);
    free(v->frame);
    free(v->buf);
    memset(v, 0, sizeof(*v));
}

int xf_video_next(xf_video *v)
{
    const xf_mov_track *t = v->track;
    u32 i = v->next, size;
    int w = v->width, h = v->height;

    if (i >= t->sample_count)
        return -1;
    size = t->sample_size[i];
    if (size > v->buf_size) {
        u8 *b = realloc(v->buf, size);
        if (!b)
            return -1;
        v->buf = b;
        v->buf_size = size;
    }
    if (xf_mov_read_sample(v->mov, t, i, v->buf) != 0)
        return -1;
    v->next = i + 1;
    /* A track may switch codec between chunks, so look it up per sample. */
    switch (t->descs[t->sample_desc[i]].codec) {
    case XF_FOURCC('c', 'v', 'i', 'd'):
        return xf_cinepak_decode(v->cinepak, v->buf, size, v->frame);
    case XF_FOURCC('r', 'p', 'z', 'a'):
        return xf_rpza_decode(v->buf, size, v->frame, w, h);
    case XF_FOURCC('j', 'p', 'e', 'g'):
        return xf_jpeg_decode(v->buf, size, v->frame, w, h, w);
    case XF_FOURCC('m', 'j', 'p', 'a'):
        return xf_mjpa_decode(v->buf, size, v->frame, w, h, w);
    case XF_FOURCC('s', 'm', 'c', ' '):
        return xf_smc_decode(v->smc, v->buf, size, v->palette, v->frame, w, h);
    }
    return -1;
}

int xf_video_seek(xf_video *v, u32 index)
{
    u32 i = index;

    if (index >= v->track->sample_count)
        return -1;
    if (index != v->next) {
        while (i > 0 && !xf_mov_is_sync(v->track, i))
            i--;
        /* Continue from the current position if it is closer. */
        if (v->next > i && v->next <= index)
            i = v->next;
        v->next = i;
    }
    while (v->next <= index)
        if (xf_video_next(v) != 0)
            return -1;
    return 0;
}

int xf_movie_frame_at(xf_mov *mov, u32 ms, u32 *out, int width, int height)
{
    u64 movie_time = (u64)ms * mov->timescale / 1000, media;
    u32 k;

    for (k = 0; k < mov->track_count; k++) {
        const xf_mov_track *t = &mov->tracks[k];
        xf_video v;
        int r, y;
        if (t->kind != XF_TRACK_VIDEO || xf_mov_edit_lookup(mov, t, movie_time, &media) != 0)
            continue;
        if (xf_video_open(&v, mov, t) != 0)
            return -1;
        r = xf_video_seek(&v, xf_mov_sample_at(t, (u32)media));
        if (r == 0)
            for (y = 0; y < XF_MIN(height, v.height); y++)
                memcpy(out + (size_t)y * width, v.frame + (size_t)y * v.width,
                       (size_t)XF_MIN(width, v.width) * 4);
        xf_video_close(&v);
        return r;
    }
    return -1;
}
