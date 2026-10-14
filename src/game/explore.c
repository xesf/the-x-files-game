#include "explore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx/text.h"
#include "media/mov.h"
#include "media/player.h"
#include "media/video.h"
#include "platform/plat.h"
#include "vc/interp.h"
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
    u32 target;   /* view for moves, object id otherwise */
    u32 triggers; /* trigger list run on click, or 0 */
} spot;

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

typedef struct {
    const char *data_dir;
    int disc;
    xf_neo_db db;
    xf_neo_db gam;
    vc_interp interp;
    vc_place_map places;
    u32 pending_view;
    u32 view_triggers; /* trigger list of the current view */
    /* media queued by actions, played one after the other */
    u32 media_queue[32];
    int media_head, media_tail;
    xf_player player;
    int playing;
    /* location card from StartVideoText, shown over the next clip */
    char card[256];
    int card_pending, card_active;
    int card_slot;        /* media queue slot the card belongs to */
    int in_menu;
    xf_player menu[MENU_PARTS];
    int menu_open[MENU_PARTS];
    int menu_hover;
    /* RIGHT.mov is stepped by frame instead of played through */
    xf_mov right_mov;
    xf_video right;
    int right_open, right_frame, right_target;
    u32 right_next_ms;
    int playing_slot;     /* media queue slot now playing */
    u32 playing_triggers; /* trigger list of the media now playing */
    /* sounds play beside the clips, one at a time; loops repeat */
    xf_player sound;
    int sound_open, sound_loop;
    char sound_path[1024];
    u32 card_start;
    u32 card_until;       /* card shown without a clip: when it ends */
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
    /* moving media of the view (loops, clips), drawn over the still */
    view_anim anims[MAX_ANIMS];
    int anim_count;
    u32 view_media[32];   /* enabled media of the view, to notice changes */
    int view_media_count;
    /* one open navigation file */
    char nav_path[1024];
    xf_mov nav;
    int nav_open;
} explorer;

static void stop_media(explorer *e);
static void finish_media(explorer *e);
static void queue_media(explorer *e, u32 asset);
static void after_triggers(explorer *e);
static int play_sound(explorer *e, u32 asset);
static void stop_sound(explorer *e);

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

/* Loads the media of the view that are enabled: the navigation still
 * (media type bit 0x40, a frame of a .nmv file) and moving pictures
 * (a range of a movie; type bit 0x08 loops it). Uses the current disc,
 * or the first disc whose file is installed. */
static void load_picture(explorer *e, const vc_view *v)
{
    u32 ids[32];
    int n = vc_read_list(&e->db, v->assets, ids, 32), i, j, pass;

    e->have_picture = 0;
    e->view_media_count = 0;
    close_anims(e);
    snprintf(e->label, sizeof(e->label), "view %u  (no still)", e->view);
    for (i = 0; i < n; i++) {
        vc_asset_ref a;
        int done = 0;
        if (!vc_is_enabled(&e->interp, ids[i]) || vc_read_asset_ref(&e->db, ids[i], &a) != 0)
            continue;
        e->view_media[e->view_media_count++] = ids[i];
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
                        snprintf(e->label, sizeof(e->label), "view %u  disc %d  %s", e->view, m->disc,
                                 a.logical);
                    }
                } else if (xf_path_resolve(e->data_dir, m->path, path, sizeof(path)) == 0 &&
                           open_anim(e, path, m->start, m->end, (a.kind & 0x08) != 0) == 0) {
                    done = 1;
                    if (!e->have_picture)
                        snprintf(e->label, sizeof(e->label), "view %u  disc %d  %s", e->view, m->disc,
                                 a.logical);
                }
            }
        if (!done)
            xf_info("view %u: media %s not shown", e->view, a.logical);
    }
}

/* Reloads the view media when actions enabled or disabled some of them. */
static void refresh_picture(explorer *e, const vc_view *v)
{
    u32 ids[32];
    int n = vc_read_list(&e->db, v->assets, ids, 32), i, k = 0, same = 1;
    for (i = 0; i < n; i++)
        if (vc_is_enabled(&e->interp, ids[i]))
            same &= k < e->view_media_count && e->view_media[k++] == ids[i];
    if (!same || k != e->view_media_count)
        load_picture(e, v);
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
            if (!vc_is_enabled(&e->interp, ids[i]))
                continue;
            s->region = h.region;
            s->target = nav.target;
            s->triggers = nav.extra;
        } else {
            /* Explorables and character views start with a region; character
             * views have one 4-byte field before it. */
            if (!vc_is_enabled(&e->interp, ids[i]) || xf_neo_object(&e->db, ids[i], &r, NULL, NULL) != 0)
                continue;
            if (kind == SPOT_CHARACTER)
                xf_neo_u32(&r);
            s->region.cursor = xf_neo_u32(&r);
            s->region.left = (i16)xf_neo_u16(&r);
            s->region.top = (i16)xf_neo_u16(&r);
            s->region.right = (i16)xf_neo_u16(&r);
            s->region.bottom = (i16)xf_neo_u16(&r);
            s->target = ids[i];
            /* Explorables keep their trigger list right after the region. */
            s->triggers = kind == SPOT_EXPLORE ? xf_neo_u32(&r) : 0;
            if (r.error)
                continue;
        }
        s->kind = kind;
        e->spot_count++;
    }
}

static void host_go_to_view(void *user, u32 view)
{
    ((explorer *)user)->pending_view = view;
}

/* Plays a sound asset (a path under Sounds/) beside the clips. Returns
 * 0 when the asset is not a sound, so it goes to the clip queue. */
static int play_sound(explorer *e, u32 asset)
{
    vc_asset_ref a;
    int j;
    if (vc_read_asset_ref(&e->db, asset, &a) != 0 || strncmp(a.logical, "Sounds/", 7) != 0)
        return 0;
    if (e->sound_open)
        xf_player_close(&e->sound);
    e->sound_open = 0;
    for (j = 0; j < a.entry_count && !e->sound_open; j++)
        if (xf_path_resolve(e->data_dir, a.entries[j].path, e->sound_path, sizeof(e->sound_path)) == 0 &&
            xf_player_open(&e->sound, e->sound_path, plat_ticks_ms()) == 0)
            e->sound_open = 1;
    e->sound_loop = strstr(a.logical, "/Loops/") != NULL;
    xf_info("sound %s%s", a.logical, e->sound_open ? "" : ": no file on disk");
    vc_run_triggers(&e->interp, a.triggers, 8);
    return 1;
}

static void update_sound(explorer *e)
{
    u32 now = plat_ticks_ms();
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

/* Media actions name a media list; its first entry is played. */
static void host_play_media(void *user, u32 list, u8 selector)
{
    explorer *e = user;
    u32 ids[8];
    int n = vc_read_list(&e->db, list, ids, 8);
    if (n <= 0) {
        xf_info("play media list #%u: empty or missing", list);
        return;
    }
    if (!play_sound(e, ids[0]))
        queue_media(e, ids[0]);
    (void)selector;
}

/* Queues one media reference to play next. */
static void queue_media(explorer *e, u32 asset)
{
    if ((e->media_tail + 1) % 32 != e->media_head) {
        e->media_queue[e->media_tail] = asset;
        e->media_tail = (e->media_tail + 1) % 32;
    }
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
        queue_media(e, intro[i]);
}

/* Starts the next queued clip. Media can carry a trigger list: kind 8
 * runs when the media starts and kind 9 when it ends (finish_media). A
 * clip missing on disk still runs both, so the story goes on; if the
 * location card belongs to it, the card is shown alone for a while. */
static void start_next_media(explorer *e)
{
    while (!e->playing && !e->card_until && e->media_head != e->media_tail) {
        vc_asset_ref a;
        u32 id = e->media_queue[e->media_head];
        int j, card;
        e->playing_slot = e->media_head;
        e->media_head = (e->media_head + 1) % 32;
        if (vc_read_asset_ref(&e->db, id, &a) != 0)
            continue;
        e->playing_triggers = a.triggers;
        vc_run_triggers(&e->interp, a.triggers, 8);
        for (j = 0; j < a.entry_count && !e->playing; j++) {
            char path[1024];
            if (xf_path_resolve(e->data_dir, a.entries[j].path, path, sizeof(path)) != 0)
                continue;
            if (xf_player_open(&e->player, path, plat_ticks_ms()) == 0) {
                e->playing = 1;
                xf_info("playing %s (%s)", a.logical, a.entries[j].path);
            }
        }
        card = e->card_pending && e->playing_slot == e->card_slot;
        if (card) {
            e->card_pending = 0;
            e->card_active = 1;
            e->card_start = plat_ticks_ms();
        }
        if (!e->playing) {
            xf_info("media %s: no file on disk", a.logical);
            if (card) /* typed out, then held for two seconds */
                e->card_until = e->card_start + (u32)strlen(e->card) * 50 + 2000;
            else
                finish_media(e);
        }
    }
}

/* Ends the current media and runs its kind 9 triggers. */
static void finish_media(explorer *e)
{
    u32 triggers = e->playing_triggers;
    stop_media(e);
    if (triggers) {
        vc_run_triggers(&e->interp, triggers, 9);
        after_triggers(e);
    }
}

static void stop_media(explorer *e)
{
    if (e->playing)
        xf_player_close(&e->player);
    e->playing = 0;
    e->card_active = 0;
    e->card_until = 0;
    e->playing_triggers = 0;
}

/* Variables the game data registers with the engine at start-up through
 * the Reg* functions, by function name and argument index. */
typedef struct {
    char name[16];
    int argc;
    u32 vars[VC_MAX_ARGS];
} registration;

static registration g_regs[32];
static int g_reg_count;

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
static void format_location_card(explorer *e, u32 label_ref, i32 day_night, i32 hhmm, int with_time)
{
    static const int days[8] = { 29, 2, 3, 4, 5, 6, 7, 8 }; /* 0x659C80 */
    static const char *const wd[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday",
                                       "Friday", "Saturday" };
    static const char *const mon[13] = { "", "January", "February", "March", "April", "May", "June",
                                         "July", "August", "September", "October", "November",
                                         "December" };
    char label[128] = "";
    vc_asset_ref a;
    int idx = day_night > 10 ? day_night - 10 : day_night, month, day, n;

    idx = XF_CLAMP(idx, 0, 7);
    month = idx == 0 ? 3 : 4;
    day = days[idx];
    {
        xf_neo_reader r;
        if (vc_class(&e->db, label_ref) == VC_ASSET_REF && xf_neo_object(&e->db, label_ref, &r, NULL, NULL) == 0) {
            xf_neo_blob(&e->db, &r);
            vc_read_name(&e->db, xf_neo_u32(&r), label, sizeof(label));
        }
    }
    (void)a;
    n = snprintf(e->card, sizeof(e->card), "%s\n%s, %s %d, 1996", label, wd[weekday(1996, month, day)],
                 mon[month], day);
    if (with_time && n > 0 && (size_t)n < sizeof(e->card)) {
        int h = hhmm / 100, m = hhmm % 100;
        snprintf(e->card + n, sizeof(e->card) - (size_t)n, "\n%d:%02d %s", h % 12 ? h % 12 : 12, m,
                 h < 12 ? "AM" : "PM");
    }
}

static i32 arg_value(explorer *e, const vc_operand *o)
{
    return o->kind == VC_OPND_VARIABLE ? vc_get_var(&e->interp, o->value) : (i32)o->value;
}

static int host_call_function(void *user, const vc_action *a)
{
    explorer *e = user;
    const char *name = a->u.function.name;
    int i;

    /* 0x482E40 fn_StartVideoText: label media, day/night, time, show time */
    if (strcmp(name, "StartVideoText") == 0 && a->u.function.argc == 4) {
        format_location_card(e, a->u.function.args[0].value, arg_value(e, &a->u.function.args[1]),
                             arg_value(e, &a->u.function.args[2]), arg_value(e, &a->u.function.args[3]) != 0);
        e->card_pending = 1;
        e->card_slot = (e->media_tail + 31) % 32; /* the clip queued just before */
        xf_info("location card: %s", e->card);
        return 1;
    }

    /* 0x486030 fn_SetStartView: node, location, viewpoint, view */
    if (strcmp(name, "SetStartView") == 0 && a->u.function.argc == 4) {
        e->pending_view = a->u.function.args[3].value;
        return 1;
    }
    if (strncmp(name, "Reg", 3) == 0 && g_reg_count < (int)XF_COUNT(g_regs)) {
        registration *r = &g_regs[g_reg_count++];
        memcpy(r->name, name, sizeof(r->name));
        r->argc = a->u.function.argc;
        for (i = 0; i < a->u.function.argc; i++)
            r->vars[i] = a->u.function.args[i].value;
        return 1;
    }
    xf_info("function %s (not implemented)", name);
    return 1;
}

/* Runs the start-up triggers of the title object: they register the
 * engine variables and call SetStartView. Returns the start view. */
static u32 new_game(explorer *e)
{
    u32 i, title = 0;
    xf_neo_reader r;
    for (i = 0; i < e->db.count && !title; i++)
        if (e->db.entries[i].class_id == VC_TITLE)
            title = e->db.entries[i].id;
    if (!title || xf_neo_object(&e->db, title, &r, NULL, NULL) != 0)
        return 0;
    /* VCTitle: list, string, date, string, string, u32, trigger list, ... */
    xf_neo_read_list(&r);
    xf_neo_blob(&e->db, &r);
    xf_neo_u32(&r);
    xf_neo_blob(&e->db, &r);
    xf_neo_blob(&e->db, &r);
    xf_neo_u32(&r);
    e->pending_view = 0;
    vc_run_triggers(&e->interp, xf_neo_u32(&r), 8);
    xf_info("new game: %d engine registrations, start view %u", g_reg_count, e->pending_view);
    i = e->pending_view;
    e->pending_view = 0;
    return i;
}

static void build_spots(explorer *e, const vc_view *v)
{
    e->spot_count = 0;
    add_spots(e, v->navs, SPOT_NAV);
    add_spots(e, v->explorables, SPOT_EXPLORE);
    add_spots(e, v->characters, SPOT_CHARACTER);
}

static int enter_view(explorer *e, u32 id)
{
    vc_view v;
    if (vc_read_view(&e->db, id, &v) != 0) {
        xf_warn("explore: %u is not a view", id);
        return -1;
    }
    /* Kinds 8 and 9 are taken as entering and leaving (see
     * scene-graph.md). Leave from the inside out, enter from the outside in. */
    {
        const vc_place *from = e->view ? vc_place_of(&e->places, e->view) : NULL;
        const vc_place *to = vc_place_of(&e->places, id);
        if (e->view)
            vc_run_triggers(&e->interp, e->view_triggers, 9);
        if (from && (!to || to->viewpoint != from->viewpoint))
            vc_run_triggers(&e->interp, from->viewpoint_triggers, 9);
        if (from && (!to || to->location != from->location))
            vc_run_triggers(&e->interp, from->location_triggers, 9);
        if (from && (!to || to->node != from->node))
            vc_run_triggers(&e->interp, from->node_triggers, 9);
        if (to && (!from || to->node != from->node))
            vc_run_triggers(&e->interp, to->node_triggers, 8);
        if (to && (!from || to->location != from->location))
            vc_run_triggers(&e->interp, to->location_triggers, 8);
        if (to && (!from || to->viewpoint != from->viewpoint))
            vc_run_triggers(&e->interp, to->viewpoint_triggers, 8);
    }
    e->view = id;
    e->view_triggers = v.triggers;
    vc_run_triggers(&e->interp, v.triggers, 8);
    if (e->pending_view && e->pending_view != id) {
        u32 next = e->pending_view;
        e->pending_view = 0;
        return enter_view(e, next);
    }
    build_spots(e, &v);
    load_picture(e, &v);
    xf_info("view %u: %d hotspots, %s", id, e->spot_count, e->label);
    return 0;
}

/* Triggers run outside a view change (media start and end) can enable
 * objects or set a view: rebuild the hotspots, then follow the view. */
static void after_triggers(explorer *e)
{
    vc_view v;
    if (e->pending_view) {
        u32 next = e->pending_view;
        e->pending_view = 0;
        enter_view(e, next);
    } else if (e->view && vc_read_view(&e->db, e->view, &v) == 0) {
        build_spots(e, &v);
        refresh_picture(e, &v);
    }
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
    if (e->playing || e->card_until) {
        const u32 *f = e->playing ? xf_player_frame(&e->player) : NULL;
        const char *sub = e->playing ? xf_player_subtitle(&e->player) : NULL;
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
            n = XF_MIN(n, strlen(e->card));
            memcpy(shown, e->card, n);
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

static int open_asset_player(explorer *e, u32 asset, xf_player *p)
{
    vc_asset_ref a;
    int j;
    if (vc_read_asset_ref(&e->db, asset, &a) != 0)
        return -1;
    for (j = 0; j < a.entry_count; j++) {
        char path[1024];
        if (xf_path_resolve(e->data_dir, a.entries[j].path, path, sizeof(path)) == 0 &&
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
        if (vc_read_asset_ref(&e->db, g_menu_parts[MENU_RIGHT].asset, &a) == 0 && a.entry_count &&
            xf_path_resolve(e->data_dir, a.entries[0].path, path, sizeof(path)) == 0 &&
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
    {
        vc_host host = { NULL, host_go_to_view, host_play_media, host_call_function };
        host.user = e;
        if (xf_path_resolve(data_dir, "XFILES.GAM", path, sizeof(path)) != 0 ||
            xf_neo_open(&e->gam, path) != 0 || vc_interp_init(&e->interp, &e->db, &e->gam, &host) != 0) {
            xf_error("explore: cannot load XFILES.GAM in %s", data_dir);
            xf_neo_close(&e->db);
            free(e);
            return 1;
        }
        e->interp.trace = getenv("XF_TRACE") != NULL;
        vc_place_map_build(&e->db, &e->places);
        xf_info("%u views placed in the hierarchy", e->places.count);
    }
    if (xf_path_resolve(data_dir, "DLG.TTF", path, sizeof(path)) == 0 ||
        xf_path_resolve(data_dir, "DLG.TTR", path, sizeof(path)) == 0)
        e->have_font = xf_font_load(&e->font, path) == 0;
    if (start_view == 0) {
        /* Like the original: intro, then the main menu. */
        queue_intro(e);
        e->in_menu = -1; /* open the menu once the intro has played */
    } else if (enter_view(e, start_view) != 0) {
        xf_neo_close(&e->db);
        free(e);
        return 1;
    }
    while (running) {
        plat_event ev;
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
                    u32 v;
                    menu_close(e);
                    v = new_game(e);
                    if (v)
                        enter_view(e, v);
                } else if (item == 6) { /* Quit */
                    running = 0;
                }
            }
            else if (ev.type == PLAT_EV_MOUSE_MOVE) {
                mx = ev.x;
                my = ev.y;
            } else if ((e->playing || e->card_until) && (ev.type == PLAT_EV_MOUSE_DOWN || ev.type == PLAT_EV_KEY_DOWN)) {
                if (!e->card_active)
                    finish_media(e); /* skip the clip; the location card cannot be skipped */
            } else if (ev.type == PLAT_EV_MOUSE_DOWN && ev.button == PLAT_MOUSE_LEFT) {
                const spot *s = spot_at(e, ev.x, ev.y);
                if (s) {
                    u32 from = e->view, target = s->kind == SPOT_NAV ? s->target : 0;
                    xf_info("click %s %u", s->kind == SPOT_NAV ? "move to view" : "object", s->target);
                    e->pending_view = 0;
                    vc_run_triggers(&e->interp, s->triggers, 0);
                    if (e->pending_view)
                        target = e->pending_view;
                    e->pending_view = 0;
                    if (target && enter_view(e, target) == 0 && e->history_len < HISTORY)
                        e->history[e->history_len++] = from;
                }
            } else if ((ev.type == PLAT_EV_MOUSE_DOWN && ev.button == PLAT_MOUSE_RIGHT) ||
                       (ev.type == PLAT_EV_KEY_DOWN && ev.key == 8)) {
                if (e->history_len > 0)
                    enter_view(e, e->history[--e->history_len]);
            }
        }
        if ((e->playing && !xf_player_update(&e->player, plat_ticks_ms())) ||
            (e->card_until && (i32)(plat_ticks_ms() - e->card_until) >= 0))
            finish_media(e);
        start_next_media(e);
        update_sound(e);
        if (e->in_menu == -1 && !e->playing && !e->card_until && e->media_head == e->media_tail)
            menu_enter(e);
        if (e->in_menu == 1) {
            menu_update(e);
            menu_draw(e);
        } else {
            draw(e, mx, my);
        }
        plat_present(e->fb, SCREEN_W, SCREEN_H, SCREEN_W);
        plat_sleep_ms(e->playing ? 5 : 16);
    }
    stop_media(e);
    stop_sound(e);
    close_anims(e);
    menu_close(e);
    if (e->nav_open)
        xf_mov_close(&e->nav);
    if (e->have_font) {
        xf_text_cache_clear();
        xf_font_free(&e->font);
    }
    vc_place_map_free(&e->places);
    vc_interp_free(&e->interp);
    xf_neo_close(&e->gam);
    xf_neo_close(&e->db);
    free(e);
    return 0;
}
