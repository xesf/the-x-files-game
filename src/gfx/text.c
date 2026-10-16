/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
#include "text.h"

#include "xf_text.h"

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

/* Decodes one code point and advances *p: UTF-8, or a Windows-1252
 * byte where the bytes are not UTF-8 (the game's documents). */
static u32 next_cp(const char **p, const char *end)
{
    const u8 *s = (const u8 *)*p;
    u32 c = *s++;
    int extra = c >= 0xF5 ? 0 : c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC2 ? 1 : 0, k;
    if (c >= 0x80 && !extra) {
        *p = (const char *)s;
        return xf_cp1252_to_unicode((u8)c);
    }
    for (k = 0; k < extra; k++)
        if ((const char *)s + k >= end || (s[k] & 0xC0) != 0x80) {
            *p = (const char *)s;
            return xf_cp1252_to_unicode((u8)c);
        }
    if (extra)
        c &= 0x3F >> extra;
    while (extra--)
        c = c << 6 | (*s++ & 0x3F);
    *p = (const char *)s;
    return c;
}

/* The glyph of a character: in the font, else in its fallback (which
 * then becomes *f). */
static int glyph_of(const xf_font **f, u32 cp)
{
    int gi = xf_font_glyph_index(*f, cp), alt;
    if (!gi && cp > ' ' && (*f)->fallback && (alt = xf_font_glyph_index((*f)->fallback, cp)) != 0) {
        *f = (*f)->fallback;
        return alt;
    }
    return gi;
}

int xf_text_width(const xf_font *f, float px, const char *utf8, size_t len)
{
    const char *p = utf8, *end = utf8 + len;
    float w = 0;
    while (p < end) {
        const xf_font *g = f;
        int gi = glyph_of(&g, next_cp(&p, end));
        w += xf_font_advance(g, gi, px);
    }
    return (int)(w + 0.5f);
}

/* Characters a line may break before or after without a space: the
 * Japanese and Chinese scripts and their punctuation. */
static int breaks_anywhere(u32 cp)
{
    /* never before closing punctuation (kinsoku): 、。，．」』）】！？ー… */
    static const u32 no_start[] = { 0x3001, 0x3002, 0xFF0C, 0xFF0E, 0x300D, 0x300F, 0xFF09, 0x3011,
                                    0xFF01, 0xFF1F, 0x30FC, 0x2026, 0x30FB, 0xFF1A, 0x3009, 0x300B };
    size_t i;
    if (cp < 0x2E80 || cp >= 0xFFF0)
        return 0;
    for (i = 0; i < sizeof(no_start) / sizeof(no_start[0]); i++)
        if (cp == no_start[i])
            return 0;
    return 1;
}

int xf_text_wrap(const xf_font *f, float px, const char *text, int max_w, int *starts, int *lens, int max)
{
    int n = 0, pos = 0, len = (int)strlen(text);
    while (pos < len && n < max) {
        const char *p = text + pos, *end = text + len;
        int fit = pos, brk = -1;
        float w = 0;
        while (p < end && *p != '\n') {
            const char *q = p;
            const xf_font *g = f;
            u32 cp = next_cp(&q, end);
            int gi = glyph_of(&g, cp); /* switches g to the fallback font */
            float a = xf_font_advance(g, gi, px);
            if (w + a > (float)max_w && fit > pos)
                break;
            if (*p == ' ')
                brk = (int)(p - text); /* before the space */
            else if (breaks_anywhere(cp) && p > text + pos)
                brk = (int)(p - text); /* before a Japanese character */
            w += a;
            p = q;
            fit = (int)(p - text);
        }
        if (p < end && *p != '\n' && brk > pos)
            fit = brk; /* the line is full: back to the last break */
        starts[n] = pos;
        lens[n] = fit - pos;
        n++;
        pos = fit;
        while (pos < len && text[pos] == ' ')
            pos++;
        if (pos < len && text[pos] == '\n')
            pos++;
    }
    return n;
}

void xf_text_draw(xf_surface *s, const xf_font *f, float px, int x, int y, const char *utf8,
                  size_t len, u32 color)
{
    const char *p = utf8, *end = utf8 + len;
    float pen = (float)x;
    u32 cr = color >> 16 & 255, cg = color >> 8 & 255, cb = color & 255;

    while (p < end) {
        const xf_font *gf = f;
        int gi = glyph_of(&gf, next_cp(&p, end)), gx, gy;
        const xf_glyph *g = get_glyph(gf, gi, px);
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
        pen += xf_font_advance(gf, gi, px);
    }
}

int xf_text_draw_wrapped(xf_surface *s, const xf_font *f, float px, int cx, int y, int max_w,
                         const char *utf8, u32 color, u32 shadow)
{
    int line_h = (int)((f->ascent - f->descent + f->line_gap) * px / f->units_per_em + 0.5f);
    int starts[32], lens[32], n = xf_text_wrap(f, px, utf8, max_w, starts, lens, 32), k;
    for (k = 0; k < n; k++) {
        const char *line = utf8 + starts[k];
        size_t len = (size_t)lens[k];
        int w;
        while (len && line[len - 1] == ' ')
            len--;
        w = xf_text_width(f, px, line, len);
        if (shadow != color)
            xf_text_draw(s, f, px, cx - w / 2 + 1, y + 1, line, len, shadow);
        xf_text_draw(s, f, px, cx - w / 2, y, line, len, color);
        y += line_h;
    }
    return n;
}
