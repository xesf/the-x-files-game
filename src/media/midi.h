/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* Standard MIDI files, the music of Quantum Gate and Vortex
 * (see docs/formats/midi.md): the tracks merged into one list of
 * channel messages timed in microseconds. */
#ifndef XF_MIDI_H
#define XF_MIDI_H

#include "xf_types.h"

typedef struct {
    u32 us;           /* time from the start */
    u8 status, d1, d2;
} xf_midi_event;

typedef struct {
    xf_midi_event *events; /* in time order */
    u32 count;
    u32 length_us;    /* the end of the last track */
    int format, tracks, division;
} xf_midi_song;

/* Returns 0, or -1 for data that is not a Standard MIDI File. */
int xf_midi_parse(const u8 *data, size_t size, xf_midi_song *out);
int xf_midi_load(const char *path, xf_midi_song *out);
void xf_midi_free(xf_midi_song *s);

#endif
