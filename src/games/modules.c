/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
#include "games.h"

#include <stddef.h>

int xf_vortex_run(const char *dir, const xf_game_options *opt);
int xf_roland_run(const char *dir, const xf_game_options *opt);
int xf_qgate_run(const char *dir, const xf_game_options *opt);

static const xf_game_module g_modules[] = {
    { XF_GAME_VORTEX, "vortex", "Vortex: Quantum Gate II", xf_vortex_run },
    { XF_GAME_ROLAND, "roland", "The Madness of Roland", xf_roland_run },
    { XF_GAME_QGATE, "qgate", "Quantum Gate", xf_qgate_run },
};

const xf_game_module *xf_game_module_for(xf_game_id id)
{
    size_t i;
    for (i = 0; i < sizeof(g_modules) / sizeof(g_modules[0]); i++)
        if (g_modules[i].id == id)
            return &g_modules[i];
    return NULL;
}
