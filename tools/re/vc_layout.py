# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 xesf
"""Prints an interface layout: its items, their regions, pictures and
what each trigger kind does.

usage: python3 -I tools/re/vc_layout.py XFILES.HDB <layout>...
"""
import bisect
import collections
import os
import re
import struct
import sys

here = os.path.dirname(os.path.abspath(__file__))
code = open(os.path.join(here, "hdb_layouts.py")).read().split("want = ")[0]
env = {"sys": sys, "struct": struct, "collections": collections, "bisect": bisect}
exec(code, env)
d, rec, u32, u16 = env["d"], env["records"], env["u32"], env["u16"]
layouts = [int(x) for x in sys.argv[2:]]


def ids(root):
    out, todo = [], [root]
    while todo:
        n = todo.pop(0)
        if not n:
            continue
        c, leaf = u16(n + 6), d[n] & 0x80
        v = [u32(n + 8 + i * 4) for i in range(c)]
        if leaf:
            out += v
        else:
            todo = v + todo
    return out


def lst(i):
    o = rec.get(i, (0, 0))[1]
    return ids(u32(o + 10)) if o and rec[i][0] in (0x36, 0x42, 0x4E, 0x52) else []


def name(n):
    o = rec.get(n, (0, 0))[1]
    if not o or rec[n][0] != 0x2F:
        return "?"
    m, k = u32(o + 6), u32(o + 10)
    return d[m:m + k].split(b"\0")[0].decode("latin1")


def asset(a):
    o = rec.get(a, (0, 0))[1]
    if not o:
        return "?"
    m, n = u32(o + 6), u32(o + 10)
    s = re.sub(rb"[\x00-\x1f\x7f]+", b"|", d[m:m + n]).decode("latin1")
    parts = [p for p in s.split("|") if p]
    return parts[0][2:].split("/")[-1] if parts and parts[0][:2].isdigit() else s[:40]


def action(a):
    o = rec.get(a, (0, 0))[1]
    if rec.get(a, (0,))[0] != 0x41 or not o:
        return "?"
    m, n, t = u32(o + 6), u32(o + 10), d[o + 14]
    pre = 12 if t & 0x80 else 0
    b = d[m + pre:m + n]
    t &= 0x7F
    cond = "if " if pre else ""
    if t == 0:
        return cond + "expr"
    if t == 1 and len(b) >= 4:
        l = lst(struct.unpack(">I", b[:4])[0])
        return cond + "play " + (asset(l[0]) if l else "?")
    if t == 2:
        return cond + "timer"
    if t == 3 and len(b) >= 9:
        return cond + ("enable" if b[8] else "disable") + " #%d" % struct.unpack(">I", b[:4])[0]
    if t == 4 and len(b) >= 4:
        return cond + "view %d" % struct.unpack(">I", b[:4])[0]
    if t == 5 and len(b) >= 5:
        return cond + ("open" if b[4] == 0 else "close") + " %d" % struct.unpack(">I", b[:4])[0]
    if t == 6:
        argc = b[15]
        args = [struct.unpack(">i", b[16 + 4 * i:20 + 4 * i])[0] for i in range(argc)]
        kinds = list(b[16 + 4 * argc:16 + 5 * argc])
        txt = ", ".join(("var%d" % v if k == 0 else "event" if k == 4 else repr(chr(v)) if k == 3 else str(v))
                        for v, k in zip(args, kinds))
        return cond + "%s(%s)" % (b[:15].split(b"\0")[0].decode("latin1"), txt)
    return cond + "type %d" % t


for lay in layouts:
    o = rec[lay][1]
    print("layout %d  %s" % (lay, name(u32(o + 36))))
    for it in lst(lay):
        io = rec[it][1]
        cur = u32(io + 6)
        r = struct.unpack(">4h", d[io + 10:io + 18])
        pics, tl, flags = u32(io + 18), u32(io + 22), u16(io + 26)
        pic = ", ".join(asset(a) for a in lst(pics)[:2])
        print("  item %d (%d,%d)-(%d,%d) order %d cursor %d  %s" % ((it,) + r + (flags & 0xFF, cur, pic)))
        for t in lst(tl):
            to = rec[t][1]
            acts = [action(a) for a in lst(u32(to + 6))]
            print("    kind %d: %s" % (d[to + 10], "; ".join(acts[:8]) + (" ..." if len(acts) > 8 else "")))
