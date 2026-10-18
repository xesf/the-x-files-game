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

A view's media list is in order of priority. Its full-picture loops are
alternatives: the first enabled one is the picture and the others are
not shown. Willmore's desk (view 1237) lists Cook at the desk, the desk
with the letter and files (disc 1), then the empty desk (every disc);
both of the last two are enabled at the start of the game. Smaller loops
and the navigation still are drawn as well. A looping clip with a sound track plays its
sound and starts again at its end (the Comity Inn rooms' TV, view 72623,
`KeystoneCopsA.mov`); silent loops only step their pictures.

## VCView

| Field | Content |
|---|---|
| u32 `e` | Direction: 1, 3, 5 or 7 for the four compass directions. Views that are not a compass direction hold a name ID instead. |
| u32 `f` | ID of the `VCAssetRefList` with the media of the view |
| u32 `g` | ID of the `VCCharViewList`, or 0 |
| u32 `h` | ID of the `VCNav_List` |
| u32 `i` | ID of the `VCExplorableList`, or 0 |
| u32 `j` | ID of the `VCTriggerList`, or 0 |

## Starting a new game

`VCTitle` (object 299) holds a trigger list in its last 4-byte field
(list 23517). Its kind 8 trigger runs action list 26327, the start-up
sequence:

1. `Reg*` functions tell the engine which GAM variables hold its key
   state: inventory (`RegCurrInvVar`), current node (`RegCurrNode`,
   variable 1219 `iCurrNode`), day and night (`RegDayNight`, variable
   3742 `iDayNight`), the uber variables (`RegUberVars`), the current view
   (`RegViewSpecial`, variable 1057) and per-chapter variables.
2. The first e-mails are sent and the language is set.
3. `SetStartView(301, 303, 690, 691)` places the player: node 301,
   location 303 (the Field Office), viewpoint 690, view 691.

Entering view 691 the first time (`bFirstTimeInFieldOffice`) runs the
location card and the opening scene.

The four moves of view 691 start disabled in the GAM. The opening clip
`SC03C.mov` (media 19812) carries trigger list 28612. Its kind 9 trigger
runs when the clip ends and enables the moves, so the player can only
walk around the office after the opening scene.

## Location card

The game opens each place with a timed screen that takes no input: an
establishing shot with the place, date and time typed over it. The data
calls `StartVideoText(label, day variable, time variable, show time)`,
for example `StartVideoText(57947, var3742, var2923, var605)`:

- The label is a `VCAssetRef` whose name is the place, here
  "FBI FIELD OFFICE, SEATTLE". The establishing shot itself is queued by
  a media action in the same list.
- The date comes from `iDayNight` (`0x4DB580`). Values above 10 count
  from 10 (night); the result, clamped to 0–7, picks a day from a table
  at `0x659C80`: 29 March 1996 for 0, then 2 to 8 April 1996. The engine
  formats the date with the Windows long date format.
- When the last argument is true, the time follows: `iCurrTime` holds
  hours and minutes as a number (914 is 9:14), formatted with the Windows
  time format.
- Lines are separated by line breaks (`0x6B6538`). The text appears with
  timers of 50 ms and 200 ms (`0x483A30`); the explorer reveals one
  character every 50 ms.

At the start of the game this gives:

```
FBI FIELD OFFICE, SEATTLE
Tuesday, April 2, 1996
9:14 AM
```

The font, position and colour of the text are not yet checked against
the original.

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

Kinds 2 and 11 occur once each. What the kinds mean, with the evidence:

| Kind | Meaning | Evidence |
|---|---|---|
| 0 | Click | The click handlers call `triggers_run_kind` (0x4F6EA0) with kind 0. |
| 3 | Key press | The actions pass the event value (operand kind 4) to `LoginKeyPress` and `INGKeyPressed`. |
| 4 | Mouse over | The pointer handlers (0x4EEB90, 0x442B70) run kind 4. The 318 kind 4 triggers on interface items pair with 318 kind 5 triggers: 4 enables a highlight and 5 disables it again. |
| 5 | Mouse off | See kind 4. |
| 7 | Timer | See [Timers](actions.md#timers). Confirmed in the engine code. |
| 8 | Enter | The owner appears: view entry, media start. |
| 9 | Leave | The owner goes away: view exit, media end. |

Kind 1 is a debug click: it shows and hides interfaces and cheats (the
E-MAIL button's kind 1 sends every mail); the explorer does not run it.
Kind 6 follows a finished request. The phone's send key is the clear
case: when `PhoneSend` finds the number, the key's kind 6 triggers stop
the ringing and place the call. The other kind 6 lists play a line after
a sound.

The data supports 8 and 9: kind 8 lists on views store the current view
in variable 1057, and kind 9 lists disable hotspots and stop timers. The
explorer runs them in this order when the player moves: kind 9 of the
old view, viewpoint, location and node (inside out, only for the levels
that change), then kind 8 of the new node, location, viewpoint and view
(outside in). On the first visit to the Field Office this plays the
establishing shot of the FBI building and the scene where Willmore
enters, as in the original game.

Media also carries triggers. The third field of a `VCAssetRef` holds a
trigger list (see [Media references](#media-references)). 733 media
references in the English DVD HDB have one. Its kind 8 triggers run when
the media starts and its kind 9 triggers run when the media ends or the
player skips it. A view's own clip is media too: one that plays once
rather than loops (no 0x08 in its kind) runs its kind 9 triggers when it
ends, or when the player leaves the view before that. 24 such clips have
end triggers, for example Secret Base 2A (view 93960), which ends the
game unless Willmore fired (`xftool hdb-viewclips` lists them). The engine attaches trigger lists through
`TriggerHandler_attach` (0x416800) and runs one kind with
`triggers_run_kind` (0x4F6EA0). When a clip is missing on disk, the
explorer still runs both kinds, so the story goes on.

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
| u32 | Flags in the top byte; the ID of a `VCTriggerList` in the low 24 bits, or 0 |
| u8 | Media type: bit flags (see below) |
| u8 | Category: the number of a `VCAssetCategory` |

The media type values found in view media lists are 1, 3, 9, 11, 17, 19,
33, 41, 43, 65, 67 and 99. Bit 0x40 marks a still taken from a
navigation file (`.nmv`). Bit 0x08 marks a loop, for example the
characters idling in a room (`Navs:LoopsR:…`). The other bits are not
yet understood.

A view shows only the media in its list that are enabled. Many loops
start disabled in the GAM and actions switch them on. For example, the
opening clip of a new game enables the loop of Cook standing with his
coffee in view 691 (`VCEnabled` 424). 584 views have no still at all:
their picture is a loop or a range of a movie. The explorer draws the
still first and then the moving media over it. It reloads the view
media whenever actions change which ones are enabled.

The 84 categories (`VCAssetCategory`: name, number) group the media by
use and by place, for example 1 "Emotion Icons", 7 "Inventory", 8
"Cursor", 18 "Field Office", 21 "Dialog box pieces", 37 "Game" (intro
and main menu media), 63 "Location Text" (the location cards), 69
"E-mail" and 75 "Interface". Many are marked "Unused" by the authors.

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

## Interfaces

Close-ups, books, the PDA and other screens are interface layouts. An
interface action (type 5) shows a `VCIFaceLayout` with control 0 and
closes it with control 1 (`act_interface`, 0x44E760). Showing a layout
that is already up does nothing.

A `VCIFaceLayout` holds a list of `VCInterfaceItem` objects. Each item
has these fields:

| Field | Content |
|---|---|
| Region | Cursor and rectangle in screen coordinates |
| u32 | `VCAssetRefList` with the picture, often an image of `x.pff` |
| u32 | `VCTriggerList` |
| u16 | Flags in the high byte (0x80 in all items seen so far); drawing order in the low byte |

Items with a higher order are in front. A close-up typically has two
items. The picture has order 200. Behind it, a full-screen item with
order 199 closes the layout when the player clicks outside the picture.
Book pages add page-turn items with order 100 at the edges of the
picture. Those items have triggers and the picture has none, so the
explorer gives a click to the front item that has triggers. While a
layout is up, it takes every click. Rollover highlights are items that
start disabled: the kind 4 trigger of an item enables its highlight and
the kind 5 trigger disables it again.

## Exploring

`xfiles --explore` shows a view's still and moving media and outlines
its hotspots: green for moves, yellow for things to examine, cyan for
characters. Clicking runs the click triggers of the hotspot: moves go
to their target view, objects play clips or open close-ups, and
characters start their scenes. A right click goes back.

```sh
build/xfiles --explore /path/to/game 1236      # Field Office, disc 1
build/xfiles --explore /path/to/game 1236 2    # the same view on disc 2
build/xfiles --explore /path/to/dvd 0 1 --media /path/to/cd   # DVD data, CD clips
```

The DVD build streams its video from the disc, so its install folder
has no `XV` clips. The DVD database names the same `XV/<id>.xmv` files
as the CD version. `--media` adds folders to search for media files
that are missing from the game folder. `make run` uses the English CD
install by default.

Following every move from view 1236 reaches 76 views of the Field Office,
none of them a dead end.

## Tools

```sh
python3 -I tools/re/hdb_tree.py XFILES.HDB 301 3   # graph below node 301, 3 levels deep
xftool hdb-asset XFILES.HDB 39028                  # decode a media reference
xftool hdb-walk XFILES.HDB 1236 1                  # follow every move from a view
xftool still NAV1.NMV 112001 out.bmp               # the still shown at 112.001 s
```
