/* Scene explorer: shows a view and follows its navigation hotspots. A
 * development tool on the way to the real game loop. */
#ifndef XF_EXPLORE_H
#define XF_EXPLORE_H

#include "xf_types.h"

/* Runs until the window closes. Returns 0, or 1 when the data is missing.
 * `media_dirs` (NULL-terminated, may be NULL) are searched for media
 * files missing from `data_dir`, for example the CD clips for the DVD
 * data, whose video is on the disc. With `script_path`, a story script
 * (docs/TESTING.md) plays the game and can take screenshots. */
int xf_explore(const char *data_dir, const char *const *media_dirs, u32 start_view, int disc,
               const char *script_path);

#endif
