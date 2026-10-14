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
