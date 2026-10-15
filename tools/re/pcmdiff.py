# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 xesf
"""Compares a WAV file from xftool with raw s16le from ffmpeg:
python3 -I tools/re/pcmdiff.py ours.wav ref.raw"""
import struct
import sys

a = open(sys.argv[1], "rb").read()[44:]
b = open(sys.argv[2], "rb").read()
n = min(len(a), len(b)) // 2
sa = struct.unpack("<%dh" % n, a[:n * 2])
sb = struct.unpack("<%dh" % n, b[:n * 2])
d = [abs(x - y) for x, y in zip(sa, sb)]
print(f"samples ours {len(a) // 2} ref {len(b) // 2}, max diff {max(d) if d else 0}, "
      f"differing {sum(1 for x in d if x)}")
