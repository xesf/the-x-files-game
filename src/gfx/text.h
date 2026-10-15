/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf and contributors */
/* Draws UTF-8 text with a TrueType font into a 0x00RRGGBB framebuffer. */
#ifndef XF_TEXT_DRAW_H
#define XF_TEXT_DRAW_H

#include "gfx/ttf.h"

typedef struct {
    u32 *pixels;
    int width, height, pitch;
} xf_surface;

/* Width of a line in pixels. */
int xf_text_width(const xf_font *f, float px, const char *utf8, size_t len);
/* Draws one line with its baseline at `y`. */
void xf_text_draw(xf_surface *s, const xf_font *f, float px, int x, int y, const char *utf8,
                  size_t len, u32 color);
/* Draws text wrapped to `max_w`, each line centred on `cx`, starting with
 * the first baseline at `y`. Returns the number of lines. */
int xf_text_draw_wrapped(xf_surface *s, const xf_font *f, float px, int cx, int y, int max_w,
                         const char *utf8, u32 color, u32 shadow);
void xf_text_cache_clear(void);

#endif
