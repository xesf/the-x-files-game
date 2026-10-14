# Text and font files

## XTX text

`XT/*.xtx` files hold the documents the player reads in the game: e-mails,
files on computers, phone messages and journal pages. Each file is plain
text:

- Encoding is Windows-1252. The English release uses curly quotes
  (`0x91` to `0x94`) and `é` (`0xE9`).
- Lines end with CR LF.
- There are no control codes or markup.

The English CD release has 467 XTX files. The game data names each file by
number, like the other assets.

```sh
xftool xtx XT/49526.xtx    # print as UTF-8
```

## TTR fonts

The `.ttr` files in the install folder are standard TrueType fonts with a
different extension:

| File | Use (from the file name) |
|---|---|
| `DLG.TTR` | Dialogue and subtitles |
| `HCD.TTR` | Hand-written text |
| `JRN.TTR` | Journal |
| `PHN.TTR` | Phone display |

The engine will render them with its own TrueType rasterizer, so the game
does not need to install fonts on the system.
