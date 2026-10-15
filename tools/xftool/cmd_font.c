/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf and contributors */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx/text.h"
#include "xftool.h"

static int cmd_font(int argc, char **argv)
{
    xf_font f;
    xf_surface s;
    float px;
    int r;

    if (argc != 5) {
        fputs("usage: xftool font <font.ttr> <pixels> <text> <out.bmp>\n", stderr);
        return 2;
    }
    if (xf_font_load(&f, argv[1]) != 0) {
        fprintf(stderr, "%s: not a supported TrueType font\n", argv[1]);
        return 1;
    }
    px = (float)atof(argv[2]);
    s.width = s.pitch = 640;
    s.height = (int)(px * 4);
    s.pixels = calloc((size_t)s.width * s.height, 4);
    if (!s.pixels)
        return 1;
    xf_text_draw_wrapped(&s, &f, px, s.width / 2, (int)(px * 1.2f), s.width - 20, argv[3], 0xFFFFFF, 0x000000);
    r = xftool_write_bmp(argv[4], s.pixels, s.width, s.height);
    printf("%d glyphs, %d units per em, ascent %d, descent %d\n", f.num_glyphs, f.units_per_em, f.ascent,
           f.descent);
    free(s.pixels);
    xf_text_cache_clear();
    xf_font_free(&f);
    return r ? 1 : 0;
}

const xftool_cmd xftool_font_cmds[] = {
    { "font", "<font.ttr> <pixels> <text> <out.bmp>  render text with a game font", cmd_font },
    { 0 }
};
