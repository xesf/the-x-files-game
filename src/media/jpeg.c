#include "jpeg.h"

#include <stdlib.h>
#include <string.h>

#include "xf_endian.h"

#define MAX_COMP 3

typedef struct {
    u8 bits[17];     /* number of codes of each length */
    u8 vals[256];
    /* derived */
    i32 maxcode[18];
    i32 valptr[17];
    u16 mincode[17];
    u8 look_len[256]; /* fast lookup for codes up to 8 bits; 0 = slow path */
    u8 look_val[256];
} huff_table;

typedef struct {
    int id, h, v, tq;
    int td, ta;
    int pred;
    int bw, bh;   /* size of the component buffer in blocks */
    u8 *pixels;   /* bw*8 x bh*8 */
} component;

typedef struct {
    const u8 *p, *end;
    u32 bits;
    int nbits;
    int marker_hit;

    u16 qt[4][64];
    huff_table dc[4], ac[4];
    int width, height, ncomp;
    component comp[MAX_COMP];
    int hmax, vmax, mcux, mcuy;
    int restart_interval;
} jpeg_dec;

static const u8 g_zigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

/* Default tables from the JPEG standard (Annex K). Motion-JPEG frames
 * often omit DHT and expect these. */
static const u8 g_std_dc_bits[17] = { 0, 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
static const u8 g_std_dc_vals[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const u8 g_std_dc_bits_c[17] = { 0, 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 };
static const u8 g_std_ac_bits[17] = { 0, 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d };
static const u8 g_std_ac_vals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa,
};
static const u8 g_std_ac_bits_c[17] = { 0, 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77 };
static const u8 g_std_ac_vals_c[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
    0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
    0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa,
};

static void huff_build(huff_table *t)
{
    int len, i, k = 0;
    u16 code = 0;

    memset(t->look_len, 0, sizeof(t->look_len));
    for (len = 1; len <= 16; len++) {
        t->valptr[len] = k;
        t->mincode[len] = code;
        if (t->bits[len]) {
            for (i = 0; i < t->bits[len]; i++, k++, code++) {
                if (len <= 8) {
                    int shift = 8 - len, j;
                    for (j = 0; j < 1 << shift; j++) {
                        int idx = (code << shift) | j;
                        t->look_len[idx] = (u8)len;
                        t->look_val[idx] = t->vals[k];
                    }
                }
            }
            t->maxcode[len] = code - 1;
        } else {
            t->maxcode[len] = -1;
        }
        code <<= 1;
    }
    t->maxcode[17] = 0x7fffffff;
}

static void huff_set(huff_table *t, const u8 *bits, const u8 *vals, int nvals)
{
    memcpy(t->bits, bits, 17);
    memcpy(t->vals, vals, nvals);
    huff_build(t);
}

static void fill_bits(jpeg_dec *d)
{
    while (d->nbits <= 24) {
        u32 b = 0;
        if (!d->marker_hit && d->p < d->end) {
            b = *d->p++;
            if (b == 0xFF) {
                u8 n = d->p < d->end ? *d->p : 0;
                if (n == 0x00)
                    d->p++;
                else {
                    d->p--; /* leave the marker in place */
                    d->marker_hit = 1;
                    b = 0;
                }
            }
        }
        d->bits |= b << (24 - d->nbits);
        d->nbits += 8;
    }
}

static int get_bit(jpeg_dec *d)
{
    int b;
    fill_bits(d);
    b = (int)(d->bits >> 31);
    d->bits <<= 1;
    d->nbits--;
    return b;
}

static int get_bits(jpeg_dec *d, int n)
{
    u32 v;
    if (n == 0)
        return 0;
    fill_bits(d);
    v = d->bits >> (32 - n);
    d->bits <<= n;
    d->nbits -= n;
    return (int)v;
}

static int extend(int v, int n) { return v < (1 << (n - 1)) ? v - (1 << n) + 1 : v; }

static int huff_decode(jpeg_dec *d, const huff_table *t)
{
    int len, code;
    fill_bits(d);
    {
        int idx = (int)(d->bits >> 24);
        len = t->look_len[idx];
        if (len) {
            d->bits <<= len;
            d->nbits -= len;
            return t->look_val[idx];
        }
    }
    code = get_bits(d, 8);
    for (len = 9; len <= 16; len++) {
        code = (code << 1) | get_bit(d);
        if (code <= t->maxcode[len])
            return t->vals[t->valptr[len] + code - t->mincode[len]];
    }
    return -1;
}

/* Separable integer IDCT (AAN-free, straightforward fixed point). */
#define FIX(x) ((int)((x) * 4096 + 0.5))
static void idct_1d(const int *s, int stride, int shift, int *o, int ostride)
{
    int x0 = s[0], x1 = s[stride * 4], x2 = s[stride * 2], x3 = s[stride * 6];
    int x4 = s[stride * 1], x5 = s[stride * 7], x6 = s[stride * 5], x7 = s[stride * 3];
    int p1, p2, p3, p4, p5, t0, t1, t2, t3, t10, t11, t12, t13;
    /* even part */
    p2 = x2;
    p3 = x3;
    p1 = (p2 + p3) * FIX(0.5411961);
    t2 = p1 + p3 * FIX(-1.847759065);
    t3 = p1 + p2 * FIX(0.765366865);
    t0 = (x0 + x1) * 4096;
    t1 = (x0 - x1) * 4096;
    t10 = t0 + t3;
    t13 = t0 - t3;
    t11 = t1 + t2;
    t12 = t1 - t2;
    /* odd part */
    t0 = x5;
    t1 = x6;
    t2 = x7;
    t3 = x4;
    p3 = t0 + t2;
    p4 = t1 + t3;
    p1 = t0 + t3;
    p2 = t1 + t2;
    p5 = (p3 + p4) * FIX(1.175875602);
    t0 = t0 * FIX(0.298631336);
    t1 = t1 * FIX(2.053119869);
    t2 = t2 * FIX(3.072711026);
    t3 = t3 * FIX(1.501321110);
    p1 = p1 * FIX(-0.899976223);
    p2 = p2 * FIX(-2.562915447);
    p3 = p3 * FIX(-1.961570560);
    p4 = p4 * FIX(-0.390180644);
    p3 += p5;
    p4 += p5;
    t0 += p1 + p3;
    t1 += p2 + p4;
    t2 += p2 + p3;
    t3 += p1 + p4;
    {
        int r = 1 << (shift - 1);
        o[0] = (t10 + t3 + r) >> shift;
        o[ostride * 7] = (t10 - t3 + r) >> shift;
        o[ostride * 1] = (t11 + t2 + r) >> shift;
        o[ostride * 6] = (t11 - t2 + r) >> shift;
        o[ostride * 2] = (t12 + t1 + r) >> shift;
        o[ostride * 5] = (t12 - t1 + r) >> shift;
        o[ostride * 3] = (t13 + t0 + r) >> shift;
        o[ostride * 4] = (t13 - t0 + r) >> shift;
    }
}

static void idct_block(const int *in, u8 *out, int stride)
{
    int tmp[64], res[64], i;
    for (i = 0; i < 8; i++)
        idct_1d(in + i, 8, 10, tmp + i, 8); /* columns, keep 2 extra bits */
    for (i = 0; i < 8; i++)
        idct_1d(tmp + i * 8, 1, 17, res + i * 8, 1); /* rows; 12 + 3 + 2 */
    for (i = 0; i < 64; i++) {
        int v = res[i] + 128;
        out[(i >> 3) * stride + (i & 7)] = (u8)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
}

static int decode_block(jpeg_dec *d, component *c, u8 *out, int stride)
{
    int coef[64], k, s, r, t;
    const u16 *q = d->qt[c->tq];

    memset(coef, 0, sizeof(coef));
    t = huff_decode(d, &d->dc[c->td]);
    if (t < 0 || t > 11)
        return -1;
    s = t ? extend(get_bits(d, t), t) : 0;
    c->pred += s;
    coef[0] = c->pred * q[0];
    for (k = 1; k < 64;) {
        int rs = huff_decode(d, &d->ac[c->ta]);
        if (rs < 0)
            return -1;
        r = rs >> 4;
        s = rs & 15;
        if (s == 0) {
            if (r != 15)
                break;
            k += 16;
            continue;
        }
        k += r;
        if (k > 63)
            return -1;
        coef[g_zigzag[k]] = extend(get_bits(d, s), s) * q[k];
        k++;
    }
    idct_block(coef, out, stride);
    return 0;
}

static int read_dqt(jpeg_dec *d, const u8 *p, int len)
{
    while (len > 0) {
        int pq = p[0] >> 4, tq = p[0] & 3, i;
        if (len < 1 + 64 * (pq + 1))
            return -1;
        for (i = 0; i < 64; i++)
            d->qt[tq][i] = pq ? xf_be16(p + 1 + i * 2) : p[1 + i];
        p += 1 + 64 * (pq + 1);
        len -= 1 + 64 * (pq + 1);
    }
    return 0;
}

static int read_dht(jpeg_dec *d, const u8 *p, int len)
{
    while (len > 17) {
        int tc = p[0] >> 4, th = p[0] & 3, n = 0, i;
        huff_table *t = tc ? &d->ac[th] : &d->dc[th];
        t->bits[0] = 0;
        for (i = 1; i <= 16; i++) {
            t->bits[i] = p[i];
            n += p[i];
        }
        if (n > 256 || len < 17 + n)
            return -1;
        memcpy(t->vals, p + 17, n);
        huff_build(t);
        p += 17 + n;
        len -= 17 + n;
    }
    return 0;
}

static int read_sof(jpeg_dec *d, const u8 *p, int len)
{
    int i;
    if (len < 6 || p[0] != 8)
        return -1;
    d->height = xf_be16(p + 1);
    d->width = xf_be16(p + 3);
    d->ncomp = p[5];
    if ((d->ncomp != 1 && d->ncomp != 3) || len < 6 + d->ncomp * 3 || !d->width || !d->height)
        return -1;
    d->hmax = d->vmax = 1;
    for (i = 0; i < d->ncomp; i++) {
        component *c = &d->comp[i];
        c->id = p[6 + i * 3];
        c->h = p[7 + i * 3] >> 4;
        c->v = p[7 + i * 3] & 15;
        c->tq = p[8 + i * 3] & 3;
        if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4)
            return -1;
        d->hmax = XF_MAX(d->hmax, c->h);
        d->vmax = XF_MAX(d->vmax, c->v);
    }
    if (d->ncomp == 1)
        d->comp[0].h = d->comp[0].v = d->hmax = d->vmax = 1;
    d->mcux = (d->width + d->hmax * 8 - 1) / (d->hmax * 8);
    d->mcuy = (d->height + d->vmax * 8 - 1) / (d->vmax * 8);
    for (i = 0; i < d->ncomp; i++) {
        component *c = &d->comp[i];
        c->bw = d->mcux * c->h;
        c->bh = d->mcuy * c->v;
        c->pixels = malloc((size_t)c->bw * 8 * c->bh * 8);
        if (!c->pixels)
            return -1;
    }
    return 0;
}

static int handle_restart(jpeg_dec *d)
{
    int i;
    d->bits = 0;
    d->nbits = 0;
    d->marker_hit = 0;
    if (d->p + 1 < d->end && d->p[0] == 0xFF && d->p[1] >= 0xD0 && d->p[1] <= 0xD7)
        d->p += 2;
    for (i = 0; i < d->ncomp; i++)
        d->comp[i].pred = 0;
    return 0;
}

static int read_scan(jpeg_dec *d, const u8 *p, int len)
{
    int ns = p[0], i, mx, my, n = 0, order[MAX_COMP];
    if (ns != d->ncomp || len < 1 + ns * 2 + 3)
        return -1; /* only interleaved baseline scans */
    for (i = 0; i < ns; i++) {
        int j, id = p[1 + i * 2];
        for (j = 0; j < d->ncomp && d->comp[j].id != id; j++)
            ;
        if (j == d->ncomp)
            return -1;
        order[i] = j;
        d->comp[j].td = p[2 + i * 2] >> 4 & 3;
        d->comp[j].ta = p[2 + i * 2] & 3;
        d->comp[j].pred = 0;
    }
    d->p = p + len;
    d->bits = 0;
    d->nbits = 0;
    d->marker_hit = 0;
    for (my = 0; my < d->mcuy; my++)
        for (mx = 0; mx < d->mcux; mx++) {
            if (d->restart_interval && n && n % d->restart_interval == 0)
                handle_restart(d);
            n++;
            for (i = 0; i < ns; i++) {
                component *c = &d->comp[order[i]];
                int bx, by, stride = c->bw * 8;
                for (by = 0; by < c->v; by++)
                    for (bx = 0; bx < c->h; bx++) {
                        u8 *out = c->pixels + ((my * c->v + by) * 8) * stride + (mx * c->h + bx) * 8;
                        if (decode_block(d, c, out, stride) != 0)
                            return -1;
                    }
            }
        }
    return 0;
}

static void to_rgb(jpeg_dec *d, u32 *out, int out_w, int out_h, int pitch)
{
    int x, y, w = XF_MIN(d->width, out_w), h = XF_MIN(d->height, out_h);
    for (y = 0; y < h; y++) {
        u32 *row = out + (size_t)y * pitch;
        for (x = 0; x < w; x++) {
            int c[3], i;
            for (i = 0; i < d->ncomp; i++) {
                const component *k = &d->comp[i];
                int sx = x * k->h / d->hmax, sy = y * k->v / d->vmax;
                c[i] = k->pixels[sy * k->bw * 8 + sx];
            }
            if (d->ncomp == 1) {
                row[x] = (u32)c[0] * 0x010101;
            } else {
                int yy = c[0] << 16, cb = c[1] - 128, cr = c[2] - 128;
                int r = (yy + 91881 * cr + 32768) >> 16;
                int g = (yy - 22554 * cb - 46802 * cr + 32768) >> 16;
                int b = (yy + 116130 * cb + 32768) >> 16;
                r = XF_CLAMP(r, 0, 255);
                g = XF_CLAMP(g, 0, 255);
                b = XF_CLAMP(b, 0, 255);
                row[x] = (u32)r << 16 | (u32)g << 8 | (u32)b;
            }
        }
    }
}

static void init_defaults(jpeg_dec *d)
{
    huff_set(&d->dc[0], g_std_dc_bits, g_std_dc_vals, 12);
    huff_set(&d->dc[1], g_std_dc_bits_c, g_std_dc_vals, 12);
    huff_set(&d->ac[0], g_std_ac_bits, g_std_ac_vals, 162);
    huff_set(&d->ac[1], g_std_ac_bits_c, g_std_ac_vals_c, 162);
}

static void free_dec(jpeg_dec *d)
{
    int i;
    for (i = 0; i < MAX_COMP; i++)
        free(d->comp[i].pixels);
}

/* Walks the markers. With `out` NULL it stops after the frame header. */
static int run(jpeg_dec *d, const u8 *data, size_t size, u32 *out, int out_w, int out_h, int pitch)
{
    const u8 *p = data, *end = data + size;
    int have_frame = 0;

    d->end = end;
    while (p + 4 <= end) {
        int marker, len;
        if (p[0] != 0xFF) {
            p++;
            continue;
        }
        marker = p[1];
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0xFF) {
            p += marker == 0xFF ? 1 : 2;
            continue;
        }
        if (marker == 0xD9)
            break;
        len = xf_be16(p + 2);
        if (len < 2 || p + 2 + len > end)
            return -1;
        switch (marker) {
        case 0xC0:
        case 0xC1:
            if (read_sof(d, p + 4, len - 2) != 0)
                return -1;
            have_frame = 1;
            if (!out)
                return 0;
            break;
        case 0xC2: case 0xC3: case 0xC5: case 0xC6: case 0xC7:
        case 0xC9: case 0xCA: case 0xCB: case 0xCD: case 0xCE: case 0xCF:
            return -1; /* progressive, lossless and arithmetic are not used */
        case 0xC4:
            if (read_dht(d, p + 4, len - 2) != 0)
                return -1;
            break;
        case 0xDB:
            if (read_dqt(d, p + 4, len - 2) != 0)
                return -1;
            break;
        case 0xDD:
            d->restart_interval = xf_be16(p + 4);
            break;
        case 0xDA:
            if (!have_frame || read_scan(d, p + 4, len - 2) != 0)
                return -1;
            to_rgb(d, out, out_w, out_h, pitch);
            return 0;
        }
        p += 2 + len;
    }
    return -1;
}

int xf_jpeg_info(const u8 *data, size_t size, int *width, int *height)
{
    jpeg_dec d;
    int r;
    memset(&d, 0, sizeof(d));
    r = run(&d, data, size, NULL, 0, 0, 0);
    if (r == 0) {
        *width = d.width;
        *height = d.height;
    }
    free_dec(&d);
    return r;
}

int xf_jpeg_decode(const u8 *data, size_t size, u32 *out, int out_w, int out_h, int pitch)
{
    jpeg_dec *d = calloc(1, sizeof(*d));
    int r;
    if (!d)
        return -1;
    init_defaults(d);
    r = run(d, data, size, out, out_w, out_h, pitch);
    free_dec(d);
    free(d);
    return r;
}
