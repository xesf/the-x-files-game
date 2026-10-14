/* QuickTime movie container: tracks and sample tables
 * (see docs/formats/quicktime.md). Decoding is done elsewhere. */
#ifndef XF_MOV_H
#define XF_MOV_H

#include "xf_file.h"

enum { XF_TRACK_VIDEO, XF_TRACK_AUDIO, XF_TRACK_TEXT, XF_TRACK_OTHER };

typedef struct {
    u32 count;
    u32 duration;
} xf_mov_tts; /* time-to-sample run */

typedef struct {
    u32 first_chunk; /* 1-based, as stored */
    u32 samples_per_chunk;
    u32 desc_index;
} xf_mov_stc; /* sample-to-chunk run */

typedef struct {
    int kind;         /* XF_TRACK_* */
    u32 id;
    u32 handler;      /* hdlr subtype, e.g. 'vide' */
    u32 timescale;    /* media time units per second */
    u32 duration;     /* in media time units */
    u32 codec;        /* fourcc of the first sample description */
    u8 *desc;         /* first sample description, raw (without size and fourcc) */
    u32 desc_size;
    /* video */
    u16 width, height, depth;
    /* audio */
    u16 channels, bits;
    u32 sample_rate;  /* Hz */
    u32 samples_per_packet, bytes_per_packet, bytes_per_frame; /* version 1 sound */
    /* sample tables */
    u32 sample_count;
    u32 sample_const_size; /* nonzero for constant-size samples (audio) */
    /* Per-sample tables; only built when sample_const_size is 0. */
    u32 *sample_size;
    u64 *sample_offset;
    u32 *sample_time;   /* start time in media units */
    u32 tts_count;
    xf_mov_tts *tts;
    u32 sync_count;     /* 0 means every sample is a sync sample */
    u32 *sync;          /* 1-based sample numbers */
    u32 chunk_count;
    u64 *chunk_offset;
    u32 stc_count;
    xf_mov_stc *stc;
    u32 *chunk_samples; /* samples in each chunk */
    u32 *chunk_first;   /* index of the first sample of each chunk */
} xf_mov_track;

typedef struct {
    xf_file *file;
    u32 timescale;
    u32 duration;
    u32 track_count;
    xf_mov_track *tracks;
} xf_mov;

int xf_mov_open(xf_mov *mov, const char *path);
void xf_mov_close(xf_mov *mov);
/* Returns the first track of `kind`, or NULL. */
xf_mov_track *xf_mov_find(xf_mov *mov, int kind);
/* Returns the index of the sample shown at `time` (media units). */
u32 xf_mov_sample_at(const xf_mov_track *t, u32 time);
int xf_mov_is_sync(const xf_mov_track *t, u32 sample);
/* Returns the chunk that holds `sample`. */
u32 xf_mov_chunk_at(const xf_mov_track *t, u32 sample);
/* Reads sample `index` into `dst`, which must hold sample_size[index].
 * Only for tracks with per-sample tables. */
int xf_mov_read_sample(xf_mov *mov, const xf_mov_track *t, u32 index, void *dst);

/* Writes the fourcc as text into `buf` (5 bytes). */
const char *xf_fourcc_str(u32 cc, char *buf);

#endif
