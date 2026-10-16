/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* The Vita's GPU (sceGxm) draws the game's frame: copied into a texture
 * and drawn scaled with bilinear filtering, so the CPU only copies rows. */
#ifndef XF_VITA_GPU_H
#define XF_VITA_GPU_H

#include "xf_types.h"

/* Sets up the GPU and its display buffers. Returns 0, or -1 (the caller
 * then draws with the CPU). */
int vita_gpu_init(void);
void vita_gpu_shutdown(void);
/* Shows a 0x00RRGGBB frame scaled to x, y, w, h on the 960x544 screen,
 * black around it. */
void vita_gpu_present(const u32 *pixels, int width, int height, int pitch, int x, int y, int w, int h);
/* While a system dialog (the keyboard) runs, each present also draws it. */
void vita_gpu_dialog(int running);

#endif
