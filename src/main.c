/* Entry point. For now it opens the window, draws a test pattern and
 * plays a short tone, to prove the platform layer works. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "platform/plat.h"
#include "xf_log.h"

#define SCREEN_W 640
#define SCREEN_H 480
#define AUDIO_RATE 22050

static u32 g_fb[SCREEN_W * SCREEN_H];
static double g_phase;
static int g_tone_frames = AUDIO_RATE / 2;

static void tone(void *user, i16 *out, int frames)
{
    int i;
    for (i = 0; i < frames; i++) {
        i16 s = 0;
        if (g_tone_frames > 0) {
            s = (i16)(sin(g_phase) * 6000);
            g_phase += 2 * M_PI * 440.0 / AUDIO_RATE;
            g_tone_frames--;
        }
        out[i * 2] = out[i * 2 + 1] = s;
    }
}

static void draw_pattern(int t, int mx, int my)
{
    int x, y;
    for (y = 0; y < SCREEN_H; y++)
        for (x = 0; x < SCREEN_W; x++)
            g_fb[y * SCREEN_W + x] = (u32)((x + t) & 0xff) << 16 | (u32)(y & 0xff) << 8 | 0x40;
    for (y = my - 4; y <= my + 4; y++)
        for (x = mx - 4; x <= mx + 4; x++)
            if (x >= 0 && y >= 0 && x < SCREEN_W && y < SCREEN_H)
                g_fb[y * SCREEN_W + x] = 0xffffff;
}

int main(int argc, char **argv)
{
    int running = 1, mx = 0, my = 0;
    u32 quit_at = argc > 1 ? (u32)atoi(argv[1]) : 0;

    if (plat_init("The X-Files", SCREEN_W, SCREEN_H) != 0)
        xf_fatal("cannot open window");
    if (plat_audio_open(AUDIO_RATE, tone, NULL) != 0)
        xf_warn("no audio output");

    while (running) {
        plat_event ev;
        u32 now = plat_ticks_ms();
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
        draw_pattern((int)(now / 10), mx, my);
        plat_present(g_fb, SCREEN_W, SCREEN_H, SCREEN_W);
        plat_sleep_ms(16);
    }
    plat_shutdown();
    return 0;
}
