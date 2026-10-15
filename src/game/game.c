/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf and contributors */
#include "game.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "xf_file.h"
#include "media/hot.h"
#include "xf_log.h"

static void after_triggers(game *g);
static int hand_empty(game *g);
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
            if (g->view_asset && vc_read_asset_ref(&g->db, g->view_asset, &a) == 0)
                vc_run_triggers(&g->interp, a.triggers, 9);
            g->view_asset = now;
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
            if (!has_spot_id(old, n, g->spots[i].id)) {
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
    u32 i;
    int k = 0;
    for (i = 0; i < g->db.count && k < max; i++) {
        u32 id = g->db.entries[i].id;
        if (g->db.entries[i].class_id == VC_EVIDENCE_ICON && id != AI_ICON && vc_is_enabled(&g->interp, id) &&
            !in_list(g->kit_icons, g->kit_count, id))
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
    return g->show_evidence ? collected_evidence(g, ids, max) : 0;
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
    int i;
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
            if (vc_read_asset_ref(&g->db, g->view_asset, &a) == 0)
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
        vc_run_triggers(&g->interp, a.triggers, 8);
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

/* Media actions name a media list. Sounds (paths under Sounds/) play
 * beside the clips instead of in the queue. */
static void host_play_media(void *user, u32 list, u8 selector)
{
    game *g = user;
    vc_asset_ref a;
    u32 asset = first_enabled(g, list);
    (void)selector;
    if (!asset || vc_read_asset_ref(&g->db, asset, &a) != 0) {
        xf_info("play media list #%u: nothing enabled", list);
        return;
    }
    if (strncmp(a.logical, "Sounds/", 7) == 0) {
        g->sound = asset;
        g->sound_serial++;
        vc_run_triggers(&g->interp, a.triggers, 8);
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
void game_tick(game *g, u32 now_ms)
{
    int i;
    g->now = now_ms;
    if (g->game_over)
        return; /* the game has ended: nothing runs on */
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
            for (k = 0; k < g->iface_count; k++)
                for (j = 0; j < g->ifaces[k].count; j++)
                    vc_run_timer_triggers(&g->interp, g->ifaces[k].items[j].item.triggers, id);
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

/* The front item with triggers under (x, y), searching the layouts from
 * the top one down. A close-up has a full-screen item behind its picture,
 * so it takes every click; the inventory bar only takes clicks on its
 * items. */
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
                (!it->item.assets == !best->item.assets && item_order(it) >= item_order(best)))
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
    u32 i;
    int k = 0;
    for (i = 0; i < g->db.count && k < max; i++) {
        int c = g->db.entries[i].class_id;
        if ((c == VC_EMOTION_ICON || c == VC_ACTION_ICON) && vc_is_enabled(&g->interp, g->db.entries[i].id))
            ids[k++] = g->db.entries[i].id;
    }
    return k;
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
    u32 i;
    int k = 0;
    for (i = 0; i < g->db.count && k < max; i++)
        if (g->db.entries[i].class_id == VC_INVENTORY && vc_is_enabled(&g->interp, g->db.entries[i].id))
            ids[k++] = g->db.entries[i].id;
    if (g->ai_list && k < max)
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
            if (a.valid && a.type == VC_ACT_FUNCTION &&
                strcmp(a.u.function.name, "LoginKeyPress") == 0 && a.u.function.argc == 2) {
                int field = (int)a.u.function.args[0].value - 1;
                size_t len;
                if (field < 0 || field > 1)
                    return NULL;
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
    vc_run_triggers(&g->interp, c.triggers, 0);
    vc_run_triggers(&g->interp, c.triggers, 8);
    if (c.question)
        queue_list_media(g, c.question, 0);
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

static void click_spot(game *g, const game_spot *s)
{
    u32 from = g->view, target = s->kind == SPOT_NAV ? s->target : 0, triggers = s->triggers;
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

static const game_iface_item *find_item(game *g, u32 id)
{
    int k, i;
    for (k = g->iface_count - 1; k >= 0; k--)
        for (i = 0; i < g->ifaces[k].count; i++)
            if (g->ifaces[k].items[i].id == id)
                return &g->ifaces[k].items[i];
    return NULL;
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

int game_click(game *g, int x, int y)
{
    const game_iface_item *it = game_iface_item_at(g, x, y);
    if (it) {
        game_iface_item copy = *it;
        click_iface_item(g, &copy);
        return 1;
    }
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
 * clip ends: 'r' a hit. Shots 3, 20, 26 and 27 pick random clips or end
 * the game; 0 to 5 also count hits. */
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
        if (xf_path_resolve(g->data_dir, name, path, sizeof(path)) == 0 && xf_hot_load(hot, path) == 0)
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
static int phone_send(game *g, u32 trigger_var, u32 list_var)
{
    size_t i;
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
    ok = phone_send(g, 4467, 4468);
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
static void ing_key(const char *in, char *out, size_t size)
{
    size_t k = 0;
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

/* The text of a mail file, in Windows-1252 as the game wrote it. */
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
        if (xf_path_resolve(g->data_dir, a.entries[i].path, path, sizeof(path)) == 0 && (f = fopen(path, "rb"))) {
            n = fread(out, 1, size - 1, f);
            fclose(f);
            break;
        }
    out[n] = '\0';
    for (i = 0; i < (int)n; i++)
        if (out[i] == '\r')
            out[i] = '\n';
    return n;
}

/* A header field ("From", "Subject") of a mail. */
static void email_field(game *g, u32 asset, const char *name, char *out, size_t size)
{
    char text[1024];
    const char *p;
    size_t k = strlen(name);
    out[0] = '\0';
    email_text(g, asset, text, sizeof(text));
    for (p = text; *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : p + strlen(p)) {
        if (strncmp(p, name, k) == 0 && p[k] == ':') {
            size_t n = strcspn(p + k + 1, "\n");
            const char *v = p + k + 1;
            while (*v == ' ' && n) {
                v++;
                n--;
            }
            snprintf(out, size, "%.*s", (int)n, v);
            return;
        }
        if (*p == '\n')
            break; /* end of the header */
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
    g->email_body_item = item;
    email_text(g, asset, g->email_body, sizeof(g->email_body));
    if (attachment < 0)
        return;
    if (item == 54950) { /* the computer */
        vc_set_enabled(&g->interp, 72713, 1);
        vc_set_enabled(&g->interp, 72710, 1);
        vc_set_enabled(&g->interp, computer[attachment][1], 1);
        vc_set_enabled(&g->interp, computer[attachment][2], 1);
        if (asset != 64527)
            vc_set_enabled(&g->interp, 69527, 0); /* no Delete */
        g->email_body_item = 72758;
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
        g->email_body_item = 72761;
    }
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
    /* Scrolling a mail's text, flushing pending mails and preloading the
     * icons only change what is drawn. */
    if (strcmp(name, "MainScroll") == 0 || strcmp(name, "ScrollTextKill") == 0 ||
        strcmp(name, "EmailFlushMess") == 0 || strcmp(name, "PreloadEmIcon") == 0)
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

const char *game_email_body(game *g, u32 *item)
{
    *item = g->email_body_item;
    return g->email_body;
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

/* 0x4DB580: location label, then the date from iDayNight, then the time
 * from iCurrTime (HHMM) when `with_time` is set. The original formats the
 * date and time with the Windows locale; this is the US English form. */
static void format_location_card(game *g, u32 label_ref, i32 day_night, i32 hhmm, int with_time)
{
    static const int days[8] = { 29, 2, 3, 4, 5, 6, 7, 8 }; /* 0x659C80 */
    static const char *const wd[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday",
                                       "Friday", "Saturday" };
    static const char *const mon[13] = { "", "January", "February", "March", "April", "May", "June",
                                         "July", "August", "September", "October", "November",
                                         "December" };
    char label[128] = "";
    int idx = day_night > 10 ? day_night - 10 : day_night, month, day, n;
    xf_neo_reader r;

    idx = XF_CLAMP(idx, 0, 7);
    month = idx == 0 ? 3 : 4;
    day = days[idx];
    if (vc_class(&g->db, label_ref) == VC_ASSET_REF && xf_neo_object(&g->db, label_ref, &r, NULL, NULL) == 0) {
        xf_neo_blob(&g->db, &r);
        vc_read_name(&g->db, xf_neo_u32(&r), label, sizeof(label));
    }
    n = snprintf(g->card, sizeof(g->card), "%s\n%s, %s %d, 1996", label, wd[weekday(1996, month, day)],
                 mon[month], day);
    if (with_time && n > 0 && (size_t)n < sizeof(g->card)) {
        int h = hhmm / 100, m = hhmm % 100;
        snprintf(g->card + n, sizeof(g->card) - (size_t)n, "\n%d:%02d %s", h % 12 ? h % 12 : 12, m,
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
        xf_info("location card: %s", g->card);
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
    /* 0x48A2B0 fn_FadeBack(on): 0x40BC30 shows or hides a set of screen
     * layers; not drawn yet. */
    if (strcmp(name, "FadeBack") == 0)
        return 1;
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
    /* fn_SetAIGlow(n): only changes how the icon is drawn. */
    if (strcmp(name, "SetAIGlow") == 0)
        return 1;
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
    /* 0x4B6020 fn_EndGame(var): Willmore is dead or the case is lost.
     * 0x4EF5E0 offers to restore the last save when the variable is set
     * (string 0x27), else goes back to the main menu. */
    if (strcmp(name, "EndGame") == 0 && argc == 1) {
        g->game_over = 1 + (arg_value(g, &args[0]) != 0);
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
    if (strcmp(name, "ScrollTextInit") == 0 || strcmp(name, "ScrollTextList") == 0)
        return 1;
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
        if (g->photo_count < GAME_MAX_PHOTOS)
            g->photos[g->photo_count++] = (u32)arg_value(g, &args[0]);
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
     * fn_LoginKeyPress(field, key), 0x4598B0 fn_PswdConnect(result var):
     * the FBI computer's login. Field 1 is the name, 2 the password. */
    if (strcmp(name, "LoginInit") == 0) {
        memset(g->login, 0, sizeof(g->login));
        return 1;
    }
    if (strcmp(name, "LoginClick") == 0)
        return 1; /* the clicked field takes the focus (click_iface_item) */
    if (strcmp(name, "LoginKeyPress") == 0 && argc == 2) {
        int field = arg_value(g, &args[0]) - 1;
        i32 key = arg_value(g, &args[1]);
        size_t n;
        if (field < 0 || field > 1)
            return 1;
        n = strlen(g->login[field]);
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
         * 57905 and 68915. */
        char want_name[64], want_pass[64];
        int ok;
        game_text(g, 57905, want_name, sizeof(want_name)); /* "craig willmore" */
        game_text(g, 68915, want_pass, sizeof(want_pass)); /* "shiloh" */
        ok = strcasecmp(g->login[0], want_name) == 0 && strcasecmp(g->login[1], want_pass) == 0;
        xf_info("login %s / %s: %s", g->login[0], "******", ok ? "accepted" : "refused");
        if (args[0].kind == VC_OPND_VARIABLE)
            vc_set_var(&g->interp, args[0].value, ok);
        if (ok) {
            vc_set_var(&g->interp, 5327, 0); /* iFBIPasswordCount */
            vc_set_var(&g->interp, 4946, 1); /* IFL_bHasLoggedInToFBIComputer */
        } else {
            vc_set_var(&g->interp, 5327, vc_get_var(&g->interp, 5327) + 1);
        }
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
        i32 key = arg_value(g, &args[0]);
        size_t n = strlen(g->phone);
        if (key >= '0' && key <= '9' && n + 1 < sizeof(g->phone)) {
            g->phone[n] = (char)key;
            g->phone[n + 1] = '\0';
        }
        return 1;
    }
    if (strcmp(name, "PhoneClear") == 0) {
        g->phone[0] = '\0';
        return 1;
    }
    if (strcmp(name, "PhoneSend") == 0 && argc == 3) {
        phone_send(g, args[1].kind == VC_OPND_VARIABLE ? args[1].value : 0,
                   args[2].kind == VC_OPND_VARIABLE ? args[2].value : 0);
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
     * fills them from the player's settings. We keep the GAM defaults,
     * except Artificial Intuition (argument 1), which is on: Disc 1
     * cannot go on without it (Cook only takes the case files with it). */
    if (strcmp(name, "RegPreferences") == 0 && argc > 1)
        vc_set_var(&g->interp, args[1].value, 1);
    if (strncmp(name, "Reg", 3) == 0 && g->reg_count < (int)XF_COUNT(g->regs)) {
        game_registration *r = &g->regs[g->reg_count++];
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

const char *game_text(game *g, u32 asset, char *buf, size_t size)
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
        const char *t = xf_strings_get(&g->strings, (u32)atoi(tok[1]));
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

int game_open(game *g, const char *data_dir)
{
    char path[1024];
    vc_host host = { NULL, host_go_to_view, host_play_media, host_timer, host_interface, host_call_function };
    memset(g, 0, sizeof(*g));
    snprintf(g->data_dir, sizeof(g->data_dir), "%s", data_dir);
    g->disc = 1;
    host.user = g;
    if (xf_path_resolve(data_dir, "XFILES.HDB", path, sizeof(path)) != 0 || xf_neo_open(&g->db, path) != 0) {
        xf_error("game: cannot open XFILES.HDB in %s", data_dir);
        return -1;
    }
    if (xf_path_resolve(data_dir, "XFILES.GAM", path, sizeof(path)) != 0 || xf_neo_open(&g->gam, path) != 0 ||
        vc_interp_init(&g->interp, &g->db, &g->gam, &host) != 0) {
        xf_error("game: cannot load XFILES.GAM in %s", data_dir);
        xf_neo_close(&g->db);
        return -1;
    }
    g->interp.trace = getenv("XF_TRACE") != NULL;
    if (xf_path_resolve(data_dir, "XFILEST.DLL", path, sizeof(path)) != 0 || xf_strings_load(&g->strings, path) != 0)
        xf_warn("game: no xfilest.dll, interface texts are missing");
    vc_place_map_build(&g->db, &g->places);
    xf_info("%u views placed in the hierarchy", g->places.count);
    return 0;
}

void game_close(game *g)
{
    xf_strings_free(&g->strings);
    vc_place_map_free(&g->places);
    vc_interp_free(&g->interp);
    xf_neo_close(&g->gam);
    xf_neo_close(&g->db);
}

/* Runs the start-up triggers of the title object: they register the
 * engine variables and call SetStartView. */
int game_new(game *g)
{
    u32 i, title = 0, start;
    xf_neo_reader r;
    for (i = 0; i < g->db.count && !title; i++)
        if (g->db.entries[i].class_id == VC_TITLE)
            title = g->db.entries[i].id;
    if (!title || xf_neo_object(&g->db, title, &r, NULL, NULL) != 0)
        return -1;
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
    xf_info("new game: %d engine registrations, start view %u", g->reg_count, start);
    return start ? game_enter_view(g, start) : -1;
}
