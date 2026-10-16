/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* Translations: lang-export writes a release's text as files a translator
 * edits (docs/engine/languages.md), lang-check compares a translation
 * with them. */
#ifdef __WATCOMC__
#include <direct.h>
#define make_dir(p) mkdir(p)
#else
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#define make_dir(p) mkdir(p, 0755)
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "data/strings.h"
#include "media/mov.h"
#include "media/subtitle.h"
#include "xf_file.h"
#include "xf_text.h"
#include "xftool.h"

static const char *const g_tables[][2] = {
    { "XFILEST.DLL", "xfilest.txt" }, /* the game's text */
    { "XFILESE.DLL", "xfilese.txt" }, /* the engine's messages */
    { "XFILESC.DLL", "xfilesc.txt" }, /* the credits */
};
/* The folders whose clips can have captions. */
static const char *const g_clip_dirs[] = { "XV", "XG", "XS" };

static void put_escaped(FILE *f, const char *s)
{
    for (; *s; s++) {
        if (*s == '\n')
            fputs("\\n", f);
        else if (*s == '\r')
            fputs("\\n", f);
        else if (*s == '\t')
            fputs("\\t", f);
        else if (*s == '\\')
            fputs("\\\\", f);
        else
            fputc(*s, f);
    }
}

static void put_time(FILE *f, u32 ms)
{
    fprintf(f, "%02u:%02u:%02u,%03u", ms / 3600000, ms / 60000 % 60, ms / 1000 % 60, ms % 1000);
}

/* The lines a clip shows, or count 0. */
static int clip_captions(const char *path, xf_subtitles *shown)
{
    xf_mov mov;
    xf_subtitles all;
    u32 i;
    memset(shown, 0, sizeof(*shown));
    if (xf_mov_open(&mov, path) != 0)
        return -1;
    if (xf_subtitles_load(&mov, &all) != 0) {
        xf_mov_close(&mov);
        return -1;
    }
    xf_mov_close(&mov);
    xf_subtitles_shown(&all, shown);
    xf_subtitles_free(&all);
    for (i = 0; i < shown->count; i++) { /* Mac line ends */
        char *c;
        for (c = shown->cues[i].text; *c; c++)
            if (*c == '\r')
                *c = '\n';
    }
    return 0;
}

static int write_srt(const char *path, const xf_subtitles *s)
{
    FILE *f = fopen(path, "w");
    u32 i;
    if (!f)
        return -1;
    for (i = 0; i < s->count; i++) {
        fprintf(f, "%u\n", i + 1);
        put_time(f, s->cues[i].start_ms);
        fputs(" --> ", f);
        put_time(f, s->cues[i].end_ms);
        fprintf(f, "\n%s\n\n", s->cues[i].text);
    }
    fclose(f);
    return 0;
}

/* Calls fn for each file of `dir`/`sub`. */
static int each_file(const char *dir, const char *sub, int (*fn)(void *, const char *, const char *), void *user)
{
    char path[1024];
    DIR *d;
    struct dirent *de;
    int n = 0;
    if (xf_path_resolve(dir, sub, path, sizeof(path)) != 0 || !(d = opendir(path)))
        return 0;
    while ((de = readdir(d)) != NULL)
        if (de->d_name[0] != '.')
            n += fn(user, path, de->d_name) == 0;
    closedir(d);
    return n;
}

static int has_ext(const char *name, const char *ext)
{
    size_t n = strlen(name), k = strlen(ext);
    return n > k && strcasecmp(name + n - k, ext) == 0;
}

static void lower(char *s)
{
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z')
            *s = (char)(*s - 'A' + 'a');
}

/* --- lang-export ------------------------------------------------------ */

typedef struct {
    const char *out;
    const char *sub;
    long words, lines;
} export_ctx;

static long count_words(const char *s)
{
    long n = 0;
    int in = 0;
    for (; *s; s++) {
        int w = *s != ' ' && *s != '\n' && *s != '\t';
        n += w && !in;
        in = w;
    }
    return n;
}

static int export_doc(void *user, const char *dir, const char *name)
{
    export_ctx *x = user;
    char path[1024], out[1024], lname[256];
    size_t size, r, w = 0;
    u8 *d;
    char *text;
    FILE *f;
    if (!has_ext(name, ".xtx"))
        return -1;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (!(d = xf_load(path, &size)))
        return -1;
    for (r = 0; r < size; r++) /* CR LF and CR become LF */
        if (d[r] != '\r')
            d[w++] = d[r];
        else if (r + 1 >= size || d[r + 1] != '\n')
            d[w++] = '\n';
    text = malloc(w * 3 + 1);
    snprintf(lname, sizeof(lname), "%s", name);
    lower(lname);
    snprintf(out, sizeof(out), "%s/xt/%s", x->out, lname);
    if (!text || !(f = fopen(out, "wb"))) {
        free(text);
        free(d);
        return -1;
    }
    xf_cp1252_to_utf8(text, w * 3 + 1, d, w);
    fputs(text, f);
    fclose(f);
    x->words += count_words(text);
    x->lines++;
    free(text);
    free(d);
    return 0;
}

static int export_clip(void *user, const char *dir, const char *name)
{
    export_ctx *x = user;
    char path[1024], out[1024], lname[256];
    xf_subtitles s;
    u32 i;
    int r;
    if (!has_ext(name, ".xmv") && !has_ext(name, ".mov"))
        return -1;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (clip_captions(path, &s) != 0 || !s.count) {
        xf_subtitles_free(&s);
        return -1;
    }
    snprintf(lname, sizeof(lname), "%.*s.srt", (int)(strrchr(name, '.') - name), name);
    lower(lname);
    snprintf(out, sizeof(out), "%s/%s", x->out, x->sub);
    lower(out + strlen(x->out));
    make_dir(out);
    snprintf(out + strlen(out), sizeof(out) - strlen(out), "/%s", lname);
    r = write_srt(out, &s);
    for (i = 0; i < s.count; i++)
        x->words += count_words(s.cues[i].text);
    x->lines += (long)s.count;
    xf_subtitles_free(&s);
    return r;
}

static int cmd_lang_export(int argc, char **argv)
{
    export_ctx x;
    char path[1024];
    size_t i;
    if (argc != 3) {
        fputs("usage: xftool lang-export <release folder> <out folder>\n", stderr);
        return 2;
    }
    memset(&x, 0, sizeof(x));
    x.out = argv[2];
    make_dir(argv[2]);
    for (i = 0; i < XF_COUNT(g_tables); i++) {
        xf_strings t;
        FILE *f;
        u32 k;
        long words = 0;
        if (xf_path_resolve(argv[1], g_tables[i][0], path, sizeof(path)) != 0 || xf_strings_load(&t, path) != 0)
            continue;
        snprintf(path, sizeof(path), "%s/%s", argv[2], g_tables[i][1]);
        if (!(f = fopen(path, "wb"))) {
            xf_strings_free(&t);
            return 1;
        }
        fprintf(f, "# %s: one string a line, id<TAB>text; \\n is a line break.\n", g_tables[i][0]);
        for (k = 0; k < t.count; k++) {
            fprintf(f, "%u\t", t.ids[k]);
            put_escaped(f, t.texts[k]);
            fputc('\n', f);
            words += count_words(t.texts[k]);
        }
        fclose(f);
        printf("%s: %u strings, %ld words\n", g_tables[i][1], t.count, words);
        xf_strings_free(&t);
    }
    snprintf(path, sizeof(path), "%s/xt", argv[2]);
    make_dir(path);
    each_file(argv[1], "XT", export_doc, &x);
    printf("xt: %ld documents, %ld words\n", x.lines, x.words);
    for (i = 0; i < XF_COUNT(g_clip_dirs); i++) {
        int n;
        x.words = x.lines = 0;
        x.sub = g_clip_dirs[i];
        n = each_file(argv[1], g_clip_dirs[i], export_clip, &x);
        if (n)
            printf("%s: %d clips with captions, %ld lines, %ld words\n", g_clip_dirs[i], n, x.lines, x.words);
    }
    return 0;
}

/* --- lang-check ------------------------------------------------------- */

typedef struct {
    const char *lang;
    const char *sub;
    int missing, wrong, ok;
} check_ctx;

/* The placeholders (%s, %d, ...) of a string, in order. */
static void placeholders(const char *s, char *out, size_t size)
{
    size_t n = 0;
    for (; *s && n + 3 < size; s++)
        if (*s == '%' && s[1]) {
            out[n++] = '%';
            out[n++] = *++s;
        }
    out[n] = '\0';
}

static int check_clip(void *user, const char *dir, const char *name)
{
    check_ctx *c = user;
    char path[1024], rel[300];
    xf_subtitles en, tr;
    u32 i;
    int bad = 0;
    if (!has_ext(name, ".xmv") && !has_ext(name, ".mov"))
        return -1;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (clip_captions(path, &en) != 0 || !en.count) {
        xf_subtitles_free(&en);
        return -1;
    }
    snprintf(rel, sizeof(rel), "%s/%.*s.srt", c->sub, (int)(strrchr(name, '.') - name), name);
    if (xf_path_resolve(c->lang, rel, path, sizeof(path)) != 0 || xf_subtitles_load_srt(path, &tr) != 0) {
        c->missing++;
        xf_subtitles_free(&en);
        return -1;
    }
    if (tr.count != en.count) {
        printf("%s: %u lines, the clip has %u\n", rel, tr.count, en.count);
        bad = 1;
    }
    for (i = 0; !bad && i < en.count; i++)
        if (tr.cues[i].start_ms != en.cues[i].start_ms || tr.cues[i].end_ms != en.cues[i].end_ms) {
            printf("%s: line %u has other times\n", rel, i + 1);
            bad = 1;
        }
    c->wrong += bad;
    c->ok += !bad;
    xf_subtitles_free(&en);
    xf_subtitles_free(&tr);
    return 0;
}

/* The header of a document: "Key: value" lines up to the first blank. */
static int header_lines(const char *text)
{
    int n = 0;
    while (*text && *text != '\n') {
        const char *end = strchr(text, '\n'), *colon = strchr(text, ':');
        if (!colon || (end && colon > end))
            break;
        n++;
        text = end ? end + 1 : text + strlen(text);
    }
    return n;
}

static int check_doc(void *user, const char *dir, const char *name)
{
    check_ctx *c = user;
    char path[1024], rel[300];
    size_t a, b;
    u8 *en, *tr;
    if (!has_ext(name, ".xtx"))
        return -1;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    snprintf(rel, sizeof(rel), "XT/%s", name);
    en = xf_load(path, &a);
    if (xf_path_resolve(c->lang, rel, path, sizeof(path)) != 0 || !(tr = xf_load(path, &b))) {
        c->missing++;
        free(en);
        return -1;
    }
    {
        char *t = realloc(tr, b + 1), *e = en ? realloc(en, a + 1) : NULL;
        int he, ht;
        if (!t || !e) {
            free(t ? t : (char *)tr);
            free(e ? e : (char *)en);
            return -1;
        }
        t[b] = e[a] = '\0';
        he = header_lines(e);
        ht = header_lines(t);
        if (he != ht) {
            printf("%s: %d header lines, the original has %d\n", rel, ht, he);
            c->wrong++;
        } else {
            c->ok++;
        }
        free(t);
        free(e);
    }
    return 0;
}

static int cmd_lang_check(int argc, char **argv)
{
    check_ctx c;
    char path[1024], pe[64], pt[64];
    size_t i;
    int problems = 0;
    if (argc != 3) {
        fputs("usage: xftool lang-check <release folder> <translation folder>\n", stderr);
        return 2;
    }
    for (i = 0; i < XF_COUNT(g_tables); i++) {
        xf_strings en, tr;
        u32 k;
        int missing = 0, wrong = 0;
        if (xf_path_resolve(argv[1], g_tables[i][0], path, sizeof(path)) != 0 || xf_strings_load(&en, path) != 0)
            continue;
        memset(&tr, 0, sizeof(tr));
        if (xf_path_resolve(argv[2], g_tables[i][1], path, sizeof(path)) == 0)
            xf_strings_load_text(&tr, path);
        for (k = 0; k < en.count; k++) {
            const char *s = xf_strings_get(&tr, en.ids[k]);
            if (!s) {
                missing++;
                continue;
            }
            placeholders(en.texts[k], pe, sizeof(pe));
            placeholders(s, pt, sizeof(pt));
            if (strcmp(pe, pt) != 0) {
                printf("%s %u: placeholders %s, the original has %s\n", g_tables[i][1], en.ids[k], pt, pe);
                wrong++;
            }
        }
        printf("%s: %u strings, %d missing, %d wrong\n", g_tables[i][1], en.count, missing, wrong);
        problems += wrong;
        xf_strings_free(&en);
        xf_strings_free(&tr);
    }
    memset(&c, 0, sizeof(c));
    c.lang = argv[2];
    each_file(argv[1], "XT", check_doc, &c);
    printf("xt: %d translated, %d missing, %d wrong\n", c.ok + c.wrong, c.missing, c.wrong);
    problems += c.wrong;
    for (i = 0; i < XF_COUNT(g_clip_dirs); i++) {
        memset(&c, 0, sizeof(c));
        c.lang = argv[2];
        c.sub = g_clip_dirs[i];
        each_file(argv[1], g_clip_dirs[i], check_clip, &c);
        if (c.ok + c.wrong + c.missing)
            printf("%s: %d captioned clips translated, %d missing, %d wrong\n", g_clip_dirs[i], c.ok + c.wrong,
                   c.missing, c.wrong);
        problems += c.wrong;
    }
    return problems ? 1 : 0;
}

const xftool_cmd xftool_lang_cmds[] = {
    { "lang-export", "<release> <out>  write a release's text as files to translate", cmd_lang_export },
    { "lang-check", "<release> <translation>  compare a translation with the release's text", cmd_lang_check },
    { 0 }
};
