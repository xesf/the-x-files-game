# Game interface

The parts of the screen the player works with besides the view: the
location card, the dialogue box, the emotion and action icons, and the
inventory. Positions are in the 640×480 screen. The video and the view
picture take the 600×240 area at (20, 90).

## Location card

`StartVideoText` shows the place, the date and the time over the next
clip (see [scene-graph.md](scene-graph.md#location-card) for the text).
`VideoText_start` (0x483A30) draws it as a text object:

| Property | Value |
|---|---|
| Font | Courier New, 12 point (font number 4 of `Font_select`, 0x53C520) |
| Position | Lower left of the clip, 15 pixels from its left and bottom edges |
| Typing | One character every 50 ms |

`Font_select` maps font numbers to Windows font families: 2 Times New
Roman, 3 Arial, 4 Courier New. The game's own fonts (`Dlg.ttr`,
`Jrn.ttr`, `Phn.ttr`, `Hcd.ttr`) are installed at start-up. The explorer
loads Courier New from the system fonts.

## Dialogue box

Clicking a character with nothing in hand opens the dialogue box with
the questions Willmore can ask (see
[game-state.md](game-state.md) for conversations). The art is fixed in
the engine (`DialogBox_load_media`, 0x482610), all in `x.pff`:

| Media | Picture | Size |
|---|---|---|
| 34761 | `folder-talk.pic`, the TALK tab in front | 246×120 |
| 34760 | `folder-history.pic`, the HISTORY tab in front | 246×120 |
| 34762 | `scrollbar.pic` | 12×68 |
| 34763 | `thumb.pic` | 12×10 |
| 34759, 34764 | `up.pic`, `down.pic` | 12×10 |
| 85383 | `historyNONpatch.pic` | 94×12 |

Pure blue (0x0000FF) is transparent in these pictures.
`DialogBox_place` (0x4810A0) centres the folder on the character,
keeps it on screen, and puts its top at y 285. The box shows again as
soon as an answer has played, with the remaining questions.

The TALK tab lists the questions not asked yet; the HISTORY tab lists
the ones already asked. Clicking a tab label switches between them. The
box closes with Escape, the close box at the top right, or a click
outside it.

The text of a question is its icons field, a type 6 media reference:
the number of a string in `xfilest.dll`. That DLL holds 1015 strings in
`RT_STRING` blocks of 16. The other DLLs hold the error texts
(`xfilese.dll`), the credits (`xfilesc.dll`) and the subliminal words
(`xfiless.dll`).

`RegDlgAss` names six dialogue assets in the data, but this build of
the engine ignores the call.

## Emotion and action icons

Some scenes let the player answer with an emotion, or act, by clicking
an icon. These are `VCEmotionIcon` and `VCActionIcon` objects:

| Field | Content |
|---|---|
| u32 | `VCName`, for example "Field Office N1A Cook - Funny" |
| u32 | 0 |
| u32 | `VCAssetRefList`: the icon, an 80×60 looping clip (`Graphics:Icons:emotion icons:funny.mov`) |
| u32 | 0 |
| u32 | `VCAssetRefList` |
| u32 | `VCTriggerList`: kind 0 runs on a click |

A scene enables its icons for a while and disables them again. At the
start of the game, Cook's greeting (`SC03C.mov`) enables three: Funny,
Indifferent and Paranoid. If the player does not choose within 30
seconds, timer 0 disables them and Cook goes for a second cup of
coffee. The help text says rolling over an icon shows the emotion. The
explorer shows the enabled icons in a row 10 pixels below the video, and the name
after the dash under the icon on rollover. Choosing one disables all
the icons shown. Action icons only appear with Artificial
Intuition on.

## Inventory

`VCInventory` objects hold a name, the icon, the rollover icon, 0, a
cursor and a trigger list. The icons are 30×40 pictures in `x.pff`. The
help text says the inventory shows when the pointer moves to the bottom
of the screen, and the item in hand stays on screen. The explorer shows
the icons of the items the player carries along the bottom when the
pointer is below y 420.

## Cursors

A `VCCursor` holds the hot spot x and y (16 bits each), a name and a
media list. The pictures are `.pic` files in `x.pff`; a `.pff` cursor
is an animation from its start frame to its end frame.
`VCDefaultCursors` (321) names the cursor for characters (290, lips)
and for objects (7164, eye). Moves use the cursor of their hotspot, and
the backdrop of a close-up uses 287 (back up). With an item in hand,
the cursor is the item's cursor picture. Everywhere else, the dialogue
box included, the cursor is 25541, the spinning blue X
(`Graphics/cursors/Blue X Icon Picts/BlueX.pff`, 12 frames).

## Character lines

A `VCCharView` has its own media list: what the character says when
clicked, before the questions, such as a clerk's "Can I help you?" or
Wong's "You're back.". The first enabled entry plays. Its start
triggers can change the scene: Skinner's "That sedan has been
following us..." at the warehouse shows the car.

## Camera

Clicking the camera in the inventory runs `OnCameraClick` and, where
the scene allows it, `TakePhoto(media)` (0x4BCCC0). `Camera_take_photo`
(0x4D8FE0) makes a 300×240 `VCPhoto` of the media and keeps it in the
camera; the camera holds 24 photos. The computer downloads them later.
`SelectInv(item)` puts an item in hand without running its click list:
the photo clip uses it to give the camera back.

## Evidence

Evidence the player collects is a `VCEvidenceIcon` (class 0x49), with
the same fields as `VCInventory`. The engine rebuilds the evidence
icons with each view (`view_build_display`, 0x477000) from two lists:
the collected evidence, and the kit's copies ("Black Powder (warehouse)
- KIT"). A new piece of evidence runs its kind 8 triggers, which call
`EnableKitIcon(copy)` (0x4B5F20) to put its copy in the kit.

`DispEvIcons(1)` (0x4B5B40) shows the collected evidence as a second
row above the inventory, and `DispEvIcons(0)` hides it. At the Crime
Lab the loop where Amis leans on the window turns it on, and leaving
the view turns it off. Clicking a piece runs its kind 0 triggers: the
technician's answer plays and the piece goes away. Icon 56185, "AI
Icon", is the Artificial Intuition icon, not evidence.

A view's clip can change while the player stays in the view, when a
trigger switches its media list. The old clip's kind 9 triggers run,
then the new clip's kind 8 triggers.

## Darkness and the flashlight

Some places are dark: the warehouse mezzanine by day, and the
warehouse at night.

| Function | Address | Use |
|---|---|---|
| `OnFLightClick(item)` | 0x4B7530 | Takes the flashlight in hand. Its inventory click also opens the dark moves of the view |
| `SetFLightProps(mode, place, 'M')` | 0x44B4C0 | Mode 1 by day, 2 in the dark; place `'I'` inside, `'O'` outside |
| `SetLBoxTint(r, g, b)` | 0x4BC410 | Tints the letterbox around the picture, for example (20, 20, 50) at night |
| `ClearLBoxTint()` | 0x4B5AA0 | Black letterbox again |


## Computer

Willmore's computer is a set of interface layouts. Typing goes to the
item with focus (`SetIItemFocus`): the item's kind 3 triggers run with
the key as the event value.

| Function | Address | Use |
|---|---|---|
| `LoginInit` | 0x47D090 | Clears the name and password fields |
| `LoginClick` | 0x4B7250 | Moves the focus to the clicked field |
| `LoginKeyPress(field, key)` | 0x46A000 | Adds the key to the field; backspace removes one |
| `PswdConnect(result)` | 0x4598B0 | Checks the login |

`PswdConnect` compares the name, ignoring case, with media 57905
(string 937, "craig willmore") and the password with media 68915
("shiloh"). The password field shows stars.

`SetIItemText(item, text, color, font, size, bold, align)` writes a
label on a layout item: `font` is `'H'` (`Hcd.ttr`) or `'c'` (Courier),
`size` is in points, `bold` is `'y'` or `'n'`, and `align` is `'l'`,
`'c'` or `'r'`. `RelocateIFace(left, top, right, bottom, event)` moves
the item whose trigger is running.

## Debug overlay

Ctrl+Shift+D (or `XF_DEBUG_OVERLAY=1`) shows the hotspot outlines (green
for moves, yellow for objects, cyan for characters) and the name of the
view's media. It is off by default.

## Not yet like the original

- The scroll bar thumb and idea icons are not drawn.
- The colours of `SetIItemText` labels are guesses (`'b'` is drawn white).
- The question text colour and spacing in the box are estimates.
- The position of the inventory bar comes from the help texts, not
  from the engine code.
