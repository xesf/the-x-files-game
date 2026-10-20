/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
#include "bmp.h"

#include <stdlib.h>
#include <string.h>

#include "data/szdd.h"
#include "xf_endian.h"

#define BI_RGB 0
#define BI_RLE8 1
/* a fourcc as a little-endian u32 */
#define LE_FOURCC(a, b, c, d) ((u32)(a) | (u32)(b) << 8 | (u32)(c) << 16 | (u32)(d) << 24)
#define CRAM LE_FOURCC('C', 'R', 'A', 'M')
#define MSVC LE_FOURCC('M', 'S', 'V', 'C')

/* Writes palette index `v` at (x, y) of a bottom-up picture. */
static void put8(xf_bmp *b, int x, int y, u8 v)
{
    if (x < 0 || y < 0 || x >= b->width || y >= b->height)
        return;
    y = b->height - 1 - y;
    b->indices[(size_t)y * b->width + x] = v;
}

static void put16(xf_bmp *b, int x, int y, u16 c)
{
    u32 r = c >> 10 & 31, g = c >> 5 & 31, bl = c & 31;
    if (x < 0 || y < 0 || x >= b->width || y >= b->height)
        return;
    y = b->height - 1 - y;
    b->pixels[(size_t)y * b->width + x] = (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (bl << 3 | bl >> 2);
}

/* BI_RLE8: (count, index) runs; count 0 is an escape: 0 ends the line,
 * 1 the picture, 2 moves by (dx, dy), and n >= 3 copies n indices,
 * padded to a 16-bit boundary. Lines go bottom up. */
static int rle8(xf_bmp *b, const u8 *p, const u8 *end)
{
    int x = 0, y = 0, k;
    while (p + 2 <= end) {
        int n = p[0], v = p[1];
        p += 2;
        if (n) {
            for (k = 0; k < n; k++)
                put8(b, x++, y, (u8)v);
        } else if (v == 0) {
            x = 0;
            y++;
        } else if (v == 1) {
            return 0;
        } else if (v == 2) {
            if (p + 2 > end)
                return -1;
            x += p[0];
            y += p[1];
            p += 2;
        } else {
            if (p + v > end)
                return -1;
            for (k = 0; k < v; k++)
                put8(b, x++, y, p[k]);
            p += v + (v & 1);
        }
    }
    return 0;
}

/* Microsoft Video 1: 4x4 blocks from the bottom left, a row of blocks at
 * a time going up. Each block starts with two bytes a, b:
 *   b & 0xFC == 0x84    skip ((b - 0x84) << 8) + a blocks, this one included
 *   b < 0x80            flags = b << 8 | a, a bit per pixel from the
 *                       block's bottom left, row by row going up; a clear
 *                       bit takes the first colour of the pair
 *   otherwise           one colour (16-bit: b << 8 | a; 8-bit: a)
 * 16-bit: the pair is two RGB555 words; when the first has its top bit
 * set, three more pairs follow, one per 2x2 quarter (bottom left, bottom
 * right, top left, top right). 8-bit: b >= 0x90 gives the four pairs of
 * indices, b < 0x80 one pair. */
static int msvideo1(xf_bmp *b, const u8 *p, const u8 *end)
{
    int bw = (b->width + 3) / 4, bh = (b->height + 3) / 4, skip = 0, bx, by, x, y;
    for (by = 0; by < bh; by++)
        for (bx = 0; bx < bw; bx++) {
            int a, c;
            u16 col[8];
            if (skip > 0) {
                skip--;
                continue;
            }
            if (p + 2 > end)
                return -1;
            a = p[0];
            c = p[1];
            p += 2;
            if ((c & 0xFC) == 0x84) {
                skip = ((c - 0x84) << 8) + a - 1;
                continue;
            }
            if (b->bits == 16) {
                if (c < 0x80) {
                    unsigned flags = (unsigned)c << 8 | (unsigned)a;
                    int quads, k;
                    if (p + 4 > end)
                        return -1;
                    col[0] = xf_le16(p);
                    col[1] = xf_le16(p + 2);
                    p += 4;
                    quads = col[0] & 0x8000;
                    if (quads) {
                        if (p + 12 > end)
                            return -1;
                        for (k = 2; k < 8; k++, p += 2)
                            col[k] = xf_le16(p);
                    }
                    for (y = 0; y < 4; y++)
                        for (x = 0; x < 4; x++, flags >>= 1) {
                            int q = quads ? (y & 2) * 2 + (x & 2) : 0;
                            put16(b, bx * 4 + x, by * 4 + y, col[q + ((flags & 1) ^ 1)] & 0x7FFF);
                        }
                } else {
                    u16 one = (u16)(c << 8 | a);
                    for (y = 0; y < 4; y++)
                        for (x = 0; x < 4; x++)
                            put16(b, bx * 4 + x, by * 4 + y, one & 0x7FFF);
                }
            } else {
                if (c < 0x80 || c >= 0x90) {
                    unsigned flags = (unsigned)c << 8 | (unsigned)a;
                    int quads = c >= 0x90, n = quads ? 8 : 2, k;
                    if (p + n > end)
                        return -1;
                    for (k = 0; k < n; k++)
                        col[k] = *p++;
                    for (y = 0; y < 4; y++)
                        for (x = 0; x < 4; x++, flags >>= 1) {
                            int q = quads ? (y & 2) * 2 + (x & 2) : 0;
                            put8(b, bx * 4 + x, by * 4 + y, (u8)col[q + ((flags & 1) ^ 1)]);
                        }
                } else {
                    for (y = 0; y < 4; y++)
                        for (x = 0; x < 4; x++)
                            put8(b, bx * 4 + x, by * 4 + y, (u8)a);
                }
            }
        }
    return 0;
}

int xf_bmp_decode(const u8 *data, size_t size, xf_bmp *b)
{
    const u8 *dib = data, *bits, *end = data + size;
    u32 header, offset = 0, comp, i, colors;
    int w, h, bpp, flip = 1, x, y;
    size_t n, stride;

    memset(b, 0, sizeof(*b));
    if (size >= 14 && data[0] == 'B' && data[1] == 'M') {
        offset = xf_le32(data + 10);
        dib = data + 14;
    }
    if (dib + 40 > end || (header = xf_le32(dib)) < 40 || dib + header > end)
        return -1;
    w = (i32)xf_le32(dib + 4);
    h = (i32)xf_le32(dib + 8);
    bpp = xf_le16(dib + 14);
    comp = xf_le32(dib + 16);
    colors = xf_le32(dib + 32);
    if (h < 0) {
        h = -h;
        flip = 0;
    }
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
        return -1;
    if (bpp <= 8 && !colors)
        colors = 1u << bpp;
    /* Quantum Gate's .HSB pictures have no colour table: their indices
     * are into the game's palette. Grey stands in for it here. */
    if (!offset && bpp == 8 && comp == BI_RGB && (size_t)(end - dib) == header + (((size_t)w + 3) & ~(size_t)3) * (size_t)(h < 0 ? -h : h)) {
        for (i = 0; i < 256; i++)
            b->palette[i] = i << 16 | i << 8 | i;
        colors = 0;
    }
    if (colors > 256 || dib + header + colors * 4 > end)
        return -1;
    for (i = 0; i < colors; i++) {
        const u8 *q = dib + header + i * 4;
        b->palette[i] = (u32)q[2] << 16 | (u32)q[1] << 8 | q[0];
    }
    b->colors = (int)colors;
    bits = offset ? data + offset : dib + header + colors * 4;
    if (bits > end)
        return -1;
    b->width = w;
    b->height = h;
    b->bits = bpp;
    b->compression = comp;
    n = (size_t)w * h;
    b->pixels = calloc(n, 4);
    if (bpp == 8)
        b->indices = calloc(n, 1);
    if (!b->pixels || (bpp == 8 && !b->indices))
        goto fail;

    if (comp == CRAM || comp == MSVC) {
        if ((bpp != 8 && bpp != 16) || msvideo1(b, bits, end) != 0)
            goto fail;
    } else if (comp == BI_RLE8) {
        if (bpp != 8 || rle8(b, bits, end) != 0)
            goto fail;
    } else if (comp == BI_RGB && (bpp == 8 || bpp == 24)) {
        stride = ((size_t)w * bpp / 8 + 3) & ~(size_t)3;
        if (bits + stride * h > end)
            goto fail;
        for (y = 0; y < h; y++) {
            const u8 *row = bits + stride * (size_t)(flip ? h - 1 - y : y);
            for (x = 0; x < w; x++)
                if (bpp == 8)
                    b->indices[(size_t)y * w + x] = row[x];
                else
                    b->pixels[(size_t)y * w + x] =
                        (u32)row[x * 3 + 2] << 16 | (u32)row[x * 3 + 1] << 8 | row[x * 3];
        }
    } else {
        goto fail; /* Indeo ('RT21') and others */
    }
    if (b->indices)
        for (i = 0; i < n; i++)
            b->pixels[i] = b->palette[b->indices[i]];
    return 0;
fail:
    xf_bmp_free(b);
    return -1;
}

void xf_bmp_free(xf_bmp *b)
{
    free(b->pixels);
    free(b->indices);
    memset(b, 0, sizeof(*b));
}

int xf_bmp_load(const char *path, xf_bmp *out)
{
    size_t size;
    u8 *data = xf_szdd_load(path, &size);
    int r;
    if (!data) {
        memset(out, 0, sizeof(*out));
        return -1;
    }
    r = xf_bmp_decode(data, size, out);
    free(data);
    return r;
}
