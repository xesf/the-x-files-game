# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 xesf and contributors
"""Finds each VC class's vtable and read method, and lists the stream
calls the read method makes (stream vtable offset and field tag).

The factory stores the final vtable with `mov dword ptr [esi], offset X`
(C7 06 imm32) just before it returns. Slot 12 of the vtable reads the
object from a stream. Stream calls look like `push tag; mov ecx, edi;
call dword ptr [esi+off]`.

usage: python3 -I tools/re/vc_readers.py ida/xfiles.exe
"""
import struct
import subprocess
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from pe import PE  # noqa: E402

READ_SLOT = 12


def classes(exe):
    out = subprocess.run([sys.executable, "-I", __file__.rsplit("/", 1)[0] + "/vc_classes.py", exe],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        p = line.split()
        yield int(p[0], 16), p[1], int(p[2].split("=")[1], 16)


def final_vtable(pe, factory):
    code = pe.read(factory, 0x600)
    vt = None
    for i in range(len(code) - 6):
        if code[i] == 0xC7 and code[i + 1] == 0x06:  # mov dword ptr [esi], imm32
            vt = struct.unpack_from("<I", code, i + 2)[0]
        if code[i] == 0xC3 or code[i:i + 1] == b"\xC2":  # ret
            if vt:
                return vt
    return vt


def stream_calls(pe, fn):
    """Yields (kind, value) in order: ('base', addr) or ('call', off, tag)."""
    code = pe.read(fn, 0x400)
    tag = None
    i = 0
    while i < len(code):
        b = code[i]
        if b == 0x6A:  # push imm8
            tag = code[i + 1]
            i += 2
            continue
        if b == 0x68:  # push imm32
            tag = struct.unpack_from("<I", code, i + 1)[0]
            i += 5
            continue
        if b == 0xFF and code[i + 1] in (0x96, 0x97, 0x90, 0x91, 0x92, 0x93, 0x95):  # call [reg+disp32]
            off = struct.unpack_from("<I", code, i + 2)[0]
            yield ("call", off, tag)
            tag = None
            i += 6
            continue
        if b == 0xFF and code[i + 1] in (0x56, 0x57, 0x50, 0x51, 0x52, 0x53, 0x55):  # call [reg+disp8]
            yield ("call", code[i + 2], tag)
            tag = None
            i += 3
            continue
        if b == 0xE8:
            target = fn + i + 5 + struct.unpack_from("<i", code, i + 1)[0]
            yield ("direct", target, tag)
            i += 5
            continue
        if b in (0xC3, 0xC2):
            return
        i += 1


def main():
    pe = PE(sys.argv[1])
    for cid, name, factory in sorted(classes(sys.argv[1])):
        vt = final_vtable(pe, factory)
        read = pe.u32(vt + READ_SLOT * 4) if vt else 0
        print(f"0x{cid:02X} {name:22s} vtable=0x{vt or 0:06X} read=0x{read:06X}")
        if not read:
            continue
        for kind, val, tag in stream_calls(pe, read):
            t = "" if tag is None else (repr(chr(tag)) if 32 <= tag < 127 else hex(tag))
            if kind == "call":
                print(f"     stream+0x{val:02X} {t}")
            else:
                print(f"     call 0x{val:06X} {t}")


main()
