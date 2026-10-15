# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 xesf and contributors
"""Checks class field layouts against every object in an HDB file.

Field codes: B = u8, S = u16, D = u32, I = object id (u32), T = blob
(offset, length), L = embedded id list (26 bytes), R = region (cursor id
and rectangle, 12 bytes), P = two signed 16-bit values and an object id
(8 bytes).

usage: python3 -I tools/re/hdb_layouts.py XFILES.HDB [class_id]
"""
import bisect
import collections
import struct
import sys

LAYOUTS = {
    0x27: ("VCTitle", "LTDTTDDSS"),
    0x28: ("VCNode", "LDD"),
    0x29: ("VCLocaton", "LDDDB"),
    0x2A: ("VCViewPoint", "LPDD"),
    0x2B: ("VCView", "DDDDDD"),
    0x2C: ("VCCharacter", "D"),
    0x2D: ("VCCharView", "DRDDDD"),
    0x2E: ("VCCharViewList", "L"),
    0x2F: ("VCName", "TT"),
    0x31: ("VCConversation", "DDDD"),
    0x32: ("VCConversationList", "L"),
    0x33: ("VCNav", "DDD"),
    0x34: ("VCNav_List", "L"),
    0x35: ("VCAssetRef", "TDDBB"),
    0x36: ("VCAssetRefList", "L"),
    0x37: ("VCExplorable", "RD"),
    0x38: ("VCExplorableList", "L"),
    0x39: ("VCHotSpot", "RD"),
    0x3A: ("VCHotSpotList", "L"),
    0x3B: ("VCStdHotSpotList", "L"),
    0x3C: ("VCAssetCategory", "DB"),
    0x3D: ("VCCursor", "TDD"),
    0x3E: ("VCDefaultCursors", "DDD"),
    0x40: ("VCDefaultHotSpots", "DDD"),
    0x41: ("VCAction", "TB"),
    0x42: ("VCActionList", "L"),
    0x43: ("VCBlob", "T"),
    0x44: ("VCPlayer", "D"),
    0x46: ("VCEnabled", "DBB"),
    0x47: ("VCActionIcon", "DDDDDD"),
    0x48: ("VCEmotionIcon", "DDDDDD"),
    0x49: ("VCEvidenceIcon", "DDDDDD"),
    0x4A: ("VCIdeaIcon", "DDDDDD"),
    0x4B: ("VCInventory", "DDDDDD"),
    0x4C: ("VCIdeaMap", "DD"),
    0x4D: ("VCIdeaMapList", "L"),
    0x4E: ("VCIFaceLayout", "LDB"),
    0x4F: ("VCInterfaceItem", "RDDS"),
    0x50: ("VCString", "T"),
    0x51: ("VCTrigger", "DB"),
    0x52: ("VCTriggerList", "L"),
    0x53: ("VCVariable", "TDDBB"),
    0x54: ("VCStdAction", "DDD"),
    0x55: ("VCStdActionList", "L"),
}

d = open(sys.argv[1], "rb").read()
u16 = lambda o: struct.unpack_from(">H", d, o)[0]
u32 = lambda o: struct.unpack_from(">I", d, o)[0]

# Walk the index (same as hdb_index.py).
records = {}
todo = [(u32(8), "class", None)]
while todo:
    off, kind, cls = todo.pop()
    if not off:
        continue
    count, leaf, at = u16(off + 6), d[off] & 0x80, off + 8
    if not leaf:
        todo += [(u32(at + i * 4), kind, cls) for i in range(count)]
    elif kind == "class":
        for _ in range(count):
            n = d[at + 10]
            at += 27
            for j in range(n):
                if u32(at) == 7:
                    todo.append((u32(at + 4), "object", u32(at + 8)))
                at += 16
    else:
        for i in range(count):
            records[u32(at + 4)] = (cls, u32(at))
            at += 8
starts = sorted(off for _, off in records.values() if off)


def parse(off, layout):
    """Returns (end offset, problems) for one record."""
    at, problems = off + 6, []
    for code in layout:
        if code == "B":
            at += 1
        elif code == "S":
            at += 2
        elif code in "DI":
            v = u32(at)
            if code == "I" and v and v not in records:
                problems.append(f"id {v} missing")
            at += 4
        elif code == "T":
            mark, n = u32(at), u32(at + 4)
            if mark and (mark + n > len(d) or n > 1 << 20):
                problems.append(f"bad blob {mark:#x}+{n}")
            at += 8
        elif code == "L":
            n, root, typ, tag = u32(at), u32(at + 4), u32(at + 8), d[at + 12:at + 16]
            if typ != 11 or tag not in (b"null", b"ID  "):
                problems.append(f"bad list header {typ} {tag}")
            at += 26
        elif code == "P":
            v = u32(at + 4)
            if v and v not in records:
                problems.append(f"id {v} missing")
            at += 8
        elif code == "R":
            cursor = u32(at)
            l, t, r, b = struct.unpack_from(">4H", d, at + 4)
            if cursor and cursor not in records:
                problems.append(f"cursor {cursor} missing")
            if l > r or t > b or r > 640 or b > 480:
                problems.append(f"bad rect {l},{t},{r},{b}")
            at += 12
    return at, problems


want = int(sys.argv[2], 16) if len(sys.argv) > 2 else None
for cid in sorted(LAYOUTS):
    if want is not None and cid != want:
        continue
    name, layout = LAYOUTS[cid]
    objs = [(i, off) for i, (c, off) in records.items() if c == cid and off]
    bad = collections.Counter()
    overrun = 0
    examples = []
    for oid, off in objs:
        end, problems = parse(off, layout)
        k = bisect.bisect_right(starts, off)
        nxt = starts[k] if k < len(starts) else len(d)
        if end > nxt:
            overrun += 1
            problems.append(f"runs into next record ({end - nxt} bytes)")
        for p in problems:
            bad[p.split(" ")[0] + " " + p.split(" ")[1]] += 1
        if problems and len(examples) < 2:
            examples.append((oid, off, problems[:2]))
    status = "ok" if not bad else ", ".join(f"{k} x{v}" for k, v in bad.most_common(3))
    print(f"0x{cid:02X} {name:20s} {layout:10s} {len(objs):6d} objects: {status}")
    for oid, off, p in examples:
        print(f"      id {oid} at {off:#x}: {p}")
