/* xftool play: runs the game logic from a script, without a window or
 * media. Every clip ends as soon as it starts, so its start and end
 * triggers run in order. See docs/TESTING.md for the script commands. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/game.h"
#include "xf_log.h"

#include "xftool.h"

/* Plays every queued clip through: start triggers, then end triggers. */
static void drain_media(game *g)
{
    game_media m;
    int guard = 0;
    while (guard++ < 256 && game_media_start(g, &m))
        game_media_end(g);
}

/* Short name of a view: the production path of its first enabled media. */
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
static int walk(game *g, u32 target)
{
    enum { MAXV = 4096 };
    static u32 queue[MAXV], prev[MAXV], via[MAXV];
    u32 path[256];
    int head = 0, tail = 0, i, found = -1, n = 0;
    queue[tail] = g->view;
    prev[tail++] = (u32)-1;
    while (head < tail && found < 0) {
        int cur = head++;
        vc_view v;
        u32 navs[64];
        int k, m;
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
            if (!vc_is_enabled(&g->interp, navs[k]) || vc_read_nav(&g->db, navs[k], &nav) != 0)
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
        return 0;
    for (i = found; prev[i] != (u32)-1 && n < 256; i = (int)prev[i])
        path[n++] = via[i];
    while (n-- > 0) {
        printf("  move %u\n", path[n]);
        if (!game_click_object(g, path[n]))
            return 0;
        drain_media(g);
    }
    return g->view == target;
}

/* Talks to a character and asks every question that has not been asked
 * yet, again and again while answers open new ones. Timers get a few
 * seconds between rounds, since some questions open on a timer. */
static int ask_all(game *g, u32 character, u32 *now)
{
    int round, asked = 0;
    for (round = 0; round < 16; round++) {
        u32 ids[GAME_MAX_CHOICES];
        int n, i, any = 0;
        if (!game_click_object(g, character))
            return asked > 0;
        drain_media(g);
        n = game_choices(g, ids, GAME_MAX_CHOICES);
        for (i = 0; i < n; i++)
            if (!game_times_asked(g, ids[i])) {
                char q[128] = "";
                vc_conversation c;
                if (vc_read_conversation(&g->db, ids[i], &c) == 0)
                    vc_read_name(&g->db, c.name, q, sizeof(q));
                printf("  ask %u  %s\n", ids[i], q);
                game_ask(g, ids[i]);
                drain_media(g);
                asked++;
                any = 1;
                break; /* the choices may change after each answer */
            }
        if (!any) {
            u32 end = *now + 3000;
            int before = n;
            while (*now < end) {
                *now += 100;
                game_tick(g, *now);
                drain_media(g);
            }
            if (!game_click_object(g, character))
                break;
            drain_media(g);
            n = game_choices(g, ids, GAME_MAX_CHOICES);
            for (i = 0; i < n && !any; i++)
                any = !game_times_asked(g, ids[i]);
            if (!any && n == before)
                break;
        }
    }
    return 1;
}

/* Checks one "expect" line. Returns 1 when it holds. */
static int expect(game *g, const char *what, long a, long b, int have_b)
{
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
static int run_script(game *g, const char *path, u32 *now, int *steps, int quiet)
{
    FILE *fp = fopen(path, "r");
    char line[512], file[256];
    int lineno = 0, failed = 0;
    if (!fp) {
        perror(path);
        return 1;
    }
    while (fgets(line, sizeof(line), fp)) {
        char cmd[32] = "", what[32] = "";
        long a = 0, b = 0;
        int n, ok = 1;
        char *hash = strchr(line, '#');
        lineno++;
        if (hash)
            *hash = '\0';
        n = sscanf(line, "%31s", cmd);
        if (n <= 0)
            continue;
        (*steps)++;
        if (!quiet) {
            printf("%s:%d: %s", path, lineno, line);
            if (!strchr(line, '\n'))
                printf("\n");
        }
        if (strcmp(cmd, "include") == 0 && sscanf(line, "%*s %255s", file) == 1) {
            /* Plays another script first, quietly: a disc starts where
             * the previous one ends. */
            char inc[512];
            const char *slash = strrchr(path, '/');
            snprintf(inc, sizeof(inc), "%.*s%s", slash ? (int)(slash - path + 1) : 0, path, file);
            ok = run_script(g, inc, now, steps, 1) == 0;
        } else if (strcmp(cmd, "new") == 0) {
            ok = game_new(g) == 0;
        } else if (strcmp(cmd, "view") == 0 && sscanf(line, "%*s %ld", &a) == 1) {
            ok = game_enter_view(g, (u32)a) == 0;
        } else if (strcmp(cmd, "click") == 0 && sscanf(line, "%*s %ld", &a) == 1) {
            ok = game_click_object(g, (u32)a);
        } else if (strcmp(cmd, "at") == 0 && sscanf(line, "%*s %ld %ld", &a, &b) == 2) {
            ok = game_click(g, (int)a, (int)b);
        } else if (strcmp(cmd, "walk") == 0 && sscanf(line, "%*s %ld", &a) == 1) {
            ok = walk(g, (u32)a);
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
            ok = ask_all(g, (u32)a, now);
        } else if (strcmp(cmd, "inv") == 0 && sscanf(line, "%*s %ld", &a) == 1) {
            ok = game_click_inventory(g, (u32)a);
        } else if (strcmp(cmd, "back") == 0) {
            game_back(g);
        } else if (strcmp(cmd, "wait") == 0 && sscanf(line, "%*s %ld", &a) == 1) {
            /* Advance in small steps so timers fire in order. */
            u32 end = *now + (u32)a;
            while (*now < end) {
                *now = XF_MIN(*now + 100, end);
                game_tick(g, *now);
                drain_media(g);
            }
        } else if (strcmp(cmd, "set") == 0 && sscanf(line, "%*s %ld %ld", &a, &b) == 2) {
            vc_set_var(&g->interp, (u32)a, (i32)b);
        } else if (strcmp(cmd, "state") == 0) {
            print_state(g);
        } else if (strcmp(cmd, "expect") == 0 && (n = sscanf(line, "%*s %31s %ld %ld", what, &a, &b)) >= 2) {
            ok = expect(g, what, a, b, n == 3);
        } else {
            fprintf(stderr, "%s:%d: cannot read this line\n", path, lineno);
            ok = 0;
        }
        drain_media(g);
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

static int cmd_play(int argc, char **argv)
{
    game *g;
    int failed, steps = 0, i;
    u32 now = 0;

    setvbuf(stdout, NULL, _IOLBF, 0); /* keep the log and the steps in order */
    if (argc != 3) {
        fputs("usage: xftool play <game folder> <script>\n", stderr);
        return 2;
    }
    g = malloc(sizeof(*g));
    if (!g || game_open(g, argv[1]) != 0) {
        free(g);
        return 1;
    }
    failed = run_script(g, argv[2], &now, &steps, 0);
    printf("%d steps, %s\n", steps, failed ? "stopped at a failure" : "all passed");
    if (g->missing_count) {
        printf("functions not implemented yet (%d):", g->missing_count);
        for (i = 0; i < g->missing_count; i++)
            printf(" %s", g->missing[i]);
        printf("\n");
    }
    game_close(g);
    free(g);
    return failed ? 1 : 0;
}

const xftool_cmd xftool_play_cmds[] = {
    { "play", "<game folder> <script>  run the game logic from a script", cmd_play },
    { NULL, NULL, NULL },
};
