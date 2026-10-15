/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf and contributors */
/* Basic types shared by the whole engine. */
#ifndef XF_TYPES_H
#define XF_TYPES_H

#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

#define XF_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define XF_MIN(a, b) ((a) < (b) ? (a) : (b))
#define XF_MAX(a, b) ((a) > (b) ? (a) : (b))
#define XF_FOURCC(a, b, c, d) ((u32)(a) << 24 | (u32)(b) << 16 | (u32)(c) << 8 | (u32)(d))
#define XF_CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

#endif
