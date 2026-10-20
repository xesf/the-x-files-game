/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* PlayStation Vita backend (VitaSDK):
 *
 *   Video     The GPU (vita_gpu.c) draws the frame scaled with bilinear
 *             filtering, the 640x480 one to 725x544, centred. Without it,
 *             the CPU scales into a SceDisplay framebuffer in CDRAM.
 *   Input     The front touch panel moves the pointer; a short tap clicks.
 *             Either stick moves the pointer, Cross clicks, Circle right-clicks, Start is
 *             Escape. A text field opens the system keyboard. The d-pad moves between the things that take a
 *             click; Square clicks too, Triangle shows the inventory, L and
 *             R step to the previous and next one.
 *   Sound     One BGM port at the engine's rate, fed by a thread.
 *
 * The game files live in ux0:data/xfiles, outside the VPK. Based on the
 * Vita backend of the Ecstatica engine (same author, GPL-3.0-or-later). */
#include <psp2/audioout.h>
#include <psp2/common_dialog.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/ime_dialog.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/touch.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/plat.h"
#include "vita_gpu.h"
#include "xf_jobs.h"
#include "xf_log.h"

/* vita-elf-create appends 2 KB of import data to the code segment, and
 * fails ("segment 1 overlaps") when the code ends just short of the 64
 * KB boundary where the data segment starts. This pad moved the end
 * away from it; if the error comes back, change its size. */
__attribute__((used)) static const char g_segment_pad[4096] = { 1 };

/* Clips, pictures and the database live in newlib's heap. */
int _newlib_heap_size_user = 192 * 1024 * 1024;
unsigned int sceUserMainThreadStackSize = 1024 * 1024;

#define SCREEN_W 960
#define SCREEN_H 544
#define FB_BYTES ((SCREEN_W * SCREEN_H * 4 + 0x3FFFF) & ~0x3FFFF)
#define EVENT_QUEUE_SIZE 256 /* room for a typed line */
#define TEXT_MAX 63

static int g_gpu;
static SceUID g_fb_mem[2];
static u32 *g_fb[2];
static int g_back;
static int g_width, g_height, g_view_x, g_view_w;
static int *g_col_map;
static plat_event g_events[EVENT_QUEUE_SIZE];
static int g_ev_head, g_ev_tail;
static u32 g_buttons;
static int g_touching;
static u32 g_touch_start; /* when the finger came down, and where */
static float g_touch_x, g_touch_y;
static float g_px, g_py; /* pointer, in frame pixels */
static u32 g_last_pump;
static int g_ime_open, g_ime_clear;
static SceWChar16 g_ime_title[32], g_ime_initial[TEXT_MAX + 1], g_ime_text[TEXT_MAX + 1];

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
    g_gpu = vita_gpu_init() == 0;
    if (!g_gpu) {
        vita_gpu_shutdown();
        for (i = 0; i < 2; i++) {
            g_fb_mem[i] = sceKernelAllocMemBlock("fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, FB_BYTES, NULL);
            if (g_fb_mem[i] < 0)
                return -1;
            sceKernelGetMemBlockBase(g_fb_mem[i], (void **)&g_fb[i]);
            memset(g_fb[i], 0, SCREEN_W * SCREEN_H * 4);
        }
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
    vita_gpu_shutdown();
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
    u32 pressed, released, now;
    float dt;
    int moved = 0;
    size_t i;

    sceCtrlPeekBufferPositive(0, &pad, 1);
    now = plat_ticks_ms();
    dt = g_last_pump ? (float)(now - g_last_pump) / 1000.0f : 0.0f;
    if (dt > 0.1f)
        dt = 0.1f;
    g_last_pump = now;
    /* Either stick: the pointer, from 120 pixels a second just past the
     * dead zone (the sticks rest a little off centre) up to 900 at a full
     * push, faster towards the edge; the same speed at any frame rate. */
    for (i = 0; i < 2; i++) {
        float sx = ((i ? pad.rx : pad.lx) - 128) / 127.0f, sy = ((i ? pad.ry : pad.ly) - 128) / 127.0f;
        float r = sqrtf(sx * sx + sy * sy), k, speed;
        if (r > 0.18f) {
            k = (r > 1.0f ? 1.0f : r) - 0.18f;
            k /= 0.82f;
            speed = 120.0f + 780.0f * k * sqrtf(k);
            g_px += sx / r * speed * dt;
            g_py += sy / r * speed * dt;
            moved = 1;
        }
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
    /* The finger is the pointer: touching and sliding moves it, so things
     * highlight under it; a short tap that hardly moved is a left click
     * where it lifted. */
    if (touch.reportNum > 0 && !g_touching) {
        g_touch_start = now;
        g_touch_x = g_px;
        g_touch_y = g_py;
    }
    if (touch.reportNum == 0 && g_touching && now - g_touch_start < 300 &&
        fabsf(g_px - g_touch_x) < 12 && fabsf(g_py - g_touch_y) < 12) {
        push_event(PLAT_EV_MOUSE_DOWN, PLAT_MOUSE_LEFT, 0);
        push_event(PLAT_EV_MOUSE_UP, PLAT_MOUSE_LEFT, 0);
    }
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

/* The keyboard is up until the player closes it; then the text arrives
 * as key presses. */
static void ime_poll(void)
{
    SceImeDialogResult res;
    int i;
    if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED)
        return;
    memset(&res, 0, sizeof(res));
    sceImeDialogGetResult(&res);
    if (res.button == SCE_IME_DIALOG_BUTTON_ENTER) {
        for (i = 0; i < g_ime_clear; i++) {
            push_event(PLAT_EV_KEY_DOWN, 0, 8);
            push_event(PLAT_EV_KEY_UP, 0, 8);
        }
        for (i = 0; i < TEXT_MAX && g_ime_text[i]; i++) {
            int c = g_ime_text[i], mods = 0;
            if (c < 32 || c > 126)
                continue; /* the fields take plain ASCII */
            if (c >= 'A' && c <= 'Z') {
                c += 'a' - 'A';
                mods = PLAT_MOD_SHIFT;
            }
            push_event(PLAT_EV_KEY_DOWN, 0, c);
            g_events[(g_ev_head + EVENT_QUEUE_SIZE - 1) % EVENT_QUEUE_SIZE].mods = mods;
            push_event(PLAT_EV_KEY_UP, 0, c);
        }
    }
    sceImeDialogTerm();
    vita_gpu_dialog(0);
    g_ime_open = 0;
    {
        /* A button or touch that closed the keyboard does not press again;
         * the click that opened it still gets its release. */
        SceCtrlData pad;
        SceTouchData touch;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
        g_buttons |= pad.buttons;
        g_touching |= touch.reportNum > 0;
    }
    g_last_pump = 0;
}

static void to_utf16(SceWChar16 *out, const char *in, int size)
{
    int i;
    for (i = 0; i < size - 1 && in[i]; i++)
        out[i] = (unsigned char)in[i];
    out[i] = 0;
}

int plat_text_input(const char *title, const char *initial, int password, int clear)
{
    static int configured;
    SceImeDialogParam p;
    if (!g_gpu || g_ime_open)
        return 0; /* the dialog draws itself through the GPU */
    if (!configured) {
        SceCommonDialogConfigParam config;
        sceCommonDialogConfigParamInit(&config); /* the system's language and Enter button */
        sceCommonDialogSetConfigParam(&config);
        configured = 1;
    }
    to_utf16(g_ime_title, title, 32);
    to_utf16(g_ime_initial, initial, TEXT_MAX + 1);
    memcpy(g_ime_text, g_ime_initial, sizeof(g_ime_text));
    sceImeDialogParamInit(&p);
    p.supportedLanguages = SCE_IME_LANGUAGE_ENGLISH;
    p.languagesForced = SCE_TRUE;
    p.type = SCE_IME_TYPE_BASIC_LATIN;
    p.option = SCE_IME_OPTION_NO_AUTO_CAPITALIZATION | SCE_IME_OPTION_NO_ASSISTANCE;
    p.textBoxMode = password ? SCE_IME_DIALOG_TEXTBOX_MODE_PASSWORD : SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
    p.title = g_ime_title;
    p.maxTextLength = TEXT_MAX;
    p.initialText = g_ime_initial;
    p.inputTextBuffer = g_ime_text;
    if (sceImeDialogInit(&p) < 0)
        return 0;
    g_ime_open = 1;
    g_ime_clear = clear;
    vita_gpu_dialog(1);
    return 1;
}

int plat_poll(plat_event *ev)
{
    if (g_ime_open)
        ime_poll();
    if (g_ev_tail == g_ev_head && !g_ime_open)
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
    if (g_gpu) {
        if (width != g_width || height != g_height)
            fit_frame(width, height);
        vita_gpu_present(pixels, width, height, pitch, g_view_x, 0, g_view_w, SCREEN_H);
        return;
    }
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

/* --- MIDI --------------------------------------------------------------- */

/* No system synthesizer here yet: the earlier games play without music
 * (docs/OTHER-GAMES.md). */
int plat_midi_open(void) { return -1; }
void plat_midi_send(u8 status, u8 d1, u8 d2) { (void)status; (void)d1; (void)d2; }
void plat_midi_close(void) {}
