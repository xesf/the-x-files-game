/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* Text encoding helpers. Game text is Windows-1252. */
#ifndef XF_TEXT_H
#define XF_TEXT_H

#include "xf_types.h"

/* Returns the Unicode code point of a Windows-1252 byte. */
u32 xf_cp1252_to_unicode(u8 c);
/* Converts Windows-1252 text to UTF-8. Writes at most `dst_size` bytes
 * including the terminating zero. Returns the length written. */
size_t xf_cp1252_to_utf8(char *dst, size_t dst_size, const u8 *src, size_t src_size);

/* Returns the Unicode code point of a Mac Roman byte. */
u32 xf_macroman_to_unicode(u8 c);
/* Converts Mac Roman text to UTF-8, like xf_cp1252_to_utf8. */
size_t xf_macroman_to_utf8(char *dst, size_t dst_size, const u8 *src, size_t src_size);

/* Writes text (UTF-8, or Windows-1252 where not UTF-8) as lower-case
 * ASCII, accents dropped ("Plutónio" -> "plutonio"), for comparing typed
 * words. Returns the length. */
size_t xf_fold_ascii(char *dst, size_t size, const char *src);
/* Converts Shift-JIS (code page 932: the Japanese release's documents)
 * to UTF-8, like xf_cp1252_to_utf8. */
size_t xf_sjis_to_utf8(char *dst, size_t dst_size, const u8 *src, size_t src_size);
/* Whether the bytes are valid UTF-8. */
int xf_is_utf8(const char *text, size_t size);

#endif
