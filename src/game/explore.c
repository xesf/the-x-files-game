#include "explore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/game.h"
#include "game/script.h"
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
#define MAX_ICONS 6
#define MAX_INV 32
#define MAX_CURSORS 24
#define MAX_CURSOR_FRAMES 12
#define CARD_PX 16 /* 12 point at 96 dots per inch */

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
    xf_font font;         /* Dlg.ttr: dialogue and labels */
    int have_font;
    xf_font card_font;    /* Courier New: location cards */
    int have_card_font;
    xf_font hcd_font;     /* Hcd.ttr: computer screens */
    int have_hcd_font;
    int debug;            /* Ctrl+Shift+D: hotspot outlines and names */
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
    int cursor_count;
    u32 held_inv;         /* VCInventory taken in hand, for its cursor */
    /* inventory icons: normal and rollover */
    xf_image inv_img[MAX_INV][2];
    u32 inv_ids[MAX_INV];
    int inv_count;
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
    int running, mx, my;
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

static void blit_keyed(explorer *e, const xf_image *img, int x0, int y0);

static void draw_ifaces(explorer *e)
{
    int k, i, order;
    char buf[64];
    for (k = 0; k < e->g->iface_count; k++)
        for (order = 0; order < 256; order++) /* back to front */
            for (i = 0; i < e->g->ifaces[k].count; i++) {
                const game_iface_item *it = &e->g->ifaces[k].items[i];
                if ((it->item.flags & 0xFF) == order && vc_is_enabled(&e->g->interp, it->id))
                    blit_keyed(e, &e->iface_img[k][i], it->item.region.left, it->item.region.top);
            }
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
            xf_surface sf = { e->fb, SCREEN_W, SCREEN_H, SCREEN_W };
            if (!l || !l->text[0] || !vc_is_enabled(&e->g->interp, it->id))
                continue;
            f = l->font == 'H' && e->have_hcd_font ? &e->hcd_font
              : l->font == 'c' && e->have_card_font ? &e->card_font : &e->font;
            px = (float)l->size * 96.0f / 72.0f;
            w = xf_text_width(f, px, l->text, strlen(l->text));
            x = l->align == 'c' ? (r->left + r->right - w) / 2 : l->align == 'r' ? r->right - w : r->left;
            xf_text_draw(&sf, f, px, x, r->bottom - (r->bottom - r->top - (int)px) / 2 - 3, l->text, strlen(l->text),
                         l->color == 'b' ? 0xFFFFFF : 0x000000);
        }
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
                xf_surface sf = { e->fb, SCREEN_W, SCREEN_H, SCREEN_W };
                xf_text_draw(&sf, &e->font, 15, r->left + 8, (r->top + r->bottom) / 2 + 5, text, strlen(text),
                             0x000000);
            }
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
        if (!e->pff_open)
            e->pff_open = xf_pff_open(&e->pff, path) == 0;
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
    xf_surface s = { e->fb, SCREEN_W, SCREEN_H, SCREEN_W };
    int dx, dy;
    for (dy = -1; dy <= 1; dy++)
        for (dx = -1; dx <= 1; dx++)
            if (dx || dy)
                xf_text_draw(&s, f, px, x + dx, y + dy, text, len, 0x000000);
    xf_text_draw(&s, f, px, x, y, text, len, color);
}

/* Splits `text` into lines no wider than `w`. Returns the line count;
 * `starts` and `lens` describe each line. */
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
    xf_surface s = { e->fb, SCREEN_W, SCREEN_H, SCREEN_W };
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
    /* Rolling over an icon shows what it does. */
    if (hover >= 0 && e->have_font) {
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

/* VCInventory: name, icon, rollover icon, 0, cursor, triggers. */
static void refresh_inventory(explorer *e)
{
    u32 ids[MAX_INV];
    int n = game_inventory(e->g, ids, MAX_INV), i;
    if (n == e->inv_count && memcmp(ids, e->inv_ids, (size_t)n * sizeof(u32)) == 0)
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
}

static int inv_at(explorer *e, int x, int y)
{
    int i;
    if (y < INV_Y || y >= INV_Y + INV_H)
        return -1;
    for (i = 0; i < e->inv_count; i++) {
        int x0 = 10 + i * (INV_W + INV_GAP);
        if (x >= x0 && x < x0 + INV_W)
            return i;
    }
    return -1;
}

static void draw_inventory(explorer *e, int mx, int my)
{
    xf_surface s = { e->fb, SCREEN_W, SCREEN_H, SCREEN_W };
    int i, hover = inv_at(e, mx, my);
    for (i = 0; i < e->inv_count; i++) {
        const xf_image *img = &e->inv_img[i][i == hover && e->inv_img[i][1].pixels ? 1 : 0];
        blit_keyed(e, img, 10 + i * (INV_W + INV_GAP), INV_Y);
    }
    if (hover >= 0 && e->have_font) {
        char name[96] = "";
        xf_neo_reader r;
        if (xf_neo_object(&e->g->db, e->inv_ids[hover], &r, NULL, NULL) == 0)
            vc_read_name(&e->g->db, xf_neo_u32(&r), name, sizeof(name));
        xf_text_draw(&s, &e->font, 14, 10, INV_Y - 6, name, strlen(name), 0xFFFFFF);
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
        if (!e->pff_open)
            e->pff_open = xf_pff_open(&e->pff, path) == 0;
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

/* Draws the game's cursor at the pointer, or shows the system pointer
 * where the game has none. */
static void draw_cursor(explorer *e, int mx, int my)
{
    u32 id = held_cursor(e);
    const cursor_art *c;
    if (!id && !e->playing && e->in_menu != 1)
        id = game_cursor_at(e->g, mx, my);
    c = id ? get_cursor(e, id) : NULL;
    plat_show_cursor(!c);
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

static void draw(explorer *e, int mx, int my)
{
    static const u32 colors[] = { 0x40FF40, 0xFFE040, 0x40E0FF };
    xf_surface s = { e->fb, SCREEN_W, SCREEN_H, SCREEN_W };
    const game *g = e->g;
    int i, y;

    memset(e->fb, 0, sizeof(e->fb));
    refresh_icons(e);
    if (e->playing) {
        const u32 *f = e->have_clip ? xf_player_frame(&e->player) : NULL;
        const char *sub = e->have_clip ? xf_player_subtitle(&e->player) : NULL;
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
        if (sub && e->have_font)
            xf_text_draw_wrapped(&s, &e->font, 20, SCREEN_W / 2, PICTURE_Y + PICTURE_H + 30, SCREEN_W - 40,
                                 sub, 0xFFFFFF, 0x000000);
        if (e->card_active)
            draw_card(e, cx, cy, cw, ch);
        draw_icons(e, mx, my);
        plat_show_cursor(1);
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
    if (e->debug) {
        /* Ctrl+Shift+D: hotspot outlines and the view's media name. */
        for (i = 0; i < g->spot_count; i++) {
            const vc_region *r = &g->spots[i].region;
            int inside = mx >= r->left && mx < r->right && my >= r->top && my < r->bottom;
            frame_rect(e, r, inside ? 0xFFFFFF : colors[g->spots[i].kind]);
        }
        if (e->have_font)
            xf_text_draw(&s, &e->font, 14, 8, 20, e->label, strlen(e->label), 0xC0C0C0);
    }
    draw_icons(e, mx, my);
    if (g->talking)
        draw_dialog(e, mx, my);
    refresh_inventory(e);
    if (my >= INV_REVEAL && !g->iface_count)
        draw_inventory(e, mx, my);
    draw_ifaces(e);
    draw_cursor(e, mx, my);
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
    plat_show_cursor(1);
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

/* One frame: input, clips and sound, drawing. */
static void run_frame(explorer *e)
{
    plat_event ev;
    game_tick(e->g, plat_ticks_ms());
    while (plat_poll(&ev)) {
        if (ev.type == PLAT_EV_KEY_DOWN && ev.key == PLAT_KEY_ESCAPE && e->g->talking)
            game_end_talk(e->g); /* Escape closes the dialogue box first */
        else if (ev.type == PLAT_EV_QUIT || (ev.type == PLAT_EV_KEY_DOWN && ev.key == PLAT_KEY_ESCAPE))
            e->running = 0;
        else if (ev.type == PLAT_EV_KEY_DOWN && ev.key == 'd' &&
                 (ev.mods & (PLAT_MOD_CTRL | PLAT_MOD_SHIFT)) == (PLAT_MOD_CTRL | PLAT_MOD_SHIFT)) {
            e->debug = !e->debug;
            xf_info("debug overlay %s", e->debug ? "on" : "off");
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
                menu_close(e);
                game_new(e->g);
            } else if (item == 6) { /* Quit */
                e->running = 0;
            }
        } else if (ev.type == PLAT_EV_MOUSE_MOVE) {
            e->mx = ev.x;
            e->my = ev.y;
            if (!e->playing)
                game_hover(e->g, e->mx, e->my);
        } else if (e->playing && ev.type == PLAT_EV_MOUSE_DOWN && icon_at(e, ev.x, ev.y) >= 0) {
            /* An emotion icon answers at once: the clip ends first. */
            u32 icon = e->icon_ids[icon_at(e, ev.x, ev.y)];
            finish_media(e);
            game_click_icon(e->g, icon);
        } else if (e->playing && (ev.type == PLAT_EV_MOUSE_DOWN || ev.type == PLAT_EV_KEY_DOWN)) {
            if (!e->card_active)
                finish_media(e); /* skip the clip; the location card cannot be skipped */
        } else if (ev.type == PLAT_EV_MOUSE_DOWN && ev.button == PLAT_MOUSE_LEFT && icon_at(e, ev.x, ev.y) >= 0) {
            game_click_icon(e->g, e->icon_ids[icon_at(e, ev.x, ev.y)]);
        } else if (ev.type == PLAT_EV_MOUSE_DOWN && ev.button == PLAT_MOUSE_LEFT) {
            /* Layout items first (a close-up covers everything), then
             * the dialogue box, the inventory, then the view. */
            int inv = ev.y >= INV_REVEAL && !e->g->iface_count ? inv_at(e, ev.x, ev.y) : -1;
            if (game_iface_item_at(e->g, ev.x, ev.y))
                game_click(e->g, ev.x, ev.y);
            else if (e->g->talking && click_dialog(e, ev.x, ev.y))
                ;
            else if (inv >= 0) {
                game_click_inventory(e->g, e->inv_ids[inv]);
                e->held_inv = e->inv_ids[inv];
            }
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
        draw(e, e->mx, e->my);
    }
    plat_present(e->fb, SCREEN_W, SCREEN_H, SCREEN_W);
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
    while ((e->playing || game_media_pending(e->g)) && guard++ < 256) {
        if (e->playing)
            finish_media(e);
        start_next_media(e);
    }
    run_frame(e);
}

static void script_point(void *user, int x, int y)
{
    explorer *e = user;
    e->mx = x;
    e->my = y;
    if (!e->playing)
        game_hover(e->g, x, y);
}

static int script_shot(void *user, const char *path)
{
    explorer *e = user;
    FILE *fp = fopen(path, "wb");
    u8 header[54] = { 'B', 'M' };
    int x, y;
    u32 size = 54 + SCREEN_W * SCREEN_H * 3;
    if (!fp)
        return -1;
    run_frame(e);
    header[2] = (u8)size, header[3] = (u8)(size >> 8), header[4] = (u8)(size >> 16);
    header[10] = 54;
    header[14] = 40;
    header[18] = SCREEN_W & 0xFF, header[19] = SCREEN_W >> 8;
    header[22] = SCREEN_H & 0xFF, header[23] = SCREEN_H >> 8;
    header[26] = 1;
    header[28] = 24;
    fwrite(header, 1, sizeof(header), fp);
    for (y = SCREEN_H - 1; y >= 0; y--)
        for (x = 0; x < SCREEN_W; x++) {
            u32 c = e->fb[y * SCREEN_W + x];
            u8 px[3] = { (u8)c, (u8)(c >> 8), (u8)(c >> 16) };
            fwrite(px, 1, 3, fp);
        }
    fclose(fp);
    xf_info("screenshot %s", path);
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
    e->disc = disc;
    if (xf_path_resolve(data_dir, "DLG.TTF", path, sizeof(path)) == 0 ||
        xf_path_resolve(data_dir, "DLG.TTR", path, sizeof(path)) == 0)
        e->have_font = xf_font_load(&e->font, path) == 0;
    if (xf_path_resolve(data_dir, "HCD.TTR", path, sizeof(path)) == 0)
        e->have_hcd_font = xf_font_load(&e->hcd_font, path) == 0;
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
    close_icons(e);
    free_inventory(e);
    free_cursors(e);
    plat_show_cursor(1);
    free(e->dlg_talk.pixels);
    free(e->dlg_history.pixels);
    free(e->dlg_up.pixels);
    free(e->dlg_down.pixels);
    if (e->pff_open)
        xf_pff_close(&e->pff);
    menu_close(e);
    if (e->nav_open)
        xf_mov_close(&e->nav);
    if (e->have_font || e->have_card_font)
        xf_text_cache_clear();
    if (e->have_font)
        xf_font_free(&e->font);
    if (e->have_card_font)
        xf_font_free(&e->card_font);
    if (e->have_hcd_font)
        xf_font_free(&e->hcd_font);
    game_close(e->g);
    free(e->g);
    free(e);
    return result;
}
