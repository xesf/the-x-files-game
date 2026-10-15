/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf and contributors */
/* DEFLATE decompression (RFC 1951) with the zlib wrapper (RFC 1950). */
#ifndef XF_INFLATE_H
#define XF_INFLATE_H

#include "xf_types.h"

/* Decompresses a zlib stream into `dst`. Returns the number of bytes
 * written, or -1 on error or if `dst` is too small. */
long xf_zlib_uncompress(u8 *dst, size_t dst_size, const u8 *src, size_t src_size);
/* Same for a raw DEFLATE stream without the zlib header. */
long xf_inflate(u8 *dst, size_t dst_size, const u8 *src, size_t src_size);

#endif
