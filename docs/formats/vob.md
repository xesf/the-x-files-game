# DVD video (`.vob`)

The DVD edition keeps 12 clips as DVD video in a `vob` folder of the
install. Every other clip is a QuickTime movie, as on the CDs. The
reader is `src/media/vob.c`, with the MPEG-2 decoder in
`src/media/mpeg2.c`.

## The files

| File | Use | CD equivalent |
|---|---|---|
| `ddigital1.vob` | Opening title (72 s) | none |
| `teaser.vob` | Opening title (46 s) | `XV/19668.xmv` |
| `19650.vob`, `19652.vob`, `23412.vob`, `23440.vob` | End game scenes | `XV/<same number>.xmv` |
| `20383.vob`, `20385.vob` | Entering the warehouse | `XV/<same number>.xmv` |
| `38767.vob` | The gunman on the stairs | `XV/38767.xmv` |
| `70710.vob`, `70712.vob`, `71914.vob` | Link 1 close-ups | `XV/<same number>.xmv` |

The engine also asks for `intro.vob`, which neither DVD install has.

## How the original uses them

`media_object_create` (`0x517190`) takes the file name of a movie's
resolved path, without its extension, and tries
`<install>/vob/<name>.vob` through `dvd_play_title` (`0x4E5E30`). Only
when that fails does it open the QuickTime file. `dvd_play_title` loads
the MPEG player DLL named in `dlmpg.ini` (`dlmpgmci.dll`), hides the
cursor and plays the first 704 columns of the picture at 640×480. A
click ends the clip early.

At start-up, `play_intro_sequence` (`0x471820`) plays the Fox and
HyperBole logos. When `ddigital1` plays, it then plays `teaser` and
`intro` and skips the CD's teaser and title clips.

## Container

An MPEG-2 program stream in 2,048-byte packs. Each pack starts with a
pack header (`00 00 01 BA`, 14 bytes plus stuffing), then holds PES
packets:

| Stream ID | Content |
|---|---|
| `E0` | MPEG-2 video |
| `BD` | Private stream 1: substream `A0`, LPCM sound |
| `BF` | Private stream 2: DVD navigation packets |
| `BB`, `BE` | System header, padding |

Time stamps (PTS) count at 90 kHz. The sound starts at 0.347 s and the
pictures at 0.414 s, so the pictures start 67 ms after the sound.

LPCM payloads start with 7 bytes: the substream ID, a frame count, a
2-byte pointer to the first access unit, then a 3-byte header. Every
file is 16-bit stereo at 48 kHz. The samples are big-endian, left then
right.

## Video

Every file uses the same settings:

| Setting | Value |
|---|---|
| Profile | Main profile, 4:2:0 |
| Size | 720×480, 4:3, pixel aspect 8:9 |
| Rate | 29.97 frames per second, interlaced |
| Pictures | Frame pictures only, with field or frame prediction and field or frame DCT |
| Coding | Non-linear quantiser scale, intra VLC table B.15, alternate scan, 9-bit intra DC |

A few pictures of the teaser set `repeat_first_field`, so they last three
fields.

The scene clips letterbox the game's 600×240 picture at (30, 118), 660×240
in the 720-pixel frame. The DVD player's 704→640 scaling makes it
600×240, the same as the CD clips, so our engine shows that part of a
scene clip in the normal picture area. The opening titles use the
whole frame, scaled to 640×480. Our engine blends each row with its
neighbours to hide the interlacing.

## Checking

| Command | What it does |
|---|---|
| `xftool vob <file.vob> <ms> <out.bmp> [full]` | Saves the picture shown at a time, scene part or whole frame |
| `xftool vob-yuv <file.vob> <out.yuv> [pictures]` | Saves the decoded pictures as raw 4:2:0 |
| `xftool vob-audio <file.vob> <out.wav>` | Saves the sound |

Compared with FFmpeg on all 12 files, the decoder gives the same number
of pictures, each within 5 levels (61 dB PSNR or better). The difference
comes from the inverse DCT's rounding. The sound is identical.
