#include <stdio.h>
#include <stdlib.h>

#include "data/neo.h"
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
        printf("%u: kind %u, \"%s\" (%s)\n", id, r.kind, r.logical, name);
        for (i = 0; i < r.entry_count; i++)
            printf("    disc %d: %s %u..%u\n", r.entries[i].disc, r.entries[i].path, r.entries[i].start,
                   r.entries[i].end);
    }
    xf_neo_close(&db);
    return 0;
}

const xftool_cmd xftool_hdb_cmds[] = {
    { "hdb-asset", "<file.hdb> <id>...  decode media references", cmd_asset },
    { "hdb", "<file.hdb>  count the objects of each class", cmd_hdb },
    { "hdb-obj", "<file.hdb> <id> [list]  dump one object, and its list", cmd_obj },
    { "hdb-check", "<file.hdb>  read every list and check its length", cmd_check_lists },
    { 0 }
};
