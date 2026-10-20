/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* Vortex: not written yet (docs/OTHER-GAMES.md). */
#include "games/games.h"
#include "xf_log.h"

int xf_vortex_run(const char *dir, const xf_game_options *opt)
{
    (void)opt;
    xf_warn("%s: Vortex is detected but its module is not written yet", dir);
    return 1;
}
