/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf and contributors */
/* Story scripts (see docs/TESTING.md): one step of play or one check per
 * line, run against the game logic. */
#ifndef XF_SCRIPT_H
#define XF_SCRIPT_H

#include "game/game.h"

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
} script_host;

typedef struct {
    game *g;
    script_host host;
    u32 now;   /* game clock of the script, in ms */
    int keep_clips; /* `clips on`: the window plays clips across steps */
    int steps;
} script;

/* Runs a script file. With `quiet`, steps are not printed. Returns the
 * number of failed steps (it stops at the first one). */
int script_run(script *s, const char *path, int quiet);

#endif
