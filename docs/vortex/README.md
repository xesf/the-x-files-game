# Vortex: Quantum Gate II

Notes on `VORTEX.EXE` (Virtual Cinema 2.0, 1994), the record of what
is known so far. Addresses are `segN:offset` in the NE executable;
`tools/ne/ne.py` lists them.

## The program

`VORTEX.EXE` is a 16-bit Windows 3.1 (NE) program of 56 segments:

| Segments | Content |
|---|---|
| 1 to 17, 22 | The engine (C, with the compiler's run-time library in 16) |
| 18 to 21, 23 to 55 | The game: sequence data, about 640 KB |
| 56 | The automatic data segment (globals, the run-time's strings, assertion texts) |

The engine's assertion texts name its source files: `sequence.c` (the
interpreter), `uber.c` (moods), `bitmap.c`, `playmidi.c`, `midi.c`. It
imports KERNEL, USER, GDI, MMSYSTEM (MIDI and sound), MSVIDEO (Video
for Windows decompressors) and loads QuickTime for Windows itself
(`qtim.dll`, `qcmc.qtc`, `qtcvid.qtc` and the other codecs).

So the game's logic is data after all, but compiled into the program:
the sequence segments hold instructions, strings and far pointers to
other instructions, joined by the program's segment fixups. No other
file holds it.

## The sequence interpreter

`seg8` runs up to 45 sequences side by side. Each has a context of 0x136
bytes in an array at `DS:51D8`; `seg8:0000` clears them and starts the
first at `seg18:0000`. The context's fields include:

| Offset | Content |
|---|---|
| +0x04 | In use |
| +0x14 | The far pointer to the sequence's next instruction |
| +0x24 | The sequence's number |
| +0x64 | Its local variables, 2 bytes each |

To run an instruction the interpreter copies the pointer to `DS:2578`,
reads the first word, clears its top bit and branches on it
(`seg8:0A26`, the branches from `seg8:6B42`). The handler reads its
operands after the opcode and moves the pointer on (`add [2578], n`).
There are 156 opcodes, from 0x0001 to 0x0828;
`tools/ne/vortex_ops.py` lists each with its handler.

### Operands

Most operands are 16-bit words whose top two bits say what they are
(`seg8:6EEA`):

| Top bits | Meaning |
|---|---|
| `00` | The value itself (0 to 0x3FFF) |
| `01` | The sequence's local variable `n` (`context + 0x64 + 2n`) |
| `10` | Global variable `n` (`DS:39F8 + 2n`) |
| `11` | The context's word `n` (`context + 2n`): its own fields |

Strings follow a type byte of 1 and end with a 0 byte. Far pointers to
other instructions are an offset word and a segment word fixed up at
load time.

### Examples

From the start, `seg18:0000`:

| Bytes | Instruction |
|---|---|
| `07 04 12 C0 2D 00` | 0x407: context word 0x12 (its number, +0x24) = 45 |
| `07 04 4D 80 09 00` | 0x407: global 0x4D = 9 |
| `06 04 4D 80 21 80` | 0x406: global 0x4D = global 0x21 |
| `5D 00 01 "c\cursor1.bmp"` | 0x5D with a string |
| `90 01 01 "c\splash\forbob.mov"` ... | 0x190: a clip |

### Operand layouts

`tools/ne/vortex_seq.py` decodes the sequences. It starts at
`seg18:0000` and follows every far pointer an instruction holds, as a
disassembler follows jumps; the far pointers are found from the
program's fixups. Most opcodes have a fixed length; the others have a
string or a sub-type byte:

| Opcode | Operands |
|---|---|
| 0x0007 | x, y, a picture file, a word |
| 0x000A | x, y, a text, four words |
| 0x012C | a sub-type: 1 and 0x12 a file and two words, 2 and 0x13 three words, 4 a file, 0x0F none |
| 0x0190 | a clip: a sub-type; 1, 6, 7 and 8 a file, x, y, four words and two bytes; 2 to 5, 9 and 10 none |
| 0x01F5 | a sound: a word, a sub-type, then a file (1) or a word (3) |
| 0x0352 | MIDI: a word, a sub-type, then a file (1) or none (6, 9, 10) |
| 0x07D0 | two words, a byte, a cursor file |
| 0x07E4, 0x07E5 | a text sprite: the sprite slot, x, y, z, two words, the font, red, green, blue, a word, the text |
| 0x012E | a screen update: an effect, then 0x7FFE and a rectangle, 0x7FFF (the whole screen) or a sequence (its sprite's place), then a word |
| 0x0810 | a sub-type; 1: a file and three words |
| 0x0821 | a byte, x, y, a picture file |
| 0x00E6 | a switch on an operand over case records: 0xE7 low, high, target; 0xE8 target; 0xE9 the end |

Coordinates above 640 (x) or 480 (y) are variable operands. Opcode 0
stops a sequence; 0x101 jumps through a table of 13 targets that 0x100
sets.

The decoder reads 62,572 instructions, 93% of the sequence segments'
bytes; all but 69 of their 15,262 far pointers fall in an instruction.
The rest are tables of targets that 0x100 and 0x101 use.

## Drawing

The program keeps two images: the pictures (a back buffer that 0x12C
draws into) and the screen. Texts are drawn on the screen only, so a
screen update (0x12E) that copies a rectangle of the pictures back
erases the texts there.

Sprites are slots (DS:46DC, 14 bytes each): 0 to 0x5C are sprites,
0x5D to 0x7C cursors. 0x7D0 loads a file of bitmaps, one after the
other (Video 1, 16-bit), into the slots from the one it names; 0x7E4
makes a sprite of a text. Each sequence can show one sprite (context
+6) at its x, y (+0xA, +0xC) when bit 0 of its flags (+0x26) is set;
before each instruction, the sequence's width and height (+0x10,
+0x12) become its sprite's. Sprites are drawn the lowest z first (seg3:1790). The drawing mode
(+0x28) makes a colour (RGB555) clear: black for 0, 3, 4, 7 and 8,
white for 1, 5 and 6, none for 2. Modes 3 to 6 draw the other pixels in
one colour, white (3, 5) or red (4, 6). Mode 8 moves each pixel a third
of the way to the colour of the dominant mood (see the uber device).

Clips are windows over everything. 0x194 makes the sequence's place
its clip's window; 0x195 waits until the clip reaches a time (1/30
second).

## Screen updates

The screen updates (0x12E to 0x138) are queued during a tick and carried
out after it (seg7:33B8, 35EA). Each copies a rectangle of the pictures
to the screen with an effect: wipes from a side (0x12E effects 1 to 4),
an iris (5), a box opening (6) or closing (7), a dissolve by 4x4 cells
(0x130), lines (0x135), a clock sweep (0x136) or a swirl (0x138). None
of them reads a clock: the original's speed was the machine's, and the
game waits until the effect ends. The module plays each in about half a
second; the swirl (0x138) is shown as a dissolve.

0x803 saves a rectangle of the screen and the pictures and 0x804 puts it
back. 0x810 draws a bitmap file into the pictures with black clear; 0x821
draws one onto the screen only, so the next update there takes it away.
0x815 moves the screen's pixels in a rectangle part of the way to a
colour, every other one in a stipple.

0xEB and 0xEE call a target when a variable is not 0 or not a value;
0xFC drops the last return address. 0x825 goes to a target keeping where
to come back in one of four places, and 0x826 comes back.

## Pointer

When the pointer moves (seg3:2F0E), each running sequence it leaves or
enters gets an event (0x20000, 0x10000) and, when +0x120 is set, its
sprite becomes +0x124 or +0x122 (a highlight). The cursor is that of
the sequence under the pointer with the lowest z (+0x2C, set by
0x2BC), over a clip the cross, else the default (0x2BC with 0x0C).
Cursors are the resources 0xFA2 (wait) and 0xFA3 (a small cross), or a
cursor slot: an image, then its mask in the next slot, drawn from the
pointer.

## Choices and captions

0x100 gives twelve targets (1 to 12) and a current one; 0x101 jumps to
target n. 0xFD gives a far pointer beside each target, the text of a
dialogue choice. 0xFE moves a target and its text to another number, or
empties it, and 0x102 closes the gaps in the first six, putting the
current one last of them (segment 22). The dialogue box shows choice n
with 0xD, which names the text by its place in the data segment
(DS:50A4 + 4n). 0xD draws the text, or makes it the sequence's sprite,
or both: then the sprite stays hidden until 0x80C shows the sprites of a
z range again, which brings the text back after the box was hidden.

Texts are drawn on the screen over the sprites. Where the screen is
drawn again (a screen update, a sprite that moves, shows or hides), the
text there is gone.

0x816 and 0x817 give a sequence a caption, a string resource that shows
in light green above the sequence (below it at the top of the screen)
while the pointer is over it; 0x818 gives another sequence one, with a
place and a colour. Only sequences with hover sprites (0x7D1) show
theirs.

0x807 to 0x80C take a sequence's cursor or sprite away and give it
back, one sequence or those whose z is in a range.

## Discs and music

The game is on three discs, VORTEX_1 to VORTEX_3, each with its days'
folders (`c\1a`, `c\2a`, `c\3a`). 0xFF with 9 asks which disc is in;
a scene on another disc shows "insert disc" until it is. A game folder
can hold the discs' contents as folders `VORTEX_1` to `VORTEX_3`; the
disc in the drive is read first, and the game changes discs when a
scene asks.

0x800 plays the adaptive score: `system\midifile.txt` (read by
`midi.c`) gives its size (49 scenes, 2 variants, 4 moods: balance,
anger, secret, psycho), the MIDI files, then for each cell a song and
how many times it plays.

## The uber device

Segment 13 (`uber.c`) keeps the player's moods and a log of events
(`src/games/vortex/vuber.c`). There are three moods, 0 to 2. Each keeps
the values recorded for it (0 to 255), their total and how many were
above 0. Opcode 0x805 drives it by a sub-command at +4:

| Sub | Does |
|---|---|
| 1, 2 | Start (with 24 built-in events), free |
| 3 | Records a value for a mood (0xFF: none) and an event: an id and seven words, string resources and a rectangle the scripts use |
| 4 | Whether a mood's share of the values reaches a percentage (mode 1 the newest value, 2 the newest n, 3 the totals, 4 the newest n counted twice) |
| 5 | Whether two moods' totals are within a percentage of the sum |
| 6 | The mood with the most, by the same modes; a tie goes to the mood with more values above 0 |
| 7, 8 | The default mood |
| 9 to 15, 23 | The event log: its size, a cursor and the ids and moods at it, an event marked as seen |

Events with ids from a table of 169 are logged once. 0x806 gives a
mood's colour at a level 0 to 3: green for mood 0, blue for 1, red for
2, purple when 1 and 2 are within 20% of each other.

The log is shown as a book of two pages (0x81A opens it, 0x81B closes
it, 0x81D turns a page and gives the events on both sides). 0x81E draws
a bitmap's black in a colour and keeps the result in the left or right
page's panel; 0x198 sub 0x11 shows a panel again. 0x824 dissolves a
bitmap onto the screen; 0x9 draws a string resource word-wrapped in a
240 by 400 box.

## Status

`src/games/vortex` runs every opcode the sequences hold: the menus, GO
TO SCENE, the three days on their discs, the dialogue box and its
choices, captions, idea icons, the uber device and its journal, the
score and the screen effects. Not done yet: the swirl effect (a
dissolve for now), and a play-through of every scene to the endings.
