/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* Entry point.
 *   xfiles --play <movie> [--data <game folder>]   play one movie
 *   xfiles --explore <game folder> <view> [disc] [--media <folder>]...
 *                                                   walk the scene graph; view 0
 *                                                   starts a new game; --media
 *                                                   adds folders to search for clips;
 *                                                   --script <file> plays a story script
 *   xfiles --game <folder> [--script <file>]        run whichever HyperBole game the
 *                                                   folder holds (src/games/games.h)
 * Without arguments it starts a new game from the first game folder it
 * finds (see find_game_dir), or shows a test pattern when there is none. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/mixer.h"
#include "games/xfiles/explore.h"
#include "games/games.h"
#include "gfx/text.h"
#include "media/player.h"
#include "platform/plat.h"
#include "xf_file.h"
#include "xf_lang.h"
#include "xf_log.h"

#define SCREEN_W 640
#define SCREEN_H 480
#define AUDIO_RATE 22050

static u32 g_fb[SCREEN_W * SCREEN_H];

static void blit_centered(const u32 *src, int w, int h)
{
    int x0 = (SCREEN_W - w) / 2, y0 = (SCREEN_H - h) / 2, y;
    for (y = 0; y < h; y++) {
        int sy = y + y0;
        if (sy < 0 || sy >= SCREEN_H)
            continue;
        if (x0 >= 0 && x0 + w <= SCREEN_W)
            memcpy(&g_fb[sy * SCREEN_W + x0], &src[y * w], (size_t)w * 4);
    }
}

/* Loads the dialogue font from the game folder. Installs name it
 * DLG.TTR or dlg.ttf. */
static int load_dialogue_font(const char *data_dir, xf_font *font)
{
    static const char *const names[] = { "DLG.TTR", "DLG.TTF" };
    char path[1024];
    size_t i;
    for (i = 0; i < XF_COUNT(names); i++)
        if (xf_path_resolve(data_dir, names[i], path, sizeof(path)) == 0 && xf_font_load(font, path) == 0)
            return 0;
    xf_warn("no dialogue font in %s; subtitles go to the console", data_dir);
    return -1;
}

static int play_movie(const char *path, const char *data_dir)
{
    xf_player player;
    xf_font font;
    xf_surface screen = { g_fb, SCREEN_W, SCREEN_H, SCREEN_W };
    const char *last_sub = NULL;
    int running = 1, have_font = 0;

    if (data_dir)
        have_font = load_dialogue_font(data_dir, &font) == 0;
    if (xf_player_open(&player, path, plat_ticks_ms()) != 0)
        return 1;
    while (running) {
        plat_event ev;
        const char *sub;
        while (plat_poll(&ev))
            if (ev.type == PLAT_EV_QUIT || ev.type == PLAT_EV_KEY_DOWN)
                running = 0;
        if (!xf_player_update(&player, plat_ticks_ms()))
            running = 0;
        memset(g_fb, 0, sizeof(g_fb));
        if (xf_player_frame(&player))
            blit_centered(xf_player_frame(&player), player.video.width, player.video.height);
        sub = xf_player_subtitle(&player);
        if (sub && have_font) {
            int bottom = (SCREEN_H + (xf_player_frame(&player) ? player.video.height : 0)) / 2;
            xf_text_draw_wrapped(&screen, &font, 20, SCREEN_W / 2, bottom + 30, SCREEN_W - 40, sub,
                                 0xFFFFFF, 0x000000);
        } else if (sub && sub != last_sub) {
            printf("[%u ms] %s\n", player.now_ms, sub);
        }
        last_sub = sub;
        plat_present(g_fb, SCREEN_W, SCREEN_H, SCREEN_W);
        plat_sleep_ms(5);
    }
    xf_player_close(&player);
    if (have_font) {
        xf_text_cache_clear();
        xf_font_free(&font);
    }
    return 0;
}

/* A folder holds the game when XFILES.HDB is in it, or in one of its
 * language folders (src/core/xf_lang.h). */
static int is_game_dir(const char *dir)
{
    return xf_detect_game(dir) != XF_GAME_NONE;
}

/* Runs an earlier game's module. */
static int run_module(const char *dir, const char *script)
{
    const xf_game_module *m = xf_game_module_for(xf_detect_game(dir));
    xf_game_options opt;
    if (!m || !m->run) {
        xf_warn("%s: no HyperBole game found there", dir);
        return 1;
    }
    opt.script = script;
    opt.media = NULL;
    return m->run(dir, &opt);
}

/* Where a release looks for the game: $XF_GAME_DIR, then a folder named
 * "game" beside the program, beside the app bundle (the program is in
 * X-Files.app/Contents/MacOS), or in the current folder. */
static const char *find_game_dir(const char *argv0, char *buf, size_t size)
{
    static const char *const rel[] = { "game", "../../../game" };
    const char *env = getenv("XF_GAME_DIR"), *slash = argv0 ? strrchr(argv0, '/') : NULL;
    size_t i;
#ifdef __vita__
    if (is_game_dir("ux0:data/xfiles"))
        return "ux0:data/xfiles";
#endif
    if (env && is_game_dir(env))
        return env;
    for (i = 0; slash && i < XF_COUNT(rel); i++) {
        snprintf(buf, size, "%.*s/%s", (int)(slash - argv0), argv0, rel[i]);
        if (is_game_dir(buf))
            return buf;
    }
    if (is_game_dir("game"))
        return "game";
    return NULL;
}

static void test_pattern(u32 quit_at)
{
    int running = 1, mx = 0, my = 0;
    while (running) {
        plat_event ev;
        u32 now = plat_ticks_ms();
        int x, y;
        while (plat_poll(&ev)) {
            if (ev.type == PLAT_EV_QUIT || (ev.type == PLAT_EV_KEY_DOWN && ev.key == PLAT_KEY_ESCAPE))
                running = 0;
            else if (ev.type == PLAT_EV_MOUSE_MOVE) {
                mx = ev.x;
                my = ev.y;
            }
        }
        if (quit_at && now >= quit_at)
            running = 0;
        for (y = 0; y < SCREEN_H; y++)
            for (x = 0; x < SCREEN_W; x++)
                g_fb[y * SCREEN_W + x] = (u32)((x + (int)now / 10) & 0xff) << 16 | (u32)(y & 0xff) << 8 | 0x40;
        for (y = my - 4; y <= my + 4; y++)
            for (x = mx - 4; x <= mx + 4; x++)
                if (x >= 0 && y >= 0 && x < SCREEN_W && y < SCREEN_H)
                    g_fb[y * SCREEN_W + x] = 0xffffff;
        plat_present(g_fb, SCREEN_W, SCREEN_H, SCREEN_W);
        plat_sleep_ms(16);
    }
}

int main(int argc, char **argv)
{
    int r = 0;

    char buf[1024];
    const char *dir = argc <= 1 ? find_game_dir(argc ? argv[0] : NULL, buf, sizeof(buf)) : NULL;
    xf_game_id game = argc > 2 && strcmp(argv[1], "--game") == 0 ? xf_detect_game(argv[2]) : xf_detect_game(dir);

    if (getenv("XF_DEBUG"))
        xf_log_set_level(XF_LOG_DEBUG);
    if (plat_init(game == XF_GAME_NONE ? "The X-Files" : xf_game_title(game), SCREEN_W, SCREEN_H) != 0)
        xf_fatal("cannot open window");
    xf_mixer_init(AUDIO_RATE, plat_audio_lock, plat_audio_unlock);
    if (plat_audio_open(AUDIO_RATE, xf_mixer_render, NULL) != 0)
        xf_warn("no audio output");

    if (argc > 3 && strcmp(argv[1], "--explore") == 0) {
        const char *media[8], *script = NULL;
        int i, n = 0, disc = 1;
        for (i = 4; i < argc; i++) {
            if (strcmp(argv[i], "--media") == 0 && i + 1 < argc && n < 7)
                media[n++] = argv[++i];
            else if (strcmp(argv[i], "--script") == 0 && i + 1 < argc)
                script = argv[++i];
            else
                disc = atoi(argv[i]);
        }
        media[n] = NULL;
        r = xf_explore(argv[2], media, (u32)strtoul(argv[3], NULL, 10), disc, script);
    }
    else if (argc > 2 && strcmp(argv[1], "--play") == 0)
        r = play_movie(argv[2], argc > 4 && strcmp(argv[3], "--data") == 0 ? argv[4] : NULL);
    else if (argc > 2 && strcmp(argv[1], "--game") == 0 && game != XF_GAME_XFILES)
        r = run_module(argv[2], argc > 4 && strcmp(argv[3], "--script") == 0 ? argv[4] : NULL);
    else if (dir && game != XF_GAME_XFILES)
        r = run_module(dir, NULL);
    else {
        if (argc > 2 && strcmp(argv[1], "--game") == 0)
            dir = argv[2];
        if (dir) {
            /* XF_MEDIA_DIR adds a folder of clips, for installs that
             * keep them apart (the DVD build streams them from the disc). */
            /* The CD's clips can also go in a folder named media inside
             * the game folder: the Vita has no environment variables. */
            const char *media[4];
            static char extra[1024];
            int n = 0;
            snprintf(extra, sizeof(extra), "%s/media", dir);
            media[n++] = dir;
            if (getenv("XF_MEDIA_DIR"))
                media[n++] = getenv("XF_MEDIA_DIR");
            media[n++] = extra;
            media[n] = NULL;
            r = xf_explore(dir, media, 0, 1, NULL);
        } else {
            if (argc <= 1)
                xf_warn("no game folder: set XF_GAME_DIR or put the game files in a folder named "
                        "\"game\" beside the program");
            test_pattern(argc > 1 ? (u32)atoi(argv[1]) : 0);
        }
    }
    plat_shutdown();
    return r;
}
