/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* The HyperBole games the engine runs, and how it tells them apart
 * (see docs/OTHER-GAMES.md). Each game is a module that owns its loop;
 * the shared layers (platform, graphics, media, audio) serve them all. */
#ifndef XF_GAMES_H
#define XF_GAMES_H

#include "xf_types.h"

typedef enum {
    XF_GAME_NONE,
    XF_GAME_XFILES,  /* Virtual Cinema 3.0, 1998 */
    XF_GAME_VORTEX,  /* Virtual Cinema 2.0, 1994 */
    XF_GAME_ROLAND,  /* Visual Basic 3, 1992 */
    XF_GAME_QGATE    /* Virtual Cinema 1.0, 1993 */
} xf_game_id;

/* Options a module gets from the command line. */
typedef struct {
    const char *script;   /* a test script to play, or NULL */
    const char *const *media; /* extra folders to search, NULL-terminated */
} xf_game_options;

typedef struct {
    xf_game_id id;
    const char *name;     /* short name: "vortex" */
    const char *title;    /* window title */
    /* Runs the game from the folder until the player quits. Returns the
     * process exit code. NULL while a module is not written yet. */
    int (*run)(const char *dir, const xf_game_options *opt);
} xf_game_module;

/* Which game a folder holds, from the files in it: XFILES.HDB (or a
 * language folder with it), VORTEX.EXE, ROLAND.EX_ or ROLAND.EXE,
 * QGATE.EXE. */
xf_game_id xf_detect_game(const char *dir);
const char *xf_game_name(xf_game_id id);
const char *xf_game_title(xf_game_id id);
/* The module for a game, or NULL (the X-Files' runs from main.c, which
 * keeps its own options). */
const xf_game_module *xf_game_module_for(xf_game_id id);

#endif
