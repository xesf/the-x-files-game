/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* TrueType font reader and antialiased rasterizer for the game's .ttr
 * fonts (glyf outlines, cmap formats 0, 4 and 6), and TrueType
 * collections (.ttc: the first font). */
#ifndef XF_TTF_H
#define XF_TTF_H

#include "xf_types.h"

typedef struct xf_font {
    u8 *data;
    size_t size;
    /* Characters this font lacks are drawn with this one (a Japanese
     * font under the game's Latin ones); set after loading. */
    const struct xf_font *fallback;
    u32 glyf, loca, hmtx, cmap_sub;
    int cmap_format;       /* 0, 4 or 6 */
    int cmap_mac_roman;    /* 1 when the cmap maps Mac Roman bytes */
    int loca_long, num_glyphs, num_hmetrics;
    int units_per_em, ascent, descent, line_gap;
} xf_font;

typedef struct {
    int width, height;     /* bitmap size */
    int x0, y0;            /* bitmap offset from the pen position (y down) */
    int advance;           /* in pixels, rounded */
    u8 *alpha;             /* width * height coverage, freed with free() */
} xf_glyph;

/* Takes ownership of `data` (from malloc). Returns 0 on success. */
int xf_font_init(xf_font *f, u8 *data, size_t size);
int xf_font_load(xf_font *f, const char *path);
void xf_font_free(xf_font *f);
/* Glyph index for a Unicode code point, 0 if missing. */
int xf_font_glyph_index(const xf_font *f, u32 codepoint);
/* Renders a glyph at `px` pixels per em. Returns 0 on success. */
int xf_font_render(const xf_font *f, int glyph, float px, xf_glyph *out);
/* Horizontal advance of a glyph in pixels at `px` per em. */
float xf_font_advance(const xf_font *f, int glyph, float px);

#endif
