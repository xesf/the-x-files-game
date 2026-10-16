/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* Languages: a game folder can hold one folder per language (en, de, fr,
 * it, es, jp, pt, ...), each a whole release or only translated text.
 * The player picks the voices and the text apart: clips and voice
 * sounds come from the voice language, everything else (string tables,
 * documents, pictures, fonts, captions) from the text language. A file
 * a language lacks comes from its fallback language, then English. */
#ifndef XF_LANG_H
#define XF_LANG_H

#include "xf_types.h"

#define XF_LANG_MAX 16

typedef struct {
    char code[8];    /* the folder's name, lower case */
    char name[32];   /* in its own language */
    char dir[512];
    char fallback[8]; /* lang.txt "fallback="; "en" by default */
    int game;        /* XFILES.HDB: a whole release */
    int voices;      /* clips (XV) */
    int text;        /* string tables, documents or captions */
} xf_lang;

typedef struct {
    int count;
    xf_lang langs[XF_LANG_MAX];
    int voices, text; /* the chosen ones, or -1 */
} xf_langs;

/* Finds the language folders in `root`. Returns how many; 0 when the
 * folder is an ordinary single-language install. Chooses English for
 * both, or the first that has each. */
int xf_langs_scan(xf_langs *l, const char *root);
/* The index of a language code, or -1. */
int xf_langs_find(const xf_langs *l, const char *code);
/* A folder with the game's database: English, else the first release
 * (the releases share one database). NULL when none has it. */
const char *xf_langs_game_dir(const xf_langs *l);
/* Whether a game file is voice (XV clips, XS sounds, VOB) or text. */
int xf_langs_is_voice(const char *rel);
/* Finds a game file in the chosen language for its kind, then that
 * language's fallbacks, then English. Returns 0 when found. */
int xf_langs_resolve(const xf_langs *l, const char *rel, char *out, size_t size);
/* Finds the first of several names (the same file under other names,
 * such as DLG.TTR and DLG.TTF) along the text language's chain: each
 * language is searched for all of them before its fallback. */
int xf_langs_resolve_names(const xf_langs *l, const char *const *names, int count, char *out, size_t size);
/* Like xf_langs_resolve, but always along the text language's chain. */
int xf_langs_resolve_text(const xf_langs *l, const char *rel, char *out, size_t size);
/* Finds a file in one language's own folder only. */
int xf_langs_resolve_in(const xf_langs *l, int lang, const char *rel, char *out, size_t size);
/* Where a clip's captions come from in the text language: `rel` with
 * the extension .srt, or the text language's own copy of the clip when
 * it is another file than `clip_path`. Returns 0 and the path, or -1:
 * the clip's own captions. */
int xf_langs_captions(const xf_langs *l, const char *rel, const char *clip_path, char *out, size_t size);
/* Packs a code for the preferences file, and back. */
i32 xf_lang_pack(const char *code);
void xf_lang_unpack(i32 v, char code[8]);

#endif
