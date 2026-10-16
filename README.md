# X-Files Game engine reimplementation

A portable C99 reimplementation of the VirtualCinema engine for
*The X-Files Game* (HyperBole Studios / Fox Interactive, 1998). It has no
third-party dependencies. You need your own copy of the original game
data: this project ships none of it.

Status: work in progress. On macOS the story runs from a new game
through the end of Disc 2 (checked by the story scripts in
`tests/play`); Disc 3 onwards is in progress, and some screens are not
drawn like the original yet. See
[docs/PLAN.md](docs/PLAN.md) and [docs/TESTING.md](docs/TESTING.md).

## Running a release

Each GitHub release has a package for macOS, Linux (desktop, Steam Deck
and Steam Machine), Windows, Windows 9x, DOS and the PlayStation Vita.

1. Download the package for your system and unpack it.
2. Copy the files of your installed game into a folder named `game`
   beside the program (beside `X-Files.app` on macOS), or set
   `XF_GAME_DIR` to your game folder. If your install keeps its video
   clips in another folder (the DVD edition), set `XF_MEDIA_DIR` to that
   folder too, or copy those clips into a folder named `media` inside the
   game folder. On the Vita, copy the game files to `ux0:data/xfiles`. The
   DVD edition's `vob` folder plays too: keep it inside the game folder.
   Under DOS, pass the folder: `XFILES --explore C:\XFILES 0 1`.
3. Start the program. The macOS app is not signed: the first time, open
   it with Control-click > Open.

The game keeps its original 4:3 screen. On wide screens, Ctrl+Shift+W
tries a layout where the scene fills the screen, with the interface
around it. See
[docs/engine/screen.md](docs/engine/screen.md).

The game plays with the mouse or touch, and also with the keyboard or a
d-pad:

| Keyboard | Vita | What it does |
|---|---|---|
| Arrow keys | D-pad | Move to the nearest thing that takes a click in that direction |
| Tab, Shift+Tab | R, L | The next or previous one |
| Enter, Space | Cross, Square | Click |
| I | Triangle | Show the inventory |
| Backspace | Circle | Put the item in hand away, stop talking, or go back |
| Escape | Start | The main menu; in it, Return goes back to the game |
| Mouse | Either stick, touch | Move the pointer |
| Typing | System keyboard | Opens when you click a text field, such as a login |

The DOS, Windows 9x and Vita builds are new and have only been tried in
DOSBox-X, Wine and the VitaSDK build.

To build from source, see [docs/BUILDING.md](docs/BUILDING.md). What has
changed is in [CHANGELOG.md](CHANGELOG.md).

## License

The engine and its tools are free software under the GNU General Public
License, version 3 or (at your option) any later version
(GPL-3.0-or-later). See `LICENSE`. Each source file carries an SPDX
identifier.

The license covers this project's code and documentation only. It does
not cover *The X-Files Game*, its data, video, music, text or fonts,
which belong to their owners. Do not add game files, the original
executable, or databases made from it to this repository or to a
release.

## Credits and references

This project builds on research by the following projects. Their
licenses allow this use, and their notices are kept here:

- **ScummVM** (GPL-3.0-or-later): QuickTime container and codec
  references. https://github.com/scummvm/scummvm
- **FFmpeg** (LGPL-2.1-or-later): the QDesign Music 1 (`QDMC`) decoder in
  `src/media/qdmc.c` follows FFmpeg's `libavcodec/qdmc.c`, Copyright (c)
  2017 Paul B Mahol; its tables come from there. The MPEG-2 decoder in
  `src/media/mpeg2.c` takes its code tables from FFmpeg's
  `libavcodec/mpeg12data.c`, Copyright (c) 2000, 2001 Fabrice Bellard
  and (c) 2002-2004 Michael Niedermayer. https://ffmpeg.org
- **libvita2d** (MIT): the Vita's texture shaders in
  `src/platform/vita/vita_gpu.c` are libvita2d's, compiled, and the
  sceGxm set-up follows its order. Copyright (c) 2015 Sergi Granell
  (xerpi). https://github.com/xerpi/libvita2d
- **mthcore/x-files-game** (MIT): NeoPersist HDB/GAM format research.
  https://github.com/mthcore/x-files-game
- **Zeffuro/x-files-pc-enhancement-patch** (MIT): a QuickTime and
  DirectDraw wrapper, used to run the original game for comparison.
  https://github.com/Zeffuro/x-files-pc-enhancement-patch
- **Ecstatica engine** (GPL-3.0-or-later, same author): the DOS and Vita
  platform layers are adapted from its backends.
- **Open Watcom** (Sybase Open Watcom Public License): the DOS and
  Windows 9x compiler. The DOS package includes `DOS4GW.EXE`, the DOS
  extender that Open Watcom distributes for programs built with it.
- **VitaSDK**: the PlayStation Vita toolchain.
- **ScummVM wiki, The X-Files Game page:** file format notes.
  https://wiki.scummvm.org/index.php?title=The_X-Files_Game

The MIT-licensed projects require this notice when their code is reused:

> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions: The above copyright
> notice and this permission notice shall be included in all copies or
> substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS",
> WITHOUT WARRANTY OF ANY KIND.

## Trademarks

The X-Files is a trademark of 20th Century Studios. *The X-Files Game*
was developed by HyperBole Studios and published by Fox Interactive.
This project is not affiliated with or endorsed by any of them.
