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
| `SetFLightProps(mode, place, beam)` | 0x44B4C0 | Mode 1 by day, 2 in the dark; place `'I'` inside, `'O'` outside; beam `'B'`, `'M'` or `'D'` |
| `SetLBoxTint(r, g, b)` | 0x4BC410 | Tints the letterbox around the picture, for example (20, 20, 50) at night |
| `ClearLBoxTint()` | 0x4B5AA0 | Black letterbox again |

`SetFLightProps` keeps its three values in globals (0x6B56B4 ..), and
the flashlight's cursor (0x49EA10) reads them with the player's beam
preference. In the dark the engine darkens the view by alpha 0xEE or
0xC8, and the flashlight lights the view under its beam. The explorer
darkens the view to 7% of its brightness, lights a circle of 70 pixels
around the pointer when the flashlight is in hand, and shows the view in
green with the night vision goggles. The circle size and the green are
estimates.


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

## ING searches

`INGSearch(category, database)` runs on the computer's search button.
The category is `'P'` (phone), `'L'` (licence plate) or `'N'` (name). The
database is `'Z'` civilian, `'F'` FBI, `'G'` government, `'L'` law
enforcement or `'C'` criminal. The engine ignores spaces, dashes and
brackets in what the player typed.

| Search | Engine | Records |
|---|---|---|
| Phone | `ING_search_phone` (0x5151A0) | Table at 0x65E0F0 |
| Plate | 0x514DE0 | The NSA car (240EAK, government: "Error: Restricted Data") and Mulder and Scully's rental (621517, civilian) |
| Name | 0x464F70 | 60 records at 0x65E1A0 |

A name record has five fields: a key item, whose media list gives the
databases that hold the person and then the names they answer to, a
record item with the 8 field texts, a mug shot item, a fingerprint item
and the field notes. Finding a person shows their mug shot and
fingerprint, fills the 8 fields (items at 0x65E180) and shows the field
labels (0x65E160). James Wong's record answers only once
`SHANKSN2_bDoneWithWong` is set, which happens when the player leaves
him on the docks. When nothing is found, the ING shows "No Data Found".
After a search the engine runs action list 90694 (found) or 90038 (not
found).

## E-mail

`SendEmail(media)` (0x484690) puts a mail, a text file under
`Text/Emails/`, in the mailbox. The computer's mail screen (layout 15171)
shows 8 rows, oldest at the top, scrolled so the newest is in view
(`DisplayInBox`, 0x48B7A0): the sender on one line, the subject on the
next and an icon while the mail is unread (0x48BE50).
`GetCurrEmail(row, var)` puts the mail of a row in the variable (0 for
an empty row), `MarkEmailRead(row)` marks it read, `EmailDelete(var)`
deletes it and `ScrollHeaders` scrolls the rows.

`InitEmailRead(item, var)` (0x43D9F0) shows a mail. Three mails have an
attachment, and for them the engine shows the items that open it: Amis's
Tarakan prints (61775, item 72722, which opens the fingerprint search),
the coroner's crew prints (64527, item 91007) and mail 61765.
The whole mail, headers included, goes into one scrolling text box
(0x4A3AF0): on the computer item 54950, or the smaller 72758 when the
mail has an attachment. Mail files end their lines with CR LF.

`PlayAssetAt(media, x, y)` (0x491190) plays a small clip over the screen,
such as the ING's progress bar; its start triggers start the search's
timer. `INGDisplayCook` (0x4B6620) shows Cook's ING record (number 3),
or "Restricted Data" once variable 5095 is set.

## Story progress

Each part of the story (a node) ends when the engine says so.
`SetIsNode2Done` .. `SetIsNode6Done` (0x4BC370 ..) check the variables
that `RegNodeNVars` registered and set the last one:

| Node | Complete when |
|---|---|
| 2 | Cook's talk at the desk is over and 3 pieces of evidence went to the lab |
| 3 | The coroner's office is done, Cook's field office scene too, and 3 pieces of evidence were processed (2 without the translated Cyrillic document) |
| 4, 6 | The node's last scene is finished |
| 5 | The hauling yard and the coroner's office are done |

The links between nodes have their own checks: `SetIsLink2Done` and
`SetIsLink3Done` (0x4BC330, 0x4BC350) set the second variable of
`RegLink2Vars` or `RegLink3Vars` when the current node is 12 or 13 and
the first variable is set. `RandGen(var, n)` gives the variable a random
value from 0 to n, and `RandGenRange(var, lo, hi)` one from lo to hi.

Sounds run their end triggers too: the alarm clock's end plays the
morning and shows the location card. A sound under `Loops/` never ends.

The travel lists then move `iCurrNode` to the link that follows: node
2 complete makes it 11, the warehouse at night. `SwapDisc`'s last
argument is this node variable, not a disc number.

## Gunfights

A gunfight clip has a HOT file (see [HOT](../formats/hot.md)) with a
rectangle that follows the target. A click inside the rectangle while it
is active is a shot. The engine maps the rectangle ID to a shot number
(0x4F4510); most shots need the gun in hand. Each shot plays a fixed clip
(0x4F4AC0). After the clip, depending on the shot, the game goes to a fixed
view, starts the current view again (a miss), or lets the clip's own end
triggers go on.

The Smolnikoff warehouse fight in node 6 has three areas with three goon
clips each:

1. `InitSmShootout(var, var, var)` resets the fight. Each variable is set
   when its area is clear.
2. `DoSmolShootout` goes to a random area that is not clear yet, and to
   that area's next clip. When all three areas are clear, it goes to the
   first floor (view 5208).
3. A goon clip starts timers 10 and 11. Timer 11 calls `SmIsWillShot1`:
   the goon may fire first. The chance depends on the try and on the
   difficulty (var 991): 20, 40 or 60 percent on the first try, and 50,
   65 or 80 percent on later tries. On a hit, Willmore's death clip plays.
   Timer 10 plays a random second chance or the next round.
4. The clip of a goon who is shot calls `SmolGoonShot1` when it starts,
   which clears the area, and `DoSmolShootout` when it ends.

Two fights in Alaska each have two NSA agents, and the engine counts who
is still standing:

- On Rauch's porch, the "gunfight" action icon (24616) starts the fight.
  Rectangles 101 and 103 are one agent (shot 1), 102 and 104 the other
  (shot 2). The counter is 0x4F5720. Each hit starts the view again until
  both agents are down. Then the engine puts `'a'` in RegViewState and
  goes to the porch (view 8138, through 0x4F3C60). The view's leave
  triggers read the `'a'`, and the NSA car takes Willmore to the secret
  base.
- In the woods, shots 4 and 5 are the two agents (counter 0x4F5A80).
  With both down, clip Sc114L (38448) plays.

Play scripts use `shoot [id]`: a shot at that rectangle of the view's
clip, or at the clip's first rectangle.

## Artificial Intuition

The AI icon (evidence icon 56185) sits in the inventory row while a hint
is set. `SetAIList(trigger, list)` (0x460170) sets the hint: a click on
the icon runs the action list with the trigger's parameters, which often
shows an action icon ("climb the railroad pole"). The engine makes the
icon glow while one of the list's conditions holds (0x4FE820).
`ClearAIList` removes the hint; travelling clears it.

## Binoculars

Taking the binoculars in hand (`OnBinocClick`, 0x518C80) shows the
close-up of the view at once, when it has one: a view of the same
viewpoint that no move leads to, named after the compass direction
("S number cu" beside the south view). Its picture shows whatever its
enable state, and its triggers run as usual. Putting the binoculars away
returns to the view.

## Characters that walk Willmore

`CharTeleport(event, event, node, location, viewpoint, view)` (0x4B58E0)
moves Willmore to a view, with the node, location and viewpoint changes
in between. A character uses it when clicked: Cook in the hall walks
Willmore to the meeting room. `DisplayDBox(character view)` (0x47CF00)
opens a character's dialogue box after a scene; `DisplayDBox(0)` closes
it.

## The end of the game

`EndGame(var)` (0x4B6020) ends the game: death, or the credits after the
last scene. Nothing runs after it. When the variable is set (a death) and
`SaveGameState` has taken a checkpoint, the engine asks "Try again?"
(engine string 39). Yes goes back to the checkpoint; otherwise the game
returns to the main menu.

## Saving and loading

Escape opens the main menu during a game; the game clock stops there.
The menu's Save and Load items open the Save screen (layout 82537) and
the Load screen (82634), each with three slots. On the Save screen the
player clicks a slot and types its name. Previous loads the slot that
was saved or loaded last. See [../formats/save.md](../formats/save.md)
for the functions and the file format.

Our saves go to the platform's save folder (`plat_save_dir`), or to
`XF_SAVE_DIR` when it is set:

| Platform | Folder |
|---|---|
| macOS | `~/Library/Application Support/The X-Files` |
| Linux | `$XDG_DATA_HOME/the-x-files` (by default `~/.local/share/the-x-files`) |
| Windows | `%APPDATA%\The X-Files`, or `The X-Files` beside the game on Windows 95 |
| PS Vita | `ux0:data/xfiles/saves` |
| DOS | `SAVES` beside the game |

The engine's questions (no slot name, overwrite a slot, try again) come
from `xfilese.dll` and show in a message box.

The isolation chamber in the secret base reports its doors and power
through `IsoChamberStat(door A, door B, power, eight outputs)` (0x4B6850),
which sets one output for each combination.

## Fades

`FadeToBlack(event)` (0x4B6140) fades the screen to black over 2
seconds; the explorer keeps it black until the view changes.
`FadeBack(on)` (0x48A2B0) shows or hides a set of screen layers through
0x40BC30; the explorer does not draw it yet.

## Debug overlay

Ctrl+Shift+D (or `XF_DEBUG_OVERLAY=1`) shows the hotspot outlines (green
for moves, yellow for objects, cyan for characters) and the name of the
view's media. It is off by default.

## Not yet like the original

- The scroll bar thumb and idea icons are not drawn.
- `FadeBack` and the photo download screen are not drawn.
- The colours of `SetIItemText` labels are guesses (`'b'` is drawn white).
- The question text colour and spacing in the box are estimates.
- The position of the inventory bar comes from the help texts, not
  from the engine code.
- The original shows its questions in a system message box; ours is
  drawn in the game screen.
- The original names each save file after its slot name; ours uses
  `slot1.xfs` to `slot3.xfs`, with the name inside.
- Original `.x` saves cannot be loaded yet.
