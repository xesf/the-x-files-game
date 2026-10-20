/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
#include "explore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
#include "xf_text.h"

#define SCREEN_W 640
#define SCREEN_H 480
#define PICTURE_X 20
#define PICTURE_Y 90
#define PICTURE_W 600
#define PICTURE_H 240

/* Main menu (0x48EE40 MainMenu_ctor): media IDs are fixed in the engine
 * (0x482FE0 MainMenu_load_media). Positions measured from the original
 * running under Wine; see docs/engine/runtime.md. */
enum { MENU_LEFT, MENU_RIGHT, MENU_MID, MENU_SOUND, MENU_TOP, MENU_PARTS };

typedef struct {
    u32 asset;
    int x, y, loop;
} menu_part;

static const menu_part g_menu_parts[MENU_PARTS] = {
    { 49583, 0, 0, 0 },    /* LEFTmain.mov, 472x480 */
    { 49587, 472, 0, 0 },  /* RIGHT.mov, 168x480, item labels */
    { 49585, 60, 102, 1 }, /* MIDloop.mov, 388x310 */
    { 94320, 0, 0, 1 },    /* MIDloop.amv, the loop's sound */
    { 49589, 57, 0, 1 },   /* TOPro.mov, 352x60: the title's rollover, with sound */
};

/* The title's rollover area (0x6C3F38, MainMenu_hover 0x45E320). */
#define MENU_TOP_LEFT 57
#define MENU_TOP_RIGHT 409
#define MENU_TOP_BOTTOM 60

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
 * its last frame. A clip that plays once has a full player, with its
 * sound and captions, and its end triggers run when it ends. */
typedef struct {
    xf_mov mov;
    xf_video video;
    const xf_mov_track *track;
    u32 start_ms, length_ms;
    int loop;
    u32 started;
    u32 asset;            /* the media reference it shows */
    xf_player player;
    int use_player, ended;
    char path[1024];      /* a looping player starts again from it */
    u32 range_start, range_end;
} view_anim;

static const xf_video *anim_video(const view_anim *an) { return an->use_player ? &an->player.video : &an->video; }

#define MAX_ANIMS 4
#define KEY_BLUE 0x0000FF
#define MAX_ICONS 6
#define MAX_IDEAS 8
#define MAX_INV 32
#define MAX_CURSORS 24
#define MAX_CURSOR_FRAMES 12
/* Sizes from the exe, in pixels: Font_select (0x53C520) passes them to
 * Font_create, which makes lfHeight -size. Font 4 is Courier New. */
#define CARD_PX 12      /* VideoText_start (0x483A30): Courier New 12 */
#define ROLLOVER_PX 14  /* the rollover text object (0x48F870): Courier New 14 */
#define FIELD_PX 14     /* typed text, FBI login and ING (0x468660, 0x407990): Courier New 14 */
#define PWBK_FIELD_PX 12 /* the PowerBook login (0x4F2C30): Courier New 12 */
/* Captions are the clips' own text tracks: a 600x20 strip under the
 * picture, Geneva 12 white on black, centred (QuickTime for Windows
 * draws Geneva with Arial). */
#define CAPTION_PX 12
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
    int prefer_dvd;       /* DVD clips before QuickTime ones (not on the Vita) */
    /* the DVD edition's opening titles, played after the queued clips */
    const char *titles[3];
    int title_count, title_next;
    u32 card_start, card_until;
    /* the main menu */
    int in_menu;
    int lang_panel;       /* the Options screen's Language panel is open */
    int media_paused;     /* the game's clips and sounds hold while the menu is up */
    u32 media_pause_from;
    int from_menu;        /* Options or Help, opened from the menu during a game */
    xf_image return_img;  /* return.pic: "Return" over the Previous row */
    xf_player menu[MENU_PARTS];
    int menu_open[MENU_PARTS];
    int menu_seen;        /* the menu's opening animation has played once */
    int menu_hover;
    xf_mov right_mov;     /* RIGHT.mov is stepped by frame */
    xf_video right;
    int right_open, right_frame, right_target;
    xf_player right_sound;  /* RIGHT.mov's sound for the frames shown: the labels coming in, a rollover */
    int right_sound_open;
    char right_path[1024];
    u32 right_next_ms;
    /* sounds beside the clips; loops repeat */
    xf_player sound;
    int sound_open, sound_loop;
    xf_player effect;     /* game effect sounds (PageTurnSound) */
    int effect_open;
    u32 effect_serial;
    u32 sound_serial;
    u32 fade_serial, fade_start, fade_view; /* FadeToBlack */
    u32 subliminal_shown; /* the serial of the last subliminal word drawn */
    xf_image tool_img[6]; /* the tools' masks, loaded when first in hand */
    int tool_img_tried[6];
    xf_player overlay;    /* PlayAssetAt: a small clip over the screen */
    int overlay_open;
    u32 overlay_serial;
    char sound_path[1024];
    xf_font font;         /* Dlg.ttr: dialogue and labels */
    int have_font;
    xf_font card_font;    /* Courier New: location cards */
    int have_card_font;
    xf_font caption_font; /* Arial for the captions' Geneva */
    int have_caption_font;
    xf_font cjk_font;     /* Japanese characters, under every other font */
    int have_cjk_font;
    char cjk_path[1024];
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
    xf_image dlg_talk, dlg_history, dlg_up, dlg_down, dlg_bar, dlg_thumb;
    int talk_count; /* questions in the open tab */
    int talk_scroll;
    int talk_tab;         /* 0 TALK: questions not asked yet; 1 HISTORY: asked */
    /* emotion and action icons */
    view_anim icon_anim[MAX_ICONS];
    int icon_open[MAX_ICONS];
    u32 icon_ids[MAX_ICONS];
    int icon_count;
    u8 icon_action[MAX_ICONS]; /* an action icon (on the right), not an emotion */
    u32 idea_ids[MAX_IDEAS], idea_convs[MAX_IDEAS]; /* the idea icons of the character talked to */
    xf_image idea_img[MAX_IDEAS][2];
    int idea_count;
    /* cursors */
    cursor_art cursors[MAX_CURSORS];
    cursor_art cross;     /* the default pointer */
    u32 cross_px[CROSS_SIZE * CROSS_SIZE];
    int cursor_count;
    u32 held_inv;         /* VCInventory taken in hand */
    int inv_inside;       /* the pointer is over the inventory's rows */
    u32 inv_cursor[48];   /* the VCCursor of each inventory value (Inventory_i... = n) */
    int inv_cursors_built;
    /* inventory icons: normal and rollover */
    xf_image inv_img[MAX_INV][2];
    u32 inv_ids[MAX_INV];
    int inv_count;
    int inv_items;        /* inv_ids: the inventory, then the lab's evidence */
    int ai_ready;         /* the Artificial Intuition icon is up, top right */
    xf_image ai_img[8];   /* AI.pff: it spins while a hint is close */
    int ai_frames;
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

/* Courier New where the exe draws it, else the dialogue font. */
static const xf_font *courier(const explorer *e) { return e->have_card_font ? &e->card_font : &e->font; }
static const xf_font *caption_font(const explorer *e) { return e->have_caption_font ? &e->caption_font : &e->font; }

/* Finds a media file in the chosen language (voices for clips and
 * sounds, text for the rest), the game folder, then the extra media
 * folders. */
static int resolve_media(const explorer *e, const char *name, char *out, size_t size)
{
    int i;
    if (game_resolve(e->g, name, out, size) == 0)
        return 0;
    for (i = 0; e->media_dirs && e->media_dirs[i]; i++)
        if (xf_path_resolve(e->media_dirs[i], name, out, size) == 0)
            return 0;
    return -1;
}

/* A DVD clip carries its release's voices: with language folders, only
 * the voice language's own. */
static int resolve_vob(const explorer *e, const char *name, char *out, size_t size)
{
    if (e->g->langs.count)
        return xf_langs_resolve_in(&e->g->langs, e->g->langs.voices, name, out, size);
    return resolve_media(e, name, out, size);
}

/* The text language's captions for a clip, when they come from another
 * file: a translation's SRT, or another release's copy of the clip. */
static void use_captions(explorer *e, xf_player *p, const char *rel, const char *path)
{
    char src[1024];
    if (xf_langs_captions(&e->g->langs, rel, path, src, sizeof(src)) == 0 && xf_player_captions(p, src) == 0)
        xf_debug("captions of %s from %s", rel, src);
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

static void close_anim(view_anim *an)
{
    if (an->use_player) {
        xf_player_close(&an->player);
    } else {
        xf_video_close(&an->video);
        xf_mov_close(&an->mov);
    }
}

static void close_anims(explorer *e)
{
    int i;
    for (i = 0; i < e->anim_count; i++)
        close_anim(&e->anims[i]);
    e->anim_count = 0;
}

static void set_volume(explorer *e, const xf_player *p, u32 pref);

/* Opens a movie range as a view animation. Times are movie time in ms;
 * 0..0 means the whole movie. */
static int movie_has_sound(const char *path)
{
    xf_mov m;
    int has;
    if (xf_mov_open(&m, path) != 0)
        return 0;
    has = xf_mov_find(&m, XF_TRACK_AUDIO) != NULL;
    xf_mov_close(&m);
    return has;
}

static int open_anim(explorer *e, const char *path, u32 start, u32 end, int loop)
{
    view_anim *an = &e->anims[e->anim_count];
    u32 k;
    if (e->anim_count >= MAX_ANIMS)
        return -1;
    memset(an, 0, sizeof(*an));
    /* a clip that plays once, or a loop with a sound track (the Comity
     * Inn's TV), has a full player; a silent loop only its pictures */
    if ((!loop || movie_has_sound(path)) && xf_player_open_range(&an->player, path, plat_ticks_ms(), start, end) == 0) {
        if (!an->player.has_video) {
            xf_player_close(&an->player);
            return -1;
        }
        an->loop = loop;
        if (loop)
            xf_debug("view clip %s loops with its sound", path);
        snprintf(an->path, sizeof(an->path), "%s", path);
        an->range_start = start;
        an->range_end = end;
        an->use_player = 1;
        an->start_ms = start;
        an->length_ms = XF_MAX(an->player.duration_ms, 1);
        an->started = plat_ticks_ms();
        set_volume(e, &an->player, 976); /* Pref_iVolumeDialog */
        e->anim_count++;
        return 0;
    }
    if (xf_mov_open(&an->mov, path) != 0)
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

/* Steps a view animation to the current time. A clip that plays once
 * runs its end triggers when it ends (game_view_clip_end). */
static void update_anim(explorer *e, view_anim *an, u32 now)
{
    u32 t = now - an->started;
    u64 movie_time, media;
    if (an->use_player) {
        if (!xf_player_update(&an->player, now)) {
            xf_player again;
            if (an->loop && xf_player_open_range(&again, an->path, now, an->range_start, an->range_end) == 0) {
                /* a sound loop starts again */
                xf_player_close(&an->player);
                an->player = again;
                set_volume(e, &an->player, 976);
            } else if (an->loop) {
                an->loop = 0; /* cannot reopen: holds its last picture */
            } else if (!an->ended) {
                an->ended = 1;
                game_view_clip_end(e->g, an->asset);
            }
        }
        return;
    }
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
        const xf_video *vd = anim_video(an);
        int w = XF_MIN(vd->width, PICTURE_W), h = XF_MIN(vd->height, PICTURE_H);
        int x0 = PICTURE_X + (PICTURE_W - w) / 2, y0 = PICTURE_Y + (PICTURE_H - h) / 2;
        u32 t = an->use_player ? an->player.now_ms : plat_ticks_ms() - an->started, hot;
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
 * disc whose file is installed. A view lists its full-picture loops in
 * order of priority (Willmore's desk: Cook at the desk, the desk with
 * the letter and files, then the empty desk): the first enabled one is
 * the picture, the others are not shown. */
static void load_picture(explorer *e)
{
    int i, j, pass, have_full = 0;
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
                    const xf_video *vd = anim_video(&e->anims[e->anim_count - 1]);
                    int full = vd->width >= PICTURE_W && vd->height >= PICTURE_H;
                    done = 1;
                    if (full && have_full) { /* a lower-priority picture */
                        close_anim(&e->anims[--e->anim_count]);
                        xf_debug("view %u: %s behind an earlier picture", e->g->view, a.logical);
                        continue;
                    }
                    have_full |= full;
                    e->anims[e->anim_count - 1].asset = e->view_media[i];
                    if (e->anims[e->anim_count - 1].use_player)
                        use_captions(e, &e->anims[e->anim_count - 1].player, m->path, path);
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
            if (img->pixels && strstr(a.logical, "pda pieces") && !strstr(a.logical, "/maps/")) {
                /* the PDA's pieces (map dots and rings, the frame) have a
                 * white background where the others have blue; the maps
                 * themselves are whole pictures */
                size_t k;
                for (k = 0; k < (size_t)img->width * (size_t)img->height; k++)
                    if ((img->pixels[k] & 0xFFFFFF) == 0xFFFFFF)
                        img->pixels[k] = KEY_BLUE;
            }
        } else if (open_nav_file(e, m->path) == 0 && w > 0 && h > 0) {
            /* The whole frame: a close-up's item can be smaller than its
             * picture, which then showed cut at the bottom. */
            const xf_mov_track *t = xf_mov_find(&e->nav, XF_TRACK_VIDEO);
            if (t && t->width && t->height) {
                w = t->width;
                h = t->height;
            }
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

/* An item's enabled pictures, each drawn over the one before (the PDA
 * map's places list the green dot, then the red one: an open place is
 * red). With none enabled, the first picture. */
static void load_item_picture(explorer *e, const game_iface_item *it, xf_image *img)
{
    u32 ids[8];
    int n = vc_read_list(&e->g->db, it->item.assets, ids, 8), i, w = it->item.region.right - it->item.region.left,
        h = it->item.region.bottom - it->item.region.top;
    memset(img, 0, sizeof(*img));
    for (i = 0; i < n; i++) {
        xf_image over;
        size_t k;
        if (!vc_is_enabled(&e->g->interp, ids[i]))
            continue;
        if (!img->pixels) {
            load_asset_picture(e, ids[i], w, h, img);
            continue;
        }
        memset(&over, 0, sizeof(over));
        load_asset_picture(e, ids[i], w, h, &over);
        if (over.pixels && over.width == img->width && over.height == img->height)
            for (k = 0; k < (size_t)img->width * (size_t)img->height; k++)
                if (over.pixels[k] != KEY_BLUE)
                    img->pixels[k] = over.pixels[k];
        free(over.pixels);
    }
    for (i = 0; i < n && !img->pixels; i++)
        load_asset_picture(e, ids[i], w, h, img);
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
static void draw_scroll_box(explorer *e, const game_iface_item *it, const game_scroll *sb)
{
    const xf_font *f;
    xf_surface sf = screen_surface(e);
    float px;
    int y, line = 0, pos = 0, len, lh;
    if (e->g->text_off || !e->have_font)
        return;
    /* TextFile_show (0x480850): 'H' (Helvetica) is Arial, the rest Courier New */
    f = sb->font == 'H' && e->have_caption_font ? &e->caption_font : courier(e);
    px = (float)(sb->size ? sb->size : 12); /* pixels: 0x53D560 makes lfHeight -size */
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

/* SetIItemText labels: font 'H' is Hcd.ttr, 'c' Courier New; the size is
 * the character height in pixels (0x53D560 makes lfHeight -size). */
static void draw_label(explorer *e, const game_iface_item *it, const game_item_label *l)
{
    const vc_region *r = &it->item.region;
    xf_surface sf = screen_surface(e);
    /* SetIItemText (0x47D560): 'H' (Helvetica) is Arial (Font_select 3),
     * the rest Courier New (4); 'P', the phone display, is ours */
    const xf_font *f = l->font == 'P' && e->have_phn_font ? &e->phn_font
                     : l->font == 'H' && e->have_caption_font ? &e->caption_font : courier(e);
    float px = (float)l->size;
    if (l->align == 'w') {
        /* a page of text: wrapped to the item, from its top */
        const char *t = l->text;
        int y = r->top + (int)px, lh = (int)(px * 1.2f);
        while (*t && y <= r->bottom) {
            int starts[64], lens[64], nl, j;
            size_t plen = strcspn(t, "\n");
            char para[1024];
            snprintf(para, sizeof(para), "%.*s", (int)plen, t);
            nl = para[0] ? wrap_lines_font(f, para, px, r->right - r->left, starts, lens, 64) : 1;
            if (!para[0])
                lens[0] = starts[0] = 0;
            for (j = 0; j < nl && y <= r->bottom; j++, y += lh)
                xf_text_draw(&sf, f, px, r->left, y, para + starts[j], (size_t)lens[j], 0x000000);
            t += plen + (t[plen] == '\n');
        }
    } else {
        /* One line per newline, the block centred in the item. */
        const char *line = l->text, *c;
        int lines = 1, lh = (int)(px * 1.2f), y;
        for (c = l->text; *c; c++)
            lines += *c == '\n';
        y = r->bottom - (r->bottom - r->top - (int)px) / 2 - 3 - (lines - 1) * lh / 2;
        while (line) {
            const char *nl = strchr(line, '\n');
            size_t len = nl ? (size_t)(nl - line) : strlen(line);
            int w = xf_text_width(f, px, line, len), x;
            while (len && w > r->right - r->left) { /* cut at the item's edge, a whole character at a time */
                do
                    len--;
                while (len && ((u8)line[len] & 0xC0) == 0x80);
                w = xf_text_width(f, px, line, len);
            }
            x = l->align == 'c' ? (r->left + r->right - w) / 2 : l->align == 'r' ? r->right - w : r->left;
            /* 0x47D560: 'w' is white, 'b' and the rest black */
            xf_text_draw(&sf, f, px, x, y, line, len, l->color == 'w' ? 0xFFFFFF : 0x000000);
            y += lh;
            line = nl ? nl + 1 : NULL;
        }
    }
}

/* One layout: its pictures back to front, its labels, its scrolling
 * text, then the typed text of its fields with a caret in the one that
 * has focus. */
static void draw_iface_layer(explorer *e, int k)
{
    const game_iface *d = &e->g->ifaces[k];
    const game_scroll *sb;
    int i, order;
    char buf[64];
    for (order = 255; order >= 0; order--) { /* a low order is in front */
        if (order == 0xC7 && e->g->fade_back && k == e->g->iface_count - 1) {
            /* FadeBack: the screen dims behind the layout's own pieces:
             * the Help screen's picture of the game (order 0xC8) dims
             * under its highlights and texts (0x65 and up); the PDA (it
             * calls FadeBack as it opens) keeps its map (0xC7) bright */
            size_t p;
            for (p = 0; p < (size_t)SCREEN_W * SCREEN_H; p++) {
                u32 c = e->fb[p];
                e->fb[p] = ((c >> 16 & 0xFF) * 3 / 10) << 16 | ((c >> 8 & 0xFF) * 3 / 10) << 8 | (c & 0xFF) * 3 / 10;
            }
        }
        for (i = 0; i < d->count; i++)
            if ((d->items[i].item.flags & 0xFF) == order && vc_is_enabled(&e->g->interp, d->items[i].id))
                blit_keyed(e, &e->iface_img[k][i], d->items[i].item.region.left, d->items[i].item.region.top);
    }
    for (i = 0; i < d->count; i++) {
        const game_item_label *l = game_item_label_of(e->g, d->items[i].id);
        if (l && l->text[0] && vc_is_enabled(&e->g->interp, d->items[i].id))
            draw_label(e, &d->items[i], l);
    }
    for (i = 0; i < d->count; i++) /* a text shows with its item */
        if ((sb = game_scroll_box(e->g, d->items[i].id)) && vc_is_enabled(&e->g->interp, d->items[i].id))
            draw_scroll_box(e, &d->items[i], sb);
    for (i = 0; e->have_font && i < d->count; i++) {
        const game_iface_item *it = &d->items[i];
        const vc_region *r = &it->item.region;
        const char *text;
        xf_surface sf;
        if (!vc_is_enabled(&e->g->interp, it->id) || !(text = game_item_text(e->g, it->id, buf, sizeof(buf) - 1)))
            continue;
        if (it->id == e->g->focus_item && (plat_ticks_ms() / 500) % 2)
            strcat(buf, "|");
        sf = screen_surface(e);
        {
            /* the PowerBook's fields (50749, 50750) are smaller */
            float px = it->id == 50749 || it->id == 50750 ? PWBK_FIELD_PX : FIELD_PX;
            xf_text_draw(&sf, courier(e), px, r->left + 8, (r->top + r->bottom) / 2 + (int)px / 3, text, strlen(text),
                         0x000000);
        }
    }
}

/* The Options screen (layout 49738) has three tabs, Audio, Graphics and
 * Game Play, each opening a panel layout. When the game folder holds
 * language folders, our engine adds a fourth, Language, under them: its
 * panel has a Voices and a Text row, drawn like the Audio panel's rows
 * (the control on the left, the letter-spaced name, a line). */
#define OPTIONS_MAIN 49738
#define LANG_TAB_X0 492 /* the tab's name, right-aligned like Game Play's */
#define LANG_TAB_X1 624
#define LANG_TAB_Y0 160 /* under Game Play, also when it takes two lines (Spanish) */
#define LANG_TAB_Y1 190
#define LANG_ROW_Y 164 /* the first row's top, as the Audio panel's; rows 48 apart */
#define LANG_ROW_H 18
#define LANG_CTRL_X0 57 /* the control: < name >, where the Audio sliders are */
#define LANG_CTRL_X1 192
#define LANG_LABEL_X 207
static const u32 g_option_panels[] = { 49781, 49783, 49785 }; /* Audio, Graphics, Game Play */
static const u32 g_option_tab_lights[] = { 49744, 49746, 49748 };

static int options_layer(const explorer *e)
{
    const game *g = e->g;
    int k;
    for (k = 0; k < g->iface_count; k++)
        if (g->ifaces[k].layout == OPTIONS_MAIN)
            return k;
    return -1;
}

static int lang_tab_shown(const explorer *e)
{
    const game *g = e->g;
    int k = options_layer(e);
    return g->langs.count > 1 && k >= 0 && k >= g->iface_count - 2 && e->have_font && !g->ask;
}

static int lang_panel_shown(const explorer *e)
{
    const game *g = e->g;
    return e->lang_panel && lang_tab_shown(e) && g->ifaces[g->iface_count - 1].layout == OPTIONS_MAIN;
}

/* What is under a point: 1 the tab, 2 or 3 a row (voices, text) with -1
 * for its < arrow; 0 nothing. */
static int lang_hit(const explorer *e, int x, int y, int *dir)
{
    int k;
    *dir = 1;
    if (!lang_tab_shown(e))
        return 0;
    if (x >= LANG_TAB_X0 && x < LANG_TAB_X1 && y >= LANG_TAB_Y0 && y < LANG_TAB_Y1)
        return 1;
    if (!lang_panel_shown(e))
        return 0;
    for (k = 0; k < 2; k++)
        if (y >= LANG_ROW_Y + 48 * k - 4 && y < LANG_ROW_Y + 48 * k + LANG_ROW_H + 4 && x >= LANG_CTRL_X0 && x < 450) {
            if (x < LANG_CTRL_X0 + 18)
                *dir = -1;
            return 2 + k;
        }
    return 0;
}

/* The row names in the text language. */
static const char *lang_row_name(const explorer *e, int row)
{
    static const struct {
        const char *code, *title, *voices, *text;
    } names[] = {
        { "de", "Sprache", "Stimmen", "Text" }, { "fr", "Langue", "Voix", "Texte" },
        { "it", "Lingua", "Voci", "Testo" },    { "es", "Idioma", "Voces", "Texto" },
        { "pt", "Idioma", "Vozes", "Texto" },
    };
    const xf_langs *l = &e->g->langs;
    size_t i;
    for (i = 0; l->text >= 0 && i < XF_COUNT(names); i++)
        if (strcmp(names[i].code, l->langs[l->text].code) == 0)
            return row < 0 ? names[i].title : row ? names[i].text : names[i].voices;
    return row < 0 ? "Language" : row ? "Text" : "Voices";
}

static void load_game_fonts(explorer *e);

/* The next (dir 1) or previous (-1) language with voices (row 0) or text
 * (row 1). */
static void lang_row_click(explorer *e, int row, int dir)
{
    const xf_langs *l = &e->g->langs;
    int cur = row ? l->text : l->voices, k, next = -1;
    for (k = 1; k <= l->count && next < 0; k++) {
        int i = ((cur + dir * k) % l->count + l->count) % l->count;
        if (row ? l->langs[i].text : l->langs[i].voices)
            next = i;
    }
    if (next < 0 || next == cur)
        return;
    game_set_languages(e->g, row ? -1 : next, row ? next : -1);
    xf_info("options: %s %s", row ? "text" : "voices", l->langs[next].code);
    if (row) {
        load_game_fonts(e);
        e->g->iface_serial++; /* the layouts' pictures, in the new language */
        free(e->dlg_talk.pixels); /* the dialogue folder's tabs too */
        free(e->dlg_history.pixels);
        e->dlg_talk.pixels = e->dlg_history.pixels = NULL;
    }
}

/* A click on the Language tab: the other panels close and their tab
 * names go dark, as choosing a tab does; ours opens. */
static void lang_tab_click(explorer *e)
{
    size_t i;
    for (i = 0; i < XF_COUNT(g_option_panels); i++) {
        game_close_layout(e->g, g_option_panels[i]);
        vc_set_enabled(&e->g->interp, g_option_tab_lights[i], 0);
    }
    e->lang_panel = 1;
    xf_info("options: language panel");
}

/* Letter-spaced text, as the Options screen's own labels. */
static int draw_spaced(explorer *e, float px, int x, int y, const char *text, int gap, u32 color)
{
    xf_surface sf = screen_surface(e);
    const char *c = text;
    while (*c) {
        size_t n = 1;
        while (((u8)c[n] & 0xC0) == 0x80)
            n++;
        xf_text_draw(&sf, &e->font, px, x, y, c, n, color);
        x += xf_text_width(&e->font, px, c, n) + gap;
        c += n;
    }
    return x;
}

static int spaced_width(explorer *e, float px, const char *text, int gap)
{
    const char *c = text;
    int w = 0;
    while (*c) {
        size_t n = 1;
        while (((u8)c[n] & 0xC0) == 0x80)
            n++;
        w += xf_text_width(&e->font, px, c, n) + gap;
        c += n;
    }
    return w - gap;
}

/* The Options tabs' words are pictures in the clip: square letters of
 * 2-pixel strokes, capitals 20 pixels high, small letters 16, about 10
 * pixels apart. The Language tab draws its name in the same style from
 * these cells (2x2 pixels each; rows 0-9 down to the baseline, 10-12
 * below it). Other letters fall back to the dialogue font. */
static const struct {
    char c;
    const char *rows;
} g_tab_font[] = {
    { 'A', "###|#.#|#.#|#.#|###|#.#|#.#|#.#|#.#|#.#" },
    { 'B', "##.|#.#|#.#|#.#|###|#.#|#.#|#.#|#.#|###" },
    { 'C', "###|#..|#..|#..|#..|#..|#..|#..|#..|###" },
    { 'D', "##.|#.#|#.#|#.#|#.#|#.#|#.#|#.#|#.#|##." },
    { 'E', "###|#..|#..|#..|###|#..|#..|#..|#..|###" },
    { 'F', "###|#..|#..|#..|###|#..|#..|#..|#..|#.." },
    { 'G', "###|#..|#..|#..|#.#|#.#|#.#|#.#|#.#|###" },
    { 'H', "#.#|#.#|#.#|#.#|###|#.#|#.#|#.#|#.#|#.#" },
    { 'I', "#|#|#|#|#|#|#|#|#|#" },
    { 'J', "..#|..#|..#|..#|..#|..#|..#|..#|..#|###" },
    { 'K', "#.#|#.#|#.#|##.|##.|#.#|#.#|#.#|#.#|#.#" },
    { 'L', "#..|#..|#..|#..|#..|#..|#..|#..|#..|###" },
    { 'M', "#####|#.#.#|#.#.#|#.#.#|#.#.#|#.#.#|#.#.#|#.#.#|#.#.#|#.#.#" },
    { 'N', "###|#.#|#.#|#.#|#.#|#.#|#.#|#.#|#.#|#.#" },
    { 'O', "###|#.#|#.#|#.#|#.#|#.#|#.#|#.#|#.#|###" },
    { 'P', "###|#.#|#.#|#.#|###|#..|#..|#..|#..|#.." },
    { 'Q', "###|#.#|#.#|#.#|#.#|#.#|#.#|#.#|#.#|###|..#" },
    { 'R', "###|#.#|#.#|#.#|##.|#.#|#.#|#.#|#.#|#.#" },
    { 'S', "###|#..|#..|#..|###|..#|..#|..#|..#|###" },
    { 'T', "###|.#.|.#.|.#.|.#.|.#.|.#.|.#.|.#.|.#." },
    { 'U', "#.#|#.#|#.#|#.#|#.#|#.#|#.#|#.#|#.#|###" },
    { 'V', "#.#|#.#|#.#|#.#|#.#|#.#|#.#|#.#|#.#|.#." },
    { 'W', "#.#.#|#.#.#|#.#.#|#.#.#|#.#.#|#.#.#|#.#.#|#.#.#|#.#.#|#####" },
    { 'X', "#.#|#.#|#.#|#.#|.#.|.#.|#.#|#.#|#.#|#.#" },
    { 'Y', "#.#|#.#|#.#|#.#|###|.#.|.#.|.#.|.#.|.#." },
    { 'Z', "###|..#|..#|..#|.#.|.#.|#..|#..|#..|###" },
    { 'a', "|||###|..#|###|#.#|#.#|#.#|###" },
    { 'b', "#..|#..|###|#.#|#.#|#.#|#.#|#.#|#.#|###" },
    { 'c', "||###|#..|#..|#..|#..|#..|#..|###" },
    { 'd', "..#|..#|###|#.#|#.#|#.#|#.#|#.#|#.#|###" },
    { 'e', "||###|#.#|#.#|###|#..|#..|#..|###" },
    { 'f', ".##|#..|###|#..|#..|#..|#..|#..|#..|#.." },
    { 'g', "||###|#.#|#.#|#.#|#.#|#.#|#.#|###|..#|..#|###" },
    { 'h', "#..|#..|###|#.#|#.#|#.#|#.#|#.#|#.#|#.#" },
    { 'i', "#||#|#|#|#|#|#|#|#" },
    { 'j', ".#||.#|.#|.#|.#|.#|.#|.#|.#|.#|##" },
    { 'k', "#..|#..|#.#|#.#|#.#|##.|#.#|#.#|#.#|#.#" },
    { 'l', "#|#|#|#|#|#|#|#|#|#" },
    { 'm', "||#####|#.#.#|#.#.#|#.#.#|#.#.#|#.#.#|#.#.#|#.#.#" },
    { 'n', "||###|#.#|#.#|#.#|#.#|#.#|#.#|#.#" },
    { 'o', "||###|#.#|#.#|#.#|#.#|#.#|#.#|###" },
    { 'p', "||###|#.#|#.#|#.#|#.#|#.#|#.#|###|#..|#..|#.." },
    { 'q', "||###|#.#|#.#|#.#|#.#|#.#|#.#|###|..#|..#|..#" },
    { 'r', "||###|#..|#..|#..|#..|#..|#..|#.." },
    { 's', "||###|#..|#..|###|..#|..#|..#|###" },
    { 't', "#..|#..|###|#..|#..|#..|#..|#..|#..|.##" },
    { 'u', "||#.#|#.#|#.#|#.#|#.#|#.#|#.#|###" },
    { 'v', "||#.#|#.#|#.#|#.#|#.#|#.#|#.#|.#." },
    { 'w', "||#.#.#|#.#.#|#.#.#|#.#.#|#.#.#|#.#.#|#.#.#|#####" },
    { 'x', "||#.#|#.#|#.#|.#.|.#.|#.#|#.#|#.#" },
    { 'y', "||#.#|#.#|#.#|#.#|#.#|#.#|#.#|###|..#|..#|###" },
    { 'z', "||###|..#|..#|.#.|.#.|#..|#..|###" },
};

#define TAB_CELL 2
#define TAB_GAP 9 /* between letters */

static const char *tab_glyph(char c)
{
    size_t i;
    for (i = 0; i < XF_COUNT(g_tab_font); i++)
        if (g_tab_font[i].c == c)
            return g_tab_font[i].rows;
    return NULL;
}

static int tab_glyph_width(const char *rows)
{
    int w = 0, n = 0;
    for (; *rows; rows++)
        if (*rows == '|')
            n = 0;
        else
            w = XF_MAX(w, ++n);
    return w * TAB_CELL;
}

/* A word in ASCII letters: accented letters lose their accents (and
 * their case), the others stay as they are. */
static void tab_fold(char *out, size_t size, const char *utf8)
{
    size_t n = 0;
    while (*utf8 && n + 1 < size) {
        size_t len = 1;
        char one[8], folded[8];
        while (((u8)utf8[len] & 0xC0) == 0x80 && len < sizeof(one) - 1)
            len++;
        if (len == 1)
            out[n++] = *utf8;
        else {
            memcpy(one, utf8, len);
            one[len] = '\0';
            xf_fold_ascii(folded, sizeof(folded), one);
            out[n++] = folded[0] ? folded[0] : '?';
        }
        utf8 += len;
    }
    out[n] = '\0';
}

/* Whether every letter of a word has a cell picture (ASCII letters and
 * spaces after folding accents). */
static int tab_word_drawable(const char *ascii)
{
    for (; *ascii; ascii++)
        if (*ascii != ' ' && !tab_glyph(*ascii))
            return 0;
    return 1;
}

static int tab_word_width(const char *ascii)
{
    int w = 0;
    for (; *ascii; ascii++)
        w += (*ascii == ' ' ? 3 * TAB_CELL : tab_glyph_width(tab_glyph(*ascii))) + TAB_GAP;
    return w ? w - TAB_GAP : 0;
}

static void tab_cell_put(explorer *e, int x, int y, u32 color, int alpha)
{
    u32 *p, c;
    int r, g, b;
    if (x < 0 || y < 0 || x >= SCREEN_W || y >= SCREEN_H)
        return;
    p = &e->fb[y * SCREEN_W + x];
    c = *p;
    /* the letters glow on the dark picture: the lighter of the two */
    r = XF_MAX((int)(c >> 16 & 0xFF), (int)(color >> 16 & 0xFF) * alpha / 255);
    g = XF_MAX((int)(c >> 8 & 0xFF), (int)(color >> 8 & 0xFF) * alpha / 255);
    b = XF_MAX((int)(c & 0xFF), (int)(color & 0xFF) * alpha / 255);
    *p = (u32)r << 16 | (u32)g << 8 | (u32)b;
}

/* Draws a folded (ASCII) word with its baseline at y, from x. */
static void draw_tab_word(explorer *e, int x, int y, const char *ascii, u32 color)
{
    int pass;
    for (pass = 0; pass < 2; pass++) { /* a soft edge, then the strokes */
        int cx = x;
        const char *c;
        for (c = ascii; *c; c++) {
            const char *rows = tab_glyph(*c);
            int row = 0, col = 0;
            if (*c == ' ' || !rows) {
                cx += 3 * TAB_CELL + TAB_GAP;
                continue;
            }
            for (; *rows; rows++) {
                if (*rows == '|') {
                    row++, col = 0;
                    continue;
                }
                if (*rows == '#') {
                    int px0 = cx + col * TAB_CELL, py0 = y - 10 * TAB_CELL + row * TAB_CELL, dx, dy;
                    for (dy = -1 + pass; dy < TAB_CELL + 1 - pass; dy++)
                        for (dx = -1 + pass; dx < TAB_CELL + 1 - pass; dx++)
                            tab_cell_put(e, px0 + dx, py0 + dy, color, pass ? 255 : 70);
                }
                col++;
            }
            cx += tab_glyph_width(tab_glyph(*c)) + TAB_GAP;
        }
    }
}

static void draw_lang_rows(explorer *e)
{
    const xf_langs *l = &e->g->langs;
    int k, dir, hit = lang_hit(e, e->mx, e->my, &dir);
    if (options_layer(e) < 0)
        e->lang_panel = 0; /* Options closed: the next opening starts without it */
    if (!lang_tab_shown(e))
        return;
    {
        /* the tab's name, lit while its panel is open */
        const char *title = lang_row_name(e, -1);
        char ascii[64];
        u32 color = lang_panel_shown(e) ? 0x8FD0F8 : hit == 1 ? 0x4F9CD8 : 0x215473;
        tab_fold(ascii, sizeof(ascii), title);
        if (tab_word_drawable(ascii)) /* as the clip's own tab words */
            draw_tab_word(e, LANG_TAB_X1 - 4 - tab_word_width(ascii), LANG_TAB_Y1 - 4, ascii, color);
        else
            draw_spaced(e, 21, LANG_TAB_X1 - spaced_width(e, 21, title, 5), LANG_TAB_Y1 - 8, title, 5, color);
    }
    if (!lang_panel_shown(e))
        return;
    for (k = 0; k < 2; k++) {
        int idx = k ? l->text : l->voices, y = LANG_ROW_Y + 48 * k, x, w;
        const char *name = idx >= 0 ? l->langs[idx].name : "-";
        u32 grey = hit == 2 + k ? 0xFFFFFF : 0xC0C0C0;
        xf_surface sf = screen_surface(e);
        /* the control: arrows at its ends, the language between them */
        xf_text_draw(&sf, &e->font, 16, LANG_CTRL_X0, y + 14, "<", 1, hit == 2 + k && dir < 0 ? 0xFFFFFF : 0x4F94CD);
        xf_text_draw(&sf, &e->font, 16, LANG_CTRL_X1 - 10, y + 14, ">", 1, hit == 2 + k && dir > 0 ? 0xFFFFFF : 0x4F94CD);
        w = xf_text_width(&e->font, 15, name, strlen(name));
        xf_text_draw(&sf, &e->font, 15, (LANG_CTRL_X0 + LANG_CTRL_X1) / 2 - w / 2, y + 14, name, strlen(name), grey);
        x = draw_spaced(e, 14, LANG_LABEL_X, y + 13, lang_row_name(e, k), 3, grey);
        for (x += 8; x < 450; x++) /* the row's line */
            e->fb[(y + 9) * SCREEN_W + x] = 0x34495A;
    }
}

static void draw_ifaces(explorer *e)
{
    int k;
    /* Layer by layer, so a message box covers the text of the layout
     * under it. */
    for (k = 0; k < e->g->iface_count; k++)
        draw_iface_layer(e, k);
    draw_caption_option(e);
    draw_lang_rows(e);
    draw_photos(e);
}

/* --- Clips and sounds ------------------------------------------------ */

/* A player's mixer volume: its preference (0-100) times the game volume
 * (Pref_iVolumeGame). */
static void set_volume(explorer *e, const xf_player *p, u32 pref)
{
    if (p->channel >= 0)
        xf_mixer_set_volume(p->channel, (int)(game_pref(e->g, pref) * game_pref(e->g, 979) * 256 / 10000));
}

/* 0x517190 media_object_create: on the DVD edition a movie plays from
 * vob/<name>.vob when that file exists (0x4E5E30 dvd_play_title), and
 * from its QuickTime file otherwise. */
static int open_dvd_clip(explorer *e, const char *movie)
{
    char name[64], path[1024];
    const char *base = strrchr(movie, '/'), *dot;
    size_t n;
    base = base ? base + 1 : movie;
    dot = strrchr(base, '.');
    if (!dot || strcasecmp(dot, ".xmv") != 0 || (n = (size_t)(dot - base)) > 40)
        return -1;
    snprintf(name, sizeof(name), "vob/%.*s.vob", (int)n, base);
    if (resolve_vob(e, name, path, sizeof(path)) != 0)
        return -1;
    return xf_player_open_vob(&e->player, path, plat_ticks_ms(), &xf_vob_scene_view);
}

/* The next DVD title, once the queued clips have played. */
static void start_title(explorer *e)
{
    while (!e->playing && !game_media_pending(e->g) && e->title_next < e->title_count) {
        char name[64], path[1024];
        snprintf(name, sizeof(name), "vob/%s.vob", e->titles[e->title_next++]);
        if (resolve_vob(e, name, path, sizeof(path)) == 0 &&
            xf_player_open_vob(&e->player, path, plat_ticks_ms(), &xf_vob_full_view) == 0) {
            e->have_clip = e->playing = 1;
            e->card_active = 0;
            set_volume(e, &e->player, 976); /* Pref_iVolumeDialog */
            xf_info("playing DVD title %s", name);
        }
    }
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
        /* The DVD clip first, as the DVD edition does, unless QuickTime
         * comes first (the Vita: MPEG-2 is too slow there); the other
         * kind when the first is missing. */
        if (vc_read_asset_ref(&e->g->db, m.asset, &a) == 0) {
            int pass;
            for (pass = 0; pass < 2 && !e->have_clip; pass++) {
                int dvd = pass == 0 ? e->prefer_dvd : !e->prefer_dvd;
                for (j = 0; j < a.entry_count && !e->have_clip; j++) {
                    char path[1024];
                    if (dvd && open_dvd_clip(e, a.entries[j].path) == 0) {
                        e->have_clip = 1;
                        set_volume(e, &e->player, 976); /* Pref_iVolumeDialog */
                        xf_info("playing %s (DVD clip for %s)", a.logical, a.entries[j].path);
                    } else if (!dvd && resolve_media(e, a.entries[j].path, path, sizeof(path)) == 0 &&
                               xf_player_open_range(&e->player, path, plat_ticks_ms(),
                                                    a.entries[j].start + game_clip_offset(e->g, m.asset),
                                                    a.entries[j].end) == 0) {
                        e->have_clip = 1;
                        use_captions(e, &e->player, a.entries[j].path, path);
                        set_volume(e, &e->player, 976); /* Pref_iVolumeDialog */
                        xf_info("playing %s (%s)", a.logical, a.entries[j].path);
                    }
                }
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

/* A short effect (a page turn) plays on its own, over the ambience. */
static void update_effect(explorer *e)
{
    u32 now = plat_ticks_ms();
    if (e->effect_serial != e->g->effect_serial) {
        vc_asset_ref a;
        char path[1024];
        int j;
        e->effect_serial = e->g->effect_serial;
        if (e->effect_open)
            xf_player_close(&e->effect);
        e->effect_open = 0;
        if (vc_read_asset_ref(&e->g->db, e->g->effect, &a) == 0)
            for (j = 0; j < a.entry_count && !e->effect_open; j++)
                if (resolve_media(e, a.entries[j].path, path, sizeof(path)) == 0 &&
                    xf_player_open(&e->effect, path, now) == 0) {
                    e->effect_open = 1;
                    set_volume(e, &e->effect, 978); /* Pref_iVolumeEffect */
                }
        xf_info("effect %s%s", a.logical, e->effect_open ? "" : ": no file on disk");
    }
    if (e->effect_open && !xf_player_update(&e->effect, now)) {
        xf_player_close(&e->effect);
        e->effect_open = 0;
    }
}

static void stop_sound(explorer *e)
{
    if (e->sound_open)
        xf_player_close(&e->sound);
    e->sound_open = 0;
    e->sound_loop = 0; /* a stopped loop must not start again */
}

/* --- Game interface ---------------------------------------------------- */

/* The dialogue box (0x482610 loads its art, fixed in the engine): a
 * 246x120 folder with TALK and HISTORY tabs, a scroll bar and arrows.
 * 0x4810A0 centres it on the character and puts it at y 285. Blue is
 * transparent in the game's pictures. */
#define DLG_TALK 34761
#define DLG_HISTORY 34760
#define DLG_UP 34759
#define DLG_BAR 34762   /* scrollbar.pic, 12x68 */
#define DLG_THUMB 34763 /* thumb.pic, 12x10 */
/* The scroll column under the folder's close box (x 234-241, y 18-24):
 * the up arrow, the bar and the down arrow fill y 27 to 115. */
#define DLG_SCROLL_X 232 /* centred under the close box (x 234-241) */
#define DLG_UP_Y 27
#define DLG_BAR_Y 37    /* the bar, between the arrows */
#define DLG_DOWN_Y 105
#define DLG_DOWN 34764
#define DLG_Y 285
#define DLG_W 246
#define DLG_TEXT_X 12
#define DLG_TEXT_W 210
#define DLG_TEXT_TOP 32
#define DLG_TEXT_BOTTOM 114
#define DLG_LINE 15
#define DLG_PX 12 /* 0x482D00: the question lists in Courier New 12 */

/* Emotion and action icons: 80x60 animated clips below the video, each
 * with its name underneath on rollover. */
#define ICON_W 80
#define ICON_H 60
#define ICON_Y (PICTURE_Y + PICTURE_H + 13) /* the Help screen's icons, y 343 */
#define ICON_GAP 24

/* Inventory: 30x40 icons along the bottom, shown when the pointer is
 * near the bottom of the screen, with the hovered item's name under
 * them. */
#define INV_Y 428 /* the Help screen's inventory: (20, 428)-(198, 468) */
#define INV_NAME_PX 10
#define INV_W 30
#define INV_H 40
#define INV_GAP 8
#define INV_REVEAL 418

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

/* Text with a black shadow one pixel down and right, as the game draws
 * its rollover names, location card and credits. */
static void draw_shadowed(explorer *e, const xf_font *f, float px, int x, int y, const char *text, size_t len,
                          u32 color)
{
    xf_surface s = screen_surface(e);
    xf_text_draw(&s, f, px, x + 1, y + 1, text, len, 0x000000);
    xf_text_draw(&s, f, px, x, y, text, len, color);
}

/* Splits `text` into lines no wider than `w`. Returns the line count;
 * `starts` and `lens` describe each line. */
static int wrap_lines_font(const xf_font *f, const char *text, float px, int w, int *starts, int *lens, int max)
{
    return xf_text_wrap(f, px, text, w, starts, lens, max);
}

/* Where the dialogue box goes (DialogBox_place, 0x4810A0): centred on
 * the character, kept inside the clip's picture (the rectangle at
 * 0x6C0EE0), so it never reaches the screen's edges. */
static int dialog_x(explorer *e)
{
    int i, cx = SCREEN_W / 2;
    for (i = 0; i < e->g->spot_count; i++)
        if (e->g->spots[i].id == e->g->talking)
            cx = (e->g->spots[i].region.left + e->g->spots[i].region.right) / 2;
    return XF_CLAMP(cx - DLG_W / 2, PICTURE_X, PICTURE_X + PICTURE_W - DLG_W);
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
    u32 idea_icons[MAX_IDEAS], idea_convs[MAX_IDEAS];
    int ideas = game_ideas(e->g, idea_icons, idea_convs, MAX_IDEAS), j;
    /* TALK lists what has not been asked yet, but the ideas (asked with
     * their icons); HISTORY what has been asked. */
    for (i = 0; i < m; i++) {
        for (j = 0; j < ideas && idea_convs[j] != all[i]; j++)
            ;
        if ((game_times_asked(e->g, all[i]) > 0) == (e->talk_tab == 1) && j == ideas)
            ids[n++] = all[i];
    }
    *more = 0;
    e->talk_count = n;
    e->talk_scroll = XF_CLAMP(e->talk_scroll, 0, XF_MAX(n - 1, 0));
    for (i = e->talk_scroll; i < n && k < max; i++) {
        char q[256] = "";
        int starts[8], lens[8], lines;
        game_question_text(e->g, ids[i], q, sizeof(q));
        lines = wrap_lines_font(courier(e), q, DLG_PX, DLG_TEXT_W, starts, lens, 8);
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

/* A translation without pictures of its own: the tab words of the
 * dialogue folder erased and drawn again from lang.txt ("talk=",
 * "history="), in the colours of the words they replace. `active` is the
 * tab drawn in front (0 TALK, 1 HISTORY). */
/* The pixel size at which the dialogue font's capital H is `cap` rows
 * high. */
static float tab_word_px(explorer *e, int cap)
{
    xf_glyph g;
    int h = 0;
    if (cap <= 0 || xf_font_render(&e->font, xf_font_glyph_index(&e->font, 'H'), 100, &g) != 0)
        return 9;
    h = g.height;
    free(g.alpha);
    return h > 0 ? 100.0f * (float)cap / (float)h : 9;
}

static void relabel_tabs(explorer *e, xf_image *img, int active)
{
    const xf_langs *l = &e->g->langs;
    const char *word[2];
    char path[1024];
    int t;
    if (!img->pixels || l->text < 0 || !e->have_font ||
        xf_langs_resolve_in(l, l->text, "X.PFF", path, sizeof(path)) == 0)
        return; /* the language's own pictures */
    word[0] = xf_lang_value(&l->langs[l->text], "talk");
    word[1] = xf_lang_value(&l->langs[l->text], "history");
    if (!word[0] || !word[1])
        return;
    for (t = 0; t < 2; t++) {
        /* tab t spans x 100t .. 100t + 100, its word in rows 1..12: the
         * word's box from its bright pixels, the tab's colour the most
         * common one around it */
        u32 ink = t == active ? 0xFFFFFF : 0x4F8FCF, ground = 0, seen[16];
        int counts[16], kinds = 0, x, y, k, x0 = 1000, x1 = -1, y0 = 1000, y1 = -1, best = -1, w, avail;
        float px;
        xf_surface sf;
        for (y = 1; y < 13 && y < img->height; y++)
            for (x = 100 * t; x < 100 * t + 100 && x < img->width; x++) {
                u32 c = img->pixels[(size_t)y * img->width + x];
                int hi = (int)XF_MAX((c >> 16) & 0xFF, XF_MAX((c >> 8) & 0xFF, c & 0xFF));
                if (c == KEY_BLUE)
                    continue;
                if (hi >= 110) {
                    x0 = XF_MIN(x0, x);
                    x1 = XF_MAX(x1, x);
                    y0 = XF_MIN(y0, y);
                    y1 = XF_MAX(y1, y);
                    if (hi > best) {
                        best = hi;
                        ink = c;
                    }
                } else {
                    for (k = 0; k < kinds && seen[k] != c; k++)
                        ;
                    if (k == kinds && kinds < 16) {
                        seen[kinds] = c;
                        counts[kinds++] = 0;
                    }
                    if (k < kinds)
                        counts[k]++;
                }
            }
        if (x1 < 0)
            continue;
        for (k = 1, ground = seen[0]; k < kinds; k++)
            if (counts[k] > counts[0]) {
                counts[0] = counts[k];
                ground = seen[k];
            }
        for (y = 1; y < 13 && y < img->height; y++)
            for (x = x0 - 1; x <= x1 + 1 && x < img->width; x++) {
                u32 *c = &img->pixels[(size_t)y * img->width + x];
                int d = abs((int)((*c >> 16) & 0xFF) - (int)((ground >> 16) & 0xFF)) +
                        abs((int)((*c >> 8) & 0xFF) - (int)((ground >> 8) & 0xFF)) +
                        abs((int)(*c & 0xFF) - (int)(ground & 0xFF));
                if (*c != KEY_BLUE && d > 24)
                    *c = ground;
            }
        /* capitals as high as the word they replace (rows 2 to 8 of the
         * 11-row tab), so accents stay inside the tab */
        px = tab_word_px(e, y1 - y0 + 1);
        avail = x1 - x0 + 1 + 16; /* a little wider than the word it replaces */
        while (px > 6 && xf_text_width(&e->font, px, word[t], strlen(word[t])) > avail)
            px -= 0.5f;
        sf.pixels = img->pixels;
        sf.width = img->width;
        sf.height = img->height;
        sf.pitch = img->width;
        w = xf_text_width(&e->font, px, word[t], strlen(word[t]));
        xf_text_draw(&sf, &e->font, px, (x0 + x1) / 2 - w / 2, y1 + 1, word[t], strlen(word[t]), ink);
    }
}

static void ui_piece(explorer *e, int x, int y, int w, int h, int anchor);

/* --- Idea icons --------------------------------------------------------
 * VCIdeaIcon: name, rollover picture, picture, 0, label, triggers. While
 * the player talks to a character, its ideas show above the picture (the
 * Help screen's area (24, 22)-(224, 84)); a click asks about one. */
#define IDEA_X 24
#define IDEA_Y 22
#define IDEA_H 62
#define IDEA_GAP 12

static void refresh_ideas(explorer *e)
{
    u32 ids[MAX_IDEAS], convs[MAX_IDEAS];
    int n = game_ideas(e->g, ids, convs, MAX_IDEAS), i;
    if (n == e->idea_count && memcmp(ids, e->idea_ids, (size_t)n * sizeof(u32)) == 0)
        return;
    for (i = 0; i < e->idea_count; i++) {
        free(e->idea_img[i][0].pixels);
        free(e->idea_img[i][1].pixels);
    }
    for (i = 0; i < n; i++) {
        xf_neo_reader r;
        e->idea_ids[i] = ids[i];
        e->idea_convs[i] = convs[i];
        memset(e->idea_img[i], 0, sizeof(e->idea_img[i]));
        if (xf_neo_object(&e->g->db, ids[i], &r, NULL, NULL) == 0) {
            u32 over, plain;
            xf_neo_u32(&r); /* name */
            over = xf_neo_u32(&r);
            plain = xf_neo_u32(&r);
            load_list_image(e, plain, &e->idea_img[i][0]);
            load_list_image(e, over, &e->idea_img[i][1]);
        }
    }
    e->idea_count = n;
}

static int idea_x(explorer *e, int i)
{
    int x = IDEA_X, k;
    for (k = 0; k < i; k++)
        x += (e->idea_img[k][0].pixels ? e->idea_img[k][0].width : 30) + IDEA_GAP;
    return x;
}

static int idea_at(explorer *e, int x, int y)
{
    int i;
    if (!e->g->talking || e->g->iface_count || e->playing)
        return -1;
    for (i = 0; i < e->idea_count; i++) {
        const xf_image *im = &e->idea_img[i][0];
        int w = im->pixels ? im->width : 30, h = im->pixels ? im->height : IDEA_H, x0 = idea_x(e, i);
        int y0 = IDEA_Y + (IDEA_H - h) / 2;
        if (x >= x0 && x < x0 + w && y >= y0 && y < y0 + h)
            return i;
    }
    return -1;
}

static void draw_ideas(explorer *e, int mx, int my)
{
    int i, hover;
    refresh_ideas(e);
    if (!e->g->talking || e->g->iface_count || e->playing)
        return;
    hover = idea_at(e, mx, my);
    for (i = 0; i < e->idea_count; i++) {
        const xf_image *im = &e->idea_img[i][i == hover && e->idea_img[i][1].pixels ? 1 : 0];
        blit_keyed(e, im, idea_x(e, i), IDEA_Y + (IDEA_H - im->height) / 2);
    }
    if (hover >= 0 && e->have_font && game_pref(e->g, 982)) {
        char name[128];
        int w, x;
        game_object_label(e->g, e->idea_ids[hover], name, sizeof(name));
        w = xf_text_width(courier(e), ROLLOVER_PX, name, strlen(name));
        x = XF_CLAMP(idea_x(e, hover), 4, SCREEN_W - 4 - w);
        draw_shadowed(e, courier(e), ROLLOVER_PX, x, IDEA_Y + IDEA_H + 2, name, strlen(name), 0xFFFFFF);
    }
    ui_piece(e, 0, 0, SCREEN_W / 2, PICTURE_Y, SCR_ANCHOR_TOP_LEFT);
}

static void draw_dialog(explorer *e, int mx, int my)
{
    xf_surface s = screen_surface(e);
    dlg_entry ent[GAME_MAX_CHOICES];
    int x0 = dialog_x(e), n, more, i, l;
    if (!e->dlg_talk.pixels) {
        load_asset_image(e, DLG_TALK, &e->dlg_talk);
        load_asset_image(e, DLG_HISTORY, &e->dlg_history);
        relabel_tabs(e, &e->dlg_talk, 0);
        relabel_tabs(e, &e->dlg_history, 1);
    }
    if (!e->dlg_up.pixels) {
        load_asset_image(e, DLG_UP, &e->dlg_up);
        load_asset_image(e, DLG_DOWN, &e->dlg_down);
        load_asset_image(e, DLG_BAR, &e->dlg_bar);
        load_asset_image(e, DLG_THUMB, &e->dlg_thumb);
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
        /* TALK in white, blue (65, 140, 175) under the pointer; HISTORY,
         * the questions asked, in grey, lighter (205, 205, 205) under
         * the pointer */
        u32 color = e->talk_tab ? (over ? 0xCDCDCD : 0x8C8C8C) : (over ? 0x418CAF : 0xFFFFFF);
        game_question_text(e->g, ent[i].id, q, sizeof(q));
        lines = wrap_lines_font(courier(e), q, DLG_PX, DLG_TEXT_W, starts, lens, 8);
        for (l = 0; l < lines; l++)
            xf_text_draw(&s, courier(e), DLG_PX, x0 + DLG_TEXT_X, ent[i].y + l * DLG_LINE, q + starts[l],
                         (size_t)lens[l], color);
    }
    if (e->talk_scroll > 0 || more) {
        /* more questions than the box holds: the arrows, the bar and
         * its thumb at the scroll position */
        int left = e->talk_count - e->talk_scroll - n, span = e->talk_scroll + left;
        int room = (e->dlg_bar.pixels ? e->dlg_bar.height : 68) - (e->dlg_thumb.pixels ? e->dlg_thumb.height : 10);
        blit_keyed(e, &e->dlg_up, x0 + DLG_SCROLL_X, DLG_Y + DLG_UP_Y);
        blit_keyed(e, &e->dlg_bar, x0 + DLG_SCROLL_X, DLG_Y + DLG_BAR_Y);
        blit_keyed(e, &e->dlg_thumb, x0 + DLG_SCROLL_X,
                   DLG_Y + DLG_BAR_Y + (span > 0 ? room * e->talk_scroll / span : 0));
        blit_keyed(e, &e->dlg_down, x0 + DLG_SCROLL_X, DLG_Y + DLG_DOWN_Y);
    }
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
    if (x >= x0 + DLG_W - 16 && y < DLG_Y + DLG_UP_Y) { /* the close box */
        game_end_talk(e->g);
        return 1;
    }
    n = dialog_layout(e, ent, GAME_MAX_CHOICES, &more);
    if (x >= x0 + DLG_SCROLL_X - 1) { /* the scroll arrows */
        if (y < DLG_Y + (DLG_UP_Y + DLG_DOWN_Y + 10) / 2 && e->talk_scroll > 0)
            e->talk_scroll--;
        else if (y >= DLG_Y + (DLG_UP_Y + DLG_DOWN_Y + 10) / 2 && more)
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
        e->icon_action[i] = vc_class(&e->g->db, ids[i]) == VC_ACTION_ICON;
        e->icon_open[i] = 0;
        an->use_player = an->ended = 0;
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

/* As the Help screen shows them: the emotion icons from x 210 (areas
 * 207-390), the action icons on the right from x 443 (432-620). */
static int icon_x(explorer *e, int i)
{
    int k, rank = 0;
    for (k = 0; k < i; k++)
        rank += e->icon_action[k] == e->icon_action[i];
    return e->icon_action[i] ? 443 + rank * 92 : 210 + rank * 95;
}

static int icon_at(explorer *e, int x, int y);

/* Whether an icon's rollover name shows under the icons (it takes the
 * captions' place while the pointer is on the icon). */
static int icon_name_shown(explorer *e, int mx, int my)
{
    return icon_at(e, mx, my) >= 0 && game_pref(e->g, 982);
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
        update_anim(e, an, plat_ticks_ms());
        w = XF_MIN(an->video.width, ICON_W);
        h = XF_MIN(an->video.height, ICON_H);
        for (y = 0; y < h; y++)
            memcpy(&e->fb[(ICON_Y + y) * SCREEN_W + x0], an->video.frame + (size_t)y * an->video.width,
                   (size_t)w * 4);
    }
    /* Rolling over an icon shows what it does (Pref_bRolloverText). */
    if (hover >= 0 && e->have_font && game_pref(e->g, 982)) {
        char name[128];
        game_object_label(e->g, e->icon_ids[hover], name, sizeof(name));
        {
            int cx = icon_x(e, hover) + ICON_W / 2, w = xf_text_width(courier(e), ROLLOVER_PX, name, strlen(name));
            int x = XF_CLAMP(cx - w / 2, 4, SCREEN_W - 4 - w);
            draw_shadowed(e, courier(e), ROLLOVER_PX, x, ICON_Y + ICON_H + 16, name, strlen(name), 0xFFFFFF);
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
static int load_pff_image(explorer *e, const char *path, u32 index, xf_image *img);

static void refresh_inventory(explorer *e)
{
    u32 ids[MAX_INV];
    int items = game_inventory(e->g, ids, MAX_INV), i, k;
    int n;
    /* The Artificial Intuition icon is not in the bar: it sits in the
     * top-right corner, under the menu icon, as the Help screen shows. */
    e->ai_ready = 0;
    for (i = k = 0; i < items; i++)
        if (ids[i] == GAME_AI_ICON)
            e->ai_ready = 1;
        else
            ids[k++] = ids[i];
    items = k;
    if (e->ai_ready && !e->ai_frames) {
        /* VCEvidenceIcon 56185 has no still picture; its rollover list
         * names AI.pff (56700), eight frames of the spin. */
        xf_neo_reader r;
        u32 ids[4];
        vc_asset_ref a;
        char path[1024];
        if (xf_neo_object(&e->g->db, GAME_AI_ICON, &r, NULL, NULL) == 0) {
            xf_neo_u32(&r);
            xf_neo_u32(&r);
            if (vc_read_list(&e->g->db, xf_neo_u32(&r), ids, 4) > 0 && vc_read_asset_ref(&e->g->db, ids[0], &a) == 0 &&
                a.entry_count && resolve_media(e, a.entries[0].path, path, sizeof(path)) == 0) {
                u32 f;
                for (f = a.entries[0].start; f <= a.entries[0].end && e->ai_frames < 8; f++)
                    if (load_pff_image(e, path, f, &e->ai_img[e->ai_frames]) == 0)
                        e->ai_frames++;
            }
        }
        if (!e->ai_frames)
            e->ai_frames = -1; /* not found: do not try again */
    }
    n = items + game_evidence(e->g, ids + items, MAX_INV - items);
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
    /* evidence icons sit at the left of the icon band, (11, 327)-(149,
     * 376) on the Help screen */
    int row = i >= e->inv_items, col = row ? i - e->inv_items : i;
    *x = row ? 18 + col * 44 : 20 + col * (INV_W + INV_GAP);
    *y = row ? 332 : INV_Y;
}

/* The inventory shows when the pointer is low on the screen (higher
 * when a lab's evidence row is up). */
static int inv_reveal(const explorer *e)
{
    (void)e;
    return INV_REVEAL;
}

static int inv_at(explorer *e, int x, int y);

/* Whether a clip is on: the queue's (a scene, a question and its answer)
 * or a view's own clip that plays once. */
static int scene_playing(explorer *e)
{
    int i;
    if (e->playing || game_media_pending(e->g))
        return 1;
    for (i = 0; i < e->anim_count; i++)
        if (e->anims[i].use_player && !e->anims[i].loop && !e->anims[i].ended)
            return 1;
    return 0;
}

/* Whether the inventory bar can show and take clicks: not while a clip
 * plays, nor while the dialogue box is open. The evidence row (the Crime
 * Lab) stays usable while talking: the evidence goes to the technician. */
static int inv_open(explorer *e, int evidence)
{
    if (e->g->iface_count || scene_playing(e))
        return 0;
    return evidence || !e->g->talking;
}

/* An inventory or evidence icon under (x, y) that takes clicks: the bar
 * when it shows, the evidence row (the Crime Lab) always. */
static int inv_hit(explorer *e, int x, int y)
{
    int i = inv_at(e, x, y);
    if (i >= 0 && !inv_open(e, i >= e->inv_items))
        i = -1;
    if (i >= 0 && i < e->inv_items && game_pref(e->g, 983) != 'V' && y < inv_reveal(e))
        i = -1;
    return i;
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

static u32 held_cursor(explorer *e);
static int holding(explorer *e);
enum { TOOL_NONE, TOOL_FLASHLIGHT, TOOL_CAMERA, TOOL_GUN, TOOL_NIGHTVISION, TOOL_BINOCULARS };
static int tool_in_hand(explorer *e);
static void frame_rect(explorer *e, const vc_region *r, u32 color);

/* The middle of what a picture shows: some item pictures sit in the
 * left part of their 30-pixel slot, on a transparent or black ground. */
static int picture_middle(const xf_image *img)
{
    int x, y, lo = -1, hi = -1;
    for (x = 0; img->pixels && x < img->width; x++)
        for (y = 0; y < img->height; y++) {
            u32 c = img->pixels[(size_t)y * img->width + x];
            if (c != KEY_BLUE && (c >> 16 & 0xFF) + (c >> 8 & 0xFF) + (c & 0xFF) > 48) {
                if (lo < 0)
                    lo = x;
                hi = x;
                break;
            }
        }
    return lo < 0 ? INV_W / 2 : (lo + hi + 1) / 2;
}

static void draw_inventory(explorer *e, int mx, int my, int bar)
{
    xf_surface s = screen_surface(e);
    int i, x, y, hover = inv_at(e, mx, my);
    int held = holding(e) ? -1 : -2;
    for (i = 0; held == -1 && i < e->inv_count; i++)
        if (e->inv_ids[i] == e->held_inv)
            held = i;
    for (i = 0; i < e->inv_count; i++) {
        /* the item in hand stays lit; the object's first
         * picture is the lit one, its rollover picture the plain one */
        const xf_image *img = &e->inv_img[i][(i == hover || i == held) || !e->inv_img[i][1].pixels ? 0 : 1];
        if (i >= e->inv_items) /* evidence: the icon, its white rollover shape under the pointer */
            img = &e->inv_img[i][i == hover && e->inv_img[i][1].pixels ? 1 : 0];
        if (held >= 0 && i != held)
            continue; /* in hand: only that item shows */
        if (i < e->inv_items && !bar)
            continue; /* the bar is hidden: the evidence row only */
        inv_pos(e, i, &x, &y);
        blit_keyed(e, img, x, y);
    }
    if (hover >= 0 && (bar || hover >= e->inv_items) && e->have_font && game_pref(e->g, 982)) {
        char name[96];
        int w;
        game_object_label(e->g, e->inv_ids[hover], name, sizeof(name));
        w = xf_text_width(courier(e), INV_NAME_PX, name, strlen(name));
        inv_pos(e, hover, &x, &y);
        x = XF_CLAMP(x + picture_middle(&e->inv_img[hover][0]) - w / 2, 4, SCREEN_W - 4 - w);
        xf_text_draw(&s, courier(e), INV_NAME_PX, x, y + INV_H + INV_NAME_PX, name, strlen(name), 0xFFFFFF);
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
/* Each inventory item has a cursor, shown while it is in hand: the
 * cursors are named after the items (InventoryLockpick), as the GAM's
 * inventory values are (Inventory_iLockpick = 12). Pairs them by name,
 * with the few names that differ. */
static void build_inventory_cursors(explorer *e)
{
    static const char *const alias[][2] = {
        { "photom&s", "photomulderscully" }, { "videotape", "8mmtape" }, { "faxleadsphere", "fax" },
        { "faxlogtranslation", "fax" },      { "josechung", "josechungbook" }, { "travelreq", "travelreqs" },
    };
    const game *g = e->g;
    u32 i, k;
    e->inv_cursors_built = 1;
    memset(e->inv_cursor, 0, sizeof(e->inv_cursor));
    for (i = 0; i < g->interp.var_count; i++) {
        const char *v = g->interp.vars[i].name;
        i32 value = g->interp.vars[i].value;
        char want[48];
        size_t a;
        if (strncmp(v, "Inventory_i", 11) != 0 || value <= 0 || value >= (i32)XF_COUNT(e->inv_cursor))
            continue;
        xf_fold_ascii(want, sizeof(want), v + 11);
        for (a = 0; a < XF_COUNT(alias); a++)
            if (strcmp(want, alias[a][0]) == 0)
                snprintf(want, sizeof(want), "%s", alias[a][1]);
        for (k = 0; k < g->db.count && !e->inv_cursor[value]; k++) {
            xf_neo_reader r;
            char name[64], folded[64];
            if (g->db.entries[k].class_id != 0x3D || xf_neo_object(&g->db, g->db.entries[k].id, &r, NULL, NULL) != 0)
                continue;
            xf_neo_u32(&r); /* hot spot */
            name[0] = '\0';
            vc_read_name(&g->db, xf_neo_u32(&r), name, sizeof(name));
            xf_fold_ascii(folded, sizeof(folded), name);
            if (strncmp(folded, "inventory", 9) == 0 && strcmp(folded + 9, want) == 0)
                e->inv_cursor[value] = g->db.entries[k].id;
            /* the tools have no item cursor: the gun shows the gunsight
             * (VCCursor 7158, registered with gun.amv by 0x4762A0) */
            if (strcmp(want, "gun") == 0 && strcmp(folded, "gunsight") == 0)
                e->inv_cursor[value] = g->db.entries[k].id;
        }
    }
}

/* Whether something is in hand (RegCurrInvVar is not its empty value). */
static int holding(explorer *e)
{
    if (game_reg_value(e->g, "RegCurrInvVar", 0) == game_reg_value(e->g, "RegCurrInvVar", 1)) {
        e->held_inv = 0;
        return 0;
    }
    return 1;
}

/* The cursor of the item in hand, or 0. */
static u32 held_cursor(explorer *e)
{
    i32 value = game_reg_value(e->g, "RegCurrInvVar", 0);
    if (value == game_reg_value(e->g, "RegCurrInvVar", 1)) {
        e->held_inv = 0;
        return 0;
    }
    if (!e->inv_cursors_built)
        build_inventory_cursors(e);
    return value > 0 && value < (i32)XF_COUNT(e->inv_cursor) ? e->inv_cursor[value] : 0;
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
    int tool = e->in_menu == 1 ? TOOL_NONE : tool_in_hand(e);
    /* the camera's viewfinder follows the pointer in place of a cursor;
     * with a tool in hand the hotspots show no cursors of their own */
    if (tool == TOOL_CAMERA && !e->g->iface_count && my >= PICTURE_Y && my < PICTURE_Y + PICTURE_H)
        return NULL;
    /* a close-up shown during a conversation (Skinner's travel papers)
     * has its own cursors, its zoom eye included */
    if (!id && !tool && !e->playing && e->in_menu != 1 && (!e->g->talking || e->g->iface_count))
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
        draw_shadowed(e, f, CARD_PX, x, y, text + pos, len, 0xFFFFFF);
        pos = end + 1;
        y += line_h;
    }
}

/* The tools in hand (0x41AEA0 registers their assets): the flashlight's
 * beam (flashlightmask.pic, 34745), the camera's movable viewfinder
 * (34749), the night vision goggles' frame (34751) and the binoculars'
 * mask (34753); the gun fires gun.amv (34743), the camera's shutter is
 * Camera.amv (34747). */
#define TOOL_MASK_FLASHLIGHT 34745
#define TOOL_MASK_CAMERA 34749
#define TOOL_MASK_NIGHTVISION 34751
#define TOOL_MASK_BINOCULARS 34753
#define TOOL_SOUND_GUN 34743
#define TOOL_SOUND_SHUTTER 34747

static int tool_in_hand(explorer *e)
{
    static const char *const vars[] = { NULL, "Inventory_iFlashlight", "Inventory_iCamera", "Inventory_iGun",
                                        "Inventory_iNightVision", "Inventory_iBinoculars" };
    i32 hand;
    int t;
    if (!holding(e))
        return TOOL_NONE;
    hand = game_reg_value(e->g, "RegCurrInvVar", 0);
    for (t = 1; t < (int)XF_COUNT(vars); t++) {
        u32 v = vc_find_var(&e->g->interp, vars[t]);
        if (v && hand == vc_get_var(&e->g->interp, v))
            return t;
    }
    return TOOL_NONE;
}

static const xf_image *tool_mask(explorer *e, int tool)
{
    static const u32 ids[] = { 0, TOOL_MASK_FLASHLIGHT, TOOL_MASK_CAMERA, 0, TOOL_MASK_NIGHTVISION,
                               TOOL_MASK_BINOCULARS };
    xf_image *img = &e->tool_img[tool];
    if (!ids[tool])
        return NULL;
    if (!img->pixels && !e->tool_img_tried[tool]) {
        e->tool_img_tried[tool] = 1;
        load_asset_image(e, ids[tool], img);
    }
    return img->pixels ? img : NULL;
}

/* The beam picture is scaled by Pref_cFlashlightBeam (0x49EE90): inside,
 * Wide keeps it whole, Medium 0.72, Small 0.45 (0x656058, 0x656060);
 * outside, Medium and Wide 0.6, Small 0.45 (0x656068). */
static float beam_scale(explorer *e)
{
    char size = (char)game_pref(e->g, 990);
    if (e->g->flight_place == 'O')
        return size == 'S' ? 0.45f : 0.6f;
    return size == 'S' ? 0.45f : size == 'W' ? 1.0f : 0.72f;
}

/* Scales a pixel's colour by k/255. */
static u32 shade(u32 c, int k)
{
    return (u32)(((c >> 16) & 0xFF) * k / 255) << 16 | (u32)(((c >> 8) & 0xFF) * k / 255) << 8 |
           (u32)((c & 0xFF) * k / 255);
}

/* SetFLightProps mode 2 darkens the view (FLight_darken, 0x49EA10: the
 * room at alpha 0xEE); the flashlight in hand lights it through its beam
 * picture, and in a lit view still dims the rest, at 0xC8. The night
 * vision goggles turn the view green under their frame, the binoculars
 * show it through their mask, the camera its viewfinder over the
 * pointer. The green is an estimate. */
static void draw_darkness(explorer *e, int mx, int my)
{
    game *g = e->g;
    int tool = tool_in_hand(e), dark = g->flight_mode == 2, x, y;
    const xf_image *m = tool ? tool_mask(e, tool) : NULL;
    if (g->iface_count && tool != TOOL_BINOCULARS)
        tool = TOOL_NONE; /* a close-up or the computer: the tool is not over the view */
    if (dark || tool == TOOL_FLASHLIGHT) {
        int rest = dark ? 17 : 55; /* 255 - 0xEE, 255 - 0xC8 */
        float s = beam_scale(e);
        int bw = m ? (int)(m->width * s) : 0, bh = m ? (int)(m->height * s) : 0;
        for (y = PICTURE_Y; y < PICTURE_Y + PICTURE_H; y++)
            for (x = PICTURE_X; x < PICTURE_X + PICTURE_W; x++) {
                int k = rest;
                if (tool == TOOL_FLASHLIGHT && m && bw > 0 && bh > 0) {
                    int u = (x - mx + bw / 2) * m->width / bw, v = (y - my + bh / 2) * m->height / bh;
                    if (u >= 0 && v >= 0 && u < m->width && v < m->height) {
                        int l = (int)((m->pixels[(size_t)v * m->width + u] >> 8) & 0xFF);
                        k = rest + (255 - rest) * l / 255;
                    }
                }
                e->fb[y * SCREEN_W + x] = shade(e->fb[y * SCREEN_W + x], k);
            }
    }
    if (tool == TOOL_NIGHTVISION) {
        for (y = PICTURE_Y; y < PICTURE_Y + PICTURE_H; y++)
            for (x = PICTURE_X; x < PICTURE_X + PICTURE_W; x++) {
                u32 c = e->fb[y * SCREEN_W + x];
                int l = (int)((((c >> 16) & 0xFF) * 77 + ((c >> 8) & 0xFF) * 150 + (c & 0xFF) * 29) >> 8);
                e->fb[y * SCREEN_W + x] = (u32)(l / 4) << 16 | (u32)XF_MIN(255, l * 3 / 2) << 8 | (u32)(l / 4);
            }
        if (m)
            blit_keyed(e, m, PICTURE_X, PICTURE_Y);
    } else if (tool == TOOL_BINOCULARS && m) {
        int oy = (PICTURE_H - m->height) / 2; /* 275 rows over the 240 of the picture */
        for (y = PICTURE_Y; y < PICTURE_Y + PICTURE_H; y++)
            for (x = PICTURE_X; x < PICTURE_X + PICTURE_W; x++) {
                int u = (x - PICTURE_X) * m->width / PICTURE_W, v = y - PICTURE_Y - oy;
                int l = v >= 0 && v < m->height ? (int)((m->pixels[(size_t)v * m->width + u] >> 8) & 0xFF) : 0;
                e->fb[y * SCREEN_W + x] = shade(e->fb[y * SCREEN_W + x], l);
            }
    } else if (tool == TOOL_CAMERA && m) {
        /* the viewfinder moves with the pointer, inside the picture */
        int x0 = mx - m->width / 2, y0 = my - m->height / 2;
        for (y = XF_MAX(y0, PICTURE_Y); y < XF_MIN(y0 + m->height, PICTURE_Y + PICTURE_H); y++)
            for (x = XF_MAX(x0, PICTURE_X); x < XF_MIN(x0 + m->width, PICTURE_X + PICTURE_W); x++) {
                u32 c = m->pixels[(size_t)(y - y0) * m->width + (x - x0)];
                if (c != KEY_BLUE)
                    e->fb[y * SCREEN_W + x] = c;
            }
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

/* RollHdCredits: the opening credit cards in Typist (Hcd.ttr) 12 point,
 * centred at the bottom of the scene strip, 5 pixels in (rectangle
 * 0x6C4988, 0x458AA0). */
/* A subliminal word (0x4681D0): Courier New 20 in dark grey (50, the
 * preferences' byte at +0xC8), centred on the screen (0x6C48A0, the
 * whole 640x480), for a single frame. */
static void draw_subliminal(explorer *e)
{
    u32 serial;
    const char *word = game_subliminal(e->g, &serial);
    xf_surface s = screen_surface(e);
    int w;
    if (!word || serial == e->subliminal_shown || e->g->text_off || !e->have_card_font)
        return;
    e->subliminal_shown = serial;
    w = xf_text_width(&e->card_font, 20, word, strlen(word));
    xf_text_draw(&s, &e->card_font, 20, (SCREEN_W - w) / 2, SCREEN_H / 2 + 7, word, strlen(word), 0x323232);
}

static void draw_credit(explorer *e)
{
    const char *text = game_credit(e->g), *line;
    const xf_font *f = e->have_hcd_font ? &e->hcd_font : &e->font;
    const float px = 16.0f;
    int lines = 1, lh = (int)(px * 1.25f), y;
    if (!text || (!e->have_hcd_font && !e->have_font))
        return;
    for (line = text; *line; line++)
        lines += *line == '\n';
    y = PICTURE_Y + PICTURE_H - 5 - 4 - (lines - 1) * lh; /* the last baseline above the bottom */
    for (line = text; line; y += lh) {
        const char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        int w = xf_text_width(f, px, line, len);
        draw_shadowed(e, f, px, PICTURE_X + (PICTURE_W - w) / 2, y, line, len, 0xFFFFFF);
        line = nl ? nl + 1 : NULL;
    }
}

#define AI_ICON_X 567
#define AI_ICON_Y 68

static int ai_icon_at(const explorer *e, int x, int y);

/* The menu icon shows while the pointer is in the top-right corner above
 * the picture, but not on the AI icon just under it, which keeps its
 * pointer and its clicks. */
static int menu_icon_shown(const explorer *e, int mx, int my)
{
    const game *g = e->g;
    return e->in_menu == 0 && g->view && !g->iface_count && !g->slot_mode && !g->ask && my < PICTURE_Y &&
           mx >= MENU_ICON_REVEAL_X && !ai_icon_at(e, mx, my);
}

static int menu_icon_at(const explorer *e, int x, int y)
{
    return menu_icon_shown(e, x, y) && x >= MENU_ICON_X - 22 && x < MENU_ICON_X + 22 && y < MENU_ICON_Y + 22;
}

static int ai_icon_shown(const explorer *e)
{
    const game *g = e->g;
    /* only while a hint is there for the player: SetAIGlow, or an
     * action of the hint list whose condition holds */
    return e->ai_ready && e->ai_frames > 0 && e->in_menu == 0 && g->view && !g->iface_count && !g->slot_mode &&
           game_ai_glowing(e->g);
}

static int ai_icon_at(const explorer *e, int x, int y)
{
    const xf_image *im = &e->ai_img[0];
    return ai_icon_shown(e) && x >= AI_ICON_X - im->width / 2 && x < AI_ICON_X + (im->width + 1) / 2 &&
           y >= AI_ICON_Y - im->height / 2 && y < AI_ICON_Y + (im->height + 1) / 2;
}

/* The Artificial Intuition icon, top right: it shows, spinning, while
 * a hint is there, and a click shows the hint. */
static void draw_ai_icon(explorer *e, int mx, int my)
{
    const xf_image *img;
    refresh_inventory(e); /* it says whether a hint list is set */
    if (!ai_icon_shown(e))
        return;
    (void)mx;
    (void)my;
    img = &e->ai_img[(plat_ticks_ms() / 100) % (u32)e->ai_frames];
    blit_keyed(e, img, AI_ICON_X - img->width / 2, AI_ICON_Y - img->height / 2);
    ui_piece(e, AI_ICON_X - 40, AI_ICON_Y - 30, SCREEN_W - (AI_ICON_X - 40), 60, SCR_ANCHOR_TOP_RIGHT);
}

static void draw_menu_icon(explorer *e, int mx, int my)
{
    const cursor_art *c;
    const xf_image *f;
    const game *g = e->g;
    if (!menu_icon_shown(e, mx, my) || !(c = get_cursor(e, MENU_ICON))) {
        /* On wide screens the strip can reach the top: the corner stays
         * the menu icon's (a clear piece), so pointing there shows it. */
        if (e->in_menu == 0 && g->view && !g->iface_count && !g->slot_mode && !g->ask)
            ui_piece(e, MENU_ICON_REVEAL_X, 0, SCREEN_W - MENU_ICON_REVEAL_X, MENU_ICON_Y + 22,
                     SCR_ANCHOR_TOP_RIGHT);
        return;
    }
    f = &c->frames[(plat_ticks_ms() / 100) % (u32)c->count];
    blit_keyed(e, f, MENU_ICON_X - f->width / 2, MENU_ICON_Y - f->height / 2);
    ui_piece(e, MENU_ICON_X - 40, 0, SCREEN_W - (MENU_ICON_X - 40), MENU_ICON_Y + 9, SCR_ANCHOR_TOP_RIGHT);
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
        draw_credit(e);
        begin_ui(e);
        s = screen_surface(e);
        scr_reserve_bottom(&e->layout, inv_reveal(e)); /* icons stay put when the clip ends */
        draw_icons(e, mx, my);
        if (e->icon_count)
            ui_piece(e, 0, ICON_Y, SCREEN_W, ICON_H, SCR_ANCHOR_BELOW);
        if (sub && e->have_font && !icon_name_shown(e, mx, my)) {
            /* Captions under the picture; under the emotion icons when
             * they are up, so both show, but not over an icon's name. */
            int top = e->icon_count ? ICON_Y + ICON_H + 4 : PICTURE_Y + PICTURE_H + 4;
            xf_text_draw_wrapped(&s, caption_font(e), CAPTION_PX, SCREEN_W / 2, top + 12, PICTURE_W - 4, sub, 0xFFFFFF,
                                 0x000000);
            ui_piece(e, 0, top, SCREEN_W, SCREEN_H - top, SCR_ANCHOR_BELOW);
        }
        draw_ai_icon(e, mx, my);
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
        const xf_video *vd = anim_video(&e->anims[i]);
        int w, h, x0, y0;
        update_anim(e, &e->anims[i], plat_ticks_ms());
        if (!vd->frame)
            continue;
        w = XF_MIN(vd->width, PICTURE_W);
        h = XF_MIN(vd->height, PICTURE_H);
        x0 = PICTURE_X + (PICTURE_W - w) / 2;
        y0 = PICTURE_Y + (PICTURE_H - h) / 2;
        for (y = 0; y < h; y++)
            memcpy(&e->fb[(y0 + y) * SCREEN_W + x0], vd->frame + (size_t)y * vd->width, (size_t)w * 4);
    }
    draw_darkness(e, mx, my);
    draw_fade(e);
    draw_credit(e);
    draw_subliminal(e);
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
    {
        /* the captions of a view's clip that plays once */
        const char *sub = NULL;
        for (i = 0; i < e->anim_count && !sub && game_pref(g, 2573); i++) /* Pref_bClosedCaptioning */
            if (e->anims[i].use_player)
                sub = xf_player_subtitle(&e->anims[i].player);
        if (sub && e->have_font && !icon_name_shown(e, mx, my)) {
            int top = e->icon_count ? ICON_Y + ICON_H + 4 : PICTURE_Y + PICTURE_H + 4;
            xf_text_draw_wrapped(&s, caption_font(e), CAPTION_PX, SCREEN_W / 2, top + 12, PICTURE_W - 4, sub, 0xFFFFFF,
                                 0x000000);
            ui_piece(e, 0, top, SCREEN_W, SCREEN_H - top, SCR_ANCHOR_BELOW);
        }
    }
    if (g->talking) {
        int x0 = dialog_x(e);
        draw_ideas(e, mx, my);
        draw_dialog(e, mx, my);
        ui_piece(e, x0, DLG_Y - 6, DLG_W, 130, SCR_ANCHOR_UNDER);
    }
    refresh_inventory(e);
    /* Pref_cInventory; the item in hand stays in view */
    if (inv_open(e, 1)) {
        int bar = inv_open(e, 0) &&
                  (my >= inv_reveal(e) || game_pref(e->g, 983) == 'V' || (e->held_inv && holding(e)));
        if (bar || e->inv_items < e->inv_count) {
            /* the bar, with the item's name under it, and the evidence row */
            int top = e->inv_items < e->inv_count ? 328 : INV_Y - 4;
            draw_inventory(e, mx, my, bar);
            ui_piece(e, 0, top, SCREEN_W, SCREEN_H - top, SCR_ANCHOR_BOTTOM_LEFT);
        }
    }
    draw_ifaces(e);
    draw_ai_icon(e, mx, my);
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
        w = xf_text_width(&e->card_font, (float)l->size, l->text, strlen(l->text));
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

/* Whether a media reference has a file the player can open. */
static int media_on_disk(explorer *e, u32 asset)
{
    vc_asset_ref a;
    int j;
    char path[1024];
    if (vc_read_asset_ref(&e->g->db, asset, &a) != 0)
        return 0;
    for (j = 0; j < a.entry_count; j++)
        if (resolve_media(e, a.entries[j].path, path, sizeof(path)) == 0)
            return 1;
    return 0;
}

/* 0x471820 play_intro_sequence: Fox logo, HyperBole logo, teaser and
 * title, each skippable with a click. When the DVD title ddigital1
 * plays, the DVD titles ddigital1, teaser and intro replace the last
 * two. */
static void queue_intro(explorer *e)
{
    static const u32 intro[] = { 56003, 56002, 19668, 56001 };
    char path[1024];
    size_t i;
    if (getenv("XF_SKIP_INTRO") || game_pref(e->g, 994)) /* Pref_bSkipIntro */
        return;
    if (resolve_vob(e, "vob/ddigital1.vob", path, sizeof(path)) == 0 &&
        (e->prefer_dvd || !media_on_disk(e, intro[2]))) {
        e->titles[0] = "ddigital1";
        e->titles[1] = "teaser";
        e->titles[2] = "intro";
        e->title_count = 3;
        e->title_next = 0;
        game_queue_media(e->g, intro[0]);
        game_queue_media(e->g, intro[1]);
        return;
    }
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
    {
        int i;
        for (i = 0; i < e->anim_count; i++)
            if (e->anims[i].use_player)
                xf_player_pause(&e->anims[i].player, paused, now);
    }
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
    e->fade_start = 0; /* a FadeToBlack ends with its game */
    e->from_menu = 0;
}

/* Plays RIGHT.mov's sound for frames first..last: the picture and the
 * sound share the movie's time, so each rollover has its own sound. */
static void menu_right_sound(explorer *e, int first, int last)
{
    const xf_mov_track *t = e->right.track;
    u32 t0, t1;
    if (e->right_sound_open)
        xf_player_close(&e->right_sound);
    e->right_sound_open = 0;
    if (!e->right_open || !t || first < 0 || (u32)first >= t->sample_count)
        return;
    t0 = (u32)((u64)t->sample_time[first] * 1000 / t->timescale);
    t1 = (u32)((u64)((u32)last + 1 < t->sample_count ? t->sample_time[last + 1] : t->duration) * 1000 / t->timescale);
    e->right_sound_open = xf_player_open_range(&e->right_sound, e->right_path, plat_ticks_ms(), t0, t1) == 0;
}

static void menu_close(explorer *e)
{
    int i;
    if (e->right_sound_open)
        xf_player_close(&e->right_sound);
    e->right_sound_open = 0;
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
    if (game_running(e))
        pause_game_media(e, 1); /* the game waits behind the menu */
    else
        drop_game_media(e);
    e->from_menu = 0;
    /* The background animation first; the centre loop and its sound once
     * it has played (menu_update), the title's rollover on the pointer. */
    e->menu_open[MENU_LEFT] = open_asset_player(e, g_menu_parts[MENU_LEFT].asset, &e->menu[MENU_LEFT]) == 0;
    if (e->menu_seen && e->menu_open[MENU_LEFT]) {
        /* back to the menu: the background as it ends, without its sound */
        xf_player *p = &e->menu[MENU_LEFT];
        if (p->has_audio && p->channel >= 0)
            xf_mixer_close(p->channel);
        p->channel = -1;
        p->has_audio = 0;
        p->start_ticks = plat_ticks_ms() - p->duration_ms - 1000;
        xf_player_update(p, plat_ticks_ms());
    }
    {
        vc_asset_ref a;
        char path[1024];
        if (vc_read_asset_ref(&e->g->db, g_menu_parts[MENU_RIGHT].asset, &a) == 0 && a.entry_count &&
            resolve_media(e, a.entries[0].path, path, sizeof(path)) == 0 &&
            xf_mov_open(&e->right_mov, path) == 0) {
            xf_mov_track *t = xf_mov_find(&e->right_mov, XF_TRACK_VIDEO);
            e->right_open = t && xf_video_open(&e->right, &e->right_mov, t) == 0;
            snprintf(e->right_path, sizeof(e->right_path), "%s", path);
        }
        e->right_frame = e->menu_seen ? RIGHT_INTRO_END - 1 : -1; /* the labels come in once */
        e->right_target = RIGHT_INTRO_END;
        e->right_next_ms = plat_ticks_ms();
        if (!e->menu_seen)
            menu_right_sound(e, 0, RIGHT_INTRO_END - 1); /* the labels coming in */
    }
    e->menu_seen = 1;
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
        menu_right_sound(e, g_menu_rollover[item].first, g_menu_rollover[item].last);
    } else {
        e->right_frame = RIGHT_INTRO_END - 1;
        e->right_target = RIGHT_INTRO_END;
    }
    e->right_next_ms = plat_ticks_ms();
}

static void open_menu_part(explorer *e, int i)
{
    if (!e->menu_open[i])
        e->menu_open[i] = open_asset_player(e, g_menu_parts[i].asset, &e->menu[i]) == 0;
}

static void close_menu_part(explorer *e, int i)
{
    if (e->menu_open[i])
        xf_player_close(&e->menu[i]);
    e->menu_open[i] = 0;
}

/* MainMenu_hover: the title's rollover plays while the pointer is on it,
 * and stops and goes back to its start when the pointer leaves. */
static void menu_set_top_hover(explorer *e, int x, int y)
{
    int on = x >= MENU_TOP_LEFT && x < MENU_TOP_RIGHT && y >= 0 && y < MENU_TOP_BOTTOM;
    if (on)
        open_menu_part(e, MENU_TOP);
    else
        close_menu_part(e, MENU_TOP);
}

static void menu_update(explorer *e)
{
    int i;
    /* The centre loop starts once the background animation has played. */
    if (!e->menu_open[MENU_MID] &&
        (!e->menu_open[MENU_LEFT] || e->menu[MENU_LEFT].finished)) {
        open_menu_part(e, MENU_MID);
        open_menu_part(e, MENU_SOUND);
    }
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
    if (e->right_sound_open && !xf_player_update(&e->right_sound, plat_ticks_ms())) {
        xf_player_close(&e->right_sound);
        e->right_sound_open = 0;
    }
}

static void menu_draw(explorer *e)
{
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
    draw_cursor(e, e->mx, e->my); /* the game's cross, as everywhere else */
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
/* XF_PROFILE=1: the time of each stage of a frame, averaged and logged
 * every two seconds (on the Vita in ux0:data/xfiles/xfiles.log). */
enum { PROF_MEDIA, PROF_DRAW, PROF_COMPOSE, PROF_PRESENT, PROF_SLEEP, PROF_COUNT };
static const char *const g_prof_names[PROF_COUNT] = { "media", "draw", "compose", "present", "sleep" };
static struct {
    int on;
    u32 t[PROF_COUNT], frames, since;
} g_prof;

static void prof_add(int stage, u32 from) { g_prof.t[stage] += plat_ticks_ms() - from; }

static void prof_frame(void)
{
    u32 now = plat_ticks_ms();
    int i;
    g_prof.frames++;
    if (now - g_prof.since < 2000)
        return;
    if (g_prof.frames) {
        char line[256];
        int n = snprintf(line, sizeof(line), "profile: %.1f fps", g_prof.frames * 1000.0 / (now - g_prof.since));
        for (i = 0; i < PROF_COUNT; i++)
            n += snprintf(line + n, sizeof(line) - (size_t)n, ", %s %.1f ms", g_prof_names[i],
                          (double)g_prof.t[i] / g_prof.frames);
        xf_info("%s", line);
    }
    memset(g_prof.t, 0, sizeof(g_prof.t));
    g_prof.frames = 0;
    g_prof.since = now;
}

static void present_frame(explorer *e)
{
    int ow, oh;
    plat_output_size(&ow, &oh);
    while ((long)ow * oh > OUT_MAX_PIXELS) {
        ow = ow * 3 / 4;
        oh = oh * 3 / 4;
    }
    u32 t0 = plat_ticks_ms();
    scr_layout_begin(&e->layout, ow, oh, want_wide(e));
    if (e->in_menu == 1)
        menu_draw(e);
    else
        draw(e, e->mx, e->my);
    e->fb = e->scene_fb;
    prof_add(PROF_DRAW, t0);
    t0 = plat_ticks_ms();
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
    prof_add(PROF_COMPOSE, t0);
    t0 = plat_ticks_ms();
    plat_present(e->shown, e->shown_w, e->shown_h, e->shown_w);
    prof_add(PROF_PRESENT, t0);
}

/* --- Keyboard and d-pad navigation --------------------------------------
 * The arrow keys (the Vita's d-pad) move the pointer to the nearest thing
 * that takes a click in that direction; Tab cycles through them; Enter or
 * Space clicks; I brings up the inventory. Keys typed into a text field
 * (the computer's login, the ING) still go to the field. */

#define NAV_MAX 160

typedef struct {
    int x, y;
} nav_point;

static void handle_event(explorer *e, plat_event ev);

static int nav_targets(explorer *e, nav_point *t, int max)
{
    game *g = e->g;
    int n = 0, i, k;
#define NAV_ADD(px, py)                                                                                     \
    do {                                                                                                     \
        if (n < max) {                                                                                       \
            t[n].x = (px);                                                                                   \
            t[n].y = (py);                                                                                   \
            n++;                                                                                             \
        }                                                                                                    \
    } while (0)
    if (e->in_menu == 1) {
        for (i = 0; i < (int)XF_COUNT(g_menu_items); i++)
            NAV_ADD(556, (g_menu_items[i].top + g_menu_items[i].bottom) / 2);
        return n;
    }
    if (g->ask) {
        for (i = 0; i < (g->ask_action != GAME_ASK_OK ? 2 : 1); i++) {
            vc_region r = msg_button(e, i);
            NAV_ADD((r.left + r.right) / 2, (r.top + r.bottom) / 2);
        }
        return n;
    }
    if (g->iface_count) {
        /* the top layout's items that take a click */
        const game_iface *d = &g->ifaces[g->iface_count - 1];
        for (i = 0; i < d->count; i++) {
            const vc_region *r = &d->items[i].item.region;
            if (d->items[i].item.triggers && vc_is_enabled(&g->interp, d->items[i].id) &&
                (r->right - r->left < SCREEN_W || r->bottom - r->top < SCREEN_H))
                NAV_ADD((r->left + r->right) / 2, (r->top + r->bottom) / 2);
        }
        if (lang_tab_shown(e))
            NAV_ADD((LANG_TAB_X0 + LANG_TAB_X1) / 2, (LANG_TAB_Y0 + LANG_TAB_Y1) / 2);
        if (lang_panel_shown(e))
            for (i = 0; i < 2; i++)
                NAV_ADD(LANG_LABEL_X + 40, LANG_ROW_Y + 48 * i + LANG_ROW_H / 2);
        return n;
    }
    if (g->talking) {
        dlg_entry ent[GAME_MAX_CHOICES];
        int more, x0 = dialog_x(e), m = dialog_layout(e, ent, GAME_MAX_CHOICES, &more);
        for (k = 0; k < m; k++)
            NAV_ADD(x0 + DLG_TEXT_X + 40, ent[k].y - 4);
    }
    for (i = 0; i < e->icon_count; i++)
        NAV_ADD(icon_x(e, i) + ICON_W / 2, ICON_Y + ICON_H / 2);
    if (!e->playing)
        for (i = 0; i < g->spot_count; i++) {
            const vc_region *r = &g->spots[i].region;
            NAV_ADD((r->left + r->right) / 2, (r->top + r->bottom) / 2);
        }
    if ((e->my >= inv_reveal(e) || game_pref(g, 983) == 'V') && inv_open(e, 0))
        for (i = 0; i < e->inv_count; i++) {
            int x, y;
            inv_pos(e, i, &x, &y);
            NAV_ADD(x + INV_W / 2, y + INV_H / 2);
        }
    if (ai_icon_shown(e))
        NAV_ADD(AI_ICON_X, AI_ICON_Y);
#undef NAV_ADD
    return n;
}

/* Moves the pointer as the mouse would, so rollovers follow. */
static void nav_point_to(explorer *e, int x, int y)
{
    plat_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = PLAT_EV_MOUSE_MOVE;
    scr_to_output(&e->layout, x, y, &ev.x, &ev.y);
    plat_set_pointer(ev.x, ev.y); /* the Vita clicks where its pointer is */
    handle_event(e, ev);
}

/* dx, dy: a direction; 0, 0: the next target (Tab), dy -1 with dx 0 and
 * `back` set: the previous one. */
static void nav_move(explorer *e, int dx, int dy, int back)
{
    nav_point t[NAV_MAX];
    int n = nav_targets(e, t, NAV_MAX), i, best = -1;
    long best_d = 0;
    if (!n)
        return;
    if (!dx && !dy) {
        /* the target under the pointer, then the next or previous one */
        int cur = -1;
        long cur_d = 0;
        for (i = 0; i < n; i++) {
            long d = (long)(t[i].x - e->mx) * (t[i].x - e->mx) + (long)(t[i].y - e->my) * (t[i].y - e->my);
            if (cur < 0 || d < cur_d)
                cur = i, cur_d = d;
        }
        best = cur_d > 64 ? cur : (cur + (back ? n - 1 : 1)) % n;
    } else {
        for (i = 0; i < n; i++) {
            long vx = t[i].x - e->mx, vy = t[i].y - e->my;
            long along = vx * dx + vy * dy, across = labs(vx * dy) + labs(vy * dx), d;
            if (along <= 4)
                continue;
            d = along + 2 * across;
            if (best < 0 || d < best_d)
                best = i, best_d = d;
        }
    }
    if (best >= 0)
        nav_point_to(e, t[best].x, t[best].y);
}

static void nav_click(explorer *e)
{
    plat_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.button = PLAT_MOUSE_LEFT;
    ev.x = e->out_mx;
    ev.y = e->out_my;
    ev.type = PLAT_EV_MOUSE_DOWN;
    handle_event(e, ev);
    ev.type = PLAT_EV_MOUSE_UP;
    handle_event(e, ev);
}

/* Returns 1 when the key was used for navigation. */
/* Fast travel: in a view, the arrow keys take its exits by their
 * cursors: Left (284), Up the forward ones (286, 59950, 288), Right (285),
 * Down the way back (287, 4474). Returns 1 when one was taken. */
static int nav_exit(explorer *e, int key)
{
    static const u32 left[] = { 284 }, right[] = { 285 }, up[] = { 286, 59950, 288 }, down[] = { 287, 4474 };
    const u32 *want = key == PLAT_KEY_LEFT ? left : key == PLAT_KEY_RIGHT ? right : key == PLAT_KEY_UP ? up : down;
    size_t n = key == PLAT_KEY_UP ? XF_COUNT(up) : key == PLAT_KEY_DOWN ? XF_COUNT(down) : 1, k;
    game *g = e->g;
    int i;
    if (!g->view || g->iface_count || g->talking || g->ask || e->in_menu || e->playing || g->slot_mode)
        return 0;
    for (k = 0; k < n; k++)
        for (i = 0; i < g->spot_count; i++) {
            const game_spot *s = &g->spots[i];
            if (s->kind == SPOT_NAV && s->region.cursor == want[k]) {
                int x = (s->region.left + s->region.right) / 2, y = (s->region.top + s->region.bottom) / 2;
                xf_info("key: exit %u", s->id);
                nav_point_to(e, x, y);
                game_click(g, x, y);
                return 1;
            }
        }
    return 0;
}

static int nav_key(explorer *e, const plat_event *ev)
{
    const game *g = e->g;
    int typing = g->iface_count && g->focus_item; /* a text field has the focus */
    /* plain arrows take the view's exits; Ctrl+arrows (or no such exit)
     * step the pointer between the things that take a click */
    if ((ev->key == PLAT_KEY_LEFT || ev->key == PLAT_KEY_RIGHT || ev->key == PLAT_KEY_UP ||
         ev->key == PLAT_KEY_DOWN) &&
        !(ev->mods & PLAT_MOD_CTRL) && nav_exit(e, ev->key))
        return 1;
    switch (ev->key) {
    case PLAT_KEY_LEFT: nav_move(e, -1, 0, 0); return 1;
    case PLAT_KEY_RIGHT: nav_move(e, 1, 0, 0); return 1;
    case PLAT_KEY_UP: nav_move(e, 0, -1, 0); return 1;
    case PLAT_KEY_DOWN: nav_move(e, 0, 1, 0); return 1;
    }
    if (typing)
        return 0;
    if (ev->key == 9) {
        nav_move(e, 0, 0, (ev->mods & PLAT_MOD_SHIFT) != 0);
        return 1;
    }
    if ((ev->key == PLAT_KEY_RETURN && !g->ask) || ev->key == PLAT_KEY_SPACE) {
        nav_click(e);
        return 1;
    }
    if (ev->key == 'i' && inv_open(e, 0) && !e->in_menu && g->view) {
        /* the pointer to the inventory bar, which shows it */
        int x, y;
        nav_point_to(e, 10 + INV_W / 2, SCREEN_H - 20);
        refresh_inventory(e);
        if (e->inv_count) {
            inv_pos(e, 0, &x, &y);
            nav_point_to(e, x + INV_W / 2, y + INV_H / 2);
        }
        return 1;
    }
    return 0;
}

/* A click put the focus on a text field: where there is no keyboard
 * (the Vita), the system's on-screen one opens, its text replacing the
 * field's. */
static void ask_text(explorer *e, u32 clicked)
{
    game *g = e->g;
    char buf[64];
    const char *text;
    int secret;
    if (!clicked || clicked != g->focus_item || !g->iface_count)
        return;
    secret = game_item_secret(g, clicked);
    text = g->slot_mode == 'S' ? g->slot_names[g->slot] : game_item_text(g, clicked, buf, sizeof(buf));
    if (!text)
        text = "";
    plat_text_input("The X-Files", secret ? "" : text, secret, (int)strlen(text));
}

/* A game font: FONT.TTR as shipped, or FONT.TTF as the English CD
 * install names three of them (the same files). */
static int resolve_font(const explorer *e, const char *stem, char *out, size_t size)
{
    char ttr[16], ttf[16];
    const char *names[2];
    int k;
    snprintf(ttr, sizeof(ttr), "%s.TTR", stem);
    snprintf(ttf, sizeof(ttf), "%s.TTF", stem);
    names[0] = ttr;
    names[1] = ttf;
    if (xf_langs_resolve_names(&e->g->langs, names, 2, out, size) == 0)
        return 0;
    for (k = 0; k < 2; k++)
        if (xf_path_resolve(e->g->data_dir, names[k], out, size) == 0)
            return 0;
    return -1;
}

/* The game's fonts, from the text language. */
/* A font with Japanese characters, when the text language needs one: the
 * language's own (lang.txt "font=", or font.ttf / font.ttc in its
 * folder), else the system's. The Japanese release's fonts have none:
 * the original drew Japanese with a Windows font. */
static void load_cjk_font(explorer *e)
{
    const xf_langs *l = &e->g->langs;
    const xf_lang *lang = l->text >= 0 ? &l->langs[l->text] : NULL;
    char path[1024] = "";
    if (!lang || (!lang->sjis && !xf_lang_value(lang, "font"))) {
        if (e->have_cjk_font)
            xf_font_free(&e->cjk_font);
        e->have_cjk_font = 0;
        e->cjk_path[0] = '\0';
        return;
    }
    if (!(xf_lang_value(lang, "font") &&
          xf_langs_resolve_in(l, l->text, xf_lang_value(lang, "font"), path, sizeof(path)) == 0) &&
        xf_langs_resolve_in(l, l->text, "font.ttf", path, sizeof(path)) != 0 &&
        xf_langs_resolve_in(l, l->text, "font.ttc", path, sizeof(path)) != 0 &&
        plat_font_path("Japanese", path, sizeof(path)) != 0) {
        xf_warn("explore: no font with Japanese characters; put one in %s as font.ttf", lang->dir);
        return;
    }
    if (e->have_cjk_font && strcmp(path, e->cjk_path) == 0)
        return;
    if (e->have_cjk_font)
        xf_font_free(&e->cjk_font);
    e->have_cjk_font = xf_font_load(&e->cjk_font, path) == 0;
    snprintf(e->cjk_path, sizeof(e->cjk_path), "%s", path);
    xf_info("explore: Japanese characters from %s%s", path, e->have_cjk_font ? "" : " (not a TrueType font)");
}

static void load_game_fonts(explorer *e)
{
    char path[1024];
    xf_text_cache_clear();
    if (e->have_font)
        xf_font_free(&e->font);
    if (e->have_hcd_font)
        xf_font_free(&e->hcd_font);
    if (e->have_phn_font)
        xf_font_free(&e->phn_font);
    e->have_font = e->have_hcd_font = e->have_phn_font = 0;
    if (resolve_font(e, "DLG", path, sizeof(path)) == 0) {
        e->have_font = xf_font_load(&e->font, path) == 0;
        xf_debug("dialogue font %s", path);
    }
    if (resolve_font(e, "HCD", path, sizeof(path)) == 0)
        e->have_hcd_font = xf_font_load(&e->hcd_font, path) == 0;
    if (resolve_font(e, "PHN", path, sizeof(path)) == 0)
        e->have_phn_font = xf_font_load(&e->phn_font, path) == 0;
    if (!e->have_card_font && plat_font_path("Courier New", path, sizeof(path)) == 0)
        e->have_card_font = xf_font_load(&e->card_font, path) == 0;
    e->card_font.crisp = 1; /* Courier New's thin strokes, sharp as Windows drew them */
    if (!e->have_caption_font &&
        (plat_font_path("Arial", path, sizeof(path)) == 0 || plat_font_path("arial", path, sizeof(path)) == 0))
        e->have_caption_font = xf_font_load(&e->caption_font, path) == 0;
    load_cjk_font(e);
    {
        /* every font draws the characters it lacks with the Japanese one */
        const xf_font *cjk = e->have_cjk_font ? &e->cjk_font : NULL;
        e->font.fallback = e->hcd_font.fallback = e->phn_font.fallback = cjk;
        e->card_font.fallback = e->caption_font.fallback = cjk;
    }
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
    if (ev.type == PLAT_EV_KEY_DOWN && nav_key(e, &ev))
        return;
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
    else if (e->in_menu == 1 && ev.type != PLAT_EV_MOUSE_MOVE &&
             !(ev.type == PLAT_EV_MOUSE_DOWN && ev.button == PLAT_MOUSE_LEFT))
        ; /* the menu holds the game: no right click, keys or typing reach it */
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
    else if (e->in_menu == 1 && ev.type == PLAT_EV_MOUSE_MOVE) {
        e->mx = ev.x; /* the menu draws the cross there */
        e->my = ev.y;
        menu_set_hover(e, menu_item_at(ev.x, ev.y));
        menu_set_top_hover(e, ev.x, ev.y);
    }
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
    } else if (ev.type == PLAT_EV_MOUSE_DOWN && ev.button == PLAT_MOUSE_LEFT && !e->playing && ai_icon_at(e, ev.x, ev.y)) {
        game_click_inventory(e->g, GAME_AI_ICON); /* the hint */
    } else if (ev.type == PLAT_EV_MOUSE_DOWN && ev.button == PLAT_MOUSE_LEFT && menu_icon_at(e, ev.x, ev.y)) {
        menu_enter(e); /* the menu icon, a clip included: it waits */
    } else if (ev.type == PLAT_EV_MOUSE_MOVE) {
        e->mx = ev.x;
        e->my = ev.y;
        if (!e->playing) {
            int inv = inv_hit(e, ev.x, ev.y);
            int inside = ev.y >= inv_reveal(e) && !e->g->iface_count;
            if (inside && !e->inv_inside && e->held_inv) {
                game_hold(e->g, NULL); /* back to the inventory: the item goes back */
                e->held_inv = 0;
            }
            e->inv_inside = inside;
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
        int inv = inv_hit(e, ev.x, ev.y);
        const game_iface_item *it = game_iface_item_at(e->g, ev.x, ev.y);
        int dir, hit = lang_hit(e, ev.x, ev.y, &dir);
        if (hit == 1) {
            lang_tab_click(e);
        } else if (hit >= 2) {
            lang_row_click(e, hit - 2, dir);
        } else if (it) {
            if (it->id == 49749 || it->id == 49750 || it->id == 49751)
                e->lang_panel = 0; /* another tab */
            u32 clicked = it->id;
            game_click(e->g, ev.x, ev.y);
            ask_text(e, clicked);
        }
        else if (idea_at(e, ev.x, ev.y) >= 0)
            game_ask(e->g, e->idea_convs[idea_at(e, ev.x, ev.y)]); /* an idea: ask about it */
        else if (e->g->talking && click_dialog(e, ev.x, ev.y))
            ;
        else if (inv >= e->inv_items) {
            game_click_evidence(e->g, e->inv_ids[inv]);
        } else if (inv >= 0 && e->inv_ids[inv] == e->held_inv && holding(e)) {
            game_hold(e->g, NULL); /* the item in hand, clicked again: put away */
            e->held_inv = 0;
        } else if (inv >= 0) {
            game_click_inventory(e->g, e->inv_ids[inv]);
            if (e->inv_ids[inv] != GAME_AI_ICON)
                e->held_inv = e->inv_ids[inv];
        }
        else if (shoot_at(e, ev.x, ev.y))
            ;
        else {
            u32 was = e->g->talking;
            int tool = tool_in_hand(e);
            /* the gun fires and the camera's shutter clicks over the view
             * (gun.amv and Camera.amv, 0x41AEA0); a hotspot that reacts
             * to the tool runs its triggers as usual */
            if (!e->g->iface_count && ev.y >= PICTURE_Y && ev.y < PICTURE_Y + PICTURE_H &&
                (tool == TOOL_GUN || tool == TOOL_CAMERA))
                game_play_effect(e->g, tool == TOOL_GUN ? TOOL_SOUND_GUN : TOOL_SOUND_SHUTTER);
            /* the scene's hotspots take the click: their triggers check
             * what is in hand (a shot opens a picture); aiming the gun or
             * the camera does not walk away through a move */
            if (!((tool == TOOL_GUN || tool == TOOL_CAMERA) ? game_click_aiming(e->g, ev.x, ev.y)
                                                            : game_click(e->g, ev.x, ev.y)))
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
    u32 t0, start = plat_ticks_ms(), budget;
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
    t0 = plat_ticks_ms();
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
        start_title(e);
        update_sound(e);
        update_effect(e);
        update_overlay(e);
    }
    prof_add(PROF_MEDIA, t0);
    if (e->in_menu == -1 && !e->playing && !game_media_pending(e->g) && e->title_next >= e->title_count)
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
    /* What is left of the frame's time: a slow device gets no sleep. */
    t0 = plat_ticks_ms();
    budget = e->playing ? 10 : 16;
    if (t0 - start < budget)
        plat_sleep_ms(budget - (t0 - start));
    prof_add(PROF_SLEEP, t0);
    if (g_prof.on)
        prof_frame();
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
    plat_event ev; /* as the pointer moves: hovers, the inventory */
    memset(&ev, 0, sizeof(ev));
    ev.type = PLAT_EV_MOUSE_MOVE;
    ev.x = x;
    ev.y = y;
    e->mx = x;
    e->my = y;
    scr_to_output(&e->layout, x, y, &e->out_mx, &e->out_my);
    handle_event(e, ev);
}

static void script_input(void *user, int key, int x, int y)
{
    explorer *e = user;
    plat_event ev;
    memset(&ev, 0, sizeof(ev));
    if (key > 0) {
        ev.type = PLAT_EV_KEY_DOWN;
        ev.key = key;
        handle_event(e, ev);
    } else {
        int types[3] = { PLAT_EV_MOUSE_MOVE, PLAT_EV_MOUSE_DOWN, PLAT_EV_MOUSE_UP }, i;
        scr_to_output(&e->layout, x, y, &ev.x, &ev.y);
        ev.button = key < 0 ? PLAT_MOUSE_RIGHT : PLAT_MOUSE_LEFT;
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
    if (strcmp(what, "credit") == 0)
        return game_credit(e->g) != NULL;
    if (strcmp(what, "sound") == 0)
        return (int)e->g->sound;
    if (strcmp(what, "clip") == 0)
        return (int)e->g->current.asset;
    if (strcmp(what, "cursor") == 0) { /* the VCCursor under the pointer, 0 for the cross */
        const cursor_art *c = current_cursor(e, e->mx, e->my);
        return c && c != cross_cursor(e) ? (int)c->id : 0;
    }
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
    /* Random chances differ from run to run, as the original's generators
     * seeded from the clock; play scripts keep the fixed seed. */
    if (!script_path)
        e->g->rand_seed = (u32)time(NULL) ^ plat_ticks_ms();
    {
        /* XF_SCREEN=wide or original overrides the saved setting */
        const char *screen = getenv("XF_SCREEN");
        e->wide_pref = e->g->screen_wide;
        if (screen)
            e->wide_pref = strcmp(screen, "wide") == 0 || strcmp(screen, "fill") == 0;
    }
    e->disc = disc;
    load_game_fonts(e);
    if (!e->have_card_font)
        xf_warn("explore: no Courier New; location cards use the dialogue font");
    e->debug = getenv("XF_DEBUG_OVERLAY") != NULL;
    /* XF_DVD_VIDEO=0 or 1 overrides: QuickTime first on the Vita */
#ifdef __vita__
    e->prefer_dvd = 0;
#else
    e->prefer_dvd = 1;
#endif
    if (getenv("XF_DVD_VIDEO"))
        e->prefer_dvd = atoi(getenv("XF_DVD_VIDEO")) != 0;
    /* XF_PROFILE, or a file named profile.txt in the game folder (for
     * consoles without an environment) */
    g_prof.on = getenv("XF_PROFILE") != NULL || xf_path_resolve(data_dir, "profile.txt", path, sizeof(path)) == 0;
    g_prof.since = plat_ticks_ms();
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
    if (e->effect_open)
        xf_player_close(&e->effect);
    {
        int t;
        for (t = 0; t < (int)XF_COUNT(e->tool_img); t++)
            free(e->tool_img[t].pixels);
        for (t = 0; t < e->idea_count; t++) {
            free(e->idea_img[t][0].pixels);
            free(e->idea_img[t][1].pixels);
        }
    }
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
    {
        int f;
        for (f = 0; f < e->ai_frames; f++)
            free(e->ai_img[f].pixels);
    }
    free(e->dlg_history.pixels);
    free(e->dlg_up.pixels);
    free(e->dlg_down.pixels);
    free(e->dlg_bar.pixels);
    free(e->dlg_thumb.pixels);
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
    if (e->have_caption_font)
        xf_font_free(&e->caption_font);
    if (e->have_cjk_font)
        xf_font_free(&e->cjk_font);
    game_close(e->g);
    free(e->g);
    free(e);
    return result;
}
