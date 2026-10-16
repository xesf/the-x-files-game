# Text and font files

## XTX text

`XT/*.xtx` files hold the documents the player reads in the game: e-mails,
files on computers, phone messages and journal pages. Each file is plain
text:

- Encoding is Windows-1252. The English release uses curly quotes
  (`0x91` to `0x94`) and `é` (`0xE9`). The Japanese release uses
  Shift-JIS (code page 932), with full-width colons in mail headers.
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

In the English CD install the fonts are named `dlg.ttf`, `hcd.ttf` and
`jrn.ttf`, with only `phn.ttr` keeping the original extension. The engine
looks for both names, in each language folder before its fallback.

The four fonts are the same files in every release (English, French,
German, Italian, Spanish and Japanese): only the extension differs. The
Japanese release's fonts have no Japanese glyphs.

| File | Glyphs | Units per em | Character map |
|---|---|---|---|
| `DLG` | 247 | 1100 | Windows Unicode |
| `HCD` | 230 | 4096 | Macintosh Roman only |
| `JRN` | 221 | 4096 | Macintosh Roman only |
| `PHN` | 247 | 1271 | Windows Unicode |

The engine renders the fonts with its own TrueType rasterizer
(`src/gfx/ttf.c`), so the game does not install fonts on the system. For
fonts with only a Macintosh character map, the engine converts each
Unicode character to its Mac Roman byte before the lookup.

```sh
xftool font DLG.TTR 24 "The truth is out there" out.bmp
```
