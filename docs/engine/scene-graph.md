# Scene graph

How the game model in `XFILES.HDB` describes places and how the player
moves between them. This page grows as each part is reverse-engineered.
Object IDs below are from the English DVD release.

## Hierarchy

```
VCTitle (299)
└─ VCNode: a chapter of the game (13 in total)
   └─ VCLocaton: a place, for example the Field Office
      └─ VCViewPoint: a spot where the player stands
         └─ VCView: one direction the player can face at that spot
            ├─ VCAssetRefList: media for this view (stills, loops, videos)
            ├─ VCCharViewList: characters visible in this view
            ├─ VCNav_List: moves to other views
            ├─ VCExplorableList: things the player can examine
            └─ VCTriggerList: triggers checked in this view
```

## VCViewPoint

| Field | Content |
|---|---|
| list | The views of this viewpoint |
| position | A grid position (x, y) and the ID of the viewpoint's name. The name ID is the viewpoint ID minus one. |
| u32 | ID of a trigger list, or 0 |
| u32 | Not yet known |

Example: viewpoint 690 has position (0, 0), name 689 and four views.

## VCView

| Field | Content |
|---|---|
| u32 `e` | Direction: 1, 3, 5 or 7 for the four compass directions. Views that are not a compass direction hold a name ID instead. |
| u32 `f` | ID of the `VCAssetRefList` with the media of the view |
| u32 `g` | ID of the `VCCharViewList`, or 0 |
| u32 `h` | ID of the `VCNav_List` |
| u32 `i` | ID of the `VCExplorableList`, or 0 |
| u32 `j` | ID of the `VCTriggerList`, or 0 |

## Navigation

A `VCNav` connects a view to another view:

| Field | Content |
|---|---|
| u32 | ID of the target `VCView` |
| u32 | ID of the `VCHotSpot` to click |
| u32 | Not yet known, 0 in the examples |

The standard turn hotspots are shared by many views. Hotspot 275 covers
the right edge of the screen, (500, 90) to (640, 330), with cursor 285.
Hotspot 274 covers the left edge, (0, 90) to (140, 330), with cursor 284.
A move forward uses a hotspot in the middle of the picture, with cursor
286.

The picture is 600×240 pixels, but hotspot rectangles use screen
coordinates (up to 640 wide), so the picture is drawn at an offset. The
exact offset is not yet confirmed.

## Media references

A `VCAssetRef` names one piece of media:

| Field | Content |
|---|---|
| string | Packed media description (below) |
| u32 | ID of a `VCName` with a short label |
| u32 | Flags |
| u8 | Media kind: 3 for video, 9 for navigation loops, 67 for navigation stills |
| u8 | Not yet known |

The packed description is a sequence of tokens. Each token is a Pascal
string: one length byte, then that many characters. Numbers are written
in decimal. Paths use `0x7F` as the folder separator. The tokens are:

1. The logical path from production, for example
   `Video:Node 1:Field Office:SC03G.mov`.
2. For some kinds, a start and end time in that source.
3. For each disc the media is on: the disc number, the shipped file (for
   example `XV:19820.xmv` or `nav1.nmv`), and a start and end time.

Examples, with tokens separated by `|`:

```
Navs:LoopsR:Field Office:043 - sc3NAV1ncooklooptk2.mov | 0 | 1 | XN:25683.xmv | 0 | 0
Video:Node 1:Field Office:SC03G.mov | 7800 | 7867 | 0 | 1 | XV:19820.xmv | 7800 | 7867
Navs:Field OfficeR:01:1n empty placeholder.mov | 0 | 0 | 0 | 1 | nav1.nmv | 0 | 0 | 2 | nav2.nmv | 0 | 0 | …
```

The navigation stills are listed once per disc, because each disc has its
own `NAV*.NMV`. The meaning of every number is not yet confirmed.

## Tools

```sh
python3 -I tools/re/hdb_tree.py XFILES.HDB 301 3   # graph below node 301, 3 levels deep
```
