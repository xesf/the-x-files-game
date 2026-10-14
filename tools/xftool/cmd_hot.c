#include <stdio.h>

#include "media/hot.h"
#include "xftool.h"

static int cmd_hot(int argc, char **argv)
{
    xf_hot hot;
    u32 i;

    if (argc != 2) {
        fputs("usage: xftool hot <file.hot>\n", stderr);
        return 2;
    }
    if (xf_hot_load(&hot, argv[1]) != 0) {
        fprintf(stderr, "%s: not a valid HOT file\n", argv[1]);
        return 1;
    }
    printf("%u hotspots\n", hot.count);
    printf("   id  left  top right bottom   start     end\n");
    for (i = 0; i < hot.count; i++) {
        const xf_hotspot *s = &hot.spots[i];
        printf("%5u %5u %4u %5u %6u %7u %7u\n", s->id, s->left, s->top, s->right, s->bottom,
               s->start, s->end);
    }
    xf_hot_free(&hot);
    return 0;
}

const xftool_cmd xftool_hot_cmds[] = {
    { "hot", "<file.hot>  list the hotspots of a video clip", cmd_hot },
    { 0 }
};
