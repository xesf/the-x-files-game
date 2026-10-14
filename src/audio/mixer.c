#include "mixer.h"

#include <stdlib.h>
#include <string.h>

#define RING_FRAMES (1 << 16) /* about 3 seconds at 22050 Hz */

typedef struct {
    int used, finished;
    int rate, channels, volume;
    i16 *ring;      /* stereo frames */
    u32 read, write; /* frame positions, wrap at RING_FRAMES */
    u32 frac;       /* 16.16 position between read and read + 1 */
    u32 step;       /* 16.16 source frames per output frame */
    u64 played;
} channel;

static channel g_ch[XF_MIXER_CHANNELS];
static int g_out_rate = 22050;
static xf_lock_fn g_lock, g_unlock;

static void lock(void)
{
    if (g_lock)
        g_lock();
}

static void unlock(void)
{
    if (g_unlock)
        g_unlock();
}

void xf_mixer_init(int out_rate, xf_lock_fn lock_fn, xf_lock_fn unlock_fn)
{
    g_out_rate = out_rate;
    g_lock = lock_fn;
    g_unlock = unlock_fn;
}

static u32 filled(const channel *c) { return (c->write - c->read) & (RING_FRAMES - 1); }

int xf_mixer_open(int rate, int channels)
{
    int i;
    i16 *ring = malloc(RING_FRAMES * 2 * sizeof(i16));
    if (!ring || rate <= 0)
        return free(ring), -1;
    lock();
    for (i = 0; i < XF_MIXER_CHANNELS; i++)
        if (!g_ch[i].used) {
            channel *c = &g_ch[i];
            free(c->ring);
            memset(c, 0, sizeof(*c));
            c->used = 1;
            c->rate = rate;
            c->channels = channels;
            c->volume = 256;
            c->ring = ring;
            c->step = (u32)(((u64)rate << 16) / (u32)g_out_rate);
            unlock();
            return i;
        }
    unlock();
    free(ring);
    return -1;
}

void xf_mixer_close(int ch)
{
    i16 *ring;
    if (ch < 0 || ch >= XF_MIXER_CHANNELS)
        return;
    lock();
    ring = g_ch[ch].ring;
    memset(&g_ch[ch], 0, sizeof(g_ch[ch]));
    unlock();
    free(ring);
}

u32 xf_mixer_space(int ch)
{
    u32 n;
    lock();
    n = g_ch[ch].used ? RING_FRAMES - 1 - filled(&g_ch[ch]) : 0;
    unlock();
    return n;
}

u32 xf_mixer_write(int ch, const i16 *pcm, u32 frames)
{
    channel *c = &g_ch[ch];
    u32 i, n;

    lock();
    n = c->used ? XF_MIN(frames, RING_FRAMES - 1 - filled(c)) : 0;
    for (i = 0; i < n; i++) {
        u32 w = (c->write + i) & (RING_FRAMES - 1);
        if (c->channels == 2) {
            c->ring[w * 2] = pcm[i * 2];
            c->ring[w * 2 + 1] = pcm[i * 2 + 1];
        } else {
            c->ring[w * 2] = c->ring[w * 2 + 1] = pcm[i];
        }
    }
    c->write = (c->write + n) & (RING_FRAMES - 1);
    unlock();
    return n;
}

void xf_mixer_finish(int ch)
{
    lock();
    g_ch[ch].finished = 1;
    unlock();
}

u64 xf_mixer_played(int ch)
{
    u64 n;
    lock();
    n = g_ch[ch].played;
    unlock();
    return n;
}

int xf_mixer_active(int ch)
{
    int a;
    lock();
    a = g_ch[ch].used && !(g_ch[ch].finished && filled(&g_ch[ch]) < 2);
    unlock();
    return a;
}

void xf_mixer_set_volume(int ch, int volume)
{
    lock();
    g_ch[ch].volume = XF_CLAMP(volume, 0, 256);
    unlock();
}

void xf_mixer_render(void *user, i16 *out, int frames)
{
    int i, k;
    static i32 acc[4096 * 2];

    (void)user;
    while (frames > 0) {
        int n = XF_MIN(frames, 4096);
        memset(acc, 0, (size_t)n * 2 * sizeof(i32));
        for (k = 0; k < XF_MIXER_CHANNELS; k++) {
            channel *c = &g_ch[k];
            if (!c->used)
                continue;
            for (i = 0; i < n; i++) {
                u32 avail = filled(c), r0, r1;
                i32 f = (i32)(c->frac & 0xFFFF);
                if (avail < 2)
                    break; /* underrun: wait for more data */
                r0 = c->read;
                r1 = (r0 + 1) & (RING_FRAMES - 1);
                acc[i * 2] += ((c->ring[r0 * 2] * (65536 - f) + c->ring[r1 * 2] * f) >> 16) * c->volume >> 8;
                acc[i * 2 + 1] +=
                    ((c->ring[r0 * 2 + 1] * (65536 - f) + c->ring[r1 * 2 + 1] * f) >> 16) * c->volume >> 8;
                c->frac += c->step;
                c->read = (c->read + (c->frac >> 16)) & (RING_FRAMES - 1);
                c->played += c->frac >> 16;
                c->frac &= 0xFFFF;
            }
        }
        for (i = 0; i < n * 2; i++)
            out[i] = (i16)XF_CLAMP(acc[i], -32768, 32767);
        out += n * 2;
        frames -= n;
    }
}
