/* Scene explorer: shows a view and follows its navigation hotspots. A
 * development tool on the way to the real game loop. */
#ifndef XF_EXPLORE_H
#define XF_EXPLORE_H

#include "xf_types.h"

/* Runs until the window closes. Returns 0, or 1 when the data is missing. */
int xf_explore(const char *data_dir, u32 start_view, int disc);

#endif
