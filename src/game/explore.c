/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
#include "explore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/mixer.h"
#include "game/game.h"
#include "game/script.h"
#include "gfx/screen.h"
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
#define MENU_RETURN 72895 /* return.pic */
/* The main menu icon: the spinning blue X (25541) in the top-right
 * corner, as the Help screen shows it; it appears when the pointer comes
 * there, as the inventory does at the bottom, and a click opens the
 * menu. */
#define MENU_ICON 25541
#define MENU_ICON_X 565
#define MENU_ICON_Y 29
#define MENU_ICON_REVEAL_X 500
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
    u32 asset;            /* the media reference it shows */
} view_anim;

#define MAX_ANIMS 4
#define MAX_ICONS 6
#define MAX_INV 32
#define MAX_CURSORS 24
#define MAX_CURSOR_FRAMES 12
#define CARD_PX 16 /* 12 point at 96 dots per inch */
#define CROSS_SIZE 16

/* A VCCursor: its hot point and pictures (several for animated ones). */
typedef struct {
    u32 id;
    int hot_x, hot_y;
    xf_image frames[MAX_CURSOR_FRAMES];
    int count;
} cursor_art;

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
    int media_paused;     /* the game's clips and sounds hold while the menu is up */
    u32 media_pause_from;
    int from_menu;        /* Options or Help, opened from the menu during a game */
    xf_image return_img;  /* return.pic: "Return" over the Previous row */
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
    u32 fade_serial, fade_start, fade_view; /* FadeToBlack */
    xf_player overlay;    /* PlayAssetAt: a small clip over the screen */
    int overlay_open;
    u32 overlay_serial;
    char sound_path[1024];
    xf_font font;         /* Dlg.ttr: dialogue and labels */
    int have_font;
    xf_font card_font;    /* Courier New: location cards */
    int have_card_font;
    xf_font hcd_font;     /* Hcd.ttr: computer screens */
    int have_hcd_font;
    xf_font phn_font;     /* Phn.ttr: the cell phone's display */
    int have_phn_font;
    int debug;            /* Ctrl+Shift+D: hotspot outlines and names */
    u32 paused_ms;        /* time spent in the menu and message boxes */
    u32 clip_stop_serial;
    struct {
        u32 asset;
        xf_image img;
    } photo_cache[GAME_MAX_PHOTOS]; /* the photo screen's pictures */
    u32 pause_from;       /* when the pause started, 0 when running */
    /* dialogue box */
    xf_image dlg_talk, dlg_history, dlg_up, dlg_down;
    int talk_scroll;
    int talk_tab;         /* 0 TALK: questions not asked yet; 1 HISTORY: asked */
    /* emotion and action icons */
    view_anim icon_anim[MAX_ICONS];
    int icon_open[MAX_ICONS];
    u32 icon_ids[MAX_ICONS];
    int icon_count;
    /* cursors */
    cursor_art cursors[MAX_CURSORS];
    cursor_art cross;     /* the default pointer */
    u32 cross_px[CROSS_SIZE * CROSS_SIZE];
    int cursor_count;
    u32 held_inv;         /* VCInventory taken in hand, for its cursor */
    /* inventory icons: normal and rollover */
    xf_image inv_img[MAX_INV][2];
    u32 inv_ids[MAX_INV];
    int inv_count;
    int inv_items;        /* inv_ids: the inventory, then the lab's evidence */
    u32 *fb;              /* where drawing goes: scene_fb, or ui_fb in the wide layout */
    u32 scene_fb[SCREEN_W * SCREEN_H];
    u32 ui_fb[SCREEN_W * SCREEN_H]; /* the interface, SCR_CLEAR where the scene shows */
    /* the output: the wide layout, or the 640x480 screen as it is */
    int wide_pref;        /* the wide layout: the saved setting, XF_SCREEN, Ctrl+Shift+W */
    scr_layout layout;    /* of the frame shown last */
    u32 *out;
    size_t out_cap;
    int out_mx, out_my;   /* the pointer on the output */
    u32 piece_px[SCREEN_W * SCREEN_H]; /* the interface pieces, cut out of ui_fb */
    size_t piece_used;
    const u32 *shown;     /* the frame shown last, for screenshots */
    int shown_w, shown_h;
    /* the view picture: still and moving media */
    u32 picture[PICTURE_W * PICTURE_H];
    int have_picture;
    u32 shown_view;
    u32 view_media[32];
    int view_media_count;
    view_anim anims[MAX_ANIMS];
    int anim_count;
    char label[256];
    int running, mx, my;
    /* pictures of the interface items on screen, by layout and item */
    xf_image iface_img[GAME_MAX_IFACES][GAME_MAX_IFACE_ITEMS];
    u32 iface_serial;
    xf_pff pff;           /* x.pff, for interface pictures */
    int pff_open;
    char pff_path[1024];  /* the PFF bundle open in pff */
    /* one open navigation file */
    char nav_path[1024];
    xf_mov nav;
    int nav_open;
} explorer;

/* The screen as a drawing surface. */
static xf_surface screen_surface(explorer *e)
{
    xf_surface s;
    s.pixels = e->fb;
    s.width = SCREEN_W;
    s.height = SCREEN_H;
    s.pitch = SCREEN_W;
    return s;
}

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

/* A click on a moving picture of the view: the HOT rectangle under it at
 * the clip's current time, if any, takes the shot (game_shoot). */
static int shoot_at(explorer *e, int x, int y)
{
    int i;
    for (i = 0; i < e->anim_count; i++) {
        const view_anim *an = &e->anims[i];
        const xf_video *vd = &an->video;
        int w = XF_MIN(vd->width, PICTURE_W), h = XF_MIN(vd->height, PICTURE_H);
        int x0 = PICTURE_X + (PICTURE_W - w) / 2, y0 = PICTURE_Y + (PICTURE_H - h) / 2;
        u32 t = plat_ticks_ms() - an->started, hot;
        t = an->loop ? t % an->length_ms : XF_MIN(t, an->length_ms - 1);
        if (x < x0 || y < y0 || x >= x0 + w || y >= y0 + h)
            continue;
        hot = game_hot_at(e->g, an->asset, x - x0, y - y0, an->start_ms + t);
        if (hot && game_shoot(e->g, hot))
            return 1;
    }
    return 0;
}

/* A click on the clip being played: the HOT rectangle under it, if any,
 * takes the shot. Scene clips fill the picture area. */
static int clip_shot(explorer *e, int x, int y)
{
    u32 hot, t;
    if (!e->g->current.asset || e->player.video.width != PICTURE_W || e->player.video.height != PICTURE_H)
        return 0;
    if (x < PICTURE_X || y < PICTURE_Y || x >= PICTURE_X + PICTURE_W || y >= PICTURE_Y + PICTURE_H)
        return 0;
    t = e->player.now_ms;
    hot = game_hot_at(e->g, e->g->current.asset, x - PICTURE_X, y - PICTURE_Y, t);
    return hot && game_shoot(e->g, hot);
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
                    e->anims[e->anim_count - 1].asset = e->view_media[i];
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
/* Pictures come from several PFF bundles (X.PFF, X7.PFF, the Save
 * screen's cursor.pff): keeps the one last used open. */
static void use_pff(explorer *e, const char *path)
{
    if (e->pff_open && strcmp(e->pff_path, path) == 0)
        return;
    if (e->pff_open)
        xf_pff_close(&e->pff);
    e->pff_open = xf_pff_open(&e->pff, path) == 0;
    snprintf(e->pff_path, sizeof(e->pff_path), "%s", path);
}

/* The picture of one media reference: a PICT in a PFF bundle, or a frame
 * of the navigation movie (w by h). */
static void load_asset_picture(explorer *e, u32 asset, int w, int h, xf_image *img)
{
    vc_asset_ref a;
    int j;
    memset(img, 0, sizeof(*img));
    if (vc_read_asset_ref(&e->g->db, asset, &a) != 0)
        return;
    for (j = 0; j < a.entry_count && !img->pixels; j++) {
        const vc_media_entry *m = &a.entries[j];
        const char *ext = strrchr(m->path, '.');
        char path[1024];
        if (resolve_media(e, m->path, path, sizeof(path)) != 0)
            continue;
        if (ext && strcmp(ext, ".pff") == 0) {
            u32 size;
            u8 *data;
            use_pff(e, path);
            data = e->pff_open ? xf_pff_read(&e->pff, m->start, &size) : NULL;
            if (data && xf_pict_decode(data, size, img) != 0)
                memset(img, 0, sizeof(*img));
            free(data);
        } else if (open_nav_file(e, m->path) == 0 && w > 0 && h > 0) {
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

static void load_item_picture(explorer *e, const game_iface_item *it, xf_image *img)
{
    u32 ids[8];
    int n = vc_read_list(&e->g->db, it->item.assets, ids, 8), i;
    memset(img, 0, sizeof(*img));
    for (i = 0; i < n && !img->pixels; i++)
        load_asset_picture(e, ids[i], it->item.region.right - it->item.region.left,
                           it->item.region.bottom - it->item.region.top, img);
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

static void blit_keyed(explorer *e, const xf_image *img, int x0, int y0);
static void draw_message(explorer *e, int mx, int my);
static void place_slot_cursor(explorer *e);

static int wrap_lines_font(const xf_font *f, const char *text, float px, int w, int *starts, int *lens, int max);

/* The scrolling text box (ScrollTextList, a mail): the text's own lines,
 * each wrapped to the item's width, from the first line shown down to the
 * bottom of the item. */
static void draw_scroll_box(explorer *e)
{
    const game_scroll *sb = game_scroll_box(e->g);
    const game_iface_item *it;
    const xf_font *f;
    xf_surface sf = screen_surface(e);
    float px;
    int y, line = 0, pos = 0, len, lh;
    if (!sb || e->g->text_off || !(it = game_find_item(e->g, sb->item)) || !e->have_font)
        return;
    f = sb->font == 'H' && e->have_hcd_font ? &e->hcd_font
      : sb->font == 'c' && e->have_card_font ? &e->card_font : &e->font;
    px = (float)(sb->size ? sb->size : 12) * 96.0f / 72.0f;
    lh = (int)(px * 1.2f);
    y = it->item.region.top + (int)px;
    len = (int)strlen(sb->text);
    while (pos <= len) {
        int end = pos, starts[64], lens[64], n, k;
        char para[1024];
        while (end < len && sb->text[end] != '\n')
            end++;
        snprintf(para, sizeof(para), "%.*s", end - pos, sb->text + pos);
        n = para[0] ? wrap_lines_font(f, para, px, it->item.region.right - it->item.region.left - 8, starts, lens, 64) : 1;
        if (!para[0])
            lens[0] = 0, starts[0] = 0;
        for (k = 0; k < n; k++, line++) {
            if (line < sb->line)
                continue;
            if (y > it->item.region.bottom)
                return;
            xf_text_draw(&sf, f, px, it->item.region.left + 4, y, para + starts[k], (size_t)lens[k],
                         sb->color == 'w' ? 0xFFFFFF : 0x000000);
            y += lh;
        }
        pos = end + 1;
    }
}

/* A picture scaled (nearest pixel) into a w by h box at x0, y0. */
static void blit_scaled(explorer *e, const xf_image *img, int x0, int y0, int w, int h)
{
    int x, y;
    if (!img->pixels || w <= 0 || h <= 0)
        return;
    for (y = 0; y < h; y++) {
        int sy = y * img->height / h, dy = y0 + y;
        if (dy < 0 || dy >= SCREEN_H)
            continue;
        for (x = 0; x < w; x++) {
            int dx = x0 + x;
            if (dx >= 0 && dx < SCREEN_W)
                e->fb[dy * SCREEN_W + dx] = img->pixels[sy * img->width + x * img->width / w] & 0xFFFFFF;
        }
    }
}

static const xf_image *photo_picture(explorer *e, u32 asset)
{
    int i, free_slot = -1;
    for (i = 0; i < GAME_MAX_PHOTOS; i++) {
        if (e->photo_cache[i].asset == asset)
            return &e->photo_cache[i].img;
        if (!e->photo_cache[i].asset && free_slot < 0)
            free_slot = i;
    }
    if (free_slot < 0) { /* full: drop the first */
        free(e->photo_cache[0].img.pixels);
        memmove(&e->photo_cache[0], &e->photo_cache[1], sizeof(e->photo_cache[0]) * (GAME_MAX_PHOTOS - 1));
        free_slot = GAME_MAX_PHOTOS - 1;
    }
    e->photo_cache[free_slot].asset = asset;
    load_asset_picture(e, asset, 0, 0, &e->photo_cache[free_slot].img);
    return &e->photo_cache[free_slot].img;
}

/* The computer's photo screen (layout 15232): the thumbnails in the
 * items that call PhotoEnlarge(slot), the enlarged photo in item 51396. */
static void draw_photos(explorer *e)
{
    static const u32 slots[20] = { 51376, 51378, 51379, 51377, 51380, 51381, 51384, 51385, 51386, 51387,
                                   51382, 51388, 51389, 51390, 51391, 51383, 51392, 51393, 51394, 51395 };
    const game_iface_item *big = game_find_item(e->g, 51396);
    int k;
    u32 asset;
    if (!big)
        return;
    for (k = 0; k < 20; k++) {
        const game_iface_item *it = game_find_item(e->g, slots[k]);
        const vc_region *r;
        if (!it || !(asset = game_photo_at(e->g, k)))
            continue;
        r = &it->item.region;
        blit_scaled(e, photo_picture(e, asset), r->left, r->top, r->right - r->left, r->bottom - r->top);
    }
    if ((asset = game_photo_at(e->g, -1)))
        blit_scaled(e, photo_picture(e, asset), big->item.region.left, big->item.region.top,
                    big->item.region.right - big->item.region.left, big->item.region.bottom - big->item.region.top);
}

/* The Options screen's Graphics tab (layout 49783) has a Closed
 * Captioning row: button 49939 (PrefClosedCap) and its lights 49889
 * (off) and 49891 (on). The tab's background lies in front of them and
 * has no label for the row, so the shipped game never shows it. We draw
 * the row over the background, with a label like the others. */
#define OPTIONS_GRAPHICS 49783
static const u32 g_caption_row[] = { 49939, 49889, 49891 };

static void draw_caption_option(explorer *e)
{
    static const char label[] = "Closed Captioning";
    int k, i, j;
    for (k = 0; k < e->g->iface_count; k++) {
        if (e->g->ifaces[k].layout != OPTIONS_GRAPHICS)
            continue;
        for (j = 0; j < (int)XF_COUNT(g_caption_row); j++)
            for (i = 0; i < e->g->ifaces[k].count; i++) {
                const game_iface_item *it = &e->g->ifaces[k].items[i];
                if (it->id == g_caption_row[j] && vc_is_enabled(&e->g->interp, it->id))
                    blit_keyed(e, &e->iface_img[k][i], it->item.region.left, it->item.region.top);
            }
        if (e->have_font) {
            /* letter-spaced grey, as the tab's own labels */
            xf_surface sf = screen_surface(e);
            int x = 79;
            const char *c;
            for (c = label; *c; c++) {
                xf_text_draw(&sf, &e->font, 14, x, 424, c, 1, 0xC0C0C0);
                x += xf_text_width(&e->font, 14, c, 1) + 3;
            }
        }
    }
}

static void draw_ifaces(explorer *e)
{
    int k, i, order;
    char buf[64];
    for (k = 0; k < e->g->iface_count; k++)
        for (order = 255; order >= 0; order--) /* back to front: a low order is in front */
            for (i = 0; i < e->g->ifaces[k].count; i++) {
                const game_iface_item *it = &e->g->ifaces[k].items[i];
                if ((it->item.flags & 0xFF) == order && vc_is_enabled(&e->g->interp, it->id))
                    blit_keyed(e, &e->iface_img[k][i], it->item.region.left, it->item.region.top);
            }
    draw_caption_option(e);
    /* SetIItemText labels: font 'H' is Hcd.ttr, 'c' Courier New; size in
     * points (96 dots per inch). */
    for (k = 0; k < e->g->iface_count; k++)
        for (i = 0; i < e->g->ifaces[k].count; i++) {
            const game_iface_item *it = &e->g->ifaces[k].items[i];
            const game_item_label *l = game_item_label_of(e->g, it->id);
            const xf_font *f;
            const vc_region *r = &it->item.region;
            float px;
            int w, x;
            xf_surface sf = screen_surface(e);
            if (!l || !l->text[0] || !vc_is_enabled(&e->g->interp, it->id))
                continue;
            f = l->font == 'H' && e->have_hcd_font ? &e->hcd_font
              : l->font == 'P' && e->have_phn_font ? &e->phn_font
              : l->font == 'c' && e->have_card_font ? &e->card_font : &e->font;
            px = (float)l->size * 96.0f / 72.0f;
            if (l->align == 'w') {
                /* a page of text: wrapped to the item, from its top */
                const char *t = l->text;
                int y = r->top + (int)px, lh = (int)(px * 1.2f);
                while (*t && y <= r->bottom) {
                    int starts[64], lens[64], nl, k;
                    size_t plen = strcspn(t, "\n");
                    char para[1024];
                    snprintf(para, sizeof(para), "%.*s", (int)plen, t);
                    nl = para[0] ? wrap_lines_font(f, para, px, r->right - r->left, starts, lens, 64) : 1;
                    if (!para[0])
                        lens[0] = starts[0] = 0;
                    for (k = 0; k < nl && y <= r->bottom; k++, y += lh)
                        xf_text_draw(&sf, f, px, r->left, y, para + starts[k], (size_t)lens[k], 0x000000);
                    t += plen + (t[plen] == '\n');
                }
                continue;
            }
            {
                /* One line per newline, the block centred in the item. */
                const char *line = l->text;
                int lines = 1, lh = (int)(px * 1.2f), y;
                const char *c;
                for (c = l->text; *c; c++)
                    lines += *c == '\n';
                y = r->bottom - (r->bottom - r->top - (int)px) / 2 - 3 - (lines - 1) * lh / 2;
                while (line) {
                    const char *nl = strchr(line, '\n');
                    size_t len = nl ? (size_t)(nl - line) : strlen(line);
                    w = xf_text_width(f, px, line, len);
                    while (len && w > r->right - r->left) /* cut at the item's edge */
                        w = xf_text_width(f, px, line, --len);
                    x = l->align == 'c' ? (r->left + r->right - w) / 2 : l->align == 'r' ? r->right - w : r->left;
                    xf_text_draw(&sf, f, px, x, y, line, len, l->color == 'b' ? 0xFFFFFF : 0x000000);
                    y += lh;
                    line = nl ? nl + 1 : NULL;
                }
            }
        }
    draw_scroll_box(e);
    draw_photos(e);
    /* Typed text in text fields, with a caret in the one that has focus. */
    for (k = 0; k < e->g->iface_count && e->have_font; k++)
        for (i = 0; i < e->g->ifaces[k].count; i++) {
            const game_iface_item *it = &e->g->ifaces[k].items[i];
            const char *text = game_item_text(e->g, it->id, buf, sizeof(buf) - 1);
            const vc_region *r = &it->item.region;
            if (!text)
                continue;
            if (it->id == e->g->focus_item && (plat_ticks_ms() / 500) % 2)
                strcat(buf, "|");
            {
                xf_surface sf = screen_surface(e);
                xf_text_draw(&sf, &e->font, 15, r->left + 8, (r->top + r->bottom) / 2 + 5, text, strlen(text),
                             0x000000);
            }
        }
}

/* --- Clips and sounds ------------------------------------------------ */

/* A player's mixer volume: its preference (0-100) times the game volume
 * (Pref_iVolumeGame). */
static void set_volume(explorer *e, const xf_player *p, u32 pref)
{
    if (p->channel >= 0)
        xf_mixer_set_volume(p->channel, (int)(game_pref(e->g, pref) * game_pref(e->g, 979) * 256 / 10000));
}

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
                    xf_player_open_range(&e->player, path, plat_ticks_ms(),
                                         a.entries[j].start + game_clip_offset(e->g, m.asset),
                                         a.entries[j].end) == 0) {
                    e->have_clip = 1;
                    set_volume(e, &e->player, 976); /* Pref_iVolumeDialog */
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
                    xf_player_open(&e->sound, e->sound_path, now) == 0) {
                    e->sound_open = 1;
                    set_volume(e, &e->sound, 978); /* Pref_iVolumeEffect */
                }
            e->sound_loop = strstr(a.logical, "/Loops/") != NULL || e->g->sound_looped;
            xf_info("sound %s%s", a.logical, e->sound_open ? "" : ": no file on disk");
        }
    }
    if (e->sound_open && xf_player_update(&e->sound, now))
        return;
    if (e->sound_open)
        xf_player_close(&e->sound);
    e->sound_open = e->sound_loop && xf_player_open(&e->sound, e->sound_path, now) == 0;
    /* A sound that ended, or has no file, runs its end triggers. */
    if (!e->sound_open && e->g->sound)
        game_sound_end(e->g);
}

static void stop_sound(explorer *e)
{
    if (e->sound_open)
        xf_player_close(&e->sound);
    e->sound_open = 0;
}

/* --- Game interface ---------------------------------------------------- */

/* The dialogue box (0x482610 loads its art, fixed in the engine): a
 * 246x120 folder with TALK and HISTORY tabs, a scroll bar and arrows.
 * 0x4810A0 centres it on the character and puts it at y 285. Blue is
 * transparent in the game's pictures. */
#define DLG_TALK 34761
#define DLG_HISTORY 34760
#define DLG_UP 34759
#define DLG_DOWN 34764
#define DLG_Y 285
#define DLG_W 246
#define DLG_TEXT_X 12
#define DLG_TEXT_W 210
#define DLG_TEXT_TOP 32
#define DLG_TEXT_BOTTOM 114
#define DLG_LINE 15
#define DLG_PX 13
#define KEY_BLUE 0x0000FF

/* Emotion and action icons: 80x60 animated clips below the video, each
 * with its name underneath on rollover. */
#define ICON_W 80
#define ICON_H 60
#define ICON_Y (PICTURE_Y + PICTURE_H + 10)
#define ICON_GAP 24

/* Inventory: 30x40 icons along the bottom, shown when the pointer is
 * near the bottom of the screen. */
#define INV_Y 436
#define INV_W 30
#define INV_H 40
#define INV_GAP 8
#define INV_REVEAL 420
#define INV_ROW_GAP 6

static int load_asset_image(explorer *e, u32 asset, xf_image *img)
{
    vc_asset_ref a;
    int j;
    memset(img, 0, sizeof(*img));
    if (vc_read_asset_ref(&e->g->db, asset, &a) != 0)
        return -1;
    for (j = 0; j < a.entry_count && !img->pixels; j++) {
        char path[1024];
        u32 size;
        u8 *data;
        if (!strstr(a.entries[j].path, ".pff") || resolve_media(e, a.entries[j].path, path, sizeof(path)) != 0)
            continue;
        use_pff(e, path);
        data = e->pff_open ? xf_pff_read(&e->pff, a.entries[j].start, &size) : NULL;
        if (data && xf_pict_decode(data, size, img) != 0)
            memset(img, 0, sizeof(*img));
        free(data);
    }
    return img->pixels ? 0 : -1;
}

/* The first media of a list as a picture. */
static int load_list_image(explorer *e, u32 list, xf_image *img)
{
    u32 ids[4];
    memset(img, 0, sizeof(*img));
    return vc_read_list(&e->g->db, list, ids, 4) > 0 ? load_asset_image(e, ids[0], img) : -1;
}

/* Copies a picture, leaving out the blue key colour. */
static void blit_keyed(explorer *e, const xf_image *img, int x0, int y0)
{
    int x, y;
    if (!img->pixels)
        return;
    for (y = 0; y < img->height; y++) {
        int sy = y0 + y;
        if (sy < 0 || sy >= SCREEN_H)
            continue;
        for (x = 0; x < img->width; x++) {
            int sx = x0 + x;
            u32 c = img->pixels[(size_t)y * img->width + x];
            if (sx >= 0 && sx < SCREEN_W && c != KEY_BLUE)
                e->fb[sy * SCREEN_W + sx] = c;
        }
    }
}

static int text_px(explorer *e, const char *text, float px)
{
    return e->have_font ? xf_text_width(&e->font, px, text, strlen(text)) : (int)strlen(text) * 7;
}

/* Text with a one-pixel black outline, readable over any picture. */
static void draw_outlined(explorer *e, const xf_font *f, float px, int x, int y, const char *text, size_t len,
                          u32 color)
{
    xf_surface s = screen_surface(e);
    int dx, dy;
    for (dy = -1; dy <= 1; dy++)
        for (dx = -1; dx <= 1; dx++)
            if (dx || dy)
                xf_text_draw(&s, f, px, x + dx, y + dy, text, len, 0x000000);
    xf_text_draw(&s, f, px, x, y, text, len, color);
}

/* Splits `text` into lines no wider than `w`. Returns the line count;
 * `starts` and `lens` describe each line. */
static int wrap_lines_font(const xf_font *f, const char *text, float px, int w, int *starts, int *lens, int max)
{
    int n = 0, pos = 0, len = (int)strlen(text);
    while (pos < len && n < max) {
        int end = pos, last_space = -1;
        while (end < len && xf_text_width(f, px, text + pos, (size_t)(end + 1 - pos)) <= w) {
            if (text[end] == ' ')
                last_space = end;
            end++;
        }
        if (end < len && last_space > pos)
            end = last_space;
        if (end == pos)
            end = pos + 1;
        starts[n] = pos;
        lens[n] = end - pos;
        n++;
        pos = end;
        while (pos < len && text[pos] == ' ')
            pos++;
    }
    return n;
}

static int wrap_lines(explorer *e, const char *text, float px, int w, int *starts, int *lens, int max)
{
    int n = 0, pos = 0, len = (int)strlen(text);
    while (pos < len && n < max) {
        int end = pos, last_space = -1;
        while (end < len) {
            char tmp[256];
            int k = XF_MIN(end + 1 - pos, (int)sizeof(tmp) - 1);
            memcpy(tmp, text + pos, (size_t)k);
            tmp[k] = '\0';
            if (text_px(e, tmp, px) > w)
                break;
            if (text[end] == ' ')
                last_space = end;
            end++;
        }
        if (end < len && last_space > pos)
            end = last_space;
        if (end == pos)
            end = pos + 1;
        starts[n] = pos;
        lens[n] = end - pos;
        n++;
        pos = end;
        while (pos < len && text[pos] == ' ')
            pos++;
    }
    return n;
}

/* Where the dialogue box goes: centred on the character, on screen. */
static int dialog_x(explorer *e)
{
    int i, cx = SCREEN_W / 2;
    for (i = 0; i < e->g->spot_count; i++)
        if (e->g->spots[i].id == e->g->talking)
            cx = (e->g->spots[i].region.left + e->g->spots[i].region.right) / 2;
    return XF_CLAMP(cx - DLG_W / 2, 0, SCREEN_W - DLG_W);
}

typedef struct {
    u32 id;
    int y, lines;
} dlg_entry;

/* Lays out the questions in the TALK folder from the scroll position.
 * Returns the number of entries visible; `more` is set when some are
 * below the box. */
static int dialog_layout(explorer *e, dlg_entry *out, int max, int *more)
{
    u32 all[GAME_MAX_CHOICES], ids[GAME_MAX_CHOICES];
    int m = game_choices(e->g, all, GAME_MAX_CHOICES), n = 0, i, k = 0, y = DLG_Y + DLG_TEXT_TOP;
    /* TALK lists what has not been asked yet; HISTORY what has. */
    for (i = 0; i < m; i++)
        if ((game_times_asked(e->g, all[i]) > 0) == (e->talk_tab == 1))
            ids[n++] = all[i];
    *more = 0;
    e->talk_scroll = XF_CLAMP(e->talk_scroll, 0, XF_MAX(n - 1, 0));
    for (i = e->talk_scroll; i < n && k < max; i++) {
        char q[256] = "";
        int starts[8], lens[8], lines;
        game_question_text(e->g, ids[i], q, sizeof(q));
        lines = wrap_lines(e, q, DLG_PX, DLG_TEXT_W, starts, lens, 8);
        if (y + (lines - 1) * DLG_LINE > DLG_Y + DLG_TEXT_BOTTOM) {
            *more = 1;
            break;
        }
        out[k].id = ids[i];
        out[k].y = y;
        out[k].lines = lines;
        k++;
        y += lines * DLG_LINE + 3;
    }
    return k;
}

static void draw_dialog(explorer *e, int mx, int my)
{
    xf_surface s = screen_surface(e);
    dlg_entry ent[GAME_MAX_CHOICES];
    int x0 = dialog_x(e), n, more, i, l;
    if (!e->dlg_talk.pixels) {
        load_asset_image(e, DLG_TALK, &e->dlg_talk);
        load_asset_image(e, DLG_HISTORY, &e->dlg_history);
    }
    if (!e->dlg_up.pixels) {
        load_asset_image(e, DLG_UP, &e->dlg_up);
        load_asset_image(e, DLG_DOWN, &e->dlg_down);
    }
    blit_keyed(e, e->talk_tab ? &e->dlg_history : &e->dlg_talk, x0, DLG_Y);
    if (!e->have_font)
        return;
    n = dialog_layout(e, ent, GAME_MAX_CHOICES, &more);
    for (i = 0; i < n; i++) {
        char q[256] = "";
        int starts[8], lens[8], lines;
        int over = mx >= x0 + DLG_TEXT_X && mx < x0 + DLG_TEXT_X + DLG_TEXT_W && my >= ent[i].y - DLG_LINE + 3 &&
                   my < ent[i].y + (ent[i].lines - 1) * DLG_LINE + 4;
        u32 color = over ? 0xFFFFFF : 0xD8D8D8;
        game_question_text(e->g, ent[i].id, q, sizeof(q));
        lines = wrap_lines(e, q, DLG_PX, DLG_TEXT_W, starts, lens, 8);
        for (l = 0; l < lines; l++)
            xf_text_draw(&s, &e->font, DLG_PX, x0 + DLG_TEXT_X, ent[i].y + l * DLG_LINE, q + starts[l],
                         (size_t)lens[l], color);
    }
    if (e->talk_scroll > 0)
        blit_keyed(e, &e->dlg_up, x0 + DLG_W - 17, DLG_Y + 22);
    if (more)
        blit_keyed(e, &e->dlg_down, x0 + DLG_W - 17, DLG_Y + 104);
}

/* Returns 1 when the click hit the dialogue box. */
static int click_dialog(explorer *e, int x, int y)
{
    dlg_entry ent[GAME_MAX_CHOICES];
    int x0 = dialog_x(e), n, more, i;
    if (x < x0 || x >= x0 + DLG_W || y < DLG_Y || y >= DLG_Y + 120)
        return 0;
    if (y < DLG_Y + 16) { /* the TALK and HISTORY tabs */
        e->talk_tab = x >= x0 + 100;
        e->talk_scroll = 0;
        return 1;
    }
    if (x >= x0 + DLG_W - 16 && y < DLG_Y + 30) { /* the close box */
        game_end_talk(e->g);
        return 1;
    }
    n = dialog_layout(e, ent, GAME_MAX_CHOICES, &more);
    if (x >= x0 + DLG_W - 18) { /* the scroll arrows */
        if (y < DLG_Y + 60 && e->talk_scroll > 0)
            e->talk_scroll--;
        else if (y >= DLG_Y + 60 && more)
            e->talk_scroll++;
        return 1;
    }
    for (i = 0; i < n; i++)
        if (y >= ent[i].y - DLG_LINE + 3 && y < ent[i].y + (ent[i].lines - 1) * DLG_LINE + 4) {
            game_ask(e->g, ent[i].id);
            return 1;
        }
    return 1;
}

/* --- Emotion and action icons ----------------------------------------- */

static void close_icons(explorer *e)
{
    int i;
    for (i = 0; i < e->icon_count; i++)
        if (e->icon_open[i]) {
            xf_video_close(&e->icon_anim[i].video);
            xf_mov_close(&e->icon_anim[i].mov);
        }
    e->icon_count = 0;
}

static void refresh_icons(explorer *e)
{
    u32 ids[MAX_ICONS];
    int n = game_icons(e->g, ids, MAX_ICONS), i;
    if (n == e->icon_count && memcmp(ids, e->icon_ids, (size_t)n * sizeof(u32)) == 0)
        return;
    close_icons(e);
    for (i = 0; i < n; i++) {
        game_icon ic;
        u32 media[4];
        vc_asset_ref a;
        char path[1024];
        view_anim *an = &e->icon_anim[i];
        e->icon_ids[i] = ids[i];
        e->icon_open[i] = 0;
        if (game_read_icon(e->g, ids[i], &ic) != 0 || vc_read_list(&e->g->db, ic.icon, media, 4) <= 0 ||
            vc_read_asset_ref(&e->g->db, media[0], &a) != 0 || a.entry_count == 0 ||
            resolve_media(e, a.entries[0].path, path, sizeof(path)) != 0 || xf_mov_open(&an->mov, path) != 0)
            continue;
        an->track = xf_mov_find(&an->mov, XF_TRACK_VIDEO);
        if (!an->track || xf_video_open(&an->video, &an->mov, an->track) != 0) {
            xf_mov_close(&an->mov);
            continue;
        }
        an->start_ms = 0;
        an->length_ms = XF_MAX((u32)((u64)an->mov.duration * 1000 / an->mov.timescale), 1);
        an->loop = 1;
        an->started = plat_ticks_ms();
        e->icon_open[i] = 1;
    }
    e->icon_count = n;
}

static int icon_x(explorer *e, int i)
{
    int total = e->icon_count * ICON_W + (e->icon_count - 1) * ICON_GAP;
    return (SCREEN_W - total) / 2 + i * (ICON_W + ICON_GAP);
}

static int icon_at(explorer *e, int x, int y)
{
    int i;
    if (game_iface_item_at(e->g, x, y))
        return -1; /* a layout covers the icons there */
    for (i = 0; i < e->icon_count; i++)
        if (x >= icon_x(e, i) && x < icon_x(e, i) + ICON_W && y >= ICON_Y && y < ICON_Y + ICON_H)
            return i;
    return -1;
}

static void draw_icons(explorer *e, int mx, int my)
{
    int i, y, hover = icon_at(e, mx, my);
    for (i = 0; i < e->icon_count; i++) {
        view_anim *an = &e->icon_anim[i];
        int x0 = icon_x(e, i), w, h;
        if (!e->icon_open[i])
            continue;
        update_anim(an, plat_ticks_ms());
        w = XF_MIN(an->video.width, ICON_W);
        h = XF_MIN(an->video.height, ICON_H);
        for (y = 0; y < h; y++)
            memcpy(&e->fb[(ICON_Y + y) * SCREEN_W + x0], an->video.frame + (size_t)y * an->video.width,
                   (size_t)w * 4);
    }
    /* Rolling over an icon shows what it does (Pref_bRolloverText). */
    if (hover >= 0 && e->have_font && game_pref(e->g, 982)) {
        game_icon ic;
        char name[128] = "", *dash;
        if (game_read_icon(e->g, e->icon_ids[hover], &ic) == 0)
            vc_read_name(&e->g->db, ic.name, name, sizeof(name));
        dash = strrchr(name, '-'); /* "Field Office N1A Cook - Funny" */
        if (dash && dash[1] == ' ')
            memmove(name, dash + 2, strlen(dash + 2) + 1);
        {
            int cx = icon_x(e, hover) + ICON_W / 2, w = text_px(e, name, 14);
            int x = XF_CLAMP(cx - w / 2, 4, SCREEN_W - 4 - w);
            draw_outlined(e, &e->font, 14, x, ICON_Y + ICON_H + 16, name, strlen(name), 0xFFFFFF);
        }
    }
}

/* --- Inventory ---------------------------------------------------------- */

static void free_inventory(explorer *e)
{
    int i;
    for (i = 0; i < e->inv_count; i++) {
        free(e->inv_img[i][0].pixels);
        free(e->inv_img[i][1].pixels);
    }
    e->inv_count = 0;
}

/* VCInventory and VCEvidenceIcon: name, icon, rollover icon, 0, cursor,
 * triggers. In a lab the collected evidence makes a second row above the
 * inventory; a click hands it to the technician. */
static void refresh_inventory(explorer *e)
{
    u32 ids[MAX_INV];
    int items = game_inventory(e->g, ids, MAX_INV), i;
    int n = items + game_evidence(e->g, ids + items, MAX_INV - items);
    if (n == e->inv_count && items == e->inv_items && memcmp(ids, e->inv_ids, (size_t)n * sizeof(u32)) == 0)
        return;
    free_inventory(e);
    for (i = 0; i < n; i++) {
        xf_neo_reader r;
        e->inv_ids[i] = ids[i];
        memset(e->inv_img[i], 0, sizeof(e->inv_img[i]));
        if (xf_neo_object(&e->g->db, ids[i], &r, NULL, NULL) == 0) {
            u32 name = xf_neo_u32(&r), icon = xf_neo_u32(&r), over = xf_neo_u32(&r);
            (void)name;
            load_list_image(e, icon, &e->inv_img[i][0]);
            load_list_image(e, over, &e->inv_img[i][1]);
        }
    }
    e->inv_count = n;
    e->inv_items = items;
}

static void inv_pos(const explorer *e, int i, int *x, int *y)
{
    int row = i >= e->inv_items, col = row ? i - e->inv_items : i;
    *x = 10 + col * (INV_W + INV_GAP);
    *y = row ? INV_Y - INV_H - INV_ROW_GAP : INV_Y;
}

/* The inventory shows when the pointer is low on the screen (higher
 * when a lab's evidence row is up). */
static int inv_reveal(const explorer *e)
{
    return e->inv_items < e->inv_count ? INV_Y - INV_H - INV_ROW_GAP : INV_REVEAL;
}

static int inv_at(explorer *e, int x, int y)
{
    int i;
    for (i = 0; i < e->inv_count; i++) {
        int x0, y0;
        inv_pos(e, i, &x0, &y0);
        if (x >= x0 && x < x0 + INV_W && y >= y0 && y < y0 + INV_H)
            return i;
    }
    return -1;
}

static void draw_inventory(explorer *e, int mx, int my)
{
    xf_surface s = screen_surface(e);
    int i, x, y, hover = inv_at(e, mx, my);
    for (i = 0; i < e->inv_count; i++) {
        /* The Artificial Intuition icon blinks while a hint is ready. */
        int glow = e->inv_ids[i] == GAME_AI_ICON && game_ai_glowing(e->g) && (plat_ticks_ms() / 500) % 2;
        const xf_image *img = &e->inv_img[i][(i == hover || glow) && e->inv_img[i][1].pixels ? 1 : 0];
        inv_pos(e, i, &x, &y);
        blit_keyed(e, img, x, y);
    }
    if (hover >= 0 && e->have_font && game_pref(e->g, 982)) {
        char name[96] = "";
        xf_neo_reader r;
        if (xf_neo_object(&e->g->db, e->inv_ids[hover], &r, NULL, NULL) == 0)
            vc_read_name(&e->g->db, xf_neo_u32(&r), name, sizeof(name));
        inv_pos(e, e->inv_items < e->inv_count ? e->inv_items : 0, &x, &y);
        xf_text_draw(&s, &e->font, 14, 10, y - 6, name, strlen(name), 0xFFFFFF);
    }
}

/* --- Cursors ------------------------------------------------------------ */

/* Reads one picture of a .pff bundle; x.pff stays open. */
static int load_pff_image(explorer *e, const char *path, u32 index, xf_image *img)
{
    xf_pff pff, *p = &pff;
    u32 size;
    u8 *data;
    int own = 0;
    memset(img, 0, sizeof(*img));
    if (strstr(path, "x.pff") || strstr(path, "X.PFF")) {
        use_pff(e, path);
        if (!e->pff_open)
            return -1;
        p = &e->pff;
    } else {
        if (xf_pff_open(&pff, path) != 0)
            return -1;
        own = 1;
    }
    data = xf_pff_read(p, index, &size);
    if (data && xf_pict_decode(data, size, img) != 0)
        memset(img, 0, sizeof(*img));
    free(data);
    if (own)
        xf_pff_close(&pff);
    return img->pixels ? 0 : -1;
}

/* VCCursor: hot point (two 16-bit values), name, media list. A .pic
 * cursor is one picture of x.pff; a .pff cursor is a small bundle whose
 * pictures from the start to the end index animate. */
static const cursor_art *get_cursor(explorer *e, u32 id)
{
    cursor_art *c;
    xf_neo_reader r;
    u32 media[4];
    vc_asset_ref a;
    int i, j;
    for (i = 0; i < e->cursor_count; i++)
        if (e->cursors[i].id == id)
            return e->cursors[i].count ? &e->cursors[i] : NULL;
    if (e->cursor_count >= MAX_CURSORS || vc_class(&e->g->db, id) != 0x3D ||
        xf_neo_object(&e->g->db, id, &r, NULL, NULL) != 0)
        return NULL;
    c = &e->cursors[e->cursor_count++];
    memset(c, 0, sizeof(*c));
    c->id = id;
    c->hot_x = (i16)xf_neo_u16(&r);
    c->hot_y = (i16)xf_neo_u16(&r);
    xf_neo_u32(&r); /* name */
    if (vc_read_list(&e->g->db, xf_neo_u32(&r), media, 4) <= 0 || vc_read_asset_ref(&e->g->db, media[0], &a) != 0)
        return NULL;
    for (j = 0; j < a.entry_count && !c->count; j++) {
        char path[1024];
        u32 k;
        if (resolve_media(e, a.entries[j].path, path, sizeof(path)) != 0)
            continue;
        for (k = a.entries[j].start; k <= XF_MAX(a.entries[j].start, a.entries[j].end) && c->count < MAX_CURSOR_FRAMES;
             k++)
            if (load_pff_image(e, path, k, &c->frames[c->count]) == 0)
                c->count++;
    }
    return c->count ? c : NULL;
}

static void free_cursors(explorer *e)
{
    int i, k;
    for (i = 0; i < e->cursor_count; i++)
        for (k = 0; k < e->cursors[i].count; k++)
            free(e->cursors[i].frames[k].pixels);
    e->cursor_count = 0;
}

/* The cursor of the item in hand: field 5 of its VCInventory. */
static u32 held_cursor(explorer *e)
{
    xf_neo_reader r;
    int i;
    if (!e->held_inv ||
        game_reg_value(e->g, "RegCurrInvVar", 0) == game_reg_value(e->g, "RegCurrInvVar", 1)) {
        e->held_inv = 0;
        return 0;
    }
    if (xf_neo_object(&e->g->db, e->held_inv, &r, NULL, NULL) != 0)
        return 0;
    for (i = 0; i < 4; i++)
        xf_neo_u32(&r);
    return xf_neo_u32(&r);
}

/* The default pointer, wherever no hotspot or item names one (the
 * dialogue box included): the window class cursor, resource 129 of the
 * exe (0x459230), a white cross with a black outline, hot spot (7, 7).
 * The blue X (VCCursor 25541) is never shown: RegXCursorAss
 * (0x4BBAF0) ends in an empty function in the shipped build. */
static const cursor_art *cross_cursor(explorer *e)
{
    static const char *const rows[7] = {
        "  o#o  ",
        "  o#o  ",
        "ooo#ooo",
        "###o###",
        "ooo#ooo",
        "  o#o  ",
        "  o#o  ",
    };
    int x, y;
    if (!e->cross.count) {
        for (x = 0; x < CROSS_SIZE * CROSS_SIZE; x++)
            e->cross_px[x] = KEY_BLUE;
        for (y = 0; y < 7; y++)
            for (x = 0; x < 7; x++)
                if (rows[y][x] != ' ')
                    e->cross_px[(y + 4) * CROSS_SIZE + x + 4] = rows[y][x] == '#' ? 0xFFFFFF : 0x000000;
        e->cross.frames[0].pixels = e->cross_px;
        e->cross.frames[0].width = CROSS_SIZE;
        e->cross.frames[0].height = CROSS_SIZE;
        e->cross.hot_x = 7;
        e->cross.hot_y = 7;
        e->cross.count = 1;
    }
    return &e->cross;
}

/* The cursor for the pointer at (mx, my): a hotspot's or the item in
 * hand's, or the cross. */
static const cursor_art *current_cursor(explorer *e, int mx, int my)
{
    u32 id = held_cursor(e);
    const cursor_art *c;
    if (!id && !e->playing && e->in_menu != 1 && !e->g->talking)
        id = game_cursor_at(e->g, mx, my);
    c = id ? get_cursor(e, id) : NULL;
    if (!c)
        c = cross_cursor(e);
    plat_show_cursor(0);
    return c;
}

static void draw_cursor(explorer *e, int mx, int my)
{
    const cursor_art *c = current_cursor(e, mx, my);
    if (c)
        blit_keyed(e, &c->frames[(plat_ticks_ms() / 100) % (u32)c->count], mx - c->hot_x, my - c->hot_y);
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

/* 0x483A30: the location card is a text object in Courier New, 12 point,
 * typed one character every 50 ms in the lower left corner of the clip,
 * 15 pixels from its edges. */
static void draw_card(explorer *e, int clip_x, int clip_y, int clip_w, int clip_h)
{
    const xf_font *f = e->have_card_font ? &e->card_font : &e->font;
    const char *text = e->g->card;
    size_t shown = XF_MIN((size_t)((plat_ticks_ms() - e->card_start) / 50), strlen(text));
    int lines = 1, line_h = CARD_PX + 2, x = clip_x + 15, y, i;
    size_t pos = 0;
    (void)clip_w;
    if (!e->have_card_font && !e->have_font)
        return;
    for (i = 0; text[i]; i++)
        lines += text[i] == '\n';
    y = clip_y + clip_h - 15 - (lines - 1) * line_h - CARD_PX / 4;
    while (pos < shown) {
        const char *nl = strchr(text + pos, '\n');
        size_t end = nl ? (size_t)(nl - text) : strlen(text);
        size_t len = XF_MIN(end, shown) - pos;
        draw_outlined(e, f, CARD_PX, x, y, text + pos, len, 0xFFFFFF);
        pos = end + 1;
        y += line_h;
    }
}

/* SetFLightProps mode 2: the view is dark (0x49EA10 darkens it by alpha
 * 0xEE or 0xC8, from the beam setting). The flashlight in hand lights a
 * circle around the pointer; night vision shows the whole view in green.
 * The beam size and the green are estimates. */
#define BEAM_RADIUS 70

static void draw_darkness(explorer *e, int mx, int my)
{
    game *g = e->g;
    i32 hand = game_reg_value(g, "RegCurrInvVar", 0);
    u32 fl = vc_find_var(&g->interp, "Inventory_iFlashlight"), nv = vc_find_var(&g->interp, "Inventory_iNightVision");
    int light = fl && hand == vc_get_var(&g->interp, fl), green = nv && hand == vc_get_var(&g->interp, nv);
    int x, y;
    if (g->flight_mode != 2)
        return;
    for (y = PICTURE_Y; y < PICTURE_Y + PICTURE_H; y++)
        for (x = PICTURE_X; x < PICTURE_X + PICTURE_W; x++) {
            u32 c = e->fb[y * SCREEN_W + x];
            int r = (c >> 16) & 0xFF, gg = (c >> 8) & 0xFF, b = c & 0xFF, k = 17; /* 255 - 0xEE */
            if (green) {
                int l = (r * 77 + gg * 150 + b * 29) >> 8;
                e->fb[y * SCREEN_W + x] = (u32)(l / 4) << 16 | (u32)XF_MIN(255, l * 3 / 2) << 8 | (u32)(l / 4);
                continue;
            }
            if (light) {
                int dx = x - mx, dy = y - my, d2 = dx * dx + dy * dy;
                if (d2 < BEAM_RADIUS * BEAM_RADIUS) {
                    int edge = BEAM_RADIUS * BEAM_RADIUS - d2; /* soft rim */
                    k = 17 + XF_MIN(238, edge * 238 / (BEAM_RADIUS * 20));
                }
            }
            e->fb[y * SCREEN_W + x] = (u32)(r * k / 255) << 16 | (u32)(gg * k / 255) << 8 | (u32)(b * k / 255);
        }
}

/* FadeToBlack: the picture goes black over 2 seconds and stays black
 * until the view changes. */
static void draw_fade(explorer *e)
{
    u32 now = plat_ticks_ms(), k;
    int x, y;
    if (e->fade_serial != e->g->fade_serial) {
        e->fade_serial = e->g->fade_serial;
        e->fade_start = now;
        e->fade_view = e->g->view;
    }
    if (!e->fade_start)
        return;
    if (e->fade_view != e->g->view && now - e->fade_start > 2000) {
        e->fade_start = 0;
        return;
    }
    k = now - e->fade_start >= 2000 ? 0 : 255 - (now - e->fade_start) * 255 / 2000;
    for (y = PICTURE_Y; y < PICTURE_Y + PICTURE_H; y++)
        for (x = PICTURE_X; x < PICTURE_X + PICTURE_W; x++) {
            u32 c = e->fb[y * SCREEN_W + x];
            e->fb[y * SCREEN_W + x] = (((c >> 16) & 0xFF) * k / 255) << 16 | (((c >> 8) & 0xFF) * k / 255) << 8 |
                                      ((c & 0xFF) * k / 255);
        }
}

static void draw_overlay(explorer *e);
static int open_asset_player(explorer *e, u32 asset, xf_player *p);

/* The wide layout: the scene strip fills the output and the interface
 * pieces go around it (see docs/engine/screen.md). Screens drawn for
 * 640x480 as a whole keep it: layouts (the computer, the PDA...), the
 * menu, message slots and clips of other sizes, such as the logos. */
static int want_wide(const explorer *e)
{
    const game *g = e->g;
    if (!e->wide_pref || e->in_menu || g->iface_count || g->slot_mode)
        return 0;
    if (e->playing && e->have_clip && xf_player_frame((xf_player *)&e->player) &&
        (e->player.video.width != PICTURE_W || e->player.video.height != PICTURE_H))
        return 0;
    return e->playing || g->view;
}

/* From here on, drawing goes to the interface layer. */
static void begin_ui(explorer *e)
{
    int i;
    if (!e->layout.wide)
        return;
    e->fb = e->ui_fb;
    e->piece_used = 0;
    for (i = 0; i < SCREEN_W * SCREEN_H; i++)
        e->ui_fb[i] = SCR_CLEAR;
}

/* Cuts a piece out of the interface layer, so that pieces drawn over
 * each other in 640x480 can go apart on the output. */
static void ui_piece(explorer *e, int x, int y, int w, int h, int anchor)
{
    scr_rect r;
    u32 *px;
    int row;
    if (!e->layout.wide)
        return;
    r.x = XF_MAX(x, 0);
    r.y = XF_MAX(y, 0);
    r.w = XF_MIN(x + w, SCREEN_W) - r.x;
    r.h = XF_MIN(y + h, SCREEN_H) - r.y;
    if (r.w <= 0 || r.h <= 0 || e->piece_used + (size_t)r.w * r.h > XF_COUNT(e->piece_px))
        return;
    px = e->piece_px + e->piece_used;
    e->piece_used += (size_t)r.w * r.h;
    for (row = 0; row < r.h; row++) {
        u32 *src = &e->ui_fb[(r.y + row) * SCREEN_W + r.x];
        int k;
        memcpy(px + (size_t)row * r.w, src, (size_t)r.w * 4);
        for (k = 0; k < r.w; k++)
            src[k] = SCR_CLEAR;
    }
    scr_add_piece(&e->layout, r, anchor, px);
}

static int menu_icon_shown(const explorer *e, int mx, int my)
{
    const game *g = e->g;
    return e->in_menu == 0 && g->view && !g->iface_count && !g->slot_mode && !g->ask && my < PICTURE_Y &&
           mx >= MENU_ICON_REVEAL_X;
}

static int menu_icon_at(const explorer *e, int x, int y)
{
    return menu_icon_shown(e, x, y) && x >= MENU_ICON_X - 22 && x < MENU_ICON_X + 22 && y < MENU_ICON_Y + 22;
}

static void draw_menu_icon(explorer *e, int mx, int my)
{
    const cursor_art *c;
    const xf_image *f;
    if (!menu_icon_shown(e, mx, my) || !(c = get_cursor(e, MENU_ICON)))
        return;
    f = &c->frames[(plat_ticks_ms() / 100) % (u32)c->count];
    blit_keyed(e, f, MENU_ICON_X - f->width / 2, MENU_ICON_Y - f->height / 2);
    ui_piece(e, MENU_ICON_X - 40, 0, SCREEN_W - (MENU_ICON_X - 40), MENU_ICON_Y + 30, SCR_ANCHOR_TOP_RIGHT);
}

static void draw(explorer *e, int mx, int my)
{
    static const u32 colors[] = { 0x40FF40, 0xFFE040, 0x40E0FF };
    xf_surface s;
    const game *g = e->g;
    int i, y, wide = e->layout.wide;

    e->fb = e->scene_fb;
    if (g->lbox_tinted) {
        /* SetLBoxTint: the letterbox takes the scene's tint */
        u32 c = (u32)g->lbox_tint[0] << 16 | (u32)g->lbox_tint[1] << 8 | g->lbox_tint[2];
        for (i = 0; i < SCREEN_W * SCREEN_H; i++)
            e->fb[i] = c;
    } else {
        memset(e->fb, 0, sizeof(e->scene_fb));
    }
    refresh_icons(e);
    if (e->playing) {
        const u32 *f = e->have_clip ? xf_player_frame(&e->player) : NULL;
        const char *sub = e->have_clip && game_pref(e->g, 2573) ? xf_player_subtitle(&e->player) : NULL; /* Pref_bClosedCaptioning */
        int cx = PICTURE_X, cy = PICTURE_Y, cw = PICTURE_W, ch = PICTURE_H;
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
            cx = x0;
            cy = y0;
            cw = w;
            ch = h;
        }
        if (e->card_active && !e->g->text_off)
            draw_card(e, cx, cy, cw, ch);
        begin_ui(e);
        s = screen_surface(e);
        scr_reserve_bottom(&e->layout, inv_reveal(e)); /* icons stay put when the clip ends */
        if (sub && e->have_font) {
            xf_text_draw_wrapped(&s, &e->font, 20, SCREEN_W / 2, PICTURE_Y + PICTURE_H + 30, SCREEN_W - 40,
                                 sub, 0xFFFFFF, 0x000000);
            ui_piece(e, 0, PICTURE_Y + PICTURE_H, SCREEN_W, SCREEN_H - PICTURE_Y - PICTURE_H, SCR_ANCHOR_BELOW);
        }
        draw_icons(e, mx, my);
        if (e->icon_count)
            ui_piece(e, 0, ICON_Y, SCREEN_W, ICON_H, SCR_ANCHOR_BELOW);
        draw_menu_icon(e, mx, my);
        if (!wide)
            draw_cursor(e, mx, my);
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
    draw_darkness(e, mx, my);
    draw_fade(e);
    if (e->debug)
        /* Ctrl+Shift+D: hotspot outlines and the view's media name. */
        for (i = 0; i < g->spot_count; i++) {
            const vc_region *r = &g->spots[i].region;
            int inside = mx >= r->left && mx < r->right && my >= r->top && my < r->bottom;
            frame_rect(e, r, inside ? 0xFFFFFF : colors[g->spots[i].kind]);
        }
    /* PlayAssetAt: a small clip, with the scene when it is on the strip */
    if (!wide || (g->overlay_x >= PICTURE_X && g->overlay_y >= PICTURE_Y &&
                  g->overlay_x + e->overlay.video.width <= PICTURE_X + PICTURE_W &&
                  g->overlay_y + e->overlay.video.height <= PICTURE_Y + PICTURE_H))
        draw_overlay(e);
    else if (e->overlay_open) {
        begin_ui(e);
        draw_overlay(e);
        ui_piece(e, g->overlay_x, g->overlay_y, e->overlay.video.width, e->overlay.video.height, SCR_ANCHOR_CENTER);
    }
    if (e->fb != e->ui_fb)
        begin_ui(e);
    s = screen_surface(e);
    scr_reserve_bottom(&e->layout, inv_reveal(e));
    if (e->debug && e->have_font) {
        xf_text_draw(&s, &e->font, 14, 8, 20, e->label, strlen(e->label), 0xC0C0C0);
        ui_piece(e, 0, 0, SCREEN_W, 30, SCR_ANCHOR_TOP_LEFT);
    }
    draw_icons(e, mx, my);
    if (e->icon_count)
        ui_piece(e, 0, ICON_Y, SCREEN_W, ICON_H, SCR_ANCHOR_BELOW);
    if (g->talking) {
        int x0 = dialog_x(e);
        draw_dialog(e, mx, my);
        ui_piece(e, x0, DLG_Y - 6, DLG_W, 130, SCR_ANCHOR_UNDER);
    }
    refresh_inventory(e);
    if ((my >= inv_reveal(e) || game_pref(e->g, 983) == 'V') && !g->iface_count) { /* Pref_cInventory */
        /* the rows, with room above for the item's name */
        int top = (e->inv_items < e->inv_count ? INV_Y - INV_H - INV_ROW_GAP : INV_Y) - 24;
        int cols = XF_MAX(e->inv_items, e->inv_count - e->inv_items);
        draw_inventory(e, mx, my);
        ui_piece(e, 0, top, XF_MIN(SCREEN_W, 10 + cols * (INV_W + INV_GAP) + 60), SCREEN_H - top,
                 SCR_ANCHOR_BOTTOM_LEFT);
    }
    draw_ifaces(e);
    draw_menu_icon(e, mx, my);
    draw_message(e, mx, my);
    if (g->ask)
        ui_piece(e, (SCREEN_W - 360) / 2, (SCREEN_H - 150) / 2, 360, 150, SCR_ANCHOR_CENTER);
    if (!wide)
        draw_cursor(e, mx, my);
}

/* A message box for game_message: the text, then OK, or Yes and No.
 * The original uses the system's message box; ours is drawn like the
 * computer screens: white, with a black frame. */
#define MSG_W 360
#define MSG_H 150
#define MSG_X ((SCREEN_W - MSG_W) / 2)
#define MSG_Y ((SCREEN_H - MSG_H) / 2)
#define MSG_BTN_W 90
#define MSG_BTN_H 26

static vc_region msg_button(const explorer *e, int which)
{
    vc_region r;
    memset(&r, 0, sizeof(r));
    int two = e->g->ask_action != GAME_ASK_OK;
    int x = two ? MSG_X + MSG_W / 2 + (which ? 10 : -10 - MSG_BTN_W) : MSG_X + (MSG_W - MSG_BTN_W) / 2;
    r.left = (i16)x;
    r.top = (i16)(MSG_Y + MSG_H - MSG_BTN_H - 16);
    r.right = (i16)(x + MSG_BTN_W);
    r.bottom = (i16)(r.top + MSG_BTN_H);
    return r;
}

/* 0: the first button (Yes or OK), 1: No, -1: none. */
static int msg_button_at(const explorer *e, int x, int y)
{
    int i;
    for (i = 0; i < (e->g->ask_action != GAME_ASK_OK ? 2 : 1); i++) {
        vc_region r = msg_button(e, i);
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom)
            return i;
    }
    return -1;
}

static void fill_box(explorer *e, int x0, int y0, int x1, int y1, u32 c)
{
    int x, y;
    for (y = XF_MAX(y0, 0); y < XF_MIN(y1, SCREEN_H); y++)
        for (x = XF_MAX(x0, 0); x < XF_MIN(x1, SCREEN_W); x++)
            e->fb[y * SCREEN_W + x] = c;
}

static void draw_message(explorer *e, int mx, int my)
{
    static const char *labels[2][2] = { { "OK", "" }, { "Yes", "No" } };
    xf_surface s = screen_surface(e);
    vc_region frame;
    const xf_font *f = e->have_card_font ? &e->card_font : &e->font;
    char text[300];
    int two = e->g->ask_action != GAME_ASK_OK, i;
    if (!e->g->ask || !e->have_font)
        return;
    memset(&frame, 0, sizeof(frame));
    frame.left = MSG_X;
    frame.top = MSG_Y;
    frame.right = MSG_X + MSG_W;
    frame.bottom = MSG_Y + MSG_H;
    /* the engine strings end without their question mark */
    snprintf(text, sizeof(text), "%s%s", game_message_text(e->g), two ? "?" : "");
    fill_box(e, MSG_X, MSG_Y, MSG_X + MSG_W, MSG_Y + MSG_H, 0xFFFFFF);
    frame_rect(e, &frame, 0x000000);
    xf_text_draw_wrapped(&s, f, 16, SCREEN_W / 2, MSG_Y + 36, MSG_W - 40, text, 0x000000, 0xFFFFFF);
    for (i = 0; i < (two ? 2 : 1); i++) {
        vc_region r = msg_button(e, i);
        int hot = msg_button_at(e, mx, my) == i;
        const char *l = labels[two][i];
        fill_box(e, r.left, r.top, r.right, r.bottom, hot ? 0x000000 : 0xFFFFFF);
        frame_rect(e, &r, 0x000000);
        xf_text_draw(&s, f, 16, (r.left + r.right - xf_text_width(f, 16, l, strlen(l))) / 2, r.bottom - 8, l,
                     strlen(l), hot ? 0xFFFFFF : 0x000000);
    }
}

/* The Save screen's text cursor (item 82561) follows the typed name. */
static void place_slot_cursor(explorer *e)
{
    game_iface_item *c = (game_iface_item *)game_find_item(e->g, 82561);
    const game_item_label *l;
    static const u32 names[3] = { 82556, 82557, 82558 };
    int w = 0;
    if (!c || e->g->slot_mode != 'S')
        return;
    l = game_item_label_of(e->g, names[e->g->slot]);
    if (l && e->have_card_font)
        w = xf_text_width(&e->card_font, (float)l->size * 96.0f / 72.0f, l->text, strlen(l->text));
    c->item.region.left = (i16)(200 + w + 2);
    c->item.region.right = (i16)(c->item.region.left + 3);
    c->item.region.top = (i16)(164 + 58 * e->g->slot);
    c->item.region.bottom = (i16)(c->item.region.top + 25);
}

/* PlayAssetAt: the overlay clip starts when the game names a new one and
 * ends (running its end triggers) when it has played. */
static void update_overlay(explorer *e)
{
    u32 now = plat_ticks_ms();
    if (e->overlay_serial != e->g->overlay_serial) {
        e->overlay_serial = e->g->overlay_serial;
        if (e->overlay_open)
            xf_player_close(&e->overlay);
        e->overlay_open = e->g->overlay && open_asset_player(e, e->g->overlay, &e->overlay) == 0;
        if (e->g->overlay && !e->overlay_open)
            game_overlay_end(e->g); /* no file: the scene goes on */
    }
    if (!e->overlay_open)
        return;
    if (!e->g->overlay) { /* StopPlayAssets */
        xf_player_close(&e->overlay);
        e->overlay_open = 0;
    } else if (!xf_player_update(&e->overlay, now)) {
        xf_player_close(&e->overlay);
        e->overlay_open = 0;
        game_overlay_end(e->g);
    }
}

static void draw_overlay(explorer *e)
{
    const u32 *f;
    int w, h, x0, y0, y;
    if (!e->overlay_open || !(f = xf_player_frame(&e->overlay)))
        return;
    w = e->overlay.video.width;
    h = e->overlay.video.height;
    x0 = e->g->overlay_x;
    y0 = e->g->overlay_y;
    for (y = 0; y < h; y++) {
        int sy = y0 + y, x;
        if (sy < 0 || sy >= SCREEN_H)
            continue;
        for (x = 0; x < w; x++)
            if (x0 + x >= 0 && x0 + x < SCREEN_W)
                e->fb[sy * SCREEN_W + x0 + x] = f[y * w + x];
    }
}

/* 0x471820 play_intro_sequence: Fox logo, HyperBole logo, teaser and
 * title, each skippable with a click. The DVD build plays its own DVD
 * titles instead of the last two; we use the QuickTime files. */
static void queue_intro(explorer *e)
{
    static const u32 intro[] = { 56003, 56002, 19668, 56001 };
    size_t i;
    if (getenv("XF_SKIP_INTRO") || game_pref(e->g, 994)) /* Pref_bSkipIntro */
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

/* A game is going on: the menu shows Return and holds the game. */
static int game_running(const explorer *e) { return e->g->view && !e->g->game_over; }

/* Holds or resumes the game's clip, sounds and location card. */
static void pause_game_media(explorer *e, int paused)
{
    u32 now = plat_ticks_ms();
    if (paused == e->media_paused)
        return;
    if (e->have_clip)
        xf_player_pause(&e->player, paused, now);
    if (e->sound_open)
        xf_player_pause(&e->sound, paused, now);
    if (e->overlay_open)
        xf_player_pause(&e->overlay, paused, now);
    if (paused)
        e->media_pause_from = now;
    else if (e->card_until) {
        e->card_start += now - e->media_pause_from;
        e->card_until += now - e->media_pause_from;
    }
    e->media_paused = paused;
}

/* The game's media stop for good: a new game or a load. */
static void drop_game_media(explorer *e)
{
    pause_game_media(e, 0);
    stop_media(e);
    stop_sound(e);
    e->from_menu = 0;
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
    if (game_running(e))
        pause_game_media(e, 1); /* the game waits behind the menu */
    else
        drop_game_media(e);
    e->from_menu = 0;
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

/* Escape in the menu during a game: back to it, with its sound. */
static void menu_leave(explorer *e)
{
    menu_close(e);
    pause_game_media(e, 0);
    e->g->iface_serial++;
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
    plat_show_cursor(1);
    int i, y;
    memset(e->fb, 0, sizeof(e->scene_fb));
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
    /* MainMenu_load_media: during a game, return.pic (72895) covers the
     * Previous row, at (472, 215) (0x6C3F50). */
    if (game_running(e) && e->right_frame >= RIGHT_INTRO_END) {
        if (!e->return_img.pixels)
            load_asset_image(e, MENU_RETURN, &e->return_img);
        if (e->return_img.pixels)
            for (y = 0; y < e->return_img.height && 215 + y < SCREEN_H; y++)
                memcpy(&e->fb[(215 + y) * SCREEN_W + 472], e->return_img.pixels + y * e->return_img.width,
                       (size_t)XF_MIN(e->return_img.width, SCREEN_W - 472) * 4);
    }
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

/* The software compositor's limit: larger windows get a smaller frame,
 * which the platform scales up. */
#define OUT_MAX_PIXELS (1920 * 1200)

/* Draws the frame and shows it: the 640x480 screen as it is, or the
 * wide layout at the output's size. */
static void present_frame(explorer *e)
{
    int ow, oh;
    plat_output_size(&ow, &oh);
    while ((long)ow * oh > OUT_MAX_PIXELS) {
        ow = ow * 3 / 4;
        oh = oh * 3 / 4;
    }
    scr_layout_begin(&e->layout, ow, oh, want_wide(e));
    if (e->in_menu == 1)
        menu_draw(e);
    else
        draw(e, e->mx, e->my);
    e->fb = e->scene_fb;
    if (e->layout.wide && (size_t)ow * oh > e->out_cap) {
        u32 *o = realloc(e->out, (size_t)ow * oh * 4);
        if (o) {
            e->out = o;
            e->out_cap = (size_t)ow * oh;
        } else {
            scr_layout_begin(&e->layout, ow, oh, 0); /* no memory: the plain screen */
            draw(e, e->mx, e->my);
            e->fb = e->scene_fb;
        }
    }
    if (!e->layout.wide) {
        scr_layout_begin(&e->layout, SCREEN_W, SCREEN_H, 0); /* the platform scales it */
        e->shown = e->scene_fb;
        e->shown_w = SCREEN_W;
        e->shown_h = SCREEN_H;
    } else {
        const game *g = e->g;
        const cursor_art *c = current_cursor(e, e->mx, e->my);
        u32 bg = g->lbox_tinted ? (u32)g->lbox_tint[0] << 16 | (u32)g->lbox_tint[1] << 8 | g->lbox_tint[2] : 0;
        scr_compose(&e->layout, e->out, e->scene_fb, e->ui_fb, bg);
        if (c) {
            const xf_image *f = &c->frames[(plat_ticks_ms() / 100) % (u32)c->count];
            scr_draw_scaled(&e->layout, e->out, f->pixels, f->width, f->height, c->hot_x, c->hot_y, e->out_mx,
                            e->out_my, KEY_BLUE);
        }
        e->shown = e->out;
        e->shown_w = ow;
        e->shown_h = oh;
    }
    plat_present(e->shown, e->shown_w, e->shown_h, e->shown_w);
}

/* One input event: from the platform, or from a script. Mouse positions
 * are in the pixels of the frame shown last. */
static void handle_event(explorer *e, plat_event ev)
{
    if (ev.type == PLAT_EV_MOUSE_MOVE || ev.type == PLAT_EV_MOUSE_DOWN || ev.type == PLAT_EV_MOUSE_UP) {
        /* Events come in the pixels of the frame shown last. */
        e->out_mx = ev.x;
        e->out_my = ev.y;
        scr_to_screen(&e->layout, ev.x, ev.y, &ev.x, &ev.y);
    }
    if (ev.type == PLAT_EV_QUIT)
        e->running = 0;
    else if (e->g->ask && ev.type == PLAT_EV_KEY_DOWN && (ev.key == PLAT_KEY_ESCAPE || ev.key == '\r'))
        game_answer(e->g, ev.key == '\r'); /* Return: Yes or OK; Escape: No */
    else if (e->g->ask && ev.type == PLAT_EV_MOUSE_DOWN && ev.button == PLAT_MOUSE_LEFT) {
        int b = msg_button_at(e, ev.x, ev.y);
        if (b >= 0)
            game_answer(e->g, b == 0);
    } else if (e->g->ask && ev.type == PLAT_EV_MOUSE_MOVE) {
        e->mx = ev.x;
        e->my = ev.y;
    } else if (e->g->ask)
        ; /* the message box takes every input */
    else if (ev.type == PLAT_EV_KEY_DOWN && ev.key == PLAT_KEY_ESCAPE && e->g->talking)
        game_end_talk(e->g); /* Escape closes the dialogue box first */
    else if (ev.type == PLAT_EV_KEY_DOWN && ev.key == PLAT_KEY_ESCAPE && e->g->slot_mode)
        game_close_slots(e->g);
    else if (ev.type == PLAT_EV_KEY_DOWN && ev.key == PLAT_KEY_ESCAPE && e->in_menu == 1) {
        if (e->g->view && !e->g->game_over)
            menu_leave(e); /* back to the game */
        else
            e->running = 0;
    } else if (ev.type == PLAT_EV_KEY_DOWN && ev.key == PLAT_KEY_ESCAPE && e->in_menu == 0 && e->g->view)
        menu_enter(e); /* the main menu over the game, a clip included: it waits */
    else if (ev.type == PLAT_EV_KEY_DOWN && ev.key == PLAT_KEY_ESCAPE)
        ; /* a clip is playing: a click skips it */
    else if (ev.type == PLAT_EV_KEY_DOWN && ev.key == 'd' &&
             (ev.mods & (PLAT_MOD_CTRL | PLAT_MOD_SHIFT)) == (PLAT_MOD_CTRL | PLAT_MOD_SHIFT)) {
        e->debug = !e->debug;
        xf_info("debug overlay %s", e->debug ? "on" : "off");
    } else if (ev.type == PLAT_EV_KEY_DOWN && ev.key == 'w' &&
               (ev.mods & (PLAT_MOD_CTRL | PLAT_MOD_SHIFT)) == (PLAT_MOD_CTRL | PLAT_MOD_SHIFT)) {
        /* Ctrl+Shift+W: the wide layout on or off */
        e->wide_pref = !e->wide_pref;
        game_set_screen_wide(e->g, e->wide_pref);
        xf_info("screen: %s", e->wide_pref ? "wide" : "original");
    }
    else if (ev.type == PLAT_EV_KEY_DOWN && !e->playing && e->g->iface_count && ev.key &&
             (ev.key < 0x100) && !(ev.mods & (PLAT_MOD_CTRL | PLAT_MOD_CMD))) {
        /* Typing into a layout's text field (login, ING search). */
        int key = ev.key;
        if ((ev.mods & PLAT_MOD_SHIFT) && key >= 'a' && key <= 'z')
            key -= 'a' - 'A';
        game_key(e->g, key);
    }
    else if (e->in_menu == 1 && ev.type == PLAT_EV_MOUSE_MOVE)
        menu_set_hover(e, menu_item_at(ev.x, ev.y));
    else if (e->in_menu == 1 && ev.type == PLAT_EV_MOUSE_DOWN && ev.button == PLAT_MOUSE_LEFT) {
        int item = menu_item_at(ev.x, ev.y);
        if (item >= 0)
            xf_info("menu: %s", g_menu_items[item].name);
        if (item == 0) { /* New */
            drop_game_media(e);
            menu_close(e);
            if (e->g->view)
                game_reset(e->g); /* a game was running: start clean */
            game_new(e->g);
        } else if (item == 1) { /* Load */
            menu_close(e);
            e->from_menu = game_running(e);
            game_open_slots(e->g, 0);
        } else if (item == 2 && e->g->view && !e->g->game_over) { /* Save */
            menu_close(e);
            e->from_menu = 1;
            game_open_slots(e->g, 1);
        } else if (item == 3 && game_running(e)) { /* Return: back to the game */
            menu_leave(e);
        } else if (item == 3) { /* Previous: the last game saved or loaded */
            drop_game_media(e);
            menu_close(e);
            if (game_load_previous(e->g) != 0)
                menu_enter(e);
        } else if (item == 4 || item == 5) { /* Options, Help: HDB layouts */
            menu_close(e);
            e->from_menu = game_running(e);
            game_open_layout(e->g, item == 4 ? 49738 : 50460);
        } else if (item == 6) { /* Quit */
            e->running = 0;
        }
    } else if (ev.type == PLAT_EV_MOUSE_DOWN && ev.button == PLAT_MOUSE_LEFT && menu_icon_at(e, ev.x, ev.y)) {
        menu_enter(e); /* the menu icon, a clip included: it waits */
    } else if (ev.type == PLAT_EV_MOUSE_MOVE) {
        e->mx = ev.x;
        e->my = ev.y;
        if (!e->playing) {
            int reveal = game_pref(e->g, 983) == 'V' ? 0 : inv_reveal(e);
            int inv = ev.y >= reveal && !e->g->iface_count ? inv_at(e, ev.x, ev.y) : -1;
            game_hover(e->g, e->mx, e->my);
            game_hover_inventory(e->g, inv >= 0 ? e->inv_ids[inv] : 0, inv >= e->inv_items);
        }
    } else if (e->playing && ev.type == PLAT_EV_MOUSE_DOWN && icon_at(e, ev.x, ev.y) >= 0) {
        /* An emotion icon answers at once: the clip ends first. */
        u32 icon = e->icon_ids[icon_at(e, ev.x, ev.y)];
        finish_media(e);
        game_click_icon(e->g, icon);
    } else if (e->playing && e->have_clip && ev.type == PLAT_EV_MOUSE_DOWN && ev.button == PLAT_MOUSE_LEFT &&
               clip_shot(e, ev.x, ev.y)) {
        stop_media(e); /* a shot: the clip ends without its end triggers */
    } else if (e->playing && (ev.type == PLAT_EV_MOUSE_DOWN || ev.type == PLAT_EV_KEY_DOWN)) {
        /* Pref_bSkipClips: "Right/Command + Click to Skip Video Clip".
         * The opening titles skip with any click or key. The location
         * card cannot be skipped. */
        int skip = e->in_menu == -1 ||
                   (game_pref(e->g, 995) && ev.type == PLAT_EV_MOUSE_DOWN &&
                    (ev.button == PLAT_MOUSE_RIGHT || (ev.mods & (PLAT_MOD_CMD | PLAT_MOD_CTRL))));
        if (skip && !e->card_active)
            finish_media(e);
    } else if (ev.type == PLAT_EV_MOUSE_DOWN && ev.button == PLAT_MOUSE_LEFT && icon_at(e, ev.x, ev.y) >= 0) {
        game_click_icon(e->g, e->icon_ids[icon_at(e, ev.x, ev.y)]);
    } else if (ev.type == PLAT_EV_MOUSE_DOWN && ev.button == PLAT_MOUSE_LEFT) {
        /* Layout items first (a close-up covers everything), then
         * the dialogue box, the inventory, then the view. */
        int reveal = game_pref(e->g, 983) == 'V' ? 0 : inv_reveal(e);
        int inv = ev.y >= reveal && !e->g->iface_count ? inv_at(e, ev.x, ev.y) : -1;
        if (game_iface_item_at(e->g, ev.x, ev.y))
            game_click(e->g, ev.x, ev.y);
        else if (e->g->talking && click_dialog(e, ev.x, ev.y))
            ;
        else if (inv >= e->inv_items) {
            game_click_evidence(e->g, e->inv_ids[inv]);
        } else if (inv >= 0) {
            game_click_inventory(e->g, e->inv_ids[inv]);
            if (e->inv_ids[inv] != GAME_AI_ICON)
                e->held_inv = e->inv_ids[inv];
        }
        else if (shoot_at(e, ev.x, ev.y))
            ;
        else {
            u32 was = e->g->talking;
            if (!game_click(e->g, ev.x, ev.y))
                game_end_talk(e->g); /* a click elsewhere closes the box */
            if (e->g->talking != was) {
                e->talk_scroll = 0;
                e->talk_tab = 0;
            }
        }
    } else if ((ev.type == PLAT_EV_MOUSE_DOWN && ev.button == PLAT_MOUSE_RIGHT) ||
               (ev.type == PLAT_EV_KEY_DOWN && ev.key == 8)) {
        /* Right click: put the item in hand away, stop talking, or go back. */
        if (game_reg_value(e->g, "RegCurrInvVar", 0) != game_reg_value(e->g, "RegCurrInvVar", 1))
            game_hold(e->g, NULL);
        else if (e->g->talking)
            game_end_talk(e->g);
        else
            game_back(e->g);
    }
}

/* One frame: input, clips and sound, drawing. */
static void run_frame(explorer *e)
{
    plat_event ev;
    /* The game clock stops in the menu, on the Save and Load screens and
     * while a message box waits. */
    int paused;
    /* The game holds while the menu is up, and while Options, Help or the
     * Save and Load screens opened from it are. */
    if (e->from_menu && e->in_menu == 0 && !e->g->iface_count && !e->g->slot_mode)
        e->from_menu = 0;
    pause_game_media(e, e->in_menu == 1 || e->from_menu);
    paused = e->in_menu == 1 || e->g->slot_mode || e->g->ask || e->media_paused;
    if (paused && !e->pause_from)
        e->pause_from = plat_ticks_ms() | 1;
    else if (!paused && e->pause_from) {
        e->paused_ms += plat_ticks_ms() - e->pause_from;
        e->pause_from = 0;
    }
    if (!paused)
        game_tick(e->g, plat_ticks_ms() - e->paused_ms);
    while (plat_poll(&ev))
        handle_event(e, ev);
    if (!e->media_paused) {
        if ((e->have_clip && !xf_player_update(&e->player, plat_ticks_ms())) ||
            (e->card_until && (i32)(plat_ticks_ms() - e->card_until) >= 0))
            finish_media(e);
        if (e->clip_stop_serial != e->g->clip_stop_serial) {
            /* a media action with selector 3 stopped the clip: no end triggers */
            e->clip_stop_serial = e->g->clip_stop_serial;
            if (e->playing)
                stop_media(e);
        }
        start_next_media(e);
        update_sound(e);
        update_overlay(e);
    }
    if (e->in_menu == -1 && !e->playing && !game_media_pending(e->g))
        menu_enter(e);
    switch (e->g->ui_request) {
    case GAME_UI_MENU:
        e->g->ui_request = GAME_UI_NONE;
        menu_enter(e);
        break;
    case GAME_UI_LOAD:
        e->g->ui_request = GAME_UI_NONE;
        drop_game_media(e);
        if (game_load_slot(e->g, e->g->load_slot) != 0)
            menu_enter(e);
        break;
    case GAME_UI_GAME:
        e->g->ui_request = GAME_UI_NONE;
        break;
    }
    /* The game has ended (the credits, or a death with no way back):
     * once the last clip has played, the main menu. */
    if (e->g->game_over && !e->g->ask && e->in_menu == 0 && !e->playing && !game_media_pending(e->g) &&
        !e->g->slot_mode)
        menu_enter(e);
    place_slot_cursor(e);
    if (e->in_menu == 1) {
        menu_update(e);
    } else {
        if (e->g->view)
            refresh_picture(e);
        refresh_iface_pictures(e);
    }
    present_frame(e);
    plat_sleep_ms(e->playing ? 5 : 16);
}

/* --- Scripts in the window --------------------------------------------- */

static void script_advance(void *user, u32 ms)
{
    explorer *e = user;
    u32 end = plat_ticks_ms() + ms;
    while (e->running && (i32)(plat_ticks_ms() - end) < 0)
        run_frame(e);
}

/* Ends every clip at once, running its triggers, like a player who
 * skips them all; the location card included. */
static void script_settle(void *user)
{
    explorer *e = user;
    int guard = 0;
    run_frame(e);
    while ((e->playing || game_media_pending(e->g) || e->g->sound) && guard++ < 256) {
        if (e->playing)
            finish_media(e);
        else if (!game_media_pending(e->g) && e->g->sound) {
            stop_sound(e);
            game_sound_end(e->g);
        }
        start_next_media(e);
    }
    run_frame(e);
}

static void script_point(void *user, int x, int y)
{
    explorer *e = user;
    e->mx = x;
    e->my = y;
    scr_to_output(&e->layout, x, y, &e->out_mx, &e->out_my);
    if (!e->playing)
        game_hover(e->g, x, y);
}

static void script_input(void *user, int key, int x, int y)
{
    explorer *e = user;
    plat_event ev;
    memset(&ev, 0, sizeof(ev));
    if (key) {
        ev.type = PLAT_EV_KEY_DOWN;
        ev.key = key;
        handle_event(e, ev);
    } else {
        int types[3] = { PLAT_EV_MOUSE_MOVE, PLAT_EV_MOUSE_DOWN, PLAT_EV_MOUSE_UP }, i;
        scr_to_output(&e->layout, x, y, &ev.x, &ev.y);
        ev.button = PLAT_MOUSE_LEFT;
        for (i = 0; i < 3; i++) {
            ev.type = types[i];
            handle_event(e, ev);
        }
    }
    run_frame(e);
}

static int script_window(void *user, const char *what)
{
    explorer *e = user;
    if (strcmp(what, "menu") == 0)
        return e->in_menu == 1;
    if (strcmp(what, "paused") == 0)
        return e->media_paused;
    if (strcmp(what, "playing") == 0)
        return e->playing;
    return -1;
}

static int script_shot(void *user, const char *path)
{
    explorer *e = user;
    FILE *fp;
    u8 header[54] = { 'B', 'M' };
    int x, y, w, h, pad;
    u32 size;
    run_frame(e);
    w = e->shown_w;
    h = e->shown_h;
    pad = (4 - w * 3 % 4) % 4;
    size = 54 + (u32)((w * 3 + pad) * h);
    if (!e->shown || !(fp = fopen(path, "wb")))
        return -1;
    header[2] = (u8)size, header[3] = (u8)(size >> 8), header[4] = (u8)(size >> 16), header[5] = (u8)(size >> 24);
    header[10] = 54;
    header[14] = 40;
    header[18] = (u8)w, header[19] = (u8)(w >> 8);
    header[22] = (u8)h, header[23] = (u8)(h >> 8);
    header[26] = 1;
    header[28] = 24;
    fwrite(header, 1, sizeof(header), fp);
    for (y = h - 1; y >= 0; y--) {
        static const u8 zero[3] = { 0, 0, 0 };
        for (x = 0; x < w; x++) {
            u32 c = e->shown[(size_t)y * w + x];
            u8 px[3];
            px[0] = (u8)c;
            px[1] = (u8)(c >> 8);
            px[2] = (u8)(c >> 16);
            fwrite(px, 1, 3, fp);
        }
        fwrite(zero, 1, (size_t)pad, fp);
    }
    fclose(fp);
    xf_info("screenshot %s (%dx%d)", path, w, h);
    return 0;
}

int xf_explore(const char *data_dir, const char *const *media_dirs, u32 start_view, int disc,
               const char *script_path)
{
    explorer *e = calloc(1, sizeof(*e));
    char path[1024];
    int running = 1, result = 0;

    if (!e || !(e->g = malloc(sizeof(game))) || game_open(e->g, data_dir) != 0) {
        if (e)
            free(e->g);
        free(e);
        return 1;
    }
    e->data_dir = data_dir;
    e->media_dirs = media_dirs;
    e->fb = e->scene_fb;
    scr_layout_begin(&e->layout, SCREEN_W, SCREEN_H, 0);
    if (getenv("XF_SAVE_DIR"))
        snprintf(e->g->save_dir, sizeof(e->g->save_dir), "%s", getenv("XF_SAVE_DIR"));
    else if (plat_save_dir(e->g->save_dir, sizeof(e->g->save_dir)) != 0)
        e->g->save_dir[0] = '\0';
    game_prefs_load(e->g);
    {
        /* XF_SCREEN=wide or original overrides the saved setting */
        const char *screen = getenv("XF_SCREEN");
        e->wide_pref = e->g->screen_wide;
        if (screen)
            e->wide_pref = strcmp(screen, "wide") == 0 || strcmp(screen, "fill") == 0;
    }
    e->disc = disc;
    if (xf_path_resolve(data_dir, "DLG.TTF", path, sizeof(path)) == 0 ||
        xf_path_resolve(data_dir, "DLG.TTR", path, sizeof(path)) == 0)
        e->have_font = xf_font_load(&e->font, path) == 0;
    if (xf_path_resolve(data_dir, "HCD.TTR", path, sizeof(path)) == 0)
        e->have_hcd_font = xf_font_load(&e->hcd_font, path) == 0;
    if (xf_path_resolve(data_dir, "PHN.TTR", path, sizeof(path)) == 0)
        e->have_phn_font = xf_font_load(&e->phn_font, path) == 0;
    if (plat_font_path("Courier New", path, sizeof(path)) == 0)
        e->have_card_font = xf_font_load(&e->card_font, path) == 0;
    if (!e->have_card_font)
        xf_warn("explore: no Courier New; location cards use the dialogue font");
    e->debug = getenv("XF_DEBUG_OVERLAY") != NULL;
    if (script_path) {
        /* The script starts the game itself. */
    } else if (start_view == 0) {
        /* Like the original: intro, then the main menu. */
        queue_intro(e);
        e->in_menu = -1; /* open the menu once the intro has played */
    } else if (game_enter_view(e->g, start_view) != 0) {
        running = 0;
    }
    e->running = running;
    if (script_path && running) {
        script sc;
        memset(&sc, 0, sizeof(sc));
        sc.g = e->g;
        sc.host.user = e;
        sc.host.advance = script_advance;
        sc.host.settle = script_settle;
        sc.host.shot = script_shot;
        sc.host.point = script_point;
        sc.host.input = script_input;
        sc.host.window = script_window;
        e->in_menu = 0;
        result = script_run(&sc, script_path, 0) ? 1 : 0;
        xf_info("script: %d steps, %s", sc.steps, result ? "stopped at a failure" : "all passed");
    } else {
        while (e->running)
            run_frame(e);
    }
    stop_media(e);
    stop_sound(e);
    close_anims(e);
    free_iface_pictures(e);
    {
        int i;
        for (i = 0; i < GAME_MAX_PHOTOS; i++)
            free(e->photo_cache[i].img.pixels);
    }
    close_icons(e);
    free_inventory(e);
    free_cursors(e);
    plat_show_cursor(1);
    free(e->dlg_talk.pixels);
    free(e->return_img.pixels);
    free(e->dlg_history.pixels);
    free(e->dlg_up.pixels);
    free(e->dlg_down.pixels);
    if (e->pff_open)
        xf_pff_close(&e->pff);
    menu_close(e);
    if (e->nav_open)
        xf_mov_close(&e->nav);
    free(e->out);
    if (e->have_font || e->have_card_font)
        xf_text_cache_clear();
    if (e->have_font)
        xf_font_free(&e->font);
    if (e->have_card_font)
        xf_font_free(&e->card_font);
    if (e->have_hcd_font)
        xf_font_free(&e->hcd_font);
    if (e->have_phn_font)
        xf_font_free(&e->phn_font);
    game_close(e->g);
    free(e->g);
    free(e);
    return result;
}
