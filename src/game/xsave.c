/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* The original saved games (.x, see docs/formats/save.md): a copy of the
 * GAM database with the variables, enable states and strings as they
 * were, plus a header object (class 0x57) and lists of the state that the
 * engine keeps outside the GAM. 0x4D14B0 writes them. */
#include "game/game.h"

#include <stdlib.h>
#include <string.h>

#include "core/xf_log.h"

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
 * then five lists: sounds playing, inventory, evidence, kit icons, and
 * one the saves leave empty. */
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

    /* Sounds: (asset, looped, time). Our engine plays one beside the clips. */
    stride = blob_stride(g, &lists[0], 12, 9);
    for (i = 0; lists[0].p && lists[0].end - lists[0].p >= (long)(i + 1) * stride; i++) {
        const u8 *p = lists[0].p + i * stride;
        g->sound = le32(p);
        g->sound_looped = le32(p + 4) != 0;
    }
    if (g->sound)
        g->sound_serial++;
    /* Evidence: (icon, shown) for the evidence bar, then the kit's icons. */
    stride = blob_stride(g, &lists[2], 8, 5);
    for (i = 0; lists[2].p && lists[2].end - lists[2].p >= (long)(i + 1) * stride; i++)
        if (g->evidence_count < GAME_MAX_EVIDENCE)
            g->evidence[g->evidence_count++] = le32(lists[2].p + i * stride);
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
