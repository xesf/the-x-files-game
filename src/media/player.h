/* Plays a QuickTime movie: decodes video to the current time and feeds
 * the sound to the mixer. The sound drives the clock when there is any. */
#ifndef XF_PLAYER_H
#define XF_PLAYER_H

#include "media/audio.h"
#include "media/mov.h"
#include "media/subtitle.h"
#include "media/video.h"

typedef struct {
    xf_mov mov;
    xf_video video;
    int has_video;
    xf_audio_stream audio;
    int has_audio;
    int channel;        /* mixer channel, or -1 */
    u32 audio_skip;     /* frames to drop before the edit list start */
    int audio_done;
    i16 *pcm;
    xf_subtitles subs;
    u32 duration_ms;
    u32 start_ticks;    /* wall clock at start, for movies without sound */
    u32 now_ms;
    int finished;
} xf_player;

int xf_player_open(xf_player *p, const char *path, u32 now_ticks);
void xf_player_close(xf_player *p);
/* Advances to wall-clock time `now_ticks` (ms). Returns 1 while playing. */
int xf_player_update(xf_player *p, u32 now_ticks);
/* Current picture, or NULL for a movie without video. */
const u32 *xf_player_frame(const xf_player *p);
/* Subtitle line to show now, or NULL. */
const char *xf_player_subtitle(const xf_player *p);

#endif
