/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* Quantum Gate: not written yet (docs/OTHER-GAMES.md). */
#include "games/games.h"
#include "xf_log.h"

int xf_qgate_run(const char *dir, const xf_game_options *opt)
{
    (void)opt;
    xf_warn("%s: Quantum Gate is detected but its module is not written yet", dir);
    return 1;
}
