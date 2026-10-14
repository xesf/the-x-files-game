/* PFF: a bundle of Macintosh PICT images (see docs/formats/pff.md). */
#ifndef XF_PFF_H
#define XF_PFF_H

#include "xf_file.h"

typedef struct {
    xf_file *file;
    u32 count;
    u32 *offsets; /* count + 1 entries; the last one is the file size */
} xf_pff;

int xf_pff_open(xf_pff *pff, const char *path);
void xf_pff_close(xf_pff *pff);
u32 xf_pff_entry_size(const xf_pff *pff, u32 index);
/* Reads entry `index` into a new buffer, freed with free(). */
u8 *xf_pff_read(xf_pff *pff, u32 index, u32 *size);

#endif
