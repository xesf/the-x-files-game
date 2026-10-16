/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "media/vob.h"
#include "xftool.h"

static int cmd_vob(int argc, char **argv)
{
    const xf_vob_view *view;
    xf_vob *v;
    u32 *rgb;
    int r;
    if (argc < 4 || argc > 5) {
        fputs("usage: xftool vob <file.vob> <ms> <out.bmp> [full]\n", stderr);
        return 2;
    }
    v = xf_vob_open(argv[1]);
    if (!v)
        return 1;
    view = argc == 5 ? &xf_vob_full_view : &xf_vob_scene_view;
    rgb = calloc((size_t)view->out_w * view->out_h, 4);
    printf("%u ms, sound %d Hz %d channels\n", xf_vob_duration_ms(v), xf_vob_sample_rate(v), xf_vob_channels(v));
    r = rgb && xf_vob_video(v, (u32)strtoul(argv[2], NULL, 10), rgb, view) >= 0 &&
        xftool_write_bmp(argv[3], rgb, view->out_w, view->out_h) == 0;
    free(rgb);
    xf_vob_close(v);
    return r ? 0 : 1;
}

/* Raw 4:2:0 pictures, to compare with another decoder. */
static int cmd_vob_yuv(int argc, char **argv)
{
    xf_mpeg2_picture p;
    xf_vob *v;
    FILE *fp;
    int n = 0, max, y;
    if (argc < 3 || argc > 4) {
        fputs("usage: xftool vob-yuv <file.vob> <out.yuv> [pictures]\n", stderr);
        return 2;
    }
    max = argc == 4 ? atoi(argv[3]) : 1 << 30;
    v = xf_vob_open(argv[1]);
    fp = fopen(argv[2], "wb");
    if (!v || !fp)
        return 1;
    while (n < max && xf_vob_picture(v, &p) == 0) {
        for (y = 0; y < p.height; y++)
            fwrite(p.y + (size_t)y * p.stride, 1, (size_t)p.width, fp);
        for (y = 0; y < p.height / 2; y++)
            fwrite(p.cb + (size_t)y * p.cstride, 1, (size_t)p.width / 2, fp);
        for (y = 0; y < p.height / 2; y++)
            fwrite(p.cr + (size_t)y * p.cstride, 1, (size_t)p.width / 2, fp);
        n++;
    }
    fclose(fp);
    xf_vob_close(v);
    printf("%d pictures\n", n);
    return 0;
}

static void put32(unsigned char *p, u32 v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static int cmd_vob_audio(int argc, char **argv)
{
    static const unsigned char base[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E',
                                            'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0 };
    unsigned char hdr[44];
    i16 pcm[4096 * 2];
    xf_vob *v;
    FILE *fp;
    u32 total = 0;
    int n, ch, rate;
    if (argc != 3) {
        fputs("usage: xftool vob-audio <file.vob> <out.wav>\n", stderr);
        return 2;
    }
    v = xf_vob_open(argv[1]);
    fp = fopen(argv[2], "wb");
    if (!v || !fp || !xf_vob_sample_rate(v))
        return 1;
    ch = xf_vob_channels(v);
    rate = xf_vob_sample_rate(v);
    memcpy(hdr, base, 44);
    fwrite(hdr, 1, 44, fp);
    while ((n = xf_vob_audio(v, pcm, 4096)) > 0) {
        fwrite(pcm, 2, (size_t)n * ch, fp); /* little-endian hosts only */
        total += (u32)n;
    }
    hdr[22] = (unsigned char)ch;
    put32(hdr + 24, (u32)rate);
    put32(hdr + 28, (u32)rate * ch * 2);
    hdr[32] = (unsigned char)(ch * 2);
    hdr[34] = 16;
    memcpy(hdr + 36, "data", 4);
    put32(hdr + 40, total * ch * 2);
    put32(hdr + 4, 36 + total * ch * 2);
    fseek(fp, 0, SEEK_SET);
    fwrite(hdr, 1, 44, fp);
    fclose(fp);
    xf_vob_close(v);
    printf("%u frames, %d Hz, %d channels\n", total, rate, ch);
    return 0;
}

const xftool_cmd xftool_vob_cmds[] = {
    { "vob", "<file.vob> <ms> <out.bmp> [full]  decode the picture shown at a time", cmd_vob },
    { "vob-yuv", "<file.vob> <out.yuv> [pictures]  save the decoded pictures as raw 4:2:0", cmd_vob_yuv },
    { "vob-audio", "<file.vob> <out.wav>  decode the sound to a WAV file", cmd_vob_audio },
    { 0 }
};
