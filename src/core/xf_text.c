/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
#include "xf_text.h"

#include <string.h>

/* 0x80-0x9F differ from Latin-1; 0 marks bytes that are not defined. */
static const u16 g_cp1252_80[32] = {
    0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178,
};

u32 xf_cp1252_to_unicode(u8 c)
{
    if (c >= 0x80 && c < 0xA0)
        return g_cp1252_80[c - 0x80] ? g_cp1252_80[c - 0x80] : 0xFFFD;
    return c;
}

static const u16 g_macroman_80[128] = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4, 0x00E3,
    0x00E5, 0x00E7, 0x00E9, 0x00E8, 0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3,
    0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC, 0x2020, 0x00B0, 0x00A2, 0x00A3,
    0x00A7, 0x2022, 0x00B6, 0x00DF, 0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
    0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211, 0x220F, 0x03C0, 0x222B, 0x00AA,
    0x00BA, 0x03A9, 0x00E6, 0x00F8, 0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB,
    0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153, 0x2013, 0x2014, 0x201C, 0x201D,
    0x2018, 0x2019, 0x00F7, 0x25CA, 0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
    0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1, 0x00CB, 0x00C8, 0x00CD, 0x00CE,
    0x00CF, 0x00CC, 0x00D3, 0x00D4, 0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC,
    0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7,
};

u32 xf_macroman_to_unicode(u8 c) { return c < 0x80 ? c : g_macroman_80[c - 0x80]; }

/* Encodes text from a single-byte character set as UTF-8. */
static size_t to_utf8(char *dst, size_t dst_size, const u8 *src, size_t src_size, u32 (*map)(u8))
{
    size_t i, o = 0;

    if (dst_size == 0)
        return 0;
    for (i = 0; i < src_size; i++) {
        u32 u = map(src[i]);
        int n = u < 0x80 ? 1 : u < 0x800 ? 2 : 3;
        if (o + n >= dst_size)
            break;
        if (n == 1) {
            dst[o++] = (char)u;
        } else if (n == 2) {
            dst[o++] = (char)(0xC0 | u >> 6);
            dst[o++] = (char)(0x80 | (u & 0x3F));
        } else {
            dst[o++] = (char)(0xE0 | u >> 12);
            dst[o++] = (char)(0x80 | (u >> 6 & 0x3F));
            dst[o++] = (char)(0x80 | (u & 0x3F));
        }
    }
    dst[o] = '\0';
    return o;
}

size_t xf_cp1252_to_utf8(char *dst, size_t dst_size, const u8 *src, size_t src_size)
{
    return to_utf8(dst, dst_size, src, src_size, xf_cp1252_to_unicode);
}

size_t xf_macroman_to_utf8(char *dst, size_t dst_size, const u8 *src, size_t src_size)
{
    return to_utf8(dst, dst_size, src, src_size, xf_macroman_to_unicode);
}

/* The next code point of UTF-8 text, or -1 when the bytes are not UTF-8. */
static long utf8_next(const u8 *s, size_t n, size_t *len)
{
    u32 c = s[0], need, i;
    if (c < 0x80) {
        *len = 1;
        return (long)c;
    }
    need = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC2 ? 1 : 0;
    if (!need || need >= n || c >= 0xF5)
        return -1;
    c &= 0x3F >> need;
    for (i = 1; i <= need; i++) {
        if ((s[i] & 0xC0) != 0x80)
            return -1;
        c = c << 6 | (s[i] & 0x3F);
    }
    *len = need + 1;
    return (long)c;
}

size_t xf_fold_ascii(char *dst, size_t size, const char *src)
{
    /* the base letters of U+00C0..U+00FF; '?' where there is none */
    static const char base[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    const u8 *s = (const u8 *)src;
    size_t n = 0, len;
    if (!size)
        return 0;
    while (*s && n + 1 < size) {
        size_t rest = strlen((const char *)s);
        long c = utf8_next(s, rest, &len);
        if (c < 0) { /* Windows-1252 */
            c = (long)xf_cp1252_to_unicode(*s);
            len = 1;
        }
        s += len;
        if (c >= 0xC0 && c <= 0xFF)
            c = base[c - 0xC0];
        else if (c >= 0x80)
            continue;
        dst[n++] = (char)(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    dst[n] = '\0';
    return n;
}

#include "xf_sjis_table.h"

/* One code point as UTF-8; returns the bytes written, 0 when it does not fit. */
static size_t put_utf8(char *dst, size_t room, u32 u)
{
    if (u < 0x80 && room > 1) {
        dst[0] = (char)u;
        return 1;
    }
    if (u < 0x800 && room > 2) {
        dst[0] = (char)(0xC0 | u >> 6);
        dst[1] = (char)(0x80 | (u & 0x3F));
        return 2;
    }
    if (u >= 0x800 && room > 3) {
        dst[0] = (char)(0xE0 | u >> 12);
        dst[1] = (char)(0x80 | (u >> 6 & 0x3F));
        dst[2] = (char)(0x80 | (u & 0x3F));
        return 3;
    }
    return 0;
}

size_t xf_sjis_to_utf8(char *dst, size_t dst_size, const u8 *src, size_t src_size)
{
    size_t i = 0, o = 0, n;
    if (dst_size == 0)
        return 0;
    while (i < src_size) {
        u32 c = src[i++], u = '?';
        if (c < 0x80) {
            u = c;
        } else if (c >= 0xA1 && c <= 0xDF) {
            u = 0xFF61 + (c - 0xA1); /* half-width katakana */
        } else if (((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) && i < src_size) {
            u32 t = src[i++], lead = c <= 0x9F ? c - 0x81 : c - 0xE0 + 31;
            if (t >= 0x40 && t <= 0xFC && g_sjis_double[lead][t - 0x40])
                u = g_sjis_double[lead][t - 0x40];
        }
        if (!(n = put_utf8(dst + o, dst_size - o, u)))
            break;
        o += n;
    }
    dst[o] = '\0';
    return o;
}

int xf_is_utf8(const char *text, size_t size)
{
    size_t r, len;
    for (r = 0; r < size; r += len)
        if (utf8_next((const u8 *)text + r, size - r, &len) < 0)
            return 0;
    return 1;
}
