# MIDI music (earlier games)

Quantum Gate (192 songs) and Vortex (240) play their music from
Standard MIDI Files, all of format 1 with 1 to 16 tracks and a tempo in
ticks a beat. The X-Files has no MIDI; its music is QuickTime sound.

A file starts with an `MThd` chunk (format, track count, ticks a beat)
and has an `MTrk` chunk per track. A track is a list of events, each
after a variable-length delta time (7 bits a byte, the top bit set on
all but the last byte): channel messages, with running status (a data
byte where a status byte is expected repeats the last status), system
exclusive messages (`F0`, `F7`) and meta events (`FF`, a type, a
length). Only the tempo meta event (`FF 51`, microseconds a beat,
500000 by default) changes the timing; `FF 2F` ends the track.

The reader merges the tracks into one list of channel messages timed in
microseconds, following the tempo changes. All 432 songs read.

## Playing

The engine links no synthesizer. `plat_midi_open` uses the system's
own: on macOS the DLS synthesizer (Apple's General MIDI sounds, from
AudioToolbox), on Windows the MIDI mapper. Linux, DOS and the Vita have
none yet, and the games play there without music. `src/games/music.c`
sends a song's messages from the main loop as they fall due.

- Reader: `src/media/midi.c`.
- `xftool midi <files>` reads songs and reports failures.
