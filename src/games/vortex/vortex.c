/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* Vortex: Quantum Gate II. Runs the sequences compiled into VORTEX.EXE
 * (docs/vortex/README.md) on the shared stage. */
#include <stdlib.h>
#include <string.h>

#include "games/games.h"
#include "games/stage.h"
#include "games/vortex/vexe.h"
#include "games/vortex/vseq.h"
#include "audio/mixer.h"
#include "media/player.h"
#include "media/wav.h"
#include "gfx/text.h"
#include "gfx/ttf.h"
#include "xf_endian.h"
#include "xf_text.h"
#include "xf_log.h"

#define MAX_CLIPS 4

typedef struct {
    xf_player p;
    int open, seq, x, y;
} vx_clip;

typedef struct {
    xf_stage st;
    vexe exe;
    vs_state vm;
    u32 pic[STAGE_W * STAGE_H]; /* the pictures (the back buffer) */
    u32 bg[STAGE_W * STAGE_H];  /* the screen: the pictures (the texts are kept apart) */
    vx_clip clips[MAX_CLIPS];
    /* Texts are drawn on the screen over the sprites under them (seg3:0E26);
     * a screen update or a change of a sprite under one redraws that place
     * without it. They are kept to draw again over the sprites each frame. */
    struct {
        int font, x, y, w, h;
        u32 rgb, frame;
        char text[256];
    } texts[32];
    int text_count;
    u32 frame;
    struct {                /* each sequence's sprite as drawn last */
        u16 slot;
        i16 x, y, w, h;
    } drawn[VS_SEQS];
    xf_wav sounds[64];      /* loaded by 0x1F5; handle = index + 1 */
    struct {
        int sound;          /* handle playing, 0 for none */
        int mixer;
        u32 written;
        int prio;
        u32 end_ms;         /* when it ends: the clock, so that it ends without a sound device too */
    } chan[2];
    struct {
        u16 op;
        vx_ptr at;
    } logged[64];
    int logged_count;
    xf_font fonts[16];
    u8 font_tried[16];
    struct {                /* the score: system\\midifile.txt (midi.c) */
        int dims[3];        /* scenes, variants, moods */
        int files;
        char (*names)[32];
        u16 (*cells)[2];    /* a song, how many times it plays */
    } score;
    int own_cursor;         /* the system's cursor is hidden */
    int discs;              /* bit n: disc n's folders are here */
    int disc;               /* the disc in the drive */
    struct {                /* the sprite slots (DS:46DC), then the cursors */
        u32 *pixels;        /* 0x00RRGGBB; KEY is clear */
        int w, h;
    } sprites[0x7D];
} vortex;

#define KEY 0xFF000000u

/* The fonts (seg14:042E builds a LOGFONT by number): a height and a face.
 * The game ships Drew, ExocetHeavy, Gill and CentBD (SYSTEM\*.TTR); Helv,
 * Courier and Tms Rmn were Windows' own, here the system's Arial,
 * Courier New and Times New Roman. */
static const struct {
    int height;
    const char *face;
} g_fonts[16] = {
    [2] = { 8, "Courier" },  [3] = { 8, "Helv" },     [4] = { 8, "Tms Rmn" },  [5] = { 12, "Courier" },
    [6] = { 12, "Helv" },    [7] = { 12, "Tms Rmn" }, [8] = { 30, "Drew" },    [9] = { 8, "Helv" },
    [10] = { 12, "ExocetHeavy" }, [11] = { 18, "Doi" }, [12] = { 22, "Gill" }, [13] = { 22, "CentBD" },
};

static const xf_font *font_for(vortex *v, int n)
{
    static const struct {
        const char *face, *file, *system;
    } faces[] = {
        { "Drew", "SYSTEM\\DREW4.TTR", NULL },  { "ExocetHeavy", "SYSTEM\\EXOCE5.TTR", NULL },
        { "Gill", "SYSTEM\\GILL.TTR", NULL },   { "CentBD", "SYSTEM\\CENTBD.TTR", NULL },
        { "Courier", NULL, "Courier New" }, { "Tms Rmn", NULL, "Times New Roman" },
    };
    char path[1024];
    const char *face;
    size_t k;
    if (n < 2 || n > 13)
        n = 9;
    if (v->font_tried[n])
        return v->fonts[n].data ? &v->fonts[n] : NULL;
    v->font_tried[n] = 1;
    face = g_fonts[n].face;
    for (k = 0; k < XF_COUNT(faces); k++)
        if (strcmp(faces[k].face, face) == 0)
            break;
    if (k < XF_COUNT(faces) && faces[k].file && stage_path(&v->st, faces[k].file, path, sizeof(path)) == 0 &&
        xf_font_load(&v->fonts[n], path) == 0)
        return &v->fonts[n];
    if (plat_font_path(k < XF_COUNT(faces) && faces[k].system ? faces[k].system : "Arial", path, sizeof(path)) == 0 &&
        xf_font_load(&v->fonts[n], path) == 0)
        return &v->fonts[n];
    return NULL;
}

static void blit(u32 *dst, const u32 *src, int w, int h, int x0, int y0)
{
    int x, y;
    for (y = 0; y < h; y++) {
        int sy = y0 + y;
        if (sy < 0 || sy >= STAGE_H)
            continue;
        for (x = 0; x < w; x++) {
            int sx = x0 + x;
            if (sx >= 0 && sx < STAGE_W)
                dst[sy * STAGE_W + sx] = src[(size_t)y * w + x];
        }
    }
}

/* --- What the sequences ask --------------------------------------------- */

static void on_unknown(void *user, int seq, u16 op, vx_ptr at)
{
    vortex *v = user;
    int i;
    for (i = 0; i < v->logged_count; i++)
        if (v->logged[i].op == op && v->logged[i].at == at)
            return;
    if (v->logged_count < (int)XF_COUNT(v->logged)) {
        v->logged[v->logged_count].op = op;
        v->logged[v->logged_count++].at = at;
    }
    xf_info("vortex: sequence %d waits at seg%d:%04X on opcode %04X, not written yet", seq, VX_SEG(at), VX_OFF(at),
            op);
}

static void stop_clip(vortex *v, vx_clip *c)
{
    if (!c->open)
        return;
    xf_debug("vortex: clip of sequence %d stopped", c->seq);
    xf_player_close(&c->p);
    c->open = 0;
    vs_set(&v->vm, c->seq, VC_MOVIE, 0);
    vs_set(&v->vm, c->seq, VC_MOVIE + 2, 0);
}

/* The clip's window into its sequence (context + 0x50). */
static void clip_rect(vortex *v, const vx_clip *c)
{
    vs_set(&v->vm, c->seq, VC_MOVIE_RECT, (u16)c->x);
    vs_set(&v->vm, c->seq, VC_MOVIE_RECT + 2, (u16)c->y);
    vs_set(&v->vm, c->seq, VC_MOVIE_RECT + 4, (u16)(c->x + c->p.video.width));
    vs_set(&v->vm, c->seq, VC_MOVIE_RECT + 6, (u16)(c->y + c->p.video.height));
}

/* 0x190: a clip at (x, y), top left; the sequence's own (seg8:1ED2). */
static int on_clip(void *user, int seq, int sub, const char *file, int x, int y, const u16 *w, u8 b0, u8 b1)
{
    vortex *v = user;
    char path[1024];
    int i;
    (void)w;
    (void)b0;
    (void)b1;
    for (i = 0; i < MAX_CLIPS; i++)
        if (v->clips[i].open && v->clips[i].seq == seq)
            stop_clip(v, &v->clips[i]);
    if (!file)
        return 0; /* the other sub-types act on the clip playing: stopped for now */
    for (i = 0; i < MAX_CLIPS && v->clips[i].open; i++)
        ;
    if (i == MAX_CLIPS || stage_path(&v->st, file, path, sizeof(path)) != 0 ||
        xf_player_open(&v->clips[i].p, path, plat_ticks_ms()) != 0) {
        xf_warn("vortex: clip %s not played", file);
        return -1;
    }
    xf_info("vortex: clip %s at (%d, %d), sub-type %d", file, x, y, sub);
    v->clips[i].open = 1;
    v->clips[i].seq = seq;
    v->clips[i].x = x;
    v->clips[i].y = y;
    vs_set(&v->vm, seq, VC_MOVIE, (u16)(i + 1));
    clip_rect(v, &v->clips[i]);
    return 0;
}

static void on_clip_move(void *user, int seq, int x, int y)
{
    vortex *v = user;
    int i;
    for (i = 0; i < MAX_CLIPS; i++)
        if (v->clips[i].open && v->clips[i].seq == seq) {
            v->clips[i].x = x;
            v->clips[i].y = y;
            clip_rect(v, &v->clips[i]);
        }
}

static long on_clip_time(void *user, int seq)
{
    vortex *v = user;
    int i;
    for (i = 0; i < MAX_CLIPS; i++)
        if (v->clips[i].open && v->clips[i].seq == seq)
            return (long)v->clips[i].p.now_ms;
    return -1;
}

static void on_clip_op(void *user, int seq, int action, u16 a, u16 b)
{
    vortex *v = user;
    int i;
    (void)a;
    (void)b;
    for (i = 0; i < MAX_CLIPS; i++)
        if (v->clips[i].open && v->clips[i].seq == seq && action == 9)
            stop_clip(v, &v->clips[i]);
}

static void on_stopped(void *user, int seq)
{
    vortex *v = user;
    int i;
    for (i = 0; i < MAX_CLIPS; i++)
        if (v->clips[i].open && v->clips[i].seq == seq)
            stop_clip(v, &v->clips[i]);
}

static void on_moved(void *user, int from, int to)
{
    vortex *v = user;
    int i;
    for (i = 0; i < MAX_CLIPS; i++)
        if (v->clips[i].open && v->clips[i].seq == from)
            v->clips[i].seq = to;
}

/* usWinAscent and usWinDescent, when the font has an OS/2 table. */
static void win_metrics(const xf_font *f, int *ascent, int *span)
{
    u32 n, i;
    if (!f->data || f->size < 12)
        return;
    n = xf_be16(f->data + 4);
    for (i = 0; i < n && 12 + 16 * (i + 1) <= f->size; i++) {
        const u8 *e = f->data + 12 + 16 * i;
        u32 off = xf_be32(e + 8);
        if (memcmp(e, "OS/2", 4) == 0 && off + 78 <= f->size) {
            *ascent = xf_be16(f->data + off + 74);
            *span = *ascent + xf_be16(f->data + off + 76);
            return;
        }
    }
}

/* Text on the background: in a box, wrapped at words from its top left
 * (DrawText with DT_WORDBREAK); else one line from x, y. */
static void render_text(vortex *v, u32 *pixels, int font, int x, int y, int w, int h, u32 rgb, const char *text)
{
    const xf_font *f = font_for(v, font);
    xf_surface s;
    char utf8[1024];
    float px;
    int starts[32], lens[32], lines, i, line_h, ascent;
    if (!f || !*text)
        return;
    xf_cp1252_to_utf8(utf8, sizeof(utf8), (const u8 *)text, strlen(text));
    {
        /* a LOGFONT height is the cell, which Windows takes from the
         * OS/2 table's usWinAscent + usWinDescent */
        int cell = g_fonts[font >= 2 && font <= 13 ? font : 9].height, asc = f->ascent, span = f->ascent - f->descent;
        win_metrics(f, &asc, &span);
        px = span > 0 ? (float)cell * (float)f->units_per_em / (float)span : (float)cell;
        line_h = cell;
        ascent = span > 0 ? cell * asc / span : cell * 4 / 5;
    }
    s.pixels = pixels;
    s.width = STAGE_W;
    s.height = STAGE_H;
    s.pitch = STAGE_W;
    lines = w > 0 ? xf_text_wrap(f, px, utf8, w, starts, lens, 32) : 1;
    if (w <= 0) {
        starts[0] = 0;
        lens[0] = (int)strlen(utf8);
    }
    for (i = 0; i < lines; i++) {
        int base = y + ascent + i * line_h;
        if (h > 0 && base > y + h)
            break;
        xf_text_draw(&s, f, px, x, base, utf8 + starts[i], (size_t)lens[i], rgb);
    }
}

/* The place a text takes: its box, or a line up to the screen's right. */
static void text_rect(vortex *v, int k, int *x0, int *y0, int *x1, int *y1)
{
    int font = v->texts[k].font, cell = g_fonts[font >= 2 && font <= 13 ? font : 9].height;
    *x0 = v->texts[k].x;
    *y0 = v->texts[k].y;
    *x1 = v->texts[k].w > 0 ? *x0 + v->texts[k].w : STAGE_W;
    *y1 = *y0 + (v->texts[k].h > 0 ? v->texts[k].h : cell);
}

/* The texts in a rectangle go (the screen there is drawn again), the ones
 * drawn this frame too unless `older`. */
static void erase_texts(vortex *v, int x, int y, int w, int h, int older)
{
    int k, n = 0;
    for (k = 0; k < v->text_count; k++) {
        int x0, y0, x1, y1;
        text_rect(v, k, &x0, &y0, &x1, &y1);
        if ((!older || v->texts[k].frame != v->frame) && x0 < x + w && x < x1 && y0 < y + h && y < y1)
            continue;
        if (n != k)
            v->texts[n] = v->texts[k];
        n++;
    }
    v->text_count = n;
}

static void on_text_extent(void *user, int font, const char *text, int *w, int *h)
{
    vortex *v = user;
    const xf_font *f = font_for(v, font);
    int cell = g_fonts[font >= 2 && font <= 13 ? font : 9].height, asc, span;
    char utf8[512];
    float px;
    *w = 0;
    *h = cell;
    if (!f)
        return;
    asc = f->ascent;
    span = f->ascent - f->descent;
    win_metrics(f, &asc, &span);
    px = span > 0 ? (float)cell * (float)f->units_per_em / (float)span : (float)cell;
    xf_cp1252_to_utf8(utf8, sizeof(utf8), (const u8 *)text, strlen(text));
    *w = xf_text_width(f, px, utf8, strlen(utf8));
}

static void on_text(void *user, int font, int x, int y, int w, int h, u32 rgb, const char *text)
{
    vortex *v = user;
    int k;
    if (!*text)
        return;
    xf_debug("vortex: text, font %d, at (%d, %d) %dx%d: %s", font, x, y, w, h, text);
    if (v->text_count == (int)XF_COUNT(v->texts))
        erase_texts(v, 0, 0, STAGE_W, STAGE_H, 0); /* full: start again */
    k = v->text_count++;
    v->texts[k].font = font;
    v->texts[k].x = x;
    v->texts[k].y = y;
    v->texts[k].w = w;
    v->texts[k].h = h;
    v->texts[k].rgb = rgb;
    v->texts[k].frame = v->frame;
    snprintf(v->texts[k].text, sizeof(v->texts[k].text), "%s", text);
}

static void free_sprite(vortex *v, int slot)
{
    free(v->sprites[slot].pixels);
    v->sprites[slot].pixels = NULL;
    v->sprites[slot].w = v->sprites[slot].h = 0;
}

static void on_sprite_free(void *user, int slot) { free_sprite(user, slot); }

/* 0x7D0: the bitmaps of a file, one after the other, into the slots
 * from `slot` (seg2:0324). */
static int on_sprite_load(void *user, int slot, const char *file)
{
    vortex *v = user;
    char path[1024];
    size_t size, at = 0;
    u8 *data;
    int n = 0;
    if (stage_path(&v->st, file, path, sizeof(path)) != 0 || !(data = xf_load(path, &size))) {
        xf_warn("vortex: sprites %s not found", file);
        return 0;
    }
    while (slot + n < (int)XF_COUNT(v->sprites) && at + 14 <= size && data[at] == 'B' && data[at + 1] == 'M') {
        u32 len = xf_le32(data + at + 2);
        xf_bmp b;
        if (len < 14 || len > size - at || xf_bmp_decode(data + at, len, &b) != 0)
            break;
        free_sprite(v, slot + n);
        v->sprites[slot + n].pixels = b.pixels; /* taken from the bitmap */
        v->sprites[slot + n].w = b.width;
        v->sprites[slot + n].h = b.height;
        b.pixels = NULL;
        xf_bmp_free(&b);
        at += len;
        n++;
    }
    free(data);
    xf_debug("vortex: sprites %s: %d into %d", file, n, slot);
    return n;
}

static int on_sprite_size(void *user, int slot, int *w, int *h)
{
    vortex *v = user;
    if (slot < 0 || slot >= (int)XF_COUNT(v->sprites) || !v->sprites[slot].pixels)
        return -1;
    *w = v->sprites[slot].w;
    *h = v->sprites[slot].h;
    return 0;
}

/* 0x7E4: a text as a sprite (seg3:10A2), one line on a clear ground. */
static int on_text_sprite(void *user, int slot, int font, u32 rgb, const char *text, int *w, int *h)
{
    vortex *v = user;
    const xf_font *f = font_for(v, font);
    char utf8[512];
    xf_surface s;
    int cell = g_fonts[font >= 2 && font <= 13 ? font : 9].height, asc, span, tw, th;
    size_t i;
    float px;
    if (!f)
        return -1;
    asc = f->ascent;
    span = f->ascent - f->descent;
    win_metrics(f, &asc, &span);
    px = span > 0 ? (float)cell * (float)f->units_per_em / (float)span : (float)cell;
    xf_cp1252_to_utf8(utf8, sizeof(utf8), (const u8 *)text, strlen(text));
    /* the extent, rounded up to 4; a box larger than that (narrower than
     * the screen, lower than 40) is kept */
    tw = (xf_text_width(f, px, utf8, strlen(utf8)) + 3) & ~3;
    th = cell;
    if (*w > tw && *w < STAGE_W)
        tw = *w;
    if (*h > th && *h < 40)
        th = *h;
    if (tw <= 0)
        tw = 4;
    free_sprite(v, slot);
    if (!(v->sprites[slot].pixels = malloc((size_t)tw * th * 4)))
        return -1;
    for (i = 0; i < (size_t)tw * th; i++)
        v->sprites[slot].pixels[i] = 0; /* black, opaque */
    v->sprites[slot].w = tw;
    v->sprites[slot].h = th;
    s.pixels = v->sprites[slot].pixels;
    s.width = tw;
    s.height = th;
    s.pitch = tw;
    xf_text_draw(&s, f, px, 0, span > 0 ? cell * asc / span : cell * 4 / 5, utf8, strlen(utf8), rgb);
    *w = tw;
    *h = th;
    return 0;
}

/* 0x12C: pictures on the background. */
static void on_picture(void *user, int seq, int sub, const char *file, int x, int y)
{
    vortex *v = user;
    char path[1024];
    xf_bmp b;
    (void)seq;
    if (!file) {
        if (sub == 2) {
            memset(v->pic, 0, sizeof(v->pic));
            memset(v->bg, 0, sizeof(v->bg));
            v->text_count = 0;
        }
        return;
    }
    if (stage_path(&v->st, file, path, sizeof(path)) != 0 || xf_bmp_load(path, &b) != 0) {
        xf_warn("vortex: picture %s not found", file);
        return;
    }
    xf_info("vortex: picture %s at (%d, %d), sub-type %d", file, x, y, sub);
    blit(v->pic, b.pixels, b.width, b.height, x, y);
    blit(v->bg, b.pixels, b.width, b.height, x, y);
    xf_bmp_free(&b);
}

/* The game comes on three discs, VORTEX_1 to VORTEX_3, each with its
 * days' folders (c\1a, c\2a, c\3a). A game folder holds one disc's
 * files, or all of them, or the discs as folders named VORTEX_1 to
 * VORTEX_3 (the disc images' contents). As with a drive, the disc in it
 * changes when the program asks for another one that is here (after its
 * "insert disc" prompt). */
static int disc_folder(vortex *v, int n, char *out, size_t size)
{
    char rel[16];
    snprintf(rel, sizeof(rel), "VORTEX_%d", n);
    return xf_path_resolve(v->st.dir, rel, out, size);
}

static void insert_disc(vortex *v, int n)
{
    v->disc = n;
    if (disc_folder(v, n, v->st.disc_dir, sizeof(v->st.disc_dir)) != 0)
        v->st.disc_dir[0] = '\0';
}

static void find_discs(vortex *v)
{
    char rel[8], path[1024];
    int n;
    for (n = 1; n <= 3; n++) {
        snprintf(rel, sizeof(rel), "c\\%da", n);
        if (disc_folder(v, n, v->st.disc_dir, sizeof(v->st.disc_dir)) != 0)
            v->st.disc_dir[0] = '\0';
        if (stage_path(&v->st, rel, path, sizeof(path)) == 0) {
            v->discs |= 1 << n;
            if (!v->disc)
                v->disc = n;
        }
    }
    if (!v->disc) /* the menus and the splash clips need no day folder */
        v->disc = 1;
    insert_disc(v, v->disc);
    xf_info("vortex: discs here %s%s%s, disc %d in", v->discs & 2 ? "1 " : "", v->discs & 4 ? "2 " : "",
            v->discs & 8 ? "3 " : "", v->disc);
}

/* The score (seg17:0000): a line "scenes variants moods songs", a song
 * file a line, then a line a cell, scene by scene, "scene variant mood
 * plays song" (a count of 0 or less plays for ever). */
static void load_score(vortex *v)
{
    char path[1024], line[256];
    FILE *f;
    int i, n;
    if (stage_path(&v->st, "system\\midifile.txt", path, sizeof(path)) != 0 || !(f = fopen(path, "r")))
        return;
    if (!fgets(line, sizeof(line), f) ||
        sscanf(line, "%d %d %d %d", &v->score.dims[0], &v->score.dims[1], &v->score.dims[2], &v->score.files) != 4 ||
        v->score.files <= 0 || v->score.files > 500)
        goto bad;
    n = v->score.dims[0] * v->score.dims[1] * v->score.dims[2];
    if (n <= 0 || n > 0x4000)
        goto bad;
    v->score.names = calloc((size_t)v->score.files, sizeof(*v->score.names));
    v->score.cells = calloc((size_t)n, sizeof(*v->score.cells));
    if (!v->score.names || !v->score.cells)
        goto bad;
    for (i = 0; i < v->score.files; i++)
        if (!fgets(line, sizeof(line), f) || sscanf(line, " %31s", v->score.names[i]) != 1)
            goto bad;
    for (i = 0; i < n; i++) {
        int plays, song;
        if (!fgets(line, sizeof(line), f) || sscanf(line, "%*s %*s %*s %d %d", &plays, &song) != 2)
            goto bad;
        v->score.cells[i][0] = (u16)song;
        v->score.cells[i][1] = (u16)(plays > 0 ? plays : 0);
    }
    fclose(f);
    xf_info("vortex: score of %d songs, %dx%dx%d", v->score.files, v->score.dims[0], v->score.dims[1],
            v->score.dims[2]);
    return;
bad:
    xf_warn("vortex: %s: not a score", path);
    fclose(f);
    free(v->score.names);
    free(v->score.cells);
    memset(&v->score, 0, sizeof(v->score));
}

static void on_score(void *user, int scene, int variant, int mood)
{
    vortex *v = user;
    char rel[64];
    int i;
    if (!v->score.cells || scene >= v->score.dims[0] || variant >= v->score.dims[1] || mood >= v->score.dims[2])
        return;
    i = (scene * v->score.dims[1] + variant) * v->score.dims[2] + mood;
    if (v->score.cells[i][0] >= v->score.files)
        return;
    snprintf(rel, sizeof(rel), "c\\midi\\%s", v->score.names[v->score.cells[i][0]]);
    xf_info("vortex: score %d %d %d: %s", scene, variant, mood, rel);
    stage_music(&v->st, rel, v->score.cells[i][1]);
}

static u16 on_disc(void *user, int n)
{
    vortex *v = user;
    if (n == 9)
        return (u16)v->disc;
    if (n < 1 || n > 3 || !(v->discs & 1 << n))
        return 0;
    insert_disc(v, n);
    return 1;
}

/* 0x12E: a rectangle of the pictures reaches the screen (seg7:33B8),
 * over the texts drawn there. The effect (a wipe, a dissolve) is not
 * shown yet. */
static void on_screen(void *user, int seq, u16 op, int effect, int x, int y, int w, int h)
{
    vortex *v = user;
    int row;
    (void)seq;
    xf_debug("vortex: screen %04X effect %d at (%d, %d) %dx%d", op, effect, x, y, w, h);
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > STAGE_W)
        w = STAGE_W - x;
    if (y + h > STAGE_H)
        h = STAGE_H - y;
    for (row = y; w > 0 && row < y + h; row++)
        memcpy(v->bg + row * STAGE_W + x, v->pic + row * STAGE_W + x, (size_t)w * 4);
    if (w > 0 && h > 0)
        erase_texts(v, x, y, w, h, 0);
}

static int on_sound(void *user, int sub, const char *file, u16 arg)
{
    vortex *v = user;
    char path[1024];
    int i;
    (void)sub;
    (void)arg;
    for (i = 0; i < (int)XF_COUNT(v->sounds) && v->sounds[i].samples; i++)
        ;
    if (i == (int)XF_COUNT(v->sounds) || stage_path(&v->st, file, path, sizeof(path)) != 0 ||
        xf_wav_load(path, &v->sounds[i]) != 0) {
        xf_warn("vortex: sound %s not loaded", file);
        return 0;
    }
    return i + 1;
}

static void stop_channel(vortex *v, int c)
{
    if (v->chan[c].sound)
        xf_mixer_close(v->chan[c].mixer);
    v->chan[c].sound = 0;
}

static void feed_channels(vortex *v)
{
    int c;
    for (c = 0; c < 2; c++) {
        const xf_wav *w;
        if (!v->chan[c].sound)
            continue;
        w = &v->sounds[v->chan[c].sound - 1];
        if (v->chan[c].written < w->frames) {
            v->chan[c].written += xf_mixer_write(v->chan[c].mixer, w->samples + (size_t)v->chan[c].written * w->channels,
                                                 w->frames - v->chan[c].written);
            if (v->chan[c].written >= w->frames)
                xf_mixer_finish(v->chan[c].mixer);
        }
        if ((i32)(plat_ticks_ms() - v->chan[c].end_ms) >= 0)
            stop_channel(v, c);
    }
}

/* seg6:0436: two channels; 3 takes whichever is free; a sound of lower
 * priority than the one playing waits its turn (here: is dropped). */
static void on_sound_op(void *user, u16 op, u16 handle, const u16 *a)
{
    vortex *v = user;
    int c;
    if (op == 0x01F8) {
        if (handle >= 1 && handle <= 2)
            stop_channel(v, handle - 1);
        return;
    }
    if (op == 0x01F7) {
        for (c = 0; c < 2; c++)
            if (v->chan[c].sound == handle)
                stop_channel(v, c);
        if (handle >= 1 && handle <= XF_COUNT(v->sounds))
            xf_wav_free(&v->sounds[handle - 1]);
        return;
    }
    if (handle < 1 || handle > XF_COUNT(v->sounds) || !v->sounds[handle - 1].samples)
        return;
    c = a[0] == 2 ? 1 : 0;
    if (a[0] == 3 && v->chan[0].sound && !v->chan[1].sound)
        c = 1;
    if (v->chan[c].sound && (int)a[1] < v->chan[c].prio)
        return;
    stop_channel(v, c);
    v->chan[c].mixer = xf_mixer_open((int)v->sounds[handle - 1].rate, v->sounds[handle - 1].channels);
    if (v->chan[c].mixer < 0)
        return;
    v->chan[c].sound = handle;
    v->chan[c].written = 0;
    v->chan[c].prio = a[1];
    v->chan[c].end_ms = plat_ticks_ms() + (u32)((u64)v->sounds[handle - 1].frames * 1000 / v->sounds[handle - 1].rate);
    feed_channels(v);
}

static int on_sound_busy(void *user, int channel)
{
    vortex *v = user;
    return channel >= 1 && channel <= 2 && v->chan[channel - 1].sound;
}

static void on_music(void *user, int sub, const char *file, u16 arg)
{
    vortex *v = user;
    (void)arg;
    if (sub == 1)
        stage_music(&v->st, file, 0);
    else if (sub == 6)
        stage_music(&v->st, NULL, 0);
}

/* --- The loop ------------------------------------------------------------- */

static void update_clips(vortex *v)
{
    int i, skipped = 0;
    for (i = 0; i < MAX_CLIPS; i++) {
        vx_clip *c = &v->clips[i];
        if (!c->open)
            continue;
        if (v->st.skip || !xf_player_update(&c->p, plat_ticks_ms())) {
            int seq = c->seq;
            if (v->st.skip)
                skipped = 1;
            stop_clip(v, c);
            vs_event(&v->vm, seq, 1); /* the clip ended */
        }
    }
    if (skipped) /* else the next clip to play */
        v->st.skip--;
}

/* Each sequence shows its sprite at its place (seg3:2406), the lowest
 * z first. The drawing mode makes a colour (RGB555) clear (seg3:1790):
 * black, white, or none. Mode 8 tints the sprite (seg13): drawn as it
 * is for now. */
static int clear_colour(int mode)
{
    switch (mode) {
    case 1: case 5: case 6: return 0x7FFF;
    case 2: case 8: return -1;
    default: return 0;
    }
}

static void draw_sprites(vortex *v)
{
    int order[VS_SEQS], n = 0, i, k;
    for (i = 0; i < VS_SEQS; i++) {
        u16 slot = vs_get(&v->vm, i, VC_SPRITE);
        if (vs_get(&v->vm, i, VC_USED) && vs_get(&v->vm, i, VC_FLAGS) & 1 && slot < VS_SPRITES &&
            v->sprites[slot].pixels && (i16)vs_get(&v->vm, i, VC_Z) >= 0)
            order[n++] = i;
    }
    for (i = 1; i < n; i++) /* by z, lowest first */
        for (k = i; k > 0 && (i16)vs_get(&v->vm, order[k], VC_Z) < (i16)vs_get(&v->vm, order[k - 1], VC_Z); k--) {
            int t = order[k];
            order[k] = order[k - 1];
            order[k - 1] = t;
        }
    for (i = 0; i < n; i++) {
        int seq = order[i], x, y, key = clear_colour((i16)vs_get(&v->vm, seq, VC_MODE));
        u16 slot = vs_get(&v->vm, seq, VC_SPRITE);
        int x0 = (i16)vs_get(&v->vm, seq, VC_X), y0 = (i16)vs_get(&v->vm, seq, VC_Y);
        for (y = 0; y < v->sprites[slot].h; y++)
            for (x = 0; x < v->sprites[slot].w; x++) {
                u32 c = v->sprites[slot].pixels[y * v->sprites[slot].w + x];
                int sx = x0 + x, sy = y0 + y;
                if (c == KEY || sx < 0 || sy < 0 || sx >= STAGE_W || sy >= STAGE_H)
                    continue;
                if (key >= 0 && (int)((c >> 19 & 31) << 10 | (c >> 11 & 31) << 5 | (c >> 3 & 31)) == key)
                    continue;
                v->st.fb[sy * STAGE_W + sx] = c;
            }
    }
}

/* The program's two cursor resources (0xFA2, 0xFA3): '#' white, '.'
 * black, from the hotspot at (hx, hy). */
static const struct {
    int hx, hy;
    const char *rows[18];
} g_cursors[2] = {
    { 6, 6, { " .......... ", ".######.###.", ".######.###.", ".######.###.", ".######.###.", "............",
              ".##########.", ".##########.", ".##########.", ".##########.", ".##########.", ".##########.",
              ".##########.", ".##########.", ".##########.", " .########. ", "  .######.  ", "   ......   " } },
    { 2, 2, { " .#. ", "..#..", "#####", "..#..", " .#. " } },
};

static void put(vortex *v, int x, int y, u32 c)
{
    if (x >= 0 && y >= 0 && x < STAGE_W && y < STAGE_H)
        v->st.fb[y * STAGE_W + x] = c;
}

/* The cursor (seg3:2DEC): a resource, or a sprite slot from 0x5D on, an
 * image and then its mask (black: drawn), from its top left (seg3:2886).
 * 0 is the system's arrow. */
static void draw_cursor(vortex *v)
{
    u16 c = v->vm.cursor;
    int x, y, own = 1;
    if (c == 0xFA2 || c == 0xFA3) {
        int k = c - 0xFA2;
        for (y = 0; y < (int)XF_COUNT(g_cursors[k].rows) && g_cursors[k].rows[y]; y++)
            for (x = 0; g_cursors[k].rows[y][x]; x++)
                if (g_cursors[k].rows[y][x] != ' ')
                    put(v, v->st.mx - g_cursors[k].hx + x, v->st.my - g_cursors[k].hy + y,
                        g_cursors[k].rows[y][x] == '#' ? 0xFFFFFF : 0);
    } else if (c >= 0x5D && c + 1 < (int)XF_COUNT(v->sprites) && v->sprites[c].pixels && v->sprites[c + 1].pixels) {
        int w = XF_MIN(v->sprites[c].w, v->sprites[c + 1].w), h = XF_MIN(v->sprites[c].h, v->sprites[c + 1].h);
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                u32 m = v->sprites[c + 1].pixels[y * v->sprites[c + 1].w + x];
                if ((m >> 16 & 0xFF) + (m >> 8 & 0xFF) + (m & 0xFF) < 384)
                    put(v, v->st.mx + x, v->st.my + y, v->sprites[c].pixels[y * v->sprites[c].w + x]);
            }
    } else
        own = 0;
    if (own != v->own_cursor) {
        plat_show_cursor(!own);
        v->own_cursor = own;
    }
}

/* A sprite that changed since the last frame is drawn again with the
 * screen under it: the texts there from before go. */
static void sprite_changes(vortex *v)
{
    int i;
    for (i = 0; i < VS_SEQS; i++) {
        u16 slot = vs_get(&v->vm, i, VC_SPRITE);
        int shown = vs_get(&v->vm, i, VC_USED) && vs_get(&v->vm, i, VC_FLAGS) & 1 && slot < VS_SPRITES &&
                    v->sprites[slot].pixels;
        i16 x = (i16)vs_get(&v->vm, i, VC_X), y = (i16)vs_get(&v->vm, i, VC_Y);
        i16 w = shown ? (i16)v->sprites[slot].w : 0, h = shown ? (i16)v->sprites[slot].h : 0;
        if (!shown)
            slot = 0xFFFF;
        if (slot == v->drawn[i].slot && (slot == 0xFFFF || (x == v->drawn[i].x && y == v->drawn[i].y)))
            continue;
        if (v->drawn[i].slot != 0xFFFF)
            erase_texts(v, v->drawn[i].x, v->drawn[i].y, v->drawn[i].w, v->drawn[i].h, 1);
        if (slot != 0xFFFF)
            erase_texts(v, x, y, w, h, 1);
        v->drawn[i].slot = slot;
        v->drawn[i].x = x;
        v->drawn[i].y = y;
        v->drawn[i].w = w;
        v->drawn[i].h = h;
    }
}

static void draw(vortex *v)
{
    int i;
    memcpy(v->st.fb, v->bg, sizeof(v->bg));
    sprite_changes(v);
    draw_sprites(v);
    for (i = 0; i < v->text_count; i++)
        render_text(v, v->st.fb, v->texts[i].font, v->texts[i].x, v->texts[i].y, v->texts[i].w, v->texts[i].h,
                    v->texts[i].rgb, v->texts[i].text);
    v->frame++;
    for (i = 0; i < MAX_CLIPS; i++)
        if (v->clips[i].open && xf_player_frame(&v->clips[i].p))
            blit(v->st.fb, xf_player_frame(&v->clips[i].p), v->clips[i].p.video.width, v->clips[i].p.video.height,
                 v->clips[i].x, v->clips[i].y);
    draw_cursor(v);
}

int xf_vortex_run(const char *dir, const xf_game_options *opt)
{
    vortex *v = calloc(1, sizeof(*v));
    vs_host host;
    char path[1024];
    int r = 1, i;

    if (!v)
        return 1;
    if (stage_open(&v->st, dir, opt) != 0)
        goto done;
    for (i = 0; i < VS_SEQS; i++)
        v->drawn[i].slot = 0xFFFF;
    insert_disc(v, 1); /* the program may be on the first disc's folder */
    if (stage_path(&v->st, "VORTEX.EXE", path, sizeof(path)) != 0 || vexe_load(&v->exe, path) != 0) {
        xf_warn("%s: cannot read VORTEX.EXE", dir);
        goto done;
    }
    memset(&host, 0, sizeof(host));
    host.user = v;
    host.unknown = on_unknown;
    host.clip = on_clip;
    host.clip_op = on_clip_op;
    host.clip_move = on_clip_move;
    host.clip_time = on_clip_time;
    host.moved = on_moved;
    host.stopped = on_stopped;
    host.picture = on_picture;
    host.text = on_text;
    host.text_sprite = on_text_sprite;
    host.text_extent = on_text_extent;
    host.sprite_load = on_sprite_load;
    host.sprite_size = on_sprite_size;
    host.sprite_free = on_sprite_free;
    host.screen = on_screen;
    host.disc = on_disc;
    host.score = on_score;
    load_score(v);
    find_discs(v);
    host.sound = on_sound;
    host.sound_op = on_sound_op;
    host.sound_busy = on_sound_busy;
    host.music = on_music;
    vs_init(&v->vm, &v->exe, &host);
    while (!v->st.quit) {
        plat_event ev;
        while (stage_poll(&v->st, &ev)) {
            if (ev.type == PLAT_EV_MOUSE_DOWN)
                vs_click(&v->vm, ev.x, ev.y, ev.button == PLAT_MOUSE_RIGHT);
            else if (ev.type == PLAT_EV_KEY_DOWN)
                vs_key(&v->vm, ev.key);
        }
        vs_move(&v->vm, v->st.mx, v->st.my); /* sprites move under it too */
        update_clips(v);
        feed_channels(v);
        v->vm.now = plat_ticks_ms();
        vs_tick(&v->vm);
        draw(v);
        stage_frame(&v->st);
    }
    r = 0;
done:
    for (i = 0; i < MAX_CLIPS; i++)
        if (v->clips[i].open)
            xf_player_close(&v->clips[i].p);
    for (i = 0; i < 2; i++)
        stop_channel(v, i);
    for (i = 0; i < (int)XF_COUNT(v->sounds); i++)
        xf_wav_free(&v->sounds[i]);
    for (i = 0; i < (int)XF_COUNT(v->sprites); i++)
        free_sprite(v, i);
    free(v->score.names);
    free(v->score.cells);
    for (i = 0; i < (int)XF_COUNT(v->fonts); i++)
        if (v->fonts[i].data)
            xf_font_free(&v->fonts[i]);
    vexe_free(&v->exe);
    stage_close(&v->st);
    free(v);
    return r;
}
