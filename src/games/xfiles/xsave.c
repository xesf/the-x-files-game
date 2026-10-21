/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* The original saved games (.x, see docs/formats/save.md): a copy of the
 * GAM database with the variables, enable states and strings as they
 * were, plus a header object (class 0x57) and lists of the state that the
 * engine keeps outside the GAM. 0x4D14B0 writes them. */
#include "games/xfiles/game.h"

#include <stdlib.h>
#include <string.h>

#include "core/xf_endian.h"
#include "core/xf_file.h"
#include "core/xf_log.h"
#include "media/mov.h"
#include "media/pff.h"
#include "media/pict.h"
#include "media/video.h"

#define X_VARIABLE 0x53
#define X_ENABLED 0x46
#define X_STRING 0x50
#define X_ASKED 0x56     /* VCConversationHistory */
#define X_HEADER 0x57    /* VCGameState */
#define X_NOTES 0x58     /* VCPDANotes */
#define X_PHOTO 0x59     /* VCPhoto */
#define X_MAIL 0x5A      /* VCEmail: mail received */
#define X_MAIL_READ 0x5B /* VCEmailRead */
#define X_PENDING 0x5C   /* VCEmailPending: replies on the way */

static u32 le32(const u8 *p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

/* The lists of the header object are blobs copied from memory, so their
 * numbers are little-endian. Returns the number of IDs read. */
static int blob_ids(const xf_neo_reader *b, u32 *ids, int max)
{
    int n = 0;
    const u8 *p;
    if (!b->p || b->error)
        return 0;
    for (p = b->p; b->end - p >= 4 && n < max; p += 4)
        ids[n++] = le32(p);
    return n;
}

/* Some lists hold records: an ID, then other fields. The saves of the
 * 1998 beta builds pad each record to `wide` bytes; the shipped engine
 * packs it in `packed`. A blob is padded to 4 bytes. */
static int blob_stride(const game *g, const xf_neo_reader *b, int wide, int packed)
{
    size_t len = b->p ? (size_t)(b->end - b->p) : 0;
    const xf_neo_entry *e;
    if (len % (size_t)wide != 0)
        return packed;
    if (len < (size_t)wide * 2 || len % (size_t)packed != 0)
        return wide;
    /* Both fit: the second record of the wide layout starts with an ID. */
    e = xf_neo_find(&g->db, le32(b->p + wide));
    return e ? wide : packed;
}

static void asked_add(game *g, u32 conversation)
{
    int i;
    for (i = 0; i < g->asked_count; i++)
        if (g->asked[i].conversation == conversation) {
            g->asked[i].count++;
            return;
        }
    if (g->asked_count < GAME_MAX_ASKED) {
        g->asked[g->asked_count].conversation = conversation;
        g->asked[g->asked_count++].count = 1;
    }
}

static void apply_object(game *g, const xf_neo_db *x, const xf_neo_entry *e)
{
    xf_neo_reader r, b;
    u32 ids[GAME_MAX_ASKED];
    int n, i, k;

    if (xf_neo_object(x, e->id, &r, NULL, NULL) != 0)
        return;
    switch (e->class_id) {
    case X_VARIABLE: /* name, value, owner, owner class, type */
        xf_neo_blob(x, &r);
        vc_set_var(&g->interp, e->id, (i32)xf_neo_u32(&r));
        break;
    case X_ENABLED: { /* target, state */
        u32 j;
        xf_neo_u32(&r);
        k = xf_neo_u8(&r);
        for (j = 0; j < g->interp.enable_count; j++)
            if (g->interp.enables[j].id == e->id) {
                g->interp.enables[j].enabled = (u8)(k != 0);
                break;
            }
        break;
    }
    case X_STRING: {
        char text[sizeof(g->interp.strings[0].text)];
        size_t len;
        b = xf_neo_blob(x, &r);
        len = b.p ? XF_MIN((size_t)(b.end - b.p), sizeof(text) - 1) : 0;
        if (len)
            memcpy(text, b.p, len);
        text[len] = '\0';
        vc_string_set(&g->interp, e->id, text);
        break;
    }
    case X_ASKED: /* a conversation appears once each time it is asked */
        b = xf_neo_blob(x, &r);
        n = blob_ids(&b, ids, GAME_MAX_ASKED);
        for (i = 0; i < n; i++)
            asked_add(g, ids[i]);
        break;
    case X_NOTES:
        b = xf_neo_blob(x, &r);
        n = blob_ids(&b, ids, GAME_MAX_NOTES);
        for (i = 0; i < n; i++)
            if (g->pda_note_count < GAME_MAX_NOTES)
                g->pda_notes[g->pda_note_count++] = ids[i];
        break;
    case X_MAIL:
        b = xf_neo_blob(x, &r);
        n = blob_ids(&b, ids, GAME_MAX_EMAILS);
        for (i = 0; i < n && g->email_count < GAME_MAX_EMAILS; i++) {
            g->emails[g->email_count].asset = ids[i];
            g->emails[g->email_count++].read = 0;
        }
        break;
    case X_MAIL_READ:
        b = xf_neo_blob(x, &r);
        n = blob_ids(&b, ids, GAME_MAX_EMAILS);
        for (i = 0; i < n; i++)
            for (k = 0; k < g->email_count; k++)
                if (g->emails[k].asset == ids[i])
                    g->emails[k].read = 1;
        break;
    case X_PENDING:
        b = xf_neo_blob(x, &r);
        g->pending_mail_count = blob_ids(&b, g->pending_mail, (int)XF_COUNT(g->pending_mail));
        break;
    case X_PHOTO: /* picture, media, ..., disc, downloaded */
        b = xf_neo_blob(x, &r);
        if (g->photo_count < GAME_MAX_PHOTOS) {
            u32 media = xf_neo_u32(&r);
            xf_neo_u32(&r);
            xf_neo_u16(&r);
            xf_neo_u32(&r);
            xf_neo_u16(&r);
            xf_neo_u8(&r);
            if (xf_neo_u8(&r))
                g->photo_downloaded |= 1u << g->photo_count;
            g->photos[g->photo_count++] = media;
        }
        break;
    }
}

/* The header (0x5EF6D0 reads it): node, location, viewpoint, view, the
 * IDs of the other save objects, the disc, the Artificial Intuition list,
 * then five lists: sounds playing, inventory, evidence, kit icons and
 * running timers. */
static int read_header(game *g, const xf_neo_db *x, u32 id, u32 *view)
{
    xf_neo_reader r, lists[5];
    u32 place[9], ids[GAME_MAX_EVIDENCE];
    int i, n, stride;

    if (xf_neo_object(x, id, &r, NULL, NULL) != 0)
        return -1;
    for (i = 0; i < 9; i++)
        place[i] = xf_neo_u32(&r);
    g->disc = (int)xf_neo_u32(&r);
    xf_neo_u32(&r); /* how the view was found: 0 from the screen, 1 from a goto, 2 from the GAM */
    g->ai_list = xf_neo_u32(&r);
    g->ai_trigger = xf_neo_u32(&r);
    xf_neo_u32(&r);
    xf_neo_u32(&r);
    for (i = 0; i < 5; i++)
        lists[i] = xf_neo_blob(x, &r);
    if (r.error)
        return -1;
    *view = place[3];

    /* Sounds: our engine plays one beside the clips. */
    stride = blob_stride(g, &lists[0], 12, 9);
    for (i = 0; lists[0].p && lists[0].end - lists[0].p >= (long)(i + 1) * stride; i++) {
        const u8 *p = lists[0].p + i * stride;
        /* (asset, looped, time) padded; (asset, time, looped) packed */
        g->sound = le32(p);
        g->sound_looped = stride == 9 ? p[8] != 0 : le32(p + 4) != 0;
    }
    if (g->sound)
        g->sound_serial++;
    /* Evidence: (icon, shown) for the evidence bar, then the kit's icons. */
    stride = blob_stride(g, &lists[2], 8, 5);
    for (i = 0; lists[2].p && lists[2].end - lists[2].p >= (long)(i + 1) * stride; i++)
        if (g->evidence_count < GAME_MAX_EVIDENCE)
            g->evidence[g->evidence_count++] = le32(lists[2].p + i * stride);
    /* Timers: (timer, time, periodic). */
    stride = blob_stride(g, &lists[4], 12, 9);
    for (i = 0; lists[4].p && lists[4].end - lists[4].p >= (long)(i + 1) * stride; i++) {
        const u8 *p = lists[4].p + i * stride;
        game_timer *t;
        if (g->timer_count >= GAME_MAX_TIMERS)
            break;
        t = &g->timers[g->timer_count++];
        t->id = (u8)le32(p);
        t->due = g->now + le32(p + 4);
        t->period = (stride == 9 ? p[8] : le32(p + 8)) ? le32(p + 4) : 0;
    }
    n = blob_ids(&lists[3], ids, GAME_MAX_EVIDENCE);
    for (i = 0; i < n && g->kit_count < GAME_MAX_EVIDENCE; i++)
        g->kit_icons[g->kit_count++] = ids[i];
    return 0;
}

/* A file moved as text gains a CR before each LF: undo it when every LF
 * has one. */
static size_t undo_crlf(u8 *data, size_t size)
{
    size_t i, k = 0, lf = 0, crlf = 0;
    for (i = 0; i < size; i++)
        if (data[i] == '\n') {
            lf++;
            crlf += i && data[i - 1] == '\r';
        }
    if (!lf || lf != crlf)
        return size;
    for (i = 0; i < size; i++)
        if (!(data[i] == '\r' && i + 1 < size && data[i + 1] == '\n'))
            data[k++] = data[i];
    return k;
}

int game_is_x_save(const u8 *data, size_t size)
{
    return size >= 32 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 5;
}

int game_load_x_mem(game *g, const u8 *data, size_t size)
{
    xf_neo_db x;
    u8 *copy = malloc(size ? size : 1);
    u32 i, view = 0, header = 0;

    if (!copy)
        return -1;
    memcpy(copy, data, size);
    if (xf_neo_init(&x, copy, size) != 0) {
        size_t fixed = undo_crlf(copy, size);
        xf_neo_close(&x);
        if (fixed == size || !(copy = malloc(size))) {
            xf_warn("save: not a saved game");
            return -1;
        }
        memcpy(copy, data, size);
        fixed = undo_crlf(copy, size);
        if (xf_neo_init(&x, copy, fixed) != 0) {
            xf_neo_close(&x);
            xf_warn("save: not a saved game");
            return -1;
        }
        xf_warn("save: the file was moved as text; repaired");
    }
    for (i = 0; i < x.count && !header; i++)
        if (x.entries[i].class_id == X_HEADER)
            header = x.entries[i].id;
    /* The engine registrations come from the title's triggers, which
     * the original runs once at start-up. */
    if (!header || game_reset(g) != 0 || !game_run_title(g)) {
        xf_warn("save: no game state in the file");
        xf_neo_close(&x);
        return -1;
    }
    /* The save holds the whole of these lists, mail the title sends too. */
    g->email_count = g->pda_note_count = g->photo_count = g->asked_count = 0;
    g->pending_mail_count = g->kit_count = g->evidence_count = 0;
    g->photo_downloaded = 0;
    g->timer_count = 0;
    for (i = 0; i < x.count; i++)
        apply_object(g, &x, &x.entries[i]);
    if (read_header(g, &x, header, &view) != 0)
        view = 0;
    xf_neo_close(&x);
    game_prefs_apply(g);
    /* 0x486140 goes to the node, location, viewpoint and view in turn,
     * so their entering triggers run (and register what they register). */
    g->current.asset = 0;
    g->current.conversation = 0;
    g->queue_head = g->queue_tail;
    if (!view || game_enter_view(g, view) != 0) {
        xf_warn("save: no view to go to");
        return -1;
    }
    xf_info("save: loaded an original save, view %u, disc %d", view, g->disc);
    return 0;
}

/* --- Writing ------------------------------------------------------------ */

#define X_NODE_SIZE 256
#define X_THUMB_W 80
#define X_THUMB_H 60

typedef struct {
    u8 *data;
    size_t size, cap;
    int error;
} xbuf;

static void x_reserve(xbuf *b, size_t n)
{
    size_t cap;
    u8 *d;
    if (b->error || b->size + n <= b->cap)
        return;
    for (cap = b->cap ? b->cap : 65536; cap < b->size + n; cap *= 2)
        ;
    if (!(d = realloc(b->data, cap))) {
        b->error = 1;
        return;
    }
    memset(d + b->cap, 0, cap - b->cap);
    b->data = d;
    b->cap = cap;
}

static void x_bytes(xbuf *b, const void *p, size_t n)
{
    x_reserve(b, n);
    if (b->error)
        return;
    memcpy(b->data + b->size, p, n);
    b->size += n;
}

static void x_u8(xbuf *b, u32 v)
{
    u8 c = (u8)v;
    x_bytes(b, &c, 1);
}

static void x_u16(xbuf *b, u32 v)
{
    x_u8(b, v >> 8);
    x_u8(b, v);
}

static void x_u32(xbuf *b, u32 v)
{
    x_u16(b, v >> 16);
    x_u16(b, v);
}

static void x_le32(xbuf *b, u32 v)
{
    u8 c[4];
    c[0] = (u8)v;
    c[1] = (u8)(v >> 8);
    c[2] = (u8)(v >> 16);
    c[3] = (u8)(v >> 24);
    x_bytes(b, c, 4);
}

static void x_zero(xbuf *b, size_t n)
{
    x_reserve(b, n);
    if (!b->error)
        b->size += n; /* the reserve is zeroed */
}

/* Records, nodes and blobs start on 8-byte boundaries. */
static u32 x_align(xbuf *b)
{
    x_zero(b, (8 - b->size % 8) % 8);
    return (u32)b->size;
}

static void x_put32(xbuf *b, size_t at, u32 v)
{
    if (b->error || at + 4 > b->size)
        return;
    b->data[at] = (u8)(v >> 24);
    b->data[at + 1] = (u8)(v >> 16);
    b->data[at + 2] = (u8)(v >> 8);
    b->data[at + 3] = (u8)v;
}

/* An object record: flags, version 1, then the class fields. */
static u32 x_record(xbuf *b)
{
    u32 at = x_align(b);
    x_u16(b, 0xC000);
    x_u32(b, 1);
    return at;
}

/* A blob field whose data follows the record: returns where to patch it. */
static size_t x_blob_field(xbuf *b)
{
    size_t at = b->size;
    x_u32(b, 0);
    x_u32(b, 0);
    return at;
}

static void x_blob_data(xbuf *b, size_t field, const xbuf *data)
{
    u32 at;
    if (!data->size)
        return;
    at = x_align(b);
    x_bytes(b, data->data, data->size);
    x_put32(b, field, at);
    x_put32(b, field + 4, (u32)data->size);
}

/* An object whose only field is a blob of little-endian IDs. */
static u32 x_id_blob(xbuf *b, const u32 *ids, int n)
{
    xbuf l = { NULL, 0, 0, 0 };
    u32 at = x_record(b);
    size_t field = x_blob_field(b);
    int i;
    for (i = 0; i < n; i++)
        x_le32(&l, ids[i]);
    x_blob_data(b, field, &l);
    b->error |= l.error;
    free(l.data);
    return at;
}

/* The camera's picture of a photo: the media's still, scaled to 80 x 60,
 * or grey when the media is not at hand. */
static void photo_pixels(game *g, u32 media, u32 *out)
{
    vc_asset_ref a;
    xf_image img;
    int j, x, y;
    memset(&img, 0, sizeof(img));
    if (vc_read_asset_ref(&g->db, media, &a) == 0)
        for (j = 0; j < a.entry_count && !img.pixels; j++) {
            const vc_media_entry *m = &a.entries[j];
            const char *ext = strrchr(m->path, '.');
            char path[1024];
            if (game_resolve(g, m->path, path, sizeof(path)) != 0)
                continue;
            if (ext && strcmp(ext, ".pff") == 0) {
                xf_pff pff;
                u32 size;
                u8 *data;
                if (xf_pff_open(&pff, path) != 0)
                    continue;
                data = xf_pff_read(&pff, m->start, &size);
                if (data && xf_pict_decode(data, size, &img) != 0)
                    memset(&img, 0, sizeof(img));
                free(data);
                xf_pff_close(&pff);
            } else {
                xf_mov mov;
                const xf_mov_track *t;
                if (xf_mov_open(&mov, path) != 0)
                    continue;
                if ((t = xf_mov_find(&mov, XF_TRACK_VIDEO)) && t->width && t->height &&
                    (img.pixels = calloc((size_t)t->width * t->height, 4))) {
                    img.width = t->width;
                    img.height = t->height;
                    if (xf_movie_frame_at(&mov, m->start, img.pixels, img.width, img.height) != 0) {
                        free(img.pixels);
                        memset(&img, 0, sizeof(img));
                    }
                }
                xf_mov_close(&mov);
            }
        }
    for (y = 0; y < X_THUMB_H; y++)
        for (x = 0; x < X_THUMB_W; x++)
            out[y * X_THUMB_W + x] =
                img.pixels ? img.pixels[(y * img.height / X_THUMB_H) * img.width + x * img.width / X_THUMB_W]
                           : 0x808080;
    free(img.pixels);
}

/* A version 2 PICT holding the pixels unpacked (DirectBitsRect, 32 bits,
 * pack type 1). The original keeps JPEG pictures; QuickDraw draws either. */
static void photo_pict(xbuf *p, const u32 *px)
{
    int y, x;
    x_u16(p, 0); /* size: patched below */
    x_u16(p, 0), x_u16(p, 0), x_u16(p, X_THUMB_H), x_u16(p, X_THUMB_W);
    x_u16(p, 0x0011), x_u16(p, 0x02FF);
    x_u16(p, 0x0C00), x_u16(p, 0xFFFE), x_u16(p, 0);
    x_u32(p, 0x00480000), x_u32(p, 0x00480000);
    x_u16(p, 0), x_u16(p, 0), x_u16(p, X_THUMB_H), x_u16(p, X_THUMB_W);
    x_u32(p, 0);
    x_u16(p, 0x001E);
    x_u16(p, 0x0001), x_u16(p, 10), x_u16(p, 0), x_u16(p, 0), x_u16(p, X_THUMB_H), x_u16(p, X_THUMB_W);
    x_u16(p, 0x009A);
    x_u32(p, 0x000000FF); /* base address */
    x_u16(p, 0x8000 | X_THUMB_W * 4);
    x_u16(p, 0), x_u16(p, 0), x_u16(p, X_THUMB_H), x_u16(p, X_THUMB_W);
    x_u16(p, 0);                       /* version */
    x_u16(p, 1);                       /* pack type: none */
    x_u32(p, 0);                       /* pack size */
    x_u32(p, 0x00480000), x_u32(p, 0x00480000);
    x_u16(p, 16), x_u16(p, 32), x_u16(p, 3), x_u16(p, 8); /* RGB direct, 32 bits */
    x_u32(p, 0), x_u32(p, 0), x_u32(p, 0);
    x_u16(p, 0), x_u16(p, 0), x_u16(p, X_THUMB_H), x_u16(p, X_THUMB_W); /* source */
    x_u16(p, 0), x_u16(p, 0), x_u16(p, X_THUMB_H), x_u16(p, X_THUMB_W); /* destination */
    x_u16(p, 0);                                                        /* srcCopy */
    for (y = 0; y < X_THUMB_H; y++)
        for (x = 0; x < X_THUMB_W; x++)
            x_u32(p, px[y * X_THUMB_W + x] & 0xFFFFFF);
    x_u16(p, 0x00FF);
    if (!p->error)
        p->data[0] = (u8)(p->size >> 8), p->data[1] = (u8)p->size;
}

typedef struct {
    u8 class_id;
    u32 count, root;
    u32 ids[GAME_MAX_PHOTOS], offsets[GAME_MAX_PHOTOS];
} x_class;

static void x_add(x_class *c, u32 id, u32 offset)
{
    c->ids[c->count] = id;
    c->offsets[c->count++] = offset;
}

/* An ID index with one leaf node: (record offset, ID) pairs. */
static void x_id_index(xbuf *b, x_class *c)
{
    u32 i;
    c->root = x_align(b);
    x_u16(b, 0xC000);
    x_u32(b, 1);
    x_u16(b, c->count);
    for (i = 0; i < c->count; i++) {
        x_u32(b, c->offsets[i]);
        x_u32(b, c->ids[i]);
    }
    x_zero(b, X_NODE_SIZE - 8 - c->count * 8);
}

static void patch_state(game *g, xbuf *b)
{
    const xf_neo_db *gam = &g->gam;
    u32 i;
    for (i = 0; i < g->interp.var_count; i++) {
        const xf_neo_entry *e = xf_neo_find(gam, g->interp.vars[i].id);
        if (e && e->offset) /* name, then the value */
            x_put32(b, e->offset + 14, (u32)g->interp.vars[i].value);
    }
    for (i = 0; i < g->interp.enable_count; i++) {
        const xf_neo_entry *e = xf_neo_find(gam, g->interp.enables[i].id);
        if (e && e->offset) /* target, then the state */
            b->data[e->offset + 10] = g->interp.enables[i].enabled;
    }
    for (i = 0; i < g->interp.string_count; i++) {
        const vc_string *t = &g->interp.strings[i];
        const xf_neo_entry *e = xf_neo_find(gam, t->id);
        xf_neo_reader r, old;
        size_t len = strlen(t->text) + 1;
        u32 at;
        if (!e || xf_neo_object(gam, t->id, &r, NULL, NULL) != 0)
            continue;
        old = xf_neo_blob(gam, &r);
        if (old.p && (size_t)(old.end - old.p) == len && memcmp(old.p, t->text, len) == 0)
            continue;
        at = x_align(b);
        x_bytes(b, t->text, len);
        x_put32(b, e->offset + 6, at);
        x_put32(b, e->offset + 10, (u32)len);
    }
}

int game_save_x_mem(game *g, u8 **out, size_t *size)
{
    const xf_neo_db *gam = &g->gam;
    xbuf b = { NULL, 0, 0, 0 }, l = { NULL, 0, 0, 0 };
    x_class cls[7];
    u32 next, ids[GAME_MAX_ASKED * 2], root, sub_list, inv[64];
    u32 sub_ids[32];
    int n = 0, k, j, sub_n = 0, gam_classes;
    const vc_place *place = vc_place_of(&g->places, g->view);
    size_t field[5];
    u32 header_at;
    const u8 *croot;

    *out = NULL;
    *size = 0;
    if (!gam->data || gam->size < 32 || !place)
        return -1;
    croot = gam->data + xf_be32(gam->data + 8);
    gam_classes = (int)xf_be16(croot + 6);
    if (!(croot[0] & 0x80) || gam_classes > 8)
        return -1; /* the shipped GAMs have one leaf node */
    x_bytes(&b, gam->data, gam->size);
    patch_state(g, &b);
    next = xf_be32(gam->data + 16);
    memset(cls, 0, sizeof(cls));
    cls[0].class_id = X_ASKED;
    cls[1].class_id = X_HEADER;
    cls[2].class_id = X_NOTES;
    cls[3].class_id = X_PHOTO;
    cls[4].class_id = X_MAIL;
    cls[5].class_id = X_MAIL_READ;
    cls[6].class_id = X_PENDING;

    /* The header first, as 0x4D14B0 does; the lists it names follow. */
    header_at = x_record(&b);
    x_add(&cls[1], next, header_at);
    x_u32(&b, place->node);
    x_u32(&b, place->location);
    x_u32(&b, place->viewpoint);
    x_u32(&b, g->view);
    for (k = 1; k <= 5; k++)
        x_u32(&b, next + (u32)k);
    x_u32(&b, (u32)g->disc);
    x_u32(&b, 2);
    x_u32(&b, g->ai_list);
    x_u32(&b, g->ai_trigger);
    x_u32(&b, 0);
    x_u32(&b, 0xFFFFFFFF);
    for (k = 0; k < 5; k++)
        field[k] = x_blob_field(&b);
    x_u8(&b, 0), x_u8(&b, 0), x_u8(&b, 1), x_u8(&b, 0), x_u8(&b, 0), x_u8(&b, 0), x_u8(&b, 0);
    x_u32(&b, g->view_asset);
    x_u8(&b, 0);
    x_u32(&b, 0);
    x_u32(&b, 0);
    x_u32(&b, 0);
    x_u16(&b, 1);
    /* Sounds, packed as the shipped engine does: (asset, time, looped). */
    if (g->sound) {
        x_le32(&l, g->sound);
        x_le32(&l, 0);
        x_u8(&l, g->sound_looped != 0);
    }
    x_blob_data(&b, field[0], &l);
    l.size = 0;
    k = game_inventory(g, inv, 64);
    for (j = 0; j < k; j++)
        if (inv[j] != GAME_AI_ICON)
            x_le32(&l, inv[j]);
    x_blob_data(&b, field[1], &l);
    l.size = 0;
    for (j = 0; j < g->evidence_count; j++) {
        x_le32(&l, g->evidence[j]);
        x_u8(&l, 1);
    }
    x_blob_data(&b, field[2], &l);
    l.size = 0;
    for (j = 0; j < g->kit_count; j++)
        x_le32(&l, g->kit_icons[j]);
    x_blob_data(&b, field[3], &l);
    /* Timers (0x5EF3D0): (timer, time, periodic); the time left for a
     * single timer, the period for a periodic one. */
    l.size = 0;
    for (j = 0; j < g->timer_count; j++) {
        const game_timer *t = &g->timers[j];
        x_le32(&l, t->id);
        x_le32(&l, t->period ? t->period : (t->due > g->now ? t->due - g->now : 0));
        x_u8(&l, t->period != 0);
    }
    x_blob_data(&b, field[4], &l);
    b.error |= l.error;
    free(l.data);

    for (k = 0; k < g->asked_count; k++)
        for (j = 0; j < g->asked[k].count && n < (int)XF_COUNT(ids); j++)
            ids[n++] = g->asked[k].conversation;
    x_add(&cls[0], next + 1, x_id_blob(&b, ids, n));
    x_add(&cls[2], next + 2, x_id_blob(&b, g->pda_notes, g->pda_note_count));
    for (n = 0; n < g->email_count; n++)
        ids[n] = g->emails[n].asset;
    x_add(&cls[4], next + 3, x_id_blob(&b, ids, n));
    for (n = 0, k = 0; k < g->email_count; k++)
        if (g->emails[k].read)
            ids[n++] = g->emails[k].asset;
    x_add(&cls[5], next + 4, x_id_blob(&b, ids, n));
    x_add(&cls[6], next + 5, x_id_blob(&b, g->pending_mail, g->pending_mail_count));
    next += 6;

    /* Photos: picture, media, time, rectangle, disc, downloaded. */
    for (k = 0; k < g->photo_count; k++) {
        static u32 px[X_THUMB_W * X_THUMB_H];
        xbuf p = { NULL, 0, 0, 0 };
        u32 at = x_record(&b);
        size_t f = x_blob_field(&b);
        x_add(&cls[3], next++, at);
        x_u32(&b, g->photos[k]);
        x_u32(&b, 0);
        x_u16(&b, 0), x_u16(&b, 0), x_u16(&b, 300), x_u16(&b, 240);
        x_u8(&b, (u32)g->disc);
        x_u8(&b, (g->photo_downloaded >> k) & 1);
        photo_pixels(g, g->photos[k], px);
        photo_pict(&p, px);
        x_blob_data(&b, f, &p);
        b.error |= p.error;
        free(p.data);
    }

    for (k = 0; k < 7; k++)
        if (cls[k].count)
            x_id_index(&b, &cls[k]);

    /* Class 1 lists its subclasses: those of the GAM, then the new ones. */
    {
        const u8 *e = croot + 8;
        u32 list = xf_be32(e + 19);
        if (list && list + 8 <= gam->size) {
            sub_n = xf_be16(gam->data + list + 6);
            for (k = 0; k < sub_n && k < 24; k++)
                sub_ids[k] = xf_be32(gam->data + list + 8 + k * 4);
            sub_n = k;
        }
        for (k = 0; k < 7; k++)
            sub_ids[sub_n++] = cls[k].class_id;
    }
    sub_list = x_align(&b);
    x_u16(&b, 0xC000);
    x_u32(&b, 1);
    x_u16(&b, (u32)sub_n);
    for (k = 0; k < sub_n; k++)
        x_u32(&b, sub_ids[k]);
    x_zero(&b, X_NODE_SIZE - 8 - (size_t)sub_n * 4);

    /* The class index: the GAM's entries, class 1 updated, then ours. */
    root = x_align(&b);
    x_u16(&b, 0xC000);
    x_u32(&b, 1);
    x_u16(&b, (u32)(gam_classes + 7));
    {
        const u8 *e = croot + 8;
        for (k = 0; k < gam_classes; k++) {
            int nidx = e[10];
            size_t at = b.size;
            x_bytes(&b, e, 27 + (size_t)nidx * 16);
            if (xf_be32(e + 11) == 1) {
                b.data[at + 8] = (u8)(sub_n >> 8);
                b.data[at + 9] = (u8)sub_n;
                x_put32(&b, at + 19, sub_list);
            }
            e += 27 + nidx * 16;
        }
    }
    for (k = 0; k < 7; k++) {
        x_zero(&b, 8);
        x_u16(&b, 0);
        x_u8(&b, 1);
        x_u32(&b, cls[k].class_id);
        x_u32(&b, cls[k].count);
        x_zero(&b, 8);
        x_u32(&b, 7);
        x_u32(&b, cls[k].count ? cls[k].root : 0);
        x_u32(&b, cls[k].class_id);
        x_bytes(&b, "ID  ", 4);
    }
    x_align(&b);
    x_put32(&b, 8, root);
    x_put32(&b, 12, (u32)(gam_classes + 7));
    x_put32(&b, 16, next);
    if (b.error) {
        free(b.data);
        return -1;
    }
    *out = b.data;
    *size = b.size;
    return 0;
}
