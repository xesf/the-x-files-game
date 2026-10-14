#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "media/audio.h"
#include "xftool.h"

static void put32(u8 *p, u32 v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static int cmd_audio(int argc, char **argv)
{
    xf_mov mov;
    xf_mov_track *t;
    xf_audio_stream s;
    i16 *pcm;
    u8 hdr[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
                   16, 0, 0, 0, 1, 0 };
    FILE *fp;
    u32 total = 0, skip;
    int n;

    if (argc != 3) {
        fputs("usage: xftool audio <movie> <out.wav>\n", stderr);
        return 2;
    }
    if (xf_mov_open(&mov, argv[1]) != 0)
        return 1;
    t = xf_mov_find(&mov, XF_TRACK_AUDIO);
    if (!t || xf_audio_open(&s, &mov, t) != 0) {
        char cc[5];
        fprintf(stderr, "no supported sound track (%s)\n", t ? xf_fourcc_str(t->codec, cc) : "none");
        xf_mov_close(&mov);
        return 1;
    }
    pcm = malloc((size_t)xf_audio_max_chunk_frames(&s) * t->channels * 2 + 1);
    fp = fopen(argv[2], "wb");
    if (!pcm || !fp) {
        perror(argv[2]);
        return 1;
    }
    fwrite(hdr, 1, 44, fp);
    /* Start where the edit list starts the sound, as QuickTime does. */
    skip = xf_audio_seek(&s, xf_mov_media_start(t));
    while ((n = xf_audio_next(&s, pcm)) > 0) {
        u32 drop = XF_MIN(skip, (u32)n);
        fwrite(pcm + drop * t->channels, 2, (size_t)(n - drop) * t->channels, fp); /* little-endian hosts only */
        total += (u32)n - drop;
        skip -= drop;
    }
    hdr[22] = (u8)t->channels;
    put32(hdr + 24, t->sample_rate);
    put32(hdr + 28, t->sample_rate * t->channels * 2);
    hdr[32] = (u8)(t->channels * 2);
    hdr[34] = 16;
    memcpy(hdr + 36, "data", 4);
    put32(hdr + 40, total * t->channels * 2);
    put32(hdr + 4, 36 + total * t->channels * 2);
    fseek(fp, 0, SEEK_SET);
    fwrite(hdr, 1, 44, fp);
    fclose(fp);
    printf("%u frames, %u Hz, %u channels\n", total, t->sample_rate, t->channels);
    free(pcm);
    xf_audio_close(&s);
    xf_mov_close(&mov);
    return n < 0 ? 1 : 0;
}

const xftool_cmd xftool_audio_cmds[] = {
    { "audio", "<movie> <out.wav>  decode the sound track to a WAV file", cmd_audio },
    { 0 }
};
