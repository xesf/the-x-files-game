# PFF picture bundle

A PFF file holds many still images in one file. The game uses PFF files for
interface graphics, inventory items, icons and photos. `X.PFF` and
`X7.PFF` are in the install folder, and smaller bundles are in `XG/`.

All numbers in the PFF header are little-endian. The images inside are
Macintosh PICT data, which is big-endian.

## Layout

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Magic `PFF ` (with a trailing space) |
| 4 | 4 | `count`: number of images |
| 8 | 4 × (`count` + 1) | Offset table |
| … | … | Unused table slots, filled with zeros |
| `offset[0]` | … | Image data |

- Image `i` starts at `offset[i]` and ends at `offset[i + 1]`.
- The last table entry, `offset[count]`, equals the file size.
- The table has room for `2 × count + 1` entries, so the first image always
  starts at `8 + 4 × (2 × count + 1)`. Only the first `count + 1` entries
  are used.
- Images are referenced by their index.

## Image data

Each entry is a version 2 PICT picture without the 512-byte file header
that PICT files on disk normally have. It starts with the picture size
(2 bytes, often truncated for large pictures) and the bounding rectangle
(top, left, bottom, right as 16-bit values), followed by the opcode stream.

The opcode that holds the pixels decides how to decode the image:

| Opcode | Name | Content | Share in `X.PFF` |
|---|---|---|---|
| `0x8200` | Compressed QuickTime | An image description and compressed data. The codec is JPEG (`jpeg`). | 418 |
| `0x009A` | DirectBitsRect | Direct-color pixels, PackBits-compressed per row. | 301 |
| `0x0098` | PackBitsRect | Indexed pixels with their own color table. | 4 |

Images that use direct pixels often carry a Photoshop long comment
(opcode `0x00A1`, kind `0x01F2`, `8BIM` data) before the pixel data. The
decoder skips it.

## Tools

```sh
xftool pff X.PFF                  # list images with size and kind
xftool pff-extract X.PFF 3 a.pict # save image 3 as a PICT file
```
