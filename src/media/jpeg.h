/* Baseline JPEG decoder (huffman, 8-bit, any sampling). Used for
 * QuickTime 'jpeg' and 'mjpa' frames and for PICT images. */
#ifndef XF_JPEG_H
#define XF_JPEG_H

#include "xf_types.h"

/* Reads the frame size without decoding. Returns 0 on success. */
int xf_jpeg_info(const u8 *data, size_t size, int *width, int *height);
/* Decodes into `out` (0x00RRGGBB, `pitch` in pixels). The image is clipped
 * to `out_w` x `out_h`. Returns 0 on success. */
int xf_jpeg_decode(const u8 *data, size_t size, u32 *out, int out_w, int out_h, int pitch);

#endif
