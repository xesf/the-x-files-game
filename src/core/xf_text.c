#include "xf_text.h"

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

size_t xf_cp1252_to_utf8(char *dst, size_t dst_size, const u8 *src, size_t src_size)
{
    size_t i, o = 0;

    if (dst_size == 0)
        return 0;
    for (i = 0; i < src_size; i++) {
        u32 u = xf_cp1252_to_unicode(src[i]);
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
