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

#endif
