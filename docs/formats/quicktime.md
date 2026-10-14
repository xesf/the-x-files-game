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
| Audio | `QDMC` (QDesign Music 1) | 18 `.xmv`, 4 `.mus`, 1 `.amv`, 1 `.dmv` | Music and a few scenes. Not QDesign Music 2 (`QDM2`). |
| Text | `text` | Most `XV/` scenes | Subtitles. A scene can have several text tracks. |

Scenes in `XV/` have one video track, one sound track and one or more text
tracks. The text tracks whose sample description has no codec name are
also subtitle tracks.

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
