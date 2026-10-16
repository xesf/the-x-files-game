/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
#include "player.h"

#include <stdlib.h>
#include <string.h>

#include "audio/mixer.h"
#include "xf_log.h"

int xf_player_open(xf_player *p, const char *path, u32 now_ticks)
{
    return xf_player_open_range(p, path, now_ticks, 0, 0);
}

int xf_player_open_range(xf_player *p, const char *path, u32 now_ticks, u32 start_ms, u32 end_ms)
{
    xf_mov_track *t;

    memset(p, 0, sizeof(*p));
    p->channel = -1;
    if (xf_mov_open(&p->mov, path) != 0)
        return -1;
    p->duration_ms = (u32)((u64)p->mov.duration * 1000 / p->mov.timescale);
    if (start_ms < p->duration_ms) {
        p->base_ms = start_ms;
        if (end_ms > start_ms && end_ms < p->duration_ms)
            p->duration_ms = end_ms;
        p->duration_ms -= start_ms;
    }

    t = xf_mov_find(&p->mov, XF_TRACK_VIDEO);
    if (t && xf_video_open(&p->video, &p->mov, t) == 0)
        p->has_video = 1;

    t = xf_mov_find(&p->mov, XF_TRACK_AUDIO);
    if (t && xf_audio_open(&p->audio, &p->mov, t) == 0) {
        p->pcm = malloc((size_t)xf_audio_max_chunk_frames(&p->audio) * t->channels * 2 + 2);
        p->channel = p->pcm ? xf_mixer_open((int)t->sample_rate, t->channels) : -1;
        if (p->channel >= 0) {
            p->has_audio = 1;
            p->audio_skip = xf_audio_seek(&p->audio, xf_mov_media_start(t) +
                                                     (u32)((u64)p->base_ms * t->timescale / 1000));
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

#define VOB_CHUNK 2048

int xf_player_open_vob(xf_player *p, const char *path, u32 now_ticks, const xf_vob_view *view)
{
    memset(p, 0, sizeof(*p));
    p->channel = -1;
    p->vob = xf_vob_open(path);
    if (!p->vob)
        return -1;
    p->view = *view;
    p->duration_ms = xf_vob_duration_ms(p->vob);
    p->video.width = view->out_w;
    p->video.height = view->out_h;
    p->video.frame = calloc((size_t)view->out_w * view->out_h, 4);
    if (!p->video.frame) {
        xf_player_close(p);
        return -1;
    }
    p->has_video = 1;
    if (xf_vob_sample_rate(p->vob)) {
        p->pcm = malloc((size_t)VOB_CHUNK * xf_vob_channels(p->vob) * 2);
        p->channel = p->pcm ? xf_mixer_open(xf_vob_sample_rate(p->vob), xf_vob_channels(p->vob)) : -1;
        p->has_audio = p->channel >= 0;
    }
    p->start_ticks = now_ticks;
    return 0;
}

void xf_player_close(xf_player *p)
{
    if (p->vob) {
        if (p->channel >= 0)
            xf_mixer_close(p->channel);
        xf_vob_close(p->vob);
        free(p->video.frame);
        free(p->pcm);
        memset(p, 0, sizeof(*p));
        return;
    }
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

static void feed_vob_audio(xf_player *p)
{
    while (!p->audio_done && xf_mixer_space(p->channel) >= VOB_CHUNK) {
        int n = xf_vob_audio(p->vob, p->pcm, VOB_CHUNK);
        if (n <= 0) {
            p->audio_done = 1;
            xf_mixer_finish(p->channel);
            break;
        }
        xf_mixer_write(p->channel, p->pcm, (u32)n);
    }
}

/* The clock as for movies; the pictures follow it and the clip ends with
 * the last picture and the sound. */
static int update_vob(xf_player *p, u32 now_ticks)
{
    u32 wall = now_ticks - p->start_ticks;
    if (p->has_audio) {
        feed_vob_audio(p);
        p->now_ms = (u32)(xf_mixer_played(p->channel) * 1000 / (u32)xf_vob_sample_rate(p->vob));
        if (wall > p->now_ms + 400)
            p->now_ms = wall - 400;
    } else {
        p->now_ms = wall;
    }
    if (p->has_video && xf_vob_video(p->vob, p->now_ms, p->video.frame, &p->view) < 0)
        p->has_video = 2; /* the pictures are over; the last one stays */
    if ((p->has_video == 2 || p->now_ms >= p->duration_ms) &&
        (!p->has_audio || !xf_mixer_active(p->channel) || wall >= p->duration_ms + 500))
        p->finished = 1;
    return !p->finished;
}

void xf_player_pause(xf_player *p, int paused, u32 now_ticks)
{
    paused = paused != 0;
    if (p->paused == paused)
        return;
    if (paused)
        p->pause_ticks = now_ticks;
    else
        p->start_ticks += now_ticks - p->pause_ticks;
    if (p->has_audio && p->channel >= 0)
        xf_mixer_set_paused(p->channel, paused);
    p->paused = paused;
}

int xf_player_update(xf_player *p, u32 now_ticks)
{
    if (p->finished)
        return 0;
    if (p->paused)
        return 1;
    if (p->vob)
        return update_vob(p, now_ticks);
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
        u64 media = xf_mov_media_start(t) + (u64)(p->base_ms + p->now_ms) * t->timescale / 1000;
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
    const xf_subtitle *s = xf_subtitles_at(&p->subs, p->base_ms + p->now_ms);
    return s ? s->text : NULL;
}
