#include <stdio.h>
#include <stdlib.h>

#include "media/cinepak.h"
#include "media/jpeg.h"
#include "media/mov.h"
#include "xftool.h"

/* Decodes video sample `index`. Inter-coded codecs need the frames since
 * the previous key frame, so those are decoded first. */
static int decode_sample(xf_mov *mov, const xf_mov_track *t, u32 index, u32 *out)
{
    u8 *data = malloc(t->sample_size[index]);
    u32 size = t->sample_size[index];
    int r = -1;

    if (!data || xf_mov_read_sample(mov, t, index, data) != 0) {
        free(data);
        return -1;
    }
    switch (t->codec) {
    case XF_FOURCC('c', 'v', 'i', 'd'): {
        xf_cinepak *c = xf_cinepak_create(t->width, t->height);
        u32 i = index;
        while (i > 0 && !xf_mov_is_sync(t, i))
            i--;
        for (r = 0; c && i <= index && r == 0; i++) {
            u8 *d = malloc(t->sample_size[i]);
            r = d && xf_mov_read_sample(mov, t, i, d) == 0
                    ? xf_cinepak_decode(c, d, t->sample_size[i], out) : -1;
            free(d);
        }
        xf_cinepak_destroy(c);
        break;
    }
    case XF_FOURCC('j', 'p', 'e', 'g'):
    case XF_FOURCC('m', 'j', 'p', 'a'):
        r = xf_jpeg_decode(data, size, out, t->width, t->height, t->width);
        break;
    }
    free(data);
    return r;
}

static int cmd_frame(int argc, char **argv)
{
    xf_mov mov;
    xf_mov_track *t;
    u32 index, *pixels;
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
    pixels = calloc((size_t)t->width * t->height, 4);
    if (pixels) {
        if (decode_sample(&mov, t, index, pixels) == 0)
            r = xftool_write_bmp(argv[4], pixels, t->width, t->height) == 0 ? 0 : 1;
        else
            fprintf(stderr, "cannot decode codec %s\n", xf_fourcc_str(t->codec, cc));
    }
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
