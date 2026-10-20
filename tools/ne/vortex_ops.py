#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 xesf
"""Maps Vortex's sequence opcodes to their handlers in VORTEX.EXE.

The interpreter ("sequence.c", segment 8) reads the instruction's first
word, clears its top bit, and branches through a compiler-made tree of
compares and jump tables starting at seg8:6B42. This walks that tree for
every opcode value and prints opcode -> handler offset.

  vortex_ops.py <VORTEX.EXE>
"""
import os
import struct
import sys

import capstone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ne as N  # noqa: E402

SEG, SWITCH, DEFAULT = 8, 0x6B42, 0x0A02


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    e = N.NE(sys.argv[1])
    code = e.seg_data(SEG)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)
    cache = {}

    def at(a):
        if a not in cache:
            cache[a] = next(md.disasm(code[a:a + 16], a))
        return cache[a]

    def imm(s):
        return int(s.split(',')[-1].strip(), 16)

    def handler(op):
        ax, flags, pc = op, (0, 0), SWITCH
        for _ in range(200):
            i = at(pc)
            m, o, nxt = i.mnemonic, i.op_str, pc + i.size
            if m == 'cmp' and o.startswith('ax,'):
                flags = (ax, imm(o))
            elif m == 'sub' and o.startswith('ax,'):
                flags = (ax, imm(o))
                ax = (ax - imm(o)) & 0xFFFF
            elif m == 'dec' and o == 'ax':
                flags = (ax, 1)
                ax = (ax - 1) & 0xFFFF
            elif m == 'add' and o == 'ax, ax':
                ax = (ax * 2) & 0xFFFF
            elif m in ('xchg', 'nop'):
                pass
            elif m == 'jmp' and 'cs:[bx' in o:
                return struct.unpack_from('<H', code, int(o.split('+')[1].strip(' ]'), 16) + ax)[0]
            elif m.startswith('j'):
                a, b = flags
                taken = {'jmp': True, 'jne': a != b, 'je': a == b, 'ja': a > b, 'jb': a < b,
                         'jae': a >= b, 'jbe': a <= b}[m]
                nxt = int(o, 16) if taken else nxt
                if nxt == DEFAULT:
                    return None
            else:
                return pc
            pc = nxt
        return None

    for op in range(0x8000):
        h = handler(op)
        if h is not None and h != DEFAULT:
            print('%04X seg%d:%04X' % (op, SEG, h))
    return 0


if __name__ == '__main__':
    sys.exit(main())
