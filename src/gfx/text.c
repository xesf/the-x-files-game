/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
#include "text.h"

#include <stdlib.h>
#include <string.h>

#define CACHE_SIZE 256

typedef struct {
    const xf_font *font;
    int glyph;
    float px;
    xf_glyph g;
    int used;
} cache_entry;

static cache_entry g_cache[CACHE_SIZE];

void xf_text_cache_clear(void)
{
    int i;
    for (i = 0; i < CACHE_SIZE; i++)
        free(g_cache[i].g.alpha);
    memset(g_cache, 0, sizeof(g_cache));
}

static const xf_glyph *get_glyph(const xf_font *f, int glyph, float px)
{
    unsigned h = ((unsigned)glyph * 31u + (unsigned)(px * 8)) % CACHE_SIZE;
    cache_entry *e = &g_cache[h];
    if (e->used && e->font == f && e->glyph == glyph && e->px == px)
        return &e->g;
    free(e->g.alpha);
    memset(e, 0, sizeof(*e));
    if (xf_font_render(f, glyph, px, &e->g) != 0)
        return NULL;
    e->font = f;
    e->glyph = glyph;
    e->px = px;
    e->used = 1;
    return &e->g;
}

/* Decodes one UTF-8 code point and advances *p. */
static u32 next_cp(const char **p, const char *end)
{
    const u8 *s = (const u8 *)*p;
    u32 c = *s++;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (extra)
        c &= 0x3F >> extra;
    while (extra-- && (const char *)s < end)
        c = c << 6 | (*s++ & 0x3F);
    *p = (const char *)s;
    return c;
}

int xf_text_width(const xf_font *f, float px, const char *utf8, size_t len)
{
    const char *p = utf8, *end = utf8 + len;
    float w = 0;
    while (p < end)
        w += xf_font_advance(f, xf_font_glyph_index(f, next_cp(&p, end)), px);
    return (int)(w + 0.5f);
}

void xf_text_draw(xf_surface *s, const xf_font *f, float px, int x, int y, const char *utf8,
                  size_t len, u32 color)
{
    const char *p = utf8, *end = utf8 + len;
    float pen = (float)x;
    u32 cr = color >> 16 & 255, cg = color >> 8 & 255, cb = color & 255;

    while (p < end) {
        int gi = xf_font_glyph_index(f, next_cp(&p, end)), gx, gy;
        const xf_glyph *g = get_glyph(f, gi, px);
        if (g && g->alpha) {
            int ox = (int)(pen + 0.5f) + g->x0, oy = y + g->y0;
            for (gy = 0; gy < g->height; gy++) {
                int sy = oy + gy;
                if (sy < 0 || sy >= s->height)
                    continue;
                for (gx = 0; gx < g->width; gx++) {
                    int sx = ox + gx;
                    u32 a = g->alpha[gy * g->width + gx], d, r, gg, b;
                    if (!a || sx < 0 || sx >= s->width)
                        continue;
                    d = s->pixels[sy * s->pitch + sx];
                    r = ((d >> 16 & 255) * (255 - a) + cr * a) / 255;
                    gg = ((d >> 8 & 255) * (255 - a) + cg * a) / 255;
                    b = ((d & 255) * (255 - a) + cb * a) / 255;
                    s->pixels[sy * s->pitch + sx] = r << 16 | gg << 8 | b;
                }
            }
        }
        pen += xf_font_advance(f, gi, px);
    }
}

int xf_text_draw_wrapped(xf_surface *s, const xf_font *f, float px, int cx, int y, int max_w,
                         const char *utf8, u32 color, u32 shadow)
{
    const char *line = utf8;
    int lines = 0, line_h = (int)((f->ascent - f->descent + f->line_gap) * px / f->units_per_em + 0.5f);

    while (*line) {
        const char *p = line, *brk = NULL;
        size_t n;
        int w;
        /* Take words while they fit. */
        while (*p && *p != '\n') {
            const char *q = p;
            while (*q == ' ')
                q++;
            while (*q && *q != ' ' && *q != '\n')
                q++;
            if (brk && xf_text_width(f, px, line, (size_t)(q - line)) > max_w)
                break;
            brk = q;
            p = q;
        }
        if (!brk)
            brk = p;
        n = (size_t)(brk - line);
        while (n && line[n - 1] == ' ')
            n--;
        w = xf_text_width(f, px, line, n);
        if (shadow != color)
            xf_text_draw(s, f, px, cx - w / 2 + 1, y + 1, line, n, shadow);
        xf_text_draw(s, f, px, cx - w / 2, y, line, n, color);
        lines++;
        y += line_h;
        line = brk;
        while (*line == ' ' || *line == '\n')
            line++;
    }
    return lines;
}
