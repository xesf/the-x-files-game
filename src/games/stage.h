/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* What the earlier games' modules share: a 640x480 screen, the game
 * folder with its DOS paths, clips, pictures, sounds, music, the pointer,
 * and a test script that drives them without a player. */
#ifndef XF_STAGE_H
#define XF_STAGE_H

#include <stdio.h>

#include "games/games.h"
#include "games/music.h"
#include "media/bmp.h"
#include "media/wav.h"
#include "platform/plat.h"

#define STAGE_W 640
#define STAGE_H 480

/* A sound longer than the mixer's buffer, fed a frame at a time. */
typedef struct {
    xf_wav wav;
    int channel;
    u32 written;
} stage_sound;

typedef struct {
    const char *dir;
    char disc_dir[1024];  /* the disc in the drive, when the discs are folders; tried first */
    u32 fb[STAGE_W * STAGE_H];
    int mx, my;           /* the pointer */
    int quit;             /* the window closed or the script ended */
    int skip;             /* the script's skip: the clips playing end now */
    xf_music music;
    stage_sound sounds[4];
    /* the test script (xf_game_options.script) */
    FILE *script;
    u32 script_wait;      /* wall clock until which the script waits */
    char script_line[256];
} xf_stage;

int stage_open(xf_stage *s, const char *dir, const xf_game_options *opt);
void stage_close(xf_stage *s);

/* A game path as the program names it ("c\splash\vclogo.mov"), found
 * whatever its case: on the disc in the drive (disc_dir), then in the
 * game folder. Returns 0, or -1. */
int stage_path(const xf_stage *s, const char *rel, char *out, size_t size);

/* The next input event, the script's when it has one; also keeps the
 * music going. Returns 1 with an event, 0 when there is none now. */
int stage_poll(xf_stage *s, plat_event *ev);
/* Shows the screen. */
void stage_present(xf_stage *s);
/* Presents and waits a frame (about 16 ms), handling quit. */
void stage_frame(xf_stage *s);

/* Plays a clip at (x, y), or centred when x < 0. A click or key skips it
 * when `skippable`. Returns 1 when skipped, 0 at its end, -1 without the
 * file. */
int stage_play_clip(xf_stage *s, const char *rel, int x, int y, int skippable);
/* Plays a sound (a QuickTime .AMV or a WAVE file) without waiting.
 * Returns the mixer channel, or -1. */
int stage_play_sound(xf_stage *s, const char *rel);
/* Draws a picture at (x, y). Returns 0, or -1. */
int stage_draw_bmp(xf_stage *s, const char *rel, int x, int y);
/* Waits for a click or a key, or `ms` (0: no limit). Returns 1 for a
 * click (its position in *x, *y), 2 for a key, 0 on timeout or quit. */
int stage_wait_input(xf_stage *s, u32 ms, int *x, int *y);
/* Starts a MIDI song, played `plays` times (0: for ever), or stops the
 * music with NULL. */
void stage_music(xf_stage *s, const char *rel, int plays);
void stage_clear(xf_stage *s, u32 color);

#endif
