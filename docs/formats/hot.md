# HOT hotspot file

A HOT file lists the clickable areas of one video clip. It sits next to the
clip in `XV/` and has the same number as its name: `XV/19672.hot` belongs to
`XV/19672.xmv`. Not every clip has a HOT file.

A hotspot can move. A figure walking across the screen, for example, is
described by a series of rectangles, each active for a short time range.

All numbers are little-endian.

## Layout

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Magic `HSPT` |
| 4 | 4 | `count`: number of records |
| 8 | 20 × `count` | Records |

## Record

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `id`: hotspot ID. Records with the same ID belong to the same hotspot. |
| 4 | 2 | `left` |
| 6 | 2 | `top` |
| 8 | 2 | `right` |
| 10 | 2 | `bottom` |
| 12 | 4 | `start`: time at which the record becomes active, in milliseconds |
| 16 | 4 | `end`: time at which the record stops being active, in milliseconds |

- Coordinates are pixels in the video frame, not on the screen.
- Start and end times fall on frame boundaries at 30 frames per second
  (for example 567, 1033, 1067).
- The IDs are not in the game data. The engine maps each ID to a shot
  (0x4F4510): most need the gun in hand, and each shot plays a fixed clip
  (0x4F4AC0). See [Gunfights](../engine/game-ui.md#gunfights). The most
  common IDs are 600 and 500.

All 390 HOT files in the English CD release follow this layout exactly.

## Tools

```sh
xftool hot XV/19672.hot
```
