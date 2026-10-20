/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
#include "stage.h"

#include <stdlib.h>
#include <string.h>

#include "audio/mixer.h"
#include "media/player.h"
#include "media/wav.h"
#include "xf_file.h"
#include "xf_log.h"

int stage_open(xf_stage *s, const char *dir, const xf_game_options *opt)
{
    memset(s, 0, sizeof(*s));
    s->dir = dir;
    s->music.volume = 100;
    if (opt && opt->script && !(s->script = fopen(opt->script, "r"))) {
        xf_warn("%s: cannot open the script", opt->script);
        return -1;
    }
    return 0;
}

void stage_close(xf_stage *s)
{
    size_t i;
    for (i = 0; i < XF_COUNT(s->sounds); i++)
        if (s->sounds[i].wav.samples) {
            xf_mixer_close(s->sounds[i].channel);
            xf_wav_free(&s->sounds[i].wav);
        }
    xf_music_close(&s->music);
    if (s->script)
        fclose(s->script);
}

int stage_path(const xf_stage *s, const char *rel, char *out, size_t size)
{
    char p[512];
    size_t i;
    snprintf(p, sizeof(p), "%s", rel);
    for (i = 0; p[i]; i++)
        if (p[i] == '\\')
            p[i] = '/';
    return xf_path_resolve(s->dir, p, out, size);
}

/* Writes the screen as a 24-bit BMP. */
static void save_shot(xf_stage *s, const char *path)
{
    FILE *f = fopen(path, "wb");
    u8 h[54] = { 'B', 'M' }, line[STAGE_W * 3];
    u32 size = 54 + STAGE_W * 3 * STAGE_H;
    int x, y;
    if (!f)
        return;
    h[2] = (u8)size; h[3] = (u8)(size >> 8); h[4] = (u8)(size >> 16);
    h[10] = 54; h[14] = 40;
    h[18] = STAGE_W & 0xFF; h[19] = STAGE_W >> 8;
    h[22] = STAGE_H & 0xFF; h[23] = STAGE_H >> 8;
    h[26] = 1; h[28] = 24;
    fwrite(h, 1, 54, f);
    for (y = STAGE_H - 1; y >= 0; y--) {
        for (x = 0; x < STAGE_W; x++) {
            u32 c = s->fb[y * STAGE_W + x];
            line[x * 3] = (u8)c;
            line[x * 3 + 1] = (u8)(c >> 8);
            line[x * 3 + 2] = (u8)(c >> 16);
        }
        fwrite(line, 1, sizeof(line), f);
    }
    fclose(f);
}

/* The script: one command a line, run in order.
 *   wait <ms>        let the game run
 *   point <x> <y>    move the pointer
 *   press <x> <y>    click there
 *   key <c>          press a key (a character, or esc, return, space)
 *   shot <file.bmp>  save the screen
 *   quit             end the game
 * Lines starting with # are comments. */
static int script_event(xf_stage *s, plat_event *ev)
{
    char cmd[32], arg[200];
    int a, b;
    if (!s->script || (s->script_wait && (i32)(plat_ticks_ms() - s->script_wait) < 0))
        return 0;
    s->script_wait = 0;
    while (fgets(s->script_line, sizeof(s->script_line), s->script)) {
        memset(ev, 0, sizeof(*ev));
        arg[0] = '\0';
        if (sscanf(s->script_line, "%31s %199s", cmd, arg) < 1 || cmd[0] == '#')
            continue;
        if (strcmp(cmd, "wait") == 0) {
            s->script_wait = plat_ticks_ms() + (u32)atoi(arg);
            return 0;
        }
        if ((strcmp(cmd, "point") == 0 || strcmp(cmd, "press") == 0) &&
            sscanf(s->script_line, "%*s %d %d", &a, &b) == 2) {
            s->mx = a;
            s->my = b;
            ev->type = cmd[1] == 'o' ? PLAT_EV_MOUSE_MOVE : PLAT_EV_MOUSE_DOWN;
            ev->x = a;
            ev->y = b;
            return 1;
        }
        if (strcmp(cmd, "key") == 0) {
            ev->type = PLAT_EV_KEY_DOWN;
            ev->key = strcmp(arg, "esc") == 0      ? PLAT_KEY_ESCAPE
                      : strcmp(arg, "return") == 0 ? PLAT_KEY_RETURN
                      : strcmp(arg, "space") == 0  ? PLAT_KEY_SPACE
                                                   : arg[0];
            return 1;
        }
        if (strcmp(cmd, "shot") == 0) {
            save_shot(s, arg);
            continue;
        }
        if (strcmp(cmd, "quit") == 0)
            break;
        xf_warn("script: unknown command %s", cmd);
    }
    s->quit = 1; /* the script has ended */
    fclose(s->script);
    s->script = NULL;
    ev->type = PLAT_EV_QUIT;
    return 1;
}

static void feed_sounds(xf_stage *s);

int stage_poll(xf_stage *s, plat_event *ev)
{
    xf_music_update(&s->music, plat_ticks_ms());
    feed_sounds(s);
    if (script_event(s, ev))
        return 1;
    if (!plat_poll(ev))
        return 0;
    if (ev->type == PLAT_EV_QUIT)
        s->quit = 1;
    if (ev->type == PLAT_EV_MOUSE_MOVE || ev->type == PLAT_EV_MOUSE_DOWN) {
        s->mx = ev->x;
        s->my = ev->y;
    }
    return 1;
}

void stage_present(xf_stage *s) { plat_present(s->fb, STAGE_W, STAGE_H, STAGE_W); }

void stage_frame(xf_stage *s)
{
    stage_present(s);
    plat_sleep_ms(16);
}

void stage_clear(xf_stage *s, u32 color)
{
    size_t i;
    for (i = 0; i < XF_COUNT(s->fb); i++)
        s->fb[i] = color;
}

static void blit(xf_stage *s, const u32 *src, int w, int h, int x0, int y0)
{
    int x, y;
    for (y = 0; y < h; y++) {
        int sy = y0 + y;
        if (sy < 0 || sy >= STAGE_H)
            continue;
        for (x = 0; x < w; x++) {
            int sx = x0 + x;
            if (sx >= 0 && sx < STAGE_W)
                s->fb[sy * STAGE_W + sx] = src[(size_t)y * w + x];
        }
    }
}

int stage_play_clip(xf_stage *s, const char *rel, int x, int y, int skippable)
{
    char path[1024];
    xf_player p;
    int skipped = 0;
    if (stage_path(s, rel, path, sizeof(path)) != 0 || xf_player_open(&p, path, plat_ticks_ms()) != 0) {
        xf_warn("clip %s: not found", rel);
        return -1;
    }
    xf_info("clip %s", rel);
    while (!s->quit && xf_player_update(&p, plat_ticks_ms())) {
        plat_event ev;
        while (stage_poll(s, &ev))
            if (skippable && (ev.type == PLAT_EV_MOUSE_DOWN || ev.type == PLAT_EV_KEY_DOWN))
                skipped = 1;
        if (skipped)
            break;
        if (xf_player_frame(&p))
            blit(s, xf_player_frame(&p), p.video.width, p.video.height,
                 x < 0 ? (STAGE_W - p.video.width) / 2 : x, y < 0 ? (STAGE_H - p.video.height) / 2 : y);
        stage_frame(s);
    }
    xf_player_close(&p);
    return skipped;
}

/* Tops up the sounds playing; frees the finished ones. */
static void feed_sounds(xf_stage *s)
{
    size_t i;
    for (i = 0; i < XF_COUNT(s->sounds); i++) {
        stage_sound *d = &s->sounds[i];
        if (!d->wav.samples)
            continue;
        if (d->written < d->wav.frames) {
            d->written += xf_mixer_write(d->channel, d->wav.samples + (size_t)d->written * d->wav.channels,
                                         d->wav.frames - d->written);
            if (d->written >= d->wav.frames)
                xf_mixer_finish(d->channel);
        } else if (!xf_mixer_active(d->channel))
            xf_wav_free(&d->wav);
    }
}

int stage_play_sound(xf_stage *s, const char *rel)
{
    char path[1024];
    size_t i;
    for (i = 0; i < XF_COUNT(s->sounds) && s->sounds[i].wav.samples; i++)
        ;
    if (i == XF_COUNT(s->sounds) || stage_path(s, rel, path, sizeof(path)) != 0 ||
        xf_wav_load(path, &s->sounds[i].wav) != 0)
        return -1;
    s->sounds[i].channel = xf_mixer_open((int)s->sounds[i].wav.rate, s->sounds[i].wav.channels);
    s->sounds[i].written = 0;
    if (s->sounds[i].channel < 0) {
        xf_wav_free(&s->sounds[i].wav);
        return -1;
    }
    feed_sounds(s);
    return s->sounds[i].channel;
}

int stage_draw_bmp(xf_stage *s, const char *rel, int x, int y)
{
    char path[1024];
    xf_bmp b;
    if (stage_path(s, rel, path, sizeof(path)) != 0 || xf_bmp_load(path, &b) != 0) {
        xf_warn("picture %s: not found", rel);
        return -1;
    }
    blit(s, b.pixels, b.width, b.height, x, y);
    xf_bmp_free(&b);
    return 0;
}

int stage_wait_input(xf_stage *s, u32 ms, int *x, int *y)
{
    u32 until = plat_ticks_ms() + ms;
    while (!s->quit && (!ms || (i32)(plat_ticks_ms() - until) < 0)) {
        plat_event ev;
        while (stage_poll(s, &ev)) {
            if (ev.type == PLAT_EV_MOUSE_DOWN) {
                if (x)
                    *x = ev.x;
                if (y)
                    *y = ev.y;
                return 1;
            }
            if (ev.type == PLAT_EV_KEY_DOWN)
                return 2;
        }
        stage_frame(s);
    }
    return 0;
}

void stage_music(xf_stage *s, const char *rel, int loop)
{
    char path[1024];
    if (!rel || stage_path(s, rel, path, sizeof(path)) != 0) {
        xf_music_stop(&s->music);
        return;
    }
    if (xf_music_play(&s->music, path, loop, plat_ticks_ms()) != 0)
        xf_info("music %s: not played", rel);
}
