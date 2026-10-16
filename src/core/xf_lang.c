/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
#include "xf_lang.h"

#ifdef __WATCOMC__
#include <direct.h>
#else
#include <dirent.h>
#include <strings.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xf_file.h"

static const struct {
    const char *code, *name;
} g_names[] = {
    { "en", "English" }, { "de", "Deutsch" },  { "fr", "Fran\xC3\xA7" "ais" }, { "it", "Italiano" },
    { "es", "Espa\xC3\xB1ol" }, { "jp", "Nihongo" }, { "ja", "Nihongo" }, { "pt", "Portugu\xC3\xAAs" },
};

static int has(const char *dir, const char *rel)
{
    char path[1024];
    return xf_path_resolve(dir, rel, path, sizeof(path)) == 0;
}

/* lang.txt: "name=...", "fallback=..." lines, UTF-8. */
static void read_lang_txt(xf_lang *g)
{
    char path[1024], line[128];
    FILE *f;
    if (xf_path_resolve(g->dir, "lang.txt", path, sizeof(path)) != 0 || !(f = fopen(path, "r")))
        return;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "name=", 5) == 0)
            snprintf(g->name, sizeof(g->name), "%s", line + 5);
        else if (strncmp(line, "fallback=", 9) == 0)
            snprintf(g->fallback, sizeof(g->fallback), "%s", line + 9);
    }
    fclose(f);
}

static int add_lang(xf_langs *l, const char *root, const char *name)
{
    xf_lang *g = &l->langs[l->count];
    size_t i, n = strlen(name);
    if (n < 2 || n > 5 || l->count >= XF_LANG_MAX)
        return 0;
    for (i = 0; i < n; i++)
        if (!((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= 'A' && name[i] <= 'Z') || name[i] == '-' ||
              name[i] == '_'))
            return 0;
    memset(g, 0, sizeof(*g));
    snprintf(g->dir, sizeof(g->dir), "%s/%s", root, name);
    for (i = 0; i < n; i++)
        g->code[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] - 'A' + 'a' : name[i]);
    g->game = has(g->dir, "XFILES.HDB");
    g->voices = has(g->dir, "XV");
    g->text = has(g->dir, "XFILEST.DLL") || has(g->dir, "xfilest.txt") || has(g->dir, "XT") || has(g->dir, "lang.txt");
    if (!g->game && !g->voices && !g->text)
        return 0;
    /* Japanese documents are Shift-JIS and its strings need the
     * Japanese fonts: not drawn yet, so only its voices are offered. */
    if (strcmp(g->code, "jp") == 0 || strcmp(g->code, "ja") == 0)
        g->text = 0;
    snprintf(g->name, sizeof(g->name), "%s", g->code);
    for (i = 0; i < XF_COUNT(g_names); i++)
        if (strcmp(g_names[i].code, g->code) == 0)
            snprintf(g->name, sizeof(g->name), "%s", g_names[i].name);
    snprintf(g->fallback, sizeof(g->fallback), "en");
    read_lang_txt(g);
    l->count++;
    return 1;
}

static int cmp_lang(const void *a, const void *b)
{
    return strcmp(((const xf_lang *)a)->code, ((const xf_lang *)b)->code);
}

int xf_langs_scan(xf_langs *l, const char *root)
{
    DIR *dir;
    struct dirent *de;
    int i;
    memset(l, 0, sizeof(*l));
    l->voices = l->text = -1;
    if (has(root, "XFILES.HDB") || !(dir = opendir(root)))
        return 0; /* an ordinary install */
    while ((de = readdir(dir)) != NULL)
        if (de->d_name[0] != '.')
            add_lang(l, root, de->d_name);
    closedir(dir);
    qsort(l->langs, (size_t)l->count, sizeof(l->langs[0]), cmp_lang);
    for (i = 0; i < l->count; i++)
        if (l->langs[i].game)
            break;
    if (i == l->count) {
        l->count = 0; /* no release among them: no game here */
        return 0;
    }
    i = xf_langs_find(l, "en");
    l->voices = i >= 0 && l->langs[i].voices ? i : -1;
    l->text = i >= 0 && l->langs[i].text ? i : -1;
    for (i = 0; i < l->count; i++) {
        if (l->voices < 0 && l->langs[i].voices)
            l->voices = i;
        if (l->text < 0 && l->langs[i].text)
            l->text = i;
    }
    return l->count;
}

int xf_langs_find(const xf_langs *l, const char *code)
{
    int i;
    for (i = 0; i < l->count; i++)
        if (strcasecmp(l->langs[i].code, code) == 0)
            return i;
    return -1;
}

const char *xf_langs_game_dir(const xf_langs *l)
{
    int i, k;
    /* The releases share one database, so the choice stays put when the
     * languages change. */
    k = xf_langs_find(l, "en");
    if (k >= 0 && l->langs[k].game)
        return l->langs[k].dir;
    for (i = 0; i < l->count; i++)
        if (l->langs[i].game)
            return l->langs[i].dir;
    return NULL;
}

int xf_langs_is_voice(const char *rel)
{
    return strncasecmp(rel, "XV/", 3) == 0 || strncasecmp(rel, "XS/", 3) == 0 || strncasecmp(rel, "VOB/", 4) == 0 ||
           strncasecmp(rel, "XV\\", 3) == 0 || strncasecmp(rel, "XS\\", 3) == 0;
}

/* The languages to look in: the language, its fallback, English. */
static int chain(const xf_langs *l, int first, int out[3])
{
    int n = 0, k;
    if (first < 0)
        first = xf_langs_find(l, "en");
    if (first < 0)
        return 0;
    out[n++] = first;
    k = xf_langs_find(l, l->langs[first].fallback);
    if (k >= 0 && k != first)
        out[n++] = k;
    k = xf_langs_find(l, "en");
    if (k >= 0 && k != out[0] && (n < 2 || k != out[1]))
        out[n++] = k;
    return n;
}

static int resolve_along(const xf_langs *l, int first, const char *rel, char *out, size_t size)
{
    int c[3], n = chain(l, first, c), i;
    for (i = 0; i < n; i++)
        if (xf_path_resolve(l->langs[c[i]].dir, rel, out, size) == 0)
            return 0;
    return -1;
}

int xf_langs_resolve(const xf_langs *l, const char *rel, char *out, size_t size)
{
    if (!l->count)
        return -1;
    return resolve_along(l, xf_langs_is_voice(rel) ? l->voices : l->text, rel, out, size);
}

int xf_langs_resolve_text(const xf_langs *l, const char *rel, char *out, size_t size)
{
    return l->count ? resolve_along(l, l->text, rel, out, size) : -1;
}

int xf_langs_resolve_in(const xf_langs *l, int lang, const char *rel, char *out, size_t size)
{
    if (lang < 0 || lang >= l->count)
        return -1;
    return xf_path_resolve(l->langs[lang].dir, rel, out, size);
}

int xf_langs_captions(const xf_langs *l, const char *rel, const char *clip_path, char *out, size_t size)
{
    char srt[512];
    const char *dot = strrchr(rel, '.');
    int c[3], n, i;
    if (!l->count)
        return -1;
    snprintf(srt, sizeof(srt), "%.*s.srt", dot ? (int)(dot - rel) : (int)strlen(rel), rel);
    n = chain(l, l->text, c);
    for (i = 0; i < n; i++) {
        if (xf_path_resolve(l->langs[c[i]].dir, srt, out, size) == 0)
            return 0;
        if (xf_path_resolve(l->langs[c[i]].dir, rel, out, size) == 0)
            return strcmp(out, clip_path) == 0 ? -1 : 0;
    }
    return -1;
}

i32 xf_lang_pack(const char *code)
{
    i32 v = 0;
    int i;
    for (i = 0; i < 3 && code[i]; i++)
        v |= (i32)(unsigned char)code[i] << (8 * i);
    return v;
}

void xf_lang_unpack(i32 v, char code[8])
{
    int i;
    for (i = 0; i < 3; i++)
        code[i] = (char)((v >> (8 * i)) & 0xFF);
    code[3] = '\0';
}
