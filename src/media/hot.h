/* HOT: hotspot rectangles for a video clip (see docs/formats/hot.md). */
#ifndef XF_HOT_H
#define XF_HOT_H

#include "xf_types.h"

typedef struct {
    u32 id;
    u16 left, top, right, bottom;
    u32 start, end; /* clip time range in which the hotspot is active */
} xf_hotspot;

typedef struct {
    u32 count;
    xf_hotspot *spots;
} xf_hot;

int xf_hot_load(xf_hot *hot, const char *path);
int xf_hot_parse(xf_hot *hot, const u8 *data, size_t size);
void xf_hot_free(xf_hot *hot);

#endif
