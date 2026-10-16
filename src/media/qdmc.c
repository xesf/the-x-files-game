/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf
 * Based on FFmpeg's QDMC decoder, Copyright (c) 2017 Paul B Mahol
 * (LGPL-2.1-or-later): the tables and the synthesis follow it. */
#include "qdmc.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "xf_endian.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_TONES 8192
#define MAX_SUBFRAME 256 /* frame_size / 32 */
#define BUFFER_SIZE 32768

typedef struct {
    u8 mode, phase, offset;
    i16 freq, amplitude;
} tone;

struct xf_qdmc {
    int channels, sample_rate, bit_rate;
    int frame_bits, frame_size, subframe_size;
    int band_index;
    u32 packet_bytes;    /* the "checksum size": bytes covered by a packet */
    int fft_n;           /* inverse FFT length: 2 * subframe_size */

    u8 noise[2][19][17];
    tone tones[5][MAX_TONES];
    int nb_tones[5], cur_tone[5];
    float alt_sin[5][31];
    float fft_buffer[4][8192 * 2]; /* 0, 1: imaginary per channel; 2, 3: real */
    float noise2_buffer[4096 * 2];
    float noise_buffer[4096 * 2];
    float buffer[2 * BUFFER_SIZE];
    int fft_offset, buffer_offset;
    u32 rndval;

    float in_re[2][2 * MAX_SUBFRAME], in_im[2][2 * MAX_SUBFRAME];
    float tw_re[MAX_SUBFRAME], tw_im[MAX_SUBFRAME];
};

/* --- Tables (from FFmpeg) ------------------------------------------- */

static const u32 g_code_prefix[] = {
    0x0,     0x1,     0x2,     0x3,     0x4,     0x6,     0x8,     0xA,     0xC,     0x10,    0x14,
    0x18,    0x1C,    0x24,    0x2C,    0x34,    0x3C,    0x4C,    0x5C,    0x6C,    0x7C,    0x9C,
    0xBC,    0xDC,    0xFC,    0x13C,   0x17C,   0x1BC,   0x1FC,   0x27C,   0x2FC,   0x37C,   0x3FC,
    0x4FC,   0x5FC,   0x6FC,   0x7FC,   0x9FC,   0xBFC,   0xDFC,   0xFFC,   0x13FC,  0x17FC,  0x1BFC,
    0x1FFC,  0x27FC,  0x2FFC,  0x37FC,  0x3FFC,  0x4FFC,  0x5FFC,  0x6FFC,  0x7FFC,  0x9FFC,  0xBFFC,
    0xDFFC,  0xFFFC,  0x13FFC, 0x17FFC, 0x1BFFC, 0x1FFFC, 0x27FFC, 0x2FFFC, 0x37FFC, 0x3FFFC,
};

static const float g_amplitude[64] = {
    1.18750000f, 1.68359380f, 2.37500000f, 3.36718750f, 4.75000000f, 6.73437500f, 9.50000000f,
    13.4687500f, 19.0000000f, 26.9375000f, 38.0000000f, 53.8750000f, 76.0000000f, 107.750000f,
    152.000000f, 215.500000f, 304.000000f, 431.000000f, 608.000000f, 862.000000f, 1216.00000f,
    1724.00000f, 2432.00000f, 3448.00000f, 4864.00000f, 6896.00000f, 9728.00000f, 13792.0000f,
    19456.0000f, 27584.0000f, 38912.0000f, 55168.0000f, 77824.0000f, 110336.000f, 155648.000f,
    220672.000f, 311296.000f, 441344.000f, 622592.000f, 882688.000f, 1245184.00f, 1765376.00f,
    2490368.00f, 3530752.00f, 4980736.00f, 7061504.00f,
};

/* Noise band edges, 21 per band layout. */
static const u16 g_nodes[112] = {
    0, 1,  2,  4,  6,  8,  12, 16, 24, 32, 48, 56, 64, 80, 96,  120, 144, 176, 208, 240, 256,
    0, 2,  4,  8,  16, 24, 32, 48, 56, 64, 80, 104, 128, 160, 208, 256, 0, 0, 0, 0, 0,
    0, 2,  4,  8,  16, 32, 48, 64, 80, 112, 160, 208, 256, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 4,  8,  16, 32, 48, 64, 96, 144, 208, 256, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 4,  16, 32, 64, 256, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0,  0,  0,  0,  0,  0,
};

static const u8 g_noise_bands_size[] = { 19, 14, 11, 9, 4, 2, 0 };
static const u8 g_noise_bands_selector[] = { 4, 3, 2, 1, 0, 0, 0 };

/* Huffman tables as (symbol + 1, code length) in canonical order; symbol
 * -1 is the escape: the value follows in 1 to 8 bits. */
static const u8 g_huff[][2] = {
    /* noise value, 27 */
    { 1, 2 }, { 10, 7 }, { 26, 9 }, { 22, 9 }, { 24, 9 }, { 14, 9 }, { 8, 6 }, { 6, 5 }, { 7, 5 },
    { 9, 7 }, { 30, 9 }, { 32, 10 }, { 13, 10 }, { 20, 9 }, { 28, 9 }, { 12, 7 }, { 15, 11 }, { 36, 12 },
    { 0, 12 }, { 34, 10 }, { 18, 9 }, { 11, 9 }, { 16, 9 }, { 5, 3 }, { 2, 3 }, { 4, 3 }, { 3, 2 },
    /* noise segment length, 12 */
    { 1, 1 }, { 2, 2 }, { 3, 4 }, { 8, 9 }, { 9, 10 }, { 0, 10 }, { 13, 8 }, { 7, 7 }, { 6, 6 },
    { 17, 5 }, { 4, 4 }, { 5, 4 },
    /* amplitude, 28 */
    { 18, 3 }, { 16, 3 }, { 22, 7 }, { 8, 10 }, { 4, 10 }, { 3, 9 }, { 2, 8 }, { 23, 8 }, { 10, 8 },
    { 11, 7 }, { 21, 5 }, { 20, 4 }, { 1, 7 }, { 7, 10 }, { 5, 10 }, { 9, 9 }, { 6, 10 }, { 25, 11 },
    { 26, 12 }, { 27, 13 }, { 0, 13 }, { 24, 9 }, { 12, 6 }, { 13, 5 }, { 14, 4 }, { 19, 3 }, { 15, 3 },
    { 17, 2 },
    /* frequency differences, 47 */
    { 2, 4 }, { 14, 6 }, { 26, 7 }, { 31, 8 }, { 32, 9 }, { 35, 9 }, { 7, 5 }, { 10, 5 }, { 22, 7 },
    { 27, 7 }, { 19, 7 }, { 20, 7 }, { 4, 5 }, { 13, 5 }, { 17, 6 }, { 15, 6 }, { 8, 5 }, { 5, 4 },
    { 28, 7 }, { 33, 9 }, { 36, 11 }, { 38, 12 }, { 42, 14 }, { 45, 16 }, { 44, 18 }, { 0, 18 },
    { 46, 17 }, { 43, 15 }, { 40, 13 }, { 37, 11 }, { 39, 12 }, { 41, 12 }, { 34, 8 }, { 16, 6 },
    { 11, 5 }, { 9, 4 }, { 1, 2 }, { 3, 4 }, { 30, 7 }, { 29, 7 }, { 23, 6 }, { 24, 6 }, { 18, 6 },
    { 6, 4 }, { 12, 5 }, { 21, 6 }, { 25, 6 },
    /* amplitude differences, 9 */
    { 1, 2 }, { 3, 3 }, { 4, 4 }, { 5, 5 }, { 6, 6 }, { 7, 7 }, { 8, 8 }, { 0, 8 }, { 2, 1 },
    /* phase differences, 9 */
    { 2, 2 }, { 1, 2 }, { 3, 4 }, { 7, 4 }, { 6, 5 }, { 5, 6 }, { 0, 6 }, { 4, 4 }, { 8, 2 },
};

enum { VLC_NOISE, VLC_SEGMENT, VLC_AMP, VLC_FREQ, VLC_AMP_DIFF, VLC_PHASE_DIFF, VLC_COUNT };
static const u8 g_huff_sizes[VLC_COUNT] = { 27, 12, 28, 47, 9, 9 };

#define MAX_CODES 47

typedef struct {
    int count;
    u32 code[MAX_CODES];
    u8 len[MAX_CODES];
    i8 sym[MAX_CODES];
} vlc;

static vlc g_vlc[VLC_COUNT];
static float g_sin[512];
static int g_ready;

static void init_tables(void)
{
    int t, i, k = 0;
    if (g_ready)
        return;
    for (t = 0; t < VLC_COUNT; t++) {
        /* canonical codes in table order, as 32-bit left-aligned values */
        u32 code = 0;
        g_vlc[t].count = g_huff_sizes[t];
        for (i = 0; i < g_huff_sizes[t]; i++, k++) {
            int len = g_huff[k][1];
            g_vlc[t].len[i] = (u8)len;
            g_vlc[t].sym[i] = (i8)(g_huff[k][0] - 1);
            g_vlc[t].code[i] = code >> (32 - len);
            code += 1u << (32 - len);
        }
    }
    for (i = 0; i < 512; i++)
        g_sin[i] = (float)sin(2.0f * i * M_PI * 0.001953125f);
    g_ready = 1;
}

/* --- Bits, least significant first ------------------------------------- */

typedef struct {
    const u8 *p;
    u32 size, pos; /* in bits */
} bits;

static u32 get_bit(bits *b)
{
    u32 v;
    if (b->pos >= b->size) {
        b->pos++;
        return 0;
    }
    v = (b->p[b->pos >> 3] >> (b->pos & 7)) & 1;
    b->pos++;
    return v;
}

static u32 get_bits(bits *b, int n)
{
    u32 v = 0;
    int i;
    for (i = 0; i < n; i++)
        v |= get_bit(b) << i;
    return v;
}

static int bits_left(const bits *b) { return (int)b->size - (int)b->pos; }

/* A Huffman value; `prefixed` values grow by extra bits (code_prefix).
 * Returns -1 on bad data. */
static int get_vlc(bits *b, int table, int prefixed)
{
    const vlc *t = &g_vlc[table];
    u32 code = 0;
    int len, i, v = -2;
    if (bits_left(b) < 1)
        return -1;
    for (len = 1; len <= 18 && v == -2; len++) {
        code = code << 1 | get_bit(b);
        for (i = 0; i < t->count; i++)
            if (t->len[i] == len && t->code[i] == code) {
                v = t->sym[i];
                break;
            }
    }
    if (v == -2)
        return -1;
    if (v < 0) /* escape */
        v = (int)get_bits(b, (int)get_bits(b, 3) + 1);
    if (prefixed) {
        if (v >= (int)XF_COUNT(g_code_prefix))
            return -1;
        v = (int)(g_code_prefix[v] + get_bits(b, v >> 2));
    }
    return v;
}

/* --- Setup ----------------------------------------------------------- */

static void make_noises(xf_qdmc *q)
{
    int i, j, n0, n1, n2, diff;
    float *p;
    for (j = 0; j < g_noise_bands_size[q->band_index]; j++) {
        n0 = g_nodes[j + 21 * q->band_index];
        n1 = g_nodes[j + 21 * q->band_index + 1];
        n2 = g_nodes[j + 21 * q->band_index + 2];
        p = q->noise_buffer + 256 * j;
        for (i = 0; i + n0 < n1; i++, p++)
            p[0] = (float)i / (float)(n1 - n0);
        diff = n2 - n1;
        p = q->noise_buffer + (j << 8) + n1 - n0;
        for (i = n1; i < n2; i++, p++, diff--)
            p[0] = (float)diff / (float)(n2 - n1);
    }
}

xf_qdmc *xf_qdmc_open(const u8 *desc, u32 desc_size)
{
    xf_qdmc *q;
    u32 i, size, fft_size;
    int fft_order, g, j, x;
    const u8 *p = NULL;

    /* The 'wave' extension holds 'frma' 'QDMC', then the 'QDCA' atom. */
    for (i = 0; i + 8 <= desc_size; i++)
        if (memcmp(desc + i, "frmaQDMC", 8) == 0) {
            p = desc + i + 8;
            break;
        }
    if (!p || (u32)(desc + desc_size - p) < 36)
        return NULL;
    size = xf_be32(p);
    if (size > (u32)(desc + desc_size - p) || memcmp(p + 4, "QDCA", 4) != 0)
        return NULL;
    init_tables();
    if (!(q = calloc(1, sizeof(*q))))
        return NULL;
    q->channels = (int)xf_be32(p + 12);
    q->sample_rate = (int)xf_be32(p + 16);
    q->bit_rate = (int)xf_be32(p + 20);
    fft_size = xf_be32(p + 28);
    q->packet_bytes = xf_be32(p + 32);
    for (fft_order = 0; (1u << fft_order) <= fft_size && fft_order < 31; fft_order++)
        ;
    if (q->channels < 1 || q->channels > 2 || fft_order < 7 || fft_order > 9 || fft_size != 1u << (fft_order - 1) ||
        q->packet_bytes < 8 || q->packet_bytes >= 1u << 28) {
        free(q);
        return NULL;
    }
    if (q->sample_rate >= 32000) {
        x = 28000;
        q->frame_bits = 13;
    } else if (q->sample_rate >= 16000) {
        x = 20000;
        q->frame_bits = 12;
    } else {
        x = 16000;
        q->frame_bits = 11;
    }
    q->frame_size = 1 << q->frame_bits;
    q->subframe_size = q->frame_size >> 5;
    q->fft_n = 1 << fft_order;
    if (q->fft_n != 2 * q->subframe_size) {
        free(q);
        return NULL;
    }
    if (q->channels == 2)
        x = 3 * x / 2;
    {
        long sel = (long)floor(q->bit_rate * 3.0 / (double)x + 0.5);
        q->band_index = g_noise_bands_selector[XF_MIN(6, XF_MAX(0, sel))];
    }
    for (g = 5; g > 0; g--)
        for (j = 0; j < (1 << g) - 1; j++)
            q->alt_sin[5 - g][j] = g_sin[((j + 1) << (8 - g)) & 0x1FF];
    for (j = 0; j < q->fft_n / 2; j++) {
        double a = 2.0 * M_PI * j / q->fft_n;
        q->tw_re[j] = (float)cos(a);
        q->tw_im[j] = (float)sin(a); /* inverse transform: e^(+i) */
    }
    make_noises(q);
    return q;
}

void xf_qdmc_close(xf_qdmc *q) { free(q); }

int xf_qdmc_channels(const xf_qdmc *q) { return q->channels; }

u32 xf_qdmc_frame_samples(const xf_qdmc *q) { return (u32)q->frame_size; }

u32 xf_qdmc_packet_bytes(const xf_qdmc *q) { return q->packet_bytes; }

void xf_qdmc_reset(xf_qdmc *q)
{
    memset(q->buffer, 0, sizeof(q->buffer));
    memset(q->fft_buffer, 0, sizeof(q->fft_buffer));
    q->fft_offset = 0;
    q->buffer_offset = 0;
}

/* --- Packet ---------------------------------------------------------- */

/* "QMC\1", then a 16-bit sum of the packet's bytes from offset 6. */
static int check_label(const xf_qdmc *q, bits *b, const u8 *packet)
{
    u32 label = get_bits(b, 32);
    u16 sum = 226, checksum = (u16)get_bits(b, 16);
    u32 i;
    if (label != (u32)('Q' | 'M' << 8 | 'C' << 16 | 1u << 24))
        return -1;
    for (i = 6; i < q->packet_bytes; i++)
        sum = (u16)(sum + packet[i]);
    return sum == checksum ? 0 : -1;
}

static int read_noise_data(xf_qdmc *q, bits *b)
{
    int ch, j, k, v, idx, band, lastval, newval, len;
    for (ch = 0; ch < q->channels; ch++)
        for (band = 0; band < g_noise_bands_size[q->band_index]; band++) {
            if ((v = get_vlc(b, VLC_NOISE, 0)) < 0)
                return -1;
            v = v & 1 ? v + 1 : -v;
            lastval = v / 2;
            q->noise[ch][band][0] = (u8)(lastval - 1);
            for (j = 0; j < 15;) {
                if ((len = get_vlc(b, VLC_SEGMENT, 1)) < 0)
                    return -1;
                len += 1;
                if ((v = get_vlc(b, VLC_NOISE, 0)) < 0)
                    return -1;
                newval = v & 1 ? lastval + (v + 1) / 2 : lastval - v / 2;
                idx = j + 1;
                if (len + idx > 16)
                    return -1;
                for (k = 1; idx <= j + len; k++, idx++)
                    q->noise[ch][band][idx] = (u8)(lastval + k * (newval - lastval) / len - 1);
                lastval = newval;
                j += len;
            }
        }
    return 0;
}

static void add_tone(xf_qdmc *q, int group, int offset, int freq, int mode, int amplitude, int phase)
{
    tone *t;
    if (q->nb_tones[group] >= MAX_TONES)
        return;
    t = &q->tones[group][q->nb_tones[group]++];
    t->offset = (u8)offset;
    t->freq = (i16)freq;
    t->mode = (u8)mode;
    t->amplitude = (i16)amplitude;
    t->phase = (u8)phase;
}

static int read_wave_data(xf_qdmc *q, bits *b)
{
    int amp, phase, mode = 0, i, group, freq, group_size, group_bits, amp2 = 0, phase2 = 0, pos2, off;
    for (group = 0; group < 5; group++) {
        group_size = 1 << (q->frame_bits - group - 1);
        group_bits = 4 - group;
        pos2 = 0;
        off = 0;
        for (i = 1;; i = freq + 1) {
            int v = get_vlc(b, VLC_FREQ, 1);
            if (v < 0)
                return -1;
            freq = i + v;
            while (freq >= group_size - 1) {
                freq += 2 - group_size;
                pos2 += group_size;
                off += 1 << group_bits;
            }
            if (pos2 >= q->frame_size)
                break;
            if (q->channels > 1)
                mode = (int)get_bits(b, 2);
            if ((amp = get_vlc(b, VLC_AMP, 0)) < 0)
                return -1;
            phase = (int)get_bits(b, 3);
            if (mode > 1) {
                if ((amp2 = get_vlc(b, VLC_AMP_DIFF, 0)) < 0)
                    return -1;
                amp2 = amp - amp2;
                if ((phase2 = get_vlc(b, VLC_PHASE_DIFF, 0)) < 0)
                    return -1;
                phase2 = phase - phase2;
                if (phase2 < 0)
                    phase2 += 8;
            }
            if ((freq >> group_bits) + 1 < q->subframe_size) {
                add_tone(q, group, off, freq, mode & 1, amp, phase);
                if (mode > 1)
                    add_tone(q, group, off, freq, ~mode & 1, amp2, phase2);
            }
        }
    }
    return 0;
}

static void lin_calc(xf_qdmc *q, float amplitude, int node1, int node2, int index)
{
    int sub = XF_MIN(q->subframe_size, node2), i, j, k, length = (sub - node1) & 0xFFFC;
    float scale = 0.5f * amplitude;
    const float *np = &q->noise_buffer[256 * index];
    for (i = 0, j = node1; i < length; i += 4, j += 4, np += 4) {
        q->noise2_buffer[j] += scale * np[0];
        q->noise2_buffer[j + 1] += scale * np[1];
        q->noise2_buffer[j + 2] += scale * np[2];
        q->noise2_buffer[j + 3] += scale * np[3];
    }
    k = length + node1;
    np = q->noise_buffer + length + (index << 8);
    for (i = length; i < sub - node1; i++, k++, np++)
        q->noise2_buffer[k] += scale * np[0];
}

static void add_noise(xf_qdmc *q, int ch, int subframe)
{
    int i, j, a;
    float *im = &q->fft_buffer[ch][q->fft_offset + q->subframe_size * subframe];
    float *re = &q->fft_buffer[2 + ch][q->fft_offset + q->subframe_size * subframe];
    memset(q->noise2_buffer, 0, 4 * (size_t)q->subframe_size);
    for (i = 0; i < g_noise_bands_size[q->band_index]; i++) {
        if (g_nodes[i + 21 * q->band_index] > q->subframe_size - 1)
            break;
        a = q->noise[ch][i][subframe / 2]; /* unsigned: a level under 0 picks a high entry */
        lin_calc(q, a > 0 ? g_amplitude[a & 0x3F] : 0.0f, g_nodes[21 * q->band_index + i],
                 g_nodes[21 * q->band_index + i + 2], i);
    }
    for (j = 2; j < q->subframe_size - 1; j++) {
        float rnd_re, rnd_im;
        q->rndval = 214013u * q->rndval + 2531011u;
        rnd_im = ((float)(q->rndval & 0x7FFF) - 16384.0f) * 0.000030517578f * q->noise2_buffer[j];
        q->rndval = 214013u * q->rndval + 2531011u;
        rnd_re = ((float)(q->rndval & 0x7FFF) - 16384.0f) * 0.000030517578f * q->noise2_buffer[j];
        im[j] += rnd_im;
        re[j] += rnd_re;
        im[j + 1] -= rnd_im;
        re[j + 1] -= rnd_re;
    }
}

static void add_wave(xf_qdmc *q, int offset, int freqs, int group, int mode, int amp, int phase)
{
    int j, group_bits = 4 - group, pos = freqs >> (4 - group), pindex;
    float amplitude = g_amplitude[amp & 0x3F], level, im, re, *imp, *rep;
    if (q->channels == 1)
        mode = 0;
    imp = &q->fft_buffer[mode][q->fft_offset + q->subframe_size * offset + pos];
    rep = &q->fft_buffer[2 + mode][q->fft_offset + q->subframe_size * offset + pos];
    pindex = (phase << 6) - ((2 * (freqs >> (4 - group)) + 1) << 7);
    for (j = 0; j < (1 << (group_bits + 1)) - 1; j++) {
        pindex += (2 * freqs + 1) << (7 - group_bits);
        level = amplitude * q->alt_sin[group][j];
        im = level * g_sin[pindex & 0x1FF];
        re = level * g_sin[(pindex + 128) & 0x1FF];
        imp[0] += im;
        imp[1] -= im;
        rep[0] += re;
        rep[1] -= re;
        imp += q->subframe_size;
        rep += q->subframe_size;
        if (imp >= &q->fft_buffer[mode][2 * q->frame_size]) {
            imp = &q->fft_buffer[mode][pos];
            rep = &q->fft_buffer[2 + mode][pos];
        }
    }
}

static void add_wave0(xf_qdmc *q, int offset, int freqs, int mode, int amp, int phase)
{
    float level = g_amplitude[amp & 0x3F];
    float im = level * g_sin[(phase << 6) & 0x1FF], re = level * g_sin[((phase << 6) + 128) & 0x1FF];
    int pos = q->fft_offset + freqs + q->subframe_size * offset;
    if (q->channels == 1)
        mode = 0;
    q->fft_buffer[mode][pos] += im;
    q->fft_buffer[2 + mode][pos] += re;
    q->fft_buffer[mode][pos + 1] -= im;
    q->fft_buffer[2 + mode][pos + 1] -= re;
}

static void add_waves(xf_qdmc *q, int subframe)
{
    int w, g;
    for (g = 0; g < 4; g++) {
        for (w = q->cur_tone[g]; w < q->nb_tones[g]; w++) {
            const tone *t = &q->tones[g][w];
            if (subframe < t->offset)
                break;
            add_wave(q, t->offset, t->freq, g, t->mode, t->amplitude, t->phase);
        }
        q->cur_tone[g] = w;
    }
    for (w = q->cur_tone[4]; w < q->nb_tones[4]; w++) {
        const tone *t = &q->tones[4][w];
        if (subframe < t->offset)
            break;
        add_wave0(q, t->offset, t->freq, t->mode, t->amplitude, t->phase);
    }
    q->cur_tone[4] = w;
}

/* In-place inverse complex FFT, unscaled: x[k] = sum x[n] e^(2 pi i nk/N). */
static void inverse_fft(const xf_qdmc *q, float *re, float *im)
{
    int n = q->fft_n, i, j, len, k;
    for (i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i];
            re[i] = re[j];
            re[j] = t;
            t = im[i];
            im[i] = im[j];
            im[j] = t;
        }
    }
    for (len = 2; len <= n; len <<= 1) {
        int step = n / len;
        for (i = 0; i < n; i += len)
            for (k = 0; k < len / 2; k++) {
                float wr = q->tw_re[k * step], wi = q->tw_im[k * step];
                float *ar = &re[i + k], *ai = &im[i + k], *br = &re[i + k + len / 2], *bi = &im[i + k + len / 2];
                float tr = *br * wr - *bi * wi, ti = *br * wi + *bi * wr;
                *br = *ar - tr;
                *bi = *ai - ti;
                *ar += tr;
                *ai += ti;
            }
    }
}

static int decode_frame(xf_qdmc *q, bits *b, const u8 *packet, i16 *out)
{
    int ch, i, n, nc = q->channels, sub = q->subframe_size;
    float *bufp;
    if (check_label(q, b, packet) != 0)
        return -1;
    q->fft_offset = q->frame_size - q->fft_offset;
    bufp = &q->buffer[nc * q->buffer_offset];
    if (read_noise_data(q, b) != 0 || read_wave_data(q, b) != 0)
        return -1;
    for (n = 0; n < 32; n++) {
        float *r;
        for (ch = 0; ch < nc; ch++)
            add_noise(q, ch, n);
        add_waves(q, n);
        for (ch = 0; ch < nc; ch++) {
            for (i = 0; i < sub; i++) {
                q->in_re[ch][i] = q->fft_buffer[ch + 2][q->fft_offset + n * sub + i];
                q->in_im[ch][i] = q->fft_buffer[ch][q->fft_offset + n * sub + i];
                q->in_re[ch][sub + i] = 0;
                q->in_im[ch][sub + i] = 0;
            }
            inverse_fft(q, q->in_re[ch], q->in_im[ch]);
        }
        r = &bufp[nc * n * sub];
        for (i = 0; i < 2 * sub; i++)
            for (ch = 0; ch < nc; ch++)
                *r++ += q->in_re[ch][i];
        r = &bufp[n * sub * nc];
        for (i = 0; i < nc * sub; i++) {
            float v = r[i] < -32768.0f ? -32768.0f : r[i] > 32767.0f ? 32767.0f : r[i];
            out[i] = (i16)v;
        }
        out += sub * nc;
        for (ch = 0; ch < nc; ch++) {
            memset(q->fft_buffer[ch] + q->fft_offset + n * sub, 0, 4 * (size_t)sub);
            memset(q->fft_buffer[ch + 2] + q->fft_offset + n * sub, 0, 4 * (size_t)sub);
        }
        memset(q->buffer + nc * (n * sub + q->frame_size + q->buffer_offset), 0, 4 * (size_t)sub * nc);
    }
    q->buffer_offset += q->frame_size;
    if (q->buffer_offset >= BUFFER_SIZE - q->frame_size) {
        memcpy(q->buffer, &q->buffer[nc * q->buffer_offset], 4 * (size_t)q->frame_size * nc);
        q->buffer_offset = 0;
    }
    return 0;
}

int xf_qdmc_decode(xf_qdmc *q, const u8 *packet, u32 size, i16 *out)
{
    bits b;
    if (size < q->packet_bytes) {
        memset(out, 0, (size_t)q->frame_size * q->channels * sizeof(i16));
        return -1;
    }
    b.p = packet;
    b.size = q->packet_bytes * 8;
    b.pos = 0;
    memset(q->nb_tones, 0, sizeof(q->nb_tones));
    memset(q->cur_tone, 0, sizeof(q->cur_tone));
    if (decode_frame(q, &b, packet, out) != 0) {
        memset(out, 0, (size_t)q->frame_size * q->channels * sizeof(i16));
        xf_qdmc_reset(q);
        return -1;
    }
    return q->frame_size;
}
