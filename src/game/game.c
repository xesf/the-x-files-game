/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
#include "game.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "xf_file.h"
#include "xf_text.h"
#include "media/hot.h"
#include "xf_log.h"

static void after_triggers(game *g);
static int hand_empty(game *g);
static u32 inventory_triggers(game *g, u32 id);
static void set_item_text(game *g, u32 item, const char *text);
static int photo_function(game *g, const char *name, int argc, const vc_operand *args);
static int book_function(game *g, const char *name, int argc, const vc_operand *args);
static int pref_function(game *g, const char *name, int argc, const vc_operand *args);
static void note_text(game *g, u32 asset, char *out, size_t size);
static void media_search(game *g);
static const u32 g_note_pages[23];
static int item_media(game *g, u32 item, u32 *out, int max);
static void run_trigger_list_pair(game *g, i32 trigger, i32 list);
static void update_evidence(game *g);
static u32 first_enabled(game *g, u32 list);
static const game_iface_item *find_item(game *g, u32 id);

static void note_missing(game *g, const char *name)
{
    int i;
    for (i = 0; i < g->missing_count; i++)
        if (strcmp(g->missing[i], name) == 0)
            return;
    if (g->missing_count < (int)XF_COUNT(g->missing))
        snprintf(g->missing[g->missing_count++], sizeof(g->missing[0]), "%s", name);
    xf_info("function %s (not implemented)", name);
}

static void set_item_size(game *g, u32 item, int size)
{
    game_item_label *l = (game_item_label *)game_item_label_of(g, item);
    if (l)
        l->size = size;
}

/* --- Hotspots -------------------------------------------------------- */

static void add_spots(game *g, u32 list, int kind)
{
    u32 ids[GAME_MAX_SPOTS];
    int n = vc_read_list(&g->db, list, ids, GAME_MAX_SPOTS), i;

    for (i = 0; i < n && g->spot_count < GAME_MAX_SPOTS; i++) {
        game_spot *s = &g->spots[g->spot_count];
        if (!vc_is_enabled(&g->interp, ids[i]))
            continue;
        memset(s, 0, sizeof(*s));
        s->kind = kind;
        s->id = ids[i];
        if (kind == SPOT_NAV) {
            vc_nav nav;
            vc_hotspot h;
            if (vc_read_nav(&g->db, ids[i], &nav) != 0 || vc_read_hotspot(&g->db, nav.hotspot, &h) != 0)
                continue;
            s->region = h.region;
            s->target = nav.target;
            s->triggers = nav.extra;
        } else if (kind == SPOT_CHARACTER) {
            vc_char_view c;
            if (vc_read_char_view(&g->db, ids[i], &c) != 0)
                continue;
            s->region = c.region;
            s->triggers = c.triggers;
        } else {
            /* VCExplorable: region, then its trigger list. */
            xf_neo_reader r;
            if (xf_neo_object(&g->db, ids[i], &r, NULL, NULL) != 0)
                continue;
            s->region.cursor = xf_neo_u32(&r);
            s->region.left = (i16)xf_neo_u16(&r);
            s->region.top = (i16)xf_neo_u16(&r);
            s->region.right = (i16)xf_neo_u16(&r);
            s->region.bottom = (i16)xf_neo_u16(&r);
            s->triggers = xf_neo_u32(&r);
            if (r.error)
                continue;
        }
        g->spot_count++;
    }
}

static int has_spot_id(const game_spot *spots, int n, u32 id)
{
    int i;
    for (i = 0; i < n; i++)
        if (spots[i].id == id)
            return 1;
    return 0;
}

/* Rebuilds the hotspots of the view. Every object on screen has its own
 * trigger handler (TriggerHandler_attach, 0x416800): an object that
 * appears runs its kind 8 triggers and one that goes away its kind 9
 * triggers. Those can enable more objects, so this repeats a few times. */
static void build_spots(game *g)
{
    static game_spot old[GAME_MAX_SPOTS];
    static int depth;
    vc_view v;
    int i, n, pass;
    if (depth)
        return;
    depth++;
    /* A trigger can switch the view's clip in place (Amis comes to the
     * window): the old clip ends and the new one starts, with their
     * triggers. */
    if (g->view && !g->entering && vc_read_view(&g->db, g->view, &v) == 0) {
        u32 now = first_enabled(g, v.assets);
        if (now != g->view_asset) {
            vc_asset_ref a;
            if (g->view_asset && !g->view_asset_ended && vc_read_asset_ref(&g->db, g->view_asset, &a) == 0)
                vc_run_triggers(&g->interp, a.triggers, 9);
            g->view_asset = now;
            g->view_asset_ended = 0;
            if (now && vc_read_asset_ref(&g->db, now, &a) == 0)
                vc_run_triggers(&g->interp, a.triggers, 8);
        }
    }
    for (pass = 0; pass < 4; pass++) {
        int changed = 0;
        n = g->spot_count;
        memcpy(old, g->spots, sizeof(g->spots[0]) * (size_t)n);
        g->spot_count = 0;
        if (g->view && vc_read_view(&g->db, g->view, &v) == 0) {
            add_spots(g, v.navs, SPOT_NAV);
            add_spots(g, v.explorables, SPOT_EXPLORE);
            add_spots(g, v.characters, SPOT_CHARACTER);
        }
        for (i = 0; i < n; i++)
            if (!has_spot_id(g->spots, g->spot_count, old[i].id)) {
                vc_run_triggers(&g->interp, old[i].triggers, 9);
                changed = 1;
            }
        for (i = 0; i < g->spot_count; i++)
            if (!has_spot_id(old, n, g->spots[i].id) && !g->resuming) {
                vc_run_triggers(&g->interp, g->spots[i].triggers, 8);
                changed = 1;
            }
        if (!changed)
            break;
    }
    /* The dialogue ends when the character leaves the view. */
    if (g->talking && !has_spot_id(g->spots, g->spot_count, g->talking))
        g->talking = 0;
    update_evidence(g);
    depth--;
    for (i = 0; i < g->spot_count; i++)
        xf_debug("spot %u kind %d (%d,%d)-(%d,%d)", g->spots[i].id, g->spots[i].kind, g->spots[i].region.left,
                 g->spots[i].region.top, g->spots[i].region.right, g->spots[i].region.bottom);
}

/* --- Evidence -------------------------------------------------------- */

static int in_list(const u32 *ids, int n, u32 id)
{
    int i;
    for (i = 0; i < n; i++)
        if (ids[i] == id)
            return 1;
    return 0;
}

/* Collected evidence: the evidence icons that are enabled and are not the
 * kit's own copies (EnableKitIcon). The engine rebuilds them with each
 * view (0x477000 view_build_display); a new one runs its kind 8 triggers,
 * which put its copy in the kit. */
/* The Artificial Intuition icon is an evidence icon too (RegAIIcon names
 * it; the call in this build passes 0). */
#define AI_ICON GAME_AI_ICON

static int collected_evidence(game *g, u32 *ids, int max)
{
    int i, k = 0;
    for (i = 0; i < g->all_evidence_count && k < max; i++) {
        u32 id = g->all_evidence[i];
        if (id != AI_ICON && vc_is_enabled(&g->interp, id) && !in_list(g->kit_icons, g->kit_count, id))
            ids[k++] = id;
    }
    return k;
}

static u32 evidence_triggers(game *g, u32 id)
{
    xf_neo_reader r;
    int i;
    if (xf_neo_object(&g->db, id, &r, NULL, NULL) != 0)
        return 0;
    for (i = 0; i < 5; i++) /* name, icon, rollover, 0, cursor */
        xf_neo_u32(&r);
    return xf_neo_u32(&r);
}

static void update_evidence(game *g)
{
    u32 now[GAME_MAX_EVIDENCE];
    int n = collected_evidence(g, now, GAME_MAX_EVIDENCE), i;
    for (i = 0; i < g->evidence_count; i++)
        if (!in_list(now, n, g->evidence[i]))
            vc_run_triggers(&g->interp, evidence_triggers(g, g->evidence[i]), 9);
    for (i = 0; i < n; i++)
        if (!in_list(g->evidence, g->evidence_count, now[i]))
            vc_run_triggers(&g->interp, evidence_triggers(g, now[i]), 8);
    g->evidence_count = collected_evidence(g, g->evidence, GAME_MAX_EVIDENCE);
}

/* DispEvIcons turns the evidence bar on and off: the lab loop where
 * Amis leans on the window turns it on. */
int game_evidence(game *g, u32 *ids, int max)
{
    return g->show_evidence || g->kit_open ? collected_evidence(g, ids, max) : 0;
}

int game_click_evidence(game *g, u32 id)
{
    u32 ids[GAME_MAX_EVIDENCE];
    int n = game_evidence(g, ids, GAME_MAX_EVIDENCE);
    if (!in_list(ids, n, id))
        return 0;
    xf_info("hand over evidence %u", id);
    vc_run_triggers(&g->interp, evidence_triggers(g, id), 0);
    after_triggers(g);
    return 1;
}

/* The objects of the view go away before the view is left. */
static void clear_spots(game *g)
{
    int i, n = g->spot_count;
    game_spot old[GAME_MAX_SPOTS];
    memcpy(old, g->spots, sizeof(old[0]) * (size_t)n);
    g->spot_count = 0;
    for (i = 0; i < n; i++)
        vc_run_triggers(&g->interp, old[i].triggers, 9);
}

const game_spot *game_spot_at(const game *g, int x, int y)
{
    int i;
    /* The last matching spot wins, so small spots inside the large turn
     * areas can be clicked. */
    for (i = g->spot_count - 1; i >= 0; i--) {
        const vc_region *r = &g->spots[i].region;
        if (x >= r->left && x < r->right && y >= r->top && y < r->bottom)
            return &g->spots[i];
    }
    return NULL;
}

int game_view_media(game *g, u32 *ids, int max)
{
    vc_view v;
    u32 all[32];
    int n, i, k = 0;
    if (!g->view || vc_read_view(&g->db, g->view, &v) != 0)
        return 0;
    n = vc_read_list(&g->db, v.assets, all, 32);
    for (i = 0; i < n && k < max; i++)
        if (vc_is_enabled(&g->interp, all[i]))
            ids[k++] = all[i];
    return k;
}

/* --- Views ----------------------------------------------------------- */


/* Removes layout k. Its items run their leave triggers (kind 9). */
static void remove_layout(game *g, int k)
{
    game_iface d = g->ifaces[k];
    int i, t;
    g->fade_back = 0;
    for (t = 0; t < g->text_count; t++)
        for (i = 0; i < d.count; i++)
            if (g->texts[t].item == d.items[i].id) {
                g->texts[t--] = g->texts[--g->text_count];
                break;
            }
    memmove(&g->ifaces[k], &g->ifaces[k + 1], sizeof(g->ifaces[0]) * (size_t)(g->iface_count - k - 1));
    g->iface_count--;
    g->iface_serial++;
    for (i = 0; i < d.count; i++)
        vc_run_triggers(&g->interp, d.items[i].item.triggers, 9);
}

/* Layouts marked with SetIFLAsView belong to the view they were opened
 * in (0x41B150 sets the flag at +0x178 of the layout display). */
static void close_view_layouts(game *g)
{
    int k;
    for (k = 0; k < g->iface_count; k++)
        if (g->ifaces[k].as_view)
            remove_layout(g, k--);
}

int game_enter_view(game *g, u32 id)
{
    vc_view v;
    if (vc_read_view(&g->db, id, &v) != 0) {
        xf_warn("game: %u is not a view", id);
        return -1;
    }
    /* Kinds 8 and 9 are entering and leaving (see scene-graph.md). Leave
     * from the inside out, enter from the outside in. */
    {
        const vc_place *from = g->view ? vc_place_of(&g->places, g->view) : NULL;
        const vc_place *to = vc_place_of(&g->places, id);
        u32 epoch = g->view_epoch, outer = g->entering;
        g->entering = id;
        clear_spots(g);
        if (g->view)
            vc_run_triggers(&g->interp, g->view_triggers, 9);
        if (g->view_asset) {
            vc_asset_ref a;
            if (!g->view_asset_ended && vc_read_asset_ref(&g->db, g->view_asset, &a) == 0)
                vc_run_triggers(&g->interp, a.triggers, 9);
            g->view_asset = 0;
        }
        if (from && (!to || to->viewpoint != from->viewpoint))
            vc_run_triggers(&g->interp, from->viewpoint_triggers, 9);
        if (from && (!to || to->location != from->location))
            vc_run_triggers(&g->interp, from->location_triggers, 9);
        if (from && (!to || to->node != from->node))
            vc_run_triggers(&g->interp, from->node_triggers, 9);
        if (to && (!from || to->node != from->node))
            vc_run_triggers(&g->interp, to->node_triggers, 8);
        if (to && (!from || to->location != from->location))
            vc_run_triggers(&g->interp, to->location_triggers, 8);
        if (to && (!from || to->viewpoint != from->viewpoint))
            vc_run_triggers(&g->interp, to->viewpoint_triggers, 8);
        g->entering = outer;
        if (g->view_epoch != epoch)
            return 0; /* a trigger already went to another view */
    }
    close_view_layouts(g);
    g->talking = 0;
    g->view = id;
    g->view_epoch++;
    g->view_triggers = v.triggers;
    g->entering = id;
    vc_run_triggers(&g->interp, v.triggers, 8);
    /* The view's clip starts next, with its own start triggers: the
     * office loop at the warehouse puts Skinner in the office. */
    g->view_asset = first_enabled(g, v.assets);
    g->view_asset_ended = 0;
    if (g->view_asset) {
        vc_asset_ref a;
        if (vc_read_asset_ref(&g->db, g->view_asset, &a) == 0)
            vc_run_triggers(&g->interp, a.triggers, 8);
    }
    g->entering = 0;
    if (g->pending_view && g->pending_view != id) {
        u32 next = g->pending_view;
        g->pending_view = 0;
        return game_enter_view(g, next);
    }
    g->pending_view = 0;
    build_spots(g);
    xf_info("view %u: %d hotspots", id, g->spot_count);
    return 0;
}

void game_view_clip_end(game *g, u32 asset)
{
    vc_asset_ref a;
    if (!asset || asset != g->view_asset || g->view_asset_ended || vc_read_asset_ref(&g->db, asset, &a) != 0)
        return;
    g->view_asset_ended = 1;
    xf_info("view clip %u ended", asset);
    if (a.triggers) {
        vc_run_triggers(&g->interp, a.triggers, 9);
        after_triggers(g);
    }
}

/* Puts the player back in a view without running any entry trigger: a
 * loaded game already holds what those triggers did. */
int game_resume_view(game *g, u32 id)
{
    vc_view v;
    if (vc_read_view(&g->db, id, &v) != 0)
        return -1;
    clear_spots(g);
    g->view = id;
    g->view_epoch++;
    g->view_triggers = v.triggers;
    g->view_asset = first_enabled(g, v.assets);
    g->view_asset_ended = 0;
    g->pending_view = 0;
    g->resuming = 1; /* the hotspots appear without their triggers */
    build_spots(g);
    g->resuming = 0;
    return 0;
}

/* Triggers run outside a view change can enable objects or set a view:
 * follow the view, or rebuild the hotspots. */
/* Binoculars: the close-up of the view, a view of the same viewpoint that
 * no move leads to. Its name starts with the compass letter of the view
 * it looks out of ("S number cu" beside the south view). 0 for none. */
static u32 binocular_view(game *g)
{
    static const char letters[] = " N E S W";
    const vc_place *p = vc_place_of(&g->places, g->view);
    u32 views[64];
    vc_view v;
    int n, i;
    if (!p || vc_read_view(&g->db, g->view, &v) != 0 || v.direction > 7 || !(v.direction & 1))
        return 0;
    n = vc_read_list(&g->db, p->viewpoint, views, 64);
    for (i = 0; i < n; i++) {
        vc_view c;
        char name[64];
        if (views[i] == g->view || vc_read_view(&g->db, views[i], &c) != 0 || c.direction <= 8 ||
            vc_read_name(&g->db, c.direction, name, sizeof(name)) != 0)
            continue;
        if (name[0] == letters[v.direction] && name[1] == ' ' && strstr(name, " cu"))
            return views[i];
    }
    return 0;
}

static void after_triggers(game *g)
{
    if (g->pending_kind6) {
        /* PhoneSend found the number: the send key's kind 6 places the call */
        u32 list = g->pending_kind6;
        g->pending_kind6 = 0;
        vc_run_triggers(&g->interp, list, 6);
    }
    if (g->binoc_from && hand_empty(g) && !g->pending_view) {
        /* The binoculars are put away: back from the close-up. */
        g->pending_view = g->binoc_from;
        g->binoc_from = 0;
    }
    if (g->pending_view) {
        u32 next = g->pending_view, from = g->view;
        g->pending_view = 0;
        if (game_enter_view(g, next) == 0 && from && g->history_len < GAME_HISTORY)
            g->history[g->history_len++] = from;
    } else {
        build_spots(g);
    }
}

void game_back(game *g)
{
    if (g->history_len > 0)
        game_enter_view(g, g->history[--g->history_len]);
}

/* --- Media ----------------------------------------------------------- */

void game_queue_media(game *g, u32 asset)
{
    if ((g->queue_tail + 1) % GAME_MEDIA_QUEUE != g->queue_head) {
        g->queue[g->queue_tail].asset = asset;
        g->queue[g->queue_tail].card = 0;
        g->queue[g->queue_tail].conversation = 0;
        g->queue_tail = (g->queue_tail + 1) % GAME_MEDIA_QUEUE;
    }
}

int game_media_pending(const game *g) { return g->queue_head != g->queue_tail; }

/* Media can carry a trigger list: kind 8 runs when the media starts and
 * kind 9 when it ends (see scene-graph.md). */
int game_media_start(game *g, game_media *m)
{
    vc_asset_ref a;
    while (game_media_pending(g)) {
        *m = g->queue[g->queue_head];
        g->queue_head = (g->queue_head + 1) % GAME_MEDIA_QUEUE;
        if (vc_read_asset_ref(&g->db, m->asset, &a) != 0)
            continue;
        g->current = *m;
        g->played[g->played_count++ % GAME_PLAYED] = m->asset;
        g->clip_starting = 1;
        vc_run_triggers(&g->interp, a.triggers, 8);
        g->clip_starting = 0;
        return 1;
    }
    return 0;
}

void game_media_end(game *g)
{
    vc_asset_ref a;
    u32 asset = g->current.asset, conv = g->current.conversation;
    g->current.asset = 0;
    g->current.conversation = 0;
    if (asset && vc_read_asset_ref(&g->db, asset, &a) == 0 && a.triggers) {
        vc_run_triggers(&g->interp, a.triggers, 9);
        after_triggers(g);
    }
    if (conv) {
        vc_conversation c;
        if (vc_read_conversation(&g->db, conv, &c) == 0) {
            vc_run_triggers(&g->interp, c.triggers, 9);
            after_triggers(g);
        }
    }
    if (asset && asset == g->shot_media) {
        u32 view = g->shot_view;
        g->shot_media = g->shot_view = 0;
        /* unless the clip's own end triggers went somewhere */
        if (view && g->view_epoch == g->shot_epoch)
            game_enter_view(g, view);
    }
}

void game_overlay_end(game *g)
{
    vc_asset_ref a;
    u32 asset = g->overlay;
    if (!asset)
        return;
    g->overlay = 0;
    if (vc_read_asset_ref(&g->db, asset, &a) == 0)
        vc_run_triggers(&g->interp, a.triggers, 9);
}

/* A sound that is not a loop ends: its kind 9 triggers run (the alarm
 * clock's end shows the morning). */
void game_sound_end(game *g)
{
    vc_asset_ref a;
    u32 sound = g->sound;
    if (!sound || vc_read_asset_ref(&g->db, sound, &a) != 0)
        return;
    g->sound = 0;
    if (strstr(a.logical, "/Loops/"))
        return;
    vc_run_triggers(&g->interp, a.triggers, 9);
    after_triggers(g);
}

/* A media list holds alternatives: the first entry that is enabled
 * plays. Actions switch entries on and off, for example a clerk's line
 * before and after the badge is shown. Returns 0 when none is enabled. */
static u32 first_enabled(game *g, u32 list)
{
    u32 ids[32];
    int n = vc_read_list(&g->db, list, ids, 32), i;
    for (i = 0; i < n; i++)
        if (vc_is_enabled(&g->interp, ids[i]))
            return ids[i];
    return 0;
}

static void start_sound(game *g, u32 asset, int looped)
{
    vc_asset_ref a;
    if (vc_read_asset_ref(&g->db, asset, &a) != 0)
        return;
    if (g->effect_mode) { /* over the ambience, which goes on */
        g->effect = asset;
        g->effect_serial++;
        return;
    }
    g->sound = asset;
    g->sound_looped = looped;
    g->sound_serial++;
    g->played[g->played_count++ % GAME_PLAYED] = asset;
    vc_run_triggers(&g->interp, a.triggers, 8);
}

/* A card every 4500 ms (0x45BAA0), shown for 3000 ms (0x45B9C0); the
 * cards end at the first missing string (0x401EE0): 4 to 21 after the
 * intro. */
#define CREDIT_PERIOD 4500
#define CREDIT_SHOWN 3000

/* Media actions name a media list. Sounds (paths under Sounds/) play
 * beside the clips instead of in the queue. */
/* 0x479BA0 act_media: selector 1 plays the list's first enabled media,
 * 2 plays it looped, 3 stops every media of the list: one playing stops
 * (0x513270), one queued is dropped (0x516550). */
static void host_play_media(void *user, u32 list, u8 selector)
{
    game *g = user;
    vc_asset_ref a;
    u32 asset;
    if (selector == 3) {
        u32 ids[64];
        int n = vc_read_list(&g->db, list, ids, 64), i, k;
        for (i = 0; i < n; i++) {
            if (g->sound == ids[i]) {
                g->sound = 0;
                g->sound_serial++;
            }
            if (g->current.asset == ids[i]) {
                g->current.asset = 0;
                g->current.conversation = 0;
                g->clip_stop_serial++;
            }
            for (k = g->queue_head; k != g->queue_tail; k = (k + 1) % GAME_MEDIA_QUEUE)
                if (g->queue[k].asset == ids[i])
                    g->queue[k].asset = 0; /* game_media_start skips it */
        }
        return;
    }
    asset = first_enabled(g, list);
    if (!asset || vc_read_asset_ref(&g->db, asset, &a) != 0) {
        xf_info("play media list #%u: nothing enabled", list);
        return;
    }
    if (strncmp(a.logical, "Sounds/", 7) == 0) {
        /* The credits music (FOCredit.mus, after RollHdCredits) waits for
         * the first card. */
        if (g->credits_pending && selector == 1) {
            g->credits_sound = asset;
            return;
        }
        start_sound(g, asset, selector == 2);
        return;
    }
    game_queue_media(g, asset);
}

/* 0x46A0D0 act_set_view: the engine runs queued actions in order, so the
 * view changes before the next action of the list runs. */
static void host_go_to_view(void *user, u32 view)
{
    game *g = user;
    u32 from = g->view;
    if (view == g->view || view == g->entering)
        return; /* already there */
    if (game_enter_view(g, view) == 0 && from && from != view && g->history_len < GAME_HISTORY)
        g->history[g->history_len++] = from;
}

/* --- Timers ---------------------------------------------------------- */

static void remove_timer(game *g, u8 id)
{
    int i;
    for (i = 0; i < g->timer_count; i++)
        if (g->timers[i].id == id)
            g->timers[i--] = g->timers[--g->timer_count];
}

/* Starting a timer replaces any timer with the same number. */
static void host_timer(void *user, u8 id, u32 ms, u8 control)
{
    game *g = user;
    remove_timer(g, id);
    if ((control & 0x7F) == 0 && g->timer_count < GAME_MAX_TIMERS) {
        game_timer *t = &g->timers[g->timer_count++];
        t->id = id;
        t->due = g->now + ms;
        t->period = control & 0x80 ? XF_MAX(ms, 1) : 0;
    }
}

/* Runs the timer triggers of everything on screen for each timer that
 * fired: the view and the places around it, and the media playing. */
/* The first card and the credits music start together. */
static void start_credits(game *g)
{
    g->credits_pending = 0;
    g->credits_rolling = 1;
    g->credits_start = g->now - CREDIT_PERIOD;
    if (g->credits_sound) {
        start_sound(g, g->credits_sound, 0);
        g->credits_sound = 0;
    }
}

/* Subliminal Messages (Pref_bSubliminalMessages, var 985): the text
 * object at app +0x108 (vtable 0x65DE88, XFilesS.dll) runs while the
 * preference is on (0x4E4FD0). Every 6000 ms (slot 4, 0x443BB0) slot 9
 * (0x4681D0) draws 1 to 4 and, unless it is 4, shows a word for 0 ms
 * (slot 5, 0x45BA30): one frame. */
#define SUBLIMINAL_PERIOD 6000

static void subliminal_tick(game *g)
{
    if (!game_pref(g, 985) || !g->subliminal_strings.count) {
        g->subliminal_due = 0;
        return;
    }
    if (!g->subliminal_due) {
        g->subliminal_due = g->now + SUBLIMINAL_PERIOD;
        return;
    }
    if ((i32)(g->now - g->subliminal_due) < 0)
        return;
    g->subliminal_due = g->now + SUBLIMINAL_PERIOD;
    g->rand_seed = g->rand_seed * 1103515245u + 12345u;
    if ((g->rand_seed >> 16) % 4 == 3)
        return; /* the draw came out 4 */
    g->rand_seed = g->rand_seed * 1103515245u + 12345u;
    g->subliminal_id = g->subliminal_strings.ids[(g->rand_seed >> 16) % g->subliminal_strings.count];
    g->subliminal_serial++;
    xf_debug("subliminal: %s", xf_strings_get(&g->subliminal_strings, g->subliminal_id));
}

void game_play_effect(game *g, u32 asset)
{
    g->effect = asset;
    g->effect_serial++;
}

const char *game_subliminal(const game *g, u32 *serial)
{
    *serial = g->subliminal_serial;
    return g->subliminal_serial ? xf_strings_get(&g->subliminal_strings, g->subliminal_id) : NULL;
}

void game_tick(game *g, u32 now_ms)
{
    int i;
    g->now = now_ms;
    if (g->credits_pending && !g->current.asset && !game_media_pending(g))
        start_credits(g); /* the clips queued with RollHdCredits have played */
    if (g->credits_rolling) {
        /* the cards end at the first missing string */
        u32 n = (g->now - g->credits_start) / CREDIT_PERIOD;
        if (n && !xf_strings_get(&g->credit_strings, (u32)g->credits_first + n - 1))
            g->credits_rolling = 0;
    }
    if (g->game_over)
        return; /* the game has ended: nothing runs on */
    subliminal_tick(g);
    for (i = 0; i < g->timer_count; i++) {
        game_timer *t = &g->timers[i];
        const vc_place *p;
        u8 id = t->id;
        if ((i32)(g->now - t->due) < 0)
            continue;
        if (t->period)
            t->due += t->period;
        else
            g->timers[i--] = g->timers[--g->timer_count];
        xf_debug("timer %u fired", id);
        p = g->view ? vc_place_of(&g->places, g->view) : NULL;
        if (p) {
            vc_run_timer_triggers(&g->interp, p->node_triggers, id);
            vc_run_timer_triggers(&g->interp, p->location_triggers, id);
            vc_run_timer_triggers(&g->interp, p->viewpoint_triggers, id);
        }
        vc_run_timer_triggers(&g->interp, g->view_triggers, id);
        {
            /* and every object on screen: hotspots and interface items */
            game_spot spots[GAME_MAX_SPOTS];
            int n = g->spot_count, k, j;
            memcpy(spots, g->spots, sizeof(spots[0]) * (size_t)n);
            for (k = 0; k < n; k++)
                vc_run_timer_triggers(&g->interp, spots[k].triggers, id);
            /* The items as they are now: a trigger that closes its layout
             * (the hint overlays close on timer 22, two of them at once)
             * must not make the others' triggers skip their turn. */
            u32 items[GAME_MAX_IFACES * GAME_MAX_IFACE_ITEMS], lists[GAME_MAX_IFACES * GAME_MAX_IFACE_ITEMS];
            int m = 0;
            for (k = 0; k < g->iface_count; k++)
                for (j = 0; j < g->ifaces[k].count; j++, m++) {
                    items[m] = g->ifaces[k].items[j].id;
                    lists[m] = g->ifaces[k].items[j].item.triggers;
                }
            for (k = 0; k < m; k++) {
                g->trigger_item = items[k];
                vc_run_timer_triggers(&g->interp, lists[k], id);
            }
            g->trigger_item = 0;
        }
        if (g->current.asset) {
            vc_asset_ref a;
            if (vc_read_asset_ref(&g->db, g->current.asset, &a) == 0)
                vc_run_timer_triggers(&g->interp, a.triggers, id);
        }
        after_triggers(g);
    }
}

/* --- Interfaces ------------------------------------------------------ */

/* 0x44E760 act_interface: control 0 shows a VCIFaceLayout unless it is
 * already up, 1 closes it. */
static void host_interface(void *user, u32 id, u8 control);

/* 0x4A3FE0: a message in the message layout (60115), one of its items,
 * which start hidden. */
static void show_message_item(game *g, u32 item)
{
    const game_iface_item *it;
    host_interface(g, 60115, 0);
    vc_set_enabled(&g->interp, item, 1);
    /* Every item set its text as the layout opened; the shown one again. */
    if ((it = find_item(g, item)) != NULL) {
        g->trigger_item = item;
        vc_run_triggers(&g->interp, it->item.triggers, 8);
        g->trigger_item = 0;
    }
}

static int login_matches_name(game *g, u32 name)
{
    char want[64];
    return strcasecmp(g->login[0], game_text(g, name, want, sizeof(want))) == 0;
}

static int login_matches(game *g, u32 name, u32 pass)
{
    char want[64];
    return login_matches_name(g, name) && strcasecmp(g->login[1], game_text(g, pass, want, sizeof(want))) == 0;
}

/* 0x4F2C30: the PowerBook's login shows Scully's name and an empty
 * password; after four tries both fields are locked. */
static void powerbook_reset(game *g)
{
    game_text(g, 68913, g->login[0], sizeof(g->login[0]));
    g->login[1][0] = '\0';
    g->focus_item = 50749;
    if (vc_get_var(&g->interp, 5326) >= 4) {
        vc_set_enabled(&g->interp, 50749, 0);
        vc_set_enabled(&g->interp, 50750, 0);
        g->focus_item = 0;
    }
    g->iface_serial++;
}

static void host_interface(void *user, u32 id, u8 control)
{
    game *g = user;
    u32 ids[GAME_MAX_IFACE_ITEMS];
    int k, n, i;
    for (k = 0; k < g->iface_count; k++)
        if (g->ifaces[k].layout == id)
            break;
    if (control == 1) {
        if (k < g->iface_count)
            remove_layout(g, k);
        g->iface_hover = 0;
        return;
    }
    if (control != 0 || k < g->iface_count || g->iface_count >= GAME_MAX_IFACES)
        return;
    n = vc_read_list(&g->db, id, ids, GAME_MAX_IFACE_ITEMS);
    if (n <= 0)
        return;
    {
        game_iface *d = &g->ifaces[g->iface_count++];
        d->layout = id;
        d->as_view = 0;
        d->count = 0;
        for (i = 0; i < n; i++) {
            game_iface_item *it = &d->items[d->count];
            if (vc_read_iface_item(&g->db, ids[i], &it->item) != 0)
                continue;
            it->id = ids[i];
            d->count++;
        }
        g->iface_serial++;
        xf_info("interface %u: %d items", id, d->count);
        /* Items run their enter triggers (kind 8) when the layout opens. */
        for (i = 0; i < d->count; i++) {
            g->trigger_item = d->items[i].id;
            vc_run_triggers(&g->interp, d->items[i].item.triggers, 8);
        }
        g->trigger_item = 0;
    }
}

static int item_order(const game_iface_item *it) { return it->item.flags & 0xFF; }
/* A smaller item over a larger one takes the pointer: a close-up's page
 * strips (next, previous) and zoom areas lie over its full-screen "back
 * up" area. */
static long area(const game_iface_item *it)
{
    const vc_region *r = &it->item.region;
    return (long)(r->right - r->left) * (long)(r->bottom - r->top);
}

/* The item with triggers under (x, y), searching the layouts from the
 * top one down: the smallest, so a close-up's full-screen item behind its
 * picture takes only the clicks no other item takes; the inventory bar
 * only takes clicks on its items. */
const game_iface_item *game_iface_item_at(game *g, int x, int y)
{
    int k, i;
    for (k = g->iface_count - 1; k >= 0; k--) {
        const game_iface *d = &g->ifaces[k];
        const game_iface_item *best = NULL;
        for (i = 0; i < d->count; i++) {
            const game_iface_item *it = &d->items[i];
            const vc_region *r = &it->item.region;
            if (!it->item.triggers || !vc_is_enabled(&g->interp, it->id) || x < r->left || x >= r->right ||
                y < r->top || y >= r->bottom)
                continue;
            /* A plain hotspot (no picture) takes the pointer before a
             * picture under it: the PDA maps lay a hotspot over each dot,
             * and only the hotspot's triggers are up to date (the dot's
             * own list is an older version). */
            if (!best || (!it->item.assets && best->item.assets) ||
                (!it->item.assets == !best->item.assets &&
                 (area(it) < area(best) || (area(it) == area(best) && item_order(it) >= item_order(best)))))
                best = it;
        }
        if (best)
            return best;
    }
    return NULL;
}

/* Mouse over and off interface items run their kind 4 and 5 triggers. */
void game_hover(game *g, int x, int y)
{
    const game_iface_item *it = game_iface_item_at(g, x, y);
    u32 id = it ? it->id : 0;
    if (id == g->iface_hover)
        return;
    if (g->iface_hover) {
        vc_iface_item old;
        if (vc_read_iface_item(&g->db, g->iface_hover, &old) == 0)
            vc_run_triggers(&g->interp, old.triggers, 5);
    }
    g->iface_hover = id;
    if (it)
        vc_run_triggers(&g->interp, it->item.triggers, 4);
    after_triggers(g);
}

/* The pointer moved onto an inventory item, or off it (0): kinds 4 and 5
 * of the item's triggers run, as for layout items (the evidence kit pops
 * up its evidence). */
void game_hover_inventory(game *g, u32 id, int on_evidence)
{
    g->pointer_on_evidence = on_evidence;
    if (id == g->inv_hover)
        return;
    if (g->inv_hover)
        vc_run_triggers(&g->interp, inventory_triggers(g, g->inv_hover), 5);
    g->inv_hover = id;
    if (id)
        vc_run_triggers(&g->interp, inventory_triggers(g, id), 4);
    after_triggers(g);
}

/* --- Cursors --------------------------------------------------------- */

/* VCDefaultCursors 321: cursors for characters and for objects when
 * their region names none. */
#define CURSOR_TALK 290
#define CURSOR_LOOK 7164

u32 game_cursor_at(game *g, int x, int y)
{
    const game_iface_item *it = game_iface_item_at(g, x, y);
    const game_spot *s;
    if (it) /* a close-up's backdrop has the "back up" cursor */
        return it->item.region.cursor;
    if (game_scene_held(g))
        return 0;
    s = game_spot_at(g, x, y);
    if (!s)
        return 0;
    if (s->region.cursor && vc_class(&g->db, s->region.cursor) == 0x3D)
        return s->region.cursor;
    return s->kind == SPOT_CHARACTER ? CURSOR_TALK : s->kind == SPOT_EXPLORE ? CURSOR_LOOK : 0;
}

/* --- Conversations --------------------------------------------------- */

i32 game_reg_value(game *g, const char *name, int i)
{
    int k;
    for (k = 0; k < g->reg_count; k++)
        if (strcmp(g->regs[k].name, name) == 0 && i < g->regs[k].argc)
            return vc_get_var(&g->interp, g->regs[k].vars[i]);
    return 0;
}

/* RegCurrInvVar registers the variable of the item in hand and the value
 * that means no item (iCurrInventory and Inventory_None). */
static int hand_empty(game *g)
{
    return game_reg_value(g, "RegCurrInvVar", 0) == game_reg_value(g, "RegCurrInvVar", 1);
}

static void set_reg_var(game *g, const char *name, int i, i32 value)
{
    int k;
    for (k = 0; k < g->reg_count; k++)
        if (strcmp(g->regs[k].name, name) == 0 && i < g->regs[k].argc)
            vc_set_var(&g->interp, g->regs[k].vars[i], value);
}

int game_hold(game *g, const char *item)
{
    char name[48];
    u32 id;
    if (!item) {
        set_reg_var(g, "RegCurrInvVar", 0, game_reg_value(g, "RegCurrInvVar", 1));
        after_triggers(g);
        return 1;
    }
    snprintf(name, sizeof(name), "Inventory_i%s", item);
    id = vc_find_var(&g->interp, name);
    if (!id)
        return 0;
    set_reg_var(g, "RegCurrInvVar", 0, vc_get_var(&g->interp, id));
    return 1;
}

int game_icons(game *g, u32 *ids, int max)
{
    int i, k = 0;
    for (i = 0; i < g->all_icon_count && k < max; i++)
        if (vc_is_enabled(&g->interp, g->all_icons[i]))
            ids[k++] = g->all_icons[i];
    return k;
}

const char *game_object_label(game *g, u32 id, char *buf, size_t size)
{
    xf_neo_reader r;
    u32 list = 0, ids[4];
    int c = vc_class(&g->db, id), i;
    buf[0] = '\0';
    if ((c != VC_EMOTION_ICON && c != VC_ACTION_ICON && c != VC_INVENTORY && c != VC_EVIDENCE_ICON &&
         c != VC_IDEA_ICON) ||
        xf_neo_object(&g->db, id, &r, NULL, NULL) != 0)
        return buf;
    for (i = 0; i < 5; i++)
        list = xf_neo_u32(&r);
    /* the fifth field: the label's media reference, a string of the text
     * language ("Funny", "Badge") */
    if (!r.error && vc_read_list(&g->db, list, ids, 4) > 0)
        game_text(g, ids[0], buf, size);
    if (!buf[0] && xf_neo_object(&g->db, id, &r, NULL, NULL) == 0) {
        /* the writers' name, "Field Office N1A Cook - Funny": its last part */
        char *dash;
        vc_read_name(&g->db, xf_neo_u32(&r), buf, size);
        dash = strrchr(buf, '-');
        if (dash && dash[1] == ' ')
            memmove(buf, dash + 2, strlen(dash + 2) + 1);
    }
    return buf;
}

int game_read_icon(game *g, u32 id, game_icon *icon)
{
    xf_neo_reader r;
    int c = vc_class(&g->db, id);
    if ((c != VC_EMOTION_ICON && c != VC_ACTION_ICON) || xf_neo_object(&g->db, id, &r, NULL, NULL) != 0)
        return -1;
    icon->name = xf_neo_u32(&r);
    xf_neo_u32(&r);
    icon->icon = xf_neo_u32(&r);
    xf_neo_u32(&r);
    xf_neo_u32(&r);
    icon->triggers = xf_neo_u32(&r);
    return r.error ? -1 : 0;
}

int game_click_icon(game *g, u32 id)
{
    game_icon icon;
    u32 shown[16];
    int n = game_icons(g, shown, 16), i;
    if (game_read_icon(g, id, &icon) != 0)
        return 0;
    xf_info("click icon %u", id);
    /* Choosing one answer takes all the icons on offer away. */
    for (i = 0; i < n; i++)
        vc_set_enabled(&g->interp, shown[i], 0);
    vc_run_triggers(&g->interp, icon.triggers, 0);
    after_triggers(g);
    return 1;
}

int game_inventory(game *g, u32 *ids, int max)
{
    int i, k = 0;
    for (i = 0; i < g->all_inventory_count && k < max; i++)
        if (vc_is_enabled(&g->interp, g->all_inventory[i]))
            ids[k++] = g->all_inventory[i];
    if (g->ai_list && k < max && game_pref(g, 873)) /* Pref_bArtificialIntuition (0x4FEA20) */
        ids[k++] = AI_ICON; /* Artificial Intuition: a hint is ready */
    return k;
}

/* VCInventory: name, icon, highlighted icon, 0, cursor, trigger list.
 * A click runs the kind 0 triggers: OnInvClick takes the item in hand,
 * OnIntInvClick opens tools such as the PDA. */
static u32 inventory_triggers(game *g, u32 id)
{
    xf_neo_reader r;
    int i;
    if (vc_class(&g->db, id) != VC_INVENTORY || xf_neo_object(&g->db, id, &r, NULL, NULL) != 0)
        return 0;
    for (i = 0; i < 5; i++)
        xf_neo_u32(&r);
    return xf_neo_u32(&r);
}

/* The value that stands for an inventory item: the argument of the
 * On...Click call (OnInvClick, OnCameraClick, ...) in its click list. */
static i32 arg_value(game *g, const vc_operand *o);

static i32 inventory_value(game *g, u32 id)
{
    u32 trigs[16], acts[64];
    int n = vc_read_list(&g->db, inventory_triggers(g, id), trigs, 16), i, j, m;
    for (i = 0; i < n; i++) {
        xf_neo_reader r;
        u32 list;
        if (xf_neo_object(&g->db, trigs[i], &r, NULL, NULL) != 0)
            continue;
        list = xf_neo_u32(&r);
        if (xf_neo_u8(&r) != 0)
            continue;
        m = vc_read_list(&g->db, list, acts, 64);
        for (j = 0; j < m; j++) {
            vc_action a;
            const char *f;
            size_t len;
            if (vc_read_action(&g->db, acts[j], &a) != 0 || a.type != VC_ACT_FUNCTION || a.u.function.argc != 1)
                continue;
            f = a.u.function.name;
            len = strlen(f);
            if (strncmp(f, "On", 2) == 0 && len > 7 && strcmp(f + len - 5, "Click") == 0)
                return arg_value(g, &a.u.function.args[0]);
        }
    }
    return 0;
}

int game_click_inventory(game *g, u32 id)
{
    u32 triggers = inventory_triggers(g, id);
    if (id == AI_ICON && g->ai_list) {
        /* The Artificial Intuition icon runs the list SetAIList named,
         * with its trigger's parameters. */
        xf_info("artificial intuition: list %u", g->ai_list);
        vc_run_trigger_list(&g->interp, g->ai_trigger, g->ai_list);
        after_triggers(g);
        return 1;
    }
    if (!triggers)
        return 0;
    xf_info("click inventory %u", id);
    vc_run_triggers(&g->interp, triggers, 0);
    after_triggers(g);
    return 1;
}

/* Whether a text field hides what is typed: the password fields of the
 * logins (LoginKeyPress fields 2 and 4). */
int game_item_secret(game *g, u32 item)
{
    const game_iface_item *it = find_item(g, item);
    u32 ids[64];
    int n, i;
    if (!it)
        return 0;
    n = vc_read_list(&g->db, it->item.triggers, ids, 64);
    for (i = 0; i < n; i++) {
        xf_neo_reader r;
        u32 acts[8];
        int m, j;
        if (xf_neo_object(&g->db, ids[i], &r, NULL, NULL) != 0)
            continue;
        m = vc_read_list(&g->db, xf_neo_u32(&r), acts, 8);
        for (j = 0; j < m; j++) {
            vc_action a;
            if (vc_read_action(&g->db, acts[j], &a) == 0 && a.valid && a.type == VC_ACT_FUNCTION &&
                strcmp(a.u.function.name, "LoginKeyPress") == 0 && a.u.function.argc == 2)
                return ((int)a.u.function.args[0].value - 1) % 2 == 1;
        }
    }
    return 0;
}

const char *game_item_text(game *g, u32 item, char *buf, size_t size)
{
    const game_iface_item *it = find_item(g, item);
    u32 ids[64];
    int n, i;
    if (!it)
        return NULL;
    /* The login fields: their kind 3 trigger calls LoginKeyPress(field). */
    n = vc_read_list(&g->db, it->item.triggers, ids, 64);
    for (i = 0; i < n; i++) {
        xf_neo_reader r;
        u32 acts[8];
        int m, j;
        if (xf_neo_object(&g->db, ids[i], &r, NULL, NULL) != 0)
            continue;
        m = vc_read_list(&g->db, xf_neo_u32(&r), acts, 8);
        for (j = 0; j < m; j++) {
            vc_action a;
            if (vc_read_action(&g->db, acts[j], &a) == 0 && a.valid && a.type == VC_ACT_FUNCTION &&
                strcmp(a.u.function.name, "INGKeyPressed") == 0) {
                snprintf(buf, size, "%s", g->ing);
                return buf;
            }
            if (a.valid && a.type == VC_ACT_FUNCTION && strcmp(a.u.function.name, "MSKeyPressed") == 0) {
                snprintf(buf, size, "%s", g->media_query);
                return buf;
            }
            if (a.valid && a.type == VC_ACT_FUNCTION &&
                strcmp(a.u.function.name, "LoginKeyPress") == 0 && a.u.function.argc == 2) {
                int field = (int)a.u.function.args[0].value - 1;
                size_t len;
                if (field < 0 || field > 3)
                    return NULL;
                field &= 1; /* fields 3 and 4, the PowerBook's, share the buffers */
                len = strlen(g->login[field]);
                if (field == 0) {
                    snprintf(buf, size, "%s", g->login[0]);
                } else {
                    len = XF_MIN(len, size - 1);
                    memset(buf, '*', len);
                    buf[len] = '\0';
                }
                return buf;
            }
        }
    }
    return NULL;
}

int game_times_asked(const game *g, u32 conversation)
{
    int i;
    for (i = 0; i < g->asked_count; i++)
        if (g->asked[i].conversation == conversation)
            return g->asked[i].count;
    return 0;
}

int game_choices(game *g, u32 *ids, int max)
{
    vc_char_view c;
    u32 all[64];
    int n, i, k = 0;
    if (!g->talking || vc_read_char_view(&g->db, g->talking, &c) != 0)
        return 0;
    n = vc_read_list(&g->db, c.conversations, all, 64);
    for (i = 0; i < n && k < max; i++)
        if (vc_class(&g->db, all[i]) == VC_CONVERSATION && vc_is_enabled(&g->interp, all[i]))
            ids[k++] = all[i];
    /* VCIdeaMap: an idea icon and a conversation. Once the player has the
     * idea (its icon is enabled), the character can be asked about it. */
    n = vc_read_list(&g->db, c.idea_maps, all, 64);
    for (i = 0; i < n && k < max; i++) {
        xf_neo_reader r;
        u32 idea, conv;
        if (vc_class(&g->db, all[i]) != VC_IDEA_MAP || xf_neo_object(&g->db, all[i], &r, NULL, NULL) != 0)
            continue;
        idea = xf_neo_u32(&r);
        conv = xf_neo_u32(&r);
        if (idea && vc_is_enabled(&g->interp, idea) && vc_class(&g->db, conv) == VC_CONVERSATION &&
            vc_is_enabled(&g->interp, conv))
            ids[k++] = conv;
    }
    return k;
}

/* The idea icons of the character talked to: its VCIdeaMaps whose idea
 * (a VCIdeaIcon) and conversation are enabled and not asked yet. The
 * Help screen: "Clicking on a character may also bring up any available
 * Idea Icons (found at the top of the video window)"; clicking or
 * dragging one onto the character asks about it. */
int game_ideas(game *g, u32 *icons, u32 *conversations, int max)
{
    vc_char_view c;
    u32 all[64];
    int n, i, k = 0;
    if (!g->talking || vc_read_char_view(&g->db, g->talking, &c) != 0)
        return 0;
    n = vc_read_list(&g->db, c.idea_maps, all, 64);
    for (i = 0; i < n && k < max; i++) {
        xf_neo_reader r;
        u32 idea, conv;
        if (vc_class(&g->db, all[i]) != VC_IDEA_MAP || xf_neo_object(&g->db, all[i], &r, NULL, NULL) != 0)
            continue;
        idea = xf_neo_u32(&r);
        conv = xf_neo_u32(&r);
        if (idea && vc_is_enabled(&g->interp, idea) && vc_class(&g->db, conv) == VC_CONVERSATION &&
            vc_is_enabled(&g->interp, conv) && game_times_asked(g, conv) == 0) {
            icons[k] = idea;
            conversations[k++] = conv;
        }
    }
    return k;
}

void game_end_talk(game *g) { g->talking = 0; }

static void queue_list_media(game *g, u32 list, u32 conversation)
{
    u32 asset = first_enabled(g, list);
    if (asset) {
        game_queue_media(g, asset);
        g->queue[(g->queue_tail + GAME_MEDIA_QUEUE - 1) % GAME_MEDIA_QUEUE].conversation = conversation;
    }
}

/* 0x4ED930 records the question in the conversation history (or counts
 * it again), then 0x4D9890 plays the question and the answer, or the
 * repeat answer when it was asked before. The trigger list runs kind 0
 * on asking, kind 8 when it starts and kind 9 when the answer ends. */
int game_ask(game *g, u32 conversation)
{
    vc_conversation c;
    int i, repeat;
    if (vc_read_conversation(&g->db, conversation, &c) != 0)
        return 0;
    for (i = 0; i < g->asked_count && g->asked[i].conversation != conversation; i++)
        ;
    if (i == g->asked_count && g->asked_count < GAME_MAX_ASKED)
        g->asked[g->asked_count++].conversation = conversation;
    if (i < g->asked_count)
        g->asked[i].count++;
    repeat = game_times_asked(g, conversation) > 1;
    {
        char name[128] = "";
        vc_read_name(&g->db, c.name, name, sizeof(name));
        xf_info("ask %u: %s%s", conversation, name, repeat ? " (again)" : "");
    }
    /* Willmore asking comes first, then what the question's triggers
     * queue: asking the Comity Inn clerk for the rooms (22650) queues her
     * "Here we go" at the room (19937) and goes there */
    if (c.question)
        queue_list_media(g, c.question, 0);
    vc_run_triggers(&g->interp, c.triggers, 0);
    vc_run_triggers(&g->interp, c.triggers, 8);
    queue_list_media(g, repeat && c.repeat ? c.repeat : c.answer, conversation);
    after_triggers(g);
    return 1;
}

/* --- Clicks ---------------------------------------------------------- */

/* 1 when a trigger list has triggers of this kind. */
static int has_kind(game *g, u32 list, int kind)
{
    u32 ids[64];
    int n = vc_read_list(&g->db, list, ids, 64), i;
    for (i = 0; i < n; i++) {
        xf_neo_reader r;
        if (vc_class(&g->db, ids[i]) == VC_TRIGGER && xf_neo_object(&g->db, ids[i], &r, NULL, NULL) == 0) {
            xf_neo_u32(&r);
            if (xf_neo_u8(&r) == kind)
                return 1;
        }
    }
    return 0;
}

static void click_iface_item(game *g, const game_iface_item *it)
{
    u32 triggers = it->item.triggers;
    xf_info("click interface item %u", it->id);
    if (has_kind(g, triggers, 3))
        g->focus_item = it->id; /* a text field */
    g->trigger_item = it->id;
    g->iface_hover = 0;
    vc_run_triggers(&g->interp, triggers, 0);
    after_triggers(g);
}

static void click_spot_how(game *g, const game_spot *s, int aiming)
{
    u32 from = g->view, target = s->kind == SPOT_NAV && !aiming ? s->target : 0, triggers = s->triggers;
    u32 epoch = g->view_epoch;
    xf_info("click %s %u", s->kind == SPOT_NAV ? "move" : s->kind == SPOT_EXPLORE ? "object" : "character", s->id);
    g->talking = 0;
    vc_run_triggers(&g->interp, triggers, 0);
    if (g->view_epoch != epoch) /* the click went to another view */
        target = 0;
    else if (s->kind == SPOT_CHARACTER && hand_empty(g) && !game_media_pending(g)) {
        /* Empty hand: talk. Items in hand run the character's triggers. */
        u32 ids[GAME_MAX_CHOICES];
        vc_char_view cv;
        g->talking = s->id;
        /* The character's own media list holds what they say first, such
         * as a clerk's "Can I help you?"; the first enabled entry plays
         * before the questions. */
        if (vc_read_char_view(&g->db, s->id, &cv) == 0 && cv.assets) {
            u32 greet = first_enabled(g, cv.assets);
            if (greet)
                game_queue_media(g, greet);
        }
        xf_info("talk to %u: %d questions", s->id, game_choices(g, ids, GAME_MAX_CHOICES));
    }
    if (target) {
        if (game_enter_view(g, target) == 0 && g->history_len < GAME_HISTORY)
            g->history[g->history_len++] = from;
    } else {
        after_triggers(g);
    }
}

static void click_spot(game *g, const game_spot *s) { click_spot_how(g, s, 0); }

int game_click_aiming(game *g, int x, int y)
{
    const game_spot *s;
    if (game_iface_item_at(g, x, y) || game_scene_held(g))
        return game_click(g, x, y);
    if (!(s = game_spot_at(g, x, y)))
        return 0;
    {
        game_spot copy = *s;
        click_spot_how(g, &copy, 1);
    }
    return 1;
}

static const game_iface_item *find_item(game *g, u32 id)
{
    int k, i;
    for (k = g->iface_count - 1; k >= 0; k--)
        for (i = 0; i < g->ifaces[k].count; i++)
            if (g->ifaces[k].items[i].id == id)
                return &g->ifaces[k].items[i];
    return NULL;
}

const game_iface_item *game_find_item(game *g, u32 id) { return find_item(g, id); }

int game_ai_glowing(game *g)
{
    return g->ai_list && (g->ai_glow || vc_list_ready(&g->interp, g->ai_trigger, g->ai_list));
}

int game_key(game *g, int key)
{
    const game_iface_item *it = g->focus_item ? find_item(g, g->focus_item) : NULL;
    i32 saved = g->interp.context;
    int k, i;
    if (!it) {
        /* No focus yet: the first item of the top layouts that takes keys. */
        g->focus_item = 0;
        for (k = g->iface_count - 1; k >= 0 && !it; k--)
            for (i = 0; i < g->ifaces[k].count && !it; i++)
                if (has_kind(g, g->ifaces[k].items[i].item.triggers, 3))
                    it = &g->ifaces[k].items[i];
        if (!it)
            return 0;
        g->focus_item = it->id;
    }
    g->interp.context = key;
    vc_run_triggers(&g->interp, it->item.triggers, 3);
    g->interp.context = saved;
    after_triggers(g);
    return 1;
}

/* A layout that is not part of the view (SetIFLAsView) holds the
 * scene: the computer, a close-up, the PDA. */
int game_scene_held(const game *g)
{
    int k;
    for (k = 0; k < g->iface_count; k++)
        if (!g->ifaces[k].as_view)
            return 1;
    return 0;
}

int game_click(game *g, int x, int y)
{
    const game_iface_item *it = game_iface_item_at(g, x, y);
    if (it) {
        game_iface_item copy = *it;
        click_iface_item(g, &copy);
        return 1;
    }
    if (game_scene_held(g))
        return 0; /* the scene behind takes no clicks */
    {
        const game_spot *s = game_spot_at(g, x, y);
        if (!s)
            return 0;
        click_spot(g, s);
        return 1;
    }
}

int game_click_object(game *g, u32 id)
{
    int i, k;
    for (k = g->iface_count - 1; k >= 0; k--)
        for (i = 0; i < g->ifaces[k].count; i++)
            if (g->ifaces[k].items[i].id == id) {
                game_iface_item it = g->ifaces[k].items[i];
                click_iface_item(g, &it);
                return 1;
            }
    for (i = 0; i < g->spot_count; i++)
        if (g->spots[i].id == id || (g->spots[i].kind == SPOT_NAV && g->spots[i].target == id)) {
            game_spot s = g->spots[i];
            click_spot(g, &s);
            return 1;
        }
    return 0;
}

/* --- Gunfights -------------------------------------------------------
 * A clip can have a HOT file of moving rectangles (docs/formats/hot.md).
 * The engine knows the rectangle IDs: 0x4F4510 turns a click on one into
 * a shot number, most only with the gun in hand, and 0x4F4AC0 plays the
 * clip of that shot. After the clip the scene goes on in a fixed view,
 * starts the view again (the shot missed or only wounded), or leaves it
 * to the clip's own end triggers (a goon is down). */

#define SHOT_ANY 0xFF  /* any item in hand, or none */

/* 0x4F4510: rectangle ID -> shot number with the gun in hand, and with
 * X's stiletto in hand (0: no shot). Mode 7 is the gun, 0x17 the
 * stiletto. */
static const struct {
    u16 hot;
    u8 gun, stiletto;
} g_hot_shots[] = {
    { 100, 27, 0 }, { 101, 1, 0 },  { 102, 2, 0 },  { 103, 1, 0 },  { 104, 2, 0 },
    { 111, 4, 0 },  { 112, 5, 0 },  { 113, 4, 0 },  { 114, 5, 0 },  { 121, 4, 0 },
    { 122, 5, 0 },  { 131, 4, 0 },  { 132, 5, 0 },  { 141, 4, 0 },  { 142, 5, 0 },
    { 201, 9, 0 },  { 211, 10, 0 }, { 221, 6, 0 },  { 222, 7, 0 },  { 223, 8, 0 },
    { 401, 11, 0 }, { 411, 12, 0 }, { 421, 13, 0 }, { 431, 14, 0 }, { 432, 15, 0 },
    { 433, 16, 17 }, { 434, 23, 0 }, { 441, SHOT_ANY, 18 }, { 442, 19, 0 },
    { 451, SHOT_ANY, 20 }, { 461, 21, 21 }, { 462, 24, 22 }, { 463, SHOT_ANY, 25 },
    { 470, 26, 26 }, { 500, 27, 0 }, { 600, 28, 0 },
};

/* 0x4F4AC0: shot number -> clip, the view that follows it (0: none;
 * GAME_SHOT_RESTART: the current view starts again) and the letter put in
 * the RegViewState variable (var 1495), which the scene reads when the
 * clip ends: 'r' a hit. Shots 20, 26 and 27 are in game_shoot; shot 3
 * has no rectangle; 1, 2, 4 and 5 also count hits. */
#define GAME_SHOT_RESTART 0xFFFFFFFFu
static const struct {
    u8 shot;
    char state;
    u32 media, view;
} g_shots[] = {
    { 0, 0, 21672, 0 },      /* Rauch */
    { 1, 0, 21664, GAME_SHOT_RESTART },
    { 2, 0, 21662, GAME_SHOT_RESTART },
    { 4, 0, 21718, GAME_SHOT_RESTART }, /* the woods */
    { 5, 0, 21716, GAME_SHOT_RESTART },
    { 6, 0, 21230, 0 },      /* Smolnikoff: a mobster gets shot, young */
    { 7, 0, 21232, 0 },      /* old */
    { 8, 0, 21234, 0 },      /* middle-aged */
    { 9, 'r', 28456, GAME_SHOT_RESTART }, /* the gunman on the stairs falls */
    { 10, 'r', 21238, 5501 }, /* the mobster by the staircase */
    { 11, 'r', 21812, GAME_SHOT_RESTART }, /* the secret base */
    { 12, 'r', 21818, GAME_SHOT_RESTART },
    { 13, 'r', 21808, GAME_SHOT_RESTART },
    { 14, 'r', 44483, 44283 },
    { 15, 'r', 49014, 14519 },
    { 16, 'z', 80090, 0 },
    { 17, 'z', 49013, 0 },
    { 18, 'z', 21782, 0 },
    { 19, 'r', 21784, 0 },
    { 21, 0, 80090, 0 },
    { 22, 0, 23434, 0 },
    { 23, 'w', 49022, 49374 },
    { 24, 'd', 49022, 49374 },
    { 25, 0, 23430, 0 },
    { 28, 0, 19656, 0 },
};

static int holding(game *g, const char *item)
{
    char name[48];
    u32 id;
    snprintf(name, sizeof(name), "Inventory_i%s", item);
    id = vc_find_var(&g->interp, name);
    return id && !hand_empty(g) && game_reg_value(g, "RegCurrInvVar", 0) == vc_get_var(&g->interp, id);
}

/* The Smolnikoff warehouse fight has three areas, each with three clips
 * of a goon (0x657900). InitSmShootout (0x46AE30) takes a variable per
 * area that tells the scripts the area is clear. */
static const u32 g_smol_views[3][3] = {
    { 62256, 62258, 62260 },
    { 62262, 62264, 62266 },
    { 62268, 62270, 62272 },
};

/* 0x4B0650 DoSmolShootout: a random area that is not clear yet (the next
 * one when the pick is clear), its next clip; the first floor (view
 * 5208) once all three are clear. */
static void smol_next(game *g)
{
    int a, k;
    if (g->smol_clear[0] && g->smol_clear[1] && g->smol_clear[2]) {
        g->smol_area = 0;
        host_go_to_view(g, 5208);
        return;
    }
    g->rand_seed = g->rand_seed * 1103515245u + 12345u;
    a = (int)((g->rand_seed >> 16) % 3);
    for (k = 0; k < 3 && g->smol_clear[a]; k++)
        a = (a + 1) % 3;
    g->smol_area = a + 1;
    host_go_to_view(g, g_smol_views[a][g->smol_visits[a]]);
    if (g->smol_visits[a] < 2)
        g->smol_visits[a]++;
}

/* 0x4B07E0 SmIsWillShot1: the goon fires first. The chance grows with
 * each try in the area and with the difficulty (var991: 'E'asy,
 * 'S'tandard, 'H'ard); the third try always hits. Willmore's death
 * clip 21168 plays and timer 10 stops. */
static void smol_will_shot(game *g, u8 timer)
{
    static const int chance[2][3] = { { 20, 40, 60 }, { 50, 65, 80 } };
    int a = g->smol_area - 1, d, tries, hit;
    char level;
    if (a < 0)
        return;
    tries = g->smol_visits[a];
    level = (char)vc_get_var(&g->interp, 991);
    d = level == 'E' ? 0 : level == 'H' ? 2 : 1;
    g->rand_seed = g->rand_seed * 1103515245u + 12345u;
    hit = tries >= 3 || (tries >= 1 && (int)((g->rand_seed >> 16) % 100) < chance[tries - 1][d]);
    if (!hit)
        return;
    xf_info("the goon shoots Willmore");
    host_timer(g, timer, 0, 1);
    game_queue_media(g, 21168);
}

/* The HOT file beside a clip: XV/19672.xmv -> XV/19672.hot. */
static int load_hot(game *g, u32 asset, xf_hot *hot)
{
    vc_asset_ref a;
    int i;
    if (vc_read_asset_ref(&g->db, asset, &a) != 0)
        return -1;
    for (i = 0; i < a.entry_count; i++) {
        char name[512], path[1024], *dot;
        snprintf(name, sizeof(name), "%s", a.entries[i].path);
        if (!(dot = strrchr(name, '.')))
            continue;
        snprintf(dot, sizeof(name) - (size_t)(dot - name), ".hot");
        if (game_resolve(g, name, path, sizeof(path)) == 0 && xf_hot_load(hot, path) == 0)
            return 0;
    }
    return -1;
}

/* The rectangle IDs in the HOT file of the view's clip: 0 when the clip
 * has none, else the first ID; *has is set when `id` is among them. */
static u32 view_hot(game *g, u32 id, int *has)
{
    xf_hot hot;
    u32 i, first = 0;
    u32 clip = g->current.asset ? g->current.asset :
               game_media_pending(g) ? g->queue[g->queue_head].asset : g->view_asset;
    *has = 0;
    if (!clip || load_hot(g, clip, &hot) != 0)
        return 0;
    first = hot.count ? hot.spots[0].id : 0;
    for (i = 0; i < hot.count; i++)
        if (hot.spots[i].id == id)
            *has = 1;
    xf_hot_free(&hot);
    return first;
}

int game_view_has_hot(game *g, u32 hot)
{
    int has;
    view_hot(g, hot, &has);
    return has;
}

int game_shoot(game *g, u32 hot)
{
    size_t i, k;
    int has;
    u32 first = view_hot(g, hot, &has);
    if (!hot)
        hot = first; /* the clip's own target */
    else if (first && !has)
        return 0;
    for (i = 0; i < XF_COUNT(g_hot_shots); i++) {
        u8 shot;
        if (g_hot_shots[i].hot != hot)
            continue;
        if (g_hot_shots[i].gun == SHOT_ANY) /* 441, 451, 463: any hand */
            shot = g_hot_shots[i].stiletto;
        else if (holding(g, "Gun"))
            shot = g_hot_shots[i].gun;
        else if (holding(g, "Stiletto"))
            shot = g_hot_shots[i].stiletto;
        else
            shot = 0;
        if (!shot)
            return 0;
        if (shot == 20 || shot == 26) {
            /* 0x4F4F9C, 0x4F501C: no clip of their own, no restart; the
             * scene reads RegViewState: 't' (shot 20), or 'b' (shot 26),
             * where the clip playing ends with its end triggers
             * (0x4F4050; our reading of it). */
            set_reg_var(g, "RegViewState", 0, shot == 20 ? 't' : 'b');
            xf_info("shot %u", shot);
            if (shot == 20) {
                g->current.asset = 0;
                g->current.conversation = 0;
                g->queue_head = g->queue_tail;
            } else if (g->current.asset)
                game_media_end(g);
            return 1;
        }
        if (shot == 27) {
            /* 0x4F5038: Willmore turns the gun on himself. RegViewState
             * 'S', and one of two suicide note screens (layouts 62847
             * and 62849, table 0x65BE88); a click on it ends the game. */
            static const u32 notes[2] = { 62847, 62849 };
            g->current.asset = 0;
            g->current.conversation = 0;
            g->queue_head = g->queue_tail;
            set_reg_var(g, "RegViewState", 0, 'S');
            g->rand_seed = g->rand_seed * 1103515245u + 12345u;
            host_interface(g, notes[(g->rand_seed >> 16) % 2], 0);
            xf_info("shot 27: suicide note");
            return 1;
        }
        for (k = 0; k < XF_COUNT(g_shots); k++)
            if (g_shots[k].shot == shot) {
                xf_info("shot %u: clip %u", g_shots[k].shot, g_shots[k].media);
                /* 0x4F4B84: the clip being played stops at once, without
                 * its end triggers (Cook stops strangling Willmore). */
                g->current.asset = 0;
                g->current.conversation = 0;
                g->queue_head = g->queue_tail;
                if (g_shots[k].state) /* 0x50D0E0 */
                    set_reg_var(g, "RegViewState", 0, g_shots[k].state);
                g->shot_media = g_shots[k].media;
                g->shot_view = g_shots[k].view == GAME_SHOT_RESTART ? g->view : g_shots[k].view;
                g->shot_epoch = g->view_epoch;
                game_queue_media(g, g_shots[k].media);
                if (g_shots[k].shot == 1 || g_shots[k].shot == 2) {
                    /* Rauch's porch: two NSA agents (0x4F5720 counts who
                     * is left). With both down the view is not started
                     * again: RegViewState 'a' tells its leave triggers,
                     * and the porch (view 8138) follows (0x4F3C60). */
                    g->rauch_down |= g_shots[k].shot;
                    if (g->rauch_down == 3) {
                        set_reg_var(g, "RegViewState", 0, 'a');
                        g->shot_view = 8138;
                    }
                }
                if (g_shots[k].shot == 4 || g_shots[k].shot == 5) {
                    /* The woods: two NSA agents (0x4F5A80 counts who is
                     * left); with both down Sc114L plays and the chase
                     * ends. */
                    g->woods_down |= g_shots[k].shot == 4 ? 1 : 2;
                    set_reg_var(g, "RegViewState", 0, g_shots[k].shot == 4 ? 't' : 'd');
                    if (g->woods_down == 3) {
                        g->shot_view = 0;
                        game_queue_media(g, 38448);
                    }
                }
                return 1;
            }
        xf_info("shot %u: not implemented", shot);
        return 0;
    }
    return 0;
}

u32 game_hot_at(game *g, u32 asset, int x, int y, u32 ms)
{
    xf_hot hot;
    u32 i, id = 0;
    if (load_hot(g, asset, &hot) != 0)
        return 0;
    for (i = 0; i < hot.count && !id; i++) {
        const xf_hotspot *h = &hot.spots[i];
        if (x >= h->left && x < h->right && y >= h->top && y < h->bottom && ms >= h->start && ms < h->end)
            id = h->id;
    }
    xf_hot_free(&hot);
    return id;
}

/* --- Cell phone ------------------------------------------------------ */

/* 0x65ECB0: the phone directory of the engine, {number, trigger, action
 * list}. The number is a VCAssetRef in category 72 ("Phone Number:
 * ...") whose path holds the digits; the action list plays the call. */
static const u32 g_phone_book[][3] = {
    { 67269, 67324, 67326 }, /* Amis */
    { 67271, 67331, 67333 }, /* Arley */
    { 67273, 67338, 67340 }, /* Asta */
    { 67275, 67345, 67347 }, /* Truit */
    { 67277, 67352, 67354 }, /* diner */
    { 67279, 67361, 67363 }, /* Cook */
    { 67281, 67368, 67370 }, /* Skinner in DC */
    { 67283, 67375, 67377 }, /* Shanks */
    { 67287, 67382, 67384 }, /* Willmore */
    { 67285, 67389, 67391 }, /* crime lab */
    { 67289, 67398, 67400 }, /* Barbara */
    { 67443, 74576, 74580 }, /* Lone Gunmen */
    { 83868, 83863, 83865 }, /* McGaffey */
};

/* The digits of a phone number media reference. */
static void phone_number(game *g, u32 asset, char *out, size_t size)
{
    vc_asset_ref a;
    out[0] = '\0';
    if (vc_read_asset_ref(&g->db, asset, &a) == 0)
        snprintf(out, size, "%s", a.logical);
}

/* A number matches with or without the long-distance 1 and, for local
 * Seattle numbers, without the area code. */
static int number_matches(const char *typed, const char *number)
{
    size_t lt = strlen(typed), ln = strlen(number);
    if (lt == 0 || lt > ln)
        return 0;
    return strcmp(typed, number + (ln - lt)) == 0 && (lt == ln || lt == 10 || lt == 7);
}

/* 0x51FCD0: looks the typed number up. On a match the trigger and the
 * action list of the call go into two variables, which the data then
 * runs with DoActionListID. */
/* The phone's display (item 39513): the number typed, or a name. */
static void phone_show(game *g, const char *text)
{
    game_item_label *l;
    set_item_text(g, 39513, text);
    if ((l = (game_item_label *)game_item_label_of(g, 39513))) {
        l->font = 'P'; /* Phn.ttr */
        l->size = 14;
        l->align = 'c';
    }
}

/* 0x51F960: a digit goes on the number; after a name from the address
 * book the next digit starts a new number (flag 0x6B6BC0). */
static void phone_key(game *g, char key)
{
    size_t n = strlen(g->phone);
    if (key < '0' || key > '9')
        return;
    if (g->phone_replace) {
        g->phone_replace = 0;
        n = 0;
    } else if (n >= 13)
        return;
    g->phone[n] = key;
    g->phone[n + 1] = '\0';
    phone_show(g, g->phone);
}

/* 0x51F640: an entry of the address book is "NAME\nNUMBER" (the name can
 * take two lines); the display shows the name and the number is ready to
 * send. step moves to the next (1) or previous (-1) entry. */
static void phone_book_show(game *g, int step)
{
    vc_iface_item it;
    u32 ids[32], on[32];
    int n, k = 0, i;
    char text[256], name[256], *nl;
    if (vc_read_iface_item(&g->db, 45309, &it) != 0)
        return;
    n = vc_read_list(&g->db, it.assets, ids, 32);
    for (i = 0; i < n; i++)
        if (vc_is_enabled(&g->interp, ids[i]))
            on[k++] = ids[i];
    if (!k) {
        g->phone[0] = '\0';
        phone_show(g, "");
        return;
    }
    g->phone_entry = ((g->phone_entry + step) % k + k) % k;
    game_text(g, on[g->phone_entry], text, sizeof(text));
    nl = strrchr(text, '\n');
    snprintf(name, sizeof(name), "%.*s", nl ? (int)(nl - text) : (int)strlen(text), text);
    snprintf(g->phone, sizeof(g->phone), "%s", nl ? nl + 1 : "");
    g->phone_replace = 1;
    phone_show(g, name);
}

static int phone_send(game *g, u32 trigger_var, u32 list_var)
{
    size_t i, n = strlen(g->phone);
    /* 0x51FCD0 first: more than 11 digits rings on; ten digits outside
     * Seattle's 206 need the 1 of a long-distance call (lists 83303 and
     * 83305 say so). Both answer -1: the call is handled. */
    if (n > 11) {
        run_trigger_list_pair(g, 67415, 67422);
        return -1;
    }
    if (n == 10 && strncmp(g->phone, "206", 3) != 0) {
        xf_info("phone: %s needs the 1 of a long-distance call", g->phone);
        run_trigger_list_pair(g, 83303, 83305);
        return -1;
    }
    for (i = 0; i < XF_COUNT(g_phone_book); i++) {
        char number[32];
        phone_number(g, g_phone_book[i][0], number, sizeof(number));
        if (number_matches(g->phone, number)) {
            xf_info("phone: %s answers", g->phone);
            if (trigger_var)
                vc_set_var(&g->interp, trigger_var, (i32)g_phone_book[i][1]);
            if (list_var)
                vc_set_var(&g->interp, list_var, (i32)g_phone_book[i][2]);
            return 1;
        }
    }
    xf_info("phone: %s does not answer", g->phone);
    return 0;
}

static void run_trigger_list_pair(game *g, i32 trigger, i32 list)
{
    xf_neo_reader r;
    u32 l = (u32)list;
    if (!l && vc_class(&g->db, (u32)trigger) == VC_TRIGGER && xf_neo_object(&g->db, (u32)trigger, &r, NULL, NULL) == 0)
        l = xf_neo_u32(&r);
    vc_run_action_list(&g->interp, l);
}

/* The phone screen calls PhoneSend(number, var4467, var4468) and then
 * DoActionListID(var4467, var4468) (English HDB). game_dial does both. */
int game_dial(game *g, const char *number)
{
    int ok;
    snprintf(g->phone, sizeof(g->phone), "%s", number);
    ok = phone_send(g, 4467, 4468) == 1;
    if (ok)
        run_trigger_list_pair(g, vc_get_var(&g->interp, 4467), vc_get_var(&g->interp, 4468));
    g->phone[0] = '\0';
    after_triggers(g);
    return ok;
}

/* --- ING database ---------------------------------------------------- */

/* 0x65E0F0: phone numbers the ING knows, {number media, result}. A
 * result of 0 or more shows that person's record; -1 is the warehouse
 * the agents called, -2 McGaffey. */
static const struct {
    u32 number;
    i32 result;
} g_ing_phones[] = {
    { 67269, 0 }, { 67271, 1 }, { 67273, 2 }, { 67275, 10 }, { 67279, 3 },  { 67281, 17 },
    { 67283, 7 }, { 67287, 12 }, { 89560, 15 }, { 89562, 16 }, { 67441, -1 }, { 83868, -2 },
};

/* 0x5151A0: a phone search. Spaces, dashes and brackets are ignored and
 * the number must have 10 digits. */
static int ing_phone(game *g, const char *text)
{
    char digits[32];
    size_t i, k = 0;
    for (i = 0; text[i] && k + 1 < sizeof(digits); i++)
        if (!strchr(" -()", text[i]))
            digits[k++] = text[i];
    digits[k] = '\0';
    if (k != 10)
        return 0;
    for (i = 0; i < XF_COUNT(g_ing_phones); i++) {
        char number[32];
        phone_number(g, g_ing_phones[i].number, number, sizeof(number));
        k = strlen(number);
        if (k < 10 || strcmp(number + k - 10, digits) != 0)
            continue;
        xf_info("ING: %s found (record %d)", digits, g_ing_phones[i].result);
        if (g_ing_phones[i].result == -1) {
            /* The Dockside Warehouse: the engine sets variable 5051 and
             * runs trigger 75766, list 75767 (it goes on the PDA). */
            vc_set_var(&g->interp, 5051, 1);
            vc_run_action_list(&g->interp, 75767);
        }
        return 1;
    }
    return 0;
}

/* 0x65E1A0: the ING name records, 60 entries of {key item (media: the
 * databases, then the names it answers to), record item (media: the 8
 * record fields), mug shot item, fingerprint item, field notes media}. */
static const struct { u32 key, record, mug, prints, notes; } g_ing_names[] = {
    { 60627, 79934, 15286, 15124, 0 },
    { 60628, 79936, 17337, 15122, 0 },
    { 60629, 79937, 17339, 15124, 81304 },
    { 60630, 79938, 17341, 15122, 81305 },
    { 60631, 79939, 17343, 15124, 0 },
    { 60632, 79940, 17345, 15122, 0 },
    { 60633, 79941, 17347, 15124, 81321 },
    { 60634, 79942, 17349, 15122, 81308 },
    { 60635, 79943, 17351, 15124, 81320 },
    { 60636, 79944, 17353, 15122, 0 },
    { 60637, 79945, 17355, 15124, 0 },
    { 60638, 79946, 17357, 15122, 0 },
    { 60639, 79947, 17359, 15124, 81310 },
    { 60640, 79948, 17361, 15122, 81311 },
    { 60641, 79949, 17363, 15124, 0 },
    { 60642, 79950, 50591, 15124, 81306 },
    { 60643, 79951, 50589, 15122, 81307 },
    { 60644, 79952, 50587, 15124, 81309 },
    { 80008, 79994, 79806, 15122, 69917 },
    { 88599, 88600, 79818, 15124, 88643 },
    { 89079, 89080, 79830, 15122, 89077 },
    { 80011, 79972, 79810, 15124, 70043 },
    { 80025, 79986, 79840, 15124, 70007 },
    { 80015, 79976, 79822, 15124, 82206 },
    { 80019, 79980, 79836, 15122, 70061 },
    { 80009, 79970, 80362, 15124, 69935 },
    { 80027, 79988, 79844, 15122, 70099 },
    { 80010, 79971, 79846, 15122, 69953 },
    { 80013, 79974, 79814, 15122, 69971 },
    { 80014, 79975, 79816, 15124, 69989 },
    { 80016, 79977, 79820, 15122, 70117 },
    { 80018, 79979, 79826, 15124, 70025 },
    { 80022, 79983, 79832, 15124, 70080 },
    { 80024, 79985, 79838, 15122, 70135 },
    { 88597, 88598, 79828, 15122, 88637 },
    { 89596, 89597, 79824, 15124, 89578 },
    { 89598, 89599, 79848, 15122, 89594 },
    { 80017, 79978, 80366, 15122, 86076 },
    { 80020, 79981, 79852, 15124, 86106 },
    { 80021, 79982, 79933, 15122, 86094 },
    { 80023, 79984, 79919, 15124, 86096 },
    { 80026, 79987, 79896, 15122, 86090 },
    { 80030, 79993, 79929, 15124, 86112 },
    { 80031, 79991, 79911, 15122, 86098 },
    { 80032, 79992, 79892, 15124, 86100 },
    { 87503, 87504, 79915, 15122, 86078 },
    { 87505, 87506, 79900, 15124, 86092 },
    { 87507, 87508, 79894, 15122, 86086 },
    { 87515, 87516, 79906, 15124, 86080 },
    { 87517, 87518, 79925, 15124, 86088 },
    { 87523, 87524, 79898, 15122, 86082 },
    { 87525, 87526, 79902, 15124, 86108 },
    { 87527, 87528, 79913, 15122, 86084 },
    { 87535, 87536, 79927, 15124, 86102 },
    { 87537, 87538, 79921, 15122, 86074 },
    { 87539, 87540, 79904, 15124, 86072 },
    { 87541, 87542, 79917, 15122, 86110 },
    { 87543, 87544, 79931, 15124, 86104 },
    { 90721, 90722, 80368, 15122, 90727 },
    { 91148, 91149, 79923, 15124, 91154 },
};
/* 0x65E160 and 0x65E180: the ING record's 8 field labels and fields. */
static const u32 g_ing_labels[8] = { 35551, 35553, 35554, 35555, 35556, 35557, 35558, 35559 };
static const u32 g_ing_fields[8] = { 35560, 35562, 35563, 35564, 35565, 35566, 35567, 35568 };

/* Puts a text on a layout item, as SetIItemText does, in the record's
 * Courier New. */
static void set_item_text(game *g, u32 item, const char *text)
{
    game_item_label *l = (game_item_label *)game_item_label_of(g, item);
    if (!l && g->label_count < GAME_MAX_LABELS)
        l = &g->labels[g->label_count++];
    if (!l)
        return;
    memset(l, 0, sizeof(*l));
    l->item = item;
    snprintf(l->text, sizeof(l->text), "%s", text);
    l->color = 'k';
    l->font = 'c';
    l->size = 10;
    l->bold = 'n';
    l->align = 'l';
}

/* Lower case, without spaces, dashes and brackets: how the ING compares
 * what the player typed. */
static void ing_key(const char *text, char *out, size_t size)
{
    char folded[256];
    const char *in = folded;
    size_t k = 0;
    xf_fold_ascii(folded, sizeof(folded), text); /* a translation's accents */
    for (; *in && k + 1 < size; in++)
        if (!strchr(" -()", *in))
            out[k++] = (char)tolower((unsigned char)*in);
    out[k] = '\0';
}

/* The media list of a layout item. */
static int item_media(game *g, u32 item, u32 *ids, int max)
{
    vc_iface_item it;
    if (vc_read_iface_item(&g->db, item, &it) != 0)
        return 0;
    return vc_read_list(&g->db, it.assets, ids, max);
}

/* 0x4CA010 (node 2) .. 0x4CA2B0 (node 6). Node 2: Cook's talk is over
 * and 3 pieces of evidence went to the lab. Node 3: the coroner's office
 * is done, Cook's field office scene too (variable 5122), and 3 pieces of
 * evidence were processed, or 2 when Willmore has not translated the
 * Cyrillic document. Nodes 4 and 6: one scene is finished. Node 5: two. */
static void set_node_done(game *g, int node)
{
    char reg[16];
    i32 cur = game_reg_value(g, "RegCurrNode", 0);
    int done = 0, last;
    snprintf(reg, sizeof(reg), "RegNode%dVars", node);
#define V(i) game_reg_value(g, reg, i)
    switch (node) {
    case 2:
        done = cur == 2 && V(0) == 1 && V(1) >= 3;
        last = 2;
        break;
    case 3:
        done = cur == 3 && vc_get_var(&g->interp, 5122) == 1 && V(0) == 1 &&
               ((V(2) == 1 && V(1) >= 3) || (V(2) == 0 && V(1) >= 2));
        last = 3;
        break;
    case 4:
        done = cur == 4 && V(0) == 1;
        last = 1;
        break;
    case 5:
        done = cur == 5 && V(0) == 1 && V(1) == 1;
        last = 2;
        break;
    case 6:
        done = cur == 6 && V(0) == 1;
        last = 1;
        break;
    default:
        return;
    }
#undef V
    set_reg_var(g, reg, last, done);
}


/* --- E-mail ------------------------------------------------------------
 * SendEmail(media) puts a text file (Text/Emails/...) in the mailbox
 * (0x484690 -> 0x483F00 makes a VCEmail). The mail screens show 8 rows:
 * a first line with the sender, a second with the subject, and an icon
 * for an unread mail (0x48BE50). */

/* Rows of the computer's inbox (layout 15171): sender line, subject line
 * and unread icon. */
static const u32 g_email_from[GAME_EMAIL_ROWS] = { 38282, 38283, 38284, 38285, 38286, 38287, 38288, 38289 };
static const u32 g_email_subject[GAME_EMAIL_ROWS] = { 42733, 42734, 42735, 42736, 42737, 42738, 42739, 42740 };
static const u32 g_email_icon[GAME_EMAIL_ROWS] = { 15293, 38253, 38255, 38257, 38259, 38261, 38263, 38265 };

/* The text of a document (a mail, a note, an article), as UTF-8. */
static size_t email_text(game *g, u32 asset, char *out, size_t size)
{
    vc_asset_ref a;
    char path[1024];
    FILE *f;
    size_t n = 0;
    int i;
    out[0] = '\0';
    if (vc_read_asset_ref(&g->db, asset, &a) != 0)
        return 0;
    for (i = 0; i < a.entry_count; i++)
        if (game_resolve(g, a.entries[i].path, path, sizeof(path)) == 0 && (f = fopen(path, "rb"))) {
            u8 *raw = malloc(size);
            n = raw ? fread(raw, 1, size - 1, f) : 0;
            fclose(f);
            /* as UTF-8: a translation's documents already are; the
             * Japanese release's are Shift-JIS, the others Windows-1252 */
            if (raw && xf_is_utf8((const char *)raw, n)) {
                memcpy(out, raw, n);
                out[n] = '\0';
            } else if (raw && xf_langs_path_sjis(&g->langs, path)) {
                n = xf_sjis_to_utf8(out, size, raw, n);
            } else if (raw) {
                n = xf_cp1252_to_utf8(out, size, raw, n);
            }
            free(raw);
            break;
        }
    out[n] = '\0';
    { /* CRLF and CR line ends become LF */
        size_t r, w = 0;
        for (r = 0; r < n; r++)
            if (out[r] != '\r')
                out[w++] = out[r];
            else if (r + 1 >= n || out[r + 1] != '\n')
                out[w++] = '\n';
        out[n = w] = '\0';
    }
    return n;
}

/* A header field ("From", "Subject") of a mail. Translations name the
 * fields in their language ("Von:", "Exp. :"), always in the order To,
 * From, Subject: the field's line is used when its English name is not
 * there. */
static void email_field(game *g, u32 asset, const char *name, char *out, size_t size)
{
    static const char *const order[] = { "To", "From", "Subject" };
    char text[1024];
    const char *p, *line[3] = { NULL, NULL, NULL };
    size_t k = strlen(name);
    int i, n_lines = 0;
    out[0] = '\0';
    email_text(g, asset, text, sizeof(text));
    for (p = text; *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : p + strlen(p)) {
        const char *colon = strchr(p, ':'), *wide = strstr(p, "\xEF\xBC\x9A"), *end = strchr(p, '\n');
        if (wide && (!colon || wide < colon))
            colon = wide + 2; /* the Japanese full-width colon: its last byte */
        if (strncmp(p, name, k) == 0 && p[k] == ':') {
            line[0] = p + k;
            n_lines = -1;
            break;
        }
        if (n_lines >= 0 && n_lines < 3 && colon && (!end || colon < end))
            line[n_lines++] = colon;
        else
            n_lines = n_lines < 3 ? -2 : n_lines; /* the header ended */
    }
    if (n_lines == 3) {
        for (i = 0; i < 3 && strcmp(order[i], name) != 0; i++)
            ;
        line[0] = i < 3 ? line[i] : NULL;
    } else if (n_lines != -1) {
        line[0] = NULL;
    }
    if (line[0]) {
        const char *v = line[0] + 1;
        size_t n = strcspn(v, "\n");
        while (*v == ' ' && n) {
            v++;
            n--;
        }
        snprintf(out, size, "%.*s", (int)n, v);
    }
}

static int email_find(game *g, u32 asset)
{
    int i;
    for (i = 0; i < g->email_count; i++)
        if (g->emails[i].asset == asset)
            return i;
    return -1;
}

/* 0x48BE50: the 8 rows from the scroll position. */
static void email_show_rows(game *g)
{
    int r;
    if (g->email_mode != 'C')
        return;
    for (r = 0; r < GAME_EMAIL_ROWS; r++) {
        int i = g->email_scroll + r;
        char from[128] = "", subject[160] = "";
        if (i < g->email_count) {
            email_field(g, g->emails[i].asset, "From", from, sizeof(from));
            email_field(g, g->emails[i].asset, "Subject", subject, sizeof(subject));
        }
        set_item_text(g, g_email_from[r], from);
        set_item_text(g, g_email_subject[r], subject);
        vc_set_enabled(&g->interp, g_email_icon[r], i < g->email_count && !g->emails[i].read);
    }
}

/* 0x48B7A0: scrolls so that the mail is in view; 0 shows the newest. */
static void email_display(game *g, u32 asset)
{
    int i = asset ? email_find(g, asset) : -1;
    if (i < 0)
        g->email_scroll = g->email_count > GAME_EMAIL_ROWS ? g->email_count - GAME_EMAIL_ROWS : 0;
    else if (i < g->email_scroll || i >= g->email_scroll + GAME_EMAIL_ROWS)
        g->email_scroll = i > GAME_EMAIL_ROWS - 1 ? i - (GAME_EMAIL_ROWS - 1) : 0;
    email_show_rows(g);
}

/* 0x43D9F0: shows a mail. Three mails carry an attachment; for them the
 * engine shows the attachment items and the text goes to a smaller area
 * (on the computer 72758, in the PDA 72761). Clicking that text (or the
 * attachment) runs the item's own triggers: the GPS coordinates from the
 * Lone Gunmen open the map of Alaska. */
static void email_read(game *g, u32 item, u32 asset)
{
    static const u32 computer[][3] = {
        { 61775, 72715, 72722 },  /* Amis: the Tarakan fingerprint file */
        { 64527, 91038, 91007 },  /* the coroner: the crew cut's prints */
        { 61765, 72723, 91939 },  /* the Lone Gunmen: GPS coordinates */
    };
    size_t i;
    int attachment = -1;
    for (i = 0; i < XF_COUNT(computer); i++)
        if (computer[i][0] == asset)
            attachment = (int)i;
    memset(&g->scroll, 0, sizeof(g->scroll));
    g->scroll.item = item;
    g->scroll.color = 'b';
    g->scroll.font = 'c';
    g->scroll.size = 12;
    email_text(g, asset, g->scroll.text, sizeof(g->scroll.text));
    if (attachment < 0)
        return;
    if (item == 54950) { /* the computer */
        vc_set_enabled(&g->interp, 72713, 1);
        vc_set_enabled(&g->interp, 72710, 1);
        vc_set_enabled(&g->interp, computer[attachment][1], 1);
        vc_set_enabled(&g->interp, computer[attachment][2], 1);
        if (asset != 64527)
            vc_set_enabled(&g->interp, 69527, 0); /* no Delete */
        g->scroll.item = 72758;
    } else { /* the PDA */
        vc_set_enabled(&g->interp, 72741, 1);
        vc_set_enabled(&g->interp, 72760, 1);
        if (asset == 61765) {
            vc_set_enabled(&g->interp, 72743, 1);
            vc_set_enabled(&g->interp, 72759, 1);
        } else {
            vc_set_enabled(&g->interp, 72751, 1);
            vc_set_enabled(&g->interp, 91947, 1);
        }
        if (asset != 64527)
            vc_set_enabled(&g->interp, 71372, 0);
        g->scroll.item = 72761;
    }
}

/* --- Shanks's reports ------------------------------------------------ */

/* The GAM id that a registration holds in argument i, or 0. */
static u32 reg_id(game *g, const char *name, int i)
{
    int k;
    for (k = 0; k < g->reg_count; k++)
        if (strcmp(g->regs[k].name, name) == 0 && i < g->regs[k].argc)
            return g->regs[k].vars[i];
    return 0;
}

/* 0x4BC4F0 fn_SetShanksVar(flag) -> 0x51E160: works out one of the flags
 * that decide what Shanks has to say, from the variables that
 * RegShanksNode2, 3 and 6 registered (in argument order, through the
 * setters 0x51DD40, 0x51DF60 and 0x48E0A0) and the story node. In node 2
 * it also mails Willmore (88440) about the warehouse, once. */
static void shanks_var(game *g, u32 flag)
{
#define N(i) reg_id(g, "RegShanksNode2", i)
#define M(i) reg_id(g, "RegShanksNode3", i)
#define P(i) reg_id(g, "RegShanksNode6", i)
#define V(id) vc_get_var(&g->interp, id)
    i32 node = game_reg_value(g, "RegCurrNode", 0);
    int set = 1, on = 0;
    if (flag == N(13))
        on = node == 2 && !(V(N(1)) && V(N(0)));
    else if (flag == N(14))
        on = node == 2 && !V(N(2));
    else if (flag == 5053) /* SHANKSN2_bTellWillCallComputerC */
        on = node == 2 && V(N(0)) == 1 && !V(5054);
    else if (flag == N(15))
        on = node == 2 && V(N(3)) == 1 && !V(4553);
    else if (flag == N(16))
        on = node == 2 && V(N(10)) == 1 && !V(N(11));
    else if (flag == N(18))
        on = node == 2 && V(N(3)) == 1 && V(N(4)) == 1 && V(N(5)) == 1 && V(N(6)) == 1 &&
             (!V(N(7)) || !V(N(8)) || !V(N(9)));
    else if (flag == N(17))
        on = node == 2 && V(N(11)) == 1 && !V(1990) && !V(N(12));
    else if (flag == N(19))
        on = 1;
    else if (flag == M(13))
        on = node == 3 && !V(M(0));
    else if (flag == M(17))
        on = node == 3 && V(M(1)) == 1 && !V(M(2));
    else if (flag == M(14))
        on = node == 3 && V(M(2)) == 1 && !V(M(4));
    else if (flag == M(15))
        on = node == 3 && V(M(4)) == 1 && !V(M(5));
    else if (flag == M(16))
        on = node == 3 && V(M(5)) == 1 && (!V(M(6)) || !V(M(7)) || !V(M(8)));
    else if (flag == M(12))
        on = node == 3 && V(M(6)) == 1 && (!V(M(9)) || !V(M(10)) || !V(M(11)));
    else if (flag == P(3))
        on = node == 6 && V(P(0)) == 1 && !V(5155);
    else if (flag == P(4))
        on = node == 6 && V(P(1)) == 1 && !V(P(2));
    else
        set = 0;
    if (set && flag)
        vc_set_var(&g->interp, flag, on);
    /* 0x51E88B: SHANKSN2_bFoundWarehouseInING (5051) and
     * SHANKSN2_bSentWarehouseEmail (5052) both unset */
    if (node == 2 && !V(N(13)) && !V(5051) && !V(5052)) {
        if (email_find(g, 88440) < 0 && g->email_count < GAME_MAX_EMAILS) {
            g->emails[g->email_count].asset = 88440;
            g->emails[g->email_count++].read = 0;
            xf_info("e-mail 88440 arrives");
        }
        vc_set_var(&g->interp, 5052, 1);
    }
#undef N
#undef M
#undef P
#undef V
}

/* --- The computer's photo screen --------------------------------------- */

/* Layout 15232 (0x4B2370 and on): 20 thumbnails, 5 a row, of the photos
 * downloaded from the camera; one enlarged. 70853 and 51397 are the
 * Download button off and on, 70855 and 51398 the Delete button. */
static int photo_index(game *g, int slot)
{
    int i, k = 0, want = g->photo_scroll * 5 + slot;
    for (i = 0; i < g->photo_count; i++)
        if (g->photo_downloaded & (1u << i)) {
            if (k == want)
                return i;
            k++;
        }
    return -1;
}

static void photo_buttons(game *g)
{
    int i, waiting = 0;
    for (i = 0; i < g->photo_count; i++)
        waiting |= !(g->photo_downloaded & (1u << i));
    vc_set_enabled(&g->interp, 70853, !waiting);
    vc_set_enabled(&g->interp, 51397, waiting);
    vc_set_enabled(&g->interp, 70855, g->photo_selected < 0);
    vc_set_enabled(&g->interp, 51398, g->photo_selected >= 0);
    g->iface_serial++;
}

u32 game_photo_at(game *g, int slot)
{
    int i = slot < 0 ? g->photo_selected : photo_index(g, slot);
    return i >= 0 && i < g->photo_count ? g->photos[i] : 0;
}

static int photo_function(game *g, const char *name, int argc, const vc_operand *args)
{
    if (strcmp(name, "PhotoInit") == 0) {
        g->photo_scroll = 0;
        g->photo_selected = -1;
        photo_buttons(g);
        return 1;
    }
    if (strcmp(name, "PhotoDownload") == 0) { /* 0x4D8FD0 */
        g->photo_downloaded = g->photo_count >= 32 ? ~0u : (1u << g->photo_count) - 1;
        photo_buttons(g);
        return 1;
    }
    if (strcmp(name, "PhotoEnlarge") == 0 && argc == 1) { /* 0x4D8FB0 */
        int i = photo_index(g, arg_value(g, &args[0]));
        if (i >= 0) {
            g->photo_selected = i;
            photo_buttons(g);
        }
        return 1;
    }
    if (strcmp(name, "PhotoDelete") == 0) { /* 0x4D8FA0 */
        int i = g->photo_selected;
        if (i >= 0 && i < g->photo_count) {
            u32 low = g->photo_downloaded & ((1u << i) - 1), high = g->photo_downloaded >> (i + 1);
            memmove(&g->photos[i], &g->photos[i + 1], sizeof(g->photos[0]) * (size_t)(g->photo_count - i - 1));
            g->photo_count--;
            g->photo_downloaded = low | (high << i);
            g->photo_selected = -1;
            photo_buttons(g);
        }
        return 1;
    }
    if (strcmp(name, "PhotoScroll") == 0 && argc == 1) { /* 0x4D8F60: a row at a time */
        int i, n = 0;
        for (i = 0; i < g->photo_count; i++)
            n += (g->photo_downloaded >> i) & 1;
        g->photo_scroll += arg_value(g, &args[0]) ? 1 : -1;
        g->photo_scroll = XF_CLAMP(g->photo_scroll, 0, n > 20 ? (n - 20 + 4) / 5 : 0);
        return 1;
    }
    /* 0x4B85C0 fn_PhotoForward is an empty function; 0x4D8F90 closes */
    if (strcmp(name, "PhotoForward") == 0 || strcmp(name, "PhotoClose") == 0)
        return 1;
    return 0;
}

/* --- Preferences (the Options screen) ---------------------------------- */

/* Each preference is a GAM variable (Pref_...); the engine keeps them in
 * its own preferences object (0x468120), so they outlive a new game or a
 * load, and in the registry. We keep them in g->prefs and in the file
 * "prefs" of the save folder. */
static const u32 g_pref_vars[GAME_PREFS] = {
    2572, 981, 990, 989, 993, 991, 873, 992, 994, 995, 982, 2573,
    985, 986, 987, 988, 983, 4210, 5046, 976, 977, 978, 979, 980,
};

static int pref_index(u32 var)
{
    int i;
    for (i = 0; i < GAME_PREFS; i++)
        if (g_pref_vars[i] == var)
            return i;
    return -1;
}

i32 game_pref(const game *g, u32 var)
{
    int i = pref_index(var);
    return i < 0 ? 0 : g->prefs[i];
}

/* Writes the preferences into their variables (after a new game or a
 * load: the GAM holds the shipped values). */
void game_prefs_apply(game *g)
{
    int i;
    for (i = 0; i < GAME_PREFS; i++)
        vc_set_var(&g->interp, g_pref_vars[i], g->prefs[i]);
}

/* Our own settings share the file, under numbers no GAM variable has. */
/* The wide layout, off by default: the original 4:3 screen fits the
 * game's interface best. 0xFFFF0001 held it when the Vita defaulted to
 * wide; that value is no longer read. */
#define PREF_SCREEN_WIDE 0xFFFF0002u
/* The voice and text languages, by code (xf_lang_pack). */
#define PREF_VOICES 0xFFFF0003u
#define PREF_TEXT 0xFFFF0004u

const char *game_credit(const game *g)
{
    u32 t, n;
    const char *s;
    if (!g->credits_rolling)
        return NULL;
    t = g->now - g->credits_start;
    n = t / CREDIT_PERIOD;
    if (n == 0 || t - n * CREDIT_PERIOD >= CREDIT_SHOWN)
        return NULL;
    s = xf_strings_get(&g->credit_strings, (u32)g->credits_first + n - 1);
    return s && *s ? s : NULL;
}

static void load_text(game *g);
static void apply_language(game *g);
static const char *original_text(game *g, u32 asset, char *buf, size_t size);

static void prefs_save(game *g)
{
    char path[1024];
    FILE *f;
    int i;
    if (!g->save_dir[0])
        return;
    snprintf(path, sizeof(path), "%s/prefs", g->save_dir);
    if (!(f = fopen(path, "w")))
        return;
    for (i = 0; i < GAME_PREFS; i++)
        fprintf(f, "%u %d\n", g_pref_vars[i], g->prefs[i]);
    fprintf(f, "%u %d\n", PREF_SCREEN_WIDE, g->screen_wide);
    if (g->langs.voices >= 0)
        fprintf(f, "%u %d\n", PREF_VOICES, xf_lang_pack(g->langs.langs[g->langs.voices].code));
    if (g->langs.text >= 0)
        fprintf(f, "%u %d\n", PREF_TEXT, xf_lang_pack(g->langs.langs[g->langs.text].code));
    fclose(f);
}

void game_set_screen_wide(game *g, int on)
{
    g->screen_wide = on != 0;
    prefs_save(g);
}

void game_prefs_load(game *g)
{
    char path[1024];
    FILE *f;
    unsigned var;
    int value, i, voices = -1, text = -1;
    char code[8];
    for (i = 0; i < GAME_PREFS; i++) /* the shipped values first */
        g->prefs[i] = vc_get_var(&g->interp, g_pref_vars[i]);
    /* Artificial Intuition is on unless the player turns it off: the GAM
     * says 0, but the story needs it (Cook takes the case files only with
     * it on, on Disc 1). */
    g->prefs[pref_index(873)] = 1;
    g->screen_wide = 0;
    snprintf(path, sizeof(path), "%s/prefs", g->save_dir);
    if (g->save_dir[0] && (f = fopen(path, "r"))) {
        while (fscanf(f, "%u %d", &var, &value) == 2)
            if (var == PREF_SCREEN_WIDE)
                g->screen_wide = value != 0;
            else if (var == PREF_VOICES || var == PREF_TEXT) {
                xf_lang_unpack(value, code);
                *(var == PREF_VOICES ? &voices : &text) = xf_langs_find(&g->langs, code);
            } else if ((i = pref_index(var)) >= 0)
                g->prefs[i] = value;
        fclose(f);
    }
    game_prefs_apply(g);
    /* XF_VOICES and XF_TEXT (language folder names) override the saved choice */
    if (getenv("XF_VOICES"))
        voices = xf_langs_find(&g->langs, getenv("XF_VOICES"));
    if (getenv("XF_TEXT"))
        text = xf_langs_find(&g->langs, getenv("XF_TEXT"));
    if (voices >= 0 && g->langs.langs[voices].voices)
        g->langs.voices = voices;
    if (text >= 0 && g->langs.langs[text].text && text != g->langs.text) {
        g->langs.text = text;
        load_text(g);
    }
}

static void pref_set(game *g, u32 var, i32 value)
{
    int i = pref_index(var);
    if (i >= 0)
        g->prefs[i] = value;
    vc_set_var(&g->interp, var, value);
    prefs_save(g);
}

/* The Pref... functions: yes/no settings toggle, lettered ones cycle
 * (0x500810 for the difficulty: S, H, E), volumes take a slider step. */
static int pref_function(game *g, const char *name, int argc, const vc_operand *args)
{
    static const struct {
        const char *name;
        const char *cycle; /* letters to cycle through, NULL for yes/no */
    } prefs[] = {
        { "PrefActionDiff", "SHE" }, { "PrefAutoRes", "AYN" }, { "PrefAutoDepth", "AYN" },
        { "PrefSaveOnExit", "AYN" }, { "PrefFlashBeam", "MWS" }, { "PrefFlashBrght", "MBD" },
        { "PrefInventory", "HV" },   { "PrefArtIntuit", NULL }, { "PrefAutoRewind", NULL },
        { "PrefSkipIntro", NULL },   { "PrefSkipVideo", NULL },  { "PrefRollover", NULL },
        { "PrefClosedCap", NULL },   { "PrefSubliminal", NULL }, { "PrefTransition", NULL },
        { "PrefTranslucnt", NULL },  { "PrefSmoothText", NULL }, { "PrefHighVideo", NULL },
        { "PrefLiveEnviro", NULL },  { "Pref3DSound", NULL },
    };
    size_t k;
    u32 var = argc >= 1 && args[0].kind == VC_OPND_VARIABLE ? args[0].value : 0;
    for (k = 0; k < XF_COUNT(prefs); k++)
        if (strcmp(name, prefs[k].name) == 0 && var) {
            i32 v = vc_get_var(&g->interp, var);
            if (prefs[k].cycle) {
                const char *c = strchr(prefs[k].cycle, (char)v);
                v = c && c[1] ? c[1] : prefs[k].cycle[0];
            } else
                v = !v;
            pref_set(g, var, v);
            return 1;
        }
    /* 0x500A50: a volume slider, 0 to 100; step 0 and 11 are the arrows */
    if ((strcmp(name, "PrefDialogVol") == 0 || strcmp(name, "PrefMusicVol") == 0 ||
         strcmp(name, "PrefEffectVol") == 0 || strcmp(name, "PrefVolume") == 0) && argc == 2 && var) {
        i32 step = arg_value(g, &args[1]), v = vc_get_var(&g->interp, var);
        v = step == 0 ? v - 10 : step == 11 ? v + 10 : step * 10;
        pref_set(g, var, XF_CLAMP(v, 0, 100));
        return 1;
    }
    /* 0x481230 fn_PrefInit, 0x4B5800 fn_AudioInit: the screen shows the
     * settings, read from the variables */
    if (strcmp(name, "PrefInit") == 0 || strcmp(name, "AudioInit") == 0) {
        game_prefs_apply(g);
        return 1;
    }
    /* 0x4B8C30 fn_PrefDefaults(preset) -> 0x500760: 'Q' best quality
     * (0x4C1830), 'P' best performance (0x4C1700). Both set the beam
     * (Wide, Small), transitions, translucency, smooth text, subliminal
     * messages, high quality video, live environments and 3D sound; Best
     * Quality also turns rollover text on, Best Performance closed
     * captioning off. Neither changes the flashlight's brightness. */
    if (strcmp(name, "PrefDefaults") == 0 && argc == 1) {
        int q = (char)arg_value(g, &args[0]) == 'Q';
        pref_set(g, 990, q ? 'W' : 'S');
        pref_set(g, 986, q);
        pref_set(g, 987, q);
        pref_set(g, 988, q);
        pref_set(g, 985, q);
        pref_set(g, 4210, q);
        pref_set(g, 5046, q);
        pref_set(g, 980, q);
        if (q)
            pref_set(g, 982, 1);
        else
            pref_set(g, 2573, 0);
        return 1;
    }
    return 0;
}

/* --- Books: Willmore's journal, Scully's Bible ------------------------- */

/* A book shows two texts side by side: the enabled media of one item. */
static int book_pages(game *g, u32 item, u32 *ids, int max)
{
    vc_iface_item it;
    u32 all[128];
    int n, i, k = 0;
    if (vc_read_iface_item(&g->db, item, &it) != 0)
        return 0;
    n = vc_read_list(&g->db, it.assets, all, 128);
    for (i = 0; i < n && k < max; i++)
        if (vc_is_enabled(&g->interp, all[i]))
            ids[k++] = all[i];
    return k;
}

static void book_show(game *g, u32 item, u32 left, u32 right, int page)
{
    u32 ids[128];
    int n = book_pages(g, item, ids, 128), side;
    for (side = 0; side < 2; side++) {
        char text[2048] = "";
        game_item_label *l;
        if (page + side < n)
            note_text(g, ids[page + side], text, sizeof(text));
        set_item_text(g, side ? right : left, text);
        if ((l = (game_item_label *)game_item_label_of(g, side ? right : left))) {
            l->font = 'c';
            l->size = 10;
            l->align = 'w'; /* wrapped, from the top */
        }
    }
}

static int book_function(game *g, const char *name, int argc, const vc_operand *args)
{
    /* 0x4B58A0 fn_BibleInit -> 0x507540, 0x4B5820 fn_BibleFlip(back):
     * item 80907's texts in 80868 and 80869; 80870 and 80871 turn back
     * and forward */
    if (strcmp(name, "BibleInit") == 0 || (strcmp(name, "BibleFlip") == 0 && argc == 1)) {
        u32 ids[128];
        int n = book_pages(g, 80907, ids, 128);
        if (name[5] == 'I')
            g->bible_page = 0;
        else
            g->bible_page += arg_value(g, &args[0]) ? -2 : 2;
        g->bible_page = XF_CLAMP(g->bible_page, 0, n > 2 ? (n - 1) & ~1 : 0);
        vc_set_enabled(&g->interp, 80870, g->bible_page > 0);
        vc_set_enabled(&g->interp, 80871, g->bible_page + 2 < n);
        book_show(g, 80907, 80868, 80869, g->bible_page);
        return 1;
    }
    /* 0x4B7150 fn_JournalInit(start at front) -> 0x4A4C40: item 49422's
     * texts (the entries written so far) in 46370 and 46371; the book opens
     * at the front or at the last entries */
    if (strcmp(name, "JournalInit") == 0 && argc == 1) {
        u32 ids[128];
        int n = book_pages(g, 49422, ids, 128);
        g->journal_page = arg_value(g, &args[0]) == 1 ? 0 : (n & 1) ? n - 1 : XF_MAX(n - 2, 0);
        book_show(g, 49422, 46370, 46371, g->journal_page);
        return 1;
    }
    /* 0x4B6F90 fn_JournalFlip(back, first page var, last page var) */
    if (strcmp(name, "JournalFlip") == 0 && argc == 3) {
        u32 ids[128];
        int n = book_pages(g, 49422, ids, 128);
        g->journal_page += arg_value(g, &args[0]) ? -2 : 2;
        g->journal_page = XF_CLAMP(g->journal_page, 0, n > 2 ? (n - 1) & ~1 : 0);
        if (args[1].kind == VC_OPND_VARIABLE)
            vc_set_var(&g->interp, args[1].value, g->journal_page == 0);
        if (args[2].kind == VC_OPND_VARIABLE)
            vc_set_var(&g->interp, args[2].value, g->journal_page + 2 >= n);
        book_show(g, 49422, 46370, 46371, g->journal_page);
        return 1;
    }
    if (strcmp(name, "JournalDestroy") == 0)
        return 1;
    /* 0x4B7950 fn_PageTurnSound -> 0x4BFF40: vc_queue_action_list
     * (0x47BD40) runs list 92468 with trigger 92463, which picks one of
     * five page sounds (RandGen into var 1479). */
    if (strcmp(name, "PageTurnSound") == 0) {
        g->effect_mode = 1;
        vc_run_trigger_list(&g->interp, 92463, 92468);
        g->effect_mode = 0;
        return 1;
    }
    return 0;
}

/* --- Media Index ------------------------------------------------------- */

/* 0x65D148: 16 items, each an article (first media) and the keywords that
 * find it (the other media, when enabled). */
static const u32 g_media_index[16] = {
    55604, 55574, 55575, 55576, 55577, 55578, 55579, 55580, 55581, 55582,
    91114, 91117, 91118, 91125, 91127, 91131,
};

/* Case and a translation's accents aside. */
static int same_words(const char *x, const char *y)
{
    char a[256], b[256];
    xf_fold_ascii(a, sizeof(a), x);
    xf_fold_ascii(b, sizeof(b), y);
    return strcmp(a, b) == 0;
}

/* 0x4654A0: the typed words must equal a keyword (case aside). A match
 * shows the article in the text box (item 55041) and turns the Forward
 * button (55036) on; the Tarakan article (55591) also plays clip 21068.
 * No match shows article 58348. Each plays its sound (90693/90694 or
 * 90037/90038). */
static void media_search(game *g)
{
    u32 ids[64], article = 0;
    int i, k, n;
    for (i = 0; i < 16 && !article; i++) {
        vc_iface_item it;
        if (vc_read_iface_item(&g->db, g_media_index[i], &it) != 0)
            continue;
        n = vc_read_list(&g->db, it.assets, ids, 64);
        for (k = 1; k < n && !article; k++) {
            char kw[256];
            if (!vc_is_enabled(&g->interp, ids[k]))
                continue;
            game_text(g, ids[k], kw, sizeof(kw));
            if (same_words(kw, g->media_query) ||
                (original_text(g, ids[k], kw, sizeof(kw)) && same_words(kw, g->media_query)))
                article = ids[0];
        }
    }
    memset(&g->scroll, 0, sizeof(g->scroll));
    g->scroll.item = 55041;
    g->scroll.color = 'k';
    g->scroll.font = 'c';
    g->scroll.size = 10;
    note_text(g, article ? article : 58348, g->scroll.text, sizeof(g->scroll.text));
    set_item_text(g, 55039, g->media_query);
    g->media_query[0] = '\0';
    vc_set_enabled(&g->interp, 55036, article != 0);
    if (article) {
        run_trigger_list_pair(g, 90693, 90694);
        if (article == 55591)
            game_queue_media(g, 21068);
    } else
        run_trigger_list_pair(g, 90037, 90038);
    g->media_article = article;
}

/* --- PDA field notes ---------------------------------------------------- */

/* 0x658F28: the 23 pages of notes, each a layout item whose media are
 * the notes (inline text, or a file under Text/Pda Notes). */
#define NOTE_PAGES 23
static const u32 g_note_pages[NOTE_PAGES] = {
    45344, 45345, 45346, 45348, 45347, 45349, 45350, 50378, 63395, 63389, 63393, 63397,
    63399, 63401, 63403, 63405, 63409, 63411, 63413, 63415, 63417, 63419, 63421,
};

/* The text of a note: a file it names, or the text itself. */
static void note_text(game *g, u32 asset, char *out, size_t size)
{
    email_text(g, asset, out, size);
    if (!out[0])
        game_text(g, asset, out, size);
}

/* 0x4CF8E0: pages 5 to 8 follow the order the player visited places in
 * (variables 5328, 5329, 5330), pages 13 and 14 swap with variable 5325
 * (0x6B6558 holds the order). Only the pages up to the first one without
 * a note are open. step moves to the next (1) or previous (-1) page. */
static void notes_show(game *g, int step)
{
    int order[NOTE_PAGES], open = 0, p, k, n;
    char *t = g->scroll.text;
    size_t used = 0;
    u32 ids[64];
    char page[8];
    for (p = 0; p < NOTE_PAGES; p++)
        order[p] = p;
    if (vc_get_var(&g->interp, 5330) == 1) {
        order[5] = 6; order[6] = 7; order[7] = 8; order[8] = 5;
    } else if (vc_get_var(&g->interp, 5329) == 1) {
        order[5] = 6; order[6] = 7; order[7] = 5;
    } else if (vc_get_var(&g->interp, 5328) == 1) {
        order[5] = 6; order[6] = 5;
    }
    if (vc_get_var(&g->interp, 5325) == 1) {
        order[13] = 14; order[14] = 13;
    }
    for (p = 0; p < NOTE_PAGES; p++) {
        vc_iface_item it;
        int any = 0;
        if (vc_read_iface_item(&g->db, g_note_pages[order[p]], &it) != 0)
            break;
        n = vc_read_list(&g->db, it.assets, ids, 64);
        for (k = 0; k < n && !any; k++)
            any = in_list(g->pda_notes, g->pda_note_count, ids[k]);
        if (!any)
            break;
        open++;
    }
    if (!open)
        open = 1;
    g->note_page = XF_CLAMP(g->note_page + step, 0, open - 1);
    g->note_page_order = order[g->note_page];
    memset(&g->scroll, 0, sizeof(g->scroll));
    g->scroll.item = 51444;
    g->scroll.color = 'k';
    g->scroll.font = 'c';
    g->scroll.size = 10;
    t[0] = '\0';
    {
        vc_iface_item it;
        if (vc_read_iface_item(&g->db, g_note_pages[order[g->note_page]], &it) == 0) {
            n = vc_read_list(&g->db, it.assets, ids, 64);
            for (k = 0; k < n; k++) {
                char line[1024];
                if (!in_list(g->pda_notes, g->pda_note_count, ids[k]))
                    continue;
                note_text(g, ids[k], line, sizeof(line));
                used += (size_t)snprintf(t + used, sizeof(g->scroll.text) - used, "%s%s", used ? "\n\n" : "", line);
                if (used >= sizeof(g->scroll.text))
                    break;
            }
        }
    }
    snprintf(page, sizeof(page), "%d", g->note_page + 1);
    set_item_text(g, 67486, page);
}

/* --- Save and Load screens -------------------------------------------- */

/* Layout 82537 is the Save screen, 82634 the Load screen; three slots
 * each. Per slot: the item clicked, the item that shows the name. */
static const u32 g_save_slot_item[3] = { 82553, 82554, 82555 };
static const u32 g_save_slot_name[3] = { 82556, 82557, 82558 };
static const u32 g_load_slot_item[3] = { 82646, 82647, 82648 };
static const u32 g_load_slot_name[3] = { 82826, 82827, 82828 };
#define SAVE_LAYOUT 82537
#define LOAD_LAYOUT 82634
#define SAVE_SLOT_TOP 166  /* 0x4CBDB0: row n is at 166 + 58 n */
#define SAVE_SLOT_STEP 58
#define SAVE_NAME_MAX 21

void game_slot_path(const game *g, int slot, char *out, size_t size)
{
    snprintf(out, size, "%s/slot%d.xfs", g->save_dir, slot + 1);
}

/* Draws the slot names; the Save screen shows a text cursor after the
 * selected one (item 82561). */
static void slot_show(game *g)
{
    int i;
    for (i = 0; i < 3; i++) {
        u32 item = g->slot_mode == 'S' ? g_save_slot_name[i] : g_load_slot_name[i];
        game_item_label *l;
        set_item_text(g, item, g->slot_names[i]);
        if ((l = (game_item_label *)game_item_label_of(g, item))) {
            l->color = 'w'; /* white, on the dark slots */
            l->size = 18; /* SaveSlots_layout (0x4CBDB0): Courier New 18 */
            l->bold = 'y';
        }
    }
    {
        game_iface_item *hl = (game_iface_item *)find_item(g, g->slot_mode == 'S' ? 83314 : 83320);
        int top = SAVE_SLOT_TOP - 1 + SAVE_SLOT_STEP * g->slot;
        if (hl) {
            int h = hl->item.region.bottom - hl->item.region.top;
            hl->item.region.top = (i16)top;
            hl->item.region.bottom = (i16)(top + h);
        }
    }
    g->iface_serial++;
}

static void slot_read_names(game *g)
{
    int i;
    for (i = 0; i < 3; i++) {
        char path[1024];
        game_slot_path(g, i, path, sizeof(path));
        g->slot_used[i] = g->save_dir[0] && game_save_file_name(path, g->slot_names[i], sizeof(g->slot_names[i])) == 0;
        if (!g->slot_used[i])
            g->slot_names[i][0] = '\0';
    }
}

static void slot_close(game *g, int to_menu)
{
    host_interface(g, g->slot_mode == 'S' ? SAVE_LAYOUT : LOAD_LAYOUT, 1);
    g->slot_mode = 0;
    if (to_menu)
        g->ui_request = GAME_UI_MENU;
}

static void slot_save(game *g)
{
    char path[1024];
    game_slot_path(g, g->slot, path, sizeof(path));
    if (game_save_file(g, path, g->slot_names[g->slot]) == 0)
        game_set_previous(g, g->slot);
    slot_close(g, 1);
}

/* 0x4CCAC0: OK on the Save screen checks the name first. */
static void slot_commit_save(game *g)
{
    int i;
    if (!g->slot_names[g->slot][0]) {
        game_message(g, 32, GAME_ASK_OK); /* "Please enter a name ..." */
        return;
    }
    for (i = 0; i < 3; i++)
        if (i != g->slot && g->slot_used[i] && strcmp(g->slot_names[i], g->slot_names[g->slot]) == 0) {
            game_message(g, 38, GAME_ASK_SAVE); /* another slot has this name */
            return;
        }
    if (g->slot_used[g->slot]) {
        game_message(g, 31, GAME_ASK_SAVE); /* "Overwrite existing saved game" */
        return;
    }
    slot_save(g);
}

static int slot_function(game *g, const char *name, int argc, const vc_operand *args)
{
    /* 0x4BC110 fn_SaveLoadInit: the item that runs it says which screen. */
    if (strcmp(name, "SaveLoadInit") == 0) {
        g->slot_mode = g->trigger_item == 82787 ? 'S' : 'L';
        g->slot = 0;
        slot_read_names(g);
        slot_show(g);
        return 1;
    }
    /* 0x4BC130 fn_SaveSlotSelect(slot) */
    if (strcmp(name, "SaveSlotSelect") == 0 && argc == 1) {
        i32 n = arg_value(g, &args[0]);
        if (n >= 0 && n < 3 && g->slot_mode) {
            g->slot = n;
            g->focus_item = g->slot_mode == 'S' ? g_save_slot_item[n] : g_load_slot_item[n];
            slot_show(g);
        }
        return 1;
    }
    /* 0x4BBFC0 fn_SaveKeyPress(slot, key): typing a name on the Save screen */
    if (strcmp(name, "SaveKeyPress") == 0 && argc == 2) {
        i32 n = arg_value(g, &args[0]), key = arg_value(g, &args[1]);
        size_t len;
        if (g->slot_mode != 'S' || n < 0 || n > 2)
            return 1;
        if (key == '\r' || key == '\n') {
            slot_commit_save(g);
            return 1;
        }
        g->slot = n;
        len = strlen(g->slot_names[n]);
        if (key == 8 && len)
            g->slot_names[n][len - 1] = '\0';
        else if (key >= 32 && key < 127 && len < SAVE_NAME_MAX) {
            g->slot_names[n][len] = (char)key;
            g->slot_names[n][len + 1] = '\0';
        }
        slot_show(g);
        return 1;
    }
    /* 0x4BC090 fn_SaveLoadComit(ok) */
    if (strcmp(name, "SaveLoadComit") == 0 && argc == 1) {
        if (!arg_value(g, &args[0]))
            slot_close(g, 1);
        else if (g->slot_mode == 'S')
            slot_commit_save(g);
        else if (g->slot_mode == 'L' && g->slot_used[g->slot]) {
            /* The load replaces the interpreter that runs this trigger:
             * the window does it afterwards (game_load_slot). */
            g->load_slot = g->slot;
            slot_close(g, 0);
            g->ui_request = GAME_UI_LOAD;
        }
        return 1;
    }
    return 0;
}

void game_open_layout(game *g, u32 layout)
{
    host_interface(g, layout, 0);
}

void game_close_layout(game *g, u32 layout)
{
    host_interface(g, layout, 1);
}

void game_close_slots(game *g)
{
    if (g->slot_mode)
        slot_close(g, 1);
}

u32 game_clip_offset(const game *g, u32 asset)
{
    int j;
    for (j = 0; j < g->clip_offset_count; j++)
        if (g->clip_offsets[j].asset == asset)
            return g->clip_offsets[j].ms;
    return 0;
}

/* The menu's Save and Load items open these screens. */
void game_open_slots(game *g, int save)
{
    if (save && !g->view)
        return; /* nothing to save before a game starts */
    host_interface(g, save ? SAVE_LAYOUT : LOAD_LAYOUT, 0);
}

/* "Previous": the slot saved or loaded last, kept in a file beside them. */
void game_set_previous(game *g, int slot)
{
    char path[1024];
    FILE *f;
    if (!g->save_dir[0])
        return;
    snprintf(path, sizeof(path), "%s/previous", g->save_dir);
    if ((f = fopen(path, "w"))) {
        fprintf(f, "%d\n", slot + 1);
        fclose(f);
    }
}

int game_load_slot(game *g, int slot)
{
    char path[1024];
    game_slot_path(g, slot, path, sizeof(path));
    if (game_load_file(g, path) != 0)
        return -1;
    game_set_previous(g, slot);
    return 0;
}

int game_load_previous(game *g)
{
    char path[1024];
    FILE *f;
    int slot = 0;
    snprintf(path, sizeof(path), "%s/previous", g->save_dir);
    if (!g->save_dir[0] || !(f = fopen(path, "r")))
        return -1;
    if (fscanf(f, "%d", &slot) != 1 || slot < 1 || slot > 3)
        slot = 0;
    fclose(f);
    if (!slot)
        return -1;
    return game_load_slot(g, slot - 1);
}

/* A question for the player in a message box (engine strings of
 * xfilese.dll). The window shows it and calls game_answer. */
void game_message(game *g, u32 text, int action)
{
    g->ask = text;
    g->ask_action = action;
}

const char *game_message_text(game *g)
{
    const char *s = g->ask ? xf_strings_get(&g->engine_strings, g->ask) : NULL;
    if (s)
        return s;
    switch (g->ask) { /* the English DVD release lacks some strings */
    case 31: return "Overwrite existing saved game";
    case 32: return "Please enter a name for your save game.";
    case 38: return "Another saved game slot with this name already exists.  Overwrite existing game";
    case 39: return "Try again";
    default: return "";
    }
}

void game_answer(game *g, int yes)
{
    int action = g->ask_action;
    g->ask = 0;
    g->ask_action = GAME_ASK_OK;
    if (action == GAME_ASK_SAVE && yes)
        slot_save(g);
    else if (action == GAME_ASK_RESTORE) {
        if (!yes || game_restore_checkpoint(g) != 0)
            g->ui_request = GAME_UI_MENU;
        else
            g->ui_request = GAME_UI_GAME;
    }
}

/* --- Writing mail -------------------------------------------------------- */

/* 0x6584D0: the nine people Willmore can mail, in the order of the reply
 * lists: Amis, Cook, Shanks, Skinner, Jim Seitz, Mike Robinson, Barbara
 * Willmore, Pam Hill, Perry Allen. */
#define MAIL_PEOPLE 9
static const u32 g_mail_people[MAIL_PEOPLE] = { 45284, 45290, 45298, 45300, 64037, 45296, 45304, 45294, 45282 };
/* 0x6B6490 and on: the rows of each list (DisplayAddrs). */
static const u32 g_rows_to[3] = { 40430, 40431, 40432 }, g_rows_cc[3] = { 40433, 40434, 40435 };
static const u32 g_rows_pick_to[5] = { 40423, 40424, 40425, 40426, 40427 };
static const u32 g_checks_to[5] = { 40416, 40417, 40418, 40419, 40420 };
static const u32 g_rows_pick_cc[5] = { 40453, 40454, 40455, 40456, 40457 };
static const u32 g_checks_cc[5] = { 40448, 40449, 40450, 40451, 40452 };
static const u32 g_rows_pda[9] = { 65031, 65030, 65029, 37215, 37216, 37217, 37218, 37219, 37220 };
/* 0x65D188: forwarded article -> the item whose media are the replies */
static const u32 g_media_replies[10][2] = {
    { 55585, 63706 }, { 55592, 63708 }, { 55587, 63710 }, { 55586, 63712 }, { 55590, 63714 },
    { 55588, 63716 }, { 55589, 63718 }, { 55591, 63720 }, { 55584, 63722 }, { 55583, 63724 },
};
/* 0x65E650: ING record (our record number) -> replies */
static const u32 g_ing_replies[18] = {
    63666, 63668, 63674, 63678, 63684, 63686, 63688, 63690, 63692,
    63694, 63696, 63698, 63700, 63702, 63704, 64583, 64582, 64584,
};
/* 0x658F88: notes page (in g_note_pages order) -> replies; page 22 none */
static const u32 g_note_replies[22] = {
    63620, 63664, 63628, 63636, 63658, 63638, 63634, 63656, 63622, 63632, 63614,
    63660, 63618, 63626, 63646, 63642, 63654, 63598, 63620, 63648, 63650, 63644,
};

/* The people the address book lists: the enabled ones. */
static int mail_people(game *g, int *idx)
{
    int i, n = 0;
    for (i = 0; i < MAIL_PEOPLE; i++)
        if (vc_is_enabled(&g->interp, g_mail_people[i]))
            idx[n++] = i;
    return n;
}

static void compose_clear(game *g)
{
    g->mail_to = g->mail_cc = g->mail_pick = 0;
    g->mail_scroll = 0;
    g->forward_type = 0;
    g->forward_id = 0;
}

/* Rows show the people of a set (a bit per person), from the first shown. */
static void show_people(game *g, const u32 *rows, int nrows, u32 set, int first)
{
    int i, k = 0, r = 0;
    char t[128];
    for (i = 0; i < MAIL_PEOPLE && r < nrows; i++)
        if (set & (1u << i)) {
            if (k++ < first)
                continue;
            set_item_text(g, rows[r++], game_text(g, g_mail_people[i], t, sizeof(t)));
        }
    while (r < nrows)
        set_item_text(g, rows[r++], "");
}

/* A picker: five people from the scroll position, checked when picked. */
static void show_picker(game *g, int cc)
{
    int idx[MAIL_PEOPLE], n = mail_people(g, idx), r;
    char t[128];
    for (r = 0; r < 5; r++) {
        int k = g->mail_scroll + r;
        set_item_text(g, (cc ? g_rows_pick_cc : g_rows_pick_to)[r],
                      k < n ? game_text(g, g_mail_people[idx[k]], t, sizeof(t)) : "");
        vc_set_enabled(&g->interp, (cc ? g_checks_cc : g_checks_to)[r], k < n && (g->mail_pick & (1u << idx[k])));
    }
    g->iface_serial++;
}

/* What a forward encloses: the enclosure line (40436), the subject
 * (40437) and the text in the message box (40438). */
static void compose_show_enclosure(game *g)
{
    char title[160] = "", *nl;
    memset(&g->scroll, 0, sizeof(g->scroll));
    g->scroll.item = 40438;
    g->scroll.color = 'k';
    g->scroll.font = 'c';
    g->scroll.size = 10;
    if (g->forward_type == 'M' && g->forward_id) {
        note_text(g, g->forward_id, g->scroll.text, sizeof(g->scroll.text));
    } else if (g->forward_type == 'I' && g->forward_id < 18) {
        u32 media[16];
        char line[160];
        int n = item_media(g, g_ing_names[g->forward_id].record, media, 16), k;
        size_t used = 0;
        for (k = 0; k < n; k++)
            used += (size_t)snprintf(g->scroll.text + used, sizeof(g->scroll.text) - used, "%s\n",
                                     game_text(g, media[k], line, sizeof(line)));
    }
    snprintf(title, sizeof(title), "%s", g->scroll.text);
    if ((nl = strchr(title, '\n')))
        *nl = '\0';
    set_item_text(g, 40436, title);
    set_item_text(g, 40437, title);
}

/* The replies a forward brings: the item whose media hold one reply per
 * person (0 for none). */
static u32 forward_replies(game *g)
{
    size_t i;
    if (g->forward_type == 'M') { /* 0x465780 */
        for (i = 0; i < XF_COUNT(g_media_replies); i++)
            if (g_media_replies[i][0] == g->forward_id)
                return g_media_replies[i][1];
    } else if (g->forward_type == 'I') { /* 0x5149B0 */
        return g->forward_id < 18 ? g_ing_replies[g->forward_id] : 0;
    } else if (g->forward_type == 'N') { /* 0x4D0660 */
        for (i = 0; i < XF_COUNT(g_note_replies); i++)
            if (g_note_pages[i] == g->forward_id)
                return g_note_replies[i];
    }
    return 0;
}

/* 0x466A80 fn_EmailSend -> 0x474E20: everyone on the To and Cc lines (once
 * each) whose reply list has an entry gets the forward; their replies wait
 * (0x484040) until EmailFlushMess delivers them. Forwarding the second
 * notes page to Cook (replies 63664) is special: with variable 1980 set he
 * does not answer, otherwise lists 44085/44089 run. */
static void email_send(game *g)
{
    u32 replies = forward_replies(g), to = g->mail_to | g->mail_cc, ids[16];
    int n = 0, i;
    if (replies) {
        vc_iface_item it;
        if (vc_read_iface_item(&g->db, replies, &it) == 0)
            n = vc_read_list(&g->db, it.assets, ids, 16);
    }
    for (i = 0; i < MAIL_PEOPLE && i < n; i++) {
        if (!(to & (1u << i)) || !ids[i])
            continue;
        if (replies == 63664 && i == 1) {
            if (vc_get_var(&g->interp, 1980) == 1)
                continue;
            run_trigger_list_pair(g, 44085, 44089);
        }
        if (g->pending_mail_count < (int)XF_COUNT(g->pending_mail))
            g->pending_mail[g->pending_mail_count++] = ids[i];
    }
    xf_info("mail sent (%c), %d replies on the way", g->forward_type ? g->forward_type : '-', g->pending_mail_count);
    compose_clear(g);
}

static void email_deliver(game *g, u32 asset)
{
    if (email_find(g, asset) >= 0 || g->email_count >= GAME_MAX_EMAILS)
        return;
    g->emails[g->email_count].asset = asset;
    g->emails[g->email_count++].read = 0;
    xf_info("e-mail %u arrives", asset);
}

static int compose_function(game *g, const char *name, int argc, const vc_operand *args)
{
    char mode = argc >= 1 ? (char)arg_value(g, &args[0]) : 0;
    /* 0x42A580 fn_ForwardObject(type) -> 0x42A2C0: what the mail encloses */
    if (strcmp(name, "ForwardObject") == 0 && argc == 1) {
        g->forward_type = mode;
        if (mode == 'M')
            g->forward_id = g->media_article;
        else if (mode == 'I')
            g->forward_id = (u32)g->ing_record;
        else if (mode == 'N')
            g->forward_id = g_note_pages[g->note_page_order];
        compose_show_enclosure(g);
        return 1;
    }
    if (strcmp(name, "EmailSend") == 0) {
        email_send(g);
        return 1;
    }
    /* 0x443B90 fn_EmailFlushMess -> 0x484110: the replies arrive */
    if (strcmp(name, "EmailFlushMess") == 0) {
        int i;
        for (i = 0; i < g->pending_mail_count; i++)
            email_deliver(g, g->pending_mail[i]);
        g->pending_mail_count = 0;
        return 1;
    }
    /* 0x42A320 fn_DisplayAddrs(list) -> 0x44A4E0 */
    if (strcmp(name, "DisplayAddrs") == 0 && argc == 1) {
        if (mode == 'A')
            show_people(g, g_rows_to, 3, g->mail_to, g->mail_scroll_to);
        else if (mode == 'B')
            show_people(g, g_rows_cc, 3, g->mail_cc, g->mail_scroll_cc);
        else if (mode == 'T' || mode == 'C') {
            g->mail_pick = mode == 'T' ? g->mail_to : g->mail_cc;
            g->mail_picking = mode;
            g->mail_scroll = 0;
            show_picker(g, mode == 'C');
        } else if (mode == 'P') { /* the PDA's list of all nine */
            int idx[MAIL_PEOPLE], k = mail_people(g, idx), r;
            char t[128];
            for (r = 0; r < 9; r++)
                set_item_text(g, g_rows_pda[r], r < k ? game_text(g, g_mail_people[idx[r]], t, sizeof(t)) : "");
        }
        return 1;
    }
    /* 0x42F830 fn_AddrSelected(row): picks or unpicks */
    if (strcmp(name, "AddrSelected") == 0 && argc == 1) {
        int idx[MAIL_PEOPLE], n = mail_people(g, idx), k = g->mail_scroll + arg_value(g, &args[0]);
        if (k >= 0 && k < n)
            g->mail_pick ^= 1u << idx[k];
        show_picker(g, g->mail_picking == 'C');
        return 1;
    }
    /* 0x4B5700 fn_AddrSelPDA(row): the PDA mails the notes to one person */
    if (strcmp(name, "AddrSelPDA") == 0 && argc == 1) {
        int idx[MAIL_PEOPLE], n = mail_people(g, idx), k = arg_value(g, &args[0]);
        char t[128];
        g->mail_to = k >= 0 && k < n ? 1u << idx[k] : 0;
        g->mail_cc = 0;
        set_item_text(g, 40516, k >= 0 && k < n ? game_text(g, g_mail_people[idx[k]], t, sizeof(t)) : "");
        return 1;
    }
    /* 0x4701C0 fn_CreateAddrList(list): the picker's choice becomes the
     * To ('A') or Cc ('B') line */
    if (strcmp(name, "CreateAddrList") == 0 && argc == 1) {
        if (mode == 'A')
            g->mail_to = g->mail_pick, g->mail_scroll_to = 0;
        else if (mode == 'B')
            g->mail_cc = g->mail_pick, g->mail_scroll_cc = 0;
        return 1;
    }
    /* 0x4600C0 fn_ScrollAddrs(list, down) */
    if (strcmp(name, "ScrollAddrs") == 0 && argc == 2) {
        int d = arg_value(g, &args[1]) ? 1 : -1, idx[MAIL_PEOPLE], n = mail_people(g, idx);
        if (mode == 'T' || mode == 'C') {
            g->mail_scroll = XF_CLAMP(g->mail_scroll + d, 0, n > 5 ? n - 5 : 0);
            show_picker(g, mode == 'C');
        } else if (mode == 'A') {
            g->mail_scroll_to = XF_MAX(g->mail_scroll_to + d, 0);
            show_people(g, g_rows_to, 3, g->mail_to, g->mail_scroll_to);
        } else if (mode == 'B') {
            g->mail_scroll_cc = XF_MAX(g->mail_scroll_cc + d, 0);
            show_people(g, g_rows_cc, 3, g->mail_cc, g->mail_scroll_cc);
        }
        return 1;
    }
    return 0;
}

static int email_function(game *g, const char *name, int argc, const vc_operand *args)
{
    int i;
    if (strcmp(name, "SendEmail") == 0 && argc == 1) {
        u32 asset = (u32)arg_value(g, &args[0]);
        if (email_find(g, asset) < 0 && g->email_count < GAME_MAX_EMAILS) {
            g->emails[g->email_count].asset = asset;
            g->emails[g->email_count++].read = 0;
            xf_info("e-mail %u arrives", asset);
        }
        return 1;
    }
    if (strcmp(name, "EmailInit") == 0 && argc == 1) {
        g->email_mode = (char)arg_value(g, &args[0]);
        if (g->trigger_item == 49328) /* the compose screen opens empty */
            compose_clear(g);
        return 1;
    }
    if (strcmp(name, "DisplayInBox") == 0 && argc == 1) {
        email_display(g, (u32)arg_value(g, &args[0]));
        return 1;
    }
    if (strcmp(name, "ScrollHeaders") == 0 && argc == 1) {
        i32 d = arg_value(g, &args[0]);
        int max = g->email_count > GAME_EMAIL_ROWS ? g->email_count - GAME_EMAIL_ROWS : 0;
        g->email_scroll += d == 0 || d == 10 ? -1 : 1;
        g->email_scroll = XF_CLAMP(g->email_scroll, 0, max);
        email_show_rows(g);
        return 1;
    }
    if (strcmp(name, "GetCurrEmail") == 0 && argc == 2) {
        i = g->email_scroll + arg_value(g, &args[0]);
        if (args[1].kind == VC_OPND_VARIABLE)
            vc_set_var(&g->interp, args[1].value,
                       i >= 0 && i < g->email_count ? (i32)g->emails[i].asset : 0);
        return 1;
    }
    if (strcmp(name, "MarkEmailRead") == 0 && argc == 1) {
        i = g->email_scroll + arg_value(g, &args[0]);
        if (i >= 0 && i < g->email_count)
            g->emails[i].read = 1;
        return 1;
    }
    if (strcmp(name, "InitEmailRead") == 0 && argc == 2) {
        email_read(g, (u32)arg_value(g, &args[0]), (u32)arg_value(g, &args[1]));
        return 1;
    }
    if (strcmp(name, "EmailDelete") == 0 && argc == 1) {
        i = email_find(g, (u32)arg_value(g, &args[0]));
        if (i >= 0) {
            memmove(&g->emails[i], &g->emails[i + 1], sizeof(g->emails[0]) * (size_t)(g->email_count - i - 1));
            g->email_count--;
        }
        return 1;
    }
    /* Flushing pending mails and preloading the icons only change what
     * is drawn. */
    if (strcmp(name, "PreloadEmIcon") == 0)
        return 1;
    /* IsIItemHeld(item, var): whether the mouse is held on an item (the
     * scroll arrows repeat while it is). */
    if (strcmp(name, "IsIItemHeld") == 0 && argc == 2) {
        if (args[1].kind == VC_OPND_VARIABLE)
            vc_set_var(&g->interp, args[1].value, 0);
        return 1;
    }
    return 0;
}

/* Keeps a ScrollTextList text for its item, replacing the item's old one. */
static void keep_text(game *g, const game_scroll *sb)
{
    int t;
    for (t = 0; t < g->text_count && g->texts[t].item != sb->item; t++)
        ;
    if (t == GAME_MAX_TEXTS)
        return;
    g->texts[t] = *sb;
    if (t == g->text_count)
        g->text_count++;
}

const game_scroll *game_scroll_box(game *g, u32 item)
{
    int t;
    if (!item)
        return NULL;
    if (g->scroll.item == item)
        return g->scroll.text[0] ? &g->scroll : NULL;
    for (t = 0; t < g->text_count; t++)
        if (g->texts[t].item == item)
            return g->texts[t].text[0] ? &g->texts[t] : NULL;
    return NULL;
}

/* What the ING shows when nothing is found (0x4653B9): "No Data Found"
 * (media 58348). */
#define ING_NOT_FOUND 58348

/* 0x4667D0: shows one record: its mug shot and fingerprint (the others
 * hidden), its 8 fields and its field notes. */
static void ing_show_record(game *g, int i)
{
    u32 media[16];
    char text[160];
    int n, k;
    set_item_text(g, 58344, "");
    for (k = 0; k < (int)XF_COUNT(g_ing_names); k++) {
        vc_set_enabled(&g->interp, g_ing_names[k].mug, k == i);
        vc_set_enabled(&g->interp, g_ing_names[k].prints, k == i);
    }
    g->ing_notes = g_ing_names[i].notes ? g_ing_names[i].notes : 38133;
    g->ing_record = i; /* what Forward encloses */
    n = item_media(g, g_ing_names[i].record, media, 16);
    for (k = 0; k < 8; k++) {
        set_item_text(g, g_ing_fields[k], k < n ? game_text(g, media[k], text, sizeof(text)) : "");
        vc_set_enabled(&g->interp, g_ing_labels[k], 1);
    }
}

/* 0x464F70, name search: the key item of each record lists the
 * databases that hold it, then the names it answers to. */
static int ing_name(game *g, char db, const char *typed)
{
    char want[64], have[64], dbs[16];
    int i, k, n;
    ing_key(typed, want, sizeof(want));
    for (i = 0; i < (int)XF_COUNT(g_ing_names); i++) {
        u32 media[16];
        int match = 0;
        n = item_media(g, g_ing_names[i].key, media, 16);
        for (k = 1; k < n && !match; k++) {
            char t[64];
            ing_key(game_text(g, media[k], t, sizeof(t)), have, sizeof(have));
            match = strcmp(want, have) == 0;
            if (!match && original_text(g, media[k], t, sizeof(t))) {
                ing_key(t, have, sizeof(have));
                match = strcmp(want, have) == 0;
            }
        }
        if (!match)
            continue;
        /* Record 13, James Wong, answers only once variable 2107
         * (SHANKSN2_bDoneWithWong) is set; the ING then shows string 1104,
         * which this build's xfilest.dll does not have. */
        if (i == 13 && !vc_get_var(&g->interp, 2107))
            return 0;
        game_text(g, media[0], dbs, sizeof(dbs));
        if (!strchr(dbs, db))
            continue;
        xf_info("ING: name %s found in %c (record %d)", typed, db, i);
        ing_show_record(g, i);
        return 1;
    }
    return 0;
}

/* 0x514DE0, plate search: two plates are on file. The NSA car's plate
 * (media 88648) answers in the government database with the restricted
 * record (58350); Mulder and Scully's rental car (88650) answers among
 * civilians with MulderRentalResponse.txt (88652). */
static int ing_plate(game *g, char db, const char *typed)
{
    char want[32], have[32], t[160];
    ing_key(typed, want, sizeof(want));
    ing_key(game_text(g, 88648, t, sizeof(t)), have, sizeof(have));
    if (db == 'G' && strcmp(want, have) == 0) {
        set_item_text(g, 58344, game_text(g, 58350, t, sizeof(t)));
        set_item_size(g, 58344, 16); /* 0x514BB0, 0x514DE0: Courier New 16 */
        g->ing_notes = 0;
        /* Variable 4713 (IFL_bINGHaveSearchedForNSACar) is set the first
         * time; the original also adds 2 to a score then. */
        vc_set_var(&g->interp, 4713, 1);
        xf_info("ING: plate %s found in %c", typed, db);
        return 1;
    }
    ing_key(game_text(g, 88650, t, sizeof(t)), have, sizeof(have));
    if (db == 'Z' && strcmp(want, have) == 0) {
        g->ing_notes = 88652;
        xf_info("ING: plate %s found in %c", typed, db);
        return 1;
    }
    return 0;
}

/* 0x4B6620 fn_INGDisplayCook -> 0x514BB0: the fingerprint search found
 * Cook. The ING shows his record (number 3), and the first time it runs
 * list 90360; once variable 5095 is set it shows "Restricted Data". */
static void ing_display_cook(game *g)
{
    char t[160];
    if (vc_get_var(&g->interp, 5095)) {
        set_item_text(g, 58344, game_text(g, 58350, t, sizeof(t)));
        set_item_size(g, 58344, 16); /* 0x514BB0, 0x514DE0: Courier New 16 */
        return;
    }
    ing_show_record(g, 3);
    if (!g->ing_cook_shown) {
        g->ing_cook_shown = 1;
        vc_run_action_list(&g->interp, 90360);
    }
}

/* 0x464F70 ING_search(category, database): runs the search, then action
 * list 90694 when it found a record and 90038 when it did not. */
int game_ing_search(game *g, char type, char db, const char *text)
{
    int found = 0;
    if (type == 'P')
        found = ing_phone(g, text);
    else if (type == 'N')
        found = ing_name(g, db, text);
    else if (type == 'L')
        found = ing_plate(g, db, text);
    if (!found) {
        char t[64];
        set_item_text(g, 58344, game_text(g, ING_NOT_FOUND, t, sizeof(t)));
    }
    vc_run_action_list(&g->interp, found ? 90694 : 90038);
    after_triggers(g);
    return found;
}

/* --- Engine functions ------------------------------------------------ */

/* Day of the week for a date in the Gregorian calendar (0 = Sunday). */
static int weekday(int y, int m, int d)
{
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3)
        y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

/* Dates and times as each text language writes them; the original took
 * them from the Windows locale. */
typedef struct {
    const char *code;
    const char *wd[7], *mon[13];
    const char *date; /* weekday, day, month, year: %1$s %2$d %3$s */
    int clock24;
    char sep;         /* between hours and minutes */
} card_locale;

static const card_locale g_card_locales[] = {
    { "en",
      { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" },
      { "", "January", "February", "March", "April", "May", "June", "July", "August", "September", "October",
        "November", "December" },
      "WMD", 0, ':' },
    { "de",
      { "Sonntag", "Montag", "Dienstag", "Mittwoch", "Donnerstag", "Freitag", "Samstag" },
      { "", "Januar", "Februar", "M\xC3\xA4rz", "April", "Mai", "Juni", "Juli", "August", "September", "Oktober",
        "November", "Dezember" },
      "WD.M", 1, ':' },
    { "fr",
      { "dimanche", "lundi", "mardi", "mercredi", "jeudi", "vendredi", "samedi" },
      { "", "janvier", "f\xC3\xA9vrier", "mars", "avril", "mai", "juin", "juillet", "ao\xC3\xBBt", "septembre",
        "octobre", "novembre", "d\xC3\xA9" "cembre" },
      "wDM", 1, ':' },
    { "it",
      { "domenica", "luned\xC3\xAC", "marted\xC3\xAC", "mercoled\xC3\xAC", "gioved\xC3\xAC", "venerd\xC3\xAC",
        "sabato" },
      { "", "gennaio", "febbraio", "marzo", "aprile", "maggio", "giugno", "luglio", "agosto", "settembre", "ottobre",
        "novembre", "dicembre" },
      "wDM", 1, '.' },
    { "es",
      { "domingo", "lunes", "martes", "mi\xC3\xA9rcoles", "jueves", "viernes", "s\xC3\xA1" "bado" },
      { "", "enero", "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto", "septiembre", "octubre",
        "noviembre", "diciembre" },
      "WDdeM", 1, ':' },
    { "pt",
      { "Domingo", "Segunda-feira", "Ter\xC3\xA7" "a-feira", "Quarta-feira", "Quinta-feira", "Sexta-feira",
        "S\xC3\xA1" "bado" },
      { "", "janeiro", "fevereiro", "mar\xC3\xA7o", "abril", "maio", "junho", "julho", "agosto", "setembro",
        "outubro", "novembro", "dezembro" },
      "WDdeM", 1, ':' },
    { "jp",
      { "\xE6\x97\xA5\xE6\x9B\x9C\xE6\x97\xA5", "\xE6\x9C\x88\xE6\x9B\x9C\xE6\x97\xA5", "\xE7\x81\xAB\xE6\x9B\x9C\xE6\x97\xA5", "\xE6\xB0\xB4\xE6\x9B\x9C\xE6\x97\xA5", "\xE6\x9C\xA8\xE6\x9B\x9C\xE6\x97\xA5", "\xE9\x87\x91\xE6\x9B\x9C\xE6\x97\xA5", "\xE5\x9C\x9F\xE6\x9B\x9C\xE6\x97\xA5" },
      { "", "", "", "", "", "", "", "", "", "", "", "", "" },
      "YMDW", 1, ':' },
};

static const card_locale *card_locale_of(const game *g)
{
    size_t i;
    if (g->langs.text >= 0)
        for (i = 0; i < XF_COUNT(g_card_locales); i++)
            if (strcmp(g_card_locales[i].code, g->langs.langs[g->langs.text].code) == 0)
                return &g_card_locales[i];
    return &g_card_locales[0];
}

/* 0x4DB580: location label, then the date from iDayNight, then the time
 * from iCurrTime (HHMM) when `with_time` is set. The label is a string of
 * the text language (falling back to the reference's name). */
static void format_location_card(game *g, u32 label_ref, i32 day_night, i32 hhmm, int with_time)
{
    static const int days[8] = { 29, 2, 3, 4, 5, 6, 7, 8 }; /* 0x659C80 */
    const card_locale *lc = card_locale_of(g);
    char label[128] = "";
    const char *wd, *mon;
    int idx = day_night > 10 ? day_night - 10 : day_night, month, day, n;
    xf_neo_reader r;

    idx = XF_CLAMP(idx, 0, 7);
    month = idx == 0 ? 3 : 4;
    day = days[idx];
    game_text(g, label_ref, label, sizeof(label));
    if (!label[0] && vc_class(&g->db, label_ref) == VC_ASSET_REF &&
        xf_neo_object(&g->db, label_ref, &r, NULL, NULL) == 0) {
        xf_neo_blob(&g->db, &r);
        vc_read_name(&g->db, xf_neo_u32(&r), label, sizeof(label));
    }
    wd = lc->wd[weekday(1996, month, day)];
    mon = lc->mon[month];
    if (strcmp(lc->date, "WMD") == 0) /* Tuesday, April 2, 1996 */
        n = snprintf(g->card, sizeof(g->card), "%s\n%s, %s %d, 1996", label, wd, mon, day);
    else if (strcmp(lc->date, "WD.M") == 0) /* Dienstag, 2. April 1996 */
        n = snprintf(g->card, sizeof(g->card), "%s\n%s, %d. %s 1996", label, wd, day, mon);
    else if (strcmp(lc->date, "YMDW") == 0) /* 1996年4月2日 火曜日 */
        n = snprintf(g->card, sizeof(g->card), "%s\n1996%s%d%s%d%s %s", label, "\xE5\xB9\xB4", month, "\xE6\x9C\x88", day, "\xE6\x97\xA5", wd);
    else if (strcmp(lc->date, "WDdeM") == 0) /* Terça-feira, 2 de abril de 1996 */
        n = snprintf(g->card, sizeof(g->card), "%s\n%s, %d de %s de 1996", label, wd, day, mon);
    else /* mardi 2 avril 1996 */
        n = snprintf(g->card, sizeof(g->card), "%s\n%s %d %s 1996", label, wd, day, mon);
    if (with_time && n > 0 && (size_t)n < sizeof(g->card)) {
        int h = hhmm / 100, m = hhmm % 100;
        if (lc->clock24)
            snprintf(g->card + n, sizeof(g->card) - (size_t)n, "\n%d%c%02d", h, lc->sep, m);
        else
            snprintf(g->card + n, sizeof(g->card) - (size_t)n, "\n%d%c%02d %s", h % 12 ? h % 12 : 12, lc->sep, m,
                     h < 12 ? "AM" : "PM");
    }
}

static i32 arg_value(game *g, const vc_operand *o)
{
    if (o->kind == VC_OPND_VARIABLE)
        return vc_get_var(&g->interp, o->value);
    if (o->kind == VC_OPND_CONTEXT)
        return g->interp.context; /* the key or timer of the event */
    return (i32)o->value;
}

static int host_call_function(void *user, const vc_action *a)
{
    game *g = user;
    const char *name = a->u.function.name;
    const vc_operand *args = a->u.function.args;
    int argc = a->u.function.argc, i;

    /* 0x482E40 fn_StartVideoText: label media, day/night, time, show time.
     * The card shows over the clip queued just before. */
    if (strcmp(name, "StartVideoText") == 0 && argc == 4) {
        format_location_card(g, args[0].value, arg_value(g, &args[1]), arg_value(g, &args[2]),
                             arg_value(g, &args[3]) != 0);
        if (game_media_pending(g))
            g->queue[(g->queue_tail + GAME_MEDIA_QUEUE - 1) % GAME_MEDIA_QUEUE].card = 1;
        xf_info("location card (label %u): %s", args[0].value, g->card);
        return 1;
    }
    /* 0x486030 fn_SetStartView: node, location, viewpoint, view */
    if (strcmp(name, "SetStartView") == 0 && argc == 4) {
        g->pending_view = args[3].value;
        return 1;
    }
    /* 0x41AE20 fn_SetIFLAsView: the layout becomes part of the view */
    if (strcmp(name, "SetIFLAsView") == 0 && argc == 1) {
        for (i = 0; i < g->iface_count; i++)
            if (g->ifaces[i].layout == (u32)arg_value(g, &args[0]))
                g->ifaces[i].as_view = 1;
        return 1;
    }
    /* 0x499E40 fn_OnInvClick(item): take the item in hand */
    if (strcmp(name, "OnInvClick") == 0 && argc == 1) {
        set_reg_var(g, "RegCurrInvVar", 0, arg_value(g, &args[0]));
        return 1;
    }
    /* 0x467710 OnIntInvClick(tool): a tool such as the PDA or the phone
     * opens with an empty hand (sub_4CB190(0) drops the item). */
    if (strcmp(name, "OnIntInvClick") == 0 && argc == 1) {
        set_reg_var(g, "RegCurrInvVar", 0, game_reg_value(g, "RegCurrInvVar", 1));
        return 1;
    }
    /* 0x469030 INGDestroy: frees the ING search state. */
    if (strcmp(name, "INGDestroy") == 0 && argc == 0) {
        g->ing[0] = '\0';
        return 1;
    }
    /* SetIsNode2Done .. SetIsNode6Done (0x4BC370 ..): the engine decides
     * whether a part of the story is finished, from the variables that
     * RegNodeNVars registered, and sets the last one. */
    if (strncmp(name, "SetIsNode", 9) == 0 && strcmp(name + 10, "Done") == 0) {
        set_node_done(g, name[9] - '0');
        return 1;
    }
    /* 0x4BC330 fn_SetIsLink2Done, 0x4BC350 fn_SetIsLink3Done: in node 12
     * (or 13), the link is done when the first variable of RegLinkNVars
     * is set (0x4CA350, 0x4CA3F0). */
    if ((strcmp(name, "SetIsLink2Done") == 0 || strcmp(name, "SetIsLink3Done") == 0)) {
        char reg[16];
        int link = name[9] - '0';
        snprintf(reg, sizeof(reg), "RegLink%dVars", link);
        set_reg_var(g, reg, 1,
                    game_reg_value(g, "RegCurrNode", 0) == 10 + link && game_reg_value(g, reg, 0) == 1);
        return 1;
    }
    /* 0x4B6140 fn_FadeToBlack(event): 0x4EF8E0 fades the screen to black
     * over 2 seconds. */
    if (strcmp(name, "FadeToBlack") == 0) {
        g->fade_serial++;
        return 1;
    }
    /* 0x48A2B0 fn_FadeBack(on): the Help screen's parts call it on
     * mouse over (1) and off (0); the screen dims behind the part's
     * highlight and text (our drawing of 0x40BC30's screen layers). */
    if (strcmp(name, "FadeBack") == 0) {
        g->fade_back = argc > 0 && arg_value(g, &args[0]) != 0;
        return 1;
    }
    /* 0x460170 fn_SetAIList(trigger, action list): 0x4688D0 keeps the
     * pair as the Artificial Intuition hint. The icon glows while an
     * action of the list has a condition that holds (0x4FE820); a click
     * runs the list. fn_ClearAIList (0x4355D0) drops it. */
    if (strcmp(name, "SetAIList") == 0 && argc == 2) {
        g->ai_trigger = (u32)arg_value(g, &args[0]);
        g->ai_list = (u32)arg_value(g, &args[1]);
        return 1;
    }
    if (strcmp(name, "ClearAIList") == 0) {
        g->ai_trigger = g->ai_list = 0;
        return 1;
    }
    /* 0x47F970 fn_SetAIGlow(n): 0x47FD40 turns the icon's glow off (0)
     * or on (1, or a view id it is meant for). */
    if (strcmp(name, "SetAIGlow") == 0 && argc == 1) {
        g->ai_glow = arg_value(g, &args[0]);
        return 1;
    }
    /* 0x48AEF0 fn_SetLanguage(text, string var): 0x48AE30 writes the
     * text of the reference ("english", "german", ...) into the string the
     * variable names. Layouts compare it with scGameLanguageEnglish and
     * move items for the longer translations (RelocateIFace). */
    if (strcmp(name, "SetLanguage") == 0 && argc == 2) {
        g->language_text = (u32)arg_value(g, &args[0]);
        g->language_var = (u32)arg_value(g, &args[1]);
        apply_language(g);
        return 1;
    }
    /* 0x48A360 fn_FixJumpCut: does nothing in this build (0x48A470 ends
     * in an empty function). */
    if (strcmp(name, "FixJumpCut") == 0)
        return 1;
    /* 0x48A580 fn_PDAEscape(on): 0x47C750 runs one of two fixed lists.
     * 1 locks the PDA's travel (list 75533, trigger 75532: no leaving
     * during a scene), anything else unlocks it again (list 75530). */
    if (strcmp(name, "PDAEscape") == 0 && argc == 1) {
        if (arg_value(g, &args[0]) == 1)
            vc_run_trigger_list(&g->interp, 75532, 75533);
        else
            vc_run_trigger_list(&g->interp, 75524, 75530);
        return 1;
    }
    /* 0x4BCD40 fn_TextOn(on): 0x4F6370 shows or hides the two text
     * windows of the engine (app +0x104, +0x108): the scrolling text and
     * the text over clips. */
    if (strcmp(name, "TextOn") == 0 && argc == 1) {
        g->text_off = arg_value(g, &args[0]) == 0;
        return 1;
    }
    /* 0x4BC950 fn_StopEnvSounds: 0x4EF750 stops the sound beside the
     * clips (ambience, music). */
    if (strcmp(name, "StopEnvSounds") == 0) {
        if (g->sound) {
            g->sound = 0;
            g->sound_serial++;
        }
        return 1;
    }
    /* 0x4B5FA0 fn_EnablePDANote(media): 0x4CFE30 adds a note (a text
     * media) to the PDA's notes. */
    if (strcmp(name, "EnablePDANote") == 0 && argc == 1) {
        u32 note = (u32)arg_value(g, &args[0]);
        if (!in_list(g->pda_notes, g->pda_note_count, note) && g->pda_note_count < GAME_MAX_NOTES)
            g->pda_notes[g->pda_note_count++] = note;
        return 1;
    }
    /* 0x4B7350 fn_NotePadInit -> 0x4CF8E0, 0x4B72D0 fn_NoteFlipDay(back)
     * -> 0x4D0250: the PDA's field notes, a page at a time. */
    if (strcmp(name, "NotePadInit") == 0) {
        notes_show(g, 0);
        return 1;
    }
    if (strcmp(name, "NoteFlipDay") == 0 && argc == 1) {
        notes_show(g, arg_value(g, &args[0]) ? -1 : 1);
        return 1;
    }
    /* 0x4B5F00 fn_EnableAllNotes -> 0x4CFFE0: every note of every page
     * (a debug click on item 64022). */
    if (strcmp(name, "EnableAllNotes") == 0) {
        int p, k, n;
        u32 ids[64];
        for (p = 0; p < NOTE_PAGES; p++) {
            vc_iface_item it;
            if (vc_read_iface_item(&g->db, g_note_pages[p], &it) != 0)
                continue;
            n = vc_read_list(&g->db, it.assets, ids, 64);
            for (k = 0; k < n; k++)
                if (!in_list(g->pda_notes, g->pda_note_count, ids[k]) && g->pda_note_count < GAME_MAX_NOTES)
                    g->pda_notes[g->pda_note_count++] = ids[k];
        }
        return 1;
    }
    /* 0x459960 fn_RollHdCredits -> 0x45A290 -> 0x45B640: the opening
     * credits over the first scene. The credits object (app + 0x104,
     * vtable 0x65DEB0) reads its cards from XFilesC.dll (0x418E70); the
     * first is string 4 when the intro played (1-3 belong to it), else 1
     * (0x44E720). */
    if (strcmp(name, "RollHdCredits") == 0) {
        /* From a clip's start triggers (Cook leaving, after the answer to
         * Cook) the cards start with that clip; from an action list they
         * wait for the clips queued with it. */
        if (g->clip_starting && !g->credits_rolling)
            start_credits(g);
        else if (!g->credits_rolling)
            g->credits_pending = 1;
        g->credits_first = game_pref(g, 994) ? 1 : 4; /* Pref_bSkipIntro */
        return 1;
    }
    /* 0x4B6850 fn_IsoChamberStat(door A, door B, power, out x8): the
     * state of the isolation chamber for the scripts (0x50C5B0). Without
     * power (third argument not 1) outputs 1 and 4 are set; otherwise
     * output 3 or 2 says whether door B is open, and one of outputs 5 to
     * 8 names the combination of the two doors. */
    if (strcmp(name, "IsoChamberStat") == 0 && argc == 11) {
        i32 a = arg_value(g, &args[0]), b = arg_value(g, &args[1]), out[8] = { 0 };
        int i;
        if (arg_value(g, &args[2]) != 1) {
            out[0] = out[3] = 1;
        } else if (a && b) {
            out[2] = out[4] = 1;
        } else if (a) {
            out[1] = out[6] = 1;
        } else if (b) {
            out[2] = out[5] = 1;
        } else {
            out[1] = out[7] = 1;
        }
        for (i = 0; i < 8; i++)
            if (args[3 + i].kind == VC_OPND_VARIABLE)
                vc_set_var(&g->interp, args[3 + i].value, out[i]);
        return 1;
    }
    if (slot_function(g, name, argc, args))
        return 1;
    /* 0x4B5D70 fn_DrawGunRauch(state, letter) -> 0x4B0410: drawing the
     * gun on Rauch's porch while the NSA agents talk (state 1) starts the
     * gunfight: the letter becomes 'b' and view 39746 follows. */
    if (strcmp(name, "DrawGunRauch") == 0 && argc == 2) {
        if (arg_value(g, &args[0]) == 1) {
            if (args[1].kind == VC_OPND_VARIABLE)
                vc_set_var(&g->interp, args[1].value, 'b');
            host_go_to_view(g, 39746);
        }
        return 1;
    }
    /* 0x4B5780 fn_AlienBlast(n) -> 0x50C6B0: while the alien's blast plays,
     * moves already queued are dropped and two screen layers hide; the
     * argument is not used. */
    if (strcmp(name, "AlienBlast") == 0) {
        g->pending_view = 0;
        return 1;
    }
    /* 0x4BBE30 fn_RollOnEvKit(item, on) -> 0x49FDB0: with the pointer on
     * the evidence kit, the collected evidence pops up beside it
     * (0x503E40 with the kit's rectangle); it closes when the pointer
     * leaves both the kit and the popup. */
    if (strcmp(name, "RollOnEvKit") == 0 && argc == 2) {
        if (arg_value(g, &args[1]))
            g->kit_open = 1;
        else if (!g->pointer_on_evidence)
            g->kit_open = 0;
        return 1;
    }
    /* 0x4B6120 fn_EvKitIconClick -> 0x49FCA0: a click on the kit closes
     * the popup (0x503E40 with 0) along with any tool screen. */
    if (strcmp(name, "EvKitIconClick") == 0) {
        g->kit_open = 0;
        return 1;
    }
    /* 0x4BBB70 fn_RelocateIFacID(left, top, right, bottom, item): moves a
     * layout item, like RelocateIFace for another item than the one whose
     * trigger runs. */
    if (strcmp(name, "RelocateIFacID") == 0 && argc == 5) {
        game_iface_item *it = (game_iface_item *)find_item(g, (u32)arg_value(g, &args[4]));
        if (it) {
            it->item.region.left = (i16)arg_value(g, &args[0]);
            it->item.region.top = (i16)arg_value(g, &args[1]);
            it->item.region.right = (i16)arg_value(g, &args[2]);
            it->item.region.bottom = (i16)arg_value(g, &args[3]);
            g->iface_serial++;
        }
        return 1;
    }
    /* 0x495F40 fn_ReturnMainMenu, 0x4BBE10 fn_ReturnFromHelp: the main
     * menu (MainMenu_ctor through 0x48FC20 and 0x4BFEC0). */
    if (strcmp(name, "ReturnMainMenu") == 0 || strcmp(name, "ReturnFromHelp") == 0) {
        g->ui_request = GAME_UI_MENU;
        return 1;
    }
    /* Only in the catalogue list 42584, which no story trigger runs. */
    if (strcmp(name, "AddIFLExcept") == 0 || strcmp(name, "ClearIFLExcept") == 0 ||
        strcmp(name, "RefNVisionAss") == 0 || strcmp(name, "BreakPoint") == 0)
        return 1;
    /* Empty in this build: 0x4B6640 fn_InitAddrList (0x5069E0), 0x4B9C80
     * fn_PswdInit (0x506A00), 0x4B9CA0 fn_PwbkInit (0x506A10) and
     * 0x4BC5F0 fn_SmAttack (nullsub_12) only return. */
    if (strcmp(name, "InitAddrList") == 0 || strcmp(name, "PswdInit") == 0 || strcmp(name, "PwbkInit") == 0 ||
        strcmp(name, "SmAttack") == 0)
        return 1;
    if (strcmp(name, "SetShanksVar") == 0 && argc == 1) {
        if (args[0].kind == VC_OPND_VARIABLE)
            shanks_var(g, args[0].value);
        return 1;
    }
    /* 0x4B6520 fn_GetPlayFBIShot, 0x4B6260 fn_GetPlayBones, 0x4B6420
     * fn_GetPlayCancer (var): whether to play a clip that shows only once
     * a session. Each flag starts at 1 (0x6B579C, 0x6B57A0, 0x6B57A4) and
     * reads 1 the first time only. */
    if ((strcmp(name, "GetPlayFBIShot") == 0 || strcmp(name, "GetPlayBones") == 0 ||
         strcmp(name, "GetPlayCancer") == 0) && argc == 1) {
        int k = name[7] == 'F' ? 0 : name[7] == 'B' ? 1 : 2;
        if (args[0].kind == VC_OPND_VARIABLE)
            vc_set_var(&g->interp, args[0].value, g->play_once[k]);
        g->play_once[k] = 0;
        return 1;
    }
    /* 0x4B9B80 fn_PrepBombTime(timer, media) -> 0x4D0F80: the bomb's
     * countdown clip runs three minutes; while the timer runs, the clip
     * starts where the countdown is (180000 ms less the time left). */
    if (strcmp(name, "PrepBombTime") == 0 && argc == 2) {
        u8 id = (u8)arg_value(g, &args[0]);
        u32 media = (u32)arg_value(g, &args[1]);
        int k;
        for (k = 0; k < g->timer_count; k++)
            if (g->timers[k].id == id) {
                i32 left = (i32)(g->timers[k].due - g->now);
                u32 elapsed = left <= 0 ? 180000u : left >= 180000 ? 0u : 180000u - (u32)left;
                int j;
                for (j = 0; j < g->clip_offset_count && g->clip_offsets[j].asset != media; j++)
                    ;
                if (j == g->clip_offset_count && g->clip_offset_count < (int)XF_COUNT(g->clip_offsets))
                    g->clip_offset_count++;
                if (j < g->clip_offset_count) {
                    g->clip_offsets[j].asset = media;
                    g->clip_offsets[j].ms = elapsed;
                }
                break;
            }
        return 1;
    }
    /* 0x4BCA40 fn_SuicideNote(event) -> 0x4B0C00: one of three notes
     * (table 0x657930), in the ZonkersHand font at 14 points, on the
     * item whose trigger runs. ZonkersHand is not in the game data;
     * the window draws Courier. */
    if (strcmp(name, "SuicideNote") == 0) {
        static const u32 notes[3] = { 62868, 62870, 62872 };
        g->rand_seed = g->rand_seed * 1103515245u + 12345u;
        memset(&g->scroll, 0, sizeof(g->scroll));
        g->scroll.item = g->trigger_item;
        g->scroll.color = 'b';
        g->scroll.font = 'c';
        g->scroll.size = 14;
        email_text(g, notes[(g->rand_seed >> 16) % 3], g->scroll.text, sizeof(g->scroll.text));
        return 1;
    }
    /* 0x4BBFA0 fn_SaveGameState: a checkpoint (save.c). */
    if (strcmp(name, "SaveGameState") == 0) {
        game_checkpoint(g);
        return 1;
    }
    /* 0x4B6020 fn_EndGame(var): Willmore is dead or the case is lost.
     * 0x4EF5E0 offers to restore the last save when the variable is set
     * (string 0x27), else goes back to the main menu. */
    if (strcmp(name, "EndGame") == 0 && argc == 1) {
        g->game_over = 1 + (arg_value(g, &args[0]) != 0);
        if (g->game_over == 2 && g->checkpoint && game_pref(g, 992)) /* Pref_bAutoRewind */
            game_message(g, 39, GAME_ASK_RESTORE); /* "Try again?" */
        xf_info("game over");
        return 1;
    }
    /* 0x4B7370 fn_NukeAllIcons: takes every emotion and action icon off
     * the screen. */
    if (strcmp(name, "NukeAllIcons") == 0) {
        u32 i;
        for (i = 0; i < g->db.count; i++) {
            int c = g->db.entries[i].class_id;
            if (c == VC_EMOTION_ICON || c == VC_ACTION_ICON)
                vc_set_enabled(&g->interp, g->db.entries[i].id, 0);
        }
        return 1;
    }
    /* The Smolnikoff fight (see smol_next). */
    if (strcmp(name, "InitSmShootout") == 0 && argc == 3) {
        int i;
        for (i = 0; i < 3; i++) {
            g->smol_vars[i] = args[i].kind == VC_OPND_VARIABLE ? args[i].value : 0;
            g->smol_clear[i] = 0;
            g->smol_visits[i] = 0;
            if (g->smol_vars[i])
                vc_set_var(&g->interp, g->smol_vars[i], 0);
        }
        g->smol_area = 0;
        return 1;
    }
    if (strcmp(name, "DoSmolShootout") == 0) {
        smol_next(g);
        return 1;
    }
    /* 0x4B0A70 SmolGoonShot1: the area of the goon is clear. */
    if (strcmp(name, "SmolGoonShot1") == 0) {
        if (g->smol_area) {
            g->smol_clear[g->smol_area - 1] = 1;
            if (g->smol_vars[g->smol_area - 1])
                vc_set_var(&g->interp, g->smol_vars[g->smol_area - 1], 1);
        }
        return 1;
    }
    /* 0x4BC6D0 fn_SmGoonShoots(result, try, timer) -> 0x4B0460: on the
     * second and third try a goon may hit Willmore (result 1): 20, 40 or
     * 60 percent on the second, 50, 65 or 80 on the third, by difficulty
     * (var991 'E', 'S', 'H'), against a number from 0 to 100. A miss
     * queues an action that stops `timer`, the goon's shot, whose event
     * would take Willmore to his death. */
    if (strcmp(name, "SmGoonShoots") == 0 && argc == 3) {
        i32 tries = arg_value(g, &args[1]);
        char level = (char)vc_get_var(&g->interp, 991);
        int d = level == 'E' ? 0 : level == 'H' ? 2 : 1, hit = 0;
        static const int chance[2][3] = { { 20, 40, 60 }, { 50, 65, 80 } };
        g->rand_seed = g->rand_seed * 1103515245u + 12345u;
        if (tries == 2 || tries == 3)
            hit = (int)(g->rand_seed % 101) < chance[tries - 2][d];
        if (args[0].kind == VC_OPND_VARIABLE)
            vc_set_var(&g->interp, args[0].value, hit);
        if (!hit)
            host_timer(g, (u8)arg_value(g, &args[2]), 0, 1);
        return 1;
    }
    if (strcmp(name, "SmIsWillShot1") == 0 && argc == 1) {
        smol_will_shot(g, (u8)arg_value(g, &args[0]));
        return 1;
    }
    /* 0x47CF00 fn_DisplayDBox(character view): 0x4516D0 finds the
     * character on screen and opens its dialogue box, as a click would,
     * after a scene (a badge shown); 0 closes the box. */
    if (strcmp(name, "DisplayDBox") == 0 && argc == 1) {
        g->talking = (u32)arg_value(g, &args[0]);
        return 1;
    }
    /* 0x4B58E0 fn_CharTeleport(event, event, node, location, viewpoint,
     * view): 0x4EE100 queues a move to each of the four that differs
     * from where Willmore is, so it ends in that view. A character the
     * player clicks uses it to walk Willmore to where they talk. */
    if (strcmp(name, "CharTeleport") == 0 && argc == 6) {
        host_go_to_view(g, (u32)arg_value(g, &args[5]));
        return 1;
    }
    /* 0x4BCE10 fn_WillmoreSleeps: does nothing in this build. */
    if (strcmp(name, "WillmoreSleeps") == 0)
        return 1;
    /* 0x4B9DC0 fn_RandGen(var, n) and 0x4B9EF0 fn_RandGenRange(var, lo,
     * hi): a random value from 0 to n, or lo to hi, both included
     * (0x468550, the C library generator). */
    if ((strcmp(name, "RandGen") == 0 && argc == 2) || (strcmp(name, "RandGenRange") == 0 && argc == 3)) {
        i32 lo = argc == 3 ? arg_value(g, &args[1]) : 0, hi = arg_value(g, &args[argc - 1]);
        u32 span = hi >= lo ? (u32)(hi - lo) + 1 : 1;
        g->rand_seed = g->rand_seed * 1103515245u + 12345u;
        if (args[0].kind == VC_OPND_VARIABLE)
            vc_set_var(&g->interp, args[0].value, lo + (i32)(g->rand_seed % span));
        return 1;
    }
    if (compose_function(g, name, argc, args))
        return 1;
    if (email_function(g, name, argc, args))
        return 1;
    if (strcmp(name, "INGDisplayCook") == 0) {
        ing_display_cook(g);
        return 1;
    }
    /* 0x407BF0 fn_INGInit -> 0x407990: clears the search field (item
     * 50742) and hides item 36320. */
    if (strcmp(name, "INGInit") == 0) {
        g->ing[0] = '\0';
        set_item_text(g, 50742, "");
        vc_set_enabled(&g->interp, 36320, 0);
        return 1;
    }
    /* 0x47D220 fn_ScrollTextList(item, text, color, font, size):
     * 0x480850 shows a text file (field notes, help pages, the APB) in a
     * scrolling box on the item. fn_ScrollTextInit (0x45DA30) starts an
     * empty box, MainScroll (0x42FA50) scrolls a line up (0) or down (1)
     * or a page (2, 3), fn_ScrollTextKill (0x43E0D0) removes it. */
    if (strcmp(name, "ScrollTextList") == 0 && argc == 5) {
        memset(&g->scroll, 0, sizeof(g->scroll));
        g->scroll.item = (u32)arg_value(g, &args[0]);
        if (!email_text(g, (u32)arg_value(g, &args[1]), g->scroll.text, sizeof(g->scroll.text)))
            game_text(g, (u32)arg_value(g, &args[1]), g->scroll.text, sizeof(g->scroll.text));
        g->scroll.color = (char)arg_value(g, &args[2]);
        g->scroll.font = (char)arg_value(g, &args[3]);
        g->scroll.size = arg_value(g, &args[4]);
        keep_text(g, &g->scroll);
        return 1;
    }
    if (strcmp(name, "ScrollTextInit") == 0 && argc == 1) {
        memset(&g->scroll, 0, sizeof(g->scroll));
        g->scroll.item = (u32)arg_value(g, &args[0]);
        return 1;
    }
    if (strcmp(name, "MainScroll") == 0 && argc == 1) {
        static const int step[4] = { -1, 1, -8, 8 };
        i32 d = arg_value(g, &args[0]);
        if (d >= 0 && d < 4)
            g->scroll.line = XF_MAX(g->scroll.line + step[d], 0);
        return 1;
    }
    if (strcmp(name, "ScrollTextKill") == 0) {
        memset(&g->scroll, 0, sizeof(g->scroll));
        g->text_count = 0;
        return 1;
    }
    /* 0x491190 fn_PlayAssetAt(media, x, y): plays a small clip over the
     * screen, such as the ING's search progress bar; its start triggers
     * run now and its end triggers when it ends. StopPlayAssets(..)
     * ends it. */
    if (strcmp(name, "PlayAssetAt") == 0 && argc == 3) {
        vc_asset_ref a;
        u32 asset = (u32)arg_value(g, &args[0]);
        game_overlay_end(g);
        if (asset && vc_read_asset_ref(&g->db, asset, &a) == 0) {
            g->overlay = asset;
            g->overlay_x = arg_value(g, &args[1]);
            g->overlay_y = arg_value(g, &args[2]);
            g->overlay_serial++;
            vc_run_triggers(&g->interp, a.triggers, 8);
        }
        return 1;
    }
    if (strcmp(name, "StopPlayAssets") == 0) {
        game_overlay_end(g);
        return 1;
    }
    /* 0x4B5B40 fn_DispEvIcons(on): shows or hides the evidence bar */
    if (strcmp(name, "DispEvIcons") == 0 && argc == 1) {
        g->show_evidence = arg_value(g, &args[0]) != 0;
        return 1;
    }
    /* 0x4B5F20 fn_EnableKitIcon(evidence icon): 0x49FFE0 enables the
     * icon and shows it in the evidence kit. */
    if (strcmp(name, "EnableKitIcon") == 0 && argc == 1) {
        u32 id = (u32)arg_value(g, &args[0]);
        if (!in_list(g->kit_icons, g->kit_count, id) && g->kit_count < GAME_MAX_EVIDENCE)
            g->kit_icons[g->kit_count++] = id;
        vc_set_enabled(&g->interp, id, 1);
        return 1;
    }
    /* 0x4BCCC0 fn_TakePhoto(media): 0x4D8FE0 Camera_take_photo keeps
     * the picture in the camera, up to 24, for the computer to download. */
    if (strcmp(name, "TakePhoto") == 0 && argc == 1) {
        if (g->photo_count < GAME_MAX_PHOTOS) {
            g->photo_downloaded &= ~(1u << g->photo_count);
            g->photos[g->photo_count++] = (u32)arg_value(g, &args[0]);
        }
        return 1;
    }
    if (photo_function(g, name, argc, args))
        return 1;
    if (book_function(g, name, argc, args))
        return 1;
    if (pref_function(g, name, argc, args))
        return 1;
    /* 0x4B7930 fn_PDAInitAddr -> 0x4C9D40, 0x4B78B0 fn_PDAFlipAddr(mode)
     * -> 0x4C9AD0: the PDA's address book, a card at a time from the
     * enabled media of item 45307, shown in item 35320. Mode 0 is the
     * next card, 1 the previous, 4 reads the cards again; 2 and 3 turn
     * media 42906 and 42908 on and off. */
    if (strcmp(name, "PDAInitAddr") == 0 || (strcmp(name, "PDAFlipAddr") == 0 && argc == 1)) {
        u32 ids[64];
        int n = book_pages(g, 45307, ids, 64), mode = name[3] == 'I' ? -1 : arg_value(g, &args[0]);
        if (mode == 2 || mode == 3) {
            vc_set_enabled(&g->interp, 42906, mode == 2);
            vc_set_enabled(&g->interp, 42908, mode == 2);
            return 1;
        }
        if (mode == 0 && n > 1)
            g->pda_card = (g->pda_card + 1) % n;
        else if (mode == 1 && n > 1)
            g->pda_card = (g->pda_card + n - 1) % n;
        if (g->pda_card >= n)
            g->pda_card = 0;
        {
            char text[512] = "";
            game_item_label *l;
            if (n)
                game_text(g, ids[g->pda_card], text, sizeof(text));
            set_item_text(g, 35320, text);
            if ((l = (game_item_label *)game_item_label_of(g, 35320))) {
                l->size = 12;
                l->align = 'w';
            }
        }
        return 1;
    }
    /* 0x4BBEE0 fn_SBaseInit, 0x4BBF00 fn_SBaseKeyPress(key): the secret
     * base terminal's password box (layout 53263) shows a mark per letter
     * typed, 31 at most; OK always leads to layout 53268. */
    if (strcmp(name, "SBaseInit") == 0 || (strcmp(name, "SBaseKeyPress") == 0 && argc == 1)) {
        static const u32 marks[31] = {
            55838, 55840, 55842, 55844, 55846, 55848, 55850, 55852, 55854, 55856, 55858,
            55860, 55862, 55864, 55866, 55868, 55870, 55874, 55876, 55878, 55880, 55882,
            55884, 55886, 55888, 55890, 55892, 55894, 55896, 55898, 55900,
        };
        int k;
        if (name[5] == 'I')
            g->sbase_typed = 0;
        else {
            i32 key = arg_value(g, &args[0]);
            if (key == 8 && g->sbase_typed)
                g->sbase_typed--;
            else if (key >= 32 && key < 127 && g->sbase_typed < 31)
                g->sbase_typed++;
        }
        for (k = 0; k < 31; k++)
            vc_set_enabled(&g->interp, marks[k], k < g->sbase_typed);
        g->iface_serial++;
        return 1;
    }
    /* The computer's Media Index (layout 15208): 0x42CBF0 fn_MediaInit,
     * 0x475CD0 fn_MSKeyPressed, 0x41EB60 fn_MediaSearch -> 0x4654A0,
     * 0x42E180 fn_MediaDestroy. */
    if (strcmp(name, "MediaInit") == 0 || strcmp(name, "MediaDestroy") == 0) {
        g->media_query[0] = '\0';
        return 1;
    }
    if (strcmp(name, "MSKeyPressed") == 0 && argc >= 1) {
        i32 key = arg_value(g, &args[0]);
        size_t n = strlen(g->media_query);
        if (key == 8 && n)
            g->media_query[n - 1] = '\0';
        else if (key >= 32 && key < 127 && n + 1 < sizeof(g->media_query)) {
            g->media_query[n] = (char)key;
            g->media_query[n + 1] = '\0';
        }
        return 1;
    }
    if (strcmp(name, "MediaSearch") == 0) {
        media_search(g);
        return 1;
    }
    /* 0x4BC230 fn_SelectInv(inventory object): puts the item in hand.
     * The item's value is the argument of the On...Click call in its
     * click list (OnInvClick, OnCameraClick, OnFLightClick). */
    if (strcmp(name, "SelectInv") == 0 && argc == 1) {
        i32 v = inventory_value(g, (u32)arg_value(g, &args[0]));
        if (v)
            set_reg_var(g, "RegCurrInvVar", 0, v);
        return 1;
    }
    /* 0x4B7530 fn_OnFLightClick, 0x4B7460 fn_OnCameraClick, OnGunClick,
     * OnNVisionClick, OnBinocClick (item): take the tool in hand, like
     * OnInvClick; the tool then works on the view (the flashlight lights
     * dark views, the camera takes photos). */
    if ((strcmp(name, "OnFLightClick") == 0 || strcmp(name, "OnCameraClick") == 0 ||
         strcmp(name, "OnGunClick") == 0 || strcmp(name, "OnNVisionClick") == 0 ||
         strcmp(name, "OnBinocClick") == 0) && argc == 1) {
        set_reg_var(g, "RegCurrInvVar", 0, arg_value(g, &args[0]));
        /* 0x518C80: the binoculars at once show the close-up of the view,
         * if it has one; putting them away comes back. */
        if (strcmp(name, "OnBinocClick") == 0 && !g->binoc_from) {
            u32 cu = binocular_view(g);
            vc_view c;
            u32 media[8];
            if (cu && vc_read_view(&g->db, cu, &c) == 0) {
                /* The engine shows the close-up's picture whatever its
                 * enable state (it swaps the picture, 0x5192B0). */
                if (vc_read_list(&g->db, c.assets, media, 8) > 0)
                    vc_set_enabled(&g->interp, media[0], 1);
                g->binoc_from = g->view;
                g->pending_view = cu;
            }
        }
        return 1;
    }
    /* 0x44B4C0 fn_SetFLightProps(mode, 'I'nside or 'O'utside, 'M'):
     * mode 1 by day, 2 in the dark. */
    /* The exe (0x44B880) keeps the third argument too: the scene's own
     * light, 'B'right, 'M'edium or 'D'im, which with the brightness
     * preference picks one of five darkness levels (0x49EA10). */
    if (strcmp(name, "SetFLightProps") == 0 && argc == 3) {
        g->flight_mode = arg_value(g, &args[0]);
        g->flight_place = (char)arg_value(g, &args[1]);
        return 1;
    }
    /* 0x4BC410 fn_SetLBoxTint(r, g, b), 0x4B5AA0 fn_ClearLBoxTint: the
     * colour of the letterbox around the picture, darker at night. */
    if (strcmp(name, "SetLBoxTint") == 0 && argc == 3) {
        for (i = 0; i < 3; i++)
            g->lbox_tint[i] = (u8)arg_value(g, &args[i]);
        g->lbox_tinted = 1;
        return 1;
    }
    if (strcmp(name, "ClearLBoxTint") == 0) {
        g->lbox_tinted = 0;
        return 1;
    }
    /* 0x47D090 fn_LoginInit, 0x4B7250 fn_LoginClick, 0x46A000
     * fn_LoginKeyPress(field, key), 0x4598B0 fn_PswdConnect(result var),
     * fn_PwbkOK(result var): the FBI computer's login (fields 1 and 2,
     * LoginInit('C')) and Scully's PowerBook (fields 3 and 4,
     * LoginInit('P')). Both share the name and password buffers. */
    if (strcmp(name, "LoginInit") == 0) {
        memset(g->login, 0, sizeof(g->login));
        if (argc == 1 && arg_value(g, &args[0]) == 'P')
            powerbook_reset(g);
        return 1;
    }
    if (strcmp(name, "LoginClick") == 0)
        return 1; /* the clicked field takes the focus (click_iface_item) */
    if (strcmp(name, "LoginKeyPress") == 0 && argc == 2) {
        int field = arg_value(g, &args[0]) - 1, pb;
        i32 key = arg_value(g, &args[1]);
        size_t n;
        if (field < 0 || field > 3)
            return 1;
        pb = field >> 1;
        field &= 1;
        n = strlen(g->login[field]);
        /* 0x4F3500: Return runs the Connect button's list (FBI: trigger
         * 39475, list 39477; PowerBook: 39584, 39586); Tab moves between
         * the name and the password (FBI: items 50747 and 50748;
         * PowerBook: 50749 and 50750). */
        if (key == 13) {
            if (pb)
                vc_run_trigger_list(&g->interp, 39584, 39586);
            else
                vc_run_trigger_list(&g->interp, 39475, 39477);
            return 1;
        }
        if (key == 9) {
            g->focus_item = (pb ? 50749 : 50747) + (field == 0);
            return 1;
        }
        if (key == 8 && n)
            g->login[field][n - 1] = '\0';
        else if (key >= 32 && key < 127 && n + 1 < sizeof(g->login[0])) {
            g->login[field][n] = (char)key;
            g->login[field][n + 1] = '\0';
        }
        return 1;
    }
    if (strcmp(name, "PswdConnect") == 0 && argc == 1) {
        /* 0x459AB0 compares, ignoring case, with the texts of media
         * 57905 and 68915. A refused login shows a message (layout
         * 60115): one of four hidden name and password pairs, else
         * "wrong password" (67487), or after three tries with the right
         * name, 67500. Then the fields clear (0x468660). */
        static const u32 eggs[4][3] = {
            { 92074, 92076, 92089 }, /* dwight, xyzzy */
            { 92078, 92087, 92097 }, /* nickolai, jason */
            { 92083, 92087, 92105 }, /* angelina, jason */
            { 92085, 92087, 92113 }, /* twila, jason */
        };
        int ok = login_matches(g, 57905, 68915), k; /* "craig willmore", "shiloh" */
        xf_info("login %s / %s: %s", g->login[0], "******", ok ? "accepted" : "refused");
        if (args[0].kind == VC_OPND_VARIABLE)
            vc_set_var(&g->interp, args[0].value, ok);
        if (ok) {
            vc_set_var(&g->interp, 5327, 0); /* iFBIPasswordCount */
            vc_set_var(&g->interp, 4946, 1); /* IFL_bHasLoggedInToFBIComputer */
            return 1;
        }
        if (login_matches_name(g, 57905))
            vc_set_var(&g->interp, 5327, vc_get_var(&g->interp, 5327) + 1);
        for (k = 0; k < 4 && !login_matches(g, eggs[k][0], eggs[k][1]); k++)
            ;
        show_message_item(g, k < 4 ? eggs[k][2] : vc_get_var(&g->interp, 5327) < 3 ? 67487 : 67500);
        memset(g->login, 0, sizeof(g->login));
        return 1;
    }
    if (strcmp(name, "PwbkOK") == 0 && argc == 1) {
        /* 0x4F3070: "Dana Scully" (68913) and the text of 68910 open the
         * PowerBook (the result variable becomes 1). Every try counts
         * (var 5326) and shows a message: 67487, or 89627 from the
         * fourth try on. Then the fields reset (0x4F2C30). */
        int ok = login_matches(g, 68913, 68910);
        if (ok && args[0].kind == VC_OPND_VARIABLE)
            vc_set_var(&g->interp, args[0].value, 1);
        vc_set_var(&g->interp, 5326, vc_get_var(&g->interp, 5326) + 1);
        xf_info("PowerBook login %s: %s", g->login[0], ok ? "accepted" : "refused");
        show_message_item(g, vc_get_var(&g->interp, 5326) < 4 ? 67487 : 89627);
        powerbook_reset(g);
        return 1;
    }
    /* SetIItemText(item, text, color, font, size, bold, align): puts a
     * text (a media reference, usually a string of xfilest.dll) on a
     * layout item. */
    if (strcmp(name, "SetIItemText") == 0 && argc == 7) {
        u32 item = (u32)arg_value(g, &args[0]);
        game_item_label *l = (game_item_label *)game_item_label_of(g, item);
        if (!l && g->label_count < GAME_MAX_LABELS)
            l = &g->labels[g->label_count++];
        if (l) {
            l->item = item;
            game_text(g, (u32)arg_value(g, &args[1]), l->text, sizeof(l->text));
            l->color = (char)arg_value(g, &args[2]);
            l->font = (char)arg_value(g, &args[3]);
            l->size = arg_value(g, &args[4]);
            l->bold = (char)arg_value(g, &args[5]);
            l->align = (char)arg_value(g, &args[6]);
        }
        return 1;
    }
    /* RelocateIFace(left, top, right, bottom, event): moves the layout
     * item whose trigger runs. */
    if (strcmp(name, "RelocateIFace") == 0 && argc >= 4) {
        game_iface_item *it = (game_iface_item *)find_item(g, g->trigger_item);
        if (it) {
            it->item.region.left = (i16)arg_value(g, &args[0]);
            it->item.region.top = (i16)arg_value(g, &args[1]);
            it->item.region.right = (i16)arg_value(g, &args[2]);
            it->item.region.bottom = (i16)arg_value(g, &args[3]);
        }
        return 1;
    }
    /* 0x74162 SetIItemFocus(item): key presses go to that item */
    if (strcmp(name, "SetIItemFocus") == 0 && argc == 1) {
        g->focus_item = (u32)arg_value(g, &args[0]);
        return 1;
    }
    /* ING: typing, clearing and searching */
    if (strcmp(name, "INGKeyPressed") == 0 && argc >= 1) {
        i32 key = arg_value(g, &args[0]);
        size_t n = strlen(g->ing);
        if (key == 8 && n)
            g->ing[n - 1] = '\0';
        else if (key >= 32 && key < 127 && n + 1 < sizeof(g->ing)) {
            g->ing[n] = (char)key;
            g->ing[n + 1] = '\0';
        }
        return 1;
    }
    if (strcmp(name, "INGClear") == 0) {
        g->ing[0] = '\0';
        return 1;
    }
    if (strcmp(name, "INGSearch") == 0 && argc >= 1) {
        game_ing_search(g, (char)arg_value(g, &args[0]), argc > 1 ? (char)arg_value(g, &args[1]) : 'Z', g->ing);
        return 1;
    }
    /* Cell phone: keys, clear, send */
    if (strcmp(name, "PhoneKeyPress") == 0 && argc >= 1) {
        phone_key(g, (char)arg_value(g, &args[0]));
        return 1;
    }
    /* 0x4B7970 fn_PhoneButton(key, number, sound) -> 0x51F960: a keypad
     * button. Digits go on the number (13 at most), the button sound
     * plays. */
    if (strcmp(name, "PhoneButton") == 0 && argc == 3) {
        u32 sound = (u32)arg_value(g, &args[2]);
        phone_key(g, (char)arg_value(g, &args[0]));
        if (sound) {
            g->sound = sound;
            g->sound_serial++;
        }
        return 1;
    }
    if (strcmp(name, "PhoneClear") == 0) {
        g->phone[0] = '\0';
        phone_show(g, "");
        return 1;
    }
    /* 0x4B7EC0 fn_PhoneInit, 0x4B7D00 fn_PhoneDestroy */
    if (strcmp(name, "PhoneInit") == 0) {
        g->phone[0] = '\0';
        g->phone_replace = 0;
        phone_show(g, "");
        return 1;
    }
    if (strcmp(name, "PhoneDestroy") == 0)
        return 1;
    /* 0x4B80D0 fn_PhoneMenuInit -> 0x51F640, 0x4B7D20 fn_PhoneFlipName(down):
     * the address book, the enabled entries of item 45309's media. */
    if (strcmp(name, "PhoneMenuInit") == 0) {
        phone_book_show(g, 0);
        return 1;
    }
    if (strcmp(name, "PhoneFlipName") == 0 && argc == 2) {
        phone_book_show(g, arg_value(g, &args[0]) ? 1 : -1);
        return 1;
    }
    /* 0x4B8240 fn_PhoneSend(number, trigger var, list var) -> Phone_lookup
     * (0x51FCD0): a number in the book sets the call's trigger and list,
     * which the send key's kind 6 triggers then run (DoActionListID). Any
     * other number rings on and on (trigger 67415, list 67422). */
    if (strcmp(name, "PhoneSend") == 0 && argc == 3) {
        const game_iface_item *it = find_item(g, g->trigger_item);
        int r = phone_send(g, args[1].kind == VC_OPND_VARIABLE ? args[1].value : 0,
                           args[2].kind == VC_OPND_VARIABLE ? args[2].value : 0);
        if (r == 1) {
            if (it)
                g->pending_kind6 = it->item.triggers;
        } else if (r == 0)
            run_trigger_list_pair(g, 67415, 67422);
        return 1;
    }
    /* 0x4B5BC0 fn_DoActionListID(trigger, list): DoActionList with IDs
     * taken from variables */
    if (strcmp(name, "DoActionListID") == 0 && argc == 2) {
        run_trigger_list_pair(g, arg_value(g, &args[0]), arg_value(g, &args[1]));
        return 1;
    }
    /* DeselectInv: empty the hand */
    if (strcmp(name, "DeselectInv") == 0) {
        game_hold(g, NULL);
        return 1;
    }
    /* 0x4BCAE0 fn_SwapDisc(node, location, viewpoint, view, iCurrNode):
     * the last argument is the node variable itself (0x5016A0 takes the
     * variable); the original asks for the disc that holds the node when
     * it is not in the drive, then goes to the view. Every disc is
     * installed here. */
    if (strcmp(name, "SwapDisc") == 0 && argc == 5) {
        g->disc = arg_value(g, &args[4]);
        xf_info("travel, node %d", g->disc);
        host_go_to_view(g, (u32)arg_value(g, &args[3]));
        return 1;
    }
    /* 0x4BC930 fn_StopAllTimers */
    if (strcmp(name, "StopAllTimers") == 0) {
        g->timer_count = 0;
        return 1;
    }
    /* RegPreferences registers the 24 preference variables; the engine
     * fills them from the player's settings (game_prefs_apply). */
    if (strcmp(name, "RegPreferences") == 0)
        game_prefs_apply(g);
    if (strncmp(name, "Reg", 3) == 0) {
        /* A new game registers again: the same name replaces its entry. */
        game_registration *r = NULL;
        for (i = 0; i < g->reg_count && !r; i++)
            if (strncmp(g->regs[i].name, name, sizeof(g->regs[i].name)) == 0)
                r = &g->regs[i];
        if (!r && g->reg_count < (int)XF_COUNT(g->regs))
            r = &g->regs[g->reg_count++];
        if (!r) {
            note_missing(g, name);
            return 1;
        }
        memcpy(r->name, name, sizeof(r->name));
        r->argc = argc;
        for (i = 0; i < argc; i++)
            r->vars[i] = args[i].value;
        return 1;
    }
    note_missing(g, name);
    return 1;
}

/* --- Set-up ---------------------------------------------------------- */

static const char *text_from(game *g, const xf_strings *table, u32 asset, char *buf, size_t size);

const char *game_text(game *g, u32 asset, char *buf, size_t size)
{
    return text_from(g, &g->strings, asset, buf, size);
}

/* The release's own text of a string a translation replaced, or NULL:
 * search words match in both languages. */
static const char *original_text(game *g, u32 asset, char *buf, size_t size)
{
    if (!g->base_strings.count)
        return NULL;
    text_from(g, &g->base_strings, asset, buf, size);
    return buf[0] ? buf : NULL;
}

static const char *text_from(game *g, const xf_strings *table, u32 asset, char *buf, size_t size)
{
    xf_neo_reader r, blob;
    const u8 *p;
    char tok[2][64];
    int k;
    buf[0] = '\0';
    if (vc_class(&g->db, asset) != VC_ASSET_REF || xf_neo_object(&g->db, asset, &r, NULL, NULL) != 0)
        return buf;
    blob = xf_neo_blob(&g->db, &r);
    p = blob.p;
    /* Two Pascal tokens: the type ("6" for a string) and the value. */
    for (k = 0; k < 2 && p && p < blob.end; k++) {
        size_t n = *p++;
        if (p + n > blob.end)
            break;
        memcpy(tok[k], p, XF_MIN(n, sizeof(tok[k]) - 1));
        tok[k][XF_MIN(n, sizeof(tok[k]) - 1)] = '\0';
        p += n;
    }
    if (k == 2 && strcmp(tok[0], "6") == 0) {
        const char *t = xf_strings_get(table, (u32)atoi(tok[1]));
        snprintf(buf, size, "%s", t ? t : "");
    } else {
        /* A text reference: the "path" token is the text. */
        vc_asset_ref a;
        if (vc_read_asset_ref(&g->db, asset, &a) == 0)
            snprintf(buf, size, "%s", a.logical);
    }
    return buf;
}

const char *game_question_text(game *g, u32 conversation, char *buf, size_t size)
{
    vc_conversation c;
    u32 ids[4];
    buf[0] = '\0';
    if (vc_read_conversation(&g->db, conversation, &c) != 0)
        return buf;
    /* The "icon" list holds the line as Willmore says it (xfilest.dll);
     * the VCName is the writers' label. */
    if (vc_read_list(&g->db, c.icons, ids, 4) > 0)
        game_text(g, ids[0], buf, size);
    if (!buf[0])
        vc_read_name(&g->db, c.name, buf, size);
    return buf;
}

const game_item_label *game_item_label_of(game *g, u32 item)
{
    int i;
    for (i = 0; i < g->label_count; i++)
        if (g->labels[i].item == item)
            return &g->labels[i];
    return NULL;
}

int game_resolve(const game *g, const char *rel, char *out, size_t size)
{
    if (xf_langs_resolve(&g->langs, rel, out, size) == 0)
        return 0;
    return xf_path_resolve(g->data_dir, rel, out, size);
}

/* One string table of the text language: its DLL (or the nearest
 * fallback's), then the language's own text file over it. */
static void load_table(game *g, xf_strings *t, const char *dll, const char *txt)
{
    char path[1024];
    xf_strings_free(t);
    if (game_resolve(g, dll, path, sizeof(path)) != 0 || xf_strings_load(t, path) != 0)
        memset(t, 0, sizeof(*t));
    if (xf_langs_resolve_in(&g->langs, g->langs.text, txt, path, sizeof(path)) == 0)
        xf_strings_load_text(t, path);
}

/* The game's language word (string 1091: "english", "german", ...)
 * into the variable SetLanguage named. It must name the language of the
 * pictures: 88 layout actions move items when it is not English, for the
 * longer words of the localized pictures. A translation shows the
 * release's pictures, so the release's own word counts, not its own. */
static void apply_language(game *g)
{
    char text[64];
    if (!g->language_var)
        return;
    if (!original_text(g, g->language_text, text, sizeof(text)))
        game_text(g, g->language_text, text, sizeof(text));
    vc_string_set(&g->interp, g->language_var, text);
}

static void load_text(game *g)
{
    char path[1024];
    load_table(g, &g->strings, "XFILEST.DLL", "xfilest.txt");
    /* a translated table: the release's own kept for search words */
    xf_strings_free(&g->base_strings);
    if (xf_langs_resolve_in(&g->langs, g->langs.text, "xfilest.txt", path, sizeof(path)) == 0 &&
        (game_resolve(g, "XFILEST.DLL", path, sizeof(path)) != 0 || xf_strings_load(&g->base_strings, path) != 0))
        memset(&g->base_strings, 0, sizeof(g->base_strings));
    if (!g->strings.count)
        xf_warn("game: no xfilest.dll, interface texts are missing");
    load_table(g, &g->engine_strings, "XFILESE.DLL", "xfilese.txt"); /* empty: English fallbacks */
    load_table(g, &g->credit_strings, "XFILESC.DLL", "xfilesc.txt");
    load_table(g, &g->subliminal_strings, "XFILESS.DLL", "xfiless.txt");
}

void game_set_languages(game *g, int voices, int text)
{
    if (voices >= 0 && voices < g->langs.count && g->langs.langs[voices].voices)
        g->langs.voices = voices;
    if (text >= 0 && text < g->langs.count && g->langs.langs[text].text && text != g->langs.text) {
        g->langs.text = text;
        load_text(g);
        apply_language(g);
    }
    prefs_save(g);
}

int game_open(game *g, const char *data_dir)
{
    char path[1024];
    vc_host host = { NULL, host_go_to_view, host_play_media, host_timer, host_interface, host_call_function };
    memset(g, 0, sizeof(*g));
    snprintf(g->root_dir, sizeof(g->root_dir), "%s", data_dir);
    if (xf_langs_scan(&g->langs, data_dir) > 0) {
        data_dir = xf_langs_game_dir(&g->langs);
        xf_info("game: %d language folders, the game from %s", g->langs.count, data_dir);
    }
    snprintf(g->data_dir, sizeof(g->data_dir), "%s", data_dir);
    g->disc = 1;
    memset(g->play_once, 1, sizeof(g->play_once));
    host.user = g;
    if (xf_path_resolve(data_dir, "XFILES.HDB", path, sizeof(path)) != 0 || xf_neo_open(&g->db, path) != 0) {
        xf_error("game: cannot open XFILES.HDB in %s", data_dir);
        return -1;
    }
    {
        u32 i;
        for (i = 0; i < g->db.count; i++) {
            int c = g->db.entries[i].class_id;
            u32 id = g->db.entries[i].id;
            if ((c == VC_EMOTION_ICON || c == VC_ACTION_ICON) && g->all_icon_count < (int)XF_COUNT(g->all_icons))
                g->all_icons[g->all_icon_count++] = id;
            else if (c == VC_INVENTORY && g->all_inventory_count < (int)XF_COUNT(g->all_inventory))
                g->all_inventory[g->all_inventory_count++] = id;
            else if (c == VC_EVIDENCE_ICON && g->all_evidence_count < (int)XF_COUNT(g->all_evidence))
                g->all_evidence[g->all_evidence_count++] = id;
        }
    }
    if (xf_path_resolve(data_dir, "XFILES.GAM", path, sizeof(path)) != 0 || xf_neo_open(&g->gam, path) != 0 ||
        vc_interp_init(&g->interp, &g->db, &g->gam, &host) != 0) {
        xf_error("game: cannot load XFILES.GAM in %s", data_dir);
        xf_neo_close(&g->db);
        return -1;
    }
    g->interp.trace = getenv("XF_TRACE") != NULL;
    load_text(g);
    vc_place_map_build(&g->db, &g->places);
    xf_info("%u views placed in the hierarchy", g->places.count);
    game_prefs_load(g); /* the shipped values until save_dir is known */
    return 0;
}

void game_close(game *g)
{
    free(g->checkpoint);
    g->checkpoint = NULL;
    xf_strings_free(&g->strings);
    xf_strings_free(&g->base_strings);
    xf_strings_free(&g->engine_strings);
    xf_strings_free(&g->credit_strings);
    xf_strings_free(&g->subliminal_strings);
    vc_place_map_free(&g->places);
    vc_interp_free(&g->interp);
    xf_neo_close(&g->gam);
    xf_neo_close(&g->db);
}

/* Runs the start-up triggers of the title object: they register the
 * engine variables and call SetStartView. */
u32 game_run_title(game *g)
{
    u32 i, title = 0, start;
    xf_neo_reader r;
    for (i = 0; i < g->db.count && !title; i++)
        if (g->db.entries[i].class_id == VC_TITLE)
            title = g->db.entries[i].id;
    if (!title || xf_neo_object(&g->db, title, &r, NULL, NULL) != 0)
        return 0;
    /* VCTitle: list, string, date, string, string, u32, trigger list, ... */
    xf_neo_read_list(&r);
    xf_neo_blob(&g->db, &r);
    xf_neo_u32(&r);
    xf_neo_blob(&g->db, &r);
    xf_neo_blob(&g->db, &r);
    xf_neo_u32(&r);
    g->pending_view = 0;
    vc_run_triggers(&g->interp, xf_neo_u32(&r), 8);
    start = g->pending_view;
    g->pending_view = 0;
    game_prefs_apply(g);
    xf_info("title: %d engine registrations, start view %u", g->reg_count, start);
    return start;
}

int game_new(game *g)
{
    u32 start = game_run_title(g);
    return start ? game_enter_view(g, start) : -1;
}
