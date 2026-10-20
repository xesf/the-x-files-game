/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* Windows bitmaps of the earlier HyperBole games (see docs/formats/bmp.md):
 * uncompressed 8- and 24-bit, RLE8 (Roland), and Microsoft Video 1
 * ('CRAM', 8- and 16-bit: Quantum Gate and Vortex). A file header is
 * optional: Quantum Gate's .HSB pictures unpack to a bare DIB. */
#ifndef XF_BMP_H
#define XF_BMP_H

#include "xf_types.h"

typedef struct {
    int width, height;
    u32 *pixels;        /* 0x00RRGGBB, top row first */
    u8 *indices;        /* palette indices, top row first; NULL above 8 bits */
    u32 palette[256];   /* 0x00RRGGBB */
    int colors;         /* entries in the palette */
    int bits;           /* 8, 16 or 24 */
    u32 compression;    /* 0, 1 (RLE8) or the codec's fourcc as stored */
} xf_bmp;

/* Returns 0, or -1 for data this reader does not handle. */
int xf_bmp_decode(const u8 *data, size_t size, xf_bmp *out);
void xf_bmp_free(xf_bmp *b);
/* Loads a file (unpacking an SZDD one) and decodes it. */
int xf_bmp_load(const char *path, xf_bmp *out);

#endif
