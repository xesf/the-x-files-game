/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
#include <stdio.h>
#include <stdlib.h>

#include "xf_file.h"
#include "xf_text.h"
#include "xftool.h"

static int cmd_xtx(int argc, char **argv)
{
    size_t size;
    u8 *data;
    char *out;

    if (argc != 2) {
        fputs("usage: xftool xtx <file.xtx>\n", stderr);
        return 2;
    }
    data = xf_load(argv[1], &size);
    if (!data) {
        perror(argv[1]);
        return 1;
    }
    out = malloc(size * 3 + 1);
    if (!out) {
        free(data);
        return 1;
    }
    xf_cp1252_to_utf8(out, size * 3 + 1, data, size);
    fputs(out, stdout);
    free(out);
    free(data);
    return 0;
}

const xftool_cmd xftool_text_cmds[] = {
    { "xtx", "<file.xtx>  print a text file as UTF-8", cmd_xtx },
    { 0 }
};
