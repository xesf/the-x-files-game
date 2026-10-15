/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* Unaligned big- and little-endian reads from byte buffers. */
#ifndef XF_ENDIAN_H
#define XF_ENDIAN_H

#include "xf_types.h"

static inline u16 xf_be16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }
static inline u32 xf_be32(const u8 *p)
{
    return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}
static inline u64 xf_be64(const u8 *p) { return (u64)xf_be32(p) << 32 | xf_be32(p + 4); }
static inline u16 xf_le16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static inline u32 xf_le32(const u8 *p)
{
    return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

#endif
