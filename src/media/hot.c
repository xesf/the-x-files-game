/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
#include "hot.h"

#include <stdlib.h>
#include <string.h>

#include "xf_endian.h"
#include "xf_file.h"

#define HOT_RECORD_SIZE 20

int xf_hot_parse(xf_hot *hot, const u8 *data, size_t size)
{
    u32 i;

    memset(hot, 0, sizeof(*hot));
    if (size < 8 || memcmp(data, "HSPT", 4) != 0)
        return -1;
    hot->count = xf_le32(data + 4);
    if (8 + (u64)hot->count * HOT_RECORD_SIZE > size)
        return -1;
    hot->spots = calloc(hot->count ? hot->count : 1, sizeof(xf_hotspot));
    if (!hot->spots)
        return -1;
    for (i = 0; i < hot->count; i++) {
        const u8 *p = data + 8 + i * HOT_RECORD_SIZE;
        xf_hotspot *s = &hot->spots[i];
        s->id = xf_le32(p);
        s->left = xf_le16(p + 4);
        s->top = xf_le16(p + 6);
        s->right = xf_le16(p + 8);
        s->bottom = xf_le16(p + 10);
        s->start = xf_le32(p + 12);
        s->end = xf_le32(p + 16);
    }
    return 0;
}

int xf_hot_load(xf_hot *hot, const char *path)
{
    size_t size;
    u8 *data = xf_load(path, &size);
    int r;

    if (!data)
        return -1;
    r = xf_hot_parse(hot, data, size);
    free(data);
    return r;
}

void xf_hot_free(xf_hot *hot)
{
    free(hot->spots);
    memset(hot, 0, sizeof(*hot));
}
