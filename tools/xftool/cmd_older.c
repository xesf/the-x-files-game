/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* Commands for the earlier HyperBole games' files. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "data/szdd.h"
#include "games/games.h"
#include "media/bmp.h"
#include "media/midi.h"
#include "media/wav.h"
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

static int cmd_bmp(int argc, char **argv)
{
    xf_bmp b;
    int i, bad = 0;

    if (argc < 2) {
        fputs("usage: xftool bmp <file> [out.bmp]   or   xftool bmp -c <files>...\n", stderr);
        return 2;
    }
    if (strcmp(argv[1], "-c") == 0) {
        for (i = 2; i < argc; i++)
            if (xf_bmp_load(argv[i], &b) != 0) {
                printf("%s: cannot decode\n", argv[i]);
                bad++;
            } else
                xf_bmp_free(&b);
        printf("%d of %d pictures failed\n", bad, argc - 2);
        return bad ? 1 : 0;
    }
    if (xf_bmp_load(argv[1], &b) != 0) {
        fprintf(stderr, "%s: cannot decode\n", argv[1]);
        return 1;
    }
    printf("%dx%d, %d-bit, compression %c%c%c%c, %d colours\n", b.width, b.height, b.bits,
           b.compression > 16 ? (char)(b.compression & 0xFF) : '0' + (char)b.compression,
           b.compression > 16 ? (char)(b.compression >> 8 & 0xFF) : ' ',
           b.compression > 16 ? (char)(b.compression >> 16 & 0xFF) : ' ',
           b.compression > 16 ? (char)(b.compression >> 24) : ' ', b.colors);
    if (argc > 2)
        bad = xftool_write_bmp(argv[2], b.pixels, b.width, b.height) != 0;
    xf_bmp_free(&b);
    return bad;
}

static int cmd_wav(int argc, char **argv)
{
    xf_wav w;
    int i, bad = 0;

    if (argc < 2) {
        fputs("usage: xftool wav <files>...\n", stderr);
        return 2;
    }
    for (i = 1; i < argc; i++)
        if (xf_wav_load(argv[i], &w) != 0) {
            printf("%s: cannot decode\n", argv[i]);
            bad++;
        } else {
            if (argc == 2)
                printf("%u frames, %u Hz, %d channels\n", w.frames, w.rate, w.channels);
            xf_wav_free(&w);
        }
    if (argc > 2)
        printf("%d of %d sounds failed\n", bad, argc - 1);
    return bad ? 1 : 0;
}

static int cmd_midi(int argc, char **argv)
{
    xf_midi_song m;
    int i, bad = 0;

    if (argc < 2) {
        fputs("usage: xftool midi <files>...\n", stderr);
        return 2;
    }
    for (i = 1; i < argc; i++)
        if (xf_midi_load(argv[i], &m) != 0) {
            printf("%s: cannot read\n", argv[i]);
            bad++;
        } else {
            if (argc == 2)
                printf("format %d, %d tracks, %d ticks a beat, %u events, %.1f s\n", m.format, m.tracks, m.division,
                       m.count, m.length_us / 1e6);
            xf_midi_free(&m);
        }
    if (argc > 2)
        printf("%d of %d songs failed\n", bad, argc - 1);
    return bad ? 1 : 0;
}

static int cmd_detect(int argc, char **argv)
{
    int i;
    if (argc < 2) {
        fputs("usage: xftool detect <folders>...\n", stderr);
        return 2;
    }
    for (i = 1; i < argc; i++) {
        xf_game_id g = xf_detect_game(argv[i]);
        printf("%s: %s (%s)\n", argv[i], xf_game_name(g), xf_game_title(g));
    }
    return 0;
}

const xftool_cmd xftool_older_cmds[] = {
    { "detect", "<folders>...  say which HyperBole game each folder holds", cmd_detect },
    { "midi", "<files>...  read Standard MIDI files and report failures", cmd_midi },
    { "wav", "<files>...  decode WAVE sounds and report failures", cmd_wav },
    { "bmp", "<file> [out.bmp]  decode a Windows bitmap (RLE8, Video 1, SZDD); -c <files> checks them", cmd_bmp },
    { "expand", "<packed> [out]  unpack an SZDD (COMPRESS.EXE) file; -c <files> checks them", cmd_expand },
    { 0 }
};
