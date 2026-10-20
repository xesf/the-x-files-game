/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* Vortex's sequence interpreter ("sequence.c", VORTEX.EXE segment 8;
 * docs/vortex/README.md): up to 45 sequences run side by side, each
 * with a context laid out as the original's, since operands can name any
 * of its words. */
#ifndef XF_VSEQ_H
#define XF_VSEQ_H

#include "games/vortex/vexe.h"

#define VS_SEQS 45
#define VS_CTX_WORDS (0x136 / 2)
#define VS_GLOBALS 0xE1
#define VS_SPRITES 0x5D  /* sprite slots (DS:46DC, 14 bytes each) */

/* Context fields, as byte offsets (seg8). */
enum {
    VC_PARENT = 0x00,    /* the sequence that started this one */
    VC_INDEX = 0x02,     /* its own slot */
    VC_USED = 0x04,
    VC_SPRITE = 0x06,    /* sprite slot, 0xFFFF for none */
    VC_X = 0x0A, VC_Y = 0x0C, VC_Z = 0x0E, VC_W = 0x10, VC_H = 0x12,
    VC_PC = 0x14,        /* far pointer: offset, then segment */
    VC_TIMER = 0x18,     /* four words */
    VC_ID = 0x24,        /* its number, which other instructions name */
    VC_FLAGS = 0x26,
    VC_HALTED = 0x2A,
    VC_WAIT = 0x2E,      /* dword: the events 0xD5 waits for */
    VC_HANDLED = 0x32,   /* dword: events with a handler (0x74 + 4 * bit) */
    VC_EVENTS = 0x36,    /* dword: events pending */
    VC_WAIT2 = 0x3A,     /* dword: the signals 0xD7 waits for */
    VC_HANDLED2 = 0x3E,  /* dword: signals with a handler (0xB8 + 4 * bit) */
    VC_EVENTS2 = 0x42,
    VC_MOVIE_WAIT = 0x48,
    VC_MOVIE = 0x5C,     /* the clip this sequence plays (nonzero while it does) */
    VC_LOCALS = 0x64,    /* six words */
    VC_VECTORS = 0x70,   /* event handlers, far pointers, bit 0 first */
    VC_VECTORS2 = 0xB8,
    VC_STACK = 0x100,    /* return addresses, eight far pointers */
};

/* What the interpreter asks of the game (the module implements them). */
typedef struct vs_host {
    void *user;
    /* An instruction the interpreter does not know yet: logged, and the
     * sequence stops. */
    void (*unknown)(void *user, int seq, u16 op, vx_ptr at);
    /* Media: clips, pictures, sounds, music, text. */
    int (*clip)(void *user, int seq, int sub, const char *file, int x, int y, const u16 *w, u8 b0, u8 b1);
    /* 0x197: an action on a sequence's clip */
    void (*clip_op)(void *user, int seq, int action, u16 a, u16 b);
    void (*picture)(void *user, int seq, int sub, const char *file, int x, int y);
    /* text onto the background: a font (seg14:042E), a box (0 wide: one
     * line at x, y), a colour 0x00RRGGBB; Windows-1252 */
    void (*text)(void *user, int font, int x, int y, int w, int h, u32 rgb, const char *s);
    /* 0x1F5: loads a sound, returns its handle (0: none) */
    int (*sound)(void *user, int sub, const char *file, u16 arg);
    /* 0x1F6 plays handle on channel args[0] (1, 2; 3 either) at
     * priority args[1]; 0x1F7 frees it; 0x1F8 stops channel `handle` */
    void (*sound_op)(void *user, u16 op, u16 handle, const u16 *args);
    /* whether a sound channel (1, 2) plays */
    int (*sound_busy)(void *user, int channel);
    void (*music)(void *user, int sub, const char *file, u16 arg);
    void (*cursor)(void *user, u16 a, u16 b, u8 c, const char *file);
    /* 0x3: a sequence's state (its clip) moved to another slot */
    void (*moved)(void *user, int from, int to);
    /* 0x12E to 0x138: a screen update (transition) of a rectangle with
     * an effect; for the others than 0x12E, the rectangle is 0, 0, 0, 0
     * for now */
    void (*screen)(void *user, int seq, u16 op, int effect, int x, int y, int w, int h);
    /* 0x7E4, 0x7E5: makes sprite `slot` an image of a text; its size into
     * *w, *h. Returns 0, or -1. */
    int (*text_sprite)(void *user, int slot, int font, u32 rgb, const char *text, int *w, int *h);
    /* 0x805: a call into segment 13 (the "uber" device), by sub-command */
    u16 (*uber)(void *user, int seq, int sub, vx_ptr at);
} vs_host;

typedef struct {
    const vexe *x;
    vs_host host;
    u16 ctx[VS_SEQS][VS_CTX_WORDS];
    u16 g[0x100];        /* globals (DS:39F8) */
    u32 events;          /* DS:4ED6: the events this tick */
    u32 signals;         /* DS:4F60: the running sequence's signals (context + 0x42) */
    vx_ptr labels[13];   /* DS:46A8: 0x101's targets, set by 0x100 */
    u32 rand_seed;
    u32 now;             /* the clock, ms (GetTickCount), set before each tick */
    u32 sleep_start[VS_SEQS];
    u32 ticks;
    int depth;           /* instructions run this tick by the sequence (DS:44F0) */
    int unknown_count;
} vs_state;

void vs_init(vs_state *s, const vexe *x, const vs_host *host);
/* One tick: every sequence runs until it waits, at most five instructions. */
void vs_tick(vs_state *s);
/* Raises events (bits of DS:4ED6) for the waiting sequences, e.g. a clip
 * that ended (bit 0). */
void vs_event(vs_state *s, int seq, u32 bits);

/* The player's input (seg4's window procedure): a click gives every
 * sequence event 0x2, and the nearest sequence (lowest z) waiting for
 * or handling 0x4 to 0x10 under the pointer 0xC (left) or 0x14
 * (right). A key gives every sequence 0x400; Escape gives 0x8000 to the
 * ones that handle it. */
void vs_click(vs_state *s, int x, int y, int right);
void vs_key(vs_state *s, int key);

u16 vs_get(const vs_state *s, int seq, int off);
void vs_set(vs_state *s, int seq, int off, u16 v);
vx_ptr vs_get_ptr(const vs_state *s, int seq, int off);
void vs_set_ptr(vs_state *s, int seq, int off, vx_ptr p);

#endif
