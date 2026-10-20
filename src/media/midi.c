/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
#include "midi.h"

#include <stdlib.h>
#include <string.h>

#include "xf_endian.h"
#include "xf_file.h"

typedef struct {
    u32 tick;
    u32 order;    /* the event's place in the file, to keep ties stable */
    u8 status, d1, d2;
    u32 tempo;    /* 0, or a tempo change in microseconds a beat */
} raw_event;

typedef struct {
    raw_event *e;
    u32 count, cap;
} raw_list;

static int push(raw_list *l, raw_event ev)
{
    if (l->count == l->cap) {
        u32 cap = l->cap ? l->cap * 2 : 1024;
        raw_event *n = realloc(l->e, cap * sizeof(*n));
        if (!n)
            return -1;
        l->e = n;
        l->cap = cap;
    }
    ev.order = l->count;
    l->e[l->count++] = ev;
    return 0;
}

static int read_var(const u8 **p, const u8 *end, u32 *v)
{
    int k;
    *v = 0;
    for (k = 0; k < 4; k++) {
        if (*p >= end)
            return -1;
        *v = *v << 7 | (**p & 0x7F);
        if (!(*(*p)++ & 0x80))
            return 0;
    }
    return -1;
}

/* One MTrk chunk: delta times, channel messages with running status,
 * system exclusive (F0, F7) and meta events (FF); only the tempo (FF 51)
 * is kept of those. */
static int read_track(const u8 *p, const u8 *end, raw_list *out, u32 *last_tick)
{
    u32 tick = 0, len;
    u8 status = 0;
    while (p < end) {
        u32 delta;
        raw_event ev;
        memset(&ev, 0, sizeof(ev));
        if (read_var(&p, end, &delta) != 0 || p >= end)
            return -1;
        tick += delta;
        if (*p == 0xFF) {
            u8 type;
            if (p + 2 > end)
                return -1;
            type = p[1];
            p += 2;
            if (read_var(&p, end, &len) != 0 || len > (u32)(end - p))
                return -1;
            if (type == 0x51 && len == 3) {
                ev.tick = tick;
                ev.tempo = (u32)p[0] << 16 | (u32)p[1] << 8 | p[2];
                if (ev.tempo && push(out, ev) != 0)
                    return -1;
            }
            p += len;
            if (type == 0x2F)
                break;
            continue;
        }
        if (*p == 0xF0 || *p == 0xF7) {
            p++;
            if (read_var(&p, end, &len) != 0 || len > (u32)(end - p))
                return -1;
            p += len;
            continue;
        }
        if (*p & 0x80)
            status = *p++;
        if (!(status & 0x80))
            return -1; /* data with no status to run on */
        ev.tick = tick;
        ev.status = status;
        if (p >= end)
            return -1;
        ev.d1 = *p++ & 0x7F;
        if ((status & 0xE0) != 0xC0) { /* program and channel pressure take one byte */
            if (p >= end)
                return -1;
            ev.d2 = *p++ & 0x7F;
        }
        if (push(out, ev) != 0)
            return -1;
    }
    *last_tick = tick > *last_tick ? tick : *last_tick;
    return 0;
}

static int by_tick(const void *a, const void *b)
{
    const raw_event *x = a, *y = b;
    if (x->tick != y->tick)
        return x->tick < y->tick ? -1 : 1;
    /* tempo changes first, then the file's order */
    if (!x->tempo != !y->tempo)
        return x->tempo ? -1 : 1;
    return x->order < y->order ? -1 : x->order > y->order;
}

int xf_midi_parse(const u8 *data, size_t size, xf_midi_song *s)
{
    const u8 *p = data, *end = data + size;
    raw_list l;
    u32 i, n = 0, last_tick = 0, tempo = 500000, at_tick = 0;
    u64 at_us = 0;
    int t;

    memset(s, 0, sizeof(*s));
    memset(&l, 0, sizeof(l));
    if (size < 14 || memcmp(p, "MThd", 4) != 0 || xf_be32(p + 4) < 6)
        return -1;
    s->format = xf_be16(p + 8);
    s->tracks = xf_be16(p + 10);
    s->division = (i16)xf_be16(p + 12);
    if (s->division <= 0)
        return -1; /* SMPTE time: not in these games */
    p += 8 + xf_be32(p + 4);
    for (t = 0; t < s->tracks && p + 8 <= end; t++) {
        u32 len = xf_be32(p + 4);
        if (len > (u32)(end - p - 8))
            len = (u32)(end - p - 8);
        if (memcmp(p, "MTrk", 4) == 0 && read_track(p + 8, p + 8 + len, &l, &last_tick) != 0) {
            free(l.e);
            return -1;
        }
        p += 8 + len;
    }
    qsort(l.e, l.count, sizeof(*l.e), by_tick);
    s->events = malloc((l.count ? l.count : 1) * sizeof(*s->events));
    if (!s->events) {
        free(l.e);
        return -1;
    }
    /* ticks to microseconds through the tempo changes */
    for (i = 0; i < l.count; i++) {
        u64 us = at_us + (u64)(l.e[i].tick - at_tick) * tempo / (u32)s->division;
        if (l.e[i].tempo) {
            at_us = us;
            at_tick = l.e[i].tick;
            tempo = l.e[i].tempo;
            continue;
        }
        s->events[n].us = (u32)us;
        s->events[n].status = l.e[i].status;
        s->events[n].d1 = l.e[i].d1;
        s->events[n].d2 = l.e[i].d2;
        n++;
    }
    s->count = n;
    s->length_us = (u32)(at_us + (u64)(last_tick - at_tick) * tempo / (u32)s->division);
    free(l.e);
    return 0;
}

int xf_midi_load(const char *path, xf_midi_song *out)
{
    size_t size;
    u8 *data = xf_load(path, &size);
    int r = data ? xf_midi_parse(data, size, out) : -1;
    if (!data)
        memset(out, 0, sizeof(*out));
    free(data);
    return r;
}

void xf_midi_free(xf_midi_song *s)
{
    free(s->events);
    memset(s, 0, sizeof(*s));
}
