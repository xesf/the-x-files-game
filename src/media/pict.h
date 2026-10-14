/* Macintosh PICT version 2 decoder for the images in PFF bundles
 * (see docs/formats/pff.md). */
#ifndef XF_PICT_H
#define XF_PICT_H

#include "xf_types.h"

typedef struct {
    int width, height;
    u32 *pixels; /* 0x00RRGGBB, width * height, freed with free() */
} xf_image;

/* Decodes a PICT without the 512-byte file header. Returns 0 on success. */
int xf_pict_decode(const u8 *data, size_t size, xf_image *img);
/* Reads the picture frame size only. */
int xf_pict_size(const u8 *data, size_t size, int *width, int *height);

#endif
