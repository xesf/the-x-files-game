/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
#include "neo.h"

#include <stdlib.h>
#include <string.h>

#include "xf_endian.h"
#include "xf_file.h"
#include "xf_log.h"

#define HEADER_VERSION 5
#define FILE_FORMAT 0x501
#define NODE_HEADER 8
#define INDEX_ID 7
#define MAX_DEPTH 32

static int has(const xf_neo_db *db, u64 at, u64 n) { return at <= db->size && n <= db->size - at; }

typedef struct {
    xf_neo_db *db;
    u32 cap;
} builder;

static int add_entry(builder *b, u32 id, u32 offset, u8 class_id)
{
    xf_neo_db *db = b->db;
    if (db->count == b->cap) {
        u32 cap = b->cap ? b->cap * 2 : 4096;
        xf_neo_entry *n = realloc(db->entries, cap * sizeof(*n));
        if (!n)
            return -1;
        db->entries = n;
        b->cap = cap;
    }
    db->entries[db->count].id = id;
    db->entries[db->count].offset = offset;
    db->entries[db->count].class_id = class_id;
    db->count++;
    return 0;
}

/* Walks an ID index: branch nodes hold child offsets, leaf nodes hold
 * (record offset, object id) pairs. */
static int walk_objects(builder *b, u32 node, u8 class_id, int depth)
{
    const u8 *d = b->db->data;
    u32 count, i;

    if (!node)
        return 0;
    if (depth > MAX_DEPTH || node < 32 || !has(b->db, node, NODE_HEADER))
        return -1;
    count = xf_be16(d + node + 6);
    if (!(d[node] & 0x80)) {
        if (count > 255 || !has(b->db, node + NODE_HEADER, (u64)count * 4))
            return -1;
        for (i = 0; i < count; i++)
            if (walk_objects(b, xf_be32(d + node + NODE_HEADER + i * 4), class_id, depth + 1) != 0)
                return -1;
        return 0;
    }
    if (count > 255 || !has(b->db, node + NODE_HEADER, (u64)count * 8))
        return -1;
    for (i = 0; i < count; i++) {
        const u8 *e = d + node + NODE_HEADER + i * 8;
        u32 offset = xf_be32(e), id = xf_be32(e + 4);
        if (offset && !has(b->db, offset, 6))
            return -1;
        if (add_entry(b, id, offset, class_id) != 0)
            return -1;
    }
    return 0;
}

/* Walks the class index: its leaf entries list, per class, the roots of
 * that class's indexes. Only the ID index (type 7) is used. */
static int walk_classes(builder *b, u32 node, int depth)
{
    const u8 *d = b->db->data;
    u32 count, i, at;

    if (depth > MAX_DEPTH || node < 32 || !has(b->db, node, NODE_HEADER))
        return -1;
    count = xf_be16(d + node + 6);
    at = node + NODE_HEADER;
    if (!(d[node] & 0x80)) {
        if (count > 255 || !has(b->db, at, (u64)count * 4))
            return -1;
        for (i = 0; i < count; i++)
            if (walk_classes(b, xf_be32(d + at + i * 4), depth + 1) != 0)
                return -1;
        return 0;
    }
    for (i = 0; i < count; i++) {
        u32 n, j;
        if (!has(b->db, at, 27))
            return -1;
        n = d[at + 10];
        at += 27;
        if (n > 6 || !has(b->db, at, (u64)n * 16))
            return -1;
        for (j = 0; j < n; j++, at += 16) {
            u32 type = xf_be32(d + at), root = xf_be32(d + at + 4), cls = xf_be32(d + at + 8);
            if (type == INDEX_ID && cls < 256 && walk_objects(b, root, (u8)cls, 0) != 0) {
                xf_warn("hdb: damaged index for class 0x%02X", cls);
                return -1;
            }
        }
    }
    return 0;
}

static int cmp_entry(const void *a, const void *b)
{
    u32 x = ((const xf_neo_entry *)a)->id, y = ((const xf_neo_entry *)b)->id;
    return x < y ? -1 : x > y;
}

int xf_neo_init(xf_neo_db *db, u8 *data, size_t size)
{
    builder b;

    memset(db, 0, sizeof(*db));
    db->data = data;
    db->size = size;
    if (size < 32 || xf_be32(data) != HEADER_VERSION || xf_be32(data + 20) != FILE_FORMAT ||
        xf_be32(data + 28) != 256) {
        xf_error("hdb: unsupported database header");
        return -1;
    }
    b.db = db;
    b.cap = 0;
    if (xf_be32(data + 8) && walk_classes(&b, xf_be32(data + 8), 0) != 0)
        return -1;
    qsort(db->entries, db->count, sizeof(*db->entries), cmp_entry);
    return 0;
}

int xf_neo_open(xf_neo_db *db, const char *path)
{
    size_t size;
    u8 *data = xf_load(path, &size);
    if (!data) {
        memset(db, 0, sizeof(*db));
        return -1;
    }
    if (xf_neo_init(db, data, size) != 0) {
        xf_neo_close(db);
        return -1;
    }
    return 0;
}

void xf_neo_close(xf_neo_db *db)
{
    free(db->data);
    free(db->entries);
    memset(db, 0, sizeof(*db));
}

const xf_neo_entry *xf_neo_find(const xf_neo_db *db, u32 id)
{
    u32 lo = 0, hi = db->count;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (db->entries[mid].id == id)
            return &db->entries[mid];
        if (db->entries[mid].id < id)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

/* Header fields: 0x4136A0 neo_persist_read_header */
int xf_neo_object(const xf_neo_db *db, u32 id, xf_neo_reader *r, u16 *flags, u32 *version)
{
    const xf_neo_entry *e = xf_neo_find(db, id);
    if (!e || !e->offset || !has(db, e->offset, 6))
        return -1;
    if (flags)
        *flags = xf_be16(db->data + e->offset);
    if (version)
        *version = xf_be32(db->data + e->offset + 2);
    r->p = db->data + e->offset + 6;
    r->end = db->data + db->size;
    r->error = 0;
    return 0;
}

static int need(xf_neo_reader *r, size_t n)
{
    if (r->error || (size_t)(r->end - r->p) < n) {
        r->error = 1;
        return -1;
    }
    return 0;
}

u8 xf_neo_u8(xf_neo_reader *r) { return need(r, 1) ? 0 : *r->p++; }

u16 xf_neo_u16(xf_neo_reader *r)
{
    u16 v;
    if (need(r, 2))
        return 0;
    v = xf_be16(r->p);
    r->p += 2;
    return v;
}

u32 xf_neo_u32(xf_neo_reader *r)
{
    u32 v;
    if (need(r, 4))
        return 0;
    v = xf_be32(r->p);
    r->p += 4;
    return v;
}

xf_neo_reader xf_neo_blob(const xf_neo_db *db, xf_neo_reader *r)
{
    xf_neo_reader b = { NULL, NULL, 0 };
    u32 offset = xf_neo_u32(r), len = xf_neo_u32(r);
    if (r->error || !offset || !len)
        return b;
    if (!has(db, offset, len)) {
        b.error = 1;
        return b;
    }
    b.p = db->data + offset;
    b.end = b.p + len;
    return b;
}

/* 0x4137E0 neo_list_read_header */
xf_neo_list xf_neo_read_list(xf_neo_reader *r)
{
    xf_neo_list l;
    l.count = xf_neo_u32(r);
    l.root = xf_neo_u32(r);
    xf_neo_u32(r); /* element type, 11 */
    xf_neo_u32(r); /* tag, 'null' */
    xf_neo_u32(r); /* 1 */
    xf_neo_u8(r);  /* 0xFF */
    xf_neo_u8(r);
    xf_neo_u32(r); /* tagged 'ID  ', present from file format 0x501 */
    return l;
}

static int walk_list(const xf_neo_db *db, u32 node, void (*fn)(void *, u32), void *user, int depth)
{
    const u8 *d = db->data;
    u32 count, i;
    int total = 0;

    if (depth > MAX_DEPTH || node < 32 || !has(db, node, NODE_HEADER))
        return -1;
    count = xf_be16(d + node + 6);
    if (count > 255 || !has(db, node + NODE_HEADER, (u64)count * 4))
        return -1;
    for (i = 0; i < count; i++) {
        u32 v = xf_be32(d + node + NODE_HEADER + i * 4);
        if (d[node] & 0x80) {
            fn(user, v);
            total++;
        } else {
            int n = walk_list(db, v, fn, user, depth + 1);
            if (n < 0)
                return -1;
            total += n;
        }
    }
    return total;
}

int xf_neo_list_ids(const xf_neo_db *db, const xf_neo_list *list, void (*fn)(void *user, u32 id),
                    void *user)
{
    if (!list->root)
        return 0;
    return walk_list(db, list->root, fn, user, 0);
}
