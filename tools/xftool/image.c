/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf and contributors */
#include <stdio.h>
#include <stdlib.h>

#include "xftool.h"

/* Writes 0x00RRGGBB pixels as a 24-bit BMP file. */
int xftool_write_bmp(const char *path, const unsigned *pixels, int w, int h)
{
    int row = (w * 3 + 3) & ~3, y, x;
    unsigned size = 54 + (unsigned)row * h;
    unsigned char hdr[54] = { 'B', 'M' };
    unsigned char *line;
    FILE *fp = fopen(path, "wb");

    if (!fp) {
        perror(path);
        return -1;
    }
    hdr[2] = size; hdr[3] = size >> 8; hdr[4] = size >> 16; hdr[5] = size >> 24;
    hdr[10] = 54;
    hdr[14] = 40;
    hdr[18] = w; hdr[19] = w >> 8; hdr[20] = w >> 16; hdr[21] = w >> 24;
    hdr[22] = h; hdr[23] = h >> 8; hdr[24] = h >> 16; hdr[25] = h >> 24;
    hdr[26] = 1;
    hdr[28] = 24;
    fwrite(hdr, 1, 54, fp);
    line = calloc(1, row);
    for (y = h - 1; y >= 0; y--) {
        for (x = 0; x < w; x++) {
            unsigned p = pixels[y * w + x];
            line[x * 3] = p & 0xff;
            line[x * 3 + 1] = p >> 8 & 0xff;
            line[x * 3 + 2] = p >> 16 & 0xff;
        }
        fwrite(line, 1, row, fp);
    }
    free(line);
    fclose(fp);
    return 0;
}
