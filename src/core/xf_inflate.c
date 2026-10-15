/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf and contributors */
#include "xf_inflate.h"

#include <string.h>

#define MAX_BITS 15

typedef struct {
    const u8 *src;
    size_t src_size, src_pos;
    u32 bits;
    int bit_count;
    u8 *dst;
    size_t dst_size, dst_pos;
} inflate_state;

typedef struct {
    u16 count[MAX_BITS + 1]; /* codes per length */
    u16 symbol[288];         /* symbols ordered by code */
} huffman;

static int need_bits(inflate_state *s, int n)
{
    while (s->bit_count < n) {
        if (s->src_pos >= s->src_size)
            return -1;
        s->bits |= (u32)s->src[s->src_pos++] << s->bit_count;
        s->bit_count += 8;
    }
    return 0;
}

static int get_bits(inflate_state *s, int n)
{
    int v;
    if (n == 0)
        return 0;
    if (need_bits(s, n) != 0)
        return -1;
    v = (int)(s->bits & ((1u << n) - 1));
    s->bits >>= n;
    s->bit_count -= n;
    return v;
}

static int build_huffman(huffman *h, const u8 *lengths, int n)
{
    u16 offs[MAX_BITS + 1];
    int i, left = 1;

    memset(h->count, 0, sizeof(h->count));
    for (i = 0; i < n; i++)
        h->count[lengths[i]]++;
    if (h->count[0] == n)
        return 0;
    for (i = 1; i <= MAX_BITS; i++) {
        left = left * 2 - h->count[i];
        if (left < 0)
            return -1; /* over-subscribed */
    }
    offs[1] = 0;
    for (i = 1; i < MAX_BITS; i++)
        offs[i + 1] = offs[i] + h->count[i];
    for (i = 0; i < n; i++)
        if (lengths[i])
            h->symbol[offs[lengths[i]]++] = (u16)i;
    return 0;
}

/* Canonical Huffman decode, one bit at a time. */
static int decode_symbol(inflate_state *s, const huffman *h)
{
    int code = 0, first = 0, index = 0, len;
    for (len = 1; len <= MAX_BITS; len++) {
        int b = get_bits(s, 1);
        int count;
        if (b < 0)
            return -1;
        code |= b;
        count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static const u16 len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                  35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const u8 len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                  2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const u16 dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                   193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
                                   4097, 6145, 8193, 12289, 16385, 24577 };
static const u8 dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                   6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int inflate_codes(inflate_state *s, const huffman *lit, const huffman *dist)
{
    for (;;) {
        int sym = decode_symbol(s, lit);
        if (sym < 0)
            return -1;
        if (sym < 256) {
            if (s->dst_pos >= s->dst_size)
                return -1;
            s->dst[s->dst_pos++] = (u8)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            int len, d, e;
            size_t from;
            sym -= 257;
            if (sym >= 29)
                return -1;
            e = get_bits(s, len_extra[sym]);
            if (e < 0)
                return -1;
            len = len_base[sym] + e;
            sym = decode_symbol(s, dist);
            if (sym < 0 || sym >= 30)
                return -1;
            e = get_bits(s, dist_extra[sym]);
            if (e < 0)
                return -1;
            d = dist_base[sym] + e;
            if ((size_t)d > s->dst_pos || s->dst_pos + (size_t)len > s->dst_size)
                return -1;
            from = s->dst_pos - (size_t)d;
            while (len--)
                s->dst[s->dst_pos++] = s->dst[from++];
        }
    }
}

static int inflate_stored(inflate_state *s)
{
    size_t len;
    s->bits = 0;
    s->bit_count = 0; /* skip to byte boundary */
    if (s->src_pos + 4 > s->src_size)
        return -1;
    len = s->src[s->src_pos] | (size_t)s->src[s->src_pos + 1] << 8;
    if ((len ^ 0xffff) != (size_t)(s->src[s->src_pos + 2] | s->src[s->src_pos + 3] << 8))
        return -1;
    s->src_pos += 4;
    if (s->src_pos + len > s->src_size || s->dst_pos + len > s->dst_size)
        return -1;
    memcpy(s->dst + s->dst_pos, s->src + s->src_pos, len);
    s->src_pos += len;
    s->dst_pos += len;
    return 0;
}

static int inflate_fixed(inflate_state *s)
{
    static huffman lit, dist;
    static int built;
    if (!built) {
        u8 l[288];
        int i;
        for (i = 0; i < 144; i++) l[i] = 8;
        for (; i < 256; i++) l[i] = 9;
        for (; i < 280; i++) l[i] = 7;
        for (; i < 288; i++) l[i] = 8;
        build_huffman(&lit, l, 288);
        for (i = 0; i < 30; i++) l[i] = 5;
        build_huffman(&dist, l, 30);
        built = 1;
    }
    return inflate_codes(s, &lit, &dist);
}

static int inflate_dynamic(inflate_state *s)
{
    static const u8 order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    u8 lengths[320];
    huffman lencode, lit, dist;
    int nlen, ndist, ncode, i;

    nlen = get_bits(s, 5) + 257;
    ndist = get_bits(s, 5) + 1;
    ncode = get_bits(s, 4) + 4;
    if (nlen < 257 || ndist < 1 || ncode < 4 || nlen > 286 || ndist > 30)
        return -1;
    memset(lengths, 0, sizeof(lengths));
    for (i = 0; i < ncode; i++) {
        int v = get_bits(s, 3);
        if (v < 0)
            return -1;
        lengths[order[i]] = (u8)v;
    }
    if (build_huffman(&lencode, lengths, 19) != 0)
        return -1;
    i = 0;
    while (i < nlen + ndist) {
        int sym = decode_symbol(s, &lencode), rep, val = 0;
        if (sym < 0)
            return -1;
        if (sym < 16) {
            lengths[i++] = (u8)sym;
            continue;
        }
        if (sym == 16) {
            if (i == 0)
                return -1;
            val = lengths[i - 1];
            rep = 3 + get_bits(s, 2);
        } else if (sym == 17) {
            rep = 3 + get_bits(s, 3);
        } else {
            rep = 11 + get_bits(s, 7);
        }
        if (rep < 3 || i + rep > nlen + ndist)
            return -1;
        while (rep--)
            lengths[i++] = (u8)val;
    }
    if (lengths[256] == 0)
        return -1;
    if (build_huffman(&lit, lengths, nlen) != 0 || build_huffman(&dist, lengths + nlen, ndist) != 0)
        return -1;
    return inflate_codes(s, &lit, &dist);
}

long xf_inflate(u8 *dst, size_t dst_size, const u8 *src, size_t src_size)
{
    inflate_state s;
    int last;

    memset(&s, 0, sizeof(s));
    s.src = src;
    s.src_size = src_size;
    s.dst = dst;
    s.dst_size = dst_size;
    do {
        int type, r;
        last = get_bits(&s, 1);
        type = get_bits(&s, 2);
        if (last < 0 || type < 0)
            return -1;
        if (type == 0)
            r = inflate_stored(&s);
        else if (type == 1)
            r = inflate_fixed(&s);
        else if (type == 2)
            r = inflate_dynamic(&s);
        else
            return -1;
        if (r != 0)
            return -1;
    } while (!last);
    return (long)s.dst_pos;
}

long xf_zlib_uncompress(u8 *dst, size_t dst_size, const u8 *src, size_t src_size)
{
    if (src_size < 2 || (src[0] & 0x0f) != 8 || ((src[0] << 8) | src[1]) % 31 != 0 || (src[1] & 0x20))
        return -1;
    return xf_inflate(dst, dst_size, src + 2, src_size - 2);
}
