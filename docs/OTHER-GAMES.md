# Plan: the other HyperBole games

After The X-Files, the engine is to run HyperBole Studios' earlier
games too. This plan says what each game is made of, what the engine
has already, and the order of the work. It is a plan, not a record:
update it as the work goes.

## The games

| Game | Year | Technology | Program | Game logic |
|---|---|---|---|---|
| The Madness of Roland | 1992 | Visual Basic 3 | `ROLAND.EX_`, `ROLGUTS.EX_` (SZDD-compressed) | VB3 p-code in the executables |
| Quantum Gate | 1993 | Virtual Cinema 1.0 (C++) | `QGATE.EXE`, `VIDGAME.EXE`, `QGATE.DLL` (16-bit NE) | Compiled into the executable; media paths are strings in `QGATE.EXE` |
| Vortex: Quantum Gate II | 1994 | Virtual Cinema 2.0 (C++) | `VORTEX.EXE` (16-bit NE, 785 KB) | Compiled into the executable; no data file holds it |
| The X-Files Game | 1998 | Virtual Cinema 3.0 | `XFILES.EXE` (32-bit PE) | The HDB scene graph and the GAM variables |

Only The X-Files keeps its logic in data. For the three others, the
logic has to be recovered from the executables, so each needs its own
game module, not just new file formats.

## What the games contain

| | Quantum Gate | Vortex | Roland |
|---|---|---|---|
| Video | 689 `.MOV`, Cinepak (`cvid`), 320×240 | 358 `.MOV`, Cinepak, 240×180 | 152 `.MOV`, Apple Graphics (`smc `) |
| Sound in clips | 8-bit `raw ` PCM | 8-bit `raw ` PCM (`.AMV` too) | 8-bit `raw ` PCM |
| Music | 192 `.MID` | 240 `.MID` | none |
| Sound effects | 450 `.WAV` | 21 `.WAV` | in the clips |
| Pictures | 348 `.BMP`, 387 `.HSB` (SZDD-compressed) | 233 `.BMP` | 408 `.RLE` (BMP with RLE8) |
| Fonts | system | 6 `.TTR` (TrueType, as in The X-Files) | system |

Vortex already shares some of The X-Files' formats (`.AMV`, `.TTR`),
which fits Virtual Cinema 2.0 being The X-Files' predecessor.

## What the engine has already

- The QuickTime reader (`src/media/mov.c`) parses all three games'
  clips.
- Cinepak and Apple Video (`rpza`) decoders; Cinepak covers Quantum Gate
  and Vortex.
- The platform layer, the window, the audio mixer, the TrueType
  rasterizer, the language folders and the test harness.

## What the engine needs

| Need | For | Size |
|---|---|---|
| QuickTime `raw ` 8-bit unsigned PCM | All three | Small |
| SZDD (MS `COMPRESS.EXE`) expansion | Quantum Gate `.HSB`, Roland `.EX_` | Small |
| BMP, 8-bit and RLE8 | All three | Small |
| WAV | Quantum Gate, Vortex | Small |
| Apple Graphics (`smc `) decoder | Roland | Medium |
| MIDI music | Quantum Gate, Vortex | Large: needs a synthesizer, see below |
| A game module per title | Each | Large: the logic recovered from the executable |
| 16-bit NE executables in the disassembler | Quantum Gate, Vortex | Tooling, see below |
| A VB3 p-code reader | Roland | Large |

### MIDI

The engine links no libraries, so it needs its own way to play
General MIDI:

1. The platform's own synthesizer where there is one: macOS has a
   General MIDI synthesizer (DLS) and Windows has its MIDI mapper,
   both reachable from the platform layer without extra libraries.
2. For the other platforms (Linux, the Vita, DOS), a small software
   synthesizer in the engine, or embedded permissive-licence code with
   its credit, as done for FFmpeg's QDMC tables.

### Disassembly

`QGATE.EXE` and `VORTEX.EXE` are 16-bit Windows 3.1 (NE) executables.
Check first that the IDA we use loads NE files with their segments and
imports. Ghidra loads NE files too, if needed. Roland's program is VB3
p-code, which a disassembler does not decode; it needs a p-code reader
written for the engine's tools.

## Engine structure

Today `src/game` and `src/vc` are The X-Files' Virtual Cinema 3.0
runtime. The shared layers (`src/platform`, `src/gfx`, `src/media`,
`src/audio`, `src/core`) already serve any game. The plan:

1. Game detection: the folder's files say which game it is (`XFILES.EXE`
   and an HDB, `QGATE.EXE`, `VORTEX.EXE`, `ROLAND.EX_`), and `main`
   starts that game's module.
2. One module per game behind a small interface (start, frame, input,
   save, load), so the shared layers do not depend on any one game.
3. The X-Files' module stays as it is and moves behind that interface
   once its own work is done.

## Order of the work

1. **Shared formats** (can start any time): `raw ` PCM, SZDD, BMP and
   RLE8, WAV, `smc `. Each with an `xftool` command and a unit test,
   as for the X-Files formats.
2. **Game detection and the module interface.**
3. **Quantum Gate** (Virtual Cinema 1.0): load `QGATE.EXE` and
   `VIDGAME.EXE` in the disassembler; find the scene tables and the
   media paths; write the module; play the clips and the navigation.
   MIDI for its music.
4. **Vortex** (Virtual Cinema 2.0): the same for `VORTEX.EXE`. Its
   logic is entirely compiled in, so this is the longest step. What it
   shares with The X-Files may help: compare its classes and functions
   with `XFILES.EXE`'s.
5. **Roland**: SZDD-expand `ROLAND.EX_` and `ROLGUTS.EX_`, write the VB3
   p-code reader, recover the forms and the logic, and write the
   module.

## Open questions

- Whether the IDA we use loads 16-bit NE executables.
- Which MIDI path to take on each platform.
- Whether Vortex's executable keeps any class names or tables in common
  with The X-Files that make its logic faster to recover.
- How far the earlier `hyperbole` project (C++, SDL, MIT) helps: it
  detects the games and reads BMP and RLE8, which this plan covers in C.
