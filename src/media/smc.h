/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* Apple Graphics ('smc ') decoder: 8-bit palettized 4x4 blocks with
 * colour caches. Roland's clips use it. */
#ifndef XF_SMC_H
#define XF_SMC_H

#include "xf_types.h"

typedef struct xf_smc_state {
    u8 pairs[256][2], quads[256][4], octets[256][8];
    int next_pair, next_quad, next_octet;
} xf_smc;

/* The Macintosh system palette (clut 8), used when a movie keeps no
 * colour table of its own. */
void xf_mac_palette(u32 *pal256);
/* Decodes one frame into `frame` (0x00RRGGBB), which must hold the
 * previous frame: skipped blocks keep their pixels. Returns 0 on success. */
int xf_smc_decode(xf_smc *s, const u8 *data, size_t size, const u32 *palette, u32 *frame, int width, int height);

#endif
