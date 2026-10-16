/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "data/neo.h"
#include "vc/action.h"
#include "vc/interp.h"
#include "xf_log.h"
#include "vc/vc.h"
#include "xftool.h"

static int cmd_hdb(int argc, char **argv)
{
    xf_neo_db db;
    u32 per[256] = { 0 }, i;

    if (argc != 2) {
        fputs("usage: xftool hdb <file.hdb>\n", stderr);
        return 2;
    }
    if (xf_neo_open(&db, argv[1]) != 0)
        return 1;
    for (i = 0; i < db.count; i++)
        per[db.entries[i].class_id]++;
    printf("%u objects\n", db.count);
    for (i = 0; i < 256; i++)
        if (per[i])
            printf("  class 0x%02X: %u\n", i, per[i]);
    xf_neo_close(&db);
    return 0;
}

static void print_id(void *user, u32 id)
{
    const xf_neo_db *db = user;
    const xf_neo_entry *e = xf_neo_find(db, id);
    printf("    %u (class 0x%02X)\n", id, e ? e->class_id : 0);
}

static int cmd_obj(int argc, char **argv)
{
    xf_neo_db db;
    xf_neo_reader r;
    const xf_neo_entry *e;
    u16 flags;
    u32 version, id, i;

    if (argc != 3 && argc != 4) {
        fputs("usage: xftool hdb-obj <file.hdb> <id> [list]\n", stderr);
        return 2;
    }
    if (xf_neo_open(&db, argv[1]) != 0)
        return 1;
    id = (u32)strtoul(argv[2], NULL, 0);
    e = xf_neo_find(&db, id);
    if (!e || xf_neo_object(&db, id, &r, &flags, &version) != 0) {
        fprintf(stderr, "no object %u\n", id);
        xf_neo_close(&db);
        return 1;
    }
    printf("object %u: class 0x%02X at 0x%X, flags 0x%04X, version %u\n  ", id, e->class_id,
           e->offset, flags, version);
    for (i = 0; i < 48 && r.p + i < r.end; i++)
        printf("%02x%s", r.p[i], (i % 16 == 15) ? "\n  " : " ");
    putchar('\n');
    if (argc == 4) {
        xf_neo_list l = xf_neo_read_list(&r);
        printf("  list: %u ids at 0x%X\n", l.count, l.root);
        xf_neo_list_ids(&db, &l, print_id, &db);
    }
    xf_neo_close(&db);
    return 0;
}

static void count_id(void *user, u32 id)
{
    (void)id;
    ++*(u32 *)user;
}

/* Reads the list of every object of the list classes and checks it. */
static int cmd_check_lists(int argc, char **argv)
{
    static const u8 list_classes[] = { 0x2E, 0x32, 0x34, 0x36, 0x38, 0x3A, 0x3B, 0x42, 0x4D, 0x52, 0x55 };
    xf_neo_db db;
    u32 i, checked = 0, bad = 0, ids = 0;
    size_t k;

    if (argc != 2) {
        fputs("usage: xftool hdb-check <file.hdb>\n", stderr);
        return 2;
    }
    if (xf_neo_open(&db, argv[1]) != 0)
        return 1;
    for (i = 0; i < db.count; i++) {
        const xf_neo_entry *e = &db.entries[i];
        xf_neo_reader r;
        xf_neo_list l;
        u32 n = 0;
        int got;
        for (k = 0; k < sizeof(list_classes) && list_classes[k] != e->class_id; k++)
            ;
        if (k == sizeof(list_classes) || xf_neo_object(&db, e->id, &r, NULL, NULL) != 0)
            continue;
        l = xf_neo_read_list(&r);
        got = xf_neo_list_ids(&db, &l, count_id, &n);
        checked++;
        ids += n;
        if (got < 0 || n != l.count) {
            if (bad < 10)
                printf("object %u (class 0x%02X): list says %u, found %d\n", e->id, e->class_id,
                       l.count, got);
            bad++;
        }
    }
    printf("%u lists checked, %u ids, %u mismatches\n", checked, ids, bad);
    xf_neo_close(&db);
    return bad ? 1 : 0;
}

static int cmd_asset(int argc, char **argv)
{
    xf_neo_db db;
    int a, i;

    if (argc < 3) {
        fputs("usage: xftool hdb-asset <file.hdb> <id>...\n", stderr);
        return 2;
    }
    if (xf_neo_open(&db, argv[1]) != 0)
        return 1;
    for (a = 2; a < argc; a++) {
        vc_asset_ref r;
        char name[128];
        u32 id = (u32)strtoul(argv[a], NULL, 0);
        if (vc_read_asset_ref(&db, id, &r) != 0) {
            printf("%u: not a media reference\n", id);
            continue;
        }
        vc_read_name(&db, r.name, name, sizeof(name));
        printf("%u: kind %u, \"%s\" (%s)", id, r.kind, r.logical, name);
        if (r.triggers)
            printf(" triggers %u", r.triggers);
        printf("\n");
        for (i = 0; i < r.entry_count; i++)
            printf("    disc %d: %s %u..%u\n", r.entries[i].disc, r.entries[i].path, r.entries[i].start,
                   r.entries[i].end);
    }
    xf_neo_close(&db);
    return 0;
}

/* Follows every move from a view and reports what it reaches. */
static int cmd_walk(int argc, char **argv)
{
    xf_neo_db db;
    u32 *queue, *seen_ids, head = 0, tail = 0, seen = 0, stills = 0, dead = 0, i;
    int disc;

    if (argc != 3 && argc != 4) {
        fputs("usage: xftool hdb-walk <file.hdb> <view> [disc]\n", stderr);
        return 2;
    }
    if (xf_neo_open(&db, argv[1]) != 0)
        return 1;
    disc = argc == 4 ? atoi(argv[3]) : 1;
    queue = calloc(db.count, sizeof(u32));
    seen_ids = calloc(db.count, sizeof(u32));
    queue[tail++] = (u32)strtoul(argv[2], NULL, 0);
    while (head < tail) {
        u32 id = queue[head++], navs[64], assets[32];
        vc_view v;
        int n, k, has_still = 0;
        for (i = 0; i < seen && seen_ids[i] != id; i++)
            ;
        if (i < seen || vc_read_view(&db, id, &v) != 0)
            continue;
        seen_ids[seen++] = id;
        n = vc_read_list(&db, v.assets, assets, 32);
        for (k = 0; k < n && !has_still; k++) {
            vc_asset_ref a;
            int j;
            if (vc_read_asset_ref(&db, assets[k], &a) == 0 && a.kind == 67)
                for (j = 0; j < a.entry_count; j++)
                    has_still |= a.entries[j].disc == disc;
        }
        stills += has_still;
        n = vc_read_list(&db, v.navs, navs, 64);
        if (n <= 0)
            dead++;
        for (k = 0; k < n; k++) {
            vc_nav nav;
            if (vc_read_nav(&db, navs[k], &nav) == 0 && tail < db.count)
                queue[tail++] = nav.target;
        }
    }
    printf("%u views reachable, %u with a still for disc %d, %u without moves\n", seen, stills, disc, dead);
    free(queue);
    free(seen_ids);
    xf_neo_close(&db);
    return 0;
}

/* Prints each trigger of a trigger list (or one trigger): its kind and
 * its decoded actions. */
static int cmd_triggers(int argc, char **argv)
{
    xf_neo_db db;
    u32 id, ids[64], acts[256];
    char line[256];
    int n, i, k, m;

    if (argc != 3) {
        fputs("usage: xftool hdb-triggers <file.hdb> <trigger list or trigger id>\n", stderr);
        return 2;
    }
    if (xf_neo_open(&db, argv[1]) != 0)
        return 1;
    id = (u32)strtoul(argv[2], NULL, 0);
    n = vc_class(&db, id) == VC_TRIGGER ? (ids[0] = id, 1) : vc_read_list(&db, id, ids, 64);
    for (i = 0; i < n; i++) {
        xf_neo_reader r;
        u32 list;
        int kind;
        if (vc_class(&db, ids[i]) != VC_TRIGGER || xf_neo_object(&db, ids[i], &r, NULL, NULL) != 0)
            continue;
        list = xf_neo_u32(&r);
        kind = xf_neo_u8(&r);
        printf("trigger %u kind %d, list %u\n", ids[i], kind, list);
        m = vc_read_list(&db, list, acts, 256);
        for (k = 0; k < m; k++) {
            vc_action a;
            if (vc_read_action(&db, acts[k], &a) != 0)
                continue;
            vc_action_text(&a, line, sizeof(line));
            printf("  #%u %s\n", acts[k], line);
        }
    }
    xf_neo_close(&db);
    return 0;
}

/* Prints the actions of a list, or statistics over all actions. */
static int cmd_actions(int argc, char **argv)
{
    xf_neo_db db;
    u32 i, types[8][2] = { { 0 } }, bad = 0;
    char line[256];
    int k;

    if (argc != 2 && argc != 3) {
        fputs("usage: xftool hdb-actions <file.hdb> [action list or action id]\n", stderr);
        return 2;
    }
    if (xf_neo_open(&db, argv[1]) != 0)
        return 1;
    if (argc == 3) {
        u32 id = (u32)strtoul(argv[2], NULL, 0), ids[256];
        int n = vc_class(&db, id) == VC_ACTION ? (ids[0] = id, 1) : vc_read_list(&db, id, ids, 256);
        for (k = 0; k < n; k++) {
            vc_action a;
            if (vc_read_action(&db, ids[k], &a) != 0)
                continue;
            vc_action_text(&a, line, sizeof(line));
            printf("#%u %s\n", ids[k], line);
        }
    } else {
        for (i = 0; i < db.count; i++) {
            vc_action a;
            if (db.entries[i].class_id != VC_ACTION || vc_read_action(&db, db.entries[i].id, &a) != 0)
                continue;
            if (a.type < 8)
                types[a.type][a.has_condition]++;
            bad += !a.valid;
        }
        for (k = 0; k < 8; k++)
            printf("type %d: %u plain, %u with condition\n", k, types[k][0], types[k][1]);
        if (getenv("XF_ACTIONS")) /* every action as text, to search */
            for (i = 0; i < db.count; i++) {
                vc_action a;
                if (db.entries[i].class_id == VC_ACTION && vc_read_action(&db, db.entries[i].id, &a) == 0) {
                    vc_action_text(&a, line, sizeof(line));
                    printf("#%u %s\n", db.entries[i].id, line);
                }
            }
        if (getenv("XF_FUNCTIONS"))
            for (i = 0; i < db.count; i++) {
                vc_action a;
                if (db.entries[i].class_id == VC_ACTION && vc_read_action(&db, db.entries[i].id, &a) == 0 &&
                    a.type == VC_ACT_FUNCTION && a.valid)
                    printf("function %s %d #%u\n", a.u.function.name, a.u.function.argc, db.entries[i].id);
            }
        printf("%u actions with an unexpected size\n", bad);
    }
    xf_neo_close(&db);
    return 0;
}

static u32 rec_u32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

static int list_has(const xf_neo_db *db, u32 list, u32 id)
{
    static u32 ids[2048];
    int n = vc_read_list(db, list, ids, 2048), k;
    for (k = 0; k < n; k++)
        if (ids[k] == id)
            return 1;
    return 0;
}

/* Prints what owns an action: action -> action list -> trigger ->
 * trigger list -> the media, view, hotspot, conversation or layout item
 * whose record names that trigger list. Lists called with DoActionList
 * lead to the calling action. */
static void print_owners(const xf_neo_db *db, u32 action, int depth)
{
    u32 i, j, k;
    if (depth > 3)
        return;
    for (i = 0; i < db->count; i++) {
        u32 al = db->entries[i].id;
        if (db->entries[i].class_id != 0x42 || !list_has(db, al, action))
            continue;
        for (j = 0; j < db->count; j++) {
            xf_neo_reader r;
            u32 t = db->entries[j].id;
            if (db->entries[j].class_id == 0x41) {
                vc_action a;
                if (vc_read_action(db, t, &a) == 0 && a.type == VC_ACT_FUNCTION &&
                    strcmp(a.u.function.name, "DoActionList") == 0 && a.u.function.argc == 2 &&
                    a.u.function.args[1].value == al) {
                    printf("%*scalled by #%u\n", depth * 2, "", t);
                    print_owners(db, t, depth + 1);
                }
                continue;
            }
            if (db->entries[j].class_id != 0x51 || xf_neo_object(db, t, &r, NULL, NULL) != 0 ||
                r.p + 4 > r.end || rec_u32(r.p) != al)
                continue;
            for (k = 0; k < db->count; k++) {
                u32 tl = db->entries[k].id, o;
                if (db->entries[k].class_id != 0x52 || !list_has(db, tl, t))
                    continue;
                for (o = 0; o < db->count; o++) {
                    xf_neo_reader q;
                    const u8 *p;
                    int c = db->entries[o].class_id;
                    if (c < 0x2A || c == 0x41 || c == 0x42 || c == 0x51 || c == 0x52 ||
                        xf_neo_object(db, db->entries[o].id, &q, NULL, NULL) != 0)
                        continue;
                    /* records do not carry their length: the fields that
                     * name trigger lists sit in the first 64 bytes */
                    for (p = q.p; p + 4 <= q.end && p < q.p + 64; p += 2)
                        if (rec_u32(p) == tl) {
                            vc_asset_ref m;
                            char name[128] = "";
                            if (vc_read_asset_ref(db, db->entries[o].id, &m) == 0)
                                snprintf(name, sizeof(name), "%s", m.logical);
                            else
                                vc_read_name(db, db->entries[o].id, name, sizeof(name));
                            printf("%*strigger %u in list %u of %u (class 0x%02X) %s\n", depth * 2, "", t, tl,
                                   db->entries[o].id, c, name);
                            break;
                        }
                }
            }
        }
    }
}

static int cmd_owner(int argc, char **argv)
{
    xf_neo_db db;
    int i;
    if (argc < 3) {
        fputs("usage: xftool hdb-owner <file.hdb> <action id>...\n", stderr);
        return 2;
    }
    if (xf_neo_open(&db, argv[1]) != 0)
        return 1;
    for (i = 2; i < argc; i++) {
        u32 id = (u32)strtoul(argv[i], NULL, 0);
        printf("#%u:\n", id);
        print_owners(&db, id, 1);
    }
    xf_neo_close(&db);
    return 0;
}

/* Runs an action list or a trigger list against the initial game state. */
static int cmd_run(int argc, char **argv)
{
    xf_neo_db hdb, gam;
    vc_interp in;
    u32 id;

    if (argc != 4 && argc != 5) {
        fputs("usage: xftool hdb-run <file.hdb> <file.gam> <action list | trigger list> [kind]\n", stderr);
        return 2;
    }
    if (xf_neo_open(&hdb, argv[1]) != 0 || xf_neo_open(&gam, argv[2]) != 0)
        return 1;
    vc_interp_init(&in, &hdb, &gam, NULL);
    in.trace = 1;
    id = (u32)strtoul(argv[3], NULL, 0);
    if (vc_class(&hdb, id) == VC_TRIGGER_LIST)
        vc_run_triggers(&in, id, argc == 5 ? atoi(argv[4]) : 0);
    else
        vc_run_action_list(&in, id);
    vc_interp_free(&in);
    xf_neo_close(&gam);
    xf_neo_close(&hdb);
    return 0;
}

const xftool_cmd xftool_hdb_cmds[] = {
    { "hdb-run", "<hdb> <gam> <list> [kind]  run an action or trigger list with tracing", cmd_run },
    { "hdb-triggers", "<file.hdb> <trigger list>  decode each trigger: kind and actions", cmd_triggers },
    { "hdb-owner", "<file.hdb> <action>...  the media, view or item whose triggers run an action", cmd_owner },
    { "hdb-actions", "<file.hdb> [list id]  decode actions, or count them by type", cmd_actions },
    { "hdb-walk", "<file.hdb> <view> [disc]  follow every move from a view", cmd_walk },
    { "hdb-asset", "<file.hdb> <id>...  decode media references", cmd_asset },
    { "hdb", "<file.hdb>  count the objects of each class", cmd_hdb },
    { "hdb-obj", "<file.hdb> <id> [list]  dump one object, and its list", cmd_obj },
    { "hdb-check", "<file.hdb>  read every list and check its length", cmd_check_lists },
    { 0 }
};
