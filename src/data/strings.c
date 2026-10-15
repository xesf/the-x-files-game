/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf and contributors */
#include "data/strings.h"

#include <stdlib.h>
#include <string.h>

#include "xf_file.h"
#include "xf_log.h"

typedef struct {
    const u8 *d;
    size_t size;
    u32 sections;
    const u8 *sec; /* section table */
    u32 res_base;  /* file offset of the resource directory */
} pe_file;

static u32 rd32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u32 rd16(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8; }

/* File offset of a relative virtual address, or 0. */
static u32 rva_offset(const pe_file *pe, u32 rva)
{
    u32 i;
    for (i = 0; i < pe->sections; i++) {
        const u8 *s = pe->sec + i * 40;
        u32 vsize = rd32(s + 8), va = rd32(s + 12), raw_size = rd32(s + 16), raw = rd32(s + 20);
        if (rva >= va && rva < va + XF_MAX(vsize, raw_size))
            return rva - va + raw;
    }
    return 0;
}

/* Calls back for each entry of a resource directory at `dir` (relative
 * to the resource base). */
static int dir_entries(const pe_file *pe, u32 dir, u32 *ids, u32 *offs, int max)
{
    const u8 *p = pe->d + pe->res_base + dir;
    int n, i;
    if (pe->res_base + dir + 16 > pe->size)
        return 0;
    n = (int)(rd16(p + 12) + rd16(p + 14));
    for (i = 0; i < n && i < max && pe->res_base + dir + 16 + 8 * i + 8 <= pe->size; i++) {
        ids[i] = rd32(p + 16 + 8 * i);
        offs[i] = rd32(p + 20 + 8 * i) & 0x7FFFFFFF;
    }
    return i;
}

/* UTF-16 to UTF-8. */
static char *utf8_from_utf16(const u8 *s, u32 n)
{
    char *out = malloc(n * 3 + 1), *o = out;
    u32 i;
    if (!out)
        return NULL;
    for (i = 0; i < n; i++) {
        u32 c = rd16(s + 2 * i);
        if (c < 0x80) {
            *o++ = (char)c;
        } else if (c < 0x800) {
            *o++ = (char)(0xC0 | c >> 6);
            *o++ = (char)(0x80 | (c & 0x3F));
        } else {
            *o++ = (char)(0xE0 | c >> 12);
            *o++ = (char)(0x80 | (c >> 6 & 0x3F));
            *o++ = (char)(0x80 | (c & 0x3F));
        }
    }
    *o = '\0';
    return out;
}

static int add(xf_strings *t, u32 id, char *text, u32 *cap)
{
    if (t->count == *cap) {
        u32 n = *cap ? *cap * 2 : 256;
        u32 *ids = realloc(t->ids, n * sizeof(*ids));
        char **texts = ids ? realloc(t->texts, n * sizeof(*texts)) : NULL;
        if (ids)
            t->ids = ids;
        if (!texts) {
            free(text);
            return -1;
        }
        t->texts = texts;
        *cap = n;
    }
    t->ids[t->count] = id;
    t->texts[t->count++] = text;
    return 0;
}

static void sort_strings(xf_strings *t)
{
    u32 i, j;
    for (i = 1; i < t->count; i++) { /* insertion sort: blocks come almost in order */
        u32 id = t->ids[i];
        char *text = t->texts[i];
        for (j = i; j > 0 && t->ids[j - 1] > id; j--) {
            t->ids[j] = t->ids[j - 1];
            t->texts[j] = t->texts[j - 1];
        }
        t->ids[j] = id;
        t->texts[j] = text;
    }
}

int xf_strings_load(xf_strings *t, const char *path)
{
    size_t size;
    u8 *d = xf_load(path, &size);
    pe_file pe;
    u32 pe_off, opt, magic, res_rva, cap = 0;
    u32 types[64], type_offs[64], blocks[512], block_offs[512], langs[16], lang_offs[16];
    int nt, nb, nl, i, j;

    memset(t, 0, sizeof(*t));
    if (!d)
        return -1;
    memset(&pe, 0, sizeof(pe));
    pe.d = d;
    pe.size = size;
    if (size < 0x40 || d[0] != 'M' || d[1] != 'Z' || (pe_off = rd32(d + 0x3C)) + 0x78 > size ||
        memcmp(d + pe_off, "PE\0\0", 4) != 0) {
        free(d);
        return -1;
    }
    pe.sections = rd16(d + pe_off + 6);
    opt = rd16(d + pe_off + 20);
    magic = rd16(d + pe_off + 24);
    pe.sec = d + pe_off + 24 + opt;
    res_rva = rd32(d + pe_off + 24 + (magic == 0x20B ? 112 : 96) + 2 * 8);
    pe.res_base = rva_offset(&pe, res_rva);
    if (!res_rva || !pe.res_base) {
        free(d);
        return -1;
    }
    /* Resource tree: type 6 (RT_STRING) / block of 16 strings / language. */
    nt = dir_entries(&pe, 0, types, type_offs, 64);
    for (i = 0; i < nt; i++) {
        if (types[i] != 6)
            continue;
        nb = dir_entries(&pe, type_offs[i], blocks, block_offs, 512);
        for (j = 0; j < nb; j++) {
            nl = dir_entries(&pe, block_offs[j], langs, lang_offs, 16);
            if (nl > 0 && pe.res_base + lang_offs[0] + 8 <= size) {
                const u8 *entry = d + pe.res_base + lang_offs[0];
                u32 at = rva_offset(&pe, rd32(entry)), end = at + rd32(entry + 4), k;
                for (k = 0; k < 16 && at + 2 <= end && end <= size; k++) {
                    u32 n = rd16(d + at);
                    at += 2;
                    if (n && at + 2 * n <= end) {
                        char *text = utf8_from_utf16(d + at, n);
                        if (text && add(t, (blocks[j] - 1) * 16 + k, text, &cap) != 0)
                            break;
                    }
                    at += 2 * n;
                }
            }
        }
    }
    free(d);
    sort_strings(t);
    xf_info("strings: %u in %s", t->count, path);
    return 0;
}

void xf_strings_free(xf_strings *t)
{
    u32 i;
    for (i = 0; i < t->count; i++)
        free(t->texts[i]);
    free(t->ids);
    free(t->texts);
    memset(t, 0, sizeof(*t));
}

const char *xf_strings_get(const xf_strings *t, u32 id)
{
    u32 lo = 0, hi = t->count;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (t->ids[mid] == id)
            return t->texts[mid];
        if (t->ids[mid] < id)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}
