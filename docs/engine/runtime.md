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

`MainMenu_show` (`0x44DB40`) shows or hides these players and adds them
to the display list at `manager + 0x7D4`.

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
