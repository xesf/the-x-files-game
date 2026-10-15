/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf and contributors */
/* Story scripts: steps of play (walk, click, ask, wait...) and checks,
 * run against the game logic. xftool play runs them headless; the
 * explorer runs them in its window and can take screenshots. See
 * docs/TESTING.md for the commands. */
#include "script.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xf_log.h"

/* Plays every queued clip through: start triggers, then end triggers. */
static void drain_media(game *g)
{
    game_media m;
    int guard = 0;
    while (guard++ < 256) {
        if (game_media_start(g, &m))
            game_media_end(g);
        else if (g->sound)
            game_sound_end(g);
        else
            break;
    }
}

/* Short name of a view: the production path of its first enabled media. */
/* Clips end at once unless the host plays them. */
static void settle(script *s)
{
    if (s->host.settle)
        s->host.settle(s->host.user);
    else
        drain_media(s->g);
}

/* Lets time pass in small steps, so timers fire in order. */
static void advance(script *s, u32 ms)
{
    u32 end = s->now + ms;
    if (s->host.advance) {
        /* The window runs the clock and plays the clips. */
        s->host.advance(s->host.user, ms);
        s->now = end;
        return;
    }
    while (s->now < end) {
        u32 step = XF_MIN(end - s->now, 100u);
        s->now += step;
        game_tick(s->g, s->now);
        settle(s);
    }
}

static void view_name(game *g, u32 view, char *out, size_t size)
{
    u32 ids[8], saved = g->view;
    vc_asset_ref a;
    int n;
    g->view = view;
    n = game_view_media(g, ids, 8);
    g->view = saved;
    snprintf(out, size, "?");
    if (n > 0 && vc_read_asset_ref(&g->db, ids[0], &a) == 0) {
        const char *p = a.logical;
        if (strncmp(p, "Navs/", 5) == 0)
            p += 5;
        snprintf(out, size, "%s", p);
    }
}

static void print_state(game *g)
{
    int i, k;
    char name[160];
    view_name(g, g->view, name, sizeof(name));
    printf("  view %u (%s), %d hotspots\n", g->view, name, g->spot_count);
    for (i = 0; i < g->spot_count; i++) {
        const game_spot *s = &g->spots[i];
        if (s->kind == SPOT_NAV) {
            view_name(g, s->target, name, sizeof(name));
            printf("    move %u -> view %u (%s)\n", s->id, s->target, name);
        } else {
            printf("    %s %u at (%d,%d)\n", s->kind == SPOT_EXPLORE ? "object" : "character", s->id,
                   s->region.left, s->region.top);
        }
    }
    {
        u32 ids[64];
        int n = game_inventory(g, ids, 64);
        printf("  inventory:");
        for (i = 0; i < n; i++) {
            char nm[64] = "";
            xf_neo_reader r;
            if (xf_neo_object(&g->db, ids[i], &r, NULL, NULL) == 0)
                vc_read_name(&g->db, xf_neo_u32(&r), nm, sizeof(nm));
            printf(" %u:%s", ids[i], nm);
        }
        printf("\n");
        n = game_evidence(g, ids, 64);
        if (n) {
            printf("  evidence:");
            for (i = 0; i < n; i++) {
                char nm[64] = "";
                xf_neo_reader r;
                if (xf_neo_object(&g->db, ids[i], &r, NULL, NULL) == 0)
                    vc_read_name(&g->db, xf_neo_u32(&r), nm, sizeof(nm));
                printf(" %u:%s", ids[i], nm);
            }
            printf("\n");
        }
        if (g->photo_count) {
            printf("  photos:");
            for (i = 0; i < g->photo_count; i++)
                printf(" %u", g->photos[i]);
            printf("\n");
        }
    }
    {
        u32 ids[16];
        int n = game_icons(g, ids, 16);
        if (n) {
            printf("  icons:");
            for (i = 0; i < n; i++) {
                game_icon ic;
                char nm[96] = "";
                if (game_read_icon(g, ids[i], &ic) == 0)
                    vc_read_name(&g->db, ic.name, nm, sizeof(nm));
                printf(" %u:%s", ids[i], nm);
            }
            printf("\n");
        }
    }
    if (g->talking) {
        u32 ids[GAME_MAX_CHOICES];
        int n = game_choices(g, ids, GAME_MAX_CHOICES);
        printf("  talking to %u, %d questions\n", g->talking, n);
        for (i = 0; i < n; i++) {
            vc_conversation c;
            char q[128] = "";
            if (vc_read_conversation(&g->db, ids[i], &c) == 0)
                vc_read_name(&g->db, c.name, q, sizeof(q));
            printf("    ask %u  %s%s\n", ids[i], q, game_times_asked(g, ids[i]) ? "  (asked)" : "");
        }
    }
    for (k = 0; k < g->iface_count; k++) {
        printf("  interface %u:", g->ifaces[k].layout);
        for (i = 0; i < g->ifaces[k].count; i++)
            if (g->ifaces[k].items[i].item.triggers && vc_is_enabled(&g->interp, g->ifaces[k].items[i].id))
                printf(" %u", g->ifaces[k].items[i].id);
        printf("\n");
    }
}

static int has_spot(const game *g, u32 id)
{
    int i;
    for (i = 0; i < g->spot_count; i++)
        if (g->spots[i].id == id || (g->spots[i].kind == SPOT_NAV && g->spots[i].target == id))
            return 1;
    return 0;
}

static int has_iface(const game *g, u32 id)
{
    int k;
    for (k = 0; k < g->iface_count; k++)
        if (g->ifaces[k].layout == id)
            return 1;
    return 0;
}

/* Walks to `target` through the moves that are enabled now: a breadth-
 * first search over the views, then one click per move. */
/* Shortest list of moves from the current view to a view, through the
 * moves that are enabled now, or through every move when `any` is set.
 * Returns the number of moves, or -1 when there is no way. */
static int find_path(game *g, u32 target, int any, u32 *path, int max)
{
    enum { MAXV = 4096 };
    static u32 queue[MAXV], prev[MAXV], via[MAXV];
    int head = 0, tail = 0, i, k, found = -1, n = 0;
    queue[tail] = g->view;
    prev[tail++] = (u32)-1;
    while (head < tail && found < 0) {
        int cur = head++;
        vc_view v;
        u32 navs[64];
        int m;
        if (queue[cur] == target) {
            found = cur;
            break;
        }
        if (vc_read_view(&g->db, queue[cur], &v) != 0)
            continue;
        m = vc_read_list(&g->db, v.navs, navs, 64);
        for (k = 0; k < m && tail < MAXV; k++) {
            vc_nav nav;
            int seen = 0;
            if ((!any && !vc_is_enabled(&g->interp, navs[k])) || vc_read_nav(&g->db, navs[k], &nav) != 0)
                continue;
            for (i = 0; i < tail && !seen; i++)
                seen = queue[i] == nav.target;
            if (seen)
                continue;
            queue[tail] = nav.target;
            prev[tail] = (u32)cur;
            via[tail++] = navs[k];
        }
    }
    if (found < 0)
        return -1;
    for (i = found; prev[i] != (u32)-1; i = (int)prev[i])
        n++;
    if (n > max)
        return -1;
    i = found;
    for (k = n; k-- > 0; i = (int)prev[i])
        path[k] = via[i];
    return n;
}

static int walk(script *s, u32 target)
{
    u32 path[256];
    game *g = s->g;
    int n = find_path(g, target, 0, path, 256), i;
    if (n < 0)
        return 0;
    for (i = 0; i < n; i++) {
        printf("  move %u\n", path[i]);
        if (!game_click_object(g, path[i]))
            return 0;
        settle(s);
    }
    return g->view == target;
}

/* Prints the shortest way to a view over every move, enabled or not,
 * with the GAM enable state that gates each closed move. */
static void route(game *g, u32 target)
{
    u32 path[256];
    int n = find_path(g, target, 1, path, 256), i;
    u32 k;
    if (n < 0) {
        printf("  no way to view %ld\n", (long)target);
        return;
    }
    for (i = 0; i < n; i++) {
        vc_nav nav;
        vc_read_nav(&g->db, path[i], &nav);
        printf("  move %u -> view %u", path[i], nav.target);
        if (!vc_is_enabled(&g->interp, path[i]))
            for (k = 0; k < g->interp.enable_count; k++)
                if (g->interp.enables[k].target == path[i])
                    printf("  (off: enable #%u)", g->interp.enables[k].id);
        printf("\n");
    }
}

/* Talks to a character and asks every question that has not been asked
 * yet, again and again while answers open new ones. Timers get a few
 * seconds between rounds, since some questions open on a timer. */
static int ask_all(script *s, u32 character)
{
    game *g = s->g;
    int round, asked = 0;
    for (round = 0; round < 16; round++) {
        u32 ids[GAME_MAX_CHOICES];
        int n, i, any = 0;
        if (!game_click_object(g, character))
            return asked > 0;
        settle(s);
        n = game_choices(g, ids, GAME_MAX_CHOICES);
        for (i = 0; i < n; i++)
            if (!game_times_asked(g, ids[i])) {
                char q[128] = "";
                vc_conversation c;
                if (vc_read_conversation(&g->db, ids[i], &c) == 0)
                    vc_read_name(&g->db, c.name, q, sizeof(q));
                printf("  ask %u  %s\n", ids[i], q);
                game_ask(g, ids[i]);
                settle(s);
                asked++;
                any = 1;
                break; /* the choices may change after each answer */
            }
        if (!any) {
            int before = n;
            advance(s, 3000);
            if (!game_click_object(g, character))
                break;
            settle(s);
            n = game_choices(g, ids, GAME_MAX_CHOICES);
            for (i = 0; i < n && !any; i++)
                any = !game_times_asked(g, ids[i]);
            if (!any && n == before)
                break;
        }
    }
    return 1;
}

/* Lists the views reachable through the moves enabled now, with their
 * objects and characters. */
static void reach(game *g)
{
    enum { MAXV = 512 };
    static u32 queue[MAXV];
    int head = 0, tail = 0, i;
    queue[tail++] = g->view;
    while (head < tail) {
        u32 v = queue[head++], navs[64], objs[64];
        vc_view vw;
        char name[160];
        int k, m;
        if (vc_read_view(&g->db, v, &vw) != 0)
            continue;
        view_name(g, v, name, sizeof(name));
        printf("  view %u (%s)", v, name);
        m = vc_read_list(&g->db, vw.explorables, objs, 64);
        for (k = 0; k < m; k++)
            if (vc_is_enabled(&g->interp, objs[k]))
                printf(" object:%u", objs[k]);
        m = vc_read_list(&g->db, vw.characters, objs, 64);
        for (k = 0; k < m; k++)
            if (vc_is_enabled(&g->interp, objs[k]))
                printf(" character:%u", objs[k]);
        printf("\n");
        m = vc_read_list(&g->db, vw.navs, navs, 64);
        for (k = 0; k < m && tail < MAXV; k++) {
            vc_nav nav;
            int seen = 0;
            if (!vc_is_enabled(&g->interp, navs[k]) || vc_read_nav(&g->db, navs[k], &nav) != 0)
                continue;
            for (i = 0; i < tail && !seen; i++)
                seen = queue[i] == nav.target;
            if (!seen)
                queue[tail++] = nav.target;
        }
    }
}


/* --- Probe -------------------------------------------------------------
 * Tries every object and character the player can reach now, one at a
 * time, from a snapshot of the game: walks there, clicks it (with an item
 * in hand if one is named), and reports the clips it queued, how many
 * variables and enable states changed, and the view it ends in. The game
 * is restored after each try. */

typedef struct {
    game *copy;
    vc_var *vars;
    vc_enable *enables;
    void *tlocals;
} snapshot;

static int snap_take(game *g, snapshot *sn)
{
    sn->copy = malloc(sizeof(*g));
    sn->vars = malloc(sizeof(vc_var) * g->interp.var_count + 1);
    sn->enables = malloc(sizeof(vc_enable) * g->interp.enable_count + 1);
    sn->tlocals = malloc(vc_trigger_locals_size() * g->interp.tlocal_count + 1);
    if (!sn->copy || !sn->vars || !sn->enables || !sn->tlocals)
        return -1;
    *sn->copy = *g;
    memcpy(sn->tlocals, g->interp.tlocals, vc_trigger_locals_size() * g->interp.tlocal_count);
    memcpy(sn->vars, g->interp.vars, sizeof(vc_var) * g->interp.var_count);
    memcpy(sn->enables, g->interp.enables, sizeof(vc_enable) * g->interp.enable_count);
    return 0;
}

static void snap_restore(game *g, const snapshot *sn)
{
    /* The trigger parameters may have grown (and moved) since. */
    void *tl = g->interp.tlocals;
    u32 cap = g->interp.tlocal_cap;
    *g = *sn->copy; /* the variable and enable arrays are the same */
    g->interp.tlocals = tl;
    g->interp.tlocal_cap = cap;
    memcpy(g->interp.tlocals, sn->tlocals, vc_trigger_locals_size() * g->interp.tlocal_count);
    memcpy(g->interp.vars, sn->vars, sizeof(vc_var) * g->interp.var_count);
    memcpy(g->interp.enables, sn->enables, sizeof(vc_enable) * g->interp.enable_count);
}

static void snap_free(snapshot *sn)
{
    free(sn->copy);
    free(sn->vars);
    free(sn->enables);
    free(sn->tlocals);
}

static void probe_one(script *s, const snapshot *sn, u32 view, u32 id, const char *item)
{
    game *g = s->g;
    u32 path[256];
    int n = find_path(g, view, 0, path, 256), i, vars = 0, en = 0;
    char clips[1024] = "";
    if (n < 0)
        return;
    for (i = 0; i < n; i++) {
        game_click_object(g, path[i]);
        drain_media(g);
    }
    if (g->view != view)
        return;
    if (item && !game_hold(g, item))
        return;
    game_click_object(g, id);
    for (i = g->queue_head; i != g->queue_tail; i = (i + 1) % GAME_MEDIA_QUEUE) {
        vc_asset_ref a;
        if (vc_read_asset_ref(&g->db, g->queue[i].asset, &a) == 0) {
            /* The media's name describes it: "Video:Node 3:...:Willmore
             * finds stolen painkillers". Show the last part. */
            char desc[160] = "", *last;
            const char *base = strrchr(a.logical, '/');
            vc_read_name(&g->db, a.name & 0xFFFFFF, desc, sizeof(desc));
            last = strrchr(desc, ':');
            snprintf(clips + strlen(clips), sizeof(clips) - strlen(clips), " [%s%s%s]", base ? base + 1 : a.logical,
                     *desc ? " " : "", last ? last + 1 : desc);
        }
    }
    drain_media(g);
    for (i = 0; i < (int)g->interp.var_count; i++)
        vars += g->interp.vars[i].value != sn->vars[i].value;
    for (i = 0; i < (int)g->interp.enable_count; i++)
        en += g->interp.enables[i].enabled != sn->enables[i].enabled;
    printf("  %u at view %u:%s  vars %d, enables %d%s", id, view, clips, vars, en,
           g->view != view ? "" : "\n");
    if (g->view != view) {
        char name[160];
        view_name(g, g->view, name, sizeof(name));
        printf(" -> view %u (%s)\n", g->view, name);
    }
}

static void probe(script *s, const char *item)
{
    enum { MAXV = 512 };
    static u32 queue[MAXV];
    game *g = s->g;
    snapshot sn;
    int head = 0, tail = 0, i;
    if (snap_take(g, &sn) != 0)
        return;
    queue[tail++] = g->view;
    while (head < tail) {
        u32 v = queue[head++], navs[64], objs[64];
        vc_view vw;
        int k, m, pass;
        if (vc_read_view(&g->db, v, &vw) != 0)
            continue;
        for (pass = 0; pass < 2; pass++) {
            m = vc_read_list(&g->db, pass ? vw.characters : vw.explorables, objs, 64);
            for (k = 0; k < m; k++)
                if (vc_is_enabled(&g->interp, objs[k])) {
                    probe_one(s, &sn, v, objs[k], item);
                    snap_restore(g, &sn);
                }
        }
        m = vc_read_list(&g->db, vw.navs, navs, 64);
        for (k = 0; k < m && tail < MAXV; k++) {
            vc_nav nav;
            int seen = 0;
            if (!vc_is_enabled(&g->interp, navs[k]) || vc_read_nav(&g->db, navs[k], &nav) != 0)
                continue;
            for (i = 0; i < tail && !seen; i++)
                seen = queue[i] == nav.target;
            if (!seen)
                queue[tail++] = nav.target;
        }
    }
    snap_free(&sn);
}

/* Checks one "expect" line. Returns 1 when it holds. */
static int expect(game *g, const char *what, long a, long b, int have_b)
{
    if (strcmp(what, "photo") == 0) {
        int i;
        for (i = 0; i < g->photo_count; i++)
            if (g->photos[i] == (u32)a)
                return 1;
        return 0;
    }
    if (strcmp(what, "view") == 0)
        return g->view == (u32)a;
    if (strcmp(what, "var") == 0 && have_b)
        return vc_get_var(&g->interp, (u32)a) == (i32)b;
    if (strcmp(what, "enabled") == 0 && have_b)
        return vc_is_enabled(&g->interp, (u32)a) == (b != 0);
    if (strcmp(what, "spot") == 0)
        return has_spot(g, (u32)a);
    if (strcmp(what, "nospot") == 0)
        return !has_spot(g, (u32)a);
    if (strcmp(what, "choice") == 0 || strcmp(what, "nochoice") == 0) {
        u32 ids[GAME_MAX_CHOICES];
        int n = game_choices(g, ids, GAME_MAX_CHOICES), i, found = 0;
        for (i = 0; i < n; i++)
            found |= ids[i] == (u32)a;
        return what[0] == 'c' ? found : !found;
    }
    if (strcmp(what, "iface") == 0)
        return has_iface(g, (u32)a);
    if (strcmp(what, "noiface") == 0)
        return !has_iface(g, (u32)a);
    fprintf(stderr, "unknown expectation: %s\n", what);
    return 0;
}

/* Plays a script. Returns the number of failed steps (it stops at the
 * first one). */
int script_run(script *s, const char *path, int quiet)
{
    game *g = s->g;
    FILE *fp = fopen(path, "r");
    char line[512], file[256];
    int lineno = 0, failed = 0;
    if (!fp) {
        perror(path);
        return 1;
    }
    while (fgets(line, sizeof(line), fp)) {
        char cmd[32] = "", what[32] = "", path2[32] = "";
        long a = 0, b = 0;
        int n, ok = 1;
        char *hash = strchr(line, '#');
        lineno++;
        if (hash)
            *hash = '\0';
        n = sscanf(line, "%31s", cmd);
        if (n <= 0)
            continue;
        s->steps++;
        if (!quiet) {
            printf("%s:%d: %s", path, lineno, line);
            if (!strchr(line, '\n'))
                printf("\n");
        }
        if (strcmp(cmd, "include") == 0 && sscanf(line, "%*s %255s", file) == 1) {
            /* Plays another script first, quietly: a disc starts where
             * the previous one ends. */
            char inc[512];
            const char *slash = file[0] == '/' ? NULL : strrchr(path, '/');
            snprintf(inc, sizeof(inc), "%.*s%s", slash ? (int)(slash - path + 1) : 0, path, file);
            ok = script_run(s, inc, 1) == 0;
        } else if (strcmp(cmd, "new") == 0) {
            ok = game_new(g) == 0;
        } else if (strcmp(cmd, "view") == 0 && sscanf(line, "%*s %ld", &a) == 1) {
            ok = game_enter_view(g, (u32)a) == 0;
        } else if (strcmp(cmd, "click") == 0 && sscanf(line, "%*s %ld", &a) == 1) {
            ok = game_click_object(g, (u32)a);
        } else if (strcmp(cmd, "at") == 0 && sscanf(line, "%*s %ld %ld", &a, &b) == 2) {
            ok = game_click(g, (int)a, (int)b);
        } else if (strcmp(cmd, "walk") == 0 && sscanf(line, "%*s %ld", &a) == 1) {
            ok = walk(s, (u32)a);
        } else if (strcmp(cmd, "ask") == 0 && sscanf(line, "%*s %ld", &a) == 1) {
            u32 ids[GAME_MAX_CHOICES];
            int n = game_choices(g, ids, GAME_MAX_CHOICES), k;
            ok = 0;
            for (k = 0; k < n && !ok; k++)
                ok = ids[k] == (u32)a;
            if (ok)
                ok = game_ask(g, (u32)a);
        } else if (strcmp(cmd, "hold") == 0 && sscanf(line, "%*s %31s", what) == 1) {
            ok = game_hold(g, strcmp(what, "none") == 0 ? NULL : what);
        } else if (strcmp(cmd, "askall") == 0 && sscanf(line, "%*s %ld", &a) == 1) {
            ok = ask_all(s, (u32)a);
        } else if ((strcmp(cmd, "ing") == 0 || strcmp(cmd, "noing") == 0) && sscanf(line, "%*s %31s %31s %31[^\n#]", file, what, path2) == 3) {
            /* ing <P|L|N> <database Z|F|G|L|C> <text, spaces allowed> */
            size_t n = strlen(path2);
            while (n && (path2[n - 1] == ' ' || path2[n - 1] == '\t' || path2[n - 1] == '\r'))
                path2[--n] = '\0';
            ok = game_ing_search(g, file[0], what[0], path2) == (strcmp(cmd, "ing") == 0);
        } else if (strcmp(cmd, "dial") == 0 && sscanf(line, "%*s %31s", what) == 1) {
            ok = game_dial(g, what);
        } else if (strcmp(cmd, "icon") == 0 && sscanf(line, "%*s %ld", &a) == 1) {
            ok = game_click_icon(g, (u32)a);
        } else if (strcmp(cmd, "evidence") == 0 && sscanf(line, "%*s %ld", &a) == 1) {
            ok = game_click_evidence(g, (u32)a);
        } else if (strcmp(cmd, "inv") == 0 && sscanf(line, "%*s %ld", &a) == 1) {
            ok = game_click_inventory(g, (u32)a);
        } else if (strcmp(cmd, "back") == 0) {
            game_back(g);
        } else if (strcmp(cmd, "wait") == 0 && sscanf(line, "%*s %ld", &a) == 1) {
            advance(s, (u32)a);
        } else if (strcmp(cmd, "play") == 0 && sscanf(line, "%*s %ld", &a) == 1) {
            /* Lets the window play the clips for a while, without ending
             * them after the step. */
            if (s->host.advance)
                s->host.advance(s->host.user, (u32)a);
            continue;
        } else if (strcmp(cmd, "clips") == 0 && sscanf(line, "%*s %31s", what) == 1) {
            s->keep_clips = strcmp(what, "on") == 0 && s->host.advance;
        } else if (strcmp(cmd, "point") == 0 && sscanf(line, "%*s %ld %ld", &a, &b) == 2) {
            if (s->host.point)
                s->host.point(s->host.user, (int)a, (int)b);
        } else if (strcmp(cmd, "type") == 0) {
            /* Types the rest of the line into the field with focus. */
            const char *t = line + strspn(line, " \t");
            t += strcspn(t, " \t");
            t += strspn(t, " \t");
            while (*t && *t != '\n' && *t != '\r')
                ok &= game_key(g, (unsigned char)*t++);
        } else if (strcmp(cmd, "skip") == 0) {
            settle(s);
        } else if (strcmp(cmd, "shot") == 0 && sscanf(line, "%*s %255s", file) == 1) {
            ok = !s->host.shot || s->host.shot(s->host.user, file) == 0;
        } else if (strcmp(cmd, "set") == 0 && sscanf(line, "%*s %ld %ld", &a, &b) == 2) {
            vc_set_var(&g->interp, (u32)a, (i32)b);
        } else if (strcmp(cmd, "reach") == 0) {
            reach(g);
        } else if (strcmp(cmd, "state") == 0) {
            print_state(g);
        } else if (strcmp(cmd, "text") == 0 && sscanf(line, "%*s %ld", &a) == 1) {
            char t[512];
            printf("  text %ld: %s\n", a, game_text(g, (u32)a, t, sizeof(t)));
        } else if (strcmp(cmd, "var") == 0 && sscanf(line, "%*s %ld", &a) == 1) {
            u32 i;
            for (i = 0; i < g->interp.var_count; i++)
                if (g->interp.vars[i].id == (u32)a)
                    printf("  var %ld %s = %d\n", a, g->interp.vars[i].name, g->interp.vars[i].value);
        } else if (strcmp(cmd, "probe") == 0) {
            int has = sscanf(line, "%*s %31s", what) == 1;
            probe(s, has ? what : NULL);
        } else if (strcmp(cmd, "route") == 0 && sscanf(line, "%*s %ld", &a) == 1) {
            route(g, (u32)a);
        } else if (strcmp(cmd, "gate") == 0 && (sscanf(line, "%*s %ld", &a) == 1 || sscanf(line, "%*s enable %ld", &b) == 1)) {
            /* The GAM enable state of an object, to look up what sets it
             * (tools/re/vc_who.py enable <id>); `gate enable <id>` names
             * the object an enable state controls. */
            u32 i;
            int by_id = strstr(line, "enable") != NULL;
            for (i = 0; i < g->interp.enable_count; i++)
                if (by_id ? g->interp.enables[i].id == (u32)b : g->interp.enables[i].target == (u32)a)
                    printf("  object %u: enable #%u is %s\n", g->interp.enables[i].target, g->interp.enables[i].id,
                           g->interp.enables[i].enabled ? "on" : "off");
        } else if (strcmp(cmd, "expect") == 0 && (n = sscanf(line, "%*s %31s %ld %ld", what, &a, &b)) >= 2) {
            ok = expect(g, what, a, b, n == 3);
        } else {
            fprintf(stderr, "%s:%d: cannot read this line\n", path, lineno);
            ok = 0;
        }
        /* Clips end with the step, unless `clips on` lets the window play
         * them (until `skip` or until they end). */
        if (!s->keep_clips)
            settle(s);
        if (!ok) {
            printf("%s:%d: %s  FAILED\n", path, lineno, line);
            print_state(g);
            failed++;
            break; /* later steps depend on this one */
        }
    }
    fclose(fp);
    return failed;
}

