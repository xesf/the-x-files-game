/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf and contributors */
#include "subtitle.h"

#include <stdlib.h>
#include <string.h>

#include "xf_endian.h"
#include "xf_text.h"

static int add_cue(xf_subtitles *subs, u32 start, u32 end, u32 track, const u8 *text, u32 len)
{
    xf_subtitle *n;
    char *s;
    u32 i;

    for (i = 0; i < len && (text[i] == ' ' || text[i] == '\r' || text[i] == '\n'); i++)
        ;
    if (i == len || end <= start)
        return 0; /* blank lines clear the screen; nothing to show */
    s = malloc((size_t)len * 3 + 1);
    n = realloc(subs->cues, (subs->count + 1) * sizeof(*n));
    if (!s || !n) {
        free(s);
        return -1;
    }
    subs->cues = n;
    xf_macroman_to_utf8(s, (size_t)len * 3 + 1, text, len);
    n[subs->count].start_ms = start;
    n[subs->count].end_ms = end;
    n[subs->count].track_id = track;
    n[subs->count].text = s;
    subs->count++;
    return 0;
}

static u32 to_ms(u64 t, u32 scale) { return scale ? (u32)(t * 1000 / scale) : 0; }

/* Adds the samples of `t` that fall in media range [m0, m1), shown from
 * movie time `movie_start` (movie units). */
static int add_range(xf_mov *mov, const xf_mov_track *t, xf_subtitles *subs, u64 movie_start,
                     u64 m0, u64 m1)
{
    u32 i;
    for (i = 0; i < t->sample_count; i++) {
        u64 s0 = t->sample_time[i];
        u64 s1 = i + 1 < t->sample_count ? t->sample_time[i + 1] : t->duration;
        u64 a = XF_MAX(s0, m0), b = XF_MIN(s1, m1);
        u8 *data;
        u32 size = t->sample_size[i];
        int r = 0;

        if (a >= b || size < 2)
            continue;
        data = malloc(size);
        if (!data || xf_mov_read_sample(mov, t, i, data) != 0) {
            free(data);
            return -1;
        }
        {
            u32 len = XF_MIN(xf_be16(data), size - 2);
            u64 start = movie_start * 1000 / mov->timescale + to_ms(a - m0, t->timescale);
            u64 end = movie_start * 1000 / mov->timescale + to_ms(b - m0, t->timescale);
            r = add_cue(subs, (u32)start, (u32)end, t->id, data + 2, len);
        }
        free(data);
        if (r != 0)
            return -1;
    }
    return 0;
}

static int cmp_cue(const void *a, const void *b)
{
    const xf_subtitle *x = a, *y = b;
    return x->start_ms < y->start_ms ? -1 : x->start_ms > y->start_ms ? 1 : 0;
}

int xf_subtitles_load(xf_mov *mov, xf_subtitles *subs)
{
    u32 k, e;

    memset(subs, 0, sizeof(*subs));
    for (k = 0; k < mov->track_count; k++) {
        const xf_mov_track *t = &mov->tracks[k];
        u64 movie_time = 0;

        if (t->kind != XF_TRACK_TEXT || !t->sample_size)
            continue;
        if (t->edit_count == 0) {
            if (add_range(mov, t, subs, 0, 0, t->duration) != 0)
                goto fail;
            continue;
        }
        for (e = 0; e < t->edit_count; e++) {
            const xf_mov_edit *ed = &t->edits[e];
            if (ed->media_time >= 0) {
                u64 len = (u64)ed->duration * t->timescale / mov->timescale;
                if (add_range(mov, t, subs, movie_time, (u64)ed->media_time,
                              (u64)ed->media_time + len) != 0)
                    goto fail;
            }
            movie_time += ed->duration;
        }
    }
    if (subs->count > 1)
        qsort(subs->cues, subs->count, sizeof(*subs->cues), cmp_cue);
    return 0;

fail:
    xf_subtitles_free(subs);
    return -1;
}

void xf_subtitles_free(xf_subtitles *subs)
{
    u32 i;
    for (i = 0; i < subs->count; i++)
        free(subs->cues[i].text);
    free(subs->cues);
    memset(subs, 0, sizeof(*subs));
}

const xf_subtitle *xf_subtitles_at(const xf_subtitles *subs, u32 ms)
{
    u32 i;
    for (i = 0; i < subs->count; i++)
        if (subs->cues[i].start_ms <= ms && ms < subs->cues[i].end_ms)
            return &subs->cues[i];
    return NULL;
}
