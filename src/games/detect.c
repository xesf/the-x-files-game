/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
#include "games.h"

#include "xf_file.h"
#include "xf_lang.h"

static int has(const char *dir, const char *name)
{
    char path[1024];
    return xf_path_resolve(dir, name, path, sizeof(path)) == 0;
}

xf_game_id xf_detect_game(const char *dir)
{
    static xf_langs langs; /* large for the stack of some systems */
    if (!dir)
        return XF_GAME_NONE;
    if (has(dir, "XFILES.HDB") || xf_langs_scan(&langs, dir) > 0)
        return XF_GAME_XFILES;
    if (has(dir, "VORTEX.EXE"))
        return XF_GAME_VORTEX;
    if (has(dir, "ROLAND.EX_") || has(dir, "ROLAND.EXE"))
        return XF_GAME_ROLAND;
    if (has(dir, "QGATE.EXE"))
        return XF_GAME_QGATE;
    return XF_GAME_NONE;
}

const char *xf_game_name(xf_game_id id)
{
    static const char *const names[] = { "none", "xfiles", "vortex", "roland", "qgate" };
    return (unsigned)id < XF_COUNT(names) ? names[id] : "none";
}

const char *xf_game_title(xf_game_id id)
{
    static const char *const titles[] = { "HyperBole", "The X-Files", "Vortex: Quantum Gate II",
                                          "The Madness of Roland", "Quantum Gate" };
    return (unsigned)id < XF_COUNT(titles) ? titles[id] : titles[0];
}
