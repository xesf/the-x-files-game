/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
#include "vexe.h"

#include <stdlib.h>
#include <string.h>

#include "xf_endian.h"
#include "xf_file.h"

/* NE header fields (from the "NE" signature): segment count at 0x1C, the
 * segment table's offset at 0x22, the alignment shift at 0x32. A segment
 * entry is the file offset (shifted), the length (0: 64 KB), flags (0x100:
 * relocations follow the data) and the memory size. A relocation is 8
 * bytes: the source type (2 = a segment word), flags (low two bits 0 =
 * internal reference, bit 2 additive), the offset, then for an internal
 * reference the segment number. Non-additive fixups chain: the word at
 * the offset holds the next offset, 0xFFFF ending the chain. */
static int add_fix(vexe_seg *s, u32 *cap, u16 off, u8 seg)
{
    if (s->fix_count == *cap) {
        u32 n = *cap ? *cap * 2 : 64;
        u16 *o = realloc(s->fix_off, n * sizeof(*o));
        u8 *g;
        if (!o)
            return -1;
        s->fix_off = o;
        if (!(g = realloc(s->fix_seg, n)))
            return -1;
        s->fix_seg = g;
        *cap = n;
    }
    s->fix_off[s->fix_count] = off;
    s->fix_seg[s->fix_count++] = seg;
    return 0;
}

static void sort_fixes(vexe_seg *s)
{
    u32 i, j;
    for (i = 1; i < s->fix_count; i++) { /* insertion sort: mostly in order */
        u16 o = s->fix_off[i];
        u8 g = s->fix_seg[i];
        for (j = i; j > 0 && s->fix_off[j - 1] > o; j--) {
            s->fix_off[j] = s->fix_off[j - 1];
            s->fix_seg[j] = s->fix_seg[j - 1];
        }
        s->fix_off[j] = o;
        s->fix_seg[j] = g;
    }
}

int vexe_load(vexe *x, const char *path)
{
    const u8 *d, *ne;
    u32 ne_off, segtab, align, i;

    memset(x, 0, sizeof(*x));
    if (!(x->file = xf_load(path, &x->file_size)) || x->file_size < 0x40)
        goto fail;
    d = x->file;
    ne_off = xf_le32(d + 0x3C);
    if (ne_off + 0x40 > x->file_size || d[ne_off] != 'N' || d[ne_off + 1] != 'E')
        goto fail;
    ne = d + ne_off;
    x->seg_count = xf_le16(ne + 0x1C);
    segtab = ne_off + xf_le16(ne + 0x22);
    align = xf_le16(ne + 0x32);
    if (x->seg_count > VX_MAX_SEGS || segtab + 8u * x->seg_count > x->file_size)
        goto fail;
    for (i = 1; i <= (u32)x->seg_count; i++) {
        const u8 *e = d + segtab + (i - 1) * 8;
        vexe_seg *s = &x->segs[i];
        u32 off = (u32)xf_le16(e) << align, len = xf_le16(e + 2), flags = xf_le16(e + 4), cap = 0;
        if (!off)
            continue;
        if (!len)
            len = 0x10000;
        if (off + len > x->file_size)
            goto fail;
        s->data = d + off;
        s->size = len;
        if (flags & 0x100 && off + len + 2 <= x->file_size) {
            u32 n = xf_le16(d + off + len), k;
            const u8 *r = d + off + len + 2;
            if (off + len + 2 + n * 8 > x->file_size)
                goto fail;
            for (k = 0; k < n; k++, r += 8) {
                u16 at = xf_le16(r + 2), seg = xf_le16(r + 4);
                int guard = 0;
                if (r[0] != 2 || (r[1] & 3) != 0 || (seg & 0xFF) == 0xFF)
                    continue; /* only the internal segment fixups name segments */
                for (;;) {
                    u16 next;
                    if ((u32)at + 2 > len || add_fix(s, &cap, at, (u8)seg) != 0 || ++guard > 4096)
                        break;
                    if (r[1] & 4)
                        break; /* additive: no chain */
                    next = xf_le16(s->data + at);
                    if (next == 0xFFFF || next == at)
                        break;
                    at = next;
                }
            }
            sort_fixes(s);
        }
    }
    return 0;
fail:
    vexe_free(x);
    return -1;
}

void vexe_free(vexe *x)
{
    int i;
    for (i = 0; i <= VX_MAX_SEGS; i++) {
        free(x->segs[i].fix_off);
        free(x->segs[i].fix_seg);
    }
    free(x->file);
    memset(x, 0, sizeof(*x));
}

const u8 *vexe_at(const vexe *x, vx_ptr p, u32 *avail)
{
    int s = VX_SEG(p);
    if (s < 1 || s > x->seg_count || !x->segs[s].data || VX_OFF(p) >= x->segs[s].size) {
        if (avail)
            *avail = 0;
        return NULL;
    }
    if (avail)
        *avail = x->segs[s].size - VX_OFF(p);
    return x->segs[s].data + VX_OFF(p);
}

u8 vexe_u8(const vexe *x, vx_ptr p)
{
    const u8 *b = vexe_at(x, p, NULL);
    return b ? *b : 0;
}

u16 vexe_u16(const vexe *x, vx_ptr p)
{
    u32 n;
    const u8 *b = vexe_at(x, p, &n);
    return b && n >= 2 ? xf_le16(b) : 0;
}

vx_ptr vexe_far(const vexe *x, vx_ptr p)
{
    const vexe_seg *s = VX_SEG(p) >= 1 && VX_SEG(p) <= x->seg_count ? &x->segs[VX_SEG(p)] : NULL;
    u16 want = (u16)(VX_OFF(p) + 2);
    u32 lo = 0, hi = s ? s->fix_count : 0;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (s->fix_off[mid] < want)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (s && lo < s->fix_count && s->fix_off[lo] == want)
        return VX_PTR(s->fix_seg[lo], vexe_u16(x, p));
    return 0;
}
