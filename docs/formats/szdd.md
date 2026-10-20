# SZDD packed files

The earlier HyperBole games ship some files packed with Microsoft's
`COMPRESS.EXE`. The packed name ends in `_` in place of its last
character (`ROLAND.EX_` for `ROLAND.EXE`). Quantum Gate's `.HSB`
pictures use the same packing under their own name.

| Game | Packed files |
|---|---|
| Quantum Gate | 387 `.HSB` pictures (a Windows DIB without the file header) |
| Roland | `ROLAND.EX_`, `ROLGUTS.EX_` and the Visual Basic controls |
| Vortex | Setup files |

All 426 packed files of the three games unpack.

## Layout

| Offset | Size | Content |
|---|---|---|
| 0 | 8 | `SZDD` `88 F0 27 33` |
| 8 | 1 | Mode: `A` |
| 9 | 1 | The original name's last character |
| 10 | 4 | Unpacked size, little-endian |
| 14 | | Packed data |

The packed data is LZSS over a 4096-byte window filled with spaces;
writing starts at window position 4080. A flag byte comes before each
group of eight items and is read from its lowest bit. A set bit is a
literal byte. A clear bit is a match of two bytes `b0 b1`: the window
position is `b0 | (b1 & 0xF0) << 4` and the length is `(b1 & 0x0F) + 3`.
Every byte written, literal or copied, also goes into the window.

Other installers on the discs use `KWAJ` (the Windows video setup) or
`ARCV` (Vortex's QuickTime drivers); the games themselves do not need
those files.

## In this project

- Reader: `src/data/szdd.c` (`xf_szdd_load` loads a file, unpacking it
  when packed).
- `xftool expand <packed> [out]` unpacks a file; `xftool expand -c
  <files>` checks a set.
