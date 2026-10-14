#include "explore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx/text.h"
#include "media/mov.h"
#include "media/video.h"
#include "platform/plat.h"
#include "vc/vc.h"
#include "xf_file.h"
#include "xf_log.h"

#define SCREEN_W 640
#define SCREEN_H 480
#define PICTURE_X 20
#define PICTURE_Y 90
#define PICTURE_W 600
#define PICTURE_H 240
#define MAX_SPOTS 64
#define HISTORY 64

enum { SPOT_NAV, SPOT_EXPLORE, SPOT_CHARACTER };

typedef struct {
    int kind;
    vc_region region;
    u32 target; /* view for moves, object id otherwise */
} spot;

typedef struct {
    const char *data_dir;
    int disc;
    xf_neo_db db;
    xf_font font;
    int have_font;
    u32 fb[SCREEN_W * SCREEN_H];
    u32 picture[PICTURE_W * PICTURE_H];
    int have_picture;
    u32 view;
    char label[256];
    spot spots[MAX_SPOTS];
    int spot_count;
    u32 history[HISTORY];
    int history_len;
    /* one open navigation file */
    char nav_path[1024];
    xf_mov nav;
    int nav_open;
} explorer;

static int open_nav_file(explorer *e, const char *name)
{
    char path[1024];
    if (xf_path_resolve(e->data_dir, name, path, sizeof(path)) != 0)
        return -1;
    if (e->nav_open && strcmp(path, e->nav_path) == 0)
        return 0;
    if (e->nav_open)
        xf_mov_close(&e->nav);
    e->nav_open = xf_mov_open(&e->nav, path) == 0;
    if (e->nav_open)
        snprintf(e->nav_path, sizeof(e->nav_path), "%s", path);
    return e->nav_open ? 0 : -1;
}

/* Finds the navigation still of the view for the current disc, or for the
 * first disc whose file is installed. */
static void load_picture(explorer *e, const vc_view *v)
{
    u32 ids[32];
    int n = vc_read_list(&e->db, v->assets, ids, 32), i, j, pass;

    e->have_picture = 0;
    for (pass = 0; pass < 2 && !e->have_picture; pass++)
        for (i = 0; i < n && !e->have_picture; i++) {
            vc_asset_ref a;
            if (vc_read_asset_ref(&e->db, ids[i], &a) != 0 || a.kind != 67)
                continue;
            for (j = 0; j < a.entry_count; j++) {
                const vc_media_entry *m = &a.entries[j];
                if (pass == 0 && m->disc != e->disc)
                    continue;
                if (open_nav_file(e, m->path) == 0 &&
                    xf_movie_frame_at(&e->nav, m->start, e->picture, PICTURE_W, PICTURE_H) == 0) {
                    e->have_picture = 1;
                    snprintf(e->label, sizeof(e->label), "view %u  disc %d  %s", e->view, m->disc,
                             a.logical);
                    break;
                }
            }
        }
    if (!e->have_picture)
        snprintf(e->label, sizeof(e->label), "view %u  (no still)", e->view);
}

static void add_spots(explorer *e, u32 list, int kind)
{
    u32 ids[MAX_SPOTS];
    int n = vc_read_list(&e->db, list, ids, MAX_SPOTS), i;

    for (i = 0; i < n && e->spot_count < MAX_SPOTS; i++) {
        spot *s = &e->spots[e->spot_count];
        xf_neo_reader r;
        if (kind == SPOT_NAV) {
            vc_nav nav;
            vc_hotspot h;
            if (vc_read_nav(&e->db, ids[i], &nav) != 0 || vc_read_hotspot(&e->db, nav.hotspot, &h) != 0)
                continue;
            s->region = h.region;
            s->target = nav.target;
        } else {
            /* Explorables and character views start with a region; character
             * views have one 4-byte field before it. */
            if (xf_neo_object(&e->db, ids[i], &r, NULL, NULL) != 0)
                continue;
            if (kind == SPOT_CHARACTER)
                xf_neo_u32(&r);
            s->region.cursor = xf_neo_u32(&r);
            s->region.left = (i16)xf_neo_u16(&r);
            s->region.top = (i16)xf_neo_u16(&r);
            s->region.right = (i16)xf_neo_u16(&r);
            s->region.bottom = (i16)xf_neo_u16(&r);
            s->target = ids[i];
            if (r.error)
                continue;
        }
        s->kind = kind;
        e->spot_count++;
    }
}

static int enter_view(explorer *e, u32 id)
{
    vc_view v;
    if (vc_read_view(&e->db, id, &v) != 0) {
        xf_warn("explore: %u is not a view", id);
        return -1;
    }
    e->view = id;
    e->spot_count = 0;
    add_spots(e, v.navs, SPOT_NAV);
    add_spots(e, v.explorables, SPOT_EXPLORE);
    add_spots(e, v.characters, SPOT_CHARACTER);
    load_picture(e, &v);
    xf_info("view %u: %d hotspots, %s", id, e->spot_count, e->label);
    return 0;
}

static void frame_rect(explorer *e, const vc_region *r, u32 color)
{
    int x, y;
    for (x = XF_MAX(r->left, 0); x < XF_MIN(r->right, SCREEN_W); x++) {
        if (r->top >= 0 && r->top < SCREEN_H)
            e->fb[r->top * SCREEN_W + x] = color;
        if (r->bottom - 1 >= 0 && r->bottom - 1 < SCREEN_H)
            e->fb[(r->bottom - 1) * SCREEN_W + x] = color;
    }
    for (y = XF_MAX(r->top, 0); y < XF_MIN(r->bottom, SCREEN_H); y++) {
        if (r->left >= 0 && r->left < SCREEN_W)
            e->fb[y * SCREEN_W + r->left] = color;
        if (r->right - 1 >= 0 && r->right - 1 < SCREEN_W)
            e->fb[y * SCREEN_W + r->right - 1] = color;
    }
}

static void draw(explorer *e, int mx, int my)
{
    static const u32 colors[] = { 0x40FF40, 0xFFE040, 0x40E0FF };
    xf_surface s = { e->fb, SCREEN_W, SCREEN_H, SCREEN_W };
    int i, y;

    memset(e->fb, 0, sizeof(e->fb));
    if (e->have_picture)
        for (y = 0; y < PICTURE_H; y++)
            memcpy(&e->fb[(PICTURE_Y + y) * SCREEN_W + PICTURE_X], &e->picture[y * PICTURE_W],
                   PICTURE_W * 4);
    for (i = 0; i < e->spot_count; i++) {
        const vc_region *r = &e->spots[i].region;
        int inside = mx >= r->left && mx < r->right && my >= r->top && my < r->bottom;
        frame_rect(e, r, inside ? 0xFFFFFF : colors[e->spots[i].kind]);
    }
    if (e->have_font) {
        xf_text_draw(&s, &e->font, 14, 8, 20, e->label, strlen(e->label), 0xC0C0C0);
        static const char help[] = "green: move  yellow: examine  cyan: character  right click: back";
        xf_text_draw(&s, &e->font, 14, 8, 470, help, sizeof(help) - 1, 0x808080);
    }
}

/* Returns the move under the mouse, or NULL. The last matching spot wins,
 * so small spots inside the large turn areas can be clicked. */
static const spot *spot_at(const explorer *e, int x, int y)
{
    int i;
    for (i = e->spot_count - 1; i >= 0; i--) {
        const vc_region *r = &e->spots[i].region;
        if (x >= r->left && x < r->right && y >= r->top && y < r->bottom)
            return &e->spots[i];
    }
    return NULL;
}

int xf_explore(const char *data_dir, u32 start_view, int disc)
{
    explorer *e = calloc(1, sizeof(*e));
    char path[1024];
    int running = 1, mx = 0, my = 0;

    if (!e)
        return 1;
    e->data_dir = data_dir;
    e->disc = disc;
    if (xf_path_resolve(data_dir, "XFILES.HDB", path, sizeof(path)) != 0 || xf_neo_open(&e->db, path) != 0) {
        xf_error("explore: cannot open XFILES.HDB in %s", data_dir);
        free(e);
        return 1;
    }
    if (xf_path_resolve(data_dir, "DLG.TTF", path, sizeof(path)) == 0 ||
        xf_path_resolve(data_dir, "DLG.TTR", path, sizeof(path)) == 0)
        e->have_font = xf_font_load(&e->font, path) == 0;
    if (enter_view(e, start_view) != 0) {
        xf_neo_close(&e->db);
        free(e);
        return 1;
    }
    while (running) {
        plat_event ev;
        while (plat_poll(&ev)) {
            if (ev.type == PLAT_EV_QUIT || (ev.type == PLAT_EV_KEY_DOWN && ev.key == PLAT_KEY_ESCAPE))
                running = 0;
            else if (ev.type == PLAT_EV_MOUSE_MOVE) {
                mx = ev.x;
                my = ev.y;
            } else if (ev.type == PLAT_EV_MOUSE_DOWN && ev.button == PLAT_MOUSE_LEFT) {
                const spot *s = spot_at(e, ev.x, ev.y);
                if (s && s->kind == SPOT_NAV) {
                    u32 from = e->view;
                    if (enter_view(e, s->target) == 0 && e->history_len < HISTORY)
                        e->history[e->history_len++] = from;
                } else if (s) {
                    xf_info("clicked object %u", s->target);
                }
            } else if ((ev.type == PLAT_EV_MOUSE_DOWN && ev.button == PLAT_MOUSE_RIGHT) ||
                       (ev.type == PLAT_EV_KEY_DOWN && ev.key == 8)) {
                if (e->history_len > 0)
                    enter_view(e, e->history[--e->history_len]);
            }
        }
        draw(e, mx, my);
        plat_present(e->fb, SCREEN_W, SCREEN_H, SCREEN_W);
        plat_sleep_ms(16);
    }
    if (e->nav_open)
        xf_mov_close(&e->nav);
    if (e->have_font) {
        xf_text_cache_clear();
        xf_font_free(&e->font);
    }
    xf_neo_close(&e->db);
    free(e);
    return 0;
}
