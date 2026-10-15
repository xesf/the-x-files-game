# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 xesf and contributors
"""Minimal PE reader for RE scripts: maps virtual addresses to file bytes."""
import struct


class PE:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        pe = struct.unpack_from("<I", self.data, 0x3C)[0]
        nsec = struct.unpack_from("<H", self.data, pe + 6)[0]
        opt = struct.unpack_from("<H", self.data, pe + 20)[0]
        self.base = struct.unpack_from("<I", self.data, pe + 24 + 28)[0]
        self.sections = []
        sec = pe + 24 + opt
        for i in range(nsec):
            name, vsize, va, rsize, raw = struct.unpack_from("<8sIIII", self.data, sec + i * 40)
            self.sections.append((name.rstrip(b"\0").decode(), self.base + va, vsize, raw, rsize))

    def off(self, va):
        for _, start, vsize, raw, rsize in self.sections:
            if start <= va < start + max(vsize, rsize):
                return raw + (va - start)
        raise ValueError(hex(va))

    def read(self, va, n):
        o = self.off(va)
        return self.data[o:o + n]

    def u32(self, va):
        return struct.unpack("<I", self.read(va, 4))[0]

    def cstr(self, va):
        o = self.off(va)
        return self.data[o:self.data.index(b"\0", o)].decode("latin-1")
