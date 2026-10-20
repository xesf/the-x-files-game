/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* VORTEX.EXE as data: its segments and their segment fixups, through
 * which the sequences' far pointers name other segments
 * (docs/vortex/README.md). */
#ifndef XF_VEXE_H
#define XF_VEXE_H

#include "xf_types.h"

#define VX_MAX_SEGS 64

typedef struct {
    u8 *data;         /* the segment as loaded: its file bytes, then zeros up to its memory size */
    u32 size;
    u16 *fix_off;     /* offsets of the segment fixups, sorted */
    u8 *fix_seg;      /* the segment each names */
    u32 fix_count;
} vexe_seg;

typedef struct {
    u16 block;        /* string resources: block n holds ids 16(n-1) to 16n-1 */
    u32 off, len;     /* in the file */
} vexe_strblock;

typedef struct {
    u8 *file;
    vexe_strblock strings[64];
    int string_blocks;
    size_t file_size;
    int seg_count;
    vexe_seg segs[VX_MAX_SEGS + 1]; /* 1-based */
} vexe;

/* A far pointer into the program: the segment in the high word. */
typedef u32 vx_ptr;
#define VX_PTR(seg, off) ((vx_ptr)(seg) << 16 | (u16)(off))
#define VX_SEG(p) ((int)((p) >> 16))
#define VX_OFF(p) ((u16)(p))

int vexe_load(vexe *x, const char *path);
/* A string resource (RT_STRING), as LoadString reads it; "" when absent. */
const char *vexe_string(const vexe *x, u16 id, char *out, size_t size);
void vexe_free(vexe *x);
/* The bytes at a pointer, or NULL outside the program; `avail` gets how
 * many follow in the segment. */
const u8 *vexe_at(const vexe *x, vx_ptr p, u32 *avail);
u8 vexe_u8(const vexe *x, vx_ptr p);
u16 vexe_u16(const vexe *x, vx_ptr p);
/* The far pointer stored at p: its offset word, and the segment its
 * fixup names (0 without a fixup). */
vx_ptr vexe_far(const vexe *x, vx_ptr p);

#endif
