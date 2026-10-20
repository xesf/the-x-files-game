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
    u32 bg[STAGE_W * STAGE_H]; /* the background pictures */
    vx_clip clips[MAX_CLIPS];
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
} vortex;

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
    xf_player_close(&c->p);
    c->open = 0;
    vs_set(&v->vm, c->seq, VC_MOVIE, 0);
    vs_set(&v->vm, c->seq, VC_MOVIE + 2, 0);
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
    return 0;
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
static void on_text(void *user, int font, int x, int y, int w, int h, u32 rgb, const char *text)
{
    vortex *v = user;
    const xf_font *f = font_for(v, font);
    xf_surface s;
    char utf8[1024];
    float px;
    int starts[32], lens[32], lines, i, line_h, ascent;
    if (!f || !*text)
        return;
    xf_debug("vortex: text, font %d, at (%d, %d) %dx%d: %s", font, x, y, w, h, text);
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
    s.pixels = v->bg;
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

/* 0x12C: pictures on the background. */
static void on_picture(void *user, int seq, int sub, const char *file, int x, int y)
{
    vortex *v = user;
    char path[1024];
    xf_bmp b;
    (void)seq;
    if (!file) {
        if (sub == 2)
            memset(v->bg, 0, sizeof(v->bg));
        return;
    }
    if (stage_path(&v->st, file, path, sizeof(path)) != 0 || xf_bmp_load(path, &b) != 0) {
        xf_warn("vortex: picture %s not found", file);
        return;
    }
    xf_info("vortex: picture %s at (%d, %d), sub-type %d", file, x, y, sub);
    blit(v->bg, b.pixels, b.width, b.height, x, y);
    xf_bmp_free(&b);
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
        stage_music(&v->st, file, 1);
    else if (sub == 6)
        stage_music(&v->st, NULL, 0);
}

/* --- The loop ------------------------------------------------------------- */

static void update_clips(vortex *v)
{
    int i;
    for (i = 0; i < MAX_CLIPS; i++) {
        vx_clip *c = &v->clips[i];
        if (!c->open)
            continue;
        if (!xf_player_update(&c->p, plat_ticks_ms())) {
            int seq = c->seq;
            stop_clip(v, c);
            vs_event(&v->vm, seq, 1); /* the clip ended */
        }
    }
}

static void draw(vortex *v)
{
    int i;
    memcpy(v->st.fb, v->bg, sizeof(v->bg));
    for (i = 0; i < MAX_CLIPS; i++)
        if (v->clips[i].open && xf_player_frame(&v->clips[i].p))
            blit(v->st.fb, xf_player_frame(&v->clips[i].p), v->clips[i].p.video.width, v->clips[i].p.video.height,
                 v->clips[i].x, v->clips[i].y);
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
    if (stage_path(&v->st, "VORTEX.EXE", path, sizeof(path)) != 0 || vexe_load(&v->exe, path) != 0) {
        xf_warn("%s: cannot read VORTEX.EXE", dir);
        goto done;
    }
    memset(&host, 0, sizeof(host));
    host.user = v;
    host.unknown = on_unknown;
    host.clip = on_clip;
    host.clip_op = on_clip_op;
    host.moved = on_moved;
    host.picture = on_picture;
    host.text = on_text;
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
    for (i = 0; i < (int)XF_COUNT(v->fonts); i++)
        if (v->fonts[i].data)
            xf_font_free(&v->fonts[i]);
    vexe_free(&v->exe);
    stage_close(&v->st);
    free(v);
    return r;
}
