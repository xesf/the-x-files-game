#include <stdio.h>
#include <stdlib.h>

#include "media/pff.h"
#include "xf_endian.h"
#include "xftool.h"

/* Names the PICT payload: the first image opcode after the header. */
static const char *pict_kind(const u8 *p, u32 size)
{
    u32 i;
    for (i = 40; i + 1 < size && i < 512; i += 2) {
        u16 op = xf_be16(p + i);
        if (op == 0x8200)
            return "quicktime";
        if (op == 0x009a)
            return "direct-bits";
        if (op == 0x0098)
            return "packbits-indexed";
    }
    return "?";
}

static int cmd_list(int argc, char **argv)
{
    xf_pff pff;
    u32 i;

    if (argc != 2) {
        fputs("usage: xftool pff <file.pff>\n", stderr);
        return 2;
    }
    if (xf_pff_open(&pff, argv[1]) != 0)
        return 1;
    printf("%u entries\n", pff.count);
    for (i = 0; i < pff.count; i++) {
        u32 size;
        u8 *p = xf_pff_read(&pff, i, &size);
        if (!p) {
            printf("%5u  (empty)\n", i);
            continue;
        }
        printf("%5u  %8u bytes  %4ux%-4u  %s\n", i, size,
               xf_be16(p + 8) - xf_be16(p + 4), xf_be16(p + 6) - xf_be16(p + 2),
               pict_kind(p, size));
        free(p);
    }
    xf_pff_close(&pff);
    return 0;
}

static int cmd_extract(int argc, char **argv)
{
    xf_pff pff;
    u32 size;
    u8 *p;
    int r;

    if (argc != 4) {
        fputs("usage: xftool pff-extract <file.pff> <index> <out.pict>\n", stderr);
        return 2;
    }
    if (xf_pff_open(&pff, argv[1]) != 0)
        return 1;
    p = xf_pff_read(&pff, (u32)atoi(argv[2]), &size);
    xf_pff_close(&pff);
    if (!p) {
        fputs("no such entry\n", stderr);
        return 1;
    }
    /* Mac tools expect a 512-byte empty header before PICT data. */
    {
        static const u8 zero[512];
        FILE *fp = fopen(argv[3], "wb");
        if (!fp) {
            free(p);
            perror(argv[3]);
            return 1;
        }
        r = fwrite(zero, 1, 512, fp) == 512 && fwrite(p, 1, size, fp) == size;
        fclose(fp);
    }
    free(p);
    return r ? 0 : 1;
}

const xftool_cmd xftool_pff_cmds[] = {
    { "pff", "<file.pff>  list the images in a PFF bundle", cmd_list },
    { "pff-extract", "<file.pff> <index> <out.pict>  save one image as a PICT file", cmd_extract },
    { 0 }
};
