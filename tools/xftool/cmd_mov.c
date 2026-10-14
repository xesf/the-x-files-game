#include <stdio.h>

#include "media/mov.h"
#include "xftool.h"

static const char *const g_kinds[] = { "video", "audio", "text", "other" };

static int cmd_mov(int argc, char **argv)
{
    xf_mov mov;
    u32 i;
    int a;

    if (argc < 2) {
        fputs("usage: xftool mov <file>...\n", stderr);
        return 2;
    }
    for (a = 1; a < argc; a++) {
        if (xf_mov_open(&mov, argv[a]) != 0)
            continue;
        printf("%s: %.2f s, %u tracks\n", argv[a],
               (double)mov.duration / mov.timescale, mov.track_count);
        for (i = 0; i < mov.track_count; i++) {
            const xf_mov_track *t = &mov.tracks[i];
            char cc[5];
            printf("  #%u %-5s %s  %u samples, scale %u", t->id, g_kinds[t->kind],
                   xf_fourcc_str(t->codec, cc), t->sample_count, t->timescale);
            if (t->kind == XF_TRACK_VIDEO)
                printf(", %ux%u %u-bit, %u sync", t->width, t->height, t->depth,
                       t->sync_count ? t->sync_count : t->sample_count);
            else if (t->kind == XF_TRACK_AUDIO)
                printf(", %u Hz %u ch %u-bit, %u chunks", t->sample_rate, t->channels, t->bits,
                       t->chunk_count);
            putchar('\n');
        }
        xf_mov_close(&mov);
    }
    return 0;
}

const xftool_cmd xftool_mov_cmds[] = {
    { "mov", "<file>...  show the tracks of QuickTime files (.xmv .nmv .amv .dmv .mus)", cmd_mov },
    { 0 }
};
