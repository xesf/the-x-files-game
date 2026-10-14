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

### Direct pixels (`0x009A`)

The PixMap gives `pixelSize` 32 and `cmpCount` 3, with `packType` 4.
Each row is PackBits-compressed (a 1-byte count when `rowBytes` is 250 or
less, a 2-byte count otherwise). A row unpacks to `width × cmpCount`
bytes, not to `rowBytes`: one plane of red bytes, then green, then blue.

### QuickTime images (`0x8200`)

The opcode data starts with a 4-byte size. The fields that matter are:

| Offset in data | Size | Field |
|---|---|---|
| 2 | 36 | Transform matrix (3 × 3, 16.16 fixed point) |
| 38 | 4 | Matte size |
| 52 | 8 | Source rectangle |
| 64 | 4 | Mask size |
| 68 | … | Matte data, mask data, then an image description and the JPEG data |

Large pictures are split into bands. A 640×480 picture can hold five
`0x8200` opcodes, each with a source rectangle at (0, 0) and the band
position in the translation part of the matrix (entries 7 and 8, at data
offsets 26 and 30). Between bands, QuickTime writes a fallback text
("QuickTime and a … decompressor are needed …") with ordinary text
opcodes. The decoder skips it.

### Placeholders

A few entries are 1×1 pictures that only paint a black rectangle. They
decode as a blank image.

All 809 images in the English CD release (`X.PFF`, `X7.PFF` and the
bundles in `XG/`) decode with the reader in `src/media/pict.c`.

## Tools

```sh
xftool pff X.PFF                  # list images with size and kind
xftool pff-extract X.PFF 3 a.pict # save image 3 as a PICT file
xftool pff-image X.PFF 3 a.bmp    # decode image 3 to BMP
xftool pff-image X.PFF all        # check that every image decodes
```
