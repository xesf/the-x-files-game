/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
#include "smc.h"

#include <string.h>

#include "xf_endian.h"

/* The system palette: a 6x6x6 cube from white down (0 to 214), ramps of
 * red, green, blue and grey without the cube's steps (215 to 254: EE DD
 * BB AA 88 77 55 44 22 11), then black. */
void xf_mac_palette(u32 *pal)
{
    static const u8 ramp[10] = { 0xEE, 0xDD, 0xBB, 0xAA, 0x88, 0x77, 0x55, 0x44, 0x22, 0x11 };
    int i, k;
    for (i = 0; i < 215; i++)
        pal[i] = (u32)(0xFF - i / 36 * 0x33) << 16 | (u32)(0xFF - i / 6 % 6 * 0x33) << 8 | (u32)(0xFF - i % 6 * 0x33);
    for (k = 0; k < 10; k++) {
        pal[215 + k] = (u32)ramp[k] << 16;
        pal[225 + k] = (u32)ramp[k] << 8;
        pal[235 + k] = ramp[k];
        pal[245 + k] = (u32)ramp[k] * 0x010101u;
    }
    pal[255] = 0;
}

typedef struct {
    u32 *frame;
    const u32 *pal;
    int width, height, bw, total, block;
} smc_out;

static void put(smc_out *o, int block, int px, int py, u32 color)
{
    int x = block % o->bw * 4 + px, y = block / o->bw * 4 + py;
    if (x < o->width && y < o->height)
        o->frame[y * o->width + x] = color;
}

static u32 get(smc_out *o, int block, int px, int py)
{
    int x = block % o->bw * 4 + px, y = block / o->bw * 4 + py;
    return x < o->width && y < o->height ? o->frame[y * o->width + x] : 0;
}

/* Copies block `from` to the current block, and moves on. */
static void copy_block(smc_out *o, int from)
{
    int x, y;
    for (y = 0; y < 4; y++)
        for (x = 0; x < 4; x++)
            put(o, o->block, x, y, get(o, from, x, y));
    o->block++;
}

/* A frame: a flag byte and a 24-bit size, then opcodes over the 4x4
 * blocks in raster order. The high nibble is the operation, the low one
 * a count - 1 (the odd operations read the count - 1 from the next byte):
 *   0x00 0x10  skip blocks          0x20 0x30  repeat the previous block
 *   0x40 0x50  repeat the previous two blocks
 *   0x60 0x70  fill with one colour
 *   0x80 0x90  two colours: new (cached) or a cache index; 16 bits a block
 *   0xA0 0xB0  four colours, likewise; 2 bits a pixel
 *   0xC0 0xD0  eight colours, likewise; 3 bits a pixel
 *   0xE0       sixteen colours given in full
 * The bits go most significant first, a row at a time from the top. */
int xf_smc_decode(xf_smc *s, const u8 *data, size_t size, const u32 *pal, u32 *frame, int width, int height)
{
    smc_out o;
    const u8 *p = data + 4, *end;
    int x, y, k;

    if (size < 4)
        return -1;
    end = data + XF_MIN(size, (size_t)(data[1] << 16 | data[2] << 8 | data[3]));
    o.frame = frame;
    o.pal = pal;
    o.width = width;
    o.height = height;
    o.bw = (width + 3) / 4;
    o.total = o.bw * ((height + 3) / 4);
    o.block = 0;
    /* each frame fills the caches from their start again */
    s->next_pair = s->next_quad = s->next_octet = 0;
    while (o.block < o.total && p < end) {
        int op = *p++, type = op & 0xF0, n = (op & 0x0F) + 1;
        if (type == 0x10 || type == 0x30 || type == 0x50 || type == 0x70) {
            if (p >= end)
                return -1;
            n = *p++ + 1;
        }
        switch (type) {
        case 0x00:
        case 0x10:
            o.block = XF_MIN(o.block + n, o.total);
            break;
        case 0x20:
        case 0x30:
            if (o.block < 1)
                return -1;
            while (n-- > 0 && o.block < o.total)
                copy_block(&o, o.block - 1);
            break;
        case 0x40:
        case 0x50:
            if (o.block < 2)
                return -1;
            while (n-- > 0 && o.block + 1 < o.total) {
                copy_block(&o, o.block - 2);
                copy_block(&o, o.block - 2);
            }
            break;
        case 0x60:
        case 0x70: {
            u32 c;
            if (p >= end)
                return -1;
            c = pal[*p++];
            while (n-- > 0 && o.block < o.total) {
                for (y = 0; y < 4; y++)
                    for (x = 0; x < 4; x++)
                        put(&o, o.block, x, y, c);
                o.block++;
            }
            break;
        }
        case 0x80:
        case 0x90: {
            const u8 *c;
            if (type == 0x80) {
                if (p + 2 > end)
                    return -1;
                memcpy(s->pairs[s->next_pair], p, 2);
                c = s->pairs[s->next_pair];
                s->next_pair = (s->next_pair + 1) & 255;
                p += 2;
            } else {
                if (p >= end)
                    return -1;
                c = s->pairs[*p++];
            }
            while (n-- > 0 && o.block < o.total) {
                unsigned flags;
                if (p + 2 > end)
                    return -1;
                flags = xf_be16(p);
                p += 2;
                for (k = 0; k < 16; k++)
                    put(&o, o.block, k & 3, k >> 2, pal[c[flags >> (15 - k) & 1]]);
                o.block++;
            }
            break;
        }
        case 0xA0:
        case 0xB0: {
            const u8 *c;
            if (type == 0xA0) {
                if (p + 4 > end)
                    return -1;
                memcpy(s->quads[s->next_quad], p, 4);
                c = s->quads[s->next_quad];
                s->next_quad = (s->next_quad + 1) & 255;
                p += 4;
            } else {
                if (p >= end)
                    return -1;
                c = s->quads[*p++];
            }
            while (n-- > 0 && o.block < o.total) {
                u32 flags;
                if (p + 4 > end)
                    return -1;
                flags = xf_be32(p);
                p += 4;
                for (k = 0; k < 16; k++)
                    put(&o, o.block, k & 3, k >> 2, pal[c[flags >> (30 - 2 * k) & 3]]);
                o.block++;
            }
            break;
        }
        case 0xC0:
        case 0xD0: {
            const u8 *c;
            if (type == 0xC0) {
                if (p + 8 > end)
                    return -1;
                memcpy(s->octets[s->next_octet], p, 8);
                c = s->octets[s->next_octet];
                s->next_octet = (s->next_octet + 1) & 255;
                p += 8;
            } else {
                if (p >= end)
                    return -1;
                c = s->octets[*p++];
            }
            while (n-- > 0 && o.block < o.total) {
                /* 6 bytes, 3 bits a pixel; the top two rows take the
                 * high 12 bits of the first two 16-bit words, the bottom
                 * two the third word's and the three low nibbles */
                u32 top, bottom;
                if (p + 6 > end)
                    return -1;
                top = (u32)(xf_be16(p) & 0xFFF0) << 8 | (u32)xf_be16(p + 2) >> 4;
                bottom = (u32)(xf_be16(p + 4) & 0xFFF0) << 8 | (u32)(p[1] & 15) << 8 | (u32)(p[3] & 15) << 4 |
                         (u32)(p[5] & 15);
                p += 6;
                for (k = 0; k < 8; k++) {
                    put(&o, o.block, k & 3, k >> 2, pal[c[top >> (21 - 3 * k) & 7]]);
                    put(&o, o.block, k & 3, 2 + (k >> 2), pal[c[bottom >> (21 - 3 * k) & 7]]);
                }
                o.block++;
            }
            break;
        }
        case 0xE0:
            while (n-- > 0 && o.block < o.total) {
                if (p + 16 > end)
                    return -1;
                for (k = 0; k < 16; k++)
                    put(&o, o.block, k & 3, k >> 2, pal[p[k]]);
                p += 16;
                o.block++;
            }
            break;
        default:
            return -1;
        }
    }
    return 0;
}
