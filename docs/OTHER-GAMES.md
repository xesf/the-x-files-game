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
The IDA we use does not handle 16-bit code, so these need another
disassembler that loads NE files with their segments and imports, such
as Ghidra. Roland's program is VB3 p-code, which a disassembler does
not decode; it needs a p-code reader written for the engine's tools.

### Earlier work on Roland

The `virtualcinema` branch of the ScummVM fork
(`~/Documents/workaround/github/scummvm`, last change 2025-05-03, all
commits by xesf) has a Virtual Cinema engine with a start on Roland
(`engines/virtualcinema/roland`, about 700 lines): the intro, the main
menu, the chapter menu and the chapter page screens, on a shared core
(`core/`: video, image, scene, engine). Its two stashes hold QuickTime
audio work. Being our own code, its Roland screens can be ported to C
here; ScummVM's own decoders it relies on are other people's code and
are not copied.

## Engine structure

`src/games/xfiles` is The X-Files' Virtual Cinema 3.0 runtime, with
its scene-graph reader and action interpreter in `vc/`. The shared layers (`src/platform`, `src/gfx`, `src/media`,
`src/audio`, `src/core`) serve any game. Every game lives in `src/games`:

| File | Content |
|---|---|
| `games.h`, `detect.c` | Which game a folder holds: `XFILES.HDB` (or a language folder with it), `VORTEX.EXE`, `ROLAND.EX_` or `ROLAND.EXE`, `QGATE.EXE`. `xftool detect` prints it. |
| `modules.c` | The module table: a name, a title and a `run(folder, options)` function per game. Each module owns its loop, as `xf_explore` does for The X-Files. |
| `stage.c` | What the modules share: the 640×480 screen, DOS paths (`c\splash\vclogo.mov`) found whatever their case, clips, pictures, sounds, music, the pointer, and a test script (`wait`, `point`, `press`, `key`, `shot`, `quit`). |
| `music.c` | Plays a MIDI song through `plat_midi_*`. |
| `xfiles/` | The X-Files: the scene graph and actions (`vc/`), the game logic, the window (`explore.c`), saves and play scripts |
| `vortex/`, `roland/`, `qgate/` | One module each. |

`xfiles --game <folder> [--script <file>]` runs the game a folder holds;
without arguments the program also finds an earlier game in the folders
it searches (`$XF_GAME_DIR`, `game` beside the program). The X-Files
keeps its own command-line options and runs as before.

## Order of the work

1. **Shared formats** (done): `raw ` PCM, SZDD, BMP with RLE8 and
   Microsoft Video 1, WAV, `smc `, MIDI with the platform synthesizers.
   Each has an `xftool` command, a unit test and a format page. Not
   read yet: one Intel Indeo picture and one QuickTime Animation (`rle `)
   clip in Quantum Gate, neither used by its program.
2. **Game detection and the module interface** (done).
3. **Vortex** (Virtual Cinema 2.0, in progress): `tools/ne` reads and
   disassembles `VORTEX.EXE`; its logic is a bytecode the program
   interprets (see `docs/vortex/README.md`), and `src/games/vortex`
   runs it. Every opcode the sequences hold is implemented, with the
   uber device (moods, the journal) and the screen effects. The three
   discs play from folders `VORTEX_1` to `VORTEX_3` (the disc images'
   contents), changing discs as the original asks. Left: playing every
   day through to its ends, MIDI on the platforms without a
   synthesizer, and the swirl effect (shown as a dissolve).
4. **Roland**: start from the ScummVM branch's screens (intro, menus,
   chapter pages); SZDD-expand `ROLAND.EX_` and `ROLGUTS.EX_`, write the
   VB3 p-code reader for what the screens do not cover yet, and write
   the module.
5. **Quantum Gate** (Virtual Cinema 1.0): the same as Vortex for
   `QGATE.EXE` and `VIDGAME.EXE`, reusing what Vortex's module and
   tools establish.

## Open questions

- Which disassembler to use for the 16-bit NE executables: our own
  (`tools/ne`, on Python's capstone) is enough for Vortex.
- Which MIDI path to take on Linux, the Vita and DOS (macOS and Windows
  use their own synthesizers).
- How far the earlier `hyperbole` project (C++, SDL, MIT) helps: it
  detects the games and reads BMP and RLE8, which this plan covers in C.
