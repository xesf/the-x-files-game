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

/* Converts text in place from UTF-8 to Windows-1252 when it is UTF-8
 * with more than ASCII (translations are written in UTF-8); leaves other
 * text alone. Characters Windows-1252 lacks become '?'. Returns the new
 * length. */
size_t xf_utf8_to_cp1252(char *text, size_t size);

#endif
