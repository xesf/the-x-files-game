/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "media/pff.h"
#include "media/pict.h"
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

/* Decodes one image, or with index "all" checks every image. */
static int cmd_image(int argc, char **argv)
{
    xf_pff pff;
    u32 i, first, last, failed = 0;
    int check_all;

    if (argc != 4 && !(argc == 3 && strcmp(argv[2], "all") == 0)) {
        fputs("usage: xftool pff-image <file.pff> <index> <out.bmp>\n"
              "       xftool pff-image <file.pff> all\n", stderr);
        return 2;
    }
    if (xf_pff_open(&pff, argv[1]) != 0)
        return 1;
    check_all = strcmp(argv[2], "all") == 0;
    first = check_all ? 0 : (u32)atoi(argv[2]);
    last = check_all ? pff.count : first + 1;
    for (i = first; i < last && i < pff.count; i++) {
        u32 size;
        u8 *p = xf_pff_read(&pff, i, &size);
        xf_image img;
        if (!p || xf_pict_decode(p, size, &img) != 0) {
            printf("%u: cannot decode\n", i);
            failed++;
        } else if (!check_all) {
            xftool_write_bmp(argv[3], img.pixels, img.width, img.height);
        }
        if (p && img.pixels)
            free(img.pixels);
        free(p);
    }
    if (check_all)
        printf("%u of %u images failed\n", failed, pff.count);
    xf_pff_close(&pff);
    return failed ? 1 : 0;
}

const xftool_cmd xftool_pff_cmds[] = {
    { "pff-image", "<file.pff> <index>|all [out.bmp]  decode an image to BMP, or check all", cmd_image },
    { "pff", "<file.pff>  list the images in a PFF bundle", cmd_list },
    { "pff-extract", "<file.pff> <index> <out.pict>  save one image as a PICT file", cmd_extract },
    { 0 }
};
