/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf and contributors */
/* Linux backend (desktop, Steam Deck, Steam Machine): an X11 window with a
 * software blit scaled to the window, and ALSA sound on its own thread.
 * Under Wayland it runs through XWayland. */
#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "platform/plat.h"

static Display *g_dpy;
static Window g_win;
static GC g_gc;
static XImage *g_image;
static u32 *g_scaled;
static int g_win_w, g_win_h, g_img_w, g_img_h;
static int g_width, g_height;
static Atom g_wm_delete;
static Cursor g_blank;
static int g_headless;
static struct timespec g_start;

/* The 4:3 picture inside the window: offset and size. */
static int g_view_x, g_view_y, g_view_w, g_view_h;

static void fit_view(void)
{
    int w = g_win_w, h = g_win_w * g_height / g_width;
    if (h > g_win_h) {
        h = g_win_h;
        w = g_win_h * g_width / g_height;
    }
    g_view_w = w > 0 ? w : 1;
    g_view_h = h > 0 ? h : 1;
    g_view_x = (g_win_w - g_view_w) / 2;
    g_view_y = (g_win_h - g_view_h) / 2;
}

int plat_init(const char *title, int width, int height)
{
    XSetWindowAttributes attr;
    XSizeHints hints;
    char empty[8] = { 0 };
    Pixmap pix;
    XColor black;
    int screen;

    g_width = width;
    g_height = height;
    clock_gettime(CLOCK_MONOTONIC, &g_start);
    /* XF_HEADLESS: no window and no sound, for scripted test runs. */
    g_headless = getenv("XF_HEADLESS") != NULL;
    if (g_headless)
        return 0;
    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy)
        return -1;
    screen = DefaultScreen(g_dpy);
    g_win_w = width * 2;
    g_win_h = height * 2;
    memset(&attr, 0, sizeof(attr));
    attr.background_pixel = BlackPixel(g_dpy, screen);
    attr.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
                      PointerMotionMask | StructureNotifyMask;
    g_win = XCreateWindow(g_dpy, RootWindow(g_dpy, screen), 0, 0, (unsigned)g_win_w, (unsigned)g_win_h, 0,
                          CopyFromParent, InputOutput, CopyFromParent, CWBackPixel | CWEventMask, &attr);
    XStoreName(g_dpy, g_win, title);
    memset(&hints, 0, sizeof(hints));
    hints.flags = PMinSize;
    hints.min_width = width;
    hints.min_height = height;
    XSetWMNormalHints(g_dpy, g_win, &hints);
    g_wm_delete = XInternAtom(g_dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(g_dpy, g_win, &g_wm_delete, 1);
    g_gc = XCreateGC(g_dpy, g_win, 0, NULL);
    pix = XCreateBitmapFromData(g_dpy, g_win, empty, 8, 8);
    memset(&black, 0, sizeof(black));
    g_blank = XCreatePixmapCursor(g_dpy, pix, pix, &black, &black, 0, 0);
    XFreePixmap(g_dpy, pix);
    XMapWindow(g_dpy, g_win);
    fit_view();
    return 0;
}

static void free_image(void)
{
    if (g_image) {
        g_image->data = NULL; /* g_scaled is ours */
        XDestroyImage(g_image);
        g_image = NULL;
    }
    free(g_scaled);
    g_scaled = NULL;
}

void plat_shutdown(void)
{
    plat_audio_close();
    if (!g_dpy)
        return;
    free_image();
    XFreeCursor(g_dpy, g_blank);
    XFreeGC(g_dpy, g_gc);
    XDestroyWindow(g_dpy, g_win);
    XCloseDisplay(g_dpy);
    g_dpy = NULL;
}

static int map_key(XKeyEvent *e)
{
    KeySym sym = XLookupKeysym(e, 0);
    switch (sym) {
    case XK_Escape: return PLAT_KEY_ESCAPE;
    case XK_Return:
    case XK_KP_Enter: return PLAT_KEY_RETURN;
    case XK_space: return PLAT_KEY_SPACE;
    case XK_Left: return PLAT_KEY_LEFT;
    case XK_Right: return PLAT_KEY_RIGHT;
    case XK_Up: return PLAT_KEY_UP;
    case XK_Down: return PLAT_KEY_DOWN;
    case XK_F1: return PLAT_KEY_F1;
    case XK_BackSpace: return 8;
    }
    if (sym >= 32 && sym < 127)
        return sym >= 'A' && sym <= 'Z' ? (int)(sym - 'A' + 'a') : (int)sym;
    return 0;
}

static int map_mods(unsigned state)
{
    return (state & ShiftMask ? PLAT_MOD_SHIFT : 0) | (state & ControlMask ? PLAT_MOD_CTRL : 0) |
           (state & Mod1Mask ? PLAT_MOD_ALT : 0) | (state & Mod4Mask ? PLAT_MOD_CMD : 0);
}

/* Window pixels to framebuffer pixels. */
static void to_frame(int wx, int wy, int *x, int *y)
{
    *x = (wx - g_view_x) * g_width / g_view_w;
    *y = (wy - g_view_y) * g_height / g_view_h;
}

int plat_poll(plat_event *ev)
{
    if (g_headless || !g_dpy)
        return 0;
    while (XPending(g_dpy)) {
        XEvent e;
        XNextEvent(g_dpy, &e);
        memset(ev, 0, sizeof(*ev));
        switch (e.type) {
        case ClientMessage:
            if ((Atom)e.xclient.data.l[0] == g_wm_delete) {
                ev->type = PLAT_EV_QUIT;
                return 1;
            }
            break;
        case ConfigureNotify:
            if (e.xconfigure.width != g_win_w || e.xconfigure.height != g_win_h) {
                g_win_w = e.xconfigure.width;
                g_win_h = e.xconfigure.height;
                fit_view();
                XClearWindow(g_dpy, g_win);
            }
            break;
        case MotionNotify:
            ev->type = PLAT_EV_MOUSE_MOVE;
            to_frame(e.xmotion.x, e.xmotion.y, &ev->x, &ev->y);
            return 1;
        case ButtonPress:
        case ButtonRelease:
            if (e.xbutton.button != Button1 && e.xbutton.button != Button3)
                break;
            ev->type = e.type == ButtonPress ? PLAT_EV_MOUSE_DOWN : PLAT_EV_MOUSE_UP;
            ev->button = e.xbutton.button == Button1 ? PLAT_MOUSE_LEFT : PLAT_MOUSE_RIGHT;
            to_frame(e.xbutton.x, e.xbutton.y, &ev->x, &ev->y);
            return 1;
        case KeyPress:
        case KeyRelease:
            /* X repeats keys as a release and a press at the same time. */
            if (e.type == KeyRelease && XEventsQueued(g_dpy, QueuedAfterReading)) {
                XEvent next;
                XPeekEvent(g_dpy, &next);
                if (next.type == KeyPress && next.xkey.time == e.xkey.time &&
                    next.xkey.keycode == e.xkey.keycode) {
                    XNextEvent(g_dpy, &next);
                    break;
                }
            }
            ev->type = e.type == KeyPress ? PLAT_EV_KEY_DOWN : PLAT_EV_KEY_UP;
            ev->key = map_key(&e.xkey);
            ev->mods = map_mods(e.xkey.state);
            if (ev->key)
                return 1;
            break;
        }
    }
    return 0;
}

void plat_present(const u32 *pixels, int width, int height, int pitch)
{
    int x, y;
    if (g_headless || !g_dpy)
        return;
    if (!g_image || g_img_w != g_view_w || g_img_h != g_view_h) {
        Visual *vis = DefaultVisual(g_dpy, DefaultScreen(g_dpy));
        free_image();
        g_scaled = malloc((size_t)g_view_w * (size_t)g_view_h * 4);
        if (!g_scaled)
            return;
        g_image = XCreateImage(g_dpy, vis, (unsigned)DefaultDepth(g_dpy, DefaultScreen(g_dpy)), ZPixmap, 0,
                               (char *)g_scaled, (unsigned)g_view_w, (unsigned)g_view_h, 32, g_view_w * 4);
        g_img_w = g_view_w;
        g_img_h = g_view_h;
    }
    /* Nearest-neighbour scale; the frame is 0x00RRGGBB like a 24-bit
     * TrueColor visual. */
    for (y = 0; y < g_view_h; y++) {
        const u32 *src = pixels + (size_t)(y * height / g_view_h) * (size_t)pitch;
        u32 *dst = g_scaled + (size_t)y * (size_t)g_view_w;
        for (x = 0; x < g_view_w; x++)
            dst[x] = src[x * width / g_view_w];
    }
    XPutImage(g_dpy, g_win, g_gc, g_image, 0, 0, g_view_x, g_view_y, (unsigned)g_view_w, (unsigned)g_view_h);
    XFlush(g_dpy);
}

u32 plat_ticks_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (u32)((t.tv_sec - g_start.tv_sec) * 1000 + (t.tv_nsec - g_start.tv_nsec) / 1000000);
}

void plat_sleep_ms(u32 ms) { usleep(ms * 1000); }

/* --- Sound ---------------------------------------------------------- */

#define AUDIO_FRAMES 1024

static snd_pcm_t *g_pcm;
static pthread_t g_audio_thread;
static pthread_mutex_t g_audio_mutex = PTHREAD_MUTEX_INITIALIZER;
static plat_audio_fn g_audio_fn;
static void *g_audio_user;
static volatile int g_audio_run;

static void *audio_main(void *arg)
{
    static i16 buf[AUDIO_FRAMES * 2];
    (void)arg;
    while (g_audio_run) {
        snd_pcm_sframes_t n;
        pthread_mutex_lock(&g_audio_mutex);
        g_audio_fn(g_audio_user, buf, AUDIO_FRAMES);
        pthread_mutex_unlock(&g_audio_mutex);
        n = snd_pcm_writei(g_pcm, buf, AUDIO_FRAMES);
        if (n < 0)
            snd_pcm_recover(g_pcm, (int)n, 1);
    }
    return NULL;
}

int plat_audio_open(int rate, plat_audio_fn fn, void *user)
{
    if (g_headless || snd_pcm_open(&g_pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0)
        return -1;
    if (snd_pcm_set_params(g_pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, (unsigned)rate, 1,
                           60000) < 0) {
        snd_pcm_close(g_pcm);
        g_pcm = NULL;
        return -1;
    }
    g_audio_fn = fn;
    g_audio_user = user;
    g_audio_run = 1;
    if (pthread_create(&g_audio_thread, NULL, audio_main, NULL) != 0) {
        snd_pcm_close(g_pcm);
        g_pcm = NULL;
        return -1;
    }
    return 0;
}

void plat_audio_close(void)
{
    if (!g_pcm)
        return;
    g_audio_run = 0;
    pthread_join(g_audio_thread, NULL);
    snd_pcm_drain(g_pcm);
    snd_pcm_close(g_pcm);
    g_pcm = NULL;
}

void plat_audio_lock(void) { pthread_mutex_lock(&g_audio_mutex); }
void plat_audio_unlock(void) { pthread_mutex_unlock(&g_audio_mutex); }

/* --- Fonts and cursor --------------------------------------------------- */

/* Courier New if installed (msttcorefonts), else the metric-compatible
 * Liberation fonts that most distributions ship. */
int plat_font_path(const char *family, char *out, size_t size)
{
    static const char *const dirs[] = { "/usr/share/fonts/truetype/msttcorefonts", "/usr/share/fonts/TTF",
                                        "/usr/share/fonts/truetype/liberation", "/usr/share/fonts/liberation",
                                        "/usr/share/fonts/truetype", "/usr/local/share/fonts" };
    const char *alt = strcmp(family, "Courier New") == 0     ? "LiberationMono-Regular"
                      : strcmp(family, "Arial") == 0         ? "LiberationSans-Regular"
                      : strcmp(family, "Times New Roman") == 0 ? "LiberationSerif-Regular"
                                                             : NULL;
    size_t i;
    for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        snprintf(out, size, "%s/%s.ttf", dirs[i], family);
        if (access(out, R_OK) == 0)
            return 0;
        if (alt) {
            snprintf(out, size, "%s/%s.ttf", dirs[i], alt);
            if (access(out, R_OK) == 0)
                return 0;
        }
    }
    return -1;
}

void plat_show_cursor(int show)
{
    static int hidden;
    if (g_headless || !g_dpy || hidden == !show)
        return;
    if (show)
        XUndefineCursor(g_dpy, g_win);
    else
        XDefineCursor(g_dpy, g_win, g_blank);
    hidden = !show;
}
