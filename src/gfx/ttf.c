/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf and contributors */
#include "ttf.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "xf_endian.h"
#include "xf_file.h"
#include "xf_text.h"

#define MAX_POINTS 4096

static u32 find_table(const u8 *d, size_t size, const char *tag)
{
    u32 n = xf_be16(d + 4), i;
    for (i = 0; i < n && 12 + i * 16 + 16 <= size; i++) {
        const u8 *e = d + 12 + i * 16;
        if (memcmp(e, tag, 4) == 0) {
            u32 off = xf_be32(e + 8), len = xf_be32(e + 12);
            return (u64)off + len <= size ? off : 0;
        }
    }
    return 0;
}

int xf_font_init(xf_font *f, u8 *data, size_t size)
{
    u32 head, hhea, maxp, cmap, i, best = 0;
    int best_kind = 0;

    memset(f, 0, sizeof(*f));
    f->data = data;
    f->size = size;
    if (size < 12)
        return -1;
    head = find_table(data, size, "head");
    hhea = find_table(data, size, "hhea");
    maxp = find_table(data, size, "maxp");
    cmap = find_table(data, size, "cmap");
    f->glyf = find_table(data, size, "glyf");
    f->loca = find_table(data, size, "loca");
    f->hmtx = find_table(data, size, "hmtx");
    if (!head || !hhea || !maxp || !cmap || !f->glyf || !f->loca || !f->hmtx)
        return -1;
    f->units_per_em = xf_be16(data + head + 18);
    f->loca_long = xf_be16(data + head + 50) != 0;
    f->num_glyphs = xf_be16(data + maxp + 4);
    f->ascent = (i16)xf_be16(data + hhea + 4);
    f->descent = (i16)xf_be16(data + hhea + 6);
    f->line_gap = (i16)xf_be16(data + hhea + 8);
    f->num_hmetrics = xf_be16(data + hhea + 34);

    /* Prefer a Unicode cmap (Windows 3,1 or 3,0), else Macintosh Roman. */
    for (i = 0; i < xf_be16(data + cmap + 2); i++) {
        const u8 *e = data + cmap + 4 + i * 8;
        int pid = xf_be16(e), eid = xf_be16(e + 2), kind = 0;
        if (pid == 3 && eid == 1)
            kind = 3;
        else if (pid == 3 && eid == 0)
            kind = 2; /* symbol fonts: codes at 0xF000 + byte */
        else if (pid == 0)
            kind = 3;
        else if (pid == 1 && eid == 0)
            kind = 1;
        if (kind > best_kind) {
            best_kind = kind;
            best = cmap + xf_be32(e + 4);
        }
    }
    if (!best || best + 6 > size)
        return -1;
    f->cmap_sub = best;
    f->cmap_format = xf_be16(data + best);
    f->cmap_mac_roman = best_kind == 1;
    if (f->cmap_format != 0 && f->cmap_format != 4 && f->cmap_format != 6)
        return -1;
    return f->units_per_em > 0 ? 0 : -1;
}

int xf_font_load(xf_font *f, const char *path)
{
    size_t size;
    u8 *data = xf_load(path, &size);
    if (!data)
        return -1;
    if (xf_font_init(f, data, size) != 0) {
        xf_font_free(f);
        return -1;
    }
    return 0;
}

void xf_font_free(xf_font *f)
{
    free(f->data);
    memset(f, 0, sizeof(*f));
}

/* Unicode to Mac Roman byte, for fonts with only a Macintosh cmap. */
static int to_mac_roman(u32 cp)
{
    int c;
    if (cp < 0x80)
        return (int)cp;
    for (c = 0x80; c < 0x100; c++)
        if (xf_macroman_to_unicode((u8)c) == cp)
            return c;
    return -1;
}

int xf_font_glyph_index(const xf_font *f, u32 cp)
{
    const u8 *t = f->data + f->cmap_sub;
    if (f->cmap_mac_roman) {
        int c = to_mac_roman(cp);
        if (c < 0)
            return 0;
        cp = (u32)c;
    }
    if (f->cmap_format == 0)
        return cp < 256 ? t[6 + cp] : 0;
    if (f->cmap_format == 6) {
        u32 first = xf_be16(t + 6), count = xf_be16(t + 8);
        return cp >= first && cp < first + count ? xf_be16(t + 10 + (cp - first) * 2) : 0;
    }
    {
        int segs = xf_be16(t + 6) / 2, i;
        const u8 *ends = t + 14, *starts = ends + segs * 2 + 2, *deltas = starts + segs * 2,
                 *ranges = deltas + segs * 2;
        if (cp > 0xFFFF)
            return 0;
        for (i = 0; i < segs; i++) {
            u32 end = xf_be16(ends + i * 2), start = xf_be16(starts + i * 2);
            if (cp > end)
                continue;
            if (cp < start)
                return 0;
            {
                u16 delta = xf_be16(deltas + i * 2), range = xf_be16(ranges + i * 2);
                const u8 *g;
                if (range == 0)
                    return (u16)(cp + delta);
                g = ranges + i * 2 + range + (cp - start) * 2;
                if ((size_t)(g + 2 - f->data) > f->size)
                    return 0;
                return xf_be16(g) ? (u16)(xf_be16(g) + delta) : 0;
            }
        }
    }
    return 0;
}

float xf_font_advance(const xf_font *f, int glyph, float px)
{
    int i = glyph < f->num_hmetrics ? glyph : f->num_hmetrics - 1;
    return xf_be16(f->data + f->hmtx + i * 4) * px / f->units_per_em;
}

/* ---- outlines ---- */

typedef struct {
    float x, y;
} pt;

typedef struct {
    pt *lines; /* pairs of points: segment start, end */
    int count, cap;
} seglist;

static int add_line(seglist *s, pt a, pt b)
{
    if (s->count + 2 > s->cap) {
        int cap = s->cap ? s->cap * 2 : 256;
        pt *n = realloc(s->lines, (size_t)cap * sizeof(pt));
        if (!n)
            return -1;
        s->lines = n;
        s->cap = cap;
    }
    s->lines[s->count++] = a;
    s->lines[s->count++] = b;
    return 0;
}

static int add_quad(seglist *s, pt a, pt c, pt b)
{
    /* Split into enough lines that the error stays under a quarter pixel. */
    float dx = a.x - 2 * c.x + b.x, dy = a.y - 2 * c.y + b.y;
    int n = 1 + (int)sqrtf(sqrtf(dx * dx + dy * dy) * 4), i;
    pt prev = a;
    if (n > 16)
        n = 16;
    for (i = 1; i <= n; i++) {
        float t = (float)i / n, u = 1 - t;
        pt p;
        p.x = u * u * a.x + 2 * u * t * c.x + t * t * b.x;
        p.y = u * u * a.y + 2 * u * t * c.y + t * t * b.y;
        if (add_line(s, prev, p) != 0)
            return -1;
        prev = p;
    }
    return 0;
}

static u32 glyph_offset(const xf_font *f, int g, u32 *len)
{
    u32 a, b;
    *len = 0;
    if (g < 0 || g >= f->num_glyphs)
        return 0;
    if (f->loca_long) {
        a = xf_be32(f->data + f->loca + g * 4);
        b = xf_be32(f->data + f->loca + g * 4 + 4);
    } else {
        a = xf_be16(f->data + f->loca + g * 2) * 2u;
        b = xf_be16(f->data + f->loca + g * 2 + 2) * 2u;
    }
    *len = b > a ? b - a : 0;
    return f->glyf + a;
}

/* Appends the outline of glyph `g`, transformed by matrix m (a b c d e f:
 * x' = a x + c y + e, y' = b x + d y + f), in font units. */
static int add_glyph(const xf_font *f, int g, const float m[6], seglist *s, int depth)
{
    u32 len, off = glyph_offset(f, g, &len);
    const u8 *p;
    int contours;

    if (!len || depth > 8 || off + len > f->size)
        return 0;
    p = f->data + off;
    contours = (i16)xf_be16(p);
    if (contours >= 0) {
        const u8 *ends = p + 10, *q;
        int npts, i, c, ins;
        static u8 flags[MAX_POINTS];
        static pt pts[MAX_POINTS];
        i32 v;

        if (contours == 0)
            return 0;
        npts = xf_be16(ends + (contours - 1) * 2) + 1;
        if (npts > MAX_POINTS)
            return -1;
        ins = xf_be16(ends + contours * 2);
        q = ends + contours * 2 + 2 + ins;
        for (i = 0; i < npts;) {
            u8 fl = *q++;
            int rep = (fl & 8) ? *q++ : 0;
            while (rep-- >= 0 && i < npts)
                flags[i++] = fl;
        }
        for (v = 0, i = 0; i < npts; i++) {
            u8 fl = flags[i];
            if (fl & 2)
                v += (fl & 16) ? *q++ : -*q++;
            else if (!(fl & 16)) {
                v += (i16)xf_be16(q);
                q += 2;
            }
            pts[i].x = (float)v;
        }
        for (v = 0, i = 0; i < npts; i++) {
            u8 fl = flags[i];
            if (fl & 4)
                v += (fl & 32) ? *q++ : -*q++;
            else if (!(fl & 32)) {
                v += (i16)xf_be16(q);
                q += 2;
            }
            pts[i].y = (float)v;
        }
        for (i = 0; i < npts; i++) {
            pt o = pts[i];
            pts[i].x = m[0] * o.x + m[2] * o.y + m[4];
            pts[i].y = m[1] * o.x + m[3] * o.y + m[5];
        }
        /* Walk each contour; off-curve points are quadratic controls, with
         * implied on-curve points between two consecutive controls. */
        for (c = 0, i = 0; c < contours; c++) {
            int end = xf_be16(ends + c * 2), first = i, k, n = end - first + 1;
            pt start, prev;
            int start_on;
            if (n < 2) {
                i = end + 1;
                continue;
            }
            start_on = flags[first] & 1;
            if (start_on)
                start = pts[first];
            else if (flags[end] & 1)
                start = pts[end];
            else {
                start.x = (pts[first].x + pts[end].x) / 2;
                start.y = (pts[first].y + pts[end].y) / 2;
            }
            prev = start;
            {
                pt ctrl = { 0, 0 };
                int have_ctrl = 0;
                for (k = start_on ? 1 : 0; k <= n; k++) {
                    int idx = first + (k % n);
                    pt cur = pts[idx];
                    int on = flags[idx] & 1;
                    if (k == n && !start_on) {
                        cur = start;
                        on = 1;
                    }
                    if (on) {
                        if (have_ctrl ? add_quad(s, prev, ctrl, cur) : add_line(s, prev, cur))
                            return -1;
                        prev = cur;
                        have_ctrl = 0;
                    } else {
                        if (have_ctrl) {
                            pt mid;
                            mid.x = (ctrl.x + cur.x) / 2;
                            mid.y = (ctrl.y + cur.y) / 2;
                            if (add_quad(s, prev, ctrl, mid))
                                return -1;
                            prev = mid;
                        }
                        ctrl = cur;
                        have_ctrl = 1;
                    }
                }
                if (have_ctrl && add_quad(s, prev, ctrl, start))
                    return -1;
                else if (!have_ctrl && (prev.x != start.x || prev.y != start.y) && add_line(s, prev, start))
                    return -1;
            }
            i = end + 1;
        }
        return 0;
    } else {
        /* Composite glyph: components with offsets and optional scale. */
        const u8 *q = p + 10;
        u16 flags;
        do {
            float cm[6] = { 1, 0, 0, 1, 0, 0 }, r[6];
            int sub;
            flags = xf_be16(q);
            sub = xf_be16(q + 2);
            q += 4;
            if (flags & 1) {
                cm[4] = (i16)xf_be16(q);
                cm[5] = (i16)xf_be16(q + 2);
                q += 4;
            } else {
                cm[4] = (i8)q[0];
                cm[5] = (i8)q[1];
                q += 2;
            }
            if (flags & 8) {
                cm[0] = cm[3] = (i16)xf_be16(q) / 16384.0f;
                q += 2;
            } else if (flags & 0x40) {
                cm[0] = (i16)xf_be16(q) / 16384.0f;
                cm[3] = (i16)xf_be16(q + 2) / 16384.0f;
                q += 4;
            } else if (flags & 0x80) {
                cm[0] = (i16)xf_be16(q) / 16384.0f;
                cm[1] = (i16)xf_be16(q + 2) / 16384.0f;
                cm[2] = (i16)xf_be16(q + 4) / 16384.0f;
                cm[3] = (i16)xf_be16(q + 6) / 16384.0f;
                q += 8;
            }
            r[0] = m[0] * cm[0] + m[2] * cm[1];
            r[1] = m[1] * cm[0] + m[3] * cm[1];
            r[2] = m[0] * cm[2] + m[2] * cm[3];
            r[3] = m[1] * cm[2] + m[3] * cm[3];
            r[4] = m[0] * cm[4] + m[2] * cm[5] + m[4];
            r[5] = m[1] * cm[4] + m[3] * cm[5] + m[5];
            if (add_glyph(f, sub, r, s, depth + 1) != 0)
                return -1;
        } while (flags & 0x20);
        return 0;
    }
}

/* ---- rasterizer: signed area accumulation ---- */

/* Adds the coverage of an edge piece that stays inside one pixel column:
 * the area right of the edge goes to this pixel, the rest carries on. */
static void add_piece(float *row, int w, float xa, float xb, float d)
{
    float xm = (xa + xb) / 2;
    int i = (int)floorf(xm);
    float frac;
    if (i < 0) {
        row[0] += d;
        return;
    }
    if (i > w)
        return;
    frac = xm - i;
    row[i] += d * (1 - frac);
    if (i + 1 <= w + 1)
        row[i + 1] += d * frac;
}

static void draw_line(float *acc, int w, int h, pt p0, pt p1)
{
    float dir = 1, dxdy, x;
    int y0, y1, y;
    if (p0.y == p1.y)
        return;
    if (p0.y > p1.y) {
        pt t = p0;
        p0 = p1;
        p1 = t;
        dir = -1;
    }
    dxdy = (p1.x - p0.x) / (p1.y - p0.y);
    x = p0.x;
    if (p0.y < 0) {
        x -= p0.y * dxdy;
        p0.y = 0;
    }
    y0 = (int)p0.y;
    y1 = XF_MIN((int)ceilf(p1.y), h);
    for (y = y0; y < y1; y++) {
        float *row = acc + y * (w + 2);
        float dy = XF_MIN((float)(y + 1), p1.y) - XF_MAX((float)y, p0.y);
        float xnext = x + dxdy * dy, d = dy * dir;
        float xa = XF_MIN(x, xnext), xb = XF_MAX(x, xnext);
        if (floorf(xa) == floorf(xb) || xb - xa < 1e-6f) {
            add_piece(row, w, xa, xb, d);
        } else {
            /* Split at pixel boundaries; each piece gets its share of dy. */
            float span = xb - xa, cur = xa;
            while (cur < xb) {
                float next = XF_MIN(floorf(cur) + 1, xb);
                add_piece(row, w, cur, next, d * (next - cur) / span);
                cur = next;
            }
        }
        x = xnext;
    }
}

int xf_font_render(const xf_font *f, int glyph, float px, xf_glyph *out)
{
    float scale = px / f->units_per_em, m[6];
    m[0] = scale;
    m[1] = m[2] = m[4] = m[5] = 0;
    m[3] = -scale;
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f, *acc;
    seglist s = { 0 };
    int i, w, h, x, y;

    memset(out, 0, sizeof(*out));
    out->advance = (int)(xf_font_advance(f, glyph, px) + 0.5f);
    if (add_glyph(f, glyph, m, &s, 0) != 0) {
        free(s.lines);
        return -1;
    }
    if (s.count == 0)
        return 0; /* blank glyph such as a space */
    for (i = 0; i < s.count; i++) {
        minx = XF_MIN(minx, s.lines[i].x);
        maxx = XF_MAX(maxx, s.lines[i].x);
        miny = XF_MIN(miny, s.lines[i].y);
        maxy = XF_MAX(maxy, s.lines[i].y);
    }
    out->x0 = (int)floorf(minx);
    out->y0 = (int)floorf(miny);
    w = (int)ceilf(maxx) - out->x0;
    h = (int)ceilf(maxy) - out->y0;
    if (w <= 0 || h <= 0) {
        free(s.lines);
        return 0;
    }
    acc = calloc((size_t)(w + 2) * h, sizeof(float));
    out->alpha = malloc((size_t)w * h);
    if (!acc || !out->alpha) {
        free(acc);
        free(out->alpha);
        out->alpha = NULL;
        free(s.lines);
        return -1;
    }
    for (i = 0; i < s.count; i += 2) {
        pt a, b;
        a.x = s.lines[i].x - out->x0;
        a.y = s.lines[i].y - out->y0;
        b.x = s.lines[i + 1].x - out->x0;
        b.y = s.lines[i + 1].y - out->y0;
        draw_line(acc, w, h, a, b);
    }
    for (y = 0; y < h; y++) {
        float sum = 0;
        for (x = 0; x < w; x++) {
            float v;
            sum += acc[y * (w + 2) + x];
            v = fabsf(sum);
            out->alpha[y * w + x] = (u8)(v >= 1 ? 255 : v * 255 + 0.5f);
        }
    }
    out->width = w;
    out->height = h;
    free(acc);
    free(s.lines);
    return 0;
}
