/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
#include "vob.h"

#include <stdlib.h>
#include <string.h>

#include "media/mpeg2.h"
#include "xf_jobs.h"
#include "xf_file.h"
#include "xf_log.h"

#define PACK_SIZE 2048
#define NO_PTS (-1)

const xf_vob_view xf_vob_full_view = { 0, 0, 704, 480, 640, 480 };
const xf_vob_view xf_vob_scene_view = { 30, 118, 660, 240, 600, 240 };

/* Reads one stream's payloads from the packs, in file order. */
typedef struct {
    xf_file *f;
    u64 pos, size;
    u8 pack[PACK_SIZE];
    int len, off;
    int audio; /* 0: video, 1: LPCM sound */
} cursor;

struct xf_vob {
    xf_file *vf, *af;
    cursor vc, ac;
    i64 t0;             /* clip time 0, in 90 kHz units */
    u32 duration_ms;
    /* sound */
    int rate, channels;
    const u8 *apay;
    int alen;
    u8 part[4];         /* bytes of a sample frame split between packets */
    int part_len;
    u32 lead;           /* silent frames before the first sound */
    /* video */
    xf_mpeg2 *dec;
    u32 video_start_ms;
    u64 fields;         /* fields shown before the next picture */
    xf_mpeg2_picture next;
    int have_next, video_done, stream_ended;
    /* scaling for the last view */
    xf_vob_view view;
    int *col, *frac;
};

static i64 read_pts(const u8 *p)
{
    return (i64)(p[0] >> 1 & 7) << 30 | (i64)p[1] << 22 | (i64)(p[2] >> 1) << 15 | (i64)p[3] << 7 | p[4] >> 1;
}

static int read_pack(cursor *c)
{
    if (c->pos + PACK_SIZE > c->size || xf_read_at(c->f, c->pos, c->pack, PACK_SIZE) != 0)
        return -1;
    c->pos += PACK_SIZE;
    c->len = PACK_SIZE;
    c->off = 0;
    return 0;
}

/* The next payload of the cursor's stream. For sound it starts at the
 * samples; *fmt points at the 3-byte LPCM header. Returns 0, or -1 at
 * the end of the file. */
static int next_payload(cursor *c, const u8 **data, int *len, i64 *pts, const u8 **fmt)
{
    for (;;) {
        const u8 *p;
        int id, plen, end;
        if (c->off + 6 > c->len) {
            if (read_pack(c) != 0)
                return -1;
        }
        p = c->pack + c->off;
        if (p[0] != 0 || p[1] != 0 || p[2] != 1) {
            c->off = c->len; /* lost: go to the next pack */
            continue;
        }
        id = p[3];
        if (id == 0xBA) {
            c->off += (p[4] & 0xC0) == 0x40 ? 14 + (p[13] & 7) : 12;
            continue;
        }
        plen = p[4] << 8 | p[5];
        end = c->off + 6 + plen;
        if (end > c->len)
            end = c->len;
        c->off = end;
        if ((id == 0xE0 && !c->audio) || (id == 0xBD && c->audio)) {
            int hdr = 9 + p[8], start;
            if ((p[6] & 0xC0) != 0x80 || c->off - (int)(p - c->pack) < hdr)
                continue;
            *pts = (p[7] & 0x80) ? read_pts(p + 9) : NO_PTS;
            start = (int)(p - c->pack) + hdr;
            if (c->audio) {
                /* substream id, frame count, first access unit, then the LPCM header */
                if (end - start < 7 || (c->pack[start] & 0xF8) != 0xA0)
                    continue;
                *fmt = c->pack + start + 4;
                start += 7;
            }
            *data = c->pack + start;
            *len = end - start;
            return 0;
        }
    }
}

static void cursor_init(cursor *c, xf_file *f, int audio)
{
    memset(c, 0, sizeof(*c));
    c->f = f;
    c->size = xf_size(f);
    c->audio = audio;
}

/* The first time stamps, the sound format and the length of the clip. */
static void scan(xf_vob *v)
{
    cursor c;
    const u8 *data, *fmt = NULL;
    int len, i;
    i64 pts, vfirst = NO_PTS, afirst = NO_PTS, last = NO_PTS;
    u64 size;

    cursor_init(&c, v->vf, 0);
    for (i = 0; i < 64 && vfirst == NO_PTS && next_payload(&c, &data, &len, &pts, &fmt) == 0; i++)
        vfirst = pts;
    cursor_init(&c, v->vf, 1);
    for (i = 0; i < 64 && afirst == NO_PTS && next_payload(&c, &data, &len, &pts, &fmt) == 0; i++)
        afirst = pts;
    if (afirst != NO_PTS && fmt && (fmt[1] >> 6) == 0) {
        static const int rates[4] = { 48000, 96000, 44100, 32000 };
        v->rate = rates[fmt[1] >> 4 & 3];
        v->channels = (fmt[1] & 7) + 1;
        if (v->channels > 2)
            v->rate = 0;
    }
    if (v->rate == 0)
        afirst = NO_PTS;
    v->t0 = vfirst;
    if (afirst != NO_PTS && (v->t0 == NO_PTS || afirst < v->t0))
        v->t0 = afirst;
    if (v->t0 == NO_PTS)
        v->t0 = 0;
    if (vfirst != NO_PTS)
        v->video_start_ms = (u32)((vfirst - v->t0) / 90);
    if (afirst != NO_PTS)
        v->lead = (u32)((afirst - v->t0) * v->rate / 90000);

    /* the last time stamp, near the end of the file */
    size = xf_size(v->vf);
    for (i = 0; i < 2; i++) {
        cursor_init(&c, v->vf, i);
        c.pos = size > 256 * PACK_SIZE ? (size / PACK_SIZE - 256) * PACK_SIZE : 0;
        while (next_payload(&c, &data, &len, &pts, &fmt) == 0)
            if (pts != NO_PTS && pts > last)
                last = pts;
    }
    v->duration_ms = last != NO_PTS && last > v->t0 ? (u32)((last - v->t0) / 90) + 34 : 0;
}

xf_vob *xf_vob_open(const char *path)
{
    xf_vob *v = calloc(1, sizeof(*v));
    if (!v)
        return NULL;
    v->vf = xf_open(path);
    v->af = xf_open(path);
    v->dec = xf_mpeg2_new();
    if (!v->vf || !v->af || !v->dec) {
        xf_vob_close(v);
        return NULL;
    }
    scan(v);
    cursor_init(&v->vc, v->vf, 0);
    cursor_init(&v->ac, v->af, 1);
    return v;
}

void xf_vob_close(xf_vob *v)
{
    if (!v)
        return;
    if (v->vf)
        xf_close(v->vf);
    if (v->af)
        xf_close(v->af);
    xf_mpeg2_free(v->dec);
    free(v->col);
    free(v->frac);
    free(v);
}

u32 xf_vob_duration_ms(const xf_vob *v) { return v->duration_ms; }
int xf_vob_sample_rate(const xf_vob *v) { return v->rate; }
int xf_vob_channels(const xf_vob *v) { return v->channels; }

/* --- Sound ----------------------------------------------------------------- */

int xf_vob_audio(xf_vob *v, i16 *out, int max)
{
    int n = 0, frame = 2 * v->channels;
    if (!v->rate)
        return 0;
    while (n < max && v->lead > 0) {
        memset(out + n * v->channels, 0, (size_t)frame);
        n++;
        v->lead--;
    }
    while (n < max) {
        if (v->alen == 0) {
            const u8 *fmt;
            i64 pts;
            if (next_payload(&v->ac, &v->apay, &v->alen, &pts, &fmt) != 0)
                break;
            continue;
        }
        if (v->part_len || v->alen < frame) {
            /* a sample frame split between two packets */
            while (v->part_len < frame && v->alen > 0) {
                v->part[v->part_len++] = *v->apay++;
                v->alen--;
            }
            if (v->part_len == frame) {
                int c;
                for (c = 0; c < v->channels; c++)
                    out[n * v->channels + c] = (i16)(v->part[c * 2] << 8 | v->part[c * 2 + 1]);
                n++;
                v->part_len = 0;
            }
            continue;
        }
        {
            int k = XF_MIN(v->alen / frame, max - n), i;
            int count = k * v->channels;
            i16 *o = out + n * v->channels;
            for (i = 0; i < count; i++)
                o[i] = (i16)(v->apay[i * 2] << 8 | v->apay[i * 2 + 1]);
            v->apay += k * frame;
            v->alen -= k * frame;
            n += k;
        }
    }
    return n;
}

/* --- Pictures ---------------------------------------------------------------- */

/* Decodes the next picture into v->next. Returns 0, or -1 at the end. */
static int pull_picture(xf_vob *v)
{
    while (!xf_mpeg2_next(v->dec, &v->next)) {
        const u8 *data, *fmt;
        int len;
        i64 pts;
        if (v->stream_ended)
            return -1;
        if (next_payload(&v->vc, &data, &len, &pts, &fmt) == 0) {
            if (xf_mpeg2_push(v->dec, data, (size_t)len) != 0)
                return -1;
        } else {
            xf_mpeg2_end(v->dec);
            v->stream_ended = 1;
        }
    }
    return 0;
}

static u32 field_ms(const xf_vob *v, u64 fields)
{
    const xf_mpeg2_picture *p = &v->next;
    if (p->field_rate_num <= 0)
        return 0;
    return (u32)(fields * 1000 * (u64)p->field_rate_den / (u64)p->field_rate_num);
}

static int set_view(xf_vob *v, const xf_vob_view *view)
{
    int x;
    if (v->col && memcmp(&v->view, view, sizeof(*view)) == 0)
        return 0;
    free(v->col);
    free(v->frac);
    v->col = malloc((size_t)view->out_w * sizeof(int));
    v->frac = malloc((size_t)view->out_w * sizeof(int));
    if (!v->col || !v->frac)
        return -1;
    for (x = 0; x < view->out_w; x++) {
        /* the centre of the output pixel, in source columns, 8-bit fraction */
        int pos = (int)(((2 * x + 1) * (i64)view->w * 256 / view->out_w - 256) / 2);
        if (pos < 0)
            pos = 0;
        v->col[x] = view->x + (pos >> 8);
        v->frac[x] = pos & 255;
    }
    v->view = *view;
    return 0;
}

static u8 clip(int v) { return (u8)(v < 0 ? 0 : v > 255 ? 255 : v); }

typedef struct {
    const xf_vob *v;
    u32 *rgb;
    const xf_vob_view *view;
} draw_job;

/* Rows part, part + parts, ... of the output: converted (BT.601, video
 * range) to RGB and scaled to the view. Interlaced pictures are blended
 * with their neighbour rows. */
static void draw_rows(void *ctx, int part, int parts)
{
    const draw_job *j = ctx;
    const xf_vob *v = j->v;
    const xf_vob_view *view = j->view;
    const xf_mpeg2_picture *p = &v->next;
    int x, y, last_col = p->width - 1;
    u8 row[2048];
    if (p->width > (int)sizeof(row))
        return;
    for (y = part; y < view->out_h; y += parts) {
        int sy = view->y + y * view->h / view->out_h, i;
        const u8 *cb, *cr;
        u32 *o = j->rgb + (size_t)y * view->out_w;
        if (sy >= p->height)
            sy = p->height - 1;
        {
            /* (above + 2 * row + below) / 4 */
            const u8 *a = p->y + (size_t)(sy > 0 ? sy - 1 : sy) * p->stride;
            const u8 *m = p->y + (size_t)sy * p->stride;
            const u8 *b = p->y + (size_t)(sy + 1 < p->height ? sy + 1 : sy) * p->stride;
            for (i = 0; i < p->width; i++)
                row[i] = (u8)((a[i] + 2 * m[i] + b[i] + 2) >> 2);
        }
        cb = p->cb + (size_t)(sy >> 1) * p->cstride;
        cr = p->cr + (size_t)(sy >> 1) * p->cstride;
        for (x = 0; x < view->out_w; x++) {
            int c0 = v->col[x], c1 = c0 < last_col ? c0 + 1 : c0, f = v->frac[x];
            int luma = (row[c0] * (256 - f) + row[c1] * f + 128) >> 8;
            int u = cb[c0 >> 1] - 128, w = cr[c0 >> 1] - 128;
            int l = 298 * (luma - 16) + 128;
            o[x] = (u32)clip((l + 409 * w) >> 8) << 16 | (u32)clip((l - 100 * u - 208 * w) >> 8) << 8 |
                   clip((l + 516 * u) >> 8);
        }
    }
}

static void draw(xf_vob *v, u32 *rgb, const xf_vob_view *view)
{
    draw_job j;
    if (set_view(v, view) != 0)
        return;
    j.v = v;
    j.rgb = rgb;
    j.view = view;
    xf_jobs_run(draw_rows, &j, xf_jobs_count());
}

int xf_vob_video(xf_vob *v, u32 ms, u32 *rgb, const xf_vob_view *view)
{
    int changed = 0, decoded = 0;
    for (;;) {
        u32 at, after;
        if (!v->have_next) {
            if (v->video_done || pull_picture(v) != 0) {
                v->video_done = 1;
                return changed ? 1 : -1;
            }
            v->have_next = 1;
        }
        at = v->video_start_ms + field_ms(v, v->fields);
        if (at > ms)
            return changed;
        after = v->video_start_ms + field_ms(v, v->fields + (u64)v->next.fields);
        v->fields += (u64)v->next.fields;
        v->have_next = 0;
        /* A machine too slow to keep up shows every third picture or so
         * rather than decoding ever more of them each time. */
        if (after > ms || ++decoded >= 3) {
            /* the picture to show now; earlier ones were late and skipped */
            draw(v, rgb, view);
            return 1;
        }
    }
}

int xf_vob_picture(xf_vob *v, xf_mpeg2_picture *pic)
{
    if (pull_picture(v) != 0)
        return -1;
    *pic = v->next;
    return 0;
}
