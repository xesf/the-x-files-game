#include <stdio.h>
#include <stdlib.h>

#include "media/jpeg.h"
#include "media/mov.h"
#include "xftool.h"

/* Decodes one video sample with the matching codec. */
static int decode_sample(const xf_mov_track *t, const u8 *data, u32 size, u32 *out)
{
    switch (t->codec) {
    case XF_FOURCC('j', 'p', 'e', 'g'):
    case XF_FOURCC('m', 'j', 'p', 'a'):
        return xf_jpeg_decode(data, size, out, t->width, t->height, t->width);
    }
    return -1;
}

static int cmd_frame(int argc, char **argv)
{
    xf_mov mov;
    xf_mov_track *t;
    u32 index, *pixels;
    u8 *data;
    int r = 1;
    char cc[5];

    if (argc != 5) {
        fputs("usage: xftool frame <movie> <track> <sample> <out.bmp>\n", stderr);
        return 2;
    }
    if (xf_mov_open(&mov, argv[1]) != 0)
        return 1;
    index = (u32)atoi(argv[2]);
    if (index < 1 || index > mov.track_count || mov.tracks[index - 1].kind != XF_TRACK_VIDEO) {
        fputs("not a video track\n", stderr);
        xf_mov_close(&mov);
        return 1;
    }
    t = &mov.tracks[index - 1];
    index = (u32)atoi(argv[3]);
    if (index >= t->sample_count) {
        fputs("no such sample\n", stderr);
        xf_mov_close(&mov);
        return 1;
    }
    data = malloc(t->sample_size[index]);
    pixels = calloc((size_t)t->width * t->height, 4);
    if (data && pixels && xf_mov_read_sample(&mov, t, index, data) == 0) {
        if (decode_sample(t, data, t->sample_size[index], pixels) == 0)
            r = xftool_write_bmp(argv[4], pixels, t->width, t->height) == 0 ? 0 : 1;
        else
            fprintf(stderr, "cannot decode codec %s\n", xf_fourcc_str(t->codec, cc));
    }
    free(data);
    free(pixels);
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

const xftool_cmd xftool_frame_cmds[] = {
    { "sample", "<movie> <track> <sample> <out.bin>  save one raw sample", cmd_sample },
    { "frame", "<movie> <track> <sample> <out.bmp>  decode one video frame", cmd_frame },
    { 0 }
};
