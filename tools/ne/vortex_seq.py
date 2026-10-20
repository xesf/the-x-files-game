#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 xesf
"""Lists Vortex's sequences: the instructions compiled into VORTEX.EXE's
data segments (docs/vortex/README.md).

From the first sequence (seg18:0000) it follows every far pointer an
instruction holds, as a disassembler follows jumps, and prints each
instruction with its operands.

  vortex_seq.py <VORTEX.EXE> [segment]
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ne as N  # noqa: E402

# Fixed lengths in bytes, opcode included. Far pointers inside are found
# from the program's fixups. Variable-length opcodes are in VARIABLE.
LENGTH = {
    0x0001: 8, 0x0002: 6, 0x0003: 4, 0x0005: 8, 0x0006: 2, 0x0008: 20, 0x0009: 15, 0x000B: 2,
    0x000D: 27, 0x000E: 8, 0x0065: 10, 0x00C8: 6, 0x00D0: 8, 0x00D2: 6, 0x00D3: 2, 0x00D5: 6,
    0x00D6: 8, 0x00D7: 6, 0x00D8: 8, 0x00D9: 10, 0x00DA: 6, 0x00DC: 8, 0x00DD: 8, 0x00DE: 10,
    0x00DF: 10, 0x00E0: 10, 0x00E1: 10, 0x00E2: 10, 0x00E3: 10, 0x00E4: 10, 0x00E5: 10,
    0x00EA: 8, 0x00EB: 8, 0x00EC: 10, 0x00ED: 10, 0x00EE: 10, 0x00EF: 10, 0x00F0: 10, 0x00F1: 10,
    0x00F2: 10, 0x00F3: 10, 0x00FC: 2, 0x00FD: 6, 0x00FE: 6, 0x00FF: 6, 0x0100: 54, 0x0102: 2,
    0x012E: 16, 0x012F: 16, 0x0130: 12, 0x0132: 2, 0x0133: 2, 0x0134: 2, 0x0135: 18, 0x0136: 12,
    0x0137: 14, 0x0138: 18, 0x0191: 8, 0x0192: 8, 0x0193: 4, 0x0194: 2, 0x0195: 5, 0x0196: 4, 0x0197: 9,
    0x0198: 25, 0x01F6: 10, 0x01F7: 4, 0x01F8: 3, 0x02BC: 7, 0x038B: 16, 0x038C: 2, 0x038D: 2,
    0x038E: 2, 0x03E8: 6, 0x03E9: 6, 0x03EB: 6, 0x03F2: 6, 0x03F3: 6, 0x03F4: 6, 0x03F5: 6,
    0x03F7: 6, 0x03F8: 6, 0x03FC: 4, 0x0406: 6, 0x0407: 6, 0x0410: 6, 0x0411: 6, 0x0412: 6,
    0x0413: 6, 0x044C: 10, 0x044D: 10, 0x04B0: 6, 0x04B1: 12, 0x04B2: 12, 0x04B3: 12,
    0x04BA: 6, 0x07D1: 8, 0x07D2: 6, 0x07DA: 12, 0x07DB: 10, 0x07DC: 12, 0x07EE: 3,
    0x07FE: 2, 0x07FF: 4, 0x0800: 8, 0x0801: 4, 0x0803: 10, 0x0804: 14, 0x0805: 34, 0x0806: 13,
    0x0807: 4, 0x0808: 4, 0x0809: 4, 0x080A: 4, 0x080B: 8, 0x080C: 8, 0x080D: 4, 0x080E: 4,
    0x080F: 4, 0x0812: 4, 0x0814: 6, 0x0815: 20, 0x0816: 4, 0x0817: 4, 0x0818: 14, 0x081A: 4,
    0x081B: 2, 0x081C: 4, 0x081D: 40, 0x081E: 18, 0x081F: 6, 0x0820: 14, 0x0822: 12,
    0x0823: 4, 0x0824: 12, 0x0825: 8, 0x0826: 4, 0x0827: 12, 0x0828: 15,
    # 0xE6 switches on an operand over the case records that follow it:
    # 0xE7 low, high, target; 0xE8 target (the default); 0xE9 the end.
    0x00E6: 4, 0x00E7: 10, 0x00E8: 6, 0x00E9: 2,
}


# Instructions after which the sequence does not go on: 0 (the
# interpreter's default case, which stops it) and 0x101 (a jump through a
# table).
END = {0x0000, 0x0101}
LENGTH[0x0101] = 4


def cstr(d, p):
    e = d.index(b'\0', p)
    return d[p:e].decode('latin-1'), e + 1


# Opcodes with a sub-type byte after the opcode: per sub-type, the
# operands that follow it (b byte, w word, s zero-terminated string).
# Variable-length opcodes: their operands (b byte, w word, s string).
FMT = {
    0x0007: 'wwsw',        # x, y, a picture file, a word
    0x000A: 'wwswwww',     # x, y, a text, four words
    0x07D0: 'wwbs',        # two operands, a byte, a cursor file
    0x07E4: 'w' * 11 + 's',  # a text box: placement and colours, then the text
    0x07E5: 'w' * 11 + 's',
    0x0821: 'bwws',        # a byte, x, y, a picture file
}

# (operands before the sub-type byte, {sub-type: operands after it})
SUB2 = {
    0x01F5: ('w', {1: 's', 3: 'w'}),     # sound: load a WAVE file, or ...
    0x0352: ('w', {1: 's', 6: '', 9: '', 10: ''}),  # MIDI: play a file, stop, ...
}
SUB = {
    0x012C: {1: 'sww', 2: 'www', 4: 's', 0x0F: '', 0x12: 'sww', 0x13: 'www'},
    # a clip: play a file at a place (1, 6, 7, 8), or act on the one playing
    0x0190: {1: 'swwwwwwbb', 6: 'swwwwwwbb', 7: 'swwwwwwbb', 8: 'swwwwwwbb',
             2: '', 3: '', 4: '', 5: '', 9: '', 10: ''},
    0x0810: {1: 'swww'},
}


def fmt_len(fmt, d, p):
    for c in fmt:
        if c == 'b':
            p += 1
        elif c == 'w':
            p += 2
        elif c == 's':
            p = d.index(b'\0', p) + 1
    return p


def var_len(op, d, p):
    """Length of a variable-length instruction at p, or None."""
    if op in SUB:
        f = SUB[op].get(d[p + 2])
        return None if f is None else fmt_len(f, d, p + 3) - p
    if op in SUB2:
        pre, subs = SUB2[op]
        q = fmt_len(pre, d, p + 2)
        f = subs.get(d[q])
        return None if f is None else fmt_len(f, d, q + 1) - p
    if op in FMT:
        return fmt_len(FMT[op], d, p + 2) - p
    return None


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    e = N.NE(sys.argv[1])
    segs = {}
    relocs = {}

    def seg(n):
        if n not in segs:
            segs[n] = e.seg_data(n)
            relocs[n] = {o - 2 for o, (src, _) in e.relocs(n).items() if src == 2}
        return segs[n]

    todo = [(18, 0)]
    done = {}
    unknown = {}
    while todo:
        s, p = todo.pop()
        d = seg(s)
        while (s, p) not in done and p + 2 <= len(d):
            w = struct.unpack_from('<H', d, p)[0]
            op = w & 0x7FFF
            if op in END:
                done[(s, p)] = (op, w, LENGTH.get(op, 2))
                break
            n = LENGTH.get(op) or var_len(op, d, p)
            if not n:
                unknown.setdefault(op, []).append('seg%d:%04X' % (s, p))
                break
            done[(s, p)] = (op, w, n)
            for k in range(p, p + n):
                if k in relocs[s]:
                    off, tseg = struct.unpack_from('<H', d, k)[0], e.relocs(s)[k + 2][1]
                    todo.append((int(tseg[3:].split(':')[0]), off))
            p += n
    want = int(sys.argv[2]) if len(sys.argv) > 2 else None
    for (s, p), (op, w, n) in sorted(done.items()):
        if want and s != want:
            continue
        d = segs[s]
        print('seg%d:%04X  %s%04X  %s' % (s, p, '*' if w & 0x8000 else ' ', op, d[p + 2:p + n].hex(' ')))
    for op, where in sorted(unknown.items()):
        print('unknown %04X at %s' % (op, ' '.join(where[:6])), file=sys.stderr)
    print('%d instructions' % len(done), file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
