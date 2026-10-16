/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* Screen layout beyond 640x480: placing the scene strip and the
 * interface pieces on the output, and mapping the pointer back. */
#include "gfx/screen.h"

#include <string.h>

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int ui(const scr_layout *l, int v) { return (int)((long long)v * l->ui_num / l->ui_den); }

static int ui_inv(const scr_layout *l, int v) { return (int)((long long)v * l->ui_den / l->ui_num); }

void scr_layout_begin(scr_layout *l, int out_w, int out_h, int wide)
{
    memset(l, 0, sizeof(*l));
    l->out_w = out_w > 0 ? out_w : SCR_W;
    l->out_h = out_h > 0 ? out_h : SCR_H;
    /* The interface keeps its 640x480 proportions, fitted to the output. */
    if ((long long)l->out_w * SCR_H >= (long long)l->out_h * SCR_W) {
        l->ui_num = l->out_h;
        l->ui_den = SCR_H;
    } else {
        l->ui_num = l->out_w;
        l->ui_den = SCR_W;
    }
    l->screen.w = ui(l, SCR_W);
    l->screen.h = ui(l, SCR_H);
    l->screen.x = (l->out_w - l->screen.w) / 2;
    l->screen.y = (l->out_h - l->screen.h) / 2;
    l->reserve_y = l->out_h;
    l->wide = wide && (long long)l->out_w * SCR_H > (long long)l->out_h * SCR_W;
    if (!l->wide)
        return;
    /* The strip fills the width, or the height on very wide outputs. It
     * sits as in the original: 90 parts of the room above, 150 below. */
    l->strip.w = l->out_w;
    l->strip.h = (int)((long long)l->out_w * SCR_STRIP_H / SCR_STRIP_W);
    if (l->strip.h > l->out_h) {
        l->strip.h = l->out_h;
        l->strip.w = (int)((long long)l->out_h * SCR_STRIP_W / SCR_STRIP_H);
    }
    {
        /* Under the strip the emotion icons and the inventory's band need
         * the 150 parts the original keeps there (scaled like the
         * interface): the strip leaves that room, smaller if need be, so
         * the icons stay off the picture. */
        int room = ui(l, SCR_H - SCR_STRIP_Y - SCR_STRIP_H);
        if (l->out_h - l->strip.h < room && l->out_h > room) {
            l->strip.h = l->out_h - room;
            l->strip.w = (int)((long long)l->strip.h * SCR_STRIP_W / SCR_STRIP_H);
        }
    }
    l->strip.x = (l->out_w - l->strip.w) / 2;
    l->strip.y = (l->out_h - l->strip.h) * SCR_STRIP_Y / (SCR_H - SCR_STRIP_H);
    /* The room above shrinks first, so the 150 parts stay under it. */
    l->strip.y = XF_MIN(l->strip.y, l->out_h - l->strip.h - ui(l, SCR_H - SCR_STRIP_Y - SCR_STRIP_H));
}

void scr_add_piece(scr_layout *l, scr_rect src, int anchor, const u32 *px)
{
    scr_piece *p;
    int strip_bottom = l->strip.y + l->strip.h, below = SCR_STRIP_Y + SCR_STRIP_H;
    if (!l->wide || l->piece_count >= SCR_MAX_PIECES || src.w <= 0 || src.h <= 0)
        return;
    p = &l->pieces[l->piece_count++];
    p->src = src;
    p->anchor = anchor;
    p->px = px;
    p->dst.w = ui(l, src.w);
    p->dst.h = ui(l, src.h);
    switch (anchor) {
    case SCR_ANCHOR_UNDER:
        p->dst.x = l->strip.x +
                   (int)((long long)(2 * src.x + src.w - 2 * SCR_STRIP_X) * l->strip.w / (2 * SCR_STRIP_W)) -
                   p->dst.w / 2;
        p->dst.y = strip_bottom + ui(l, src.y - below);
        break;
    case SCR_ANCHOR_BELOW:
        p->dst.x = l->out_w / 2 + ui(l, src.x - SCR_W / 2);
        p->dst.y = strip_bottom + ui(l, src.y - below);
        break;
    case SCR_ANCHOR_BOTTOM_LEFT:
        p->dst.x = ui(l, src.x);
        p->dst.y = l->out_h - ui(l, SCR_H - src.y);
        break;
    case SCR_ANCHOR_TOP_LEFT:
        p->dst.x = ui(l, src.x);
        p->dst.y = ui(l, src.y);
        break;
    case SCR_ANCHOR_TOP_RIGHT:
        p->dst.x = l->out_w - ui(l, SCR_W - src.x);
        p->dst.y = ui(l, src.y);
        break;
    default:
        p->dst.x = l->out_w / 2 + ui(l, src.x - SCR_W / 2);
        p->dst.y = l->out_h / 2 + ui(l, src.y - SCR_H / 2);
        break;
    }
    /* Pieces stay on the output; on very wide outputs they cover the
     * strip, as the dialogue box already does in the original. */
    if (p->dst.w <= l->out_w)
        p->dst.x = clampi(p->dst.x, 0, l->out_w - p->dst.w);
    if (p->dst.h <= l->out_h)
        p->dst.y = clampi(p->dst.y, 0, l->out_h - p->dst.h);
    /* The inventory slides in at the bottom: pieces under the strip
     * keep out of its band, on short outputs over the strip. */
    if ((anchor == SCR_ANCHOR_UNDER || anchor == SCR_ANCHOR_BELOW) && p->dst.y + p->dst.h > l->reserve_y)
        p->dst.y = XF_MAX(0, l->reserve_y - p->dst.h);
}

void scr_reserve_bottom(scr_layout *l, int y)
{
    l->reserve_y = l->out_h - ui(l, SCR_H - y);
}

static int inside(const scr_rect *r, int x, int y) { return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h; }

void scr_to_screen(const scr_layout *l, int x, int y, int *sx, int *sy)
{
    int i;
    if (!l->wide) {
        *sx = clampi(ui_inv(l, x - l->screen.x), 0, SCR_W - 1);
        *sy = clampi(ui_inv(l, y - l->screen.y), 0, SCR_H - 1);
        return;
    }
    for (i = l->piece_count - 1; i >= 0; i--) {
        const scr_piece *p = &l->pieces[i];
        if (inside(&p->dst, x, y)) {
            *sx = clampi(p->src.x + (int)((long long)(x - p->dst.x) * p->src.w / p->dst.w), 0, SCR_W - 1);
            *sy = clampi(p->src.y + (int)((long long)(y - p->dst.y) * p->src.h / p->dst.h), 0, SCR_H - 1);
            return;
        }
    }
    /* The strip, and beside it on very wide outputs. */
    *sx = SCR_STRIP_X + (int)((long long)(x - l->strip.x) * SCR_STRIP_W / l->strip.w);
    *sy = SCR_STRIP_Y + (int)((long long)(y - l->strip.y) * SCR_STRIP_H / l->strip.h);
    if (y >= l->strip.y + l->strip.h)
        /* Under the strip: measured from the bottom, where the inventory
         * slides in. */
        *sy = clampi(SCR_H - ui_inv(l, l->out_h - y), SCR_STRIP_Y + SCR_STRIP_H, SCR_H - 1);
    else if (y < l->strip.y)
        *sy = clampi(ui_inv(l, y), 0, SCR_STRIP_Y - 1);
    if (y >= l->strip.y + l->strip.h || (y < l->strip.y && x < l->out_w / 2))
        *sx = ui_inv(l, x);
    else if (y < l->strip.y) /* the top-right corner, where the menu icon shows */
        *sx = SCR_W - ui_inv(l, l->out_w - x);
    *sx = clampi(*sx, 0, SCR_W - 1);
    *sy = clampi(*sy, 0, SCR_H - 1);
}

void scr_to_output(const scr_layout *l, int sx, int sy, int *x, int *y)
{
    const scr_rect strip = { SCR_STRIP_X, SCR_STRIP_Y, SCR_STRIP_W, SCR_STRIP_H };
    int i;
    if (!l->wide) {
        *x = l->screen.x + ui(l, sx);
        *y = l->screen.y + ui(l, sy);
        return;
    }
    for (i = l->piece_count - 1; i >= 0; i--) {
        const scr_piece *p = &l->pieces[i];
        if (inside(&p->src, sx, sy)) {
            *x = p->dst.x + (int)((long long)(sx - p->src.x) * p->dst.w / p->src.w);
            *y = p->dst.y + (int)((long long)(sy - p->src.y) * p->dst.h / p->src.h);
            return;
        }
    }
    if (inside(&strip, sx, sy)) {
        *x = l->strip.x + (int)((long long)(sx - SCR_STRIP_X) * l->strip.w / SCR_STRIP_W);
        *y = l->strip.y + (int)((long long)(sy - SCR_STRIP_Y) * l->strip.h / SCR_STRIP_H);
        return;
    }
    *x = sy < SCR_STRIP_Y && sx >= SCR_W / 2 ? l->out_w - ui(l, SCR_W - sx) : ui(l, sx);
    *y = sy < SCR_STRIP_Y ? ui(l, sy) : l->out_h - ui(l, SCR_H - sy);
}

static u32 lerp(u32 a, u32 b, u32 t) /* t in 0..256 */
{
    u32 rb = ((a & 0xFF00FF) * (256 - t) + (b & 0xFF00FF) * t) >> 8;
    u32 g = ((a & 0x00FF00) * (256 - t) + (b & 0x00FF00) * t) >> 8;
    return (rb & 0xFF00FF) | (g & 0x00FF00);
}

#define MAX_COLUMNS 8192

static int col[MAX_COLUMNS], colt[MAX_COLUMNS], col1[MAX_COLUMNS];

static void scale_row(u32 *dst, const u32 *src, int n)
{
    int k;
    for (k = 0; k < n; k++)
        dst[k] = lerp(src[col[k]], src[col1[k]], (u32)colt[k]);
}

/* Scales a source rectangle onto an output rectangle with bilinear
 * filtering. With `keyed`, SCR_CLEAR pixels let the output show through. */
static void scale_rect(u32 *out, int ow, int oh, scr_rect d, const u32 *src, int pitch, scr_rect s, int keyed)
{
    static u32 rows[2][MAX_COLUMNS];
    u32 *ha = rows[0], *hb = rows[1];
    int row_a = -1, row_b = -1;
    int x, y, x0 = XF_MAX(d.x, 0), y0 = XF_MAX(d.y, 0), x1 = XF_MIN(d.x + d.w, ow), y1 = XF_MIN(d.y + d.h, oh);
    if (d.w <= 0 || d.h <= 0 || s.w <= 0 || s.h <= 0 || x1 - x0 > MAX_COLUMNS)
        return;
    /* 8.8 fixed point source positions, pixel centres aligned */
    for (x = x0; x < x1; x++) {
        long fx = ((long)(2 * (x - d.x) + 1) * s.w * 128) / d.w - 128;
        int sx = (int)(fx >> 8), tx = (int)(fx & 255);
        if (fx < 0)
            sx = 0, tx = 0;
        if (sx >= s.w - 1)
            sx = s.w - 1, tx = 0;
        col[x - x0] = sx;
        col1[x - x0] = sx + 1 < s.w ? sx + 1 : sx;
        colt[x - x0] = tx;
    }
    for (y = y0; y < y1; y++) {
        long fy = ((long)(2 * (y - d.y) + 1) * s.h * 128) / d.h - 128;
        int sy = (int)(fy >> 8), ty = (int)(fy & 255), n = x1 - x0, k;
        const u32 *r0, *r1;
        u32 *o = out + (size_t)y * ow + x0;
        if (fy < 0)
            sy = 0, ty = 0;
        if (sy >= s.h - 1)
            sy = s.h - 1, ty = 0;
        r0 = src + (size_t)(s.y + sy) * pitch + s.x;
        r1 = sy + 1 < s.h ? r0 + pitch : r0;
        if (!keyed) {
            /* Rows scaled across once: output rows share source rows. */
            if (row_a != sy) {
                if (row_b == sy) {
                    u32 *tmp = ha;
                    ha = hb, hb = tmp;
                    row_b = row_a;
                } else
                    scale_row(ha, r0, n);
                row_a = sy;
            }
            if (row_b != sy + 1) {
                scale_row(hb, r1, n);
                row_b = sy + 1;
            }
            for (k = 0; k < n; k++)
                o[k] = ty ? lerp(ha[k], hb[k], (u32)ty) : ha[k];
            continue;
        }
        for (k = 0; k < n; k++) {
            u32 a = r0[col[k]], b = r0[col1[k]], c = r1[col[k]], e = r1[col1[k]], t = (u32)colt[k];
            /* The nearest pixel decides; clear neighbours borrow it. */
            u32 m = ty < 128 ? (t < 128 ? a : b) : (t < 128 ? c : e);
            if (m == SCR_CLEAR)
                continue;
            if (a == SCR_CLEAR) a = m;
            if (b == SCR_CLEAR) b = m;
            if (c == SCR_CLEAR) c = m;
            if (e == SCR_CLEAR) e = m;
            o[k] = lerp(lerp(a, b, t), lerp(c, e, t), (u32)ty);
        }
    }
}

/* Fills the output outside a rectangle. */
static void fill_around(u32 *out, int ow, int oh, scr_rect r, u32 c)
{
    int x, y;
    for (y = 0; y < oh; y++) {
        u32 *o = out + (size_t)y * ow;
        int inside_row = y >= r.y && y < r.y + r.h;
        for (x = 0; x < ow; x++) {
            if (inside_row && x == r.x) {
                x = r.x + r.w - 1;
                continue;
            }
            o[x] = c;
        }
    }
}

void scr_compose(const scr_layout *l, u32 *out, const u32 *scene, const u32 *ui_px, u32 bg)
{
    const scr_rect whole = { 0, 0, SCR_W, SCR_H }, strip = { SCR_STRIP_X, SCR_STRIP_Y, SCR_STRIP_W, SCR_STRIP_H };
    int i;
    if (!l->wide) {
        fill_around(out, l->out_w, l->out_h, l->screen, bg);
        scale_rect(out, l->out_w, l->out_h, l->screen, scene, SCR_W, whole, 0);
        return;
    }
    fill_around(out, l->out_w, l->out_h, l->strip, bg);
    scale_rect(out, l->out_w, l->out_h, l->strip, scene, SCR_W, strip, 0);
    for (i = 0; i < l->piece_count; i++) {
        const scr_piece *p = &l->pieces[i];
        scr_rect own;
        own.x = own.y = 0;
        own.w = p->src.w;
        own.h = p->src.h;
        if (p->px)
            scale_rect(out, l->out_w, l->out_h, p->dst, p->px, p->src.w, own, 1);
        else if (ui_px)
            scale_rect(out, l->out_w, l->out_h, p->dst, ui_px, SCR_W, p->src, 1);
    }
}

void scr_draw_scaled(const scr_layout *l, u32 *out, const u32 *px, int w, int h, int hx, int hy, int x, int y,
                     u32 key)
{
    int dw = ui(l, w), dh = ui(l, h), dx, dy;
    int x0 = x - ui(l, hx), y0 = y - ui(l, hy);
    if (dw <= 0 || dh <= 0)
        return;
    for (dy = XF_MAX(0, -y0); dy < dh && y0 + dy < l->out_h; dy++)
        for (dx = XF_MAX(0, -x0); dx < dw && x0 + dx < l->out_w; dx++) {
            u32 c = px[(dy * h / dh) * w + dx * w / dw];
            if (c != key)
                out[(size_t)(y0 + dy) * l->out_w + x0 + dx] = c & 0xFFFFFF;
        }
}
