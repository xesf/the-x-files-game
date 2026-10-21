/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* Vortex's "uber" device (VORTEX.EXE segment 13, "uber.c";
 * docs/vortex/README.md): three moods, each a history of values with
 * their total, and a log of events that a two-page book shows. */
#ifndef XF_VUBER_H
#define XF_VUBER_H

#include "xf_types.h"

#define VU_MOODS 3
#define VU_NONE 9999

/* An event of the log: its mood, its id and seven words for the scripts
 * (string resources, then a rectangle). */
typedef struct {
    u16 mood, id, w[7];
} vu_event;

typedef struct {
    int ready;               /* DS:754 */
    struct {
        u32 total;           /* the sum of its values */
        u16 positives;       /* how many were above 0 */
        u8 *values;          /* the history, oldest first */
        int count, cap;
    } mood[VU_MOODS];
    vu_event *events;
    int event_count, event_cap;
    int cursor;              /* DS:21F6: kept from one start to the next */
    u16 def_mood;            /* DS:21FA */
    u8 seen[169];            /* DS:756: events logged once */
    int page;                /* DS:9FA: the book's page, -1 before the first */
    int turned;              /* DS:9FC */
    int book_count, book_pages;
    int *book;               /* left, right event of each page */
} vu_state;

void vu_init(vu_state *u);
void vu_free(vu_state *u);
/* seg13:10C2: the 24 events every game starts with (0x805 sub 1, and at
 * start). Returns 1. */
int vu_seed(vu_state *u);
/* seg13:0268: value v for mood m (0xFF: none) and an event (id 9999 and
 * w[0] 9999: none). */
int vu_record(vu_state *u, int m, u8 v, u16 id, const u16 *w);
/* seg13:05EA: whether mood m has a share of tol or more of the values
 * (mode 1 the newest, 2 the newest n, 3 the totals, 4 the newest n
 * counted twice). */
int vu_share(const vu_state *u, int m, int mode, int n, float tol);
/* seg13:089E: the mood with the most, by the same modes. */
int vu_dominant(const vu_state *u, int mode, int n);
/* seg13:0C64: whether two moods' totals differ by tol of the sum or less. */
int vu_balanced(const vu_state *u, int m1, int m2, float tol);
/* seg13:0DFA: a mood's colour at a level 0 to 3, 0x00RRGGBB. */
u32 vu_colour(const vu_state *u, int mood, int level);
/* seg13:0FC4: the cursor to the first (1), last (2), next (3) or
 * previous (4) event; -1 when it cannot move. */
int vu_move(vu_state *u, int mode);
void vu_mark_seen(vu_state *u, u16 id);
/* The book (seg13:1A64, 1C40, 1D52): open returns the page kept; turn
 * goes back (1) or on (2) and gives the events of both sides. */
int vu_open(vu_state *u);
void vu_close(vu_state *u);
void vu_turn(vu_state *u, int dir, vu_event *left, vu_event *right);

#endif
