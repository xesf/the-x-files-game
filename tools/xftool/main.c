#include <stdio.h>
#include <string.h>

#include "xftool.h"

static const xftool_cmd *const g_groups[] = {
    xftool_pff_cmds,
    xftool_hot_cmds,
    xftool_mov_cmds,
    xftool_text_cmds,
};

int xftool_write_file(const char *path, const void *data, unsigned long size)
{
    FILE *fp = fopen(path, "wb");
    int ok;

    if (!fp) {
        perror(path);
        return -1;
    }
    ok = fwrite(data, 1, size, fp) == size;
    fclose(fp);
    return ok ? 0 : -1;
}

static void usage(void)
{
    size_t g;
    const xftool_cmd *c;

    fputs("usage: xftool <command> [args]\n\ncommands:\n", stderr);
    for (g = 0; g < sizeof(g_groups) / sizeof(g_groups[0]); g++)
        for (c = g_groups[g]; c->name; c++)
            fprintf(stderr, "  %-14s %s\n", c->name, c->usage);
}

int main(int argc, char **argv)
{
    size_t g;
    const xftool_cmd *c;

    if (argc < 2) {
        usage();
        return 2;
    }
    for (g = 0; g < sizeof(g_groups) / sizeof(g_groups[0]); g++)
        for (c = g_groups[g]; c->name; c++)
            if (strcmp(c->name, argv[1]) == 0)
                return c->run(argc - 1, argv + 1);
    usage();
    return 2;
}
