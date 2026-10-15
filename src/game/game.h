/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf and contributors */
/* Game logic without a platform: the current view and its hotspots, the
 * triggers that run on entering, leaving, clicking and timers, the media
 * queue, interface layouts and the engine functions. The explorer draws
 * it and plays its media; xftool play drives it from a script. */
#ifndef XF_GAME_H
#define XF_GAME_H

#include "vc/interp.h"
#include "data/strings.h"
#include "vc/vc.h"

#define GAME_MAX_SPOTS 64
#define GAME_HISTORY 64
#define GAME_MAX_TIMERS 32
#define GAME_MAX_IFACES 8
#define GAME_MAX_IFACE_ITEMS 128
#define GAME_MEDIA_QUEUE 32

enum { SPOT_NAV, SPOT_EXPLORE, SPOT_CHARACTER };

typedef struct {
    int kind;
    u32 id;       /* VCNav, VCExplorable or VCCharView */
    vc_region region;
    u32 target;   /* view for moves */
    u32 triggers; /* trigger list run on click, or 0 */
} game_spot;

/* A timer started by a timer action (0x4285A0 act_timer). */
typedef struct {
    u8 id;
    u32 due, period; /* period 0: fires once */
} game_timer;

typedef struct {
    u32 id;
    vc_iface_item item;
} game_iface_item;

/* An interface layout on screen (0x44D8B0 IFaceLayoutDisplay_ctor). */
typedef struct {
    u32 layout;
    int as_view;          /* SetIFLAsView: part of the view, closed on leaving it */
    int count;
    game_iface_item items[GAME_MAX_IFACE_ITEMS];
} game_iface;

/* A media reference waiting to play; `card` marks the clip that the
 * location card of StartVideoText is shown over. */
typedef struct {
    u32 asset;
    int card;
    u32 conversation; /* runs this conversation's end triggers when it ends */
} game_media;

/* How often each conversation was asked (VCConversationHistory). */
typedef struct {
    u32 conversation;
    int count;
} game_asked;

#define GAME_MAX_ASKED 1024
#define GAME_MAX_CHOICES 16

typedef struct {
    char name[16];
    int argc;
    u32 vars[VC_MAX_ARGS];
} game_registration;

/* Text that SetIItemText puts on a layout item, with its style. */
typedef struct {
    u32 item;
    char text[160];
    char color;  /* 'b' and others, see game-ui.md */
    char font;   /* 'H' Hcd.ttr, 'c' Courier */
    int size;    /* points */
    char bold;   /* 'y' or 'n' */
    char align;  /* 'l', 'c' or 'r' */
} game_item_label;

#define GAME_MAX_LABELS 64
#define GAME_MAX_PHOTOS 24
#define GAME_MAX_EVIDENCE 32
#define GAME_MAX_EMAILS 64
#define GAME_EMAIL_ROWS 8

typedef struct {
    xf_neo_db db, gam;
    vc_interp interp;
    vc_place_map places;
    u32 now;              /* game clock in ms, set by game_tick */
    int disc;             /* node of the last SwapDisc (iCurrNode) */

    u32 view, view_triggers, pending_view;
    u32 view_epoch;       /* counts view changes, to notice nested ones */
    u32 entering;         /* view being entered, while its triggers run */
    game_spot spots[GAME_MAX_SPOTS];
    int spot_count;
    u32 history[GAME_HISTORY];
    int history_len;

    game_media queue[GAME_MEDIA_QUEUE];
    int queue_head, queue_tail;
    game_media current;   /* media playing, asset 0 when none */
    char card[256];       /* text of the last location card */

    u32 sound;            /* sound asset playing beside the clips */
    u32 sound_serial;     /* changes when a sound starts */

    game_timer timers[GAME_MAX_TIMERS];
    int timer_count;

    game_iface ifaces[GAME_MAX_IFACES];
    int iface_count;
    u32 iface_serial;     /* changes when a layout opens or closes */
    u32 iface_hover;

    u32 talking;          /* character view whose questions are offered, or 0 */
    game_asked asked[GAME_MAX_ASKED];
    int asked_count;

    char phone[24];       /* digits typed on the cell phone */
    char ing[64];         /* text typed in the ING search field */
    char login[2][32];    /* computer login: name and password */
    xf_strings strings;   /* xfilest.dll: the game's text */
    game_item_label labels[GAME_MAX_LABELS];
    int label_count;
    u32 trigger_item;     /* layout item whose triggers are running */
    u32 focus_item;       /* layout item that receives key presses */
    u32 kit_icons[GAME_MAX_EVIDENCE]; /* EnableKitIcon: icons in the kit */
    int kit_count;
    u32 rand_seed;                    /* RandGen */
    u32 fade_serial;                  /* changes on FadeToBlack */
    char data_dir[512];
    int ing_cook_shown;
    u32 overlay;                      /* PlayAssetAt: clip over the screen */
    int overlay_x, overlay_y;
    u32 overlay_serial;
    /* The mailbox: e-mails (text media) in the order they arrived. */
    struct {
        u32 asset;
        u8 read;
    } emails[GAME_MAX_EMAILS];
    int email_count, email_scroll;
    char email_mode;                  /* EmailInit: 'C' computer, 'P' PDA */
    u32 email_body_item;              /* InitEmailRead: the item showing a mail */
    char email_body[4096];
    u32 ing_notes;                    /* media of the ING record's field notes */
    int show_evidence;                /* DispEvIcons */
    u32 evidence[GAME_MAX_EVIDENCE];  /* evidence icons shown, for kinds 8/9 */
    int evidence_count;
    u32 photos[GAME_MAX_PHOTOS]; /* TakePhoto: media of each photo */
    int photo_count;
    u32 view_asset;       /* the view's clip, whose triggers ran on entry */
    u8 lbox_tint[3];      /* SetLBoxTint colour */
    int lbox_tinted;
    u32 smol_vars[3];     /* InitSmShootout: per area, set when clear */
    int smol_clear[3], smol_visits[3], smol_area;
    u32 shot_media, shot_view; /* the gun shot clip playing, and what follows */
    int flight_mode;      /* SetFLightProps: 1 day, 2 dark */
    char flight_place;    /* 'I' inside, 'O' outside */

    game_registration regs[32];
    int reg_count;
    char missing[64][16]; /* unimplemented functions seen, by name */
    int missing_count;
} game;

int game_open(game *g, const char *data_dir);
void game_close(game *g);

/* Runs the start-up triggers of the title object and enters the start
 * view. Returns 0, or -1 when the data has no start view. */
int game_new(game *g);
int game_enter_view(game *g, u32 view);
/* Goes back to the previous view. */
void game_back(game *g);

/* Click at a screen point: the top interface layout takes it when one is
 * up, otherwise the hotspot under it. Returns 1 when something ran. */
int game_click(game *g, int x, int y);
/* Clicks the hotspot or interface item with this object ID. */
int game_click_object(game *g, u32 id);
/* A key press: runs the kind 3 triggers of the layout item with focus,
 * with the key as the event value. Returns 1 when an item took it. */
int game_key(game *g, int key);
/* Text typed in a layout item, for drawing: the login name, or the
 * password as stars. NULL when the item shows no typed text. */
const char *game_item_text(game *g, u32 item, char *buf, size_t size);

/* The text of a media reference: a string of xfilest.dll (type 6), or
 * the reference's own text (kind 4, for example a password). Returns
 * buf, empty when there is none. */
const char *game_text(game *g, u32 asset, char *buf, size_t size);
/* What the dialogue box shows for a question. */
const char *game_question_text(game *g, u32 conversation, char *buf, size_t size);
/* The SetIItemText label of a layout item, or NULL. */
const game_item_label *game_item_label_of(game *g, u32 item);

/* Collected evidence (VCEvidenceIcon) that the player can hand to a lab
 * technician: empty outside the labs. */
int game_evidence(game *g, u32 *ids, int max);
/* Hands one piece of evidence over: runs its click triggers. */
int game_click_evidence(game *g, u32 id);

/* The mail being read and the item it is shown in. */
const char *game_email_body(game *g, u32 *item);

/* Gunfights: the HOT rectangle of a clip at a point of the video frame
 * and a clip time (0 for none), and a shot at one. game_shoot returns 0
 * when the shot needs the gun and it is not in hand. */
u32 game_hot_at(game *g, u32 asset, int x, int y, u32 ms);
int game_shoot(game *g, u32 hot);

/* Mouse moved: runs mouse over and off triggers of interface items. */
void game_hover(game *g, int x, int y);

/* Advances the clock and fires the timers that are due. */
void game_tick(game *g, u32 now_ms);

/* Media: game_media_start takes the next queued media and runs its start
 * triggers; the caller plays it, then calls game_media_end, which runs
 * its end triggers. game_media_start returns 0 when the queue is empty. */
int game_media_start(game *g, game_media *m);
void game_media_end(game *g);
/* The PlayAssetAt clip ended: its end triggers run. */
void game_overlay_end(game *g);
/* The sound started by the last sound action ended (loops never end). */
void game_sound_end(game *g);
int game_media_pending(const game *g);
void game_queue_media(game *g, u32 asset);

/* Conversations: clicking a character with no inventory item held
 * offers its enabled questions. game_choices lists them; game_ask asks
 * one. game_end_talk closes the choices. */
int game_choices(game *g, u32 *ids, int max);
int game_ask(game *g, u32 conversation);
void game_end_talk(game *g);
int game_times_asked(const game *g, u32 conversation);
/* The VCCursor for the screen point: the cursor of the layout item or
 * hotspot under it, else the default cursor for its kind (VCDefault-
 * Cursors: mouth for characters, eye for objects). 0 for none. */
u32 game_cursor_at(game *g, int x, int y);

/* Emotion and action icons (VCEmotionIcon, VCActionIcon): a scene
 * enables them for a while; clicking one runs its triggers. Fields:
 * name, 0, animated icon (VCAssetRefList), 0, a second list, triggers. */
typedef struct {
    u32 name, icon, triggers;
} game_icon;

int game_icons(game *g, u32 *ids, int max);
int game_read_icon(game *g, u32 id, game_icon *icon);
int game_click_icon(game *g, u32 id);

/* Inventory: the VCInventory objects the player carries (enabled ones),
 * and a click on one of them in the inventory bar. */
int game_inventory(game *g, u32 *ids, int max);
int game_click_inventory(game *g, u32 id);
/* Takes an inventory item in hand by name ("CaseFiles" for the GAM
 * constant Inventory_iCaseFiles), or empties the hand with NULL. */
int game_hold(game *g, const char *item);
/* Types a number on the cell phone and sends it (PhoneSend). Returns 1
 * when the number is in the phone directory. */
int game_dial(game *g, const char *number);
/* Searches the ING database at the office computer: type 'P' phone, 'L'
 * licence plate, 'N' name. Returns 1 when the search found a record. */
int game_ing_search(game *g, char type, char db, const char *text);
/* The value of the engine variable registered by Reg<name>, argument i. */
i32 game_reg_value(game *g, const char *name, int i);

/* The enabled media of the current view, in list order. */
int game_view_media(game *g, u32 *ids, int max);

const game_spot *game_spot_at(const game *g, int x, int y);
const game_iface_item *game_iface_item_at(game *g, int x, int y);

#endif
