/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* Commands for the earlier HyperBole games' files. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "data/szdd.h"
#include "xf_file.h"
#include "xftool.h"

static int cmd_expand(int argc, char **argv)
{
    size_t size;
    u8 *data;
    int i, bad = 0;

    if (argc < 2) {
        fputs("usage: xftool expand <packed> [out]   or   xftool expand -c <packed>...\n", stderr);
        return 2;
    }
    if (strcmp(argv[1], "-c") == 0) { /* check: unpack each, report */
        for (i = 2; i < argc; i++) {
            u8 *raw = xf_load(argv[i], &size);
            int packed = raw && xf_szdd_is(raw, size);
            free(raw);
            data = xf_szdd_load(argv[i], &size);
            if (!data) {
                printf("%s: bad\n", argv[i]);
                bad++;
            } else if (packed)
                printf("%s: %lu bytes\n", argv[i], (unsigned long)size);
            free(data);
        }
        return bad ? 1 : 0;
    }
    data = xf_szdd_load(argv[1], &size);
    if (!data) {
        fprintf(stderr, "%s: cannot unpack\n", argv[1]);
        return 1;
    }
    if (argc > 2 && xftool_write_file(argv[2], data, size) != 0)
        bad = 1;
    else if (argc <= 2)
        printf("%lu bytes\n", (unsigned long)size);
    free(data);
    return bad;
}

const xftool_cmd xftool_older_cmds[] = {
    { "expand", "<packed> [out]  unpack an SZDD (COMPRESS.EXE) file; -c <files> checks them", cmd_expand },
    { 0 }
};
