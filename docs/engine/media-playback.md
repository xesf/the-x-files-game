# Media playback

How the reimplementation plays a QuickTime scene. The original game used
QuickTime for Windows for this; our engine has its own decoders (see
[../formats/quicktime.md](../formats/quicktime.md)).

## Parts

| Module | Role |
|---|---|
| `src/media/mov.c` | Reads tracks, sample tables and edit lists. |
| `src/media/video.c` | Decodes frames of a video track; picks the codec per sample. |
| `src/media/audio.c` | Decodes a sound track one chunk at a time. |
| `src/media/subtitle.c` | Collects subtitle lines and places them in movie time. |
| `src/audio/mixer.c` | Mixes up to eight sound channels to the output rate. |
| `src/media/player.c` | Ties the parts together for one movie. |

## Clock

When a movie has sound, the sound is the clock. The player asks the mixer
how many source frames it has played and converts that to milliseconds.
Video and subtitles follow that time. This keeps lips in sync even when a
frame takes long to decode, because the picture catches up instead of the
sound waiting.

A movie without sound uses the wall clock from the moment it started.

## Each update

1. Fill the mixer channel with decoded sound, one chunk at a time, while
   there is room for a whole chunk. Frames before the edit list start are
   dropped.
2. Compute the current time from the sound.
3. Find the video sample shown at that time and decode up to it. Cinepak
   frames depend on earlier frames, so decoding continues from the last
   frame or restarts at the previous key frame.
4. Look up the subtitle line for that time.

The movie ends when its duration has passed and the sound has drained.

## Trying it

```sh
build/xfiles --play /path/to/game/XV/19650.xmv
```

Any key closes the movie. Subtitles print to the console until the font
renderer is written.
