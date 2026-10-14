/* Entry point.
 *   xfiles --play <movie> [--data <game folder>]   play one movie
 *   xfiles --explore <game folder> <view> [disc]   walk the scene graph;
 *                                                   view 0 starts a new game
 * Without arguments it shows a test pattern. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/mixer.h"
#include "game/explore.h"
#include "gfx/text.h"
#include "media/player.h"
#include "platform/plat.h"
#include "xf_file.h"
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

    if (getenv("XF_DEBUG"))
        xf_log_set_level(XF_LOG_DEBUG);
    if (plat_init("The X-Files", SCREEN_W, SCREEN_H) != 0)
        xf_fatal("cannot open window");
    xf_mixer_init(AUDIO_RATE, plat_audio_lock, plat_audio_unlock);
    if (plat_audio_open(AUDIO_RATE, xf_mixer_render, NULL) != 0)
        xf_warn("no audio output");

    if (argc > 3 && strcmp(argv[1], "--explore") == 0)
        r = xf_explore(argv[2], (u32)strtoul(argv[3], NULL, 10), argc > 4 ? atoi(argv[4]) : 1);
    else if (argc > 2 && strcmp(argv[1], "--play") == 0)
        r = play_movie(argv[2], argc > 4 && strcmp(argv[3], "--data") == 0 ? argv[4] : NULL);
    else
        test_pattern(argc > 1 ? (u32)atoi(argv[1]) : 0);
    plat_shutdown();
    return r;
}
