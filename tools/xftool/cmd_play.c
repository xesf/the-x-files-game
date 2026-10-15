/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* xftool play: runs a story script against the game logic, without a
 * window or media. Every clip ends as soon as it starts, so its start
 * and end triggers run in order. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/script.h"
#include "xftool.h"

static int cmd_play(int argc, char **argv)
{
    game *g;
    script s;
    int failed, i;

    setvbuf(stdout, NULL, _IOLBF, 0); /* keep the log and the steps in order */
    if (argc != 3) {
        fputs("usage: xftool play <game folder> <script>\n", stderr);
        return 2;
    }
    g = malloc(sizeof(*g));
    if (!g || game_open(g, argv[1]) != 0) {
        free(g);
        return 1;
    }
    memset(&s, 0, sizeof(s));
    s.g = g;
    failed = script_run(&s, argv[2], 0);
    printf("%d steps, %s\n", s.steps, failed ? "stopped at a failure" : "all passed");
    if (g->missing_count) {
        printf("functions not implemented yet (%d):", g->missing_count);
        for (i = 0; i < g->missing_count; i++)
            printf(" %s", g->missing[i]);
        printf("\n");
    }
    game_close(g);
    free(g);
    return failed ? 1 : 0;
}

const xftool_cmd xftool_play_cmds[] = {
    { "play", "<game folder> <script>  run the game logic from a script", cmd_play },
    { NULL, NULL, NULL },
};
