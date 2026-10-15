/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* Screen layout beyond 640x480 (see docs/engine/screen.md). The game
 * draws in 640x480 as the original. In the wide layout the 600x240
 * scene strip fills the output, and the interface pieces (dialogue box,
 * icons, inventory, captions, message box) are placed around it by
 * anchors. Mouse positions map back to 640x480. */
#ifndef XF_SCREEN_H
#define XF_SCREEN_H

#include "xf_types.h"

#define SCR_W 640
#define SCR_H 480
/* The scene strip in the 640x480 screen. */
#define SCR_STRIP_X 20
#define SCR_STRIP_Y 90
#define SCR_STRIP_W 600
#define SCR_STRIP_H 240
/* An interface pixel that shows the scene through. Drawing uses
 * 0x00RRGGBB, so the top byte marks it. */
#define SCR_CLEAR 0xFF000000u
#define SCR_MAX_PIECES 16

typedef struct {
    int x, y, w, h;
} scr_rect;

/* Where an interface piece goes. Pieces scale with the interface scale
 * (the output height over 480); the strip scales to fill the width. */
enum {
    SCR_ANCHOR_UNDER,      /* under the strip, its centre following the strip:
                              the dialogue box, under the character */
    SCR_ANCHOR_BELOW,      /* under the strip, centred: icons, captions */
    SCR_ANCHOR_BOTTOM_LEFT,/* by the screen's bottom-left corner: the inventory */
    SCR_ANCHOR_TOP_LEFT,   /* by the screen's top-left corner */
    SCR_ANCHOR_TOP_RIGHT,  /* by the screen's top-right corner: the menu icon */
    SCR_ANCHOR_CENTER      /* the middle of the screen: message boxes */
};

typedef struct {
    scr_rect src;  /* in the 640x480 interface layer */
    scr_rect dst;  /* on the output */
    int anchor;
    const u32 *px; /* its own pixels (src.w x src.h), or NULL: the layer's */
} scr_piece;

typedef struct {
    int out_w, out_h;
    int wide;              /* 0: the 640x480 screen, scaled to fit */
    scr_rect strip;        /* the scene strip on the output (wide) */
    scr_rect screen;       /* the 640x480 screen on the output (not wide) */
    int ui_num, ui_den;    /* interface scale: ui_num / ui_den */
    int reserve_y;         /* output row where the inventory's band starts */
    scr_piece pieces[SCR_MAX_PIECES];
    int piece_count;
} scr_layout;

/* Starts a layout for an output; wide is used only when the output is
 * wider than 4:3. */
void scr_layout_begin(scr_layout *l, int out_w, int out_h, int wide);
/* Keeps the bottom of the screen, from the 640x480 row `y` down, for
 * the inventory: pieces under the strip stay above it. */
void scr_reserve_bottom(scr_layout *l, int y);
/* Adds an interface piece; pieces added later are on top. `px`, when not
 * NULL, holds the piece's own pixels, src.w x src.h. */
void scr_add_piece(scr_layout *l, scr_rect src, int anchor, const u32 *px);
/* Maps an output point to the 640x480 screen. */
void scr_to_screen(const scr_layout *l, int x, int y, int *sx, int *sy);
/* Maps a 640x480 point to the output (for the pointer). */
void scr_to_output(const scr_layout *l, int sx, int sy, int *x, int *y);
/* Draws the output. Not wide: `scene` is the whole 640x480 screen.
 * Wide: the strip of `scene`, then the pieces of `ui`, over `bg`. */
void scr_compose(const scr_layout *l, u32 *out, const u32 *scene, const u32 *ui, u32 bg);
/* Draws a picture scaled by the interface scale, its point (hx, hy) at
 * the output point (x, y); pixels of the colour `key` are not drawn. */
void scr_draw_scaled(const scr_layout *l, u32 *out, const u32 *px, int w, int h, int hx, int hy, int x, int y,
                     u32 key);

#endif
