# Runtime: start-up, main loop and events

How the engine starts and runs, traced from the entry point of the DVD
`xfiles.exe`. Addresses and names are those in `tools/ida/names.txt`.

## Start-up

```
start (C runtime)
└─ WinMain                     0x654700
   ├─ app slot 1  VCEngineApp_init   0x411F90
   ├─ app slot 2  VCEngineApp_run    0x426B20
   └─ app slot 4  VCEngineApp_exit   0x4857D0
```

The application object is a global (`g_app`, `0x6B6FEC`) built by
`VCEngineApp_ctor` (`0x42F190`, vtable `0x65B060`) on top of a generic
application class (`0x493750`, vtable `0x65F528`).

`VCEngineApp_init` checks for a running copy, memory, the CD drive,
display mode and disk space, installs the four `.ttr` fonts, opens the
database, starts several subsystems, and finally posts a 0x114-byte
start-up event (`0x48EE40`, vtable `0x65D8C0`) on the event list at
`g_app + 0x268`. Our engine replaces the system checks (see the plan);
the start-up event is where the game itself begins.

## Main loop

`VCEngineApp_run` pumps Windows messages and calls app slot 3,
`VCEngineApp_tick` (`0x402A20`), on every iteration. The tick:

1. moves newly posted events from `g_app + 0x280` to `g_app + 0x268`;
2. services timer lists at `g_app + 0x104` and `+0x108`;
3. processes the events (`app_process_events`, `0x402830`) and other
   per-frame work.

## Events

Everything the engine does runs as events. An event object has a
vtable; slot 1 returns its type (0 to 23) and slot 11 runs it. Some
event classes return a fixed type (the start-up event returns 0); others
store the type in a field (queued actions keep it at `+0x18`).

`app_process_events` pops each event and passes it to `event_dispatch`
(`0x432D30`), which switches on the type and calls
`handler(manager, event, event list)` with the manager at `g_app + 0xE4`:

| Type | Handler | Type | Handler |
|---|---|---|---|
| 0 | `0x433180` ev_activate | 12 | `0x433320` |
| 1 | `0x41A250` | 13 | `0x41A410` |
| 2 | `0x440E40` | 14 | `0x435DE0` |
| 3 | `0x44D3C0` | 15 | `0x48B020` |
| 4 | `0x440FE0` | 16 | `0x41A380` |
| 5 | `0x45E0E0` | 17 | `0x4463A0` |
| 6 | `0x463860` | 18 | `0x41D200` |
| 7 | `0x4E0C80` | 19, 20 | none |
| 8 | `0x442D80` | 21 | `0x401320` |
| 9 | `0x44B5C0` | 22 | `0x433260` |
| 10 | `0x4E0BD0` | 23 | `0x4E0DF0` |
| 11 | `0x433240` | | |

Type 0 activates an object: the target of the event is registered in the
manager's table for its kind (slot 6 of the target), activated (slot 9)
and run (slot 11). The start-up event's run posts follow-up events, so
the boot sequence is a chain of events. Queued actions (see
[actions.md](actions.md)) are events too.

### Event types and target slots

Each event type calls one virtual method of the event's target, so
events work like messages:

| Type | Effect on the target |
|---|---|
| 0 | register by kind, then slot 9 (activate) and slot 11 (run) |
| 11 | slot 29 (command, with a code) |
| 12 | register by kind, then slot 9 (activate) and slot 22 (load) |

## Intro

`VCEngineApp_init` calls `play_intro_sequence` (`0x471820`) just before
it posts the main menu. Unless the preferences skip the intro
(`0x44E610`), it plays, one after the other and each skippable with a
click (see below):

| Order | Media ID | Media |
|---|---|---|
| 1 | 56003 | `Video:Game:Foxlogo.mov` (`XV/56003.xmv`) |
| 2 | 56002 | `Video:Game:HBlogo.mov` (`XV/56002.xmv`) |
| 3 | 19668 | `Video:Game:Teaser.mov` (`XV/19668.xmv`) |
| 4 | 56001 | `Video:Game:xtitle.mov` (`XV/56001.xmv`) |

On the DVD release, after the two logos the engine plays the DVD title
`ddigital1`; when it plays, the DVD titles `teaser` and `intro` follow
(`dvd_play_title`, `0x4E5E30`) instead of steps 3 and 4. Our engine does
the same when `vob/ddigital1.vob` exists (see
[formats/vob.md](../formats/vob.md)).

Each media file is looked up in the install folder first and on the CD
second (`0x461680`, `0x4616F0`).

The clips have different sizes: the Fox logo is 140×144, the HyperBole
logo and the title 560×400, the teaser 600×240. Our engine plays the
intro before a new game (`xfiles --explore <game> 0`; set
`XF_SKIP_INTRO=1` to skip it) and centres clips that are not 600×240.
Where the original places them is not yet read from the code.

## Main menu

The start-up event posted by `VCEngineApp_init` is the main menu
(`MainMenu_ctor`, `0x48EE40`). Its run (slot 11) posts two events to
itself: type 12, which loads its media, and type 11 with code 1, which
shows it.

`MainMenu_load_media` (`0x482FE0`, slot 22) creates players for media
references whose IDs are fixed in the code:

| ID | Media |
|---|---|
| 49583 | `Graphics:Game:MAIN MENU MOVIES:LEFTmain.mov` (`XG/49583.xmv`) |
| 49585 | `MIDloop.mov` (`XG/49585.xmv`) |
| 94320 | `MIDloop.amv`, the sound of the middle loop (`XS/94320.amv`) |
| 49589 | `TOPro.mov`, the top rollover (`XG/49589.xmv`) |
| 49587 | `RIGHT.mov` (`XG/49587.xmv`) |
| 72895 | `return.pic`, image 610 of `X.PFF`; only when a game is in progress (`g_game_in_progress`) |

The last entry shows that for PFF media the number in the media entry is
the image index.

`MainMenu_show` (`0x44DB40`) shows or hides these players and adds two
display items of the menu (at `menu + 0xA0` and `+0xD8`) to the display
list at `manager + 0x7D4`, on layers 21 and 20.

The menu object keeps its players at fixed offsets: left `+0x18`,
middle loop `+0x1C`, its sound `+0x20`, top `+0x24`, right `+0x28`. The
menu is animated by time: getters return segment times in milliseconds,
and tables at `0x6C3F38`–`0x6C3FB8` passed to the players (method
`0xF8`) hold rising 16-bit values, which look like segment start times.

The menu is not described in the game data: no media list or interface
item refers to its movies, and the movies carry no position (their track
matrices are all zero). We measured the layout on the original game
running under Wine with the enhancement patch, by matching decoded
movie frames against a screenshot of the menu:

| Movie | Size | Position | Match error per pixel |
|---|---|---|---|
| `LEFTmain.mov` | 472×480 | (0, 0) | low |
| `RIGHT.mov` | 168×480 | (472, 0) | low |
| `MIDloop.mov` | 388×310 | (60, 102) | 7 of 255 |
| `TOPro.mov` | 352×60 | (57, 0) | from the title's hit rectangle (57, 0)-(409, 60) |

The players are created with a canvas object (`menu + 0x44`) as parent;
reading where that canvas places them would confirm these positions.

The background, `LEFTmain.mov` (5.4 s), plays first; the centre loop
`MIDloop.mov` and its sound start once it has played. `TOPro.mov` is the
title's rollover, with sound: `MainMenu_hover` (`0x45E320`) plays it
while the pointer is in (57, 0)-(409, 60), and stops it and rewinds it
when the pointer leaves. The menu's click handler (slot 13, `0x496DC0`)
tests the item rows only, so a click on the title does nothing more.

The opening animation plays only the first time the menu opens. When
the player comes back to it (Escape during a game, or back from
Options, Help, Save or Load), our engine shows the background's last
frame and the labels already in place, without the opening sounds.

### The item column

`RIGHT.mov` (169 JPEG frames over 12 s) holds the item labels and their
rollovers, as frame ranges:

| Frames | Content |
|---|---|
| 0–65 | The labels appear one after the other |
| 66–89 | All labels, nothing highlighted |
| 90–100 | New: "Start a new investigation" |
| 101–109 | Load: "Return to an investigation that is already underway" |
| 110–120 | Save: "Store the status of your current investigation" |
| 121–130 | Previous |
| 131–140 | Options |
| 141–150 | Help |
| 151–162 | Quit |
| 163–168 | All labels again |

Each rollover is a short animation in which the label turns into its
description. The movie also has a sound track, presumably with the
rollover sounds. Our engine steps the movie by frame: it plays 0–66 once,
then plays an item's range on hover and goes back to frame 66 when the
mouse leaves. The item rows (New 80–115, Load 128–162, Save 178–210,
Previous 228–260, Options 277–312, Help 329–366, Quit 387–420, for
x ≥ 472) are measured from the frames; the engine's own hit areas and
segment tables are still to be read.

## Playing a clip with skip

`media_play_until_done_or_click` (`0x408AE0`) plays a clip and waits. In
its loop it peeks mouse messages: a left or right button press
(`WM_LBUTTONDOWN`, `WM_RBUTTONDOWN`) skips the clip. Without a mouse
press it peeks keyboard messages: Alt+X (`WM_SYSKEYDOWN` with `X`) also
ends the clip, and then runs the quit path.

## Interface layouts

`view_build_display` (`0x477000`) looks up the interface layout of the
current view, fetches the `VCIFaceLayout` from the database, wraps it in
a 0x17C-byte display object (`0x44D8B0`) and posts it as an event; it
then goes through the view's media references. Interface layouts are
therefore shown through the same event system.

## Next steps

- Follow the menu commands to "new game", and find where the intro
  videos play (before the menu or from a menu command).
- Decode `VCIFaceLayout` and `VCInterfaceItem` at run time: how items
  are placed, drawn and wired to triggers.
