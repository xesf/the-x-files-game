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

