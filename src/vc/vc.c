/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf and contributors */
#include "vc.h"

#include <stdlib.h>
#include <string.h>

int vc_class(const xf_neo_db *db, u32 id)
{
    const xf_neo_entry *e = xf_neo_find(db, id);
    return e ? e->class_id : -1;
}

static int open_obj(const xf_neo_db *db, u32 id, int cls, xf_neo_reader *r)
{
    if (vc_class(db, id) != cls)
        return -1;
    return xf_neo_object(db, id, r, NULL, NULL);
}

static vc_region read_region(xf_neo_reader *r)
{
    vc_region g;
    g.cursor = xf_neo_u32(r);
    g.left = (i16)xf_neo_u16(r);
    g.top = (i16)xf_neo_u16(r);
    g.right = (i16)xf_neo_u16(r);
    g.bottom = (i16)xf_neo_u16(r);
    return g;
}

/* 0x5EDFB0 VCView_read */
int vc_read_view(const xf_neo_db *db, u32 id, vc_view *v)
{
    xf_neo_reader r;
    if (open_obj(db, id, VC_VIEW, &r) != 0)
        return -1;
    v->direction = xf_neo_u32(&r);
    v->assets = xf_neo_u32(&r);
    v->characters = xf_neo_u32(&r);
    v->navs = xf_neo_u32(&r);
    v->explorables = xf_neo_u32(&r);
    v->triggers = xf_neo_u32(&r);
    return r.error ? -1 : 0;
}

int vc_read_nav(const xf_neo_db *db, u32 id, vc_nav *n)
{
    xf_neo_reader r;
    if (open_obj(db, id, VC_NAV, &r) != 0)
        return -1;
    n->target = xf_neo_u32(&r);
    n->hotspot = xf_neo_u32(&r);
    n->extra = xf_neo_u32(&r);
    return r.error ? -1 : 0;
}

int vc_read_hotspot(const xf_neo_db *db, u32 id, vc_hotspot *h)
{
    xf_neo_reader r;
    if (open_obj(db, id, VC_HOTSPOT, &r) != 0)
        return -1;
    h->region = read_region(&r);
    h->extra = xf_neo_u32(&r);
    return r.error ? -1 : 0;
}

int vc_read_iface_item(const xf_neo_db *db, u32 id, vc_iface_item *it)
{
    xf_neo_reader r;
    if (open_obj(db, id, VC_IFACE_ITEM, &r) != 0)
        return -1;
    it->region = read_region(&r);
    it->assets = xf_neo_u32(&r);
    it->triggers = xf_neo_u32(&r);
    it->flags = xf_neo_u16(&r);
    return r.error ? -1 : 0;
}

int vc_read_conversation(const xf_neo_db *db, u32 id, vc_conversation *c)
{
    xf_neo_reader r;
    memset(c, 0, sizeof(*c));
    if (open_obj(db, id, VC_CONVERSATION, &r) != 0)
        return -1;
    c->name = xf_neo_u32(&r);
    c->icons = xf_neo_u32(&r);
    c->answer = xf_neo_u32(&r);
    c->triggers = xf_neo_u32(&r);
    if (r.error)
        return -1;
    /* Later schema versions add the question, the repeat answer and a flag. */
    c->question = xf_neo_u32(&r);
    c->repeat = xf_neo_u32(&r);
    c->flag = xf_neo_u8(&r);
    if (r.error)
        c->question = c->repeat = c->flag = 0;
    return 0;
}

int vc_read_char_view(const xf_neo_db *db, u32 id, vc_char_view *c)
{
    xf_neo_reader r;
    if (open_obj(db, id, VC_CHAR_VIEW, &r) != 0)
        return -1;
    c->character = xf_neo_u32(&r);
    c->region = read_region(&r);
    c->conversations = xf_neo_u32(&r);
    c->idea_maps = xf_neo_u32(&r);
    c->assets = xf_neo_u32(&r);
    c->triggers = xf_neo_u32(&r);
    return r.error ? -1 : 0;
}

/* Reads one token: a length byte and that many characters. */
static int token(const u8 **p, const u8 *end, char *out, size_t size)
{
    size_t n;
    if (*p >= end)
        return -1;
    n = **p;
    if (*p + 1 + n > end)
        return -1;
    if (out) {
        size_t k = XF_MIN(n, size - 1), i;
        for (i = 0; i < k; i++)
            out[i] = (*p)[1 + i] == 0x7F ? '/' : (char)(*p)[1 + i];
        out[k] = '\0';
    }
    *p += 1 + n;
    return 0;
}

/* A length token followed by raw characters: "54" then 54 bytes. */
static int path_token(const u8 **p, const u8 *end, char *out, size_t size)
{
    char num[16];
    size_t n, i, k;
    if (token(p, end, num, sizeof(num)) != 0)
        return -1;
    n = (size_t)strtoul(num, NULL, 10);
    if (*p + n > end)
        return -1;
    k = XF_MIN(n, size - 1);
    for (i = 0; i < k; i++)
        out[i] = (*p)[i] == 0x7F ? '/' : (char)(*p)[i];
    out[k] = '\0';
    *p += n;
    return 0;
}

static u32 num_token(const u8 **p, const u8 *end, int *ok)
{
    char num[16];
    if (token(p, end, num, sizeof(num)) != 0) {
        *ok = 0;
        return 0;
    }
    return (u32)strtoul(num, NULL, 10);
}

int vc_parse_media(const u8 *data, size_t size, vc_asset_ref *a)
{
    const u8 *p = data, *end = data + size;
    int ok = 1, n = 0;

    a->entry_count = 0;
    if (path_token(&p, end, a->logical, sizeof(a->logical)) != 0)
        return -1;
    /* Numbers up to the first path: [start end] then a 0, then the disc of
     * the first entry. A path token follows the disc number. */
    while (ok && n < 4 && p < end) {
        const u8 *save = p;
        /* A token is the disc number when the next token is a path length
         * followed by a path. */
        num_token(&p, end, &ok);
        n++;
        if (!ok)
            break;
        {
            const u8 *q = p;
            char tmp[VC_PATH_MAX];
            if (path_token(&q, end, tmp, sizeof(tmp)) == 0 && strchr(tmp, '.')) {
                p = save;
                n--;
                break;
            }
        }
    }
    while (ok && p < end && a->entry_count < VC_MAX_DISC_ENTRIES) {
        vc_media_entry *e = &a->entries[a->entry_count];
        e->disc = (int)num_token(&p, end, &ok);
        if (!ok || path_token(&p, end, e->path, sizeof(e->path)) != 0)
            break;
        e->start = num_token(&p, end, &ok);
        e->end = num_token(&p, end, &ok);
        if (!ok)
            break;
        a->entry_count++;
    }
    return a->entry_count > 0 ? 0 : -1;
}

int vc_read_asset_ref(const xf_neo_db *db, u32 id, vc_asset_ref *a)
{
    xf_neo_reader r, blob;
    u32 packed;
    memset(a, 0, sizeof(*a));
    if (open_obj(db, id, VC_ASSET_REF, &r) != 0)
        return -1;
    blob = xf_neo_blob(db, &r);
    a->name = xf_neo_u32(&r);
    /* One u32: flags in the top byte, the media's trigger list below. */
    packed = xf_neo_u32(&r);
    a->flags = (u8)(packed >> 24);
    a->triggers = packed & 0xFFFFFF;
    a->kind = xf_neo_u8(&r);
    a->extra = xf_neo_u8(&r);
    if (r.error || !blob.p)
        return -1;
    return vc_parse_media(blob.p, (size_t)(blob.end - blob.p), a);
}

/* 0x62B0B0 VCName_read */
int vc_read_name(const xf_neo_db *db, u32 id, char *out, size_t size)
{
    xf_neo_reader r, b;
    size_t n;
    out[0] = '\0';
    if (open_obj(db, id, VC_NAME, &r) != 0)
        return -1;
    b = xf_neo_blob(db, &r);
    if (!b.p)
        return 0;
    n = XF_MIN((size_t)(b.end - b.p), size - 1);
    memcpy(out, b.p, n);
    out[n] = '\0';
    return 0;
}

typedef struct {
    u32 *ids;
    int max, n;
} collect;

static void collect_id(void *user, u32 id)
{
    collect *c = user;
    if (c->n < c->max)
        c->ids[c->n] = id;
    c->n++;
}

int vc_read_list(const xf_neo_db *db, u32 id, u32 *ids, int max)
{
    xf_neo_reader r;
    xf_neo_list l;
    collect c;
    c.ids = ids;
    c.max = max;
    c.n = 0;
    if (!id || xf_neo_object(db, id, &r, NULL, NULL) != 0)
        return -1;
    l = xf_neo_read_list(&r);
    if (r.error || xf_neo_list_ids(db, &l, collect_id, &c) < 0)
        return -1;
    return XF_MIN(c.n, max);
}

static int cmp_place(const void *a, const void *b)
{
    u32 x = ((const vc_place *)a)->view, y = ((const vc_place *)b)->view;
    return x < y ? -1 : x > y;
}

/* Walks title -> nodes -> locations -> viewpoints -> views. Field offsets
 * follow the layouts in docs/engine/classes.md. */
int vc_place_map_build(const xf_neo_db *db, vc_place_map *m)
{
    u32 nodes[64], locs[256], vps[512], views[64], i;
    int nn, nl, nv, nw, a, b, c, d;
    u32 cap = 0;

    memset(m, 0, sizeof(*m));
    for (i = 0; i < db->count && db->entries[i].class_id != VC_TITLE; i++)
        ;
    if (i == db->count)
        return -1;
    nn = vc_read_list(db, db->entries[i].id, nodes, 64);
    for (a = 0; a < nn; a++) {
        xf_neo_reader r;
        u32 node_triggers;
        if (xf_neo_object(db, nodes[a], &r, NULL, NULL) != 0)
            continue;
        xf_neo_read_list(&r);
        xf_neo_u32(&r);             /* name */
        node_triggers = xf_neo_u32(&r);
        nl = vc_read_list(db, nodes[a], locs, 256);
        for (b = 0; b < nl; b++) {
            u32 loc_triggers;
            if (xf_neo_object(db, locs[b], &r, NULL, NULL) != 0)
                continue;
            xf_neo_read_list(&r);
            xf_neo_u32(&r);         /* name */
            xf_neo_u32(&r);         /* media list */
            loc_triggers = xf_neo_u32(&r);
            nv = vc_read_list(db, locs[b], vps, 512);
            for (c = 0; c < nv; c++) {
                u32 vp_triggers;
                if (xf_neo_object(db, vps[c], &r, NULL, NULL) != 0)
                    continue;
                xf_neo_read_list(&r);
                xf_neo_u32(&r);     /* position: x, y */
                xf_neo_u32(&r);     /* position: name */
                vp_triggers = xf_neo_u32(&r);
                nw = vc_read_list(db, vps[c], views, 64);
                for (d = 0; d < nw; d++) {
                    vc_place *p;
                    if (m->count == cap) {
                        vc_place *n;
                        cap = cap ? cap * 2 : 1024;
                        n = realloc(m->places, cap * sizeof(*n));
                        if (!n)
                            return -1;
                        m->places = n;
                    }
                    p = &m->places[m->count++];
                    p->view = views[d];
                    p->viewpoint = vps[c];
                    p->location = locs[b];
                    p->node = nodes[a];
                    p->viewpoint_triggers = vp_triggers;
                    p->location_triggers = loc_triggers;
                    p->node_triggers = node_triggers;
                }
            }
        }
    }
    qsort(m->places, m->count, sizeof(*m->places), cmp_place);
    return 0;
}

void vc_place_map_free(vc_place_map *m)
{
    free(m->places);
    memset(m, 0, sizeof(*m));
}

const vc_place *vc_place_of(const vc_place_map *m, u32 view)
{
    u32 lo = 0, hi = m->count;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (m->places[mid].view == view)
            return &m->places[mid];
        if (m->places[mid].view < view)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}
