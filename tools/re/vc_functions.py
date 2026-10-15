# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 xesf
"""Lists the built-in functions that actions call by name, with the
address of each handler.

The table at 0x6BD820 has 20-byte entries: a name string object (16
bytes) and the handler address at +0x10. Startup code fills it in two
ways:

- `push name; push entry; call set_string`, and later
  `mov dword ptr [entry+0x10], offset handler`;
- `push handler; mov ecx, entry; push name; call init`, or the same with
  the name pushed before the entry is loaded.

The script collects (entry, name) and (entry, handler) pairs and joins
them on the entry address.

usage: python3 -I tools/re/vc_functions.py ida/xfiles.exe
"""
import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from pe import PE  # noqa: E402

START, END = 0x472310, 0x472310 + 0x2086
TABLE_LO, TABLE_HI = 0x6BD800, 0x6BF000


def string_at(pe, va):
    try:
        s = pe.cstr(va)
    except (ValueError, IndexError):
        return None
    return s if 2 < len(s) < 32 and s.isidentifier() else None


def main():
    pe = PE(sys.argv[1])
    code = pe.read(START, END - START)
    names, handlers = {}, {}
    u32 = lambda i: struct.unpack_from("<I", code, i)[0]
    for i in range(len(code) - 16):
        if code[i] == 0x68 and code[i + 5] == 0xB9 and code[i + 10] == 0x68 and code[i + 15] == 0xE8:
            n = string_at(pe, u32(i + 11))
            if n:
                names[u32(i + 6)] = n
                handlers[u32(i + 6)] = u32(i + 1)
        elif code[i] == 0x68 and code[i + 5] == 0x68 and code[i + 10] == 0xB9 and code[i + 15] == 0xE8 \
                and string_at(pe, u32(i + 6)):
            # push handler; push name; mov ecx, entry; call init
            names[u32(i + 11)] = string_at(pe, u32(i + 6))
            handlers[u32(i + 11)] = u32(i + 1)
        elif code[i] == 0x68 and string_at(pe, u32(i + 1)):
            # The entry push follows within a few instructions.
            for j in range(i + 5, min(i + 32, len(code) - 5)):
                if code[j] == 0x68 and TABLE_LO <= u32(j + 1) < TABLE_HI:
                    names[u32(j + 1)] = string_at(pe, u32(i + 1))
                    break
        elif code[i] == 0xC7 and code[i + 1] == 0x05:
            handlers[u32(i + 2) - 0x10] = u32(i + 6)
    rows = sorted((names[e], handlers.get(e, 0), e) for e in names)
    for n, h, e in rows:
        print(f"0x{h:06X} {n:16s} entry 0x{e:06X}")
    print(f"{len(rows)} functions, {sum(1 for r in rows if not r[1])} without handler", file=sys.stderr)


main()
