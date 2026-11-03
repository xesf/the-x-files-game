/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* Story scripts (see docs/TESTING.md): one step of play or one check per
 * line, run against the game logic. */
#ifndef XF_SCRIPT_H
#define XF_SCRIPT_H

#include "games/xfiles/game.h"

/* What the runner asks of the program that plays the game. Without a
 * host, clips end as soon as they start and time passes at once. */
typedef struct {
    void *user;
    /* Lets `ms` of time pass (the window plays its clips meanwhile). */
    void (*advance)(void *user, u32 ms);
    /* Ends the clips that are queued or playing, running their triggers. */
    void (*settle)(void *user);
    /* Moves the pointer, for rollovers and cursors in screenshots. */
    void (*point)(void *user, int x, int y);
    /* Saves the screen as a BMP file. Returns 0 on success. */
    int (*shot)(void *user, const char *path);
    /* Sends an input event as the platform would: `key` is a PLAT_KEY_*
     * code or a character for key presses, 0 for a left click at (x, y)
     * in 640x480 screen positions, -1 for a right click. */
    void (*input)(void *user, int key, int x, int y);
    /* The window's own state: "menu" (1 while the main menu is up),
     * "paused" (1 while the game's clips and sounds hold), "playing" (1
     * while a clip plays), "credit" (1 while an opening credit card shows),
     * "sound" (the sound asset playing beside the clips, or 0).
     * Returns -1 for an unknown name. */
    int (*window)(void *user, const char *what);
    /* Watching (script.watch): lets the clips play to their end, then a
     * moment more, as a viewer would see them. */
    void (*rest)(void *user);
    /* Where a thing is on the screen: kind 'o' a hotspot or layout item,
     * 'q' a question in the dialogue box, 'i' an inventory item, 'c' an
     * emotion or action icon, 'e' a piece of evidence. Returns 1 if shown. */
    int (*target)(void *user, int kind, u32 id, int *x, int *y);
    /* Moves the pointer there over a moment. */
    void (*glide)(void *user, int x, int y);
    /* Shoots at a HOT target of the clip playing, when it is up, through
     * the window's click. Returns 1 if the shot was taken. */
    int (*shoot)(void *user, u32 hot);
    /* Whether the viewer asked to stop. */
    int (*cancelled)(void *user);
} script_host;

typedef struct {
    game *g;
    script_host host;
    u32 now;   /* game clock of the script, in ms */
    int keep_clips; /* `clips on`: the window plays clips across steps */
    int watch;      /* the episode: clips play to their end, the pointer
                     * moves to each thing before it is clicked, checks
                     * do not stop the run */
    int steps;
} script;

/* Runs a script file. With `quiet`, steps are not printed. Returns the
 * number of failed steps (it stops at the first one). */
int script_run(script *s, const char *path, int quiet);
/* The same for a script held in memory (no include). */
int script_run_text(script *s, const char *name, const char *text, int quiet);

#endif
