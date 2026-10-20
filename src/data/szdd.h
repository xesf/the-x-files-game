/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* Files packed with Microsoft's COMPRESS.EXE ("SZDD"), as on the earlier
 * HyperBole games' discs: Quantum Gate's .HSB pictures, Roland's
 * programs (see docs/formats/szdd.md). */
#ifndef XF_SZDD_H
#define XF_SZDD_H

#include "xf_types.h"

/* Whether the data starts with the SZDD signature. */
int xf_szdd_is(const u8 *data, size_t size);
/* The unpacked size the header gives, or 0. */
u32 xf_szdd_size(const u8 *data, size_t size);
/* The last character of the original name, which the packed name
 * replaces with '_' (0 when the header does not keep it). */
char xf_szdd_last_char(const u8 *data, size_t size);
/* Unpacks into `out` (`out_size` bytes, at least xf_szdd_size). Returns
 * the number of bytes written, or -1 on bad data. */
long xf_szdd_expand(const u8 *data, size_t size, u8 *out, size_t out_size);
/* Loads a file, unpacking it when it is packed. Returns malloc'd data
 * (free() it) and its size, or NULL. */
u8 *xf_szdd_load(const char *path, size_t *size);

#endif
