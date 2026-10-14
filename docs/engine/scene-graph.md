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

## Links to triggers and media

Most objects point to a `VCTriggerList` and some to media lists. In the
English DVD HDB:

| Object | Field | Points to |
|---|---|---|
| `VCNode` | 1st u32 | `VCName` |
| `VCNode` | 2nd u32 | `VCTriggerList` (6 of 13) |
| `VCLocaton` | 1st u32 | `VCName` |
| `VCLocaton` | 2nd u32 | `VCAssetRefList` (4 of 44) |
| `VCLocaton` | 3rd u32 | `VCTriggerList` |
| `VCViewPoint` | 1st u32 | `VCTriggerList` (50 of 347) |
| `VCNav` | 3rd u32 | `VCTriggerList` (90 of 3,911) |
| `VCExplorable` | u32 | `VCTriggerList` (400 of 401) |
| `VCCharView` | u32 fields | `VCCharacter`, then `VCConversationList`, `VCIdeaMapList`, `VCAssetRefList`, `VCTriggerList` |
| `VCInterfaceItem` | u32 fields | `VCAssetRefList`, then `VCTriggerList` |
| `VCConversation` | u32 fields | `VCName`, `VCAssetRefList`, `VCAssetRefList`, `VCTriggerList` |
| `VCTrigger` | u32 | `VCActionList` (5,335 of 5,429) |

## Triggers

A `VCTrigger` holds the ID of an action list and a kind byte that says
when the list runs. The kinds by owner, counted in the English DVD HDB:

| Owner | 0 | 1 | 3 | 4 | 5 | 6 | 7 | 8 | 9 |
|---|---|---|---|---|---|---|---|---|---|
| Interface item | 1,265 | 52 | 17 | 318 | 318 | 5 | 33 | 257 | 72 |
| Explorable | 398 | 6 | | 1 | | | | 5 | 9 |
| Move (`VCNav`) | 90 | | | | | | | 1 | |
| Conversation | 248 | | | | | | | 43 | 15 |
| Character view | 114 | 5 | | 4 | 4 | | 16 | 26 | 14 |
| View | 50 | | | | | | 50 | 178 | 109 |
| Viewpoint | 14 | | | | | 1 | 14 | 31 | 18 |
| Location | 25 | 17 | 1 | 2 | 1 | | 8 | 20 | 20 |
| Node | 1 | | | | | | | 5 | |

Kinds 2 and 11 occur once each. Working guesses, not yet confirmed in
the engine code: 0 is a click, 8 is entering and 9 is leaving the owner,
4 and 5 are the mouse moving over and off an interface item.

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

Hotspot rectangles use screen coordinates. The 600×240 picture is drawn
with its top-left corner at (20, 90): the explorable rectangle
(228, 114)–(408, 249) of view 1236 then covers the bulletin board in the
picture, and the turn hotspots cover the picture's left and right edges.

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
own `NAV*.NMV`. For stills the start and end numbers are the movie time
of the picture in milliseconds, plus 1: `nav1.nmv | 112001 | 112001` is
the picture shown at 112.001 s. The movie's edit lists decide which video
track shows the picture at that time (see
[../formats/quicktime.md](../formats/quicktime.md)). ffmpeg seeking to
112.001 s gives the same picture.

The meaning of the other numbers is not yet confirmed.

## Exploring

`xfiles --explore` shows a view's still and outlines its hotspots: green
for moves, yellow for things to examine, cyan for characters. Clicking a
move goes to its target view; a right click goes back.

```sh
build/xfiles --explore /path/to/game 1236      # Field Office, disc 1
build/xfiles --explore /path/to/game 1236 2    # the same view on disc 2
```

Following every move from view 1236 reaches 76 views of the Field Office,
none of them a dead end. 47 have a navigation still on disc 1 and 50 on
disc 2; the others show loops or video, which the explorer does not play
yet.

## Tools

```sh
python3 -I tools/re/hdb_tree.py XFILES.HDB 301 3   # graph below node 301, 3 levels deep
xftool hdb-asset XFILES.HDB 39028                  # decode a media reference
xftool hdb-walk XFILES.HDB 1236 1                  # follow every move from a view
xftool still NAV1.NMV 112001 out.bmp               # the still shown at 112.001 s
```
