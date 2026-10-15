/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* Cinepak ('cvid') video decoder. */
#ifndef XF_CINEPAK_H
#define XF_CINEPAK_H

#include "xf_types.h"

typedef struct xf_cinepak xf_cinepak;

xf_cinepak *xf_cinepak_create(int width, int height);
void xf_cinepak_destroy(xf_cinepak *c);
/* Decodes one frame into `frame` (0x00RRGGBB, width x height). The frame
 * must keep the previous picture: inter frames only update changed
 * blocks. Returns 0 on success. */
int xf_cinepak_decode(xf_cinepak *c, const u8 *data, size_t size, u32 *frame);

#endif
