#include "video.h"

#include <stdlib.h>
#include <string.h>

#include "media/cinepak.h"
#include "media/jpeg.h"
#include "media/rpza.h"

static int codec_supported(u32 codec)
{
    switch (codec) {
    case XF_FOURCC('c', 'v', 'i', 'd'):
    case XF_FOURCC('r', 'p', 'z', 'a'):
    case XF_FOURCC('j', 'p', 'e', 'g'):
    case XF_FOURCC('m', 'j', 'p', 'a'):
        return 1;
    }
    return 0;
}

int xf_video_supported(const xf_mov_track *t)
{
    u32 i;
    if (t->kind != XF_TRACK_VIDEO || !t->sample_size || !t->width || !t->height || !t->desc_count)
        return 0;
    for (i = 0; i < t->desc_count; i++)
        if (!codec_supported(t->descs[i].codec))
            return 0;
    return 1;
}

int xf_video_open(xf_video *v, xf_mov *mov, const xf_mov_track *t)
{
    memset(v, 0, sizeof(*v));
    if (!xf_video_supported(t))
        return -1;
    v->mov = mov;
    v->track = t;
    v->width = t->width;
    v->height = t->height;
    v->frame = calloc((size_t)v->width * v->height, 4);
    if (!v->frame)
        return -1;
    v->cinepak = xf_cinepak_create(v->width, v->height);
    if (!v->cinepak) {
        xf_video_close(v);
        return -1;
    }
    return 0;
}

void xf_video_close(xf_video *v)
{
    xf_cinepak_destroy(v->cinepak);
    free(v->frame);
    free(v->buf);
    memset(v, 0, sizeof(*v));
}

int xf_video_next(xf_video *v)
{
    const xf_mov_track *t = v->track;
    u32 i = v->next, size;
    int w = v->width, h = v->height;

    if (i >= t->sample_count)
        return -1;
    size = t->sample_size[i];
    if (size > v->buf_size) {
        u8 *b = realloc(v->buf, size);
        if (!b)
            return -1;
        v->buf = b;
        v->buf_size = size;
    }
    if (xf_mov_read_sample(v->mov, t, i, v->buf) != 0)
        return -1;
    v->next = i + 1;
    /* A track may switch codec between chunks, so look it up per sample. */
    switch (t->descs[t->sample_desc[i]].codec) {
    case XF_FOURCC('c', 'v', 'i', 'd'):
        return xf_cinepak_decode(v->cinepak, v->buf, size, v->frame);
    case XF_FOURCC('r', 'p', 'z', 'a'):
        return xf_rpza_decode(v->buf, size, v->frame, w, h);
    case XF_FOURCC('j', 'p', 'e', 'g'):
        return xf_jpeg_decode(v->buf, size, v->frame, w, h, w);
    case XF_FOURCC('m', 'j', 'p', 'a'):
        return xf_mjpa_decode(v->buf, size, v->frame, w, h, w);
    }
    return -1;
}

int xf_video_seek(xf_video *v, u32 index)
{
    u32 i = index;

    if (index >= v->track->sample_count)
        return -1;
    if (index != v->next) {
        while (i > 0 && !xf_mov_is_sync(v->track, i))
            i--;
        /* Continue from the current position if it is closer. */
        if (v->next > i && v->next <= index)
            i = v->next;
        v->next = i;
    }
    while (v->next <= index)
        if (xf_video_next(v) != 0)
            return -1;
    return 0;
}
