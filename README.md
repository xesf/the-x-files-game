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

1. Download `xfiles-<version>-macos.zip` from the GitHub releases and
   unzip it.
2. Copy the files of your installed game into a folder named `game`
   beside `X-Files.app`, or set `XF_GAME_DIR` to your game folder. If your
   install keeps its video clips in another folder (the DVD edition),
   set `XF_MEDIA_DIR` to that folder too.
3. Open `X-Files.app`. The app is not signed: the first time, open it
   with Control-click > Open.

To build from source, see [docs/BUILDING.md](docs/BUILDING.md).

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
- **mthcore/x-files-game** (MIT): NeoPersist HDB/GAM format research.
  https://github.com/mthcore/x-files-game
- **Zeffuro/x-files-pc-enhancement-patch** (MIT): a QuickTime and
  DirectDraw wrapper, used to run the original game for comparison.
  https://github.com/Zeffuro/x-files-pc-enhancement-patch
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
