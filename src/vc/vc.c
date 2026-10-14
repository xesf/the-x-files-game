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
    u32 nums[4];
    int ok = 1, n = 0;

    a->entry_count = 0;
    if (path_token(&p, end, a->logical, sizeof(a->logical)) != 0)
        return -1;
    /* Numbers up to the first path: [start end] then a 0, then the disc of
     * the first entry. A path token follows the disc number. */
    while (ok && n < 4 && p < end) {
        const u8 *save = p;
        u32 v = num_token(&p, end, &ok);
        /* A token is the disc number when the next token is a path length
         * followed by a path. */
        nums[n++] = v;
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
    memset(a, 0, sizeof(*a));
    if (open_obj(db, id, VC_ASSET_REF, &r) != 0)
        return -1;
    blob = xf_neo_blob(db, &r);
    a->name = xf_neo_u32(&r);
    a->flags = xf_neo_u32(&r);
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
    collect c = { ids, max, 0 };
    if (!id || xf_neo_object(db, id, &r, NULL, NULL) != 0)
        return -1;
    l = xf_neo_read_list(&r);
    if (r.error || xf_neo_list_ids(db, &l, collect_id, &c) < 0)
        return -1;
    return XF_MIN(c.n, max);
}
