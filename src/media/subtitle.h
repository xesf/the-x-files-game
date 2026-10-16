/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* Subtitles from the QuickTime text tracks of a scene. */
#ifndef XF_SUBTITLE_H
#define XF_SUBTITLE_H

#include "media/mov.h"

typedef struct {
    u32 start_ms, end_ms; /* movie time */
    u32 track_id;
    char *text;           /* UTF-8 */
} xf_subtitle;

typedef struct {
    u32 count;
    xf_subtitle *cues; /* sorted by start time */
} xf_subtitles;

/* Collects the lines of every text track, placed in movie time by the
 * edit lists. Returns 0 on success (also when there are no lines). */
int xf_subtitles_load(xf_mov *mov, xf_subtitles *subs);
void xf_subtitles_free(xf_subtitles *subs);
/* Returns the first cue visible at `ms`, or NULL. */
const xf_subtitle *xf_subtitles_at(const xf_subtitles *subs, u32 ms);

/* Reads an SRT file (UTF-8): the captions of a translation. */
int xf_subtitles_load_srt(const char *path, xf_subtitles *subs);
/* Scales the times of the lines from a clip `from_ms` long to one
 * `to_ms` long: a dubbed clip runs a little shorter or longer. */
void xf_subtitles_stretch(xf_subtitles *subs, u32 from_ms, u32 to_ms);
/* The lines as the player shows them: one at a time (the first visible,
 * xf_subtitles_at), the same line across tracks merged. */
int xf_subtitles_shown(const xf_subtitles *in, xf_subtitles *out);

#endif
