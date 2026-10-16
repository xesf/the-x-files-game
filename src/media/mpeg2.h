/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf
 * The code tables follow FFmpeg's mpeg12data.c, Copyright (c) 2000,2001
 * Fabrice Bellard, Copyright (c) 2002-2004 Michael Niedermayer
 * (LGPL-2.1-or-later). */
/* MPEG-2 video (ISO/IEC 13818-2), Main profile: the DVD edition's .vob
 * clips. Frame pictures with frame, field and dual-prime prediction;
 * 4:2:0 only. The decoder takes the video elementary stream in pieces
 * and returns the pictures in display order. */
#ifndef XF_MPEG2_H
#define XF_MPEG2_H

#include "xf_types.h"

typedef struct xf_mpeg2 xf_mpeg2;

typedef struct {
    const u8 *y, *cb, *cr;
    int width, height;     /* coded size of the picture */
    int stride, cstride;   /* bytes per row, luma and chroma */
    int fields;            /* display time in fields: 2, or 3 with repeat_first_field */
    int field_rate_num;    /* fields per second = num / den */
    int field_rate_den;
} xf_mpeg2_picture;

xf_mpeg2 *xf_mpeg2_new(void);
void xf_mpeg2_free(xf_mpeg2 *d);
/* Appends elementary stream bytes. Returns 0, or -1 when out of memory. */
int xf_mpeg2_push(xf_mpeg2 *d, const u8 *data, size_t size);
/* No more data will come: the last pictures can be returned. */
void xf_mpeg2_end(xf_mpeg2 *d);
/* Decodes until a picture is ready for display. Returns 1 and fills
 * `pic` (valid until the next call), or 0 when more data is needed or
 * the stream is over. */
int xf_mpeg2_next(xf_mpeg2 *d, xf_mpeg2_picture *pic);
/* Forgets the stream, for a new start. */
void xf_mpeg2_reset(xf_mpeg2 *d);

#endif
