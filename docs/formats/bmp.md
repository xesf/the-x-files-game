# Windows bitmaps (earlier games)

The earlier HyperBole games keep their pictures as Windows bitmaps, most
of them compressed. The X-Files does not use these; its pictures are
PICT files in PFF bundles ([pff.md](pff.md)).

| Game | Pictures | Compression |
|---|---|---|
| Vortex | 224 `.BMP`, 16-bit | Microsoft Video 1 (`CRAM`) |
| Vortex | 8 `.BMP`, 8-bit (the menus) | Microsoft Video 1 |
| Quantum Gate | 329 `.BMP`, 16-bit | Microsoft Video 1 |
| Quantum Gate | 14 `.BMP`, 24-bit; 4, 8-bit | None |
| Quantum Gate | 387 `.HSB`, 8-bit | SZDD-packed ([szdd.md](szdd.md)); no colour table |
| Quantum Gate | `MILITERM/QBACKGRD.BMP` | Intel Indeo 2.1 (`RT21`), not read yet |
| Roland | 408 `.RLE`, 8-bit | RLE8 |
| Roland | 4 `.BMP`, 8-bit | None |

The reader decodes all of them but the Indeo one (1379 of 1380).

## Structure

A `BITMAPFILEHEADER` (14 bytes, `BM`, the pixel data's offset at 10) is
followed by a `BITMAPINFOHEADER` (40 bytes: width, height, planes, bits
per pixel, compression, image size, resolution, colours used,
important colours), the colour table (4 bytes an entry: blue, green,
red, 0) and the pixel data. Lines go bottom up when the height is
positive. A Quantum Gate `.HSB` unpacks to the info header and the
pixels only: no file header and no colour table, its indices being into
the game's palette.

## RLE8

Pairs of bytes: a count and a palette index repeat the index. A count
of 0 is an escape: 0 ends the line, 1 ends the picture, 2 moves the
position by the next two bytes (right, up), and a value of 3 or more
copies that many indices, padded to a 16-bit boundary.

## Microsoft Video 1

The picture is cut into 4×4 blocks, read from the bottom left, a row of
blocks at a time going up. Each block starts with two bytes `a`, `b`:

| `b` | Block |
|---|---|
| `b & 0xFC == 0x84` | Skips `((b - 0x84) << 8) + a` blocks, this one included |
| below `0x80` | Two colours: `b << 8 | a` gives a bit per pixel, from the block's bottom left, a row at a time going up; a clear bit takes the first colour |
| 16-bit, otherwise | One colour, `b << 8 | a` (RGB555) |
| 8-bit, `0x90` and above | Eight colours: a pair per 2×2 quarter |
| 8-bit, otherwise | One colour, index `a` |

In 16-bit pictures the two colours are RGB555 words; when the first has
its top bit set, three more pairs follow and each 2×2 quarter (bottom
left, bottom right, top left, top right) uses its own pair. In 8-bit
pictures the colours are palette indices.

## In this project

- Reader: `src/media/bmp.c` (`xf_bmp_load` unpacks an SZDD file first).
- `xftool bmp <file> [out.bmp]` decodes a picture; `xftool bmp -c
  <files>` checks a set.
