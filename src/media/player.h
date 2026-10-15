/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* Plays a QuickTime movie: decodes video to the current time and feeds
 * the sound to the mixer. The sound drives the clock when there is any. */
#ifndef XF_PLAYER_H
#define XF_PLAYER_H

#include "media/audio.h"
#include "media/mov.h"
#include "media/subtitle.h"
#include "media/video.h"

typedef struct {
    xf_mov mov;
    xf_video video;
    int has_video;
    xf_audio_stream audio;
    int has_audio;
    int channel;        /* mixer channel, or -1 */
    u32 audio_skip;     /* frames to drop before the edit list start */
    int audio_done;
    i16 *pcm;
    xf_subtitles subs;
    u32 duration_ms;
    u32 base_ms;        /* where the range starts in the movie */
    u32 start_ticks;    /* wall clock at start, for movies without sound */
    u32 now_ms;
    int finished;
    int paused;
    u32 pause_ticks;    /* wall clock when paused */
} xf_player;

int xf_player_open(xf_player *p, const char *path, u32 now_ticks);
/* Plays only start_ms..end_ms of the movie (end_ms 0: to the end). Media
 * references name such ranges, for example XV/90178.xmv 59100..60767. */
int xf_player_open_range(xf_player *p, const char *path, u32 now_ticks, u32 start_ms, u32 end_ms);
void xf_player_close(xf_player *p);
/* Advances to wall-clock time `now_ticks` (ms). Returns 1 while playing. */
int xf_player_update(xf_player *p, u32 now_ticks);
/* Holds the movie, picture and sound, at `now_ticks`; resuming carries
 * on from the same place. */
void xf_player_pause(xf_player *p, int paused, u32 now_ticks);
/* Current picture, or NULL for a movie without video. */
const u32 *xf_player_frame(const xf_player *p);
/* Subtitle line to show now, or NULL. */
const char *xf_player_subtitle(const xf_player *p);

#endif
