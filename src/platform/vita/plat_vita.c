/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* PlayStation Vita backend (VitaSDK):
 *
 *   Video     SceDisplay 960x544, 32-bit, double-buffered in CDRAM. A
 *             960x544 frame (the wide layout) is shown as it is; the
 *             640x480 one is scaled to 725x544 and centred.
 *   Input     The front touch panel points and clicks. The left stick moves
 *             the pointer, Cross clicks, Circle right-clicks, Start is
 *             Escape. The d-pad moves between the things that take a
 *             click; Square clicks too, Triangle shows the inventory, L and
 *             R step to the previous and next one.
 *   Sound     One BGM port at the engine's rate, fed by a thread.
 *
 * The game files live in ux0:data/xfiles, outside the VPK. Based on the
 * Vita backend of the Ecstatica engine (same author, GPL-3.0-or-later). */
#include <psp2/audioout.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/touch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/plat.h"
#include "xf_jobs.h"
#include "xf_log.h"

/* Clips, pictures and the database live in newlib's heap. */
int _newlib_heap_size_user = 192 * 1024 * 1024;
unsigned int sceUserMainThreadStackSize = 1024 * 1024;

#define SCREEN_W 960
#define SCREEN_H 544
#define FB_BYTES ((SCREEN_W * SCREEN_H * 4 + 0x3FFFF) & ~0x3FFFF)
#define EVENT_QUEUE_SIZE 64

static SceUID g_fb_mem[2];
static u32 *g_fb[2];
static int g_back;
static int g_width, g_height, g_view_x, g_view_w;
static int *g_col_map;
static plat_event g_events[EVENT_QUEUE_SIZE];
static int g_ev_head, g_ev_tail;
static u32 g_buttons;
static int g_touching;
static float g_px, g_py; /* pointer, in frame pixels */

void plat_set_pointer(int x, int y)
{
    g_px = (float)x;
    g_py = (float)y;
}

static void push_event(int type, int button, int key)
{
    int next = (g_ev_head + 1) % EVENT_QUEUE_SIZE;
    plat_event *ev = &g_events[g_ev_head];
    if (next == g_ev_tail)
        return;
    memset(ev, 0, sizeof(*ev));
    ev->type = type;
    ev->button = button;
    ev->key = key;
    ev->x = (int)g_px;
    ev->y = (int)g_py;
    g_ev_head = next;
}

/* Fits a frame of this size to the screen height, centred. */
static int fit_frame(int width, int height)
{
    int i, *map;
    int view_w = XF_MIN(SCREEN_W, SCREEN_H * width / height);
    if (!(map = realloc(g_col_map, sizeof(int) * (size_t)view_w)))
        return -1;
    g_col_map = map;
    if (g_width && g_height) {
        /* the pointer keeps its place on the screen */
        g_px = g_px * width / g_width;
        g_py = g_py * height / g_height;
    }
    g_width = width;
    g_height = height;
    g_view_w = view_w;
    g_view_x = (SCREEN_W - g_view_w) / 2;
    for (i = 0; i < g_view_w; i++)
        g_col_map[i] = i * width / g_view_w;
    return 0;
}

void plat_output_size(int *width, int *height)
{
    *width = SCREEN_W;
    *height = SCREEN_H;
}

int plat_init(const char *title, int width, int height)
{
    int i;
    (void)title;
    {
        /* With ux0:data/xfiles/profile.txt the log goes to xfiles.log
         * there (the profile lines included). */
        SceIoStat st;
        if (sceIoGetstat("ux0:data/xfiles/profile.txt", &st) >= 0) {
            FILE *f = fopen("ux0:data/xfiles/xfiles.log", "w");
            if (f) /* flushed a line at a time: the app can be closed at any moment */
                xf_log_set_file(f);
        }
    }
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    for (i = 0; i < 2; i++) {
        g_fb_mem[i] = sceKernelAllocMemBlock("fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, FB_BYTES, NULL);
        if (g_fb_mem[i] < 0)
            return -1;
        sceKernelGetMemBlockBase(g_fb_mem[i], (void **)&g_fb[i]);
        memset(g_fb[i], 0, SCREEN_W * SCREEN_H * 4);
    }
    /* The 4:3 picture fills the height. */
    if (fit_frame(width, height) != 0)
        return -1;
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    g_px = width / 2.0f;
    g_py = height / 2.0f;
    return 0;
}

void plat_shutdown(void)
{
    int i;
    plat_audio_close();
    for (i = 0; i < 2; i++)
        if (g_fb_mem[i] > 0)
            sceKernelFreeMemBlock(g_fb_mem[i]);
    free(g_col_map);
}

static void pump_input(void)
{
    static const struct { u32 bit; int key; } keys[] = {
        { SCE_CTRL_START, PLAT_KEY_ESCAPE }, { SCE_CTRL_SELECT, PLAT_KEY_F1 },
        { SCE_CTRL_LEFT, PLAT_KEY_LEFT },    { SCE_CTRL_RIGHT, PLAT_KEY_RIGHT },
        { SCE_CTRL_UP, PLAT_KEY_UP },        { SCE_CTRL_DOWN, PLAT_KEY_DOWN },
        { SCE_CTRL_SQUARE, PLAT_KEY_RETURN }, { SCE_CTRL_TRIANGLE, 'i' },
        { SCE_CTRL_RTRIGGER, 9 },            { SCE_CTRL_LTRIGGER, 9 } };
    SceCtrlData pad;
    SceTouchData touch;
    u32 pressed, released;
    int dx, dy, moved = 0;
    size_t i;

    sceCtrlPeekBufferPositive(0, &pad, 1);
    /* Left stick: the pointer, faster further out; a dead zone near the
     * centre. */
    dx = (int)pad.lx - 128;
    dy = (int)pad.ly - 128;
    if (dx * dx + dy * dy > 30 * 30) {
        g_px += dx / 16.0f;
        g_py += dy / 16.0f;
        moved = 1;
    }
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
    if (touch.reportNum > 0) {
        /* The panel reports 1920x1088; map it to the picture. */
        float sx = touch.report[0].x / 2.0f, sy = touch.report[0].y / 2.0f;
        g_px = (sx - g_view_x) * g_width / g_view_w;
        g_py = sy * g_height / SCREEN_H;
        moved = 1;
    }
    if (g_px < 0) g_px = 0;
    if (g_py < 0) g_py = 0;
    if (g_px > g_width - 1) g_px = (float)(g_width - 1);
    if (g_py > g_height - 1) g_py = (float)(g_height - 1);
    if (moved)
        push_event(PLAT_EV_MOUSE_MOVE, 0, 0);
    /* A tap is a left click where the finger touched. */
    if (touch.reportNum > 0 && !g_touching)
        push_event(PLAT_EV_MOUSE_DOWN, PLAT_MOUSE_LEFT, 0);
    if (touch.reportNum == 0 && g_touching)
        push_event(PLAT_EV_MOUSE_UP, PLAT_MOUSE_LEFT, 0);
    g_touching = touch.reportNum > 0;

    pressed = pad.buttons & ~g_buttons;
    released = g_buttons & ~pad.buttons;
    g_buttons = pad.buttons;
    if (pressed & SCE_CTRL_CROSS)
        push_event(PLAT_EV_MOUSE_DOWN, PLAT_MOUSE_LEFT, 0);
    if (released & SCE_CTRL_CROSS)
        push_event(PLAT_EV_MOUSE_UP, PLAT_MOUSE_LEFT, 0);
    if (pressed & SCE_CTRL_CIRCLE)
        push_event(PLAT_EV_MOUSE_DOWN, PLAT_MOUSE_RIGHT, 0);
    if (released & SCE_CTRL_CIRCLE)
        push_event(PLAT_EV_MOUSE_UP, PLAT_MOUSE_RIGHT, 0);
    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (pressed & keys[i].bit) {
            push_event(PLAT_EV_KEY_DOWN, 0, keys[i].key);
            if (keys[i].bit == SCE_CTRL_LTRIGGER) /* L: Shift+Tab, the previous one */
                g_events[(g_ev_head + EVENT_QUEUE_SIZE - 1) % EVENT_QUEUE_SIZE].mods = PLAT_MOD_SHIFT;
        }
        if (released & keys[i].bit)
            push_event(PLAT_EV_KEY_UP, 0, keys[i].key);
    }
}

int plat_poll(plat_event *ev)
{
    if (g_ev_tail == g_ev_head)
        pump_input();
    if (g_ev_tail == g_ev_head)
        return 0;
    *ev = g_events[g_ev_tail];
    g_ev_tail = (g_ev_tail + 1) % EVENT_QUEUE_SIZE;
    return 1;
}

typedef struct {
    const u32 *pixels;
    int height, pitch;
    u32 *dst;
} present_job;

/* One band of display rows: scaled by the column and row maps and
 * turned from 0x00RRGGBB into the display's 0xAABBGGRR. The framebuffer
 * is uncached CDRAM, slow to write a pixel at a time: each row is built
 * in cached memory and copied out in one go, and a row that repeats the
 * source row of the one above is only copied again. */
static void present_band(void *ctx, int part, int parts)
{
    const present_job *j = ctx;
    int y, x, y0 = SCREEN_H * part / parts, y1 = SCREEN_H * (part + 1) / parts, last = -1;
    u32 line[SCREEN_W];
    for (y = y0; y < y1; y++) {
        int sy = y * j->height / SCREEN_H;
        if (sy != last) {
            const u32 *src = j->pixels + (size_t)sy * (size_t)j->pitch;
            for (x = 0; x < g_view_w; x++) {
                u32 c = src[g_col_map[x]];
                line[x] = 0xFF000000u | (c & 0x00FF00u) | (c >> 16 & 0xFFu) | (c & 0xFFu) << 16;
            }
            last = sy;
        }
        memcpy(j->dst + (size_t)y * SCREEN_W + g_view_x, line, (size_t)g_view_w * 4);
    }
}

void plat_present(const u32 *pixels, int width, int height, int pitch)
{
    SceDisplayFrameBuf fb;
    present_job j;
    int x;
    if ((width != g_width || height != g_height) && fit_frame(width, height) == 0)
        for (x = 0; x < 2; x++) /* clear the bars of both buffers */
            memset(g_fb[x], 0, SCREEN_W * SCREEN_H * 4);
    j.pixels = pixels;
    j.height = height;
    j.pitch = pitch;
    j.dst = g_fb[g_back];
    xf_jobs_run(present_band, &j, xf_jobs_count());
    memset(&fb, 0, sizeof(fb));
    fb.size = sizeof(fb);
    fb.base = j.dst;
    fb.pitch = SCREEN_W;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = SCREEN_W;
    fb.height = SCREEN_H;
    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
    g_back ^= 1;
}

u32 plat_ticks_ms(void) { return (u32)(sceKernelGetProcessTimeWide() / 1000); }

void plat_sleep_ms(u32 ms) { sceKernelDelayThread(ms * 1000); }

/* --- Sound ---------------------------------------------------------- */

#define AUDIO_FRAMES 1024

static int g_port = -1;
static SceUID g_audio_thread = -1, g_audio_mutex = -1;
static volatile int g_audio_run;
static plat_audio_fn g_audio_fn;
static void *g_audio_user;
static i16 g_audio_buf[2][AUDIO_FRAMES * 2];

static int audio_main(SceSize args, void *argp)
{
    int b = 0;
    (void)args;
    (void)argp;
    while (g_audio_run) {
        sceKernelLockMutex(g_audio_mutex, 1, NULL);
        g_audio_fn(g_audio_user, g_audio_buf[b], AUDIO_FRAMES);
        sceKernelUnlockMutex(g_audio_mutex, 1);
        sceAudioOutOutput(g_port, g_audio_buf[b]); /* blocks until played */
        b ^= 1;
    }
    return 0;
}

int plat_audio_open(int rate, plat_audio_fn fn, void *user)
{
    g_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, AUDIO_FRAMES, rate, SCE_AUDIO_OUT_MODE_STEREO);
    if (g_port < 0)
        return -1;
    g_audio_fn = fn;
    g_audio_user = user;
    g_audio_mutex = sceKernelCreateMutex("audio", 0, 0, NULL);
    g_audio_run = 1;
    g_audio_thread = sceKernelCreateThread("audio", audio_main, 0x10000100, 0x10000, 0, 0, NULL);
    if (g_audio_thread < 0)
        return -1;
    sceKernelStartThread(g_audio_thread, 0, NULL);
    return 0;
}

void plat_audio_close(void)
{
    if (g_port < 0)
        return;
    g_audio_run = 0;
    if (g_audio_thread >= 0) {
        sceKernelWaitThreadEnd(g_audio_thread, NULL, NULL);
        sceKernelDeleteThread(g_audio_thread);
    }
    sceAudioOutReleasePort(g_port);
    if (g_audio_mutex >= 0)
        sceKernelDeleteMutex(g_audio_mutex);
    g_port = -1;
    g_audio_thread = g_audio_mutex = -1;
}

void plat_audio_lock(void)
{
    if (g_audio_mutex >= 0)
        sceKernelLockMutex(g_audio_mutex, 1, NULL);
}

void plat_audio_unlock(void)
{
    if (g_audio_mutex >= 0)
        sceKernelUnlockMutex(g_audio_mutex, 1);
}

/* --- Fonts and cursor --------------------------------------------------- */

/* The Vita has no system TrueType fonts the game can use; a copy of
 * cour.ttf in the game folder works. */
int plat_font_path(const char *family, char *out, size_t size)
{
    SceIoStat st;
    if (strcmp(family, "Courier New") != 0)
        return -1;
    snprintf(out, size, "ux0:data/xfiles/cour.ttf");
    return sceIoGetstat(out, &st) >= 0 ? 0 : -1;
}

void plat_show_cursor(int show) { (void)show; }

/* plat_save_dir: see plat.h. */
int plat_save_dir(char *out, size_t size)
{
    if (size < 32)
        return -1;
    snprintf(out, size, "ux0:data/xfiles");
    sceIoMkdir(out, 0777);
    snprintf(out, size, "ux0:data/xfiles/saves");
    sceIoMkdir(out, 0777);
    return 0;
}
