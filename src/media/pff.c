/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
#include "pff.h"

#include <stdlib.h>
#include <string.h>

#include "xf_endian.h"
#include "xf_log.h"

int xf_pff_open(xf_pff *pff, const char *path)
{
    u8 head[8];
    u8 *table;
    u32 i;

    memset(pff, 0, sizeof(*pff));
    pff->file = xf_open(path);
    if (!pff->file)
        return -1;
    if (xf_read(pff->file, head, 8) != 0 || memcmp(head, "PFF ", 4) != 0)
        goto fail;
    pff->count = xf_le32(head + 4);
    if (pff->count == 0 || 8 + (u64)(pff->count + 1) * 4 > xf_size(pff->file))
        goto fail;
    pff->offsets = malloc((pff->count + 1) * sizeof(u32));
    table = malloc((pff->count + 1) * 4);
    if (!pff->offsets || !table || xf_read(pff->file, table, (pff->count + 1) * 4) != 0) {
        free(table);
        goto fail;
    }
    for (i = 0; i <= pff->count; i++)
        pff->offsets[i] = xf_le32(table + i * 4);
    free(table);
    for (i = 0; i < pff->count; i++)
        if (pff->offsets[i] > pff->offsets[i + 1] || pff->offsets[i + 1] > xf_size(pff->file))
            goto fail;
    return 0;

fail:
    xf_error("%s: not a valid PFF file", path);
    xf_pff_close(pff);
    return -1;
}

void xf_pff_close(xf_pff *pff)
{
    xf_close(pff->file);
    free(pff->offsets);
    memset(pff, 0, sizeof(*pff));
}

u32 xf_pff_entry_size(const xf_pff *pff, u32 index)
{
    return index < pff->count ? pff->offsets[index + 1] - pff->offsets[index] : 0;
}

u8 *xf_pff_read(xf_pff *pff, u32 index, u32 *size)
{
    u32 n = xf_pff_entry_size(pff, index);
    u8 *buf;

    if (n == 0)
        return NULL;
    buf = malloc(n);
    if (!buf || xf_read_at(pff->file, pff->offsets[index], buf, n) != 0) {
        free(buf);
        return NULL;
    }
    if (size)
        *size = n;
    return buf;
}
