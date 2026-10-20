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
+0x12) become its sprite's. Sprites are drawn the lowest z (+0xE)
first, with a clear colour (RGB555) by the drawing mode (+0x28):
black for 0, 3, 4 and 7, white for 1, 5 and 6, none for 2; mode 8
tints the sprite.

Clips are windows over everything. 0x194 makes the sequence's place
its clip's window; 0x195 waits until the clip reaches a time (1/30
second).

## Pointer

When the pointer moves (seg3:2F0E), each running sequence it leaves or
enters gets an event (0x20000, 0x10000) and, when +0x120 is set, its
sprite becomes +0x124 or +0x122 (a highlight). The cursor is that of
the sequence under the pointer with the lowest z (+0x2C, set by
0x2BC), over a clip the cross, else the default (0x2BC with 0x0C).
Cursors are the resources 0xFA2 (wait) and 0xFA3 (a small cross), or a
cursor slot: an image, then its mask in the next slot, drawn from the
pointer.

## Discs and music

The game is on three discs, VORTEX_1 to VORTEX_3, each with its days'
folders (`c\1a`, `c\2a`, `c\3a`). 0xFF with 9 asks which disc is in;
a scene on another disc shows "insert disc" until it is.

0x800 plays the adaptive score: `system\midifile.txt` (read by
`midi.c`) gives its size (49 scenes, 2 variants, 4 moods: balance,
anger, secret, psycho), the MIDI files, then for each cell a song and
how many times it plays.

## Status

`src/games/vortex` runs the opening, the main menu, GO TO SCENE and
the Day 3 scenes that the disc-3 copy holds: clips, pictures, the
dialogue box and its choices, hover cursors and the score. Missing:
screen update effects, sprite tints, the journal and the "uber"
device (0x805, segment 13), and the opcodes the scenes have not
reached yet.
