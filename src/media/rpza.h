/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* Apple Video ('rpza', "Road Pizza") decoder. */
#ifndef XF_RPZA_H
#define XF_RPZA_H

#include "xf_types.h"

/* Decodes one frame into `frame` (0x00RRGGBB), which must hold the
 * previous frame: skipped blocks keep their pixels. Returns 0 on success. */
int xf_rpza_decode(const u8 *data, size_t size, u32 *frame, int width, int height);

#endif
