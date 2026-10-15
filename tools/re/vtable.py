# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 xesf and contributors
"""Prints the entries of a vtable: python3 -I tools/re/vtable.py exe addr [count]"""
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from pe import PE  # noqa: E402

pe = PE(sys.argv[1])
addr = int(sys.argv[2], 16)
count = int(sys.argv[3]) if len(sys.argv) > 3 else 48
text = next(s for s in pe.sections if s[0] == ".text")
for i in range(count):
    v = pe.u32(addr + i * 4)
    if not (text[1] <= v < text[1] + text[2]):
        break
    print(f"[{i:2d}] +0x{i*4:03X}  0x{v:06X}")
