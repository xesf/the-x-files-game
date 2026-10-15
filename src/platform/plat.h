/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf and contributors */
/* Platform layer. Each backend implements these functions with the
 * operating system's own APIs. The engine never calls the OS directly. */
#ifndef XF_PLAT_H
#define XF_PLAT_H

#include "xf_types.h"

enum {
    PLAT_EV_NONE,
    PLAT_EV_QUIT,
    PLAT_EV_MOUSE_MOVE,
    PLAT_EV_MOUSE_DOWN,
    PLAT_EV_MOUSE_UP,
    PLAT_EV_KEY_DOWN,
    PLAT_EV_KEY_UP
};

enum { PLAT_MOUSE_LEFT, PLAT_MOUSE_RIGHT };

/* Key codes the engine understands. Printable keys use their ASCII code. */
enum {
    PLAT_KEY_ESCAPE = 27,
    PLAT_KEY_RETURN = 13,
    PLAT_KEY_SPACE = 32,
    PLAT_KEY_LEFT = 0x100,
    PLAT_KEY_RIGHT,
    PLAT_KEY_UP,
    PLAT_KEY_DOWN,
    PLAT_KEY_F1
};

/* Modifier keys held during a key event. */
enum { PLAT_MOD_SHIFT = 1, PLAT_MOD_CTRL = 2, PLAT_MOD_ALT = 4, PLAT_MOD_CMD = 8 };

typedef struct {
    int type;
    int x, y;   /* mouse position in framebuffer pixels */
    int button; /* PLAT_MOUSE_* */
    int key;    /* PLAT_KEY_* or ASCII, lower case for letters */
    int mods;   /* PLAT_MOD_* */
} plat_event;

/* Called on the audio thread. Fills `frames` stereo frames of signed
 * 16-bit samples at the rate given to plat_audio_open. */
typedef void (*plat_audio_fn)(void *user, i16 *out, int frames);

int plat_init(const char *title, int width, int height);
void plat_shutdown(void);

/* Returns 1 and fills `ev` while events are pending, then 0. */
int plat_poll(plat_event *ev);

/* Shows a frame. Pixels are 0x00RRGGBB, `pitch` is in pixels. */
void plat_present(const u32 *pixels, int width, int height, int pitch);

/* Finds an installed system font by family name ("Courier New"). Returns
 * 0 and its path, or -1 when the system has no such font. */
int plat_font_path(const char *family, char *out, size_t size);

/* Shows or hides the system pointer over the window (the game draws its
 * own cursors). */
void plat_show_cursor(int show);

u32 plat_ticks_ms(void);
void plat_sleep_ms(u32 ms);

int plat_audio_open(int rate, plat_audio_fn fn, void *user);
void plat_audio_close(void);
void plat_audio_lock(void);
void plat_audio_unlock(void);

#endif
