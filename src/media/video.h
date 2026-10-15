/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* Decodes the frames of a QuickTime video track, whatever the codec. */
#ifndef XF_VIDEO_H
#define XF_VIDEO_H

#include "media/mov.h"

struct xf_cinepak;

typedef struct {
    xf_mov *mov;
    const xf_mov_track *track;
    int width, height;
    u32 *frame;      /* current picture, 0x00RRGGBB, width x height */
    u32 next;        /* next sample to decode */
    struct xf_cinepak *cinepak;
    u8 *buf;
    size_t buf_size;
} xf_video;

int xf_video_supported(const xf_mov_track *t);
int xf_video_open(xf_video *v, xf_mov *mov, const xf_mov_track *t);
void xf_video_close(xf_video *v);
/* Decodes the next sample into v->frame. Returns 0, or -1 on error. */
int xf_video_next(xf_video *v);
/* Makes v->frame show sample `index`, decoding from the previous key
 * frame when needed. Returns 0, or -1 on error. */
int xf_video_seek(xf_video *v, u32 index);

/* Decodes the picture a movie shows at `ms` milliseconds of movie time,
 * following the edit lists of all video tracks, into `out`
 * (0x00RRGGBB, `width` x `height`). Returns 0, or -1 when no track shows a
 * picture at that time. */
int xf_movie_frame_at(xf_mov *mov, u32 ms, u32 *out, int width, int height);

#endif
