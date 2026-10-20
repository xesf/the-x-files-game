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
} vortex;

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
    host.picture = on_picture;
    host.sound = on_sound;
    host.sound_op = on_sound_op;
    host.sound_busy = on_sound_busy;
    host.music = on_music;
    vs_init(&v->vm, &v->exe, &host);
    while (!v->st.quit) {
        plat_event ev;
        while (stage_poll(&v->st, &ev))
            if (ev.type == PLAT_EV_KEY_DOWN && ev.key == PLAT_KEY_ESCAPE)
                v->st.quit = 1;
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
    vexe_free(&v->exe);
    stage_close(&v->st);
    free(v);
    return r;
}
