/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* DVD video files (.vob) of the DVD edition: an MPEG-2 program stream
 * with MPEG-2 video and 16-bit LPCM sound (see docs/formats/vob.md). */
#ifndef XF_VOB_H
#define XF_VOB_H

#include "media/mpeg2.h"
#include "xf_types.h"

typedef struct xf_vob xf_vob;

/* The part of the 720x480 picture to show, and the size to show it at. */
typedef struct {
    int x, y, w, h;
    int out_w, out_h;
} xf_vob_view;

/* The DVD player shows the first 704 columns at 640x480. */
extern const xf_vob_view xf_vob_full_view;
/* Scene clips letterbox the game's 600x240 picture in the frame. */
extern const xf_vob_view xf_vob_scene_view;

xf_vob *xf_vob_open(const char *path);
void xf_vob_close(xf_vob *v);
u32 xf_vob_duration_ms(const xf_vob *v);
/* Sound format; rate 0 when the file has no sound we can play. */
int xf_vob_sample_rate(const xf_vob *v);
int xf_vob_channels(const xf_vob *v);
/* Reads up to `max` frames of interleaved sound. Returns the number of
 * frames, 0 at the end. */
int xf_vob_audio(xf_vob *v, i16 *out, int max);
/* Brings the picture to clip time `ms` and draws it into `rgb`
 * (0x00RRGGBB, view->out_w x view->out_h). Returns 1 when the picture
 * changed, 0 when not, -1 once the pictures are over. */
int xf_vob_video(xf_vob *v, u32 ms, u32 *rgb, const xf_vob_view *view);
/* The next picture in display order, for tools; do not mix with
 * xf_vob_video. Returns 0, or -1 at the end. */
int xf_vob_picture(xf_vob *v, xf_mpeg2_picture *pic);

#endif
