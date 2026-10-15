/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf and contributors */
/* xftool: command-line access to the game's files. */
#ifndef XFTOOL_H
#define XFTOOL_H

typedef struct {
    const char *name;
    const char *usage;
    int (*run)(int argc, char **argv); /* argv[0] is the command name */
} xftool_cmd;

extern const xftool_cmd xftool_pff_cmds[];
extern const xftool_cmd xftool_hot_cmds[];
extern const xftool_cmd xftool_mov_cmds[];
extern const xftool_cmd xftool_text_cmds[];
extern const xftool_cmd xftool_frame_cmds[];
extern const xftool_cmd xftool_audio_cmds[];
extern const xftool_cmd xftool_subs_cmds[];
extern const xftool_cmd xftool_font_cmds[];
extern const xftool_cmd xftool_hdb_cmds[];
extern const xftool_cmd xftool_play_cmds[];

int xftool_write_file(const char *path, const void *data, unsigned long size);
int xftool_write_bmp(const char *path, const unsigned *pixels, int w, int h);

#endif
