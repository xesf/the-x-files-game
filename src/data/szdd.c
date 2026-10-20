/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
#include "szdd.h"

#include <stdlib.h>
#include <string.h>

#include "xf_endian.h"
#include "xf_file.h"

/* "SZDD", 0x88 0xF0 0x27 0x33, the mode 'A', the name's last character,
 * the unpacked size (u32, little-endian), then the packed data. */
#define SZDD_HEADER 14
#define WINDOW 4096

int xf_szdd_is(const u8 *data, size_t size)
{
    static const u8 sig[8] = { 'S', 'Z', 'D', 'D', 0x88, 0xF0, 0x27, 0x33 };
    return size >= SZDD_HEADER && memcmp(data, sig, 8) == 0 && data[8] == 'A';
}

u32 xf_szdd_size(const u8 *data, size_t size)
{
    return xf_szdd_is(data, size) ? xf_le32(data + 10) : 0;
}

char xf_szdd_last_char(const u8 *data, size_t size)
{
    return xf_szdd_is(data, size) ? (char)data[9] : 0;
}

/* LZSS over a 4096-byte window filled with spaces, written from position
 * 4096 - 16. Each flag byte, low bit first, says whether the next item
 * is a literal byte (1) or a match (0): two bytes, the window position
 * in the low byte and the high nibble of the second, the length - 3 in
 * its low nibble. */
long xf_szdd_expand(const u8 *data, size_t size, u8 *out, size_t out_size)
{
    u8 window[WINDOW];
    size_t in = SZDD_HEADER, n = 0, want;
    int pos = WINDOW - 16, bit;

    if (!xf_szdd_is(data, size))
        return -1;
    want = xf_szdd_size(data, size);
    if (want > out_size)
        return -1;
    memset(window, ' ', sizeof(window));
    while (n < want && in < size) {
        u8 flags = data[in++];
        for (bit = 0; bit < 8 && n < want; bit++) {
            if (flags >> bit & 1) {
                if (in >= size)
                    return -1;
                out[n++] = window[pos] = data[in++];
                pos = (pos + 1) & (WINDOW - 1);
            } else {
                int from, len, k;
                if (in + 2 > size)
                    return -1;
                from = data[in] | (data[in + 1] & 0xF0) << 4;
                len = (data[in + 1] & 0x0F) + 3;
                in += 2;
                for (k = 0; k < len && n < want; k++) {
                    u8 c = window[(from + k) & (WINDOW - 1)];
                    out[n++] = window[pos] = c;
                    pos = (pos + 1) & (WINDOW - 1);
                }
            }
        }
    }
    return n == want ? (long)n : -1;
}

u8 *xf_szdd_load(const char *path, size_t *size)
{
    size_t packed;
    u8 *data = xf_load(path, &packed), *out;
    u32 n;

    if (!data || !xf_szdd_is(data, packed)) {
        *size = packed;
        return data;
    }
    n = xf_szdd_size(data, packed);
    out = malloc(n ? n : 1);
    if (!out || xf_szdd_expand(data, packed, out, n) != (long)n) {
        free(out);
        free(data);
        return NULL;
    }
    free(data);
    *size = n;
    return out;
}
