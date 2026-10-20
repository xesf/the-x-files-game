# QuickTime movies

All video, music, ambient sound and navigation files are QuickTime movies.
Only the file extension tells them apart:

| Extension | Folder | Content |
|---|---|---|
| `.xmv` | `XV/` | Full-motion video scenes, with sound and subtitle tracks |
| `.xmv` | `XN/` | Navigation clips: walking and turning between viewpoints |
| `.xmv` | `XG/` | Small animations: icons, cursors and interface effects |
| `.nmv` | install folder | Navigation stills: one still image per viewpoint (see below) |
| `.mus` | `XS/` | Music |
| `.amv` | `XS/` | Ambient sound and sound effects |
| `.dmv` | `XS/` | Device sounds: phone, computer, doors |
| `.mov` | `XV/` | One leftover movie, same content as `.xmv` |

The files follow the classic QuickTime file format. All numbers are
big-endian. The movie header (`moov`) comes before the media data
(`mdat`) in every file we checked.

## Codecs

Counts are tracks in the English CD release (1,198 files).

| Track | Codec | Where | Notes |
|---|---|---|---|
| Video | `cvid` (Cinepak) | Most scenes in `XV/`, some in `XN/` and `XG/` | 600×240 for scenes. A few are 600×400, 560×400 or other sizes. |
| Video | `jpeg` (Photo JPEG) | Navigation (`XN/`, `.nmv`), some `XG/` | Every frame is a key frame. |
| Video | `rpza` (Apple Video, "Road Pizza") | `XG/` | 16-bit, small icons (80×60). |
| Video | `mjpa` (Motion JPEG A) | One file in `XG/` | Interlaced: two JPEG fields per frame (see below). |
| Audio | `ima4` (IMA ADPCM, Apple variant) | Most sound | 11025, 22050 or 44100 Hz; mono or stereo. |
| Audio | `twos` (signed PCM, big-endian) | Some `.amv`, `.mus`, `.xmv` | 8-bit or 16-bit. |
| Audio | `QDMC` (QDesign Music 1) | 18 `.xmv`, 4 `.mus`, 1 `.amv`, 1 `.dmv` | Music and a few scenes: the credits music, the establishing shots, the songs on Willmore's stereo. Not QDesign Music 2 (`QDM2`). See below. |
| Video | `smc ` (Apple Graphics) | Roland (16 clips) | 8-bit, the Macintosh system palette. See below. |
| Audio | `raw ` and codec 0 (8-bit unsigned PCM) | Quantum Gate, Vortex, Roland | Codec 0 is how QuickTime 1.x wrote it. |
| Text | `text` | Most `XV/` scenes | Subtitles. A scene can have several text tracks. |

Scenes in `XV/` have one video track, one sound track and one or more text
tracks. The text tracks whose sample description has no codec name are
also subtitle tracks.

## Apple Graphics (`smc `)

Roland's 16 `smc ` clips are 8-bit. Their sample descriptions say
colour table ID -1, so the colours are the Macintosh system palette
(`clut` 8): a 6×6×6 cube from white down at 0 to 214, ramps of red,
green, blue and grey at 215 to 254 (`EE DD BB AA 88 77 55 44 22 11`),
and black at 255. A description with colour table ID 0 keeps its own
table after the fixed fields.

A frame starts with a flag byte and a 24-bit size. Opcodes follow over
the 4×4 blocks in raster order. The high nibble is the operation and
the low one a count minus 1; the operations 0x10, 0x30, 0x50 and 0x70
take the count minus 1 from the next byte instead.

| Opcode | Operation |
|---|---|
| `0x00`, `0x10` | Skip blocks (they keep the previous frame's pixels) |
| `0x20`, `0x30` | Repeat the previous block |
| `0x40`, `0x50` | Repeat the previous two blocks |
| `0x60`, `0x70` | Fill with one colour index |
| `0x80`, `0x90` | Two colours: two new indices (added to a cache) or a cache index; then 16 bits a block |
| `0xA0`, `0xB0` | Four colours, likewise; 2 bits a pixel |
| `0xC0`, `0xD0` | Eight colours, likewise; 3 bits a pixel in 6 bytes a block |
| `0xE0` | Sixteen indices given in full |

The bits go from the most significant, a row at a time from the top.
Each cache holds 256 entries and is filled from its start again with
every frame. An eight-colour block's top two rows use the high 12 bits
of its first two 16-bit words; the bottom two use the high 12 bits of
the third word, then the low nibbles of bytes 1, 3 and 5.

## QDesign Music 1 (`QDMC`)

The sound sample description (version 1) ends with a `wave` atom: `frma`
`QDMC`, then a `QDCA` atom of big-endian 32-bit fields:

| Field | Example (`FOCredit.mus`) |
|---|---|
| Version | 1 |
| Channels | 2 |
| Sample rate | 44100 |
| Bit rate | 48000 |
| Block size | 8192 |
| FFT size | 256 |
| Packet size | 1114 bytes |

A packet decodes to 8192 samples per channel at 32 kHz and above (4096
from 16 kHz, 2048 below). It starts with `QMC\1` and a 16-bit sum of its
bytes from offset 6, plus 226. The bits that follow, read least
significant first, give noise levels per band and the tones of five
groups; the decoder builds 32 sub-frames from them, each through an
inverse FFT, overlapped by half. `src/media/qdmc.c` follows FFmpeg's
decoder (see `README.md`).

## Navigation stills (`.nmv`)

Each `NAV*.NMV` file is a library of still JPEG images of 600×240, one per
viewpoint. `NAV1.NMV` has two video tracks:

| Track | Time scale | Samples | Sample duration |
|---|---|---|---|
| 1 | 1 | 166 | 1 (one second) |
| 2 | 600 | 15 | 600 (one second) |

Both tracks have edit lists (`elst`) that interleave them: the movie
timeline switches between track 1 and track 2 every few seconds. Together
the two tracks give 181 one-second images. How the game picks an image
(by movie time or by sample number) is not yet confirmed from the engine
code.

A track can have more than one sample description, and the chunk table
(`stsc`) chooses one per chunk. Track 2 of `NAV2.NMV` has four
descriptions: `jpeg`, `cvid`, `jpeg`, `jpeg`. Its third sample is a
Cinepak frame, while the samples around it are JPEG. A decoder must look
up the codec for each sample. (ffmpeg uses only the first description and
fails on that sample.)

## Unusual files

- `XN/25949.xmv` stores its movie header compressed. The `moov` atom holds
  a `cmov` atom with a `dcom` atom (method `zlib`) and a `cmvd` atom
  (4-byte uncompressed size, then the zlib data). The uncompressed data is a
  complete `moov` atom.
- `XG/94122.xmv` and `XG/94215.xmv` hold only an `mdat` atom of size 0 with
  JPEG data, and no movie header. On the Mac the header probably came from
  the resource fork. The game may not use these two files.

## Motion JPEG A

`XG/54200.xmv` (340×140) stores each frame as two JPEG images of 340×70,
one per field. The first JPEG starts with an `APP1` segment tagged `mjpg`:

| Offset in segment data | Size | Field |
|---|---|---|
| 0 | 4 | Reserved, 0 |
| 4 | 4 | Tag `mjpg` |
| 8 | 4 | Field size |
| 12 | 4 | Padded field size |
| 16 | 4 | Offset to the next field, from the start of the frame |

The first field gives the even lines (0, 2, 4, …), the second the odd
lines.

## IMA ADPCM in QuickTime

`ima4` sound is stored in packets of 34 bytes per channel. Each packet
holds 64 samples:

| Offset | Size | Content |
|---|---|---|
| 0 | 2 | Header: predictor in the top 9 bits, step index in the low 7 bits |
| 2 | 32 | 64 four-bit codes, low nibble first |

- Stereo packets alternate: one left packet, then one right packet.
- The sample table counts PCM frames, so one packet is 64 samples in
  `stsz`. A chunk of `n` samples holds `n / 64 × 34 × channels` bytes.
- Each code adds or subtracts a difference built from shifted step sizes:
  `step / 8`, plus `step` for bit 2, `step / 2` for bit 1 and `step / 4`
  for bit 0. Bit 3 is the sign.
- A decoder keeps its running predictor when the packet header has the
  same step index and a predictor within 127 of it. This avoids a small
  jump at each packet boundary.

## Edit lists

Scene sound tracks often start a little after the start of their media.
In `XV/19672.xmv` the video edit starts at media time 0 and the sound
edit at media time 48, so the first 48 sound frames are not played. The
reader stores the edit list of each track, and playback starts at the
media time of the first edit that is not empty.

## Subtitles

Each text sample is a 2-byte big-endian length followed by the text. The
text uses the Mac Roman character set (for example `0xD5` is a right
single quote), unlike the XTX documents, which use Windows-1252. Bytes
after the text (style atoms) are ignored.

The text tracks have one or two samples each, and their edit lists put
the lines on the movie timeline. An edit with media time −1 is a pause
with no text. For example, `XV/19650.xmv` has three text tracks:

| Track | Edits (in 1/600 s) | Shown |
|---|---|---|
| 3 | one edit starting at media time 300 | nothing: its only sample is shorter than 300 |
| 4 | pause 6416, sample 1 for 1204, pause 210, sample 2 for 2270 | "Yeah. It's him." at 10.69 s, "Thank you." at 13.05 s |
| 5 | pause 5550, sample 1 for 2110 | "Yeah. It's him." at 9.25 s |

Tracks 4 and 5 both contain the first line at different times, and all
text tracks are enabled with the same alternate group. Which track the
game shows is not yet known from the engine code. The reader keeps all
lines and tags each one with its track ID.

```sh
xftool subs XV/19650.xmv   # print all lines as SRT
```

## Sample tables

The reader in `src/media/mov.c` builds two indexes for each track:

- For video and text tracks, one offset and one start time per sample.
- For audio tracks, only per-chunk tables. An `ima4` music track has more than two million samples (one per
  PCM frame), so a per-sample index would waste memory.

## Tools

```sh
xftool mov XV/19672.xmv NAV1.NMV XS/30401.mus
```
