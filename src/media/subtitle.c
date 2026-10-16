/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
#include "subtitle.h"

#include <stdio.h>
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

static int add_utf8(xf_subtitles *subs, u32 start, u32 end, const char *text)
{
    xf_subtitle *n = realloc(subs->cues, (subs->count + 1) * sizeof(*n));
    char *s = malloc(strlen(text) + 1);
    if (!s || !n) {
        free(s);
        if (n)
            subs->cues = n;
        return -1;
    }
    strcpy(s, text);
    subs->cues = n;
    n[subs->count].start_ms = start;
    n[subs->count].end_ms = end;
    n[subs->count].track_id = 0;
    n[subs->count].text = s;
    subs->count++;
    return 0;
}

/* "hh:mm:ss,mmm" */
static int srt_time(const char *s, u32 *ms)
{
    unsigned h, m, sec, f;
    if (sscanf(s, "%u:%u:%u%*[,.]%u", &h, &m, &sec, &f) != 4)
        return -1;
    *ms = ((h * 60 + m) * 60 + sec) * 1000 + f;
    return 0;
}

int xf_subtitles_load_srt(const char *path, xf_subtitles *subs)
{
    FILE *f = fopen(path, "rb");
    char line[512], text[1024];
    u32 start = 0, end = 0;
    int in_cue = 0;
    memset(subs, 0, sizeof(*subs));
    if (!f)
        return -1;
    text[0] = '\0';
    for (;;) {
        char *got = fgets(line, sizeof(line), f), *arrow;
        char *p = line;
        if (got) {
            line[strcspn(line, "\r\n")] = '\0';
            if ((u8)p[0] == 0xEF && (u8)p[1] == 0xBB && (u8)p[2] == 0xBF)
                p += 3;
        }
        if (!got || !*p) { /* a blank line ends a cue */
            if (in_cue && text[0] && add_utf8(subs, start, end, text) != 0)
                break;
            in_cue = 0;
            text[0] = '\0';
            if (!got)
                break;
        } else if (!in_cue && (arrow = strstr(p, "-->")) != NULL) {
            in_cue = srt_time(p, &start) == 0 && srt_time(arrow + 3 + strspn(arrow + 3, " "), &end) == 0;
        } else if (in_cue) {
            size_t n = strlen(text);
            snprintf(text + n, sizeof(text) - n, "%s%s", n ? "\n" : "", p);
        }
    }
    fclose(f);
    if (subs->count > 1)
        qsort(subs->cues, subs->count, sizeof(*subs->cues), cmp_cue);
    return 0;
}

void xf_subtitles_stretch(xf_subtitles *subs, u32 from_ms, u32 to_ms)
{
    u32 i;
    if (!from_ms || !to_ms || from_ms == to_ms)
        return;
    for (i = 0; i < subs->count; i++) {
        subs->cues[i].start_ms = (u32)((u64)subs->cues[i].start_ms * to_ms / from_ms);
        subs->cues[i].end_ms = (u32)((u64)subs->cues[i].end_ms * to_ms / from_ms);
    }
}

int xf_subtitles_shown(const xf_subtitles *in, xf_subtitles *out)
{
    u32 *t, n = 0, i, k;
    memset(out, 0, sizeof(*out));
    if (!in->count)
        return 0;
    if (!(t = malloc(in->count * 2 * sizeof(*t))))
        return -1;
    for (i = 0; i < in->count; i++) {
        t[n++] = in->cues[i].start_ms;
        t[n++] = in->cues[i].end_ms;
    }
    for (i = 1; i < n; i++) /* sort the change times */
        for (k = i; k > 0 && t[k - 1] > t[k]; k--) {
            u32 x = t[k];
            t[k] = t[k - 1];
            t[k - 1] = x;
        }
    for (i = 0; i + 1 < n; i++) {
        const xf_subtitle *c;
        if (t[i] == t[i + 1] || !(c = xf_subtitles_at(in, t[i])))
            continue;
        if (out->count && out->cues[out->count - 1].end_ms == t[i] &&
            strcmp(out->cues[out->count - 1].text, c->text) == 0)
            out->cues[out->count - 1].end_ms = t[i + 1]; /* the same line goes on */
        else if (add_utf8(out, t[i], t[i + 1], c->text) != 0)
            break;
    }
    free(t);
    return 0;
}
