/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
#include "cinepak.h"

#include <stdlib.h>
#include <string.h>

#include "xf_endian.h"

#define MAX_STRIPS 32

/* One codebook entry: a 2x2 block of colors. */
typedef struct {
    u32 rgb[4];
} cb_entry;

typedef struct {
    cb_entry v4[256];
    cb_entry v1[256];
} strip_books;

struct xf_cinepak {
    int width, height;
    strip_books books[MAX_STRIPS];
};

xf_cinepak *xf_cinepak_create(int width, int height)
{
    xf_cinepak *c = calloc(1, sizeof(*c));
    if (c) {
        c->width = width;
        c->height = height;
    }
    return c;
}

void xf_cinepak_destroy(xf_cinepak *c) { free(c); }

static u32 yuv_to_rgb(int y, int u, int v)
{
    int r = y + 2 * v, g = y - u / 2 - v, b = y + 2 * u;
    r = XF_CLAMP(r, 0, 255);
    g = XF_CLAMP(g, 0, 255);
    b = XF_CLAMP(b, 0, 255);
    return (u32)r << 16 | (u32)g << 8 | (u32)b;
}

static const u8 *read_entry(const u8 *p, const u8 *end, cb_entry *e, int mono)
{
    int i, u = 0, v = 0;
    if (p + (mono ? 4 : 6) > end)
        return NULL;
    if (!mono) {
        u = (i8)p[4];
        v = (i8)p[5];
    }
    for (i = 0; i < 4; i++)
        e->rgb[i] = yuv_to_rgb(p[i], u, v);
    return p + (mono ? 4 : 6);
}

static int read_codebook(cb_entry *book, const u8 *p, const u8 *end, int partial, int mono)
{
    int i;
    if (!partial) {
        for (i = 0; i < 256 && p < end; i++)
            if (!(p = read_entry(p, end, &book[i], mono)))
                return -1;
        return 0;
    }
    for (i = 0; i < 256 && p + 4 <= end; i += 32) {
        u32 flags = xf_be32(p);
        int k;
        p += 4;
        for (k = 0; k < 32; k++, flags <<= 1)
            if (flags & 0x80000000u)
                if (!(p = read_entry(p, end, &book[i + k], mono)))
                    return -1;
    }
    return 0;
}

static void put_v1(u32 *frame, int pitch, int x, int y, const cb_entry *e, int w, int h)
{
    int dy, dx;
    for (dy = 0; dy < 4; dy++)
        for (dx = 0; dx < 4; dx++)
            if (x + dx < w && y + dy < h)
                frame[(y + dy) * pitch + x + dx] = e->rgb[(dy >> 1) * 2 + (dx >> 1)];
}

static void put_v4(u32 *frame, int pitch, int x, int y, const cb_entry *const e[4], int w, int h)
{
    int dy, dx;
    for (dy = 0; dy < 4; dy++)
        for (dx = 0; dx < 4; dx++)
            if (x + dx < w && y + dy < h)
                frame[(y + dy) * pitch + x + dx] =
                    e[(dy >> 1) * 2 + (dx >> 1)]->rgb[(dy & 1) * 2 + (dx & 1)];
}

/* Bit reader for the 32-bit flag words between vectors. */
typedef struct {
    const u8 *p, *end;
    u32 flags, mask;
} flag_reader;

static int next_flag(flag_reader *f, int *bit)
{
    f->mask >>= 1;
    if (!f->mask) {
        if (f->p + 4 > f->end)
            return -1;
        f->flags = xf_be32(f->p);
        f->p += 4;
        f->mask = 0x80000000u;
    }
    *bit = (f->flags & f->mask) != 0;
    return 0;
}

static int decode_vectors(xf_cinepak *c, strip_books *b, int id, const u8 *p, const u8 *end,
                          int y1, int y2, u32 *frame)
{
    flag_reader f;
    f.p = p;
    f.end = end;
    f.flags = 0;
    f.mask = 0;
    int x, y;
    for (y = y1; y < y2; y += 4)
        for (x = 0; x < c->width; x += 4) {
            int bit, v4;
            if (id & 0x01) { /* inter: first bit says whether the block changes */
                if (next_flag(&f, &bit) != 0)
                    return -1;
                if (!bit)
                    continue;
            }
            if (id & 0x02) {
                v4 = 0; /* all V1 */
            } else {
                if (next_flag(&f, &bit) != 0)
                    return -1;
                v4 = bit;
            }
            if (v4) {
                const cb_entry *e[4];
                if (f.p + 4 > end)
                    return -1;
                e[0] = &b->v4[f.p[0]];
                e[1] = &b->v4[f.p[1]];
                e[2] = &b->v4[f.p[2]];
                e[3] = &b->v4[f.p[3]];
                f.p += 4;
                put_v4(frame, c->width, x, y, e, c->width, c->height);
            } else {
                if (f.p >= end)
                    return -1;
                put_v1(frame, c->width, x, y, &b->v1[*f.p++], c->width, c->height);
            }
        }
    return 0;
}

int xf_cinepak_decode(xf_cinepak *c, const u8 *data, size_t size, u32 *frame)
{
    const u8 *p = data, *end = data + size;
    int flags, strips, i, y0 = 0;

    if (size < 10)
        return -1;
    flags = p[0];
    strips = xf_be16(p + 8);
    p += 10;
    if (strips > MAX_STRIPS)
        return -1;

    for (i = 0; i < strips; i++) {
        const u8 *strip_end;
        int strip_size, y1, y2;
        strip_books *b = &c->books[i];

        if (p + 12 > end)
            return i > 0 ? 0 : -1;
        strip_size = xf_be16(p + 2);
        /* A damaged strip after the first ends the frame with what came
         * before it (Vortex's UBERASET/LOOPS/CRASH1.MOV, frame 8). */
        if (strip_size < 12 || p + strip_size > end)
            return i > 0 ? 0 : -1;
        strip_end = p + strip_size;
        y1 = y0;
        y2 = y0 + xf_be16(p + 8); /* strip bottom, relative to the strip */
        p += 12;
        /* Without flag bit 0, each strip starts from the previous strip's books. */
        if (i > 0 && !(flags & 0x01))
            memcpy(b, &c->books[i - 1], sizeof(*b));

        while (p + 4 <= strip_end) {
            int id = p[0], chunk = xf_be16(p + 2);
            const u8 *cd = p + 4, *ce = p + chunk;
            if (chunk < 4 || ce > strip_end)
                return -1;
            switch (id) {
            case 0x20: case 0x21: case 0x24: case 0x25:
                if (read_codebook(b->v4, cd, ce, id & 0x01, id & 0x04) != 0)
                    return -1;
                break;
            case 0x22: case 0x23: case 0x26: case 0x27:
                if (read_codebook(b->v1, cd, ce, id & 0x01, id & 0x04) != 0)
                    return -1;
                break;
            case 0x30: case 0x31: case 0x32:
                if (decode_vectors(c, b, id, cd, ce, y1, XF_MIN(y2, c->height), frame) != 0)
                    return -1;
                break;
            }
            p = ce;
        }
        p = strip_end;
        y0 = y2;
    }
    return 0;
}
