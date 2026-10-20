/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
#include "music.h"

#include <string.h>

#include "platform/plat.h"

/* All notes off and controllers reset on the 16 channels. */
static void silence(void)
{
    int ch;
    for (ch = 0; ch < 16; ch++) {
        plat_midi_send((u8)(0xB0 | ch), 123, 0);
        plat_midi_send((u8)(0xB0 | ch), 121, 0);
    }
}

int xf_music_play(xf_music *m, const char *path, int plays, u32 now_ms)
{
    int volume = m->open || m->volume ? m->volume : 127;
    xf_music_stop(m);
    m->volume = volume ? volume : 127;
    if (!m->open) {
        if (plat_midi_open() != 0)
            return -1;
        m->open = 1;
    }
    if (xf_midi_load(path, &m->song) != 0)
        return -1;
    m->playing = 1;
    m->plays = plays;
    m->start_ms = now_ms;
    m->next = 0;
    return 0;
}

void xf_music_update(xf_music *m, u32 now_ms)
{
    u32 at;
    if (!m->playing)
        return;
    at = (now_ms - m->start_ms) * 1000u;
    while (m->next < m->song.count && m->song.events[m->next].us <= at) {
        const xf_midi_event *e = &m->song.events[m->next++];
        u8 d2 = e->d2;
        if ((e->status & 0xF0) == 0x90 && d2)
            d2 = (u8)XF_MAX(1, d2 * m->volume / 127);
        plat_midi_send(e->status, e->d1, d2);
    }
    if (m->next >= m->song.count && at >= m->song.length_us) {
        silence();
        if (m->plays != 1 && m->song.count) {
            if (m->plays > 1)
                m->plays--;
            m->start_ms = now_ms;
            m->next = 0;
        } else
            m->playing = 0;
    }
}

void xf_music_stop(xf_music *m)
{
    if (m->playing)
        silence();
    m->playing = 0;
    xf_midi_free(&m->song);
}

void xf_music_close(xf_music *m)
{
    xf_music_stop(m);
    if (m->open)
        plat_midi_close();
    memset(m, 0, sizeof(*m));
}
