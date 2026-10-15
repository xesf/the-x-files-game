/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf and contributors */
/* Reader for the NeoAccess object database used by XFILES.HDB and the
 * saved games (see docs/formats/hdb.md). */
#ifndef XF_NEO_H
#define XF_NEO_H

#include "xf_types.h"

typedef struct {
    u32 id;
    u32 offset;   /* file offset of the object record */
    u8 class_id;
} xf_neo_entry;

typedef struct {
    u8 *data;
    size_t size;
    u32 count;
    xf_neo_entry *entries; /* sorted by id */
} xf_neo_db;

/* A cursor over one object record or one blob. */
typedef struct {
    const u8 *p, *end;
    int error;     /* set when a read runs past the end */
} xf_neo_reader;

int xf_neo_open(xf_neo_db *db, const char *path);
/* Takes ownership of `data` (from malloc). */
int xf_neo_init(xf_neo_db *db, u8 *data, size_t size);
void xf_neo_close(xf_neo_db *db);
const xf_neo_entry *xf_neo_find(const xf_neo_db *db, u32 id);
/* Opens the record of object `id` after its 6-byte header. Returns 0 on
 * success. The header holds 2 flag bytes and the object version. */
int xf_neo_object(const xf_neo_db *db, u32 id, xf_neo_reader *r, u16 *flags, u32 *version);

u8 xf_neo_u8(xf_neo_reader *r);
u16 xf_neo_u16(xf_neo_reader *r);
u32 xf_neo_u32(xf_neo_reader *r);
/* Reads a blob reference (file offset, length) and returns a reader over
 * the blob, which is empty when the offset is 0. */
xf_neo_reader xf_neo_blob(const xf_neo_db *db, xf_neo_reader *r);

/* An embedded list of object IDs. */
typedef struct {
    u32 count;
    u32 root;      /* file offset of the first list node, 0 if empty */
} xf_neo_list;

/* Reads an embedded list header from an object record. */
xf_neo_list xf_neo_read_list(xf_neo_reader *r);
/* Calls fn for every ID in the list, in order. Returns the number of IDs
 * visited, or -1 on a damaged list. */
int xf_neo_list_ids(const xf_neo_db *db, const xf_neo_list *list,
                    void (*fn)(void *user, u32 id), void *user);

#endif
