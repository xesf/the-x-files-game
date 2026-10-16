/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf
 * The code tables follow FFmpeg's mpeg12data.c, Copyright (c) 2000,2001
 * Fabrice Bellard, Copyright (c) 2002-2004 Michael Niedermayer
 * (LGPL-2.1-or-later). */
#include "mpeg2.h"

#include <stdlib.h>
#include <string.h>

#include "xf_log.h"

/* --- Code tables ------------------------------------------------------ */

/* DCT coefficients: run and level of each code, then escape and end of
 * block. Table B.14 (all blocks but intra ones with intra_vlc_format)
 * and table B.15. */
#define RL_COUNT 111
#define RL_ESCAPE RL_COUNT
#define RL_EOB (RL_COUNT + 1)

static const u16 b14_codes[RL_COUNT + 2][2] = {
    { 0x3, 2 }, { 0x4, 4 }, { 0x5, 5 }, { 0x6, 7 }, { 0x26, 8 }, { 0x21, 8 }, { 0xa, 10 }, { 0x1d, 12 },
    { 0x18, 12 }, { 0x13, 12 }, { 0x10, 12 }, { 0x1a, 13 }, { 0x19, 13 }, { 0x18, 13 }, { 0x17, 13 }, { 0x1f, 14 },
    { 0x1e, 14 }, { 0x1d, 14 }, { 0x1c, 14 }, { 0x1b, 14 }, { 0x1a, 14 }, { 0x19, 14 }, { 0x18, 14 }, { 0x17, 14 },
    { 0x16, 14 }, { 0x15, 14 }, { 0x14, 14 }, { 0x13, 14 }, { 0x12, 14 }, { 0x11, 14 }, { 0x10, 14 }, { 0x18, 15 },
    { 0x17, 15 }, { 0x16, 15 }, { 0x15, 15 }, { 0x14, 15 }, { 0x13, 15 }, { 0x12, 15 }, { 0x11, 15 }, { 0x10, 15 },
    { 0x3, 3 }, { 0x6, 6 }, { 0x25, 8 }, { 0xc, 10 }, { 0x1b, 12 }, { 0x16, 13 }, { 0x15, 13 }, { 0x1f, 15 },
    { 0x1e, 15 }, { 0x1d, 15 }, { 0x1c, 15 }, { 0x1b, 15 }, { 0x1a, 15 }, { 0x19, 15 }, { 0x13, 16 }, { 0x12, 16 },
    { 0x11, 16 }, { 0x10, 16 }, { 0x5, 4 }, { 0x4, 7 }, { 0xb, 10 }, { 0x14, 12 }, { 0x14, 13 }, { 0x7, 5 },
    { 0x24, 8 }, { 0x1c, 12 }, { 0x13, 13 }, { 0x6, 5 }, { 0xf, 10 }, { 0x12, 12 }, { 0x7, 6 }, { 0x9, 10 },
    { 0x12, 13 }, { 0x5, 6 }, { 0x1e, 12 }, { 0x14, 16 }, { 0x4, 6 }, { 0x15, 12 }, { 0x7, 7 }, { 0x11, 12 },
    { 0x5, 7 }, { 0x11, 13 }, { 0x27, 8 }, { 0x10, 13 }, { 0x23, 8 }, { 0x1a, 16 }, { 0x22, 8 }, { 0x19, 16 },
    { 0x20, 8 }, { 0x18, 16 }, { 0xe, 10 }, { 0x17, 16 }, { 0xd, 10 }, { 0x16, 16 }, { 0x8, 10 }, { 0x15, 16 },
    { 0x1f, 12 }, { 0x1a, 12 }, { 0x19, 12 }, { 0x17, 12 }, { 0x16, 12 }, { 0x1f, 13 }, { 0x1e, 13 }, { 0x1d, 13 },
    { 0x1c, 13 }, { 0x1b, 13 }, { 0x1f, 16 }, { 0x1e, 16 }, { 0x1d, 16 }, { 0x1c, 16 }, { 0x1b, 16 },
    { 0x1, 6 }, /* escape */
    { 0x2, 2 }, /* end of block */
};

static const u16 b15_codes[RL_COUNT + 2][2] = {
    { 0x02, 2 }, { 0x06, 3 }, { 0x07, 4 }, { 0x1c, 5 }, { 0x1d, 5 }, { 0x05, 6 }, { 0x04, 6 }, { 0x7b, 7 },
    { 0x7c, 7 }, { 0x23, 8 }, { 0x22, 8 }, { 0xfa, 8 }, { 0xfb, 8 }, { 0xfe, 8 }, { 0xff, 8 }, { 0x1f, 14 },
    { 0x1e, 14 }, { 0x1d, 14 }, { 0x1c, 14 }, { 0x1b, 14 }, { 0x1a, 14 }, { 0x19, 14 }, { 0x18, 14 }, { 0x17, 14 },
    { 0x16, 14 }, { 0x15, 14 }, { 0x14, 14 }, { 0x13, 14 }, { 0x12, 14 }, { 0x11, 14 }, { 0x10, 14 }, { 0x18, 15 },
    { 0x17, 15 }, { 0x16, 15 }, { 0x15, 15 }, { 0x14, 15 }, { 0x13, 15 }, { 0x12, 15 }, { 0x11, 15 }, { 0x10, 15 },
    { 0x02, 3 }, { 0x06, 5 }, { 0x79, 7 }, { 0x27, 8 }, { 0x20, 8 }, { 0x16, 13 }, { 0x15, 13 }, { 0x1f, 15 },
    { 0x1e, 15 }, { 0x1d, 15 }, { 0x1c, 15 }, { 0x1b, 15 }, { 0x1a, 15 }, { 0x19, 15 }, { 0x13, 16 }, { 0x12, 16 },
    { 0x11, 16 }, { 0x10, 16 }, { 0x05, 5 }, { 0x07, 7 }, { 0xfc, 8 }, { 0x0c, 10 }, { 0x14, 13 }, { 0x07, 5 },
    { 0x26, 8 }, { 0x1c, 12 }, { 0x13, 13 }, { 0x06, 6 }, { 0xfd, 8 }, { 0x12, 12 }, { 0x07, 6 }, { 0x04, 9 },
    { 0x12, 13 }, { 0x06, 7 }, { 0x1e, 12 }, { 0x14, 16 }, { 0x04, 7 }, { 0x15, 12 }, { 0x05, 7 }, { 0x11, 12 },
    { 0x78, 7 }, { 0x11, 13 }, { 0x7a, 7 }, { 0x10, 13 }, { 0x21, 8 }, { 0x1a, 16 }, { 0x25, 8 }, { 0x19, 16 },
    { 0x24, 8 }, { 0x18, 16 }, { 0x05, 9 }, { 0x17, 16 }, { 0x07, 9 }, { 0x16, 16 }, { 0x0d, 10 }, { 0x15, 16 },
    { 0x1f, 12 }, { 0x1a, 12 }, { 0x19, 12 }, { 0x17, 12 }, { 0x16, 12 }, { 0x1f, 13 }, { 0x1e, 13 }, { 0x1d, 13 },
    { 0x1c, 13 }, { 0x1b, 13 }, { 0x1f, 16 }, { 0x1e, 16 }, { 0x1d, 16 }, { 0x1c, 16 }, { 0x1b, 16 },
    { 0x01, 6 }, /* escape */
    { 0x06, 4 }, /* end of block */
};

static const u8 rl_level[RL_COUNT] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
    25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18,
    1, 2, 3, 4, 5, 1, 2, 3, 4, 1, 2, 3, 1, 2, 3, 1, 2, 3, 1, 2, 1, 2,
    1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
};

static const u8 rl_run[RL_COUNT] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    2, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 5, 5, 5, 6, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 16, 16,
    17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
};

/* macroblock_address_increment 1..33, then escape (+33) and stuffing. */
static const u8 incr_codes[35][2] = {
    { 0x1, 1 }, { 0x3, 3 }, { 0x2, 3 }, { 0x3, 4 }, { 0x2, 4 }, { 0x3, 5 }, { 0x2, 5 }, { 0x7, 7 },
    { 0x6, 7 }, { 0xb, 8 }, { 0xa, 8 }, { 0x9, 8 }, { 0x8, 8 }, { 0x7, 8 }, { 0x6, 8 }, { 0x17, 10 },
    { 0x16, 10 }, { 0x15, 10 }, { 0x14, 10 }, { 0x13, 10 }, { 0x12, 10 }, { 0x23, 11 }, { 0x22, 11 }, { 0x21, 11 },
    { 0x20, 11 }, { 0x1f, 11 }, { 0x1e, 11 }, { 0x1d, 11 }, { 0x1c, 11 }, { 0x1b, 11 }, { 0x1a, 11 }, { 0x19, 11 },
    { 0x18, 11 },
    { 0x8, 11 }, /* escape */
    { 0xf, 11 }, /* stuffing */
};
#define INCR_ESCAPE 34
#define INCR_STUFFING 35

/* coded_block_pattern 0..63. */
static const u8 cbp_codes[64][2] = {
    { 0x1, 9 }, { 0xb, 5 }, { 0x9, 5 }, { 0xd, 6 }, { 0xd, 4 }, { 0x17, 7 }, { 0x13, 7 }, { 0x1f, 8 },
    { 0xc, 4 }, { 0x16, 7 }, { 0x12, 7 }, { 0x1e, 8 }, { 0x13, 5 }, { 0x1b, 8 }, { 0x17, 8 }, { 0x13, 8 },
    { 0xb, 4 }, { 0x15, 7 }, { 0x11, 7 }, { 0x1d, 8 }, { 0x11, 5 }, { 0x19, 8 }, { 0x15, 8 }, { 0x11, 8 },
    { 0xf, 6 }, { 0xf, 8 }, { 0xd, 8 }, { 0x3, 9 }, { 0xf, 5 }, { 0xb, 8 }, { 0x7, 8 }, { 0x7, 9 },
    { 0xa, 4 }, { 0x14, 7 }, { 0x10, 7 }, { 0x1c, 8 }, { 0xe, 6 }, { 0xe, 8 }, { 0xc, 8 }, { 0x2, 9 },
    { 0x10, 5 }, { 0x18, 8 }, { 0x14, 8 }, { 0x10, 8 }, { 0xe, 5 }, { 0xa, 8 }, { 0x6, 8 }, { 0x6, 9 },
    { 0x12, 5 }, { 0x1a, 8 }, { 0x16, 8 }, { 0x12, 8 }, { 0xd, 5 }, { 0x9, 8 }, { 0x5, 8 }, { 0x5, 9 },
    { 0xc, 5 }, { 0x8, 8 }, { 0x4, 8 }, { 0x4, 9 }, { 0x7, 3 }, { 0xa, 5 }, { 0x8, 5 }, { 0xc, 6 },
};

/* motion_code magnitude 0..16; a sign bit follows a nonzero one. */
static const u8 mv_codes[17][2] = {
    { 0x1, 1 }, { 0x1, 2 }, { 0x1, 3 }, { 0x1, 4 }, { 0x3, 6 }, { 0x5, 7 }, { 0x4, 7 }, { 0x3, 7 },
    { 0xb, 9 }, { 0xa, 9 }, { 0x9, 9 }, { 0x11, 10 }, { 0x10, 10 }, { 0xf, 10 }, { 0xe, 10 }, { 0xd, 10 },
    { 0xc, 10 },
};

/* dct_dc_size 0..11. */
static const u16 dc_lum_codes[12][2] = {
    { 0x4, 3 }, { 0x0, 2 }, { 0x1, 2 }, { 0x5, 3 }, { 0x6, 3 }, { 0xe, 4 },
    { 0x1e, 5 }, { 0x3e, 6 }, { 0x7e, 7 }, { 0xfe, 8 }, { 0x1fe, 9 }, { 0x1ff, 9 },
};
static const u16 dc_chroma_codes[12][2] = {
    { 0x0, 2 }, { 0x1, 2 }, { 0x2, 2 }, { 0x6, 3 }, { 0xe, 4 }, { 0x1e, 5 },
    { 0x3e, 6 }, { 0x7e, 7 }, { 0xfe, 8 }, { 0x1fe, 9 }, { 0x3fe, 10 }, { 0x3ff, 10 },
};

/* macroblock_type flags */
#define MB_INTRA 1
#define MB_PATTERN 2
#define MB_BACKWARD 4
#define MB_FORWARD 8
#define MB_QUANT 16

static const u8 ptype_codes[7][3] = {
    { 3, 5, MB_INTRA }, { 1, 2, MB_PATTERN }, { 1, 3, MB_FORWARD }, { 1, 1, MB_FORWARD | MB_PATTERN },
    { 1, 6, MB_QUANT | MB_INTRA }, { 1, 5, MB_QUANT | MB_PATTERN }, { 2, 5, MB_QUANT | MB_FORWARD | MB_PATTERN },
};
static const u8 btype_codes[11][3] = {
    { 3, 5, MB_INTRA }, { 2, 3, MB_BACKWARD }, { 3, 3, MB_BACKWARD | MB_PATTERN }, { 2, 4, MB_FORWARD },
    { 3, 4, MB_FORWARD | MB_PATTERN }, { 2, 2, MB_FORWARD | MB_BACKWARD },
    { 3, 2, MB_FORWARD | MB_BACKWARD | MB_PATTERN }, { 1, 6, MB_QUANT | MB_INTRA },
    { 2, 6, MB_QUANT | MB_BACKWARD | MB_PATTERN }, { 3, 6, MB_QUANT | MB_FORWARD | MB_PATTERN },
    { 2, 5, MB_QUANT | MB_FORWARD | MB_BACKWARD | MB_PATTERN },
};

static const u8 zigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};
static const u8 alternate_scan[64] = {
    0, 8, 16, 24, 1, 9, 2, 10, 17, 25, 32, 40, 48, 56, 57, 49,
    41, 33, 26, 18, 3, 11, 4, 12, 19, 27, 34, 42, 50, 58, 35, 43,
    51, 59, 20, 28, 5, 13, 6, 14, 21, 29, 36, 44, 52, 60, 37, 45,
    53, 61, 22, 30, 7, 15, 23, 31, 38, 46, 54, 62, 39, 47, 55, 63,
};

/* In zigzag order, as the sequence header stores them. */
static const u8 default_intra_matrix[64] = {
    8, 16, 16, 19, 16, 19, 22, 22, 22, 22, 22, 22, 26, 24, 26, 27,
    27, 27, 26, 26, 26, 26, 27, 27, 27, 29, 29, 29, 34, 34, 34, 29,
    29, 29, 27, 27, 29, 29, 32, 32, 34, 34, 37, 38, 37, 35, 35, 34,
    35, 38, 38, 40, 40, 40, 48, 48, 46, 46, 56, 56, 58, 69, 69, 83,
};

static const u8 non_linear_qscale[32] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 18, 20, 22,
    24, 28, 32, 36, 40, 44, 48, 52, 56, 64, 72, 80, 88, 96, 104, 112,
};

/* frame_rate_code 1..8 in frames per second, num / den. */
static const u16 frame_rates[9][2] = {
    { 30000, 1001 }, { 24000, 1001 }, { 24, 1 }, { 25, 1 }, { 30000, 1001 },
    { 30, 1 }, { 50, 1 }, { 60000, 1001 }, { 60, 1 },
};

/* --- Lookup tables ----------------------------------------------------- */

/* A lookup table indexed by the next `bits` bits; each entry is
 * symbol << 5 | code length, 0 for an invalid code. */
typedef struct {
    int bits;
    u16 *t;
} vlc;

static vlc v_b14, v_b15, v_incr, v_cbp, v_mv, v_dc_lum, v_dc_chroma, v_ptype, v_btype, v_itype;
static int tables_ready;

static int vlc_init(vlc *v, int bits)
{
    v->bits = bits;
    v->t = calloc((size_t)1 << bits, sizeof(u16));
    return v->t ? 0 : -1;
}

static void vlc_add(vlc *v, u32 code, int len, int sym)
{
    int shift = v->bits - len;
    u32 i, start = code << shift;
    for (i = 0; i < (1u << shift); i++)
        v->t[start + i] = (u16)(sym << 5 | len);
}

static int build_tables(void)
{
    int i;
    if (tables_ready)
        return 0;
    if (vlc_init(&v_b14, 16) || vlc_init(&v_b15, 16) || vlc_init(&v_incr, 11) || vlc_init(&v_cbp, 9) ||
        vlc_init(&v_mv, 10) || vlc_init(&v_dc_lum, 9) || vlc_init(&v_dc_chroma, 10) || vlc_init(&v_ptype, 6) ||
        vlc_init(&v_btype, 6) || vlc_init(&v_itype, 2))
        return -1;
    for (i = 0; i < RL_COUNT + 2; i++) {
        vlc_add(&v_b14, b14_codes[i][0], b14_codes[i][1], i);
        vlc_add(&v_b15, b15_codes[i][0], b15_codes[i][1], i);
    }
    for (i = 0; i < 35; i++)
        vlc_add(&v_incr, incr_codes[i][0], incr_codes[i][1], i + 1);
    for (i = 0; i < 64; i++)
        vlc_add(&v_cbp, cbp_codes[i][0], cbp_codes[i][1], i);
    for (i = 0; i < 17; i++)
        vlc_add(&v_mv, mv_codes[i][0], mv_codes[i][1], i);
    for (i = 0; i < 12; i++) {
        vlc_add(&v_dc_lum, dc_lum_codes[i][0], dc_lum_codes[i][1], i);
        vlc_add(&v_dc_chroma, dc_chroma_codes[i][0], dc_chroma_codes[i][1], i);
    }
    for (i = 0; i < 7; i++)
        vlc_add(&v_ptype, ptype_codes[i][0], ptype_codes[i][1], ptype_codes[i][2]);
    for (i = 0; i < 11; i++)
        vlc_add(&v_btype, btype_codes[i][0], btype_codes[i][1], btype_codes[i][2]);
    vlc_add(&v_itype, 1, 1, MB_INTRA);
    vlc_add(&v_itype, 1, 2, MB_QUANT | MB_INTRA);
    tables_ready = 1;
    return 0;
}

/* --- Bit reader ---------------------------------------------------------- */

/* MSB first. The buffer always has at least 4 readable bytes past `end`. */
typedef struct {
    const u8 *p;
    u32 pos, end; /* in bits */
} bits;

static u32 show(const bits *b, int n)
{
    const u8 *q = b->p + (b->pos >> 3);
    u32 v = (u32)q[0] << 24 | (u32)q[1] << 16 | (u32)q[2] << 8 | q[3];
    return (v << (b->pos & 7)) >> (32 - n);
}

static u32 get(bits *b, int n)
{
    u32 v = show(b, n);
    b->pos += (u32)n;
    return v;
}

static int get1(bits *b) { return (int)get(b, 1); }

/* Returns the symbol, or -1 for an invalid code. */
static int vlc_get(bits *b, const vlc *v)
{
    u16 e = v->t[show(b, v->bits)];
    if (!(e & 31))
        return -1;
    b->pos += e & 31;
    return e >> 5;
}

/* --- Decoder state -------------------------------------------------------- */

enum { PIC_I = 1, PIC_P = 2, PIC_B = 3 };
enum { MT_FIELD = 1, MT_FRAME = 2, MT_DUAL = 3 };

typedef struct {
    u8 *y, *cb, *cr;
    int fields;
} frame;

struct xf_mpeg2 {
    /* elementary stream */
    u8 *buf;
    size_t len, cap, scan;
    int ended;
    /* sequence */
    int have_seq;
    int width, height, mb_w, mb_h, w, h; /* w, h: coded size, multiples of 16 */
    int progressive_seq, chroma_format;
    int rate_code, rate_ext_n, rate_ext_d;
    u8 intra_q[64], inter_q[64]; /* natural order */
    /* picture */
    int pic_type, f_code[2][2], dc_prec, structure, tff, fpfd, cmv, q_type, intra_vlc, alt_scan, rff;
    int in_picture; /* 1: header read, 2: slices started */
    int valid, warned_field;
    const u8 *scan_order;
    frame frames[3];
    int ref[2];  /* older and newer reference picture, or -1 */
    int cur;
    int newest_shown;
    int out;     /* picture to return, or -1 */
    /* slice and macroblock */
    int qscale;
    int dc_pred[3];
    int pmv[2][2][2];    /* [first/second][forward/backward][x/y] */
    int mb_flags, mtype, dct_field;
    int mv[2][4][2];     /* [direction][vector][x/y]; dual prime uses 0, 2 and 3 */
    int fsel[2][2];      /* [direction][first/second] */
    i16 blk[6][64];
};

/* --- Inverse DCT ----------------------------------------------------------- */

/* Separable fixed-point IDCT, the same as the JPEG decoder's. */
#define FIX(x) ((int)((x) * 4096 + 0.5))
static void idct_1d(const int *s, int stride, int shift, int *o, int ostride)
{
    int x0 = s[0], x1 = s[stride * 4], x2 = s[stride * 2], x3 = s[stride * 6];
    int x4 = s[stride * 1], x5 = s[stride * 7], x6 = s[stride * 5], x7 = s[stride * 3];
    int p1, p2, p3, p4, p5, t0, t1, t2, t3, t10, t11, t12, t13, r;
    p1 = (x2 + x3) * FIX(0.5411961);
    t2 = p1 + x3 * FIX(-1.847759065);
    t3 = p1 + x2 * FIX(0.765366865);
    t0 = (x0 + x1) * 4096;
    t1 = (x0 - x1) * 4096;
    t10 = t0 + t3;
    t13 = t0 - t3;
    t11 = t1 + t2;
    t12 = t1 - t2;
    t0 = x5;
    t1 = x6;
    t2 = x7;
    t3 = x4;
    p3 = t0 + t2;
    p4 = t1 + t3;
    p1 = t0 + t3;
    p2 = t1 + t2;
    p5 = (p3 + p4) * FIX(1.175875602);
    t0 = t0 * FIX(0.298631336);
    t1 = t1 * FIX(2.053119869);
    t2 = t2 * FIX(3.072711026);
    t3 = t3 * FIX(1.501321110);
    p1 = p1 * FIX(-0.899976223);
    p2 = p2 * FIX(-2.562915447);
    p3 = p3 * FIX(-1.961570560) + p5;
    p4 = p4 * FIX(-0.390180644) + p5;
    t0 += p1 + p3;
    t1 += p2 + p4;
    t2 += p2 + p3;
    t3 += p1 + p4;
    r = 1 << (shift - 1);
    o[0] = (t10 + t3 + r) >> shift;
    o[ostride * 7] = (t10 - t3 + r) >> shift;
    o[ostride * 1] = (t11 + t2 + r) >> shift;
    o[ostride * 6] = (t11 - t2 + r) >> shift;
    o[ostride * 2] = (t12 + t1 + r) >> shift;
    o[ostride * 5] = (t12 - t1 + r) >> shift;
    o[ostride * 3] = (t13 + t0 + r) >> shift;
    o[ostride * 4] = (t13 - t0 + r) >> shift;
}

static u8 clamp8(int v) { return (u8)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* Writes (add 0) or adds (add 1) the inverse transform of `blk` to an
 * 8x8 block, and clears `blk`. */
static void idct_out(i16 *blk, u8 *dst, int stride, int add)
{
    int in[64], tmp[64], res[64], i, x, y;
    int dc_only = 1;
    for (i = 1; i < 64; i++)
        if (blk[i]) {
            dc_only = 0;
            break;
        }
    if (dc_only) {
        int v = (blk[0] * 4 * 4096 + (1 << 16)) >> 17;
        for (y = 0; y < 8; y++, dst += stride)
            for (x = 0; x < 8; x++)
                dst[x] = clamp8(add ? dst[x] + v : v);
        blk[0] = 0;
        return;
    }
    for (i = 0; i < 64; i++)
        in[i] = blk[i];
    for (i = 0; i < 8; i++) {
        if (!(in[8 + i] | in[16 + i] | in[24 + i] | in[32 + i] | in[40 + i] | in[48 + i] | in[56 + i])) {
            int v = in[i] * 4;
            for (y = 0; y < 8; y++)
                tmp[y * 8 + i] = v;
        } else {
            idct_1d(in + i, 8, 10, tmp + i, 8);
        }
    }
    for (i = 0; i < 8; i++)
        idct_1d(tmp + i * 8, 1, 17, res + i * 8, 1);
    for (y = 0; y < 8; y++, dst += stride)
        for (x = 0; x < 8; x++)
            dst[x] = clamp8(add ? dst[x] + res[y * 8 + x] : res[y * 8 + x]);
    memset(blk, 0, 64 * sizeof(i16));
}

/* --- Blocks ------------------------------------------------------------------ */

static int saturate(int v) { return v > 2047 ? 2047 : v < -2048 ? -2048 : v; }

/* Reads one run/level pair. Returns 0, 1 at the end of the block, -1 on
 * error. */
static int read_coef(bits *b, const vlc *table, int *run, int *level)
{
    int sym = vlc_get(b, table);
    if (sym < 0)
        return -1;
    if (sym == RL_EOB)
        return 1;
    if (sym == RL_ESCAPE) {
        int l;
        *run = (int)get(b, 6);
        l = (int)get(b, 12);
        if (l == 0 || l == 2048)
            return -1;
        *level = l >= 2048 ? l - 4096 : l;
        return 0;
    }
    *run = rl_run[sym];
    *level = get1(b) ? -rl_level[sym] : rl_level[sym];
    return 0;
}

static int block_intra(xf_mpeg2 *d, bits *b, int cc, i16 *blk)
{
    int size = vlc_get(b, cc ? &v_dc_chroma : &v_dc_lum);
    int i = 0, sum, run, level, r;
    const vlc *table = d->intra_vlc ? &v_b15 : &v_b14;
    if (size < 0)
        return -1;
    if (size) {
        int diff = (int)get(b, size);
        if (diff < 1 << (size - 1))
            diff -= (1 << size) - 1;
        d->dc_pred[cc] += diff;
    }
    blk[0] = (i16)(d->dc_pred[cc] << (3 - d->dc_prec));
    sum = blk[0];
    for (;;) {
        int j, v;
        r = read_coef(b, table, &run, &level);
        if (r < 0)
            return -1;
        if (r)
            break;
        i += run + 1;
        if (i > 63)
            return -1;
        j = d->scan_order[i];
        v = (level < 0 ? -level : level) * d->qscale * d->intra_q[j] / 16;
        v = saturate(level < 0 ? -v : v);
        blk[j] = (i16)v;
        sum ^= v;
    }
    if (!(sum & 1))
        blk[63] ^= 1;
    return 0;
}

static int block_inter(xf_mpeg2 *d, bits *b, i16 *blk)
{
    int i = -1, sum = 0, run, level, r, first = 1;
    for (;;) {
        int j, v;
        if (first && show(b, 1)) {
            /* The first coefficient's '1s' code: run 0, level 1. */
            b->pos++;
            run = 0;
            level = get1(b) ? -1 : 1;
        } else {
            r = read_coef(b, &v_b14, &run, &level);
            if (r < 0)
                return -1;
            if (r)
                break;
        }
        first = 0;
        i += run + 1;
        if (i > 63)
            return -1;
        j = d->scan_order[i];
        v = (2 * (level < 0 ? -level : level) + 1) * d->qscale * d->inter_q[j] / 32;
        v = saturate(level < 0 ? -v : v);
        blk[j] = (i16)v;
        sum ^= v;
    }
    if (!(sum & 1))
        blk[63] ^= 1;
    return 0;
}

/* --- Motion compensation ------------------------------------------------------ */

/* Puts (avg 0) or averages (avg 1) into dst the w x h block at half-pel
 * position (hx, hy) of a plane rw x rh with row stride rs. Positions
 * outside the plane repeat its edge. */
static void predict(u8 *dst, int ds, const u8 *ref, int rs, int rw, int rh, int hx, int hy, int w, int h, int avg)
{
    u8 tmp[17 * 17];
    const u8 *src;
    int ss, x, y, x0 = hx >> 1, y0 = hy >> 1, fx = hx & 1, fy = hy & 1;
    if (x0 < 0 || y0 < 0 || x0 + w + fx > rw || y0 + h + fy > rh) {
        for (y = 0; y <= h; y++) {
            int cy = y0 + y < 0 ? 0 : y0 + y >= rh ? rh - 1 : y0 + y;
            for (x = 0; x <= w; x++) {
                int cx = x0 + x < 0 ? 0 : x0 + x >= rw ? rw - 1 : x0 + x;
                tmp[y * 17 + x] = ref[cy * rs + cx];
            }
        }
        src = tmp;
        ss = 17;
    } else {
        src = ref + y0 * rs + x0;
        ss = rs;
    }
    /* one loop per half-pel case, kept apart for speed */
#define PREDICT_LOOP(expr)                                              \
    for (y = 0; y < h; y++, src += ss, dst += ds) {                     \
        if (avg)                                                        \
            for (x = 0; x < w; x++)                                     \
                dst[x] = (u8)((dst[x] + (expr) + 1) >> 1);              \
        else                                                            \
            for (x = 0; x < w; x++)                                     \
                dst[x] = (u8)(expr);                                    \
    }
    if (fx && fy)
        PREDICT_LOOP((src[x] + src[x + 1] + src[ss + x] + src[ss + x + 1] + 2) >> 2)
    else if (fx)
        PREDICT_LOOP((src[x] + src[x + 1] + 1) >> 1)
    else if (fy)
        PREDICT_LOOP((src[x] + src[ss + x] + 1) >> 1)
    else
        PREDICT_LOOP(src[x])
#undef PREDICT_LOOP
}

/* Frame prediction of the whole macroblock. */
static void mc_frame(xf_mpeg2 *d, const frame *ref, const frame *cur, int mbx, int mby, int mx, int my, int avg)
{
    int cw = d->w / 2, ch = d->h / 2;
    predict(cur->y + mby * 16 * d->w + mbx * 16, d->w, ref->y, d->w, d->w, d->h,
            mbx * 32 + mx, mby * 32 + my, 16, 16, avg);
    mx /= 2;
    my /= 2;
    predict(cur->cb + mby * 8 * cw + mbx * 8, cw, ref->cb, cw, cw, ch, mbx * 16 + mx, mby * 16 + my, 8, 8, avg);
    predict(cur->cr + mby * 8 * cw + mbx * 8, cw, ref->cr, cw, cw, ch, mbx * 16 + mx, mby * 16 + my, 8, 8, avg);
}

/* Prediction of one field (parity `dst_field`) of the macroblock from
 * field `src_field` of the reference; my is in field rows. */
static void mc_field(xf_mpeg2 *d, const frame *ref, const frame *cur, int mbx, int mby, int dst_field,
                     int src_field, int mx, int my, int avg)
{
    int w = d->w, cw = d->w / 2;
    predict(cur->y + (mby * 16 + dst_field) * w + mbx * 16, w * 2, ref->y + src_field * w, w * 2, w, d->h / 2,
            mbx * 32 + mx, mby * 16 + my, 16, 8, avg);
    mx /= 2;
    my /= 2;
    predict(cur->cb + (mby * 8 + dst_field) * cw + mbx * 8, cw * 2, ref->cb + src_field * cw, cw * 2, cw, d->h / 4,
            mbx * 16 + mx, mby * 8 + my, 8, 4, avg);
    predict(cur->cr + (mby * 8 + dst_field) * cw + mbx * 8, cw * 2, ref->cr + src_field * cw, cw * 2, cw, d->h / 4,
            mbx * 16 + mx, mby * 8 + my, 8, 4, avg);
}

static void predict_mb(xf_mpeg2 *d, int mbx, int mby)
{
    const frame *cur = &d->frames[d->cur];
    int dir, avg = 0;
    for (dir = 0; dir < 2; dir++) {
        const frame *ref;
        int f;
        if (!(d->mb_flags & (dir ? MB_BACKWARD : MB_FORWARD)))
            continue;
        /* P pictures: ref[1] is the picture itself, ref[0] the one before.
         * B pictures: forward from ref[0] (or ref[1] when the GOP opens
         * on a reference we never had), backward from ref[1]. */
        ref = &d->frames[dir == 0 && d->ref[0] >= 0 ? d->ref[0] : d->ref[1]];
        switch (d->mtype) {
        case MT_FRAME:
            mc_frame(d, ref, cur, mbx, mby, d->mv[dir][0][0], d->mv[dir][0][1], avg);
            break;
        case MT_FIELD:
            for (f = 0; f < 2; f++)
                mc_field(d, ref, cur, mbx, mby, f, d->fsel[dir][f], d->mv[dir][f][0], d->mv[dir][f][1], avg);
            break;
        default: /* dual prime: same parity, then opposite parity averaged in */
            for (f = 0; f < 2; f++) {
                mc_field(d, ref, cur, mbx, mby, f, f, d->mv[dir][0][0], d->mv[dir][0][1], avg);
                mc_field(d, ref, cur, mbx, mby, f, f ^ 1, d->mv[dir][2 + f][0], d->mv[dir][2 + f][1], 1);
            }
            break;
        }
        avg = 1;
    }
}

/* --- Macroblocks --------------------------------------------------------------- */

static int motion_delta(bits *b, int f_code, int pred)
{
    int code = vlc_get(b, &v_mv), r_size = f_code - 1, delta, v;
    int low = -(16 << r_size), high = (16 << r_size) - 1, range = 32 << r_size;
    if (code < 0)
        return pred;
    if (code == 0) {
        delta = 0;
    } else {
        int neg = get1(b);
        delta = r_size ? ((code - 1) << r_size) + (int)get(b, r_size) + 1 : code;
        if (neg)
            delta = -delta;
    }
    v = pred + delta;
    if (v < low)
        v += range;
    else if (v > high)
        v -= range;
    return v;
}

static int dmv(bits *b)
{
    if (!get1(b))
        return 0;
    return get1(b) ? -1 : 1;
}

/* motion_vectors(s) of a frame picture. */
static void read_vectors(xf_mpeg2 *d, bits *b, int s, int mtype)
{
    int r;
    switch (mtype) {
    case MT_FRAME:
        d->mv[s][0][0] = d->pmv[0][s][0] = d->pmv[1][s][0] = motion_delta(b, d->f_code[s][0], d->pmv[0][s][0]);
        d->mv[s][0][1] = d->pmv[0][s][1] = d->pmv[1][s][1] = motion_delta(b, d->f_code[s][1], d->pmv[0][s][1]);
        break;
    case MT_FIELD:
        for (r = 0; r < 2; r++) {
            d->fsel[s][r] = get1(b);
            d->mv[s][r][0] = d->pmv[r][s][0] = motion_delta(b, d->f_code[s][0], d->pmv[r][s][0]);
            d->mv[s][r][1] = motion_delta(b, d->f_code[s][1], d->pmv[r][s][1] >> 1);
            d->pmv[r][s][1] = d->mv[s][r][1] * 2;
        }
        break;
    default: {
        int mx, my, dx, dy, m;
        mx = motion_delta(b, d->f_code[s][0], d->pmv[0][s][0]);
        dx = dmv(b);
        my = motion_delta(b, d->f_code[s][1], d->pmv[0][s][1] >> 1);
        dy = dmv(b);
        d->pmv[0][s][0] = d->pmv[1][s][0] = mx;
        d->pmv[0][s][1] = d->pmv[1][s][1] = my * 2;
        d->mv[s][0][0] = mx;
        d->mv[s][0][1] = my;
        /* the opposite-parity vectors, scaled by the field distance */
        m = d->tff ? 1 : 3;
        d->mv[s][2][0] = ((mx * m + (mx > 0)) >> 1) + dx;
        d->mv[s][2][1] = ((my * m + (my > 0)) >> 1) + dy - 1;
        m = 4 - m;
        d->mv[s][3][0] = ((mx * m + (mx > 0)) >> 1) + dx;
        d->mv[s][3][1] = ((my * m + (my > 0)) >> 1) + dy + 1;
        break;
    }
    }
}

static void reset_dc(xf_mpeg2 *d) { d->dc_pred[0] = d->dc_pred[1] = d->dc_pred[2] = 1 << (7 + d->dc_prec); }

static void add_blocks(xf_mpeg2 *d, int mbx, int mby, int cbp, int intra)
{
    const frame *cur = &d->frames[d->cur];
    int w = d->w, cw = d->w / 2, k;
    u8 *y = cur->y + mby * 16 * w + mbx * 16;
    for (k = 0; k < 4; k++) {
        u8 *dst;
        if (!(cbp & (32 >> k)))
            continue;
        if (d->dct_field)
            dst = y + (k >> 1) * w + (k & 1) * 8;
        else
            dst = y + (k >> 1) * 8 * w + (k & 1) * 8;
        idct_out(d->blk[k], dst, d->dct_field ? w * 2 : w, !intra);
    }
    if (cbp & 2)
        idct_out(d->blk[4], cur->cb + mby * 8 * cw + mbx * 8, cw, !intra);
    if (cbp & 1)
        idct_out(d->blk[5], cur->cr + mby * 8 * cw + mbx * 8, cw, !intra);
}

static int read_qscale(xf_mpeg2 *d, bits *b)
{
    int code = (int)get(b, 5);
    return d->q_type ? non_linear_qscale[code] : code * 2;
}

static int decode_mb(xf_mpeg2 *d, bits *b, int mbx, int mby)
{
    int flags, cbp, k;
    switch (d->pic_type) {
    case PIC_I:
        flags = vlc_get(b, &v_itype);
        break;
    case PIC_P:
        flags = vlc_get(b, &v_ptype);
        break;
    default:
        flags = vlc_get(b, &v_btype);
        break;
    }
    if (flags < 0)
        return -1;
    d->mtype = MT_FRAME;
    if ((flags & (MB_FORWARD | MB_BACKWARD)) && !d->fpfd) {
        d->mtype = (int)get(b, 2);
        if (d->mtype == 0)
            return -1;
    }
    d->dct_field = !d->fpfd && (flags & (MB_INTRA | MB_PATTERN)) ? get1(b) : 0;
    if (flags & MB_QUANT)
        d->qscale = read_qscale(d, b);

    if (flags & MB_INTRA) {
        if (d->cmv) {
            read_vectors(d, b, 0, MT_FRAME);
            get1(b); /* marker */
        } else {
            memset(d->pmv, 0, sizeof(d->pmv));
        }
        d->mb_flags = flags;
        for (k = 0; k < 6; k++)
            if (block_intra(d, b, k < 4 ? 0 : k - 3, d->blk[k]) != 0)
                return -1;
        add_blocks(d, mbx, mby, 63, 1);
        return 0;
    }

    reset_dc(d);
    if (flags & MB_FORWARD)
        read_vectors(d, b, 0, d->mtype);
    if (flags & MB_BACKWARD)
        read_vectors(d, b, 1, d->mtype);
    if (d->pic_type == PIC_P && !(flags & MB_FORWARD)) {
        /* no motion compensation: a zero frame vector */
        memset(d->pmv, 0, sizeof(d->pmv));
        d->mv[0][0][0] = d->mv[0][0][1] = 0;
        d->mtype = MT_FRAME;
        flags |= MB_FORWARD;
    }
    d->mb_flags = flags;
    cbp = 0;
    if (flags & MB_PATTERN) {
        cbp = vlc_get(b, &v_cbp);
        if (cbp < 0)
            return -1;
        for (k = 0; k < 6; k++)
            if ((cbp & (32 >> k)) && block_inter(d, b, d->blk[k]) != 0)
                return -1;
    }
    predict_mb(d, mbx, mby);
    add_blocks(d, mbx, mby, cbp, 0);
    return 0;
}

static void skip_mb(xf_mpeg2 *d, int mbx, int mby)
{
    reset_dc(d);
    if (d->pic_type == PIC_P) {
        memset(d->pmv, 0, sizeof(d->pmv));
        d->mb_flags = MB_FORWARD;
        d->mtype = MT_FRAME;
        d->mv[0][0][0] = d->mv[0][0][1] = 0;
    } else if (d->mb_flags & MB_INTRA) {
        return; /* not allowed after an intra macroblock */
    } else {
        /* the previous directions, frame prediction with the predictors */
        int s;
        d->mtype = MT_FRAME;
        for (s = 0; s < 2; s++) {
            d->mv[s][0][0] = d->pmv[0][s][0];
            d->mv[s][0][1] = d->pmv[0][s][1];
        }
    }
    predict_mb(d, mbx, mby);
}

/* Returns the increment, or -1 on error or at the end of the slice. */
static int read_incr(bits *b)
{
    int incr = 0;
    for (;;) {
        int v = vlc_get(b, &v_incr);
        if (v < 0)
            return -1;
        if (v == INCR_ESCAPE)
            incr += 33;
        else if (v != INCR_STUFFING)
            return incr + v;
    }
}

static void decode_slice(xf_mpeg2 *d, int row, const u8 *data, size_t size)
{
    bits b;
    int addr, incr, total = d->mb_w * d->mb_h;
    b.p = data;
    b.pos = 0;
    b.end = (u32)size * 8;
    if (row >= d->mb_h)
        return;
    d->qscale = read_qscale(d, &b);
    if (get1(&b)) { /* intra_slice_flag, intra_slice, reserved, extra information */
        get(&b, 8);
        while (get1(&b))
            get(&b, 8);
    }
    reset_dc(d);
    memset(d->pmv, 0, sizeof(d->pmv));
    incr = read_incr(&b);
    if (incr < 1)
        return;
    addr = row * d->mb_w + incr - 1;
    d->mb_flags = MB_INTRA;
    for (;;) {
        if (addr >= total || decode_mb(d, &b, addr % d->mb_w, addr / d->mb_w) != 0 || b.pos > b.end)
            return;
        if (show(&b, 23) == 0) /* only zero bits before the next start code */
            return;
        incr = read_incr(&b);
        if (incr < 1)
            return;
        while (--incr > 0) {
            if (++addr >= total)
                return;
            skip_mb(d, addr % d->mb_w, addr / d->mb_w);
        }
        addr++;
    }
}

/* --- Headers and pictures ----------------------------------------------------- */

static int alloc_frames(xf_mpeg2 *d)
{
    int i;
    size_t luma = (size_t)d->w * d->h;
    for (i = 0; i < 3; i++) {
        free(d->frames[i].y);
        d->frames[i].y = malloc(luma * 3 / 2);
        if (!d->frames[i].y)
            return -1;
        memset(d->frames[i].y, 16, luma);
        memset(d->frames[i].y + luma, 128, luma / 2);
        d->frames[i].cb = d->frames[i].y + luma;
        d->frames[i].cr = d->frames[i].cb + luma / 4;
    }
    d->ref[0] = d->ref[1] = -1;
    d->newest_shown = 1;
    return 0;
}

static void load_matrix(bits *b, u8 *q)
{
    int i;
    for (i = 0; i < 64; i++)
        q[zigzag[i]] = (u8)get(b, 8);
}

static void sequence_header(xf_mpeg2 *d, bits *b)
{
    int w = (int)get(b, 12), h = (int)get(b, 12), i;
    get(b, 4); /* aspect ratio */
    d->rate_code = (int)get(b, 4);
    get(b, 18 + 1 + 10 + 1); /* bit rate, marker, vbv buffer size, constrained parameters */
    if (get1(b))
        load_matrix(b, d->intra_q);
    else
        for (i = 0; i < 64; i++)
            d->intra_q[zigzag[i]] = default_intra_matrix[i];
    if (get1(b))
        load_matrix(b, d->inter_q);
    else
        memset(d->inter_q, 16, 64);
    if (!d->have_seq || w != d->width || h != d->height) {
        d->width = w;
        d->height = h;
        d->have_seq = 0;
    }
    d->progressive_seq = 1;
    d->chroma_format = 1;
}

static void sequence_extension(xf_mpeg2 *d, bits *b)
{
    get(b, 8); /* profile and level */
    d->progressive_seq = get1(b);
    d->chroma_format = (int)get(b, 2);
    d->width |= (int)get(b, 2) << 12;
    d->height |= (int)get(b, 2) << 12;
    get(b, 12 + 1 + 8 + 1); /* bit rate, marker, vbv buffer size, low delay */
    d->rate_ext_n = (int)get(b, 2);
    d->rate_ext_d = (int)get(b, 5);
    if (!d->have_seq && d->width > 0 && d->height > 0 && d->width <= 1920 && d->height <= 1152) {
        d->mb_w = (d->width + 15) / 16;
        d->mb_h = d->progressive_seq ? (d->height + 15) / 16 : 2 * ((d->height + 31) / 32);
        d->w = d->mb_w * 16;
        d->h = d->mb_h * 16;
        d->have_seq = alloc_frames(d) == 0;
    }
}

static void picture_header(xf_mpeg2 *d, bits *b)
{
    get(b, 10); /* temporal reference */
    d->pic_type = (int)get(b, 3);
    d->in_picture = 1;
    d->valid = 0;
    d->structure = 0;
}

static void picture_coding_extension(xf_mpeg2 *d, bits *b)
{
    d->f_code[0][0] = (int)get(b, 4);
    d->f_code[0][1] = (int)get(b, 4);
    d->f_code[1][0] = (int)get(b, 4);
    d->f_code[1][1] = (int)get(b, 4);
    d->dc_prec = (int)get(b, 2);
    d->structure = (int)get(b, 2);
    d->tff = get1(b);
    d->fpfd = get1(b);
    d->cmv = get1(b);
    d->q_type = get1(b);
    d->intra_vlc = get1(b);
    d->alt_scan = get1(b);
    d->rff = get1(b);
    d->scan_order = d->alt_scan ? alternate_scan : zigzag;
}

static void quant_matrix_extension(xf_mpeg2 *d, bits *b)
{
    if (get1(b))
        load_matrix(b, d->intra_q);
    if (get1(b))
        load_matrix(b, d->inter_q);
}

/* The first slice of a picture: picks its frame buffer. */
static void start_picture(xf_mpeg2 *d)
{
    int i;
    d->valid = 0;
    if (!d->have_seq || d->chroma_format != 1 || d->pic_type < PIC_I || d->pic_type > PIC_B)
        return;
    if (d->structure != 3) {
        if (!d->warned_field)
            xf_warn("mpeg2: field pictures are not supported");
        d->warned_field = 1;
        return;
    }
    if (d->pic_type != PIC_I && d->ref[1] < 0)
        return; /* nothing to predict from */
    if (d->pic_type == PIC_B) {
        for (i = 0; i < 3 && (i == d->ref[0] || i == d->ref[1]); i++)
            ;
    } else {
        /* the older reference has been shown and can go */
        if (d->ref[0] >= 0 && d->ref[0] != d->ref[1])
            i = d->ref[0];
        else
            for (i = 0; i < 2 && i == d->ref[1]; i++)
                ;
        d->ref[0] = d->ref[1];
        d->ref[1] = i;
    }
    d->cur = i;
    d->frames[i].fields = d->progressive_seq ? 2 : 2 + d->rff;
    d->valid = 1;
}

static void finish_picture(xf_mpeg2 *d)
{
    if (!d->in_picture)
        return;
    d->in_picture = 0;
    if (!d->valid)
        return;
    d->valid = 0;
    if (d->pic_type == PIC_B) {
        d->out = d->cur;
    } else {
        if (!d->newest_shown && d->ref[0] >= 0)
            d->out = d->ref[0];
        d->newest_shown = 0;
    }
}

static void process_unit(xf_mpeg2 *d, int code, const u8 *data, size_t size)
{
    bits b;
    b.p = data;
    b.pos = 0;
    b.end = (u32)size * 8;
    if (code >= 0x01 && code <= 0xAF) {
        if (d->in_picture == 1) {
            /* the first slice */
            start_picture(d);
            d->in_picture = 2;
        }
        if (d->valid)
            decode_slice(d, code - 1, data, size);
        return;
    }
    if (code == 0x00 || code == 0xB3 || code == 0xB7 || code == 0xB8)
        finish_picture(d); /* not on extensions and user data */
    switch (code) {
    case 0x00:
        picture_header(d, &b);
        break;
    case 0xB3:
        sequence_header(d, &b);
        break;
    case 0xB5:
        switch (get(&b, 4)) {
        case 1:
            sequence_extension(d, &b);
            break;
        case 3:
            quant_matrix_extension(d, &b);
            break;
        case 8:
            picture_coding_extension(d, &b);
            break;
        }
        break;
    }
}

/* --- Interface ------------------------------------------------------------------ */

xf_mpeg2 *xf_mpeg2_new(void)
{
    xf_mpeg2 *d;
    if (build_tables() != 0)
        return NULL;
    d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->ref[0] = d->ref[1] = d->out = -1;
    d->newest_shown = 1;
    return d;
}

void xf_mpeg2_free(xf_mpeg2 *d)
{
    int i;
    if (!d)
        return;
    for (i = 0; i < 3; i++)
        free(d->frames[i].y);
    free(d->buf);
    free(d);
}

void xf_mpeg2_reset(xf_mpeg2 *d)
{
    d->len = d->scan = 0;
    d->ended = 0;
    d->in_picture = d->valid = 0;
    d->ref[0] = d->ref[1] = d->out = -1;
    d->newest_shown = 1;
}

int xf_mpeg2_push(xf_mpeg2 *d, const u8 *data, size_t size)
{
    if (d->scan > 0 && d->scan >= d->len / 2) {
        memmove(d->buf, d->buf + d->scan, d->len - d->scan);
        d->len -= d->scan;
        d->scan = 0;
    }
    if (d->len + size + 8 > d->cap) {
        size_t cap = (d->len + size + 8) * 2;
        u8 *p = realloc(d->buf, cap);
        if (!p)
            return -1;
        d->buf = p;
        d->cap = cap;
    }
    memcpy(d->buf + d->len, data, size);
    d->len += size;
    memset(d->buf + d->len, 0, 8);
    return 0;
}

void xf_mpeg2_end(xf_mpeg2 *d) { d->ended = 1; }

/* Position of the next start code prefix at or after `from`, or `len`. */
static size_t find_start(const u8 *p, size_t from, size_t len)
{
    size_t i;
    for (i = from; i + 3 < len + 1; i++) {
        if (p[i + 2] > 1)
            i += 2;
        else if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1)
            return i;
    }
    return len;
}

static void fill_picture(xf_mpeg2 *d, int i, xf_mpeg2_picture *pic)
{
    int code = d->rate_code >= 1 && d->rate_code <= 8 ? d->rate_code : 4;
    pic->y = d->frames[i].y;
    pic->cb = d->frames[i].cb;
    pic->cr = d->frames[i].cr;
    pic->width = d->width;
    pic->height = d->height;
    pic->stride = d->w;
    pic->cstride = d->w / 2;
    pic->fields = d->frames[i].fields;
    pic->field_rate_num = frame_rates[code][0] * 2 * (d->rate_ext_n + 1);
    pic->field_rate_den = frame_rates[code][1] * (d->rate_ext_d + 1);
}

int xf_mpeg2_next(xf_mpeg2 *d, xf_mpeg2_picture *pic)
{
    for (;;) {
        size_t s, n;
        if (d->out >= 0) {
            fill_picture(d, d->out, pic);
            d->out = -1;
            return 1;
        }
        s = find_start(d->buf, d->scan, d->len);
        if (s + 4 > d->len) {
            if (!d->ended)
                return 0;
            /* the end: the last picture, then the newest reference */
            d->scan = d->len;
            finish_picture(d);
            if (d->out >= 0)
                continue;
            if (!d->newest_shown && d->ref[1] >= 0) {
                d->newest_shown = 1;
                d->out = d->ref[1];
                continue;
            }
            return 0;
        }
        n = find_start(d->buf, s + 4, d->len);
        if (n >= d->len && !d->ended) {
            d->scan = s;
            return 0;
        }
        d->scan = n;
        process_unit(d, d->buf[s + 3], d->buf + s + 4, n - s - 4);
    }
}
