#include "explore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/game.h"
#include "gfx/text.h"
#include "media/mov.h"
#include "media/pff.h"
#include "media/pict.h"
#include "media/player.h"
#include "media/video.h"
#include "platform/plat.h"
#include "xf_file.h"
#include "xf_log.h"

#define SCREEN_W 640
#define SCREEN_H 480
#define PICTURE_X 20
#define PICTURE_Y 90
#define PICTURE_W 600
#define PICTURE_H 240

/* Main menu (0x48EE40 MainMenu_ctor): media IDs are fixed in the engine
 * (0x482FE0 MainMenu_load_media). Positions measured from the original
 * running under Wine; see docs/engine/runtime.md. */
enum { MENU_LEFT, MENU_RIGHT, MENU_MID, MENU_SOUND, MENU_PARTS };

typedef struct {
    u32 asset;
    int x, y, loop;
} menu_part;

static const menu_part g_menu_parts[MENU_PARTS] = {
    { 49583, 0, 0, 0 },    /* LEFTmain.mov, 472x480 */
    { 49587, 472, 0, 0 },  /* RIGHT.mov, 168x480, item labels */
    { 49585, 60, 102, 1 }, /* MIDloop.mov, 388x310 */
    { 94320, 0, 0, 1 },    /* MIDloop.amv, the loop's sound */
};

/* Item rows in screen coordinates, measured from the last frame of
 * RIGHT.mov. The exact hit rectangles in the engine are not read yet. */
static const struct {
    const char *name;
    int top, bottom;
} g_menu_items[] = {
    { "New", 80, 115 },      { "Load", 128, 162 }, { "Save", 178, 210 }, { "Previous", 228, 260 },
    { "Options", 277, 312 }, { "Help", 329, 366 }, { "Quit", 387, 420 },
};

/* Frame ranges in RIGHT.mov: 0-65 labels animate in, 66-89 plain menu,
 * then one rollover animation per item (measured frame by frame). */
#define RIGHT_INTRO_END 66
static const struct {
    int first, last;
} g_menu_rollover[] = {
    { 90, 100 }, { 101, 109 }, { 110, 120 }, { 121, 130 }, { 131, 140 }, { 141, 150 }, { 151, 162 },
};

/* A moving picture of the view: a range of a movie, looped or held on
 * its last frame. */
typedef struct {
    xf_mov mov;
    xf_video video;
    const xf_mov_track *track;
    u32 start_ms, length_ms;
    int loop;
    u32 started;
} view_anim;

#define MAX_ANIMS 4

/* The explorer draws the game and plays its media. */
typedef struct {
    game *g;
    const char *data_dir;
    const char *const *media_dirs;
    int disc;
    /* the clip playing */
    xf_player player;
    int playing;          /* a clip, or the location card alone, is on screen */
    int have_clip;
    int card_active;
    u32 card_start, card_until;
    /* the main menu */
    int in_menu;
    xf_player menu[MENU_PARTS];
    int menu_open[MENU_PARTS];
    int menu_hover;
    xf_mov right_mov;     /* RIGHT.mov is stepped by frame */
    xf_video right;
    int right_open, right_frame, right_target;
    u32 right_next_ms;
    /* sounds beside the clips; loops repeat */
    xf_player sound;
    int sound_open, sound_loop;
    u32 sound_serial;
    char sound_path[1024];
    xf_font font;
    int have_font;
    u32 fb[SCREEN_W * SCREEN_H];
    /* the view picture: still and moving media */
    u32 picture[PICTURE_W * PICTURE_H];
    int have_picture;
    u32 shown_view;
    u32 view_media[32];
    int view_media_count;
    view_anim anims[MAX_ANIMS];
    int anim_count;
    char label[256];
    /* pictures of the interface items on screen, by layout and item */
    xf_image iface_img[GAME_MAX_IFACES][GAME_MAX_IFACE_ITEMS];
    u32 iface_serial;
    xf_pff pff;           /* x.pff, for interface pictures */
    int pff_open;
    /* one open navigation file */
    char nav_path[1024];
    xf_mov nav;
    int nav_open;
} explorer;

static void stop_media(explorer *e);
static void stop_sound(explorer *e);

/* Finds a media file in the game folder, then in the extra media folders. */
static int resolve_media(const explorer *e, const char *name, char *out, size_t size)
{
    int i;
    if (xf_path_resolve(e->data_dir, name, out, size) == 0)
        return 0;
    for (i = 0; e->media_dirs && e->media_dirs[i]; i++)
        if (xf_path_resolve(e->media_dirs[i], name, out, size) == 0)
            return 0;
    return -1;
}

static int open_nav_file(explorer *e, const char *name)
{
    char path[1024];
    if (resolve_media(e, name, path, sizeof(path)) != 0)
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

/* --- View picture ---------------------------------------------------- */

static void close_anims(explorer *e)
{
    int i;
    for (i = 0; i < e->anim_count; i++) {
        xf_video_close(&e->anims[i].video);
        xf_mov_close(&e->anims[i].mov);
    }
    e->anim_count = 0;
}

/* Opens a movie range as a view animation. Times are movie time in ms;
 * 0..0 means the whole movie. */
static int open_anim(explorer *e, const char *path, u32 start, u32 end, int loop)
{
    view_anim *an = &e->anims[e->anim_count];
    u32 k;
    if (e->anim_count >= MAX_ANIMS || xf_mov_open(&an->mov, path) != 0)
        return -1;
    an->track = NULL;
    for (k = 0; k < an->mov.track_count && !an->track; k++)
        if (an->mov.tracks[k].kind == XF_TRACK_VIDEO && xf_video_supported(&an->mov.tracks[k]))
            an->track = &an->mov.tracks[k];
    if (!an->track || xf_video_open(&an->video, &an->mov, an->track) != 0) {
        xf_mov_close(&an->mov);
        return -1;
    }
    if (end <= start) {
        start = 0;
        end = (u32)((u64)an->mov.duration * 1000 / an->mov.timescale);
    }
    an->start_ms = start;
    an->length_ms = XF_MAX(end - start, 1);
    an->loop = loop;
    an->started = plat_ticks_ms();
    e->anim_count++;
    return 0;
}

/* Steps a view animation to the current time. */
static void update_anim(view_anim *an, u32 now)
{
    u32 t = now - an->started;
    u64 movie_time, media;
    t = an->loop ? t % an->length_ms : XF_MIN(t, an->length_ms - 1);
    movie_time = (u64)(an->start_ms + t) * an->mov.timescale / 1000;
    if (xf_mov_edit_lookup(&an->mov, an->track, movie_time, &media) == 0)
        xf_video_seek(&an->video, xf_mov_sample_at(an->track, (u32)media));
}

/* Loads the enabled media of the view: the navigation still (media type
 * bit 0x40, a frame of a .nmv file) and moving pictures (a range of a
 * movie; type bit 0x08 loops it). Uses the current disc, or the first
 * disc whose file is installed. */
static void load_picture(explorer *e)
{
    int i, j, pass;
    e->have_picture = 0;
    close_anims(e);
    e->shown_view = e->g->view;
    e->view_media_count = game_view_media(e->g, e->view_media, 32);
    snprintf(e->label, sizeof(e->label), "view %u  (no still)", e->g->view);
    for (i = 0; i < e->view_media_count; i++) {
        vc_asset_ref a;
        int done = 0;
        if (vc_read_asset_ref(&e->g->db, e->view_media[i], &a) != 0)
            continue;
        for (pass = 0; pass < 2 && !done; pass++)
            for (j = 0; j < a.entry_count && !done; j++) {
                const vc_media_entry *m = &a.entries[j];
                char path[1024];
                if (pass == 0 && m->disc != e->disc)
                    continue;
                if (a.kind & 0x40) {
                    if (!e->have_picture && open_nav_file(e, m->path) == 0 &&
                        xf_movie_frame_at(&e->nav, m->start, e->picture, PICTURE_W, PICTURE_H) == 0) {
                        e->have_picture = done = 1;
                        snprintf(e->label, sizeof(e->label), "view %u  disc %d  %s", e->g->view, m->disc,
                                 a.logical);
                    }
                } else if (resolve_media(e, m->path, path, sizeof(path)) == 0 &&
                           open_anim(e, path, m->start, m->end, (a.kind & 0x08) != 0) == 0) {
                    done = 1;
                    if (!e->have_picture)
                        snprintf(e->label, sizeof(e->label), "view %u  disc %d  %s", e->g->view, m->disc,
                                 a.logical);
                }
            }
        if (!done)
            xf_info("view %u: media %s not shown", e->g->view, a.logical);
    }
}

/* Reloads the picture when the view changed or actions enabled or
 * disabled some of its media. */
static void refresh_picture(explorer *e)
{
    u32 ids[32];
    int n = game_view_media(e->g, ids, 32);
    if (e->shown_view != e->g->view || n != e->view_media_count ||
        memcmp(ids, e->view_media, (size_t)n * sizeof(u32)) != 0)
        load_picture(e);
}

/* --- Interface pictures ---------------------------------------------- */

/* Loads the picture of an interface item: the first media of its list,
 * an image of a .pff bundle or a frame of a movie. */
static void load_item_picture(explorer *e, const game_iface_item *it, xf_image *img)
{
    u32 ids[8];
    int n = vc_read_list(&e->g->db, it->item.assets, ids, 8), i, j;
    memset(img, 0, sizeof(*img));
    for (i = 0; i < n && !img->pixels; i++) {
        vc_asset_ref a;
        if (vc_read_asset_ref(&e->g->db, ids[i], &a) != 0)
            continue;
        for (j = 0; j < a.entry_count && !img->pixels; j++) {
            const vc_media_entry *m = &a.entries[j];
            const char *ext = strrchr(m->path, '.');
            char path[1024];
            if (resolve_media(e, m->path, path, sizeof(path)) != 0)
                continue;
            if (ext && strcmp(ext, ".pff") == 0) {
                u32 size;
                u8 *data;
                if (!e->pff_open)
                    e->pff_open = xf_pff_open(&e->pff, path) == 0;
                data = e->pff_open ? xf_pff_read(&e->pff, m->start, &size) : NULL;
                if (data && xf_pict_decode(data, size, img) != 0)
                    memset(img, 0, sizeof(*img));
                free(data);
            } else {
                int w = it->item.region.right - it->item.region.left;
                int h = it->item.region.bottom - it->item.region.top;
                if (open_nav_file(e, m->path) == 0 && w > 0 && h > 0) {
                    img->pixels = calloc((size_t)w * h, 4);
                    img->width = w;
                    img->height = h;
                    if (img->pixels && xf_movie_frame_at(&e->nav, m->start, img->pixels, w, h) != 0) {
                        free(img->pixels);
                        memset(img, 0, sizeof(*img));
                    }
                }
            }
        }
    }
}

static void free_iface_pictures(explorer *e)
{
    int k, i;
    for (k = 0; k < GAME_MAX_IFACES; k++)
        for (i = 0; i < GAME_MAX_IFACE_ITEMS; i++) {
            free(e->iface_img[k][i].pixels);
            memset(&e->iface_img[k][i], 0, sizeof(xf_image));
        }
}

static void refresh_iface_pictures(explorer *e)
{
    int k, i;
    if (e->iface_serial == e->g->iface_serial)
        return;
    e->iface_serial = e->g->iface_serial;
    free_iface_pictures(e);
    for (k = 0; k < e->g->iface_count; k++)
        for (i = 0; i < e->g->ifaces[k].count; i++)
            load_item_picture(e, &e->g->ifaces[k].items[i], &e->iface_img[k][i]);
}

static void draw_ifaces(explorer *e)
{
    int k, i, y, order;
    for (k = 0; k < e->g->iface_count; k++)
        for (order = 0; order < 256; order++) /* back to front */
            for (i = 0; i < e->g->ifaces[k].count; i++) {
                const game_iface_item *it = &e->g->ifaces[k].items[i];
                const xf_image *img = &e->iface_img[k][i];
                int x0 = it->item.region.left, y0 = it->item.region.top, w, h;
                if ((it->item.flags & 0xFF) != order || !img->pixels || !vc_is_enabled(&e->g->interp, it->id))
                    continue;
                w = XF_MIN(img->width, SCREEN_W - x0);
                h = XF_MIN(img->height, SCREEN_H - y0);
                if (x0 < 0 || y0 < 0 || w <= 0)
                    continue;
                for (y = 0; y < h; y++)
                    memcpy(&e->fb[(y0 + y) * SCREEN_W + x0], img->pixels + (size_t)y * img->width, (size_t)w * 4);
            }
}

/* --- Clips and sounds ------------------------------------------------ */

/* Starts the next queued clip. A clip missing on disk still runs its
 * triggers, so the story goes on; if the location card belongs to it,
 * the card is shown alone for its typing time plus two seconds. */
static void start_next_media(explorer *e)
{
    game_media m;
    while (!e->playing && game_media_start(e->g, &m)) {
        vc_asset_ref a;
        int j;
        e->have_clip = 0;
        if (vc_read_asset_ref(&e->g->db, m.asset, &a) == 0)
            for (j = 0; j < a.entry_count && !e->have_clip; j++) {
                char path[1024];
                if (resolve_media(e, a.entries[j].path, path, sizeof(path)) == 0 &&
                    xf_player_open(&e->player, path, plat_ticks_ms()) == 0) {
                    e->have_clip = 1;
                    xf_info("playing %s (%s)", a.logical, a.entries[j].path);
                }
            }
        e->card_active = m.card;
        e->card_start = plat_ticks_ms();
        if (e->have_clip) {
            e->playing = 1;
        } else if (m.card) {
            e->playing = 1;
            e->card_until = e->card_start + (u32)strlen(e->g->card) * 50 + 2000;
        } else {
            xf_info("media %s: no file on disk", a.logical);
            game_media_end(e->g);
        }
    }
}

static void stop_media(explorer *e)
{
    if (e->have_clip)
        xf_player_close(&e->player);
    e->have_clip = e->playing = e->card_active = 0;
    e->card_until = 0;
}

/* Ends the clip on screen and runs its end triggers. */
static void finish_media(explorer *e)
{
    stop_media(e);
    game_media_end(e->g);
}

static void update_sound(explorer *e)
{
    u32 now = plat_ticks_ms();
    if (e->sound_serial != e->g->sound_serial) {
        vc_asset_ref a;
        int j;
        e->sound_serial = e->g->sound_serial;
        stop_sound(e);
        if (vc_read_asset_ref(&e->g->db, e->g->sound, &a) == 0) {
            for (j = 0; j < a.entry_count && !e->sound_open; j++)
                if (resolve_media(e, a.entries[j].path, e->sound_path, sizeof(e->sound_path)) == 0 &&
                    xf_player_open(&e->sound, e->sound_path, now) == 0)
                    e->sound_open = 1;
            e->sound_loop = strstr(a.logical, "/Loops/") != NULL;
            xf_info("sound %s%s", a.logical, e->sound_open ? "" : ": no file on disk");
        }
    }
    if (!e->sound_open || xf_player_update(&e->sound, now))
        return;
    xf_player_close(&e->sound);
    e->sound_open = e->sound_loop && xf_player_open(&e->sound, e->sound_path, now) == 0;
}

static void stop_sound(explorer *e)
{
    if (e->sound_open)
        xf_player_close(&e->sound);
    e->sound_open = 0;
}

/* --- Drawing --------------------------------------------------------- */

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
    const game *g = e->g;
    int i, y;

    memset(e->fb, 0, sizeof(e->fb));
    if (e->playing) {
        const u32 *f = e->have_clip ? xf_player_frame(&e->player) : NULL;
        const char *sub = e->have_clip ? xf_player_subtitle(&e->player) : NULL;
        if (f) {
            /* Scene clips fill the picture area; other clips (logos, titles)
             * are centred on the screen. */
            int w = XF_MIN(e->player.video.width, SCREEN_W), h = XF_MIN(e->player.video.height, SCREEN_H);
            int x0 = (SCREEN_W - w) / 2, y0 = (SCREEN_H - h) / 2;
            if (w == PICTURE_W && h == PICTURE_H) {
                x0 = PICTURE_X;
                y0 = PICTURE_Y;
            }
            for (y = 0; y < h; y++)
                memcpy(&e->fb[(y0 + y) * SCREEN_W + x0], f + y * e->player.video.width, (size_t)w * 4);
        }
        if (sub && e->have_font)
            xf_text_draw_wrapped(&s, &e->font, 20, SCREEN_W / 2, PICTURE_Y + PICTURE_H + 30, SCREEN_W - 40,
                                 sub, 0xFFFFFF, 0x000000);
        if (e->card_active && e->have_font) {
            char shown[256];
            size_t n = (plat_ticks_ms() - e->card_start) / 50;
            n = XF_MIN(n, strlen(g->card));
            memcpy(shown, g->card, n);
            shown[n] = '\0';
            xf_text_draw_wrapped(&s, &e->font, 22, SCREEN_W / 2, PICTURE_Y + PICTURE_H + 34, SCREEN_W - 40,
                                 shown, 0xFFFFFF, 0x000000);
        }
        return;
    }
    if (e->have_picture)
        for (y = 0; y < PICTURE_H; y++)
            memcpy(&e->fb[(PICTURE_Y + y) * SCREEN_W + PICTURE_X], &e->picture[y * PICTURE_W],
                   PICTURE_W * 4);
    for (i = 0; i < e->anim_count; i++) {
        /* Centred in the picture area; full-size ones cover it. */
        const xf_video *vd = &e->anims[i].video;
        int w = XF_MIN(vd->width, PICTURE_W), h = XF_MIN(vd->height, PICTURE_H);
        int x0 = PICTURE_X + (PICTURE_W - w) / 2, y0 = PICTURE_Y + (PICTURE_H - h) / 2;
        update_anim(&e->anims[i], plat_ticks_ms());
        for (y = 0; y < h; y++)
            memcpy(&e->fb[(y0 + y) * SCREEN_W + x0], vd->frame + (size_t)y * vd->width, (size_t)w * 4);
    }
    for (i = 0; i < g->spot_count; i++) {
        const vc_region *r = &g->spots[i].region;
        int inside = mx >= r->left && mx < r->right && my >= r->top && my < r->bottom;
        frame_rect(e, r, inside ? 0xFFFFFF : colors[g->spots[i].kind]);
    }
    if (e->have_font) {
        static const char help[] = "green: move  yellow: examine  cyan: character  right click: back";
        xf_text_draw(&s, &e->font, 14, 8, 20, e->label, strlen(e->label), 0xC0C0C0);
        xf_text_draw(&s, &e->font, 14, 8, 470, help, sizeof(help) - 1, 0x808080);
    }
    draw_ifaces(e);
}

/* 0x471820 play_intro_sequence: Fox logo, HyperBole logo, teaser and
 * title, each skippable with a click. The DVD build plays its own DVD
 * titles instead of the last two; we use the QuickTime files. */
static void queue_intro(explorer *e)
{
    static const u32 intro[] = { 56003, 56002, 19668, 56001 };
    size_t i;
    if (getenv("XF_SKIP_INTRO"))
        return;
    for (i = 0; i < XF_COUNT(intro); i++)
        game_queue_media(e->g, intro[i]);
}

static int open_asset_player(explorer *e, u32 asset, xf_player *p)
{
    vc_asset_ref a;
    int j;
    if (vc_read_asset_ref(&e->g->db, asset, &a) != 0)
        return -1;
    for (j = 0; j < a.entry_count; j++) {
        char path[1024];
        if (resolve_media(e, a.entries[j].path, path, sizeof(path)) == 0 &&
            xf_player_open(p, path, plat_ticks_ms()) == 0)
            return 0;
    }
    xf_warn("menu: cannot open media %u", asset);
    return -1;
}

static void menu_close(explorer *e)
{
    int i;
    if (e->right_open) {
        xf_video_close(&e->right);
        xf_mov_close(&e->right_mov);
        e->right_open = 0;
    }
    for (i = 0; i < MENU_PARTS; i++)
        if (e->menu_open[i]) {
            xf_player_close(&e->menu[i]);
            e->menu_open[i] = 0;
        }
    e->in_menu = 0;
}

static void menu_enter(explorer *e)
{
    int i;
    stop_media(e);
    stop_sound(e);
    for (i = 0; i < MENU_PARTS; i++)
        if (i != MENU_RIGHT)
            e->menu_open[i] = open_asset_player(e, g_menu_parts[i].asset, &e->menu[i]) == 0;
    {
        vc_asset_ref a;
        char path[1024];
        if (vc_read_asset_ref(&e->g->db, g_menu_parts[MENU_RIGHT].asset, &a) == 0 && a.entry_count &&
            resolve_media(e, a.entries[0].path, path, sizeof(path)) == 0 &&
            xf_mov_open(&e->right_mov, path) == 0) {
            xf_mov_track *t = xf_mov_find(&e->right_mov, XF_TRACK_VIDEO);
            e->right_open = t && xf_video_open(&e->right, &e->right_mov, t) == 0;
        }
        e->right_frame = -1;
        e->right_target = RIGHT_INTRO_END;
        e->right_next_ms = plat_ticks_ms();
    }
    e->in_menu = 1;
    e->menu_hover = -1;
}

/* Steps RIGHT.mov one frame at a time towards right_target, at the
 * movie's own frame durations. */
static void menu_step_right(explorer *e)
{
    const xf_mov_track *t;
    u32 now = plat_ticks_ms();
    if (!e->right_open || e->right_frame == e->right_target || now < e->right_next_ms)
        return;
    t = e->right.track;
    e->right_frame = e->right_frame < e->right_target ? e->right_frame + 1 : e->right_target;
    if (xf_video_seek(&e->right, (u32)e->right_frame) != 0)
        return;
    {
        u32 i = (u32)e->right_frame;
        u32 dur = i + 1 < t->sample_count ? t->sample_time[i + 1] - t->sample_time[i] : 40;
        e->right_next_ms = now + dur * 1000 / t->timescale;
    }
}

static void menu_set_hover(explorer *e, int item)
{
    if (item == e->menu_hover)
        return;
    e->menu_hover = item;
    if (e->right_frame < RIGHT_INTRO_END)
        return; /* labels still animating in */
    if (item >= 0) {
        e->right_frame = g_menu_rollover[item].first - 1;
        e->right_target = g_menu_rollover[item].last;
    } else {
        e->right_frame = RIGHT_INTRO_END - 1;
        e->right_target = RIGHT_INTRO_END;
    }
    e->right_next_ms = plat_ticks_ms();
}

static void menu_update(explorer *e)
{
    int i;
    for (i = 0; i < MENU_PARTS; i++) {
        if (!e->menu_open[i])
            continue;
        if (!xf_player_update(&e->menu[i], plat_ticks_ms()) && g_menu_parts[i].loop) {
            /* Loops restart from the beginning. */
            xf_player_close(&e->menu[i]);
            e->menu_open[i] = open_asset_player(e, g_menu_parts[i].asset, &e->menu[i]) == 0;
        }
        /* Finished parts keep showing their last frame. */
    }
    menu_step_right(e);
}

static void menu_draw(explorer *e)
{
    int i, y;
    memset(e->fb, 0, sizeof(e->fb));
    for (i = 0; i < MENU_PARTS; i++) {
        const u32 *f = e->menu_open[i] ? xf_player_frame(&e->menu[i]) : NULL;
        int w, h, x0 = g_menu_parts[i].x, y0 = g_menu_parts[i].y;
        if (!f)
            continue;
        w = XF_MIN(e->menu[i].video.width, SCREEN_W - x0);
        h = XF_MIN(e->menu[i].video.height, SCREEN_H - y0);
        for (y = 0; y < h; y++)
            memcpy(&e->fb[(y0 + y) * SCREEN_W + x0], f + y * e->menu[i].video.width, (size_t)w * 4);
    }
    if (e->right_open && e->right_frame >= 0)
        for (y = 0; y < e->right.height; y++)
            memcpy(&e->fb[y * SCREEN_W + 472], e->right.frame + y * e->right.width, (size_t)e->right.width * 4);
}

static int menu_item_at(int x, int y)
{
    size_t i;
    if (x < 472)
        return -1;
    for (i = 0; i < XF_COUNT(g_menu_items); i++)
        if (y >= g_menu_items[i].top && y < g_menu_items[i].bottom)
            return (int)i;
    return -1;
}

int xf_explore(const char *data_dir, const char *const *media_dirs, u32 start_view, int disc)
{
    explorer *e = calloc(1, sizeof(*e));
    char path[1024];
    int running = 1, mx = 0, my = 0;

    if (!e || !(e->g = malloc(sizeof(game))) || game_open(e->g, data_dir) != 0) {
        if (e)
            free(e->g);
        free(e);
        return 1;
    }
    e->data_dir = data_dir;
    e->media_dirs = media_dirs;
    e->disc = disc;
    if (xf_path_resolve(data_dir, "DLG.TTF", path, sizeof(path)) == 0 ||
        xf_path_resolve(data_dir, "DLG.TTR", path, sizeof(path)) == 0)
        e->have_font = xf_font_load(&e->font, path) == 0;
    if (start_view == 0) {
        /* Like the original: intro, then the main menu. */
        queue_intro(e);
        e->in_menu = -1; /* open the menu once the intro has played */
    } else if (game_enter_view(e->g, start_view) != 0) {
        running = 0;
    }
    while (running) {
        plat_event ev;
        game_tick(e->g, plat_ticks_ms());
        while (plat_poll(&ev)) {
            if (ev.type == PLAT_EV_QUIT || (ev.type == PLAT_EV_KEY_DOWN && ev.key == PLAT_KEY_ESCAPE))
                running = 0;
            else if (e->in_menu == 1 && ev.type == PLAT_EV_MOUSE_MOVE)
                menu_set_hover(e, menu_item_at(ev.x, ev.y));
            else if (e->in_menu == 1 && ev.type == PLAT_EV_MOUSE_DOWN && ev.button == PLAT_MOUSE_LEFT) {
                int item = menu_item_at(ev.x, ev.y);
                if (item >= 0)
                    xf_info("menu: %s", g_menu_items[item].name);
                if (item == 0) { /* New */
                    menu_close(e);
                    game_new(e->g);
                } else if (item == 6) { /* Quit */
                    running = 0;
                }
            } else if (ev.type == PLAT_EV_MOUSE_MOVE) {
                mx = ev.x;
                my = ev.y;
                if (!e->playing)
                    game_hover(e->g, mx, my);
            } else if (e->playing && (ev.type == PLAT_EV_MOUSE_DOWN || ev.type == PLAT_EV_KEY_DOWN)) {
                if (!e->card_active)
                    finish_media(e); /* skip the clip; the location card cannot be skipped */
            } else if (ev.type == PLAT_EV_MOUSE_DOWN && ev.button == PLAT_MOUSE_LEFT) {
                game_click(e->g, ev.x, ev.y);
            } else if ((ev.type == PLAT_EV_MOUSE_DOWN && ev.button == PLAT_MOUSE_RIGHT) ||
                       (ev.type == PLAT_EV_KEY_DOWN && ev.key == 8)) {
                game_back(e->g);
            }
        }
        if ((e->have_clip && !xf_player_update(&e->player, plat_ticks_ms())) ||
            (e->card_until && (i32)(plat_ticks_ms() - e->card_until) >= 0))
            finish_media(e);
        start_next_media(e);
        update_sound(e);
        if (e->in_menu == -1 && !e->playing && !game_media_pending(e->g))
            menu_enter(e);
        if (e->in_menu == 1) {
            menu_update(e);
            menu_draw(e);
        } else {
            if (e->g->view)
                refresh_picture(e);
            refresh_iface_pictures(e);
            draw(e, mx, my);
        }
        plat_present(e->fb, SCREEN_W, SCREEN_H, SCREEN_W);
        plat_sleep_ms(e->playing ? 5 : 16);
    }
    stop_media(e);
    stop_sound(e);
    close_anims(e);
    free_iface_pictures(e);
    if (e->pff_open)
        xf_pff_close(&e->pff);
    menu_close(e);
    if (e->nav_open)
        xf_mov_close(&e->nav);
    if (e->have_font) {
        xf_text_cache_clear();
        xf_font_free(&e->font);
    }
    game_close(e->g);
    free(e->g);
    free(e);
    return 0;
}
