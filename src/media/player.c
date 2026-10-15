/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf and contributors */
#include "player.h"

#include <stdlib.h>
#include <string.h>

#include "audio/mixer.h"
#include "xf_log.h"

int xf_player_open(xf_player *p, const char *path, u32 now_ticks)
{
    xf_mov_track *t;

    memset(p, 0, sizeof(*p));
    p->channel = -1;
    if (xf_mov_open(&p->mov, path) != 0)
        return -1;
    p->duration_ms = (u32)((u64)p->mov.duration * 1000 / p->mov.timescale);

    t = xf_mov_find(&p->mov, XF_TRACK_VIDEO);
    if (t && xf_video_open(&p->video, &p->mov, t) == 0)
        p->has_video = 1;

    t = xf_mov_find(&p->mov, XF_TRACK_AUDIO);
    if (t && xf_audio_open(&p->audio, &p->mov, t) == 0) {
        p->pcm = malloc((size_t)xf_audio_max_chunk_frames(&p->audio) * t->channels * 2 + 2);
        p->channel = p->pcm ? xf_mixer_open((int)t->sample_rate, t->channels) : -1;
        if (p->channel >= 0) {
            p->has_audio = 1;
            p->audio_skip = xf_audio_seek(&p->audio, xf_mov_media_start(t));
        } else {
            xf_audio_close(&p->audio);
        }
    } else if (t) {
        char cc[5];
        xf_warn("player: sound codec %s not supported", xf_fourcc_str(t->codec, cc));
    }
    xf_subtitles_load(&p->mov, &p->subs);
    p->start_ticks = now_ticks;
    return 0;
}

void xf_player_close(xf_player *p)
{
    if (p->channel >= 0)
        xf_mixer_close(p->channel);
    if (p->has_audio)
        xf_audio_close(&p->audio);
    if (p->has_video)
        xf_video_close(&p->video);
    free(p->pcm);
    xf_subtitles_free(&p->subs);
    xf_mov_close(&p->mov);
    memset(p, 0, sizeof(*p));
}

/* Keeps the mixer ring buffer filled, one chunk at a time. */
static void feed_audio(xf_player *p)
{
    const xf_mov_track *t = p->audio.track;
    while (!p->audio_done && xf_mixer_space(p->channel) >= xf_audio_max_chunk_frames(&p->audio)) {
        int n = xf_audio_next(&p->audio, p->pcm);
        u32 drop;
        if (n <= 0) {
            p->audio_done = 1;
            xf_mixer_finish(p->channel);
            break;
        }
        drop = XF_MIN(p->audio_skip, (u32)n);
        p->audio_skip -= drop;
        xf_mixer_write(p->channel, p->pcm + drop * t->channels, (u32)n - drop);
    }
}

int xf_player_update(xf_player *p, u32 now_ticks)
{
    if (p->finished)
        return 0;
    if (p->has_audio) {
        u32 wall = now_ticks - p->start_ticks;
        feed_audio(p);
        p->now_ms = (u32)(xf_mixer_played(p->channel) * 1000 / p->audio.track->sample_rate);
        /* Without sound output (or when the output stalls) the sound clock
         * does not move; the wall clock keeps the clip going, a little
         * behind so the sound normally leads. */
        if (wall > p->now_ms + 400)
            p->now_ms = wall - 400;
    } else {
        p->now_ms = now_ticks - p->start_ticks;
    }

    if (p->has_video) {
        const xf_mov_track *t = p->video.track;
        u64 media = xf_mov_media_start(t) + (u64)p->now_ms * t->timescale / 1000;
        u32 index = xf_mov_sample_at(t, (u32)media);
        if (index + 1 != p->video.next && xf_video_seek(&p->video, index) != 0) {
            xf_warn("player: cannot decode frame %u", index);
            p->has_video = 0;
        }
    }

    /* The clip ends with its picture and its sound; the sound gets half a
     * second more at most, so a channel that never drains does not hold
     * the last frame. */
    if (p->now_ms >= p->duration_ms &&
        (!p->has_audio || !xf_mixer_active(p->channel) || now_ticks - p->start_ticks >= p->duration_ms + 500))
        p->finished = 1;
    return !p->finished;
}

const u32 *xf_player_frame(const xf_player *p) { return p->has_video ? p->video.frame : NULL; }

const char *xf_player_subtitle(const xf_player *p)
{
    const xf_subtitle *s = xf_subtitles_at(&p->subs, p->now_ms);
    return s ? s->text : NULL;
}
