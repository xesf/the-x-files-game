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

## Status

The opcodes' operand layouts and meanings are being read from their
handlers. Next: a decoder that lists the sequences, checked against the
fixups (a far pointer must fall where the program fixes up a segment),
then a module that runs them.
