/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* File access. Game files are looked up without regard to case, because
 * the original discs used upper-case names and installs vary. */
#ifndef XF_FILE_H
#define XF_FILE_H

#include "xf_types.h"

typedef struct xf_file xf_file;

/* Finds `rel` under `root` ignoring case. Writes the real path to `out`.
 * Returns 0 on success, -1 if a path component is missing. */
int xf_path_resolve(const char *root, const char *rel, char *out, size_t out_size);

xf_file *xf_open(const char *path);
void xf_close(xf_file *f);
u64 xf_size(xf_file *f);
int xf_seek(xf_file *f, u64 pos);
u64 xf_tell(xf_file *f);
/* Reads exactly `n` bytes. Returns 0 on success, -1 on short read. */
int xf_read(xf_file *f, void *dst, size_t n);
/* Reads `n` bytes at `pos`. Returns 0 on success, -1 on error. */
int xf_read_at(xf_file *f, u64 pos, void *dst, size_t n);

/* Loads a whole file into a new buffer, freed with free(). */
u8 *xf_load(const char *path, size_t *size);

#endif
