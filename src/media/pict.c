/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
#include "pict.h"

#include <stdlib.h>
#include <string.h>

#include "jpeg.h"
#include "xf_endian.h"
#include "xf_log.h"

#define PIXMAP_SIZE 46 /* PixMap without baseAddr */

typedef struct {
    i16 top, left, bottom, right;
} rect16;

typedef struct {
    const u8 *p, *end;
    rect16 frame;
    xf_image *img;
    u8 *alpha;          /* the alpha plane of 32-bit pixmaps, per pixel */
    int alpha_clear, alpha_solid; /* which alpha values the picture has */
} pict_dec;

static rect16 read_rect(const u8 *p)
{
    rect16 r;
    r.top = (i16)xf_be16(p);
    r.left = (i16)xf_be16(p + 2);
    r.bottom = (i16)xf_be16(p + 4);
    r.right = (i16)xf_be16(p + 6);
    return r;
}

static int need(pict_dec *d, size_t n) { return (size_t)(d->end - d->p) >= n ? 0 : -1; }

/* Unpacks one PackBits row. Returns 0 if `out` was filled exactly. */
static int unpack_bits(const u8 *src, size_t src_len, u8 *out, size_t out_len, int word)
{
    size_t i = 0, o = 0, unit = word ? 2 : 1;
    while (i < src_len && o < out_len) {
        int n = (i8)src[i++];
        if (n >= 0) {
            size_t len = (size_t)(n + 1) * unit;
            if (i + len > src_len || o + len > out_len)
                return -1;
            memcpy(out + o, src + i, len);
            i += len;
            o += len;
        } else if (n != -128) {
            size_t count = (size_t)(1 - n), k;
            if (i + unit > src_len || o + count * unit > out_len)
                return -1;
            for (k = 0; k < count; k++) {
                memcpy(out + o, src + i, unit);
                o += unit;
            }
            i += unit;
        }
    }
    return o == out_len ? 0 : -1;
}

/* Copies a decoded row into the image at the destination rectangle. */
static void put_row(pict_dec *d, const rect16 *dst, int y, const u32 *row, int w)
{
    int x, iy = dst->top - d->frame.top + y;
    if (iy < 0 || iy >= d->img->height)
        return;
    for (x = 0; x < w; x++) {
        int ix = dst->left - d->frame.left + x;
        if (ix >= 0 && ix < d->img->width)
            d->img->pixels[iy * d->img->width + ix] = row[x];
    }
}

/* Keeps a row's alpha plane (the evidence icons cut out their object). */
static void keep_alpha(pict_dec *d, const rect16 *dst, int y, const u8 *a, int w)
{
    int x, iy = dst->top - d->frame.top + y;
    if (iy < 0 || iy >= d->img->height)
        return;
    if (!d->alpha && !(d->alpha = calloc((size_t)d->img->width * d->img->height, 1)))
        return;
    for (x = 0; x < w; x++) {
        int ix = dst->left - d->frame.left + x;
        if (ix < 0 || ix >= d->img->width)
            continue;
        d->alpha[iy * d->img->width + ix] = a[x];
        if (a[x])
            d->alpha_solid = 1;
        else
            d->alpha_clear = 1;
    }
}

/* Reads the per-row byte count and returns a pointer to the row data. */
static const u8 *row_data(pict_dec *d, int row_bytes, size_t *len)
{
    if (row_bytes < 8) {
        *len = (size_t)row_bytes;
    } else if (row_bytes > 250) {
        if (need(d, 2))
            return NULL;
        *len = xf_be16(d->p);
        d->p += 2;
    } else {
        if (need(d, 1))
            return NULL;
        *len = *d->p++;
    }
    if (need(d, *len))
        return NULL;
    d->p += *len;
    return d->p - *len;
}

static int op_bits(pict_dec *d, int direct)
{
    u32 palette[256];
    const u8 *pm;
    int row_bytes, pack_type, pixel_size, cmp_count, w, h, y, x;
    rect16 bounds, dst;
    u8 *buf;
    u32 *row;

    if (direct) {
        if (need(d, 4))
            return -1;
        d->p += 4; /* baseAddr */
    }
    if (need(d, PIXMAP_SIZE))
        return -1;
    pm = d->p;
    row_bytes = xf_be16(pm) & 0x3FFF;
    bounds = read_rect(pm + 2);
    pack_type = xf_be16(pm + 12);
    pixel_size = xf_be16(pm + 28);
    cmp_count = xf_be16(pm + 30);
    d->p += PIXMAP_SIZE;

    memset(palette, 0, sizeof(palette));
    if (!direct) {
        int n, i;
        if (need(d, 8))
            return -1;
        n = xf_be16(d->p + 6) + 1;
        d->p += 8;
        if (n > 256 || need(d, (size_t)n * 8))
            return -1;
        for (i = 0; i < n; i++) {
            const u8 *e = d->p + i * 8;
            int idx = (xf_be16(d->p - 4) & 0x8000) ? i : xf_be16(e) & 0xFF;
            palette[idx] = (u32)e[2] << 16 | (u32)e[4] << 8 | e[6];
        }
        d->p += (size_t)n * 8;
    }
    if (need(d, 18))
        return -1;
    dst = read_rect(d->p + 8); /* skip srcRect, use dstRect */
    d->p += 18;                /* srcRect, dstRect, mode */

    w = bounds.right - bounds.left;
    h = bounds.bottom - bounds.top;
    if (w <= 0 || h <= 0 || row_bytes <= 0)
        return -1;
    buf = malloc((size_t)row_bytes);
    row = malloc((size_t)w * 4);
    if (!buf || !row)
        goto fail;

    for (y = 0; y < h; y++) {
        size_t len;
        const u8 *src;
        if (direct && pixel_size == 32 && (pack_type == 1 || pack_type == 2 || row_bytes < 8)) {
            /* unpacked; type 2 drops the alpha byte */
            size_t n = pack_type == 2 ? (size_t)w * 3 : (size_t)row_bytes;
            if (need(d, n))
                goto fail;
            for (x = 0; x < w; x++) {
                const u8 *s = pack_type == 2 ? d->p + x * 3 : d->p + x * 4 + 1;
                row[x] = (u32)s[0] << 16 | (u32)s[1] << 8 | s[2];
            }
            d->p += n;
            put_row(d, &dst, y, row, w);
            continue;
        }
        src = row_data(d, row_bytes, &len);
        if (!src)
            goto fail;
        if (row_bytes < 8) {
            memcpy(buf, src, len);
        } else {
            /* A packed 32-bit row holds one plane per component. */
            size_t out_len = direct && pixel_size == 32 ? (size_t)w * cmp_count : (size_t)row_bytes;
            if (out_len > (size_t)row_bytes ||
                unpack_bits(src, len, buf, out_len, direct && pixel_size == 16) != 0)
                goto fail;
        }

        if (direct && pixel_size == 32) {
            /* planar components per row: [A] R G B */
            int skip = cmp_count == 4 ? w : 0;
            for (x = 0; x < w; x++)
                row[x] = (u32)buf[skip + x] << 16 | (u32)buf[skip + w + x] << 8 | buf[skip + 2 * w + x];
            if (cmp_count == 4)
                keep_alpha(d, &dst, y, buf, w);
        } else if (direct && pixel_size == 16) {
            for (x = 0; x < w; x++) {
                u16 v = xf_be16(buf + x * 2);
                u32 r = v >> 10 & 31, g = v >> 5 & 31, b = v & 31;
                row[x] = (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
            }
        } else if (!direct && pixel_size <= 8) {
            int per = 8 / pixel_size, mask = (1 << pixel_size) - 1;
            for (x = 0; x < w; x++) {
                int byte = buf[x / per], shift = 8 - pixel_size * (x % per + 1);
                row[x] = palette[byte >> shift & mask];
            }
        } else {
            goto fail;
        }
        put_row(d, &dst, y, row, w);
    }
    free(buf);
    free(row);
    return 0;

fail:
    free(buf);
    free(row);
    return -1;
}

static int op_quicktime(pict_dec *d)
{
    const u8 *op, *desc;
    u32 size, matte, mask, desc_size, codec;
    rect16 dst;
    int w, h, r;
    u32 *tmp;

    if (need(d, 4))
        return -1;
    size = xf_be32(d->p);
    if (need(d, 4 + (size_t)size))
        return -1;
    op = d->p + 4;
    d->p += 4 + size;
    if (size < 68)
        return -1;
    matte = xf_be32(op + 38);
    mask = xf_be32(op + 64);
    /* The image covers srcRect, moved by the translation of the matrix
     * (16.16 fixed point). Large pictures are split into bands this way. */
    dst = read_rect(op + 52);
    {
        int tx = (i32)xf_be32(op + 2 + 24) >> 16, ty = (i32)xf_be32(op + 2 + 28) >> 16;
        dst.left = (i16)(dst.left + tx);
        dst.right = (i16)(dst.right + tx);
        dst.top = (i16)(dst.top + ty);
        dst.bottom = (i16)(dst.bottom + ty);
    }
    if (68 + (u64)matte + mask + 86 > size)
        return -1;
    desc = op + 68 + matte + mask;
    desc_size = xf_be32(desc);
    codec = xf_be32(desc + 4);
    w = xf_be16(desc + 32);
    h = xf_be16(desc + 34);
    if (desc_size < 86 || (desc - op) + (u64)desc_size > size || w <= 0 || h <= 0)
        return -1;
    if (codec != XF_FOURCC('j', 'p', 'e', 'g')) {
        char cc[5];
        cc[0] = (char)(codec >> 24);
        cc[1] = (char)(codec >> 16);
        cc[2] = (char)(codec >> 8);
        cc[3] = (char)codec;
        cc[4] = 0;
        xf_warn("pict: unsupported QuickTime codec %s", cc);
        return -1;
    }
    tmp = malloc((size_t)w * h * 4);
    if (!tmp)
        return -1;
    r = xf_jpeg_decode(desc + desc_size, size - (desc - op) - desc_size, tmp, w, h, w);
    if (r == 0) {
        int y;
        for (y = 0; y < h; y++)
            put_row(d, &dst, y, tmp + (size_t)y * w, w);
    }
    free(tmp);
    return r;
}

/* Data length of simple opcodes that only set drawing state or draw
 * shapes we do not need; -1 means variable length or not handled. */
static const i8 g_op_len[0x100] = {
    [0x00] = 0, [0x02] = 8, [0x03] = 2, [0x04] = 1, [0x05] = 2, [0x06] = 4, [0x07] = 4,
    [0x08] = 2, [0x09] = 8, [0x0A] = 8, [0x0B] = 4, [0x0C] = 4, [0x0D] = 2, [0x0E] = 4,
    [0x0F] = 4, [0x10] = 8, [0x11] = 2, [0x15] = 2, [0x16] = 2, [0x1A] = 6, [0x1B] = 6,
    [0x1C] = 0, [0x1D] = 6, [0x1E] = 0, [0x1F] = 6, [0x20] = 8, [0x21] = 4, [0x22] = 6,
    [0x23] = 2, [0x30] = 8, [0x31] = 8, [0x32] = 8, [0x33] = 8, [0x34] = 8, [0x38] = 0,
    [0x39] = 0, [0x3A] = 0, [0x3B] = 0, [0x3C] = 0, [0x40] = 8, [0x41] = 8, [0x42] = 8,
    [0x43] = 8, [0x44] = 8, [0x48] = 0, [0x49] = 0, [0x4A] = 0, [0x4B] = 0, [0x4C] = 0,
    [0x50] = 8, [0x51] = 8, [0x52] = 8, [0x53] = 8, [0x54] = 8, [0x58] = 0, [0x59] = 0,
    [0x5A] = 0, [0x5B] = 0, [0x5C] = 0, [0xA0] = 2,
};

int xf_pict_size(const u8 *data, size_t size, int *width, int *height)
{
    rect16 f;
    if (size < 10)
        return -1;
    f = read_rect(data + 2);
    *width = f.right - f.left;
    *height = f.bottom - f.top;
    return *width > 0 && *height > 0 ? 0 : -1;
}

int xf_pict_decode(const u8 *data, size_t size, xf_image *img)
{
    pict_dec d;
    int drew = 0;

    memset(img, 0, sizeof(*img));
    if (xf_pict_size(data, size, &img->width, &img->height) != 0)
        return -1;
    img->pixels = calloc((size_t)img->width * img->height, 4);
    if (!img->pixels)
        return -1;
    d.frame = read_rect(data + 2);
    d.alpha = NULL;
    d.alpha_clear = d.alpha_solid = 0;
    d.p = data + 10;
    d.end = data + size;
    d.img = img;

    while (need(&d, 2) == 0) {
        u16 op;
        if ((d.p - data) & 1)
            d.p++; /* opcodes are word aligned */
        if (need(&d, 2))
            break;
        op = xf_be16(d.p);
        xf_debug("pict: op 0x%04X at 0x%lX", op, (unsigned long)(d.p - data));
        d.p += 2;
        switch (op) {
        case 0x0C00: /* header */
            d.p += 24;
            break;
        case 0x0001: /* clip region */
            if (need(&d, 2))
                goto done;
            d.p += xf_be16(d.p);
            break;
        case 0x00A1: /* long comment */
            if (need(&d, 4))
                goto done;
            d.p += 4 + xf_be16(d.p + 2);
            break;
        case 0x0098:
        case 0x0090:
            if (op_bits(&d, 0) != 0)
                goto done;
            drew = 1;
            break;
        case 0x009A:
            if (op_bits(&d, 1) != 0)
                goto done;
            drew = 1;
            break;
        case 0x8200:
            if (op_quicktime(&d) != 0)
                goto done;
            drew = 1;
            break;
        case 0x0028: /* LongText: point, count, text */
        case 0x0029: /* DHText: dh, count, text */
        case 0x002A: /* DVText */
        case 0x002B: { /* DHDVText */
            int skip = op == 0x0028 ? 4 : op == 0x002B ? 2 : 1;
            if (need(&d, (size_t)skip + 1))
                goto done;
            d.p += skip + 1 + d.p[skip];
            break;
        }
        case 0x002C: /* font name */
        case 0x002E: /* glyph state */
        case 0x8201: /* uncompressed QuickTime, not used */
            if (need(&d, op == 0x8201 ? 4 : 2))
                goto done;
            d.p += op == 0x8201 ? 4 + xf_be32(d.p) : 2u + xf_be16(d.p);
            break;
        case 0x00FF:
            drew = 1; /* a complete picture, even if it only draws shapes */
            goto done;
        default:
            if (op < XF_COUNT(g_op_len) && g_op_len[op] >= 0) {
                d.p += g_op_len[op];
                break;
            }
            if (!drew)
                xf_warn("pict: unsupported opcode 0x%04X", op);
            goto done;
        }
    }
done:
    /* A picture without the blue key whose alpha plane mixes zero and
     * non-zero values is cut out by it, alpha 0 being clear: the
     * evidence icons draw their object on a white ground the alpha
     * removes. The pictures that already use the blue key keep it. */
    if (drew && d.alpha && d.alpha_clear && d.alpha_solid) {
        size_t k, n = (size_t)img->width * img->height;
        int keyed = 0;
        for (k = 0; k < n && !keyed; k++)
            keyed = img->pixels[k] == XF_PICT_KEY;
        for (k = 0; k < n && !keyed; k++)
            if (!d.alpha[k])
                img->pixels[k] = XF_PICT_KEY;
    }
    free(d.alpha);
    if (!drew) {
        free(img->pixels);
        img->pixels = NULL;
        return -1;
    }
    return 0;
}
