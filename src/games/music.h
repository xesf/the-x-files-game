/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* Plays a Standard MIDI file through the platform's synthesizer
 * (plat_midi_*), timed from the main loop. */
#ifndef XF_MUSIC_H
#define XF_MUSIC_H

#include "media/midi.h"

typedef struct {
    xf_midi_song song;
    int playing, plays, open; /* plays: how many times, 0 for ever */
    u32 start_ms;     /* wall clock at the song's start */
    u32 next;         /* next event */
    int volume;       /* 0 to 127, scales note velocities */
} xf_music;

/* Starts a song, played `plays` times (0: for ever); stops the one
 * playing. Returns 0, or -1 (no file, or no synthesizer: the game goes
 * on without music). */
int xf_music_play(xf_music *m, const char *path, int plays, u32 now_ms);
/* Sends the events due by `now_ms`. Call once a frame. */
void xf_music_update(xf_music *m, u32 now_ms);
void xf_music_stop(xf_music *m);
/* Stops and closes the synthesizer. */
void xf_music_close(xf_music *m);

#endif
