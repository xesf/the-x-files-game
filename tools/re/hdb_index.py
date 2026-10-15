# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 xesf and contributors
"""Walks the NeoAccess index of an HDB file and lists objects per class.

usage: python3 -I tools/re/hdb_index.py XFILES.HDB [class_id]
"""
import collections
import struct
import sys

d = open(sys.argv[1], "rb").read()
u16 = lambda o: struct.unpack_from(">H", d, o)[0]
u32 = lambda o: struct.unpack_from(">I", d, o)[0]

assert u32(0) == 5 and u32(20) == 0x501 and u32(28) == 256, "unexpected header"
records = []
bad = []
classes = {}
todo = [(u32(8), "class", None)]
seen = set()
while todo:
    off, kind, cls = todo.pop()
    if off in seen:
        continue
    seen.add(off)
    if off + 8 > len(d):
        bad.append((kind, cls, off))
        continue
    count, leaf = u16(off + 6), d[off] & 0x80
    at = off + 8
    if count > 255 or at + count * 8 > len(d):
        bad.append((kind, cls, off))
        continue
    if not leaf:
        todo += [(u32(at + i * 4), kind, cls) for i in range(count)]
    elif kind == "class":
        for _ in range(count):
            n = d[at + 10]
            entry = d[at:at + 27]
            at += 27
            for j in range(n):
                itype, root, cid, extra = struct.unpack_from(">IIII", d, at)
                classes.setdefault(cid, []).append((itype, root, extra, entry.hex()))
                if itype == 7:
                    todo.append((root, "object", cid))
                at += 16
    else:
        for i in range(count):
            records.append((cls, u32(at + 4), u32(at)))
            at += 8

per = collections.Counter(r[0] for r in records)
ids = collections.Counter(r[1] for r in records)
dups = [i for i, n in ids.items() if n > 1]
print(f"{len(ids)} distinct ids, {len(dups)} shared by more than one entry"
      f"{', e.g. ' + str(dups[:5]) if dups else ''}")
offs = collections.Counter(r[2] for r in records if r[2])
print(f"{sum(1 for n in offs.values() if n > 1)} offsets used by more than one entry")
for kind, cls, off in bad[:5]:
    print(f"skipped bad {kind} node at 0x{off:X} (class 0x{cls or 0:02X})")
print(f"{len(bad)} bad nodes")
print(f"{len(records)} objects in {len(per)} classes, {len(seen)} index nodes")
for cid in sorted(classes):
    print(f"class 0x{cid:02X}: {per.get(cid, 0):5d} objects, indexes {[(t, hex(r)) for t, r, _, _ in classes[cid]]}")
if len(sys.argv) > 2:
    want = int(sys.argv[2], 16)
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 48
    for cls, oid, off in sorted(r for r in records if r[0] == want)[:6]:
        raw = d[off:off + n]
        txt = "".join(chr(b) if 32 <= b < 127 else "." for b in raw)
        print(f"  id {oid:6d} at 0x{off:06X}: {raw.hex(' ')}  {txt}")
