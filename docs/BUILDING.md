# Building

The engine is plain C99 with no third-party libraries. Each platform
backend uses only the APIs of that operating system.

## macOS

You need the Xcode command line tools (`xcode-select --install`).

```sh
make            # optimized build with debug symbols
make DEBUG=1    # no optimization, AddressSanitizer and UBSan
```

The build writes two programs to `build/`:

| Program | Purpose |
|---|---|
| `build/xfiles` | The game. |
| `build/xftool` | A command-line tool that lists, checks and extracts game files. |

## Linux, Steam Deck and Steam Machine

You need a C compiler, `make` and the X11 and ALSA headers (on Debian or
Ubuntu: `sudo apt install build-essential libx11-dev libasound2-dev`).

```sh
make
```

The backend (`src/platform/linux`) opens an X11 window, which also runs
under Wayland through XWayland, and plays sound through ALSA. On a Steam
Deck or a Steam Machine, add `build/xfiles` to Steam as a non-Steam
game.

## Windows

Cross-build from macOS or Linux with MinGW-w64:

```sh
make PLATFORM=windows CC=x86_64-w64-mingw32-gcc
```

The backend (`src/platform/windows`) draws with GDI and plays sound with
`waveOut`, using only calls that Windows 95 already had.

## Windows 9x and DOS

Both build with Open Watcom 2.0 (`export WATCOM=<install>` with its
`binl64` or `bino64` folder on `PATH`):

```sh
make win9x   # build/win9x/XFILES.EXE, the Win32 backend
make dos     # build/dos/XFILES.EXE and XFTOOL.EXE, DOS/4GW programs
```

The DOS program needs `DOS4GW.EXE` from Open Watcom beside it, VESA 2.0
with a 640x480 mode at 32, 16 or 15 bits per pixel and a linear
framebuffer, and a mouse driver. For sound it reads the Sound Blaster
settings from the `BLASTER` variable. Under DOS, run it with the game
folder: `XFILES --explore C:\XFILES 0 1`.

## PlayStation Vita

```sh
docker run --rm -v "$PWD:/src" -w /src vitasdk/vitasdk make vita
```

This builds `build/vita/xfiles.vpk` with VitaSDK. Copy the game files to
`ux0:data/xfiles`. The front touch panel is the pointer (a short tap
clicks, sliding only points); either stick
moves it, Cross and Circle are the mouse buttons and Start is Escape.
Clicking a text field, such as a login, opens the system keyboard; its
text replaces the field's. The Vita waits for the vertical blank, so frames never tear.
The Vita has no Courier New: copy `cour.ttf` into the game folder for
the location captions.

## Game data

The engine does not ship any game files. You need your own copy of
*The X-Files Game*. File names are matched without regard to case, so an
install with upper-case names (`XFILES.HDB`) and one with lower-case names
(`xfiles.hdb`) both work.

## Release packages

```sh
make dist                 # build/dist/xfiles-<version>-macos.zip
make dist VERSION=v0.1.0  # with an explicit version
```

`make dist` builds the engine for Apple silicon and Intel, merges the two
with `lipo`, and packs `X-Files.app`, `xftool`, `README.md` and `LICENSE`.
It never packs game files. Pushing a tag that starts with `v` makes the
`release` workflow build the same zip and publish it as a GitHub release.
The `build` workflow builds and runs the unit tests on every push.

Without arguments, the app looks for the game in `$XF_GAME_DIR`, then in
a folder named `game` beside the program or beside `X-Files.app`, then in
`./game`. `XF_MEDIA_DIR` names one more folder of clips.

