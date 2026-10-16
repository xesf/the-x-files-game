/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* Windows string tables (RT_STRING resources) of the game's DLLs.
 * xfilest.dll holds the game's text: dialogue lines, interface labels,
 * search keywords. Media references of type 6 point into it. */
#ifndef XF_STRINGS_H
#define XF_STRINGS_H

#include "xf_types.h"

typedef struct {
    u32 count;
    u32 *ids;      /* sorted */
    char **texts;  /* UTF-8 */
} xf_strings;

/* Loads every string of a PE file. Returns 0 on success. */
int xf_strings_load(xf_strings *t, const char *path);
/* Adds or replaces strings from a text file: one "id<TAB>text" line
 * each, UTF-8, with \n, \t and \\ escapes; other lines are comments.
 * Translations override a release's tables this way. */
int xf_strings_load_text(xf_strings *t, const char *path);
void xf_strings_free(xf_strings *t);
/* The string with this ID, or NULL. */
const char *xf_strings_get(const xf_strings *t, u32 id);

#endif
