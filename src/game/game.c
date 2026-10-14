#include "game.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xf_file.h"
#include "xf_log.h"

static void after_triggers(game *g);

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
    depth--;
    for (i = 0; i < g->spot_count; i++)
        xf_debug("spot %u kind %d (%d,%d)-(%d,%d)", g->spots[i].id, g->spots[i].kind, g->spots[i].region.left,
                 g->spots[i].region.top, g->spots[i].region.right, g->spots[i].region.bottom);
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
static void after_triggers(game *g)
{
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
}

/* Media actions name a media list; its first entry plays. Sounds (paths
 * under Sounds/) play beside the clips instead of in the queue. */
static void host_play_media(void *user, u32 list, u8 selector)
{
    game *g = user;
    u32 ids[8];
    vc_asset_ref a;
    int n = vc_read_list(&g->db, list, ids, 8);
    (void)selector;
    if (n <= 0 || vc_read_asset_ref(&g->db, ids[0], &a) != 0) {
        xf_info("play media list #%u: empty or missing", list);
        return;
    }
    if (strncmp(a.logical, "Sounds/", 7) == 0) {
        g->sound = ids[0];
        g->sound_serial++;
        vc_run_triggers(&g->interp, a.triggers, 8);
        return;
    }
    game_queue_media(g, ids[0]);
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
        for (i = 0; i < d->count; i++)
            vc_run_triggers(&g->interp, d->items[i].item.triggers, 8);
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
            if (!best || item_order(it) >= item_order(best))
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
        return 1;
    }
    snprintf(name, sizeof(name), "Inventory_i%s", item);
    id = vc_find_var(&g->interp, name);
    if (!id)
        return 0;
    set_reg_var(g, "RegCurrInvVar", 0, vc_get_var(&g->interp, id));
    return 1;
}

int game_inventory(game *g, u32 *ids, int max)
{
    u32 i;
    int k = 0;
    for (i = 0; i < g->db.count && k < max; i++)
        if (g->db.entries[i].class_id == VC_INVENTORY && vc_is_enabled(&g->interp, g->db.entries[i].id))
            ids[k++] = g->db.entries[i].id;
    return k;
}

/* VCInventory: name, icon, highlighted icon, 0, cursor, trigger list.
 * A click runs the kind 0 triggers: OnInvClick takes the item in hand,
 * OnIntInvClick opens tools such as the PDA. */
int game_click_inventory(game *g, u32 id)
{
    xf_neo_reader r;
    u32 triggers;
    int i;
    if (vc_class(&g->db, id) != VC_INVENTORY || xf_neo_object(&g->db, id, &r, NULL, NULL) != 0)
        return 0;
    for (i = 0; i < 5; i++)
        xf_neo_u32(&r);
    triggers = xf_neo_u32(&r);
    xf_info("click inventory %u", id);
    vc_run_triggers(&g->interp, triggers, 0);
    after_triggers(g);
    return 1;
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
    return k;
}

void game_end_talk(game *g) { g->talking = 0; }

static void queue_list_media(game *g, u32 list, u32 conversation)
{
    u32 ids[4];
    if (vc_read_list(&g->db, list, ids, 4) > 0) {
        game_queue_media(g, ids[0]);
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

static void click_iface_item(game *g, const game_iface_item *it)
{
    u32 triggers = it->item.triggers;
    xf_info("click interface item %u", it->id);
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
        g->talking = s->id;
        xf_info("talk to %u: %d questions", s->id, game_choices(g, ids, GAME_MAX_CHOICES));
    }
    if (target) {
        if (game_enter_view(g, target) == 0 && g->history_len < GAME_HISTORY)
            g->history[g->history_len++] = from;
    } else {
        after_triggers(g);
    }
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
    return o->kind == VC_OPND_VARIABLE ? vc_get_var(&g->interp, o->value) : (i32)o->value;
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
    /* DeselectInv: empty the hand */
    if (strcmp(name, "DeselectInv") == 0) {
        game_hold(g, NULL);
        return 1;
    }
    /* 0x4BCAE0 fn_SwapDisc(node, location, viewpoint, view, disc): the
     * original asks for the disc when it is not in the drive, then goes
     * to the view. Every disc is installed here. */
    if (strcmp(name, "SwapDisc") == 0 && argc == 5) {
        g->disc = arg_value(g, &args[4]);
        xf_info("disc %d", g->disc);
        host_go_to_view(g, (u32)arg_value(g, &args[3]));
        return 1;
    }
    /* 0x4BC930 fn_StopAllTimers */
    if (strcmp(name, "StopAllTimers") == 0) {
        g->timer_count = 0;
        return 1;
    }
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

int game_open(game *g, const char *data_dir)
{
    char path[1024];
    vc_host host = { NULL, host_go_to_view, host_play_media, host_timer, host_interface, host_call_function };
    memset(g, 0, sizeof(*g));
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
    vc_place_map_build(&g->db, &g->places);
    xf_info("%u views placed in the hierarchy", g->places.count);
    return 0;
}

void game_close(game *g)
{
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
