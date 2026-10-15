# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 xesf and contributors
"""Prints the object graph below an object, decoding fields with the
layouts from hdb_layouts.py.

usage: python3 -I tools/re/hdb_tree.py XFILES.HDB <id> [depth]
"""
import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
src = open(__file__.rsplit("/", 1)[0] + "/hdb_layouts.py").read()
pre = src.split("want = ")[0]
exec(pre)  # defines LAYOUTS, d, u16, u32, records

NAMES = {cid: n for cid, (n, _) in LAYOUTS.items()}


def list_ids(root):
    out, todo = [], [root]
    while todo:
        n = todo.pop(0)
        if not n:
            continue
        c, leaf = u16(n + 6), d[n] & 0x80
        vals = [u32(n + 8 + i * 4) for i in range(c)]
        if leaf:
            out += vals
        else:
            todo = vals + todo
    return out


def text(mark, n):
    if not mark or mark + n > len(d) or n > 300:
        return None
    raw = d[mark:mark + n].rstrip(b"\0")
    if all(32 <= b < 127 or b in (9, 10, 13) or b >= 0x80 for b in raw):
        return raw.decode("latin-1")
    return raw.hex()


def fields(oid):
    cid, off = records[oid]
    name, layout = LAYOUTS.get(cid, ("?", ""))
    at, out, kids = off + 6, [], []
    for code in layout:
        if code == "B":
            out.append(str(d[at])); at += 1
        elif code == "S":
            out.append(str(struct.unpack_from(">h", d, at)[0])); at += 2
        elif code in "DI":
            v = u32(at); at += 4
            if v in records:
                out.append(f"#{v}"); kids.append(v)
            else:
                out.append(str(v))
        elif code == "T":
            t = text(u32(at), u32(at + 4)); at += 8
            out.append(repr(t) if t is not None else "-")
        elif code == "L":
            ids = list_ids(u32(at + 4)); at += 26
            out.append(f"[{len(ids)}]"); kids += ids
        elif code == "R":
            c = u32(at); l, t, r, b = struct.unpack_from(">4H", d, at + 4); at += 12
            out.append(f"cursor#{c} ({l},{t})-({r},{b})")
        elif code == "P":
            a, b = struct.unpack_from(">hh", d, at); v = u32(at + 4); at += 8
            out.append(f"({a},{b}) #{v}")
    return name, out, kids


def show(oid, depth, maxd, seen):
    if oid not in records or oid in seen:
        return
    seen.add(oid)
    name, out, kids = fields(oid)
    print(f"{'  ' * depth}#{oid} {name}: {' '.join(out)}")
    if depth < maxd:
        for k in kids:
            show(k, depth + 1, maxd, seen)


show(int(sys.argv[2]), 0, int(sys.argv[3]) if len(sys.argv) > 3 else 3, set())
