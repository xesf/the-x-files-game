/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
#include <stdio.h>
#include <stdlib.h>

#include "media/audio.h"
#include "media/video.h"
#include "xftool.h"

static int cmd_frame(int argc, char **argv)
{
    xf_mov mov;
    xf_video v;
    u32 track;
    int r = 1;
    char cc[5];

    if (argc != 5) {
        fputs("usage: xftool frame <movie> <track> <sample> <out.bmp>\n", stderr);
        return 2;
    }
    if (xf_mov_open(&mov, argv[1]) != 0)
        return 1;
    track = (u32)atoi(argv[2]);
    if (track < 1 || track > mov.track_count || xf_video_open(&v, &mov, &mov.tracks[track - 1]) != 0) {
        fprintf(stderr, "track %u: not a supported video track\n", track);
        xf_mov_close(&mov);
        return 1;
    }
    if (xf_video_seek(&v, (u32)atoi(argv[3])) == 0)
        r = xftool_write_bmp(argv[4], v.frame, v.width, v.height) == 0 ? 0 : 1;
    else
        fprintf(stderr, "cannot decode sample (codec %s)\n", xf_fourcc_str(v.track->codec, cc));
    xf_video_close(&v);
    xf_mov_close(&mov);
    return r;
}

static int cmd_sample(int argc, char **argv)
{
    xf_mov mov;
    xf_mov_track *t;
    u32 track, index;
    u8 *data;
    int r = 1;

    if (argc != 5) {
        fputs("usage: xftool sample <movie> <track> <sample> <out.bin>\n", stderr);
        return 2;
    }
    if (xf_mov_open(&mov, argv[1]) != 0)
        return 1;
    track = (u32)atoi(argv[2]);
    index = (u32)atoi(argv[3]);
    if (track < 1 || track > mov.track_count || index >= mov.tracks[track - 1].sample_count ||
        !mov.tracks[track - 1].sample_size) {
        fputs("no such sample\n", stderr);
        xf_mov_close(&mov);
        return 1;
    }
    t = &mov.tracks[track - 1];
    data = malloc(t->sample_size[index]);
    if (data && xf_mov_read_sample(&mov, t, index, data) == 0)
        r = xftool_write_file(argv[4], data, t->sample_size[index]) == 0 ? 0 : 1;
    free(data);
    xf_mov_close(&mov);
    return r;
}

static int cmd_still(int argc, char **argv)
{
    xf_mov mov;
    u32 *pixels;
    int r = 1, w = 600, h = 240;

    if (argc != 4) {
        fputs("usage: xftool still <movie> <ms> <out.bmp>\n", stderr);
        return 2;
    }
    if (xf_mov_open(&mov, argv[1]) != 0)
        return 1;
    pixels = calloc((size_t)w * h, 4);
    if (pixels && xf_movie_frame_at(&mov, (u32)strtoul(argv[2], NULL, 10), pixels, w, h) == 0)
        r = xftool_write_bmp(argv[3], pixels, w, h) ? 1 : 0;
    else
        fputs("no picture at that time\n", stderr);
    free(pixels);
    xf_mov_close(&mov);
    return r;
}

/* Decodes every video frame and sound chunk of each file. */
static int cmd_check(int argc, char **argv)
{
    int a, bad_files = 0;
    char cc[5];

    for (a = 1; a < argc; a++) {
        xf_mov mov;
        u32 k;
        int bad = 0;
        if (xf_mov_open(&mov, argv[a]) != 0) {
            bad_files++;
            continue;
        }
        for (k = 0; k < mov.track_count; k++) {
            const xf_mov_track *t = &mov.tracks[k];
            if (t->kind == XF_TRACK_VIDEO) {
                xf_video v;
                u32 i;
                if (xf_video_open(&v, &mov, t) != 0) {
                    printf("%s: track %u: unsupported video %s\n", argv[a], k + 1,
                           xf_fourcc_str(t->codec, cc));
                    bad = 1;
                    continue;
                }
                for (i = 0; i < t->sample_count; i++)
                    if (xf_video_next(&v) != 0) {
                        printf("%s: track %u: frame %u fails\n", argv[a], k + 1, i);
                        bad = 1;
                        break;
                    }
                xf_video_close(&v);
            } else if (t->kind == XF_TRACK_AUDIO) {
                xf_audio_stream s;
                i16 *pcm;
                int n;
                if (xf_audio_open(&s, &mov, t) != 0) {
                    printf("%s: track %u: unsupported audio %s\n", argv[a], k + 1,
                           xf_fourcc_str(t->codec, cc));
                    bad = 1;
                    continue;
                }
                pcm = malloc((size_t)xf_audio_max_chunk_frames(&s) * t->channels * 2 + 2);
                while (pcm && (n = xf_audio_next(&s, pcm)) > 0)
                    ;
                if (!pcm || n < 0) {
                    printf("%s: track %u: sound fails\n", argv[a], k + 1);
                    bad = 1;
                }
                free(pcm);
                xf_audio_close(&s);
            }
        }
        bad_files += bad;
        xf_mov_close(&mov);
    }
    printf("%d of %d files have problems\n", bad_files, argc - 1);
    return bad_files ? 1 : 0;
}

const xftool_cmd xftool_frame_cmds[] = {
    { "frame", "<movie> <track> <sample> <out.bmp>  decode one video frame", cmd_frame },
    { "sample", "<movie> <track> <sample> <out.bin>  save one raw sample", cmd_sample },
    { "still", "<movie> <ms> <out.bmp>  decode the picture shown at a movie time", cmd_still },
    { "check", "<movie>...  decode every frame and sound chunk, report failures", cmd_check },
    { 0 }
};
