/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf and contributors */
#include "mov.h"

#include <stdlib.h>
#include <string.h>

#include "xf_endian.h"
#include "xf_inflate.h"
#include "xf_log.h"

#define MAX_DEPTH 16

typedef struct {
    xf_mov *mov;
    xf_mov_track *track; /* track being parsed, or NULL */
} parse_ctx;

const char *xf_fourcc_str(u32 cc, char *buf)
{
    int i;
    for (i = 0; i < 4; i++) {
        u8 c = (u8)(cc >> (24 - i * 8));
        buf[i] = c >= 32 && c < 127 ? (char)c : '.';
    }
    buf[4] = '\0';
    return buf;
}

static u8 *read_payload(xf_file *f, u64 pos, u64 size)
{
    u8 *p;
    if (size > 64u << 20)
        return NULL;
    p = malloc(size ? (size_t)size : 1);
    if (p && xf_read_at(f, pos, p, (size_t)size) != 0) {
        free(p);
        return NULL;
    }
    return p;
}

static int parse_stsd(xf_mov_track *t, const u8 *p, u64 n)
{
    u32 count, i;
    u64 pos = 8;

    if (n < 8)
        return -1;
    count = XF_MIN(xf_be32(p + 4), XF_MOV_MAX_DESCS);
    for (i = 0; i < count; i++) {
        xf_mov_desc *e = &t->descs[i];
        u32 size;
        const u8 *d;
        if (pos + 16 > n)
            return -1;
        size = xf_be32(p + pos);
        if (size < 16 || pos + size > n)
            return -1;
        e->codec = xf_be32(p + pos + 4);
        e->size = size - 8;
        e->data = malloc(e->size);
        if (!e->data)
            return -1;
        memcpy(e->data, p + pos + 8, e->size);
        t->desc_count = i + 1;
        d = e->data; /* 6 reserved bytes, then data reference index */
        if (t->kind == XF_TRACK_VIDEO && e->size >= 78) {
            e->width = xf_be16(d + 24);
            e->height = xf_be16(d + 26);
            e->depth = xf_be16(d + 74);
        }
        if (i == 0 && t->kind == XF_TRACK_AUDIO && e->size >= 28) {
            u16 version = xf_be16(d + 8);
            t->channels = xf_be16(d + 16);
            t->bits = xf_be16(d + 18);
            t->sample_rate = xf_be32(d + 24) >> 16;
            if (version == 1 && e->size >= 44) {
                t->samples_per_packet = xf_be32(d + 28);
                t->bytes_per_packet = xf_be32(d + 32);
                t->bytes_per_frame = xf_be32(d + 36);
            }
        }
        pos += size;
    }
    if (t->desc_count) {
        t->codec = t->descs[0].codec;
        t->desc = t->descs[0].data;
        t->desc_size = t->descs[0].size;
        t->width = t->descs[0].width;
        t->height = t->descs[0].height;
        t->depth = t->descs[0].depth;
    }
    return 0;
}

#define ALLOC_TABLE(ptr, count, n_avail, entry)                                \
    do {                                                                       \
        if ((u64)(count) * (entry) > (n_avail))                                \
            return -1;                                                         \
        (ptr) = calloc((count) ? (count) : 1, sizeof(*(ptr)));                 \
        if (!(ptr))                                                            \
            return -1;                                                         \
    } while (0)

static int parse_leaf(parse_ctx *c, u32 type, const u8 *p, u64 n)
{
    xf_mov_track *t = c->track;
    u32 i, count;

    if (type == XF_FOURCC('m', 'v', 'h', 'd') && n >= 20 && !t) {
        c->mov->timescale = xf_be32(p + 12);
        c->mov->duration = xf_be32(p + 16);
        return 0;
    }
    if (!t)
        return 0;
    switch (type) {
    case XF_FOURCC('t', 'k', 'h', 'd'):
        if (n >= 16)
            t->id = xf_be32(p + 12);
        break;
    case XF_FOURCC('m', 'd', 'h', 'd'):
        if (n >= 20) {
            t->timescale = xf_be32(p + 12);
            t->duration = xf_be32(p + 16);
        }
        break;
    case XF_FOURCC('h', 'd', 'l', 'r'):
        /* Only the media handler names the track type; 'minf' also has a
         * data handler ('alis'), which we ignore. */
        if (n >= 12 && xf_be32(p + 4) == XF_FOURCC('m', 'h', 'l', 'r')) {
            t->handler = xf_be32(p + 8);
            t->kind = t->handler == XF_FOURCC('v', 'i', 'd', 'e')   ? XF_TRACK_VIDEO
                      : t->handler == XF_FOURCC('s', 'o', 'u', 'n') ? XF_TRACK_AUDIO
                      : t->handler == XF_FOURCC('t', 'e', 'x', 't') ? XF_TRACK_TEXT
                                                                    : XF_TRACK_OTHER;
        }
        break;
    case XF_FOURCC('s', 't', 's', 'd'):
        return parse_stsd(t, p, n);
    case XF_FOURCC('e', 'l', 's', 't'):
        if (n < 8)
            return -1;
        count = xf_be32(p + 4);
        ALLOC_TABLE(t->edits, count, n - 8, 12);
        t->edit_count = count;
        for (i = 0; i < count; i++) {
            t->edits[i].duration = xf_be32(p + 8 + i * 12);
            t->edits[i].media_time = (i32)xf_be32(p + 12 + i * 12);
            t->edits[i].rate = xf_be32(p + 16 + i * 12);
        }
        break;
    case XF_FOURCC('s', 't', 't', 's'):
        if (n < 8)
            return -1;
        count = xf_be32(p + 4);
        ALLOC_TABLE(t->tts, count, n - 8, 8);
        t->tts_count = count;
        for (i = 0; i < count; i++) {
            t->tts[i].count = xf_be32(p + 8 + i * 8);
            t->tts[i].duration = xf_be32(p + 12 + i * 8);
        }
        break;
    case XF_FOURCC('s', 't', 's', 's'):
        if (n < 8)
            return -1;
        count = xf_be32(p + 4);
        ALLOC_TABLE(t->sync, count, n - 8, 4);
        t->sync_count = count;
        for (i = 0; i < count; i++)
            t->sync[i] = xf_be32(p + 8 + i * 4);
        break;
    case XF_FOURCC('s', 't', 's', 'c'):
        if (n < 8)
            return -1;
        count = xf_be32(p + 4);
        ALLOC_TABLE(t->stc, count, n - 8, 12);
        t->stc_count = count;
        for (i = 0; i < count; i++) {
            t->stc[i].first_chunk = xf_be32(p + 8 + i * 12);
            t->stc[i].samples_per_chunk = xf_be32(p + 12 + i * 12);
            t->stc[i].desc_index = xf_be32(p + 16 + i * 12);
        }
        break;
    case XF_FOURCC('s', 't', 's', 'z'):
        if (n < 12)
            return -1;
        t->sample_const_size = xf_be32(p + 4);
        count = xf_be32(p + 8);
        t->sample_count = count;
        if (t->sample_const_size == 0) {
            ALLOC_TABLE(t->sample_size, count, n - 12, 4);
            for (i = 0; i < count; i++)
                t->sample_size[i] = xf_be32(p + 12 + i * 4);
        }
        break;
    case XF_FOURCC('s', 't', 'c', 'o'):
        if (n < 8)
            return -1;
        count = xf_be32(p + 4);
        ALLOC_TABLE(t->chunk_offset, count, n - 8, 4);
        t->chunk_count = count;
        for (i = 0; i < count; i++)
            t->chunk_offset[i] = xf_be32(p + 8 + i * 4);
        break;
    case XF_FOURCC('c', 'o', '6', '4'):
        if (n < 8)
            return -1;
        count = xf_be32(p + 4);
        ALLOC_TABLE(t->chunk_offset, count, n - 8, 8);
        t->chunk_count = count;
        for (i = 0; i < count; i++)
            t->chunk_offset[i] = xf_be64(p + 8 + i * 8);
        break;
    }
    return 0;
}

static int is_container(u32 type)
{
    return type == XF_FOURCC('m', 'o', 'o', 'v') || type == XF_FOURCC('t', 'r', 'a', 'k') ||
           type == XF_FOURCC('m', 'd', 'i', 'a') || type == XF_FOURCC('m', 'i', 'n', 'f') ||
           type == XF_FOURCC('s', 't', 'b', 'l') || type == XF_FOURCC('e', 'd', 't', 's');
}

static int parse_atoms(parse_ctx *c, const u8 *p, u64 n, int depth);

/* A compressed movie header: 'dcom' names the method, 'cmvd' holds the
 * uncompressed size and the zlib data of a complete 'moov' atom. */
static int parse_cmov(parse_ctx *c, const u8 *p, u64 n, int depth)
{
    u64 pos = 0;
    u32 method = 0;
    u8 *out;
    long len;
    int r;

    while (pos + 8 <= n) {
        u32 size = xf_be32(p + pos), type = xf_be32(p + pos + 4);
        if (size < 8 || pos + size > n)
            return -1;
        if (type == XF_FOURCC('d', 'c', 'o', 'm') && size >= 12)
            method = xf_be32(p + pos + 8);
        if (type == XF_FOURCC('c', 'm', 'v', 'd') && size >= 12) {
            u32 out_size = xf_be32(p + pos + 8);
            if (method != XF_FOURCC('z', 'l', 'i', 'b') || out_size > 64u << 20)
                return -1;
            out = malloc(out_size ? out_size : 1);
            if (!out)
                return -1;
            len = xf_zlib_uncompress(out, out_size, p + pos + 12, size - 12);
            r = len < 0 ? -1 : parse_atoms(c, out, (u64)len, depth + 1);
            free(out);
            return r;
        }
        pos += size;
    }
    return -1;
}

static int parse_atoms(parse_ctx *c, const u8 *p, u64 n, int depth)
{
    u64 pos = 0;

    if (depth > MAX_DEPTH)
        return -1;
    while (pos + 8 <= n) {
        u64 size = xf_be32(p + pos), head = 8;
        u32 type = xf_be32(p + pos + 4);
        int r = 0;

        if (size == 1) {
            if (pos + 16 > n)
                break;
            size = xf_be64(p + pos + 8);
            head = 16;
        } else if (size == 0) {
            size = n - pos;
        }
        if (size < head || pos + size > n)
            break; /* trailing garbage; keep what we have */

        if (type == XF_FOURCC('t', 'r', 'a', 'k')) {
            xf_mov *m = c->mov;
            xf_mov_track *nt = realloc(m->tracks, (m->track_count + 1) * sizeof(*nt));
            if (!nt)
                return -1;
            m->tracks = nt;
            c->track = &m->tracks[m->track_count++];
            memset(c->track, 0, sizeof(*c->track));
            c->track->kind = XF_TRACK_OTHER;
            r = parse_atoms(c, p + pos + head, size - head, depth + 1);
            c->track = NULL;
        } else if (type == XF_FOURCC('c', 'm', 'o', 'v')) {
            r = parse_cmov(c, p + pos + head, size - head, depth);
        } else if (is_container(type)) {
            r = parse_atoms(c, p + pos + head, size - head, depth + 1);
        } else {
            r = parse_leaf(c, type, p + pos + head, size - head);
        }
        if (r != 0)
            return -1;
        pos += size;
    }
    return 0;
}

/* Scans the top-level atoms of the file and parses the movie header. */
static int parse_file(parse_ctx *c)
{
    xf_file *f = c->mov->file;
    u64 pos = 0, end = xf_size(f);

    while (pos + 8 <= end) {
        u8 h[16];
        u64 size, head = 8;
        u32 type;

        if (xf_read_at(f, pos, h, 8) != 0)
            return -1;
        size = xf_be32(h);
        type = xf_be32(h + 4);
        if (size == 1) {
            if (xf_read_at(f, pos + 8, h + 8, 8) != 0)
                return -1;
            size = xf_be64(h + 8);
            head = 16;
        } else if (size == 0) {
            size = end - pos;
        }
        if (size < head || pos + size > end)
            return -1;
        if (type == XF_FOURCC('m', 'o', 'o', 'v')) {
            u8 *p = read_payload(f, pos + head, size - head);
            int r;
            if (!p)
                return -1;
            r = parse_atoms(c, p, size - head, 1);
            free(p);
            return r;
        }
        pos += size;
    }
    return -1; /* no movie header */
}

/* Works out how many samples each chunk holds and where it starts. */
static int build_chunk_index(xf_mov_track *t)
{
    u32 run, chunk, s = 0;

    t->chunk_samples = calloc(t->chunk_count ? t->chunk_count : 1, sizeof(u32));
    t->chunk_first = calloc(t->chunk_count ? t->chunk_count : 1, sizeof(u32));
    if (!t->chunk_samples || !t->chunk_first)
        return -1;
    for (run = 0; run < t->stc_count; run++) {
        u32 first = t->stc[run].first_chunk;
        u32 last = run + 1 < t->stc_count ? t->stc[run + 1].first_chunk : t->chunk_count + 1;
        for (chunk = first; chunk < last && chunk >= 1 && chunk <= t->chunk_count; chunk++)
            t->chunk_samples[chunk - 1] = t->stc[run].samples_per_chunk;
    }
    for (chunk = 0; chunk < t->chunk_count; chunk++) {
        t->chunk_first[chunk] = s;
        s += t->chunk_samples[chunk];
    }
    return 0;
}

/* Returns the 0-based sample description index used by `chunk`. */
static u32 chunk_desc(const xf_mov_track *t, u32 chunk)
{
    u32 run, d = 0;
    for (run = 0; run < t->stc_count && t->stc[run].first_chunk <= chunk + 1; run++)
        d = t->stc[run].desc_index;
    return d >= 1 && d <= t->desc_count ? d - 1 : 0;
}

/* For video and text tracks, expands the chunk tables into one offset and
 * one start time per sample. Audio tracks, where every PCM frame counts
 * as a sample, are read by chunk instead. */
static int build_sample_index(xf_mov_track *t)
{
    u32 s = 0, chunk, i, j, time = 0;

    if (build_chunk_index(t) != 0)
        return -1;
    if (t->sample_count == 0 || t->kind == XF_TRACK_AUDIO)
        return 0;
    if (t->sample_const_size) {
        t->sample_size = malloc(t->sample_count * sizeof(u32));
        if (!t->sample_size)
            return -1;
        for (i = 0; i < t->sample_count; i++)
            t->sample_size[i] = t->sample_const_size;
    }
    t->sample_offset = malloc(t->sample_count * sizeof(u64));
    t->sample_time = malloc(t->sample_count * sizeof(u32));
    t->sample_desc = calloc(t->sample_count, 1);
    if (!t->sample_offset || !t->sample_time || !t->sample_desc)
        return -1;

    for (chunk = 0; chunk < t->chunk_count && s < t->sample_count; chunk++) {
        u64 off = t->chunk_offset[chunk];
        u32 desc = chunk_desc(t, chunk);
        for (j = 0; j < t->chunk_samples[chunk] && s < t->sample_count; j++, s++) {
            t->sample_offset[s] = off;
            t->sample_desc[s] = (u8)desc;
            off += t->sample_size[s];
        }
    }
    if (s != t->sample_count) {
        xf_warn("mov: track %u maps %u of %u samples", t->id, s, t->sample_count);
        t->sample_count = s;
    }

    s = 0;
    for (i = 0; i < t->tts_count; i++)
        for (j = 0; j < t->tts[i].count && s < t->sample_count; j++, s++) {
            t->sample_time[s] = time;
            time += t->tts[i].duration;
        }
    for (; s < t->sample_count; s++)
        t->sample_time[s] = time;
    return 0;
}

static void free_track(xf_mov_track *t)
{
    u32 i;
    for (i = 0; i < t->desc_count; i++)
        free(t->descs[i].data);
    free(t->sample_desc);
    free(t->sample_size);
    free(t->sample_offset);
    free(t->sample_time);
    free(t->tts);
    free(t->sync);
    free(t->chunk_offset);
    free(t->stc);
    free(t->edits);
    free(t->chunk_samples);
    free(t->chunk_first);
}

int xf_mov_open(xf_mov *mov, const char *path)
{
    parse_ctx c;
    u32 i;

    memset(mov, 0, sizeof(*mov));
    mov->file = xf_open(path);
    if (!mov->file)
        return -1;
    c.mov = mov;
    c.track = NULL;
    if (parse_file(&c) != 0 || mov->timescale == 0)
        goto fail;
    for (i = 0; i < mov->track_count; i++)
        if (build_sample_index(&mov->tracks[i]) != 0)
            goto fail;
    return 0;

fail:
    xf_error("%s: not a valid QuickTime movie", path);
    xf_mov_close(mov);
    return -1;
}

void xf_mov_close(xf_mov *mov)
{
    u32 i;
    for (i = 0; i < mov->track_count; i++)
        free_track(&mov->tracks[i]);
    free(mov->tracks);
    xf_close(mov->file);
    memset(mov, 0, sizeof(*mov));
}

xf_mov_track *xf_mov_find(xf_mov *mov, int kind)
{
    u32 i;
    for (i = 0; i < mov->track_count; i++)
        if (mov->tracks[i].kind == kind)
            return &mov->tracks[i];
    return NULL;
}

u32 xf_mov_sample_at(const xf_mov_track *t, u32 time)
{
    u32 lo = 0, hi = t->sample_count;
    if (hi == 0 || !t->sample_time)
        return 0;
    while (hi - lo > 1) {
        u32 mid = (lo + hi) / 2;
        if (t->sample_time[mid] <= time)
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}

int xf_mov_is_sync(const xf_mov_track *t, u32 sample)
{
    u32 lo = 0, hi = t->sync_count;
    if (t->sync_count == 0)
        return 1;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (t->sync[mid] == sample + 1)
            return 1;
        if (t->sync[mid] < sample + 1)
            lo = mid + 1;
        else
            hi = mid;
    }
    return 0;
}

int xf_mov_read_sample(xf_mov *mov, const xf_mov_track *t, u32 index, void *dst)
{
    if (index >= t->sample_count || !t->sample_offset)
        return -1;
    return xf_read_at(mov->file, t->sample_offset[index], dst, t->sample_size[index]);
}

u32 xf_mov_chunk_at(const xf_mov_track *t, u32 sample)
{
    u32 lo = 0, hi = t->chunk_count;
    if (hi == 0)
        return 0;
    while (hi - lo > 1) {
        u32 mid = (lo + hi) / 2;
        if (t->chunk_first[mid] <= sample)
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}

u32 xf_mov_media_start(const xf_mov_track *t)
{
    u32 i;
    for (i = 0; i < t->edit_count; i++)
        if (t->edits[i].media_time >= 0)
            return (u32)t->edits[i].media_time;
    return 0;
}

int xf_mov_edit_lookup(const xf_mov *mov, const xf_mov_track *t, u64 movie_time, u64 *media)
{
    u64 start = 0;
    u32 i;

    if (t->edit_count == 0) {
        *media = movie_time * t->timescale / mov->timescale;
        return *media < t->duration ? 0 : -1;
    }
    for (i = 0; i < t->edit_count; i++) {
        const xf_mov_edit *e = &t->edits[i];
        if (movie_time < start + e->duration) {
            if (e->media_time < 0)
                return -1;
            *media = (u64)e->media_time + (movie_time - start) * t->timescale / mov->timescale;
            return 0;
        }
        start += e->duration;
    }
    return -1;
}
