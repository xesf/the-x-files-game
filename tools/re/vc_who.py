# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 xesf
"""Finds the actions that read or write a GAM variable or enable state,
and what runs them: the trigger list, its kind and its owner object.

usage: python3 -I tools/re/vc_who.py XFILES.HDB var <id>
       python3 -I tools/re/vc_who.py XFILES.HDB enable <gam id>
       python3 -I tools/re/vc_who.py XFILES.HDB function <name>
       python3 -I tools/re/vc_who.py XFILES.HDB layout <id>
       python3 -I tools/re/vc_who.py XFILES.HDB media <list id>
       python3 -I tools/re/vc_who.py XFILES.HDB functions
"""
import bisect
import collections
import os
import struct
import sys

here = os.path.dirname(os.path.abspath(__file__))
code = open(os.path.join(here, "hdb_layouts.py")).read().split("want = ")[0]
env = {"sys": sys, "struct": struct, "collections": collections, "bisect": bisect}
exec(code, env)
d, rec, u32, u16, LAY = env["d"], env["records"], env["u32"], env["u16"], env["LAYOUTS"]


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


lists = {}
for i, (c, o) in rec.items():
    if c in (0x42, 0x52) and o:
        for x in ids(u32(o + 10)):
            lists.setdefault(x, []).append(i)
trig_of_list = {}
for i, (c, o) in rec.items():
    if c == 0x51 and o:
        trig_of_list.setdefault(u32(o + 6), []).append((i, d[o + 10]))
# owners of trigger lists: any record whose fields hold the list id
owner = {}
for i, (c, o) in rec.items():
    if not o or c in (0x42, 0x52, 0x51, 0x41):
        continue
    raw = d[o + 6:o + 6 + 64]
    for k in range(0, len(raw) - 3):
        owner.setdefault(struct.unpack(">I", raw[k:k + 4])[0], []).append((i, LAY.get(c, ("0x%x" % c,))[0]))


def operands(b, at):
    return [(b[at + 8], struct.unpack(">I", b[at:at + 4])[0]), (b[at + 9], struct.unpack(">I", b[at + 4:at + 8])[0])]


mode = sys.argv[2]
if mode == "functions":
    # Every engine function the actions call, with how many actions call it.
    count = collections.Counter()
    for a, (c, o) in rec.items():
        if c == 0x41 and o and d[o + 14] & 0x7F == 6:
            m, n, t = u32(o + 6), u32(o + 10), d[o + 14]
            pre = 12 if t & 0x80 else 0
            count[d[m + pre:m + pre + 15].split(b"\0")[0].decode("latin1")] += 1
    for name, k in sorted(count.items()):
        print("%5d %s" % (k, name))
    sys.exit(0)
want = int(sys.argv[3]) if sys.argv[3].isdigit() else 0
for a, (c, o) in sorted(rec.items()):
    if c != 0x41 or not o:
        continue
    m, n, t = u32(o + 6), u32(o + 10), d[o + 14]
    b = d[m:m + n]
    pre = 12 if t & 0x80 else 0
    hit = False
    if mode == "var":
        exprs = ([0] if pre else []) + ([pre] if t & 0x7F == 0 else [])
        for at in exprs:
            for kind, v in operands(b, at):
                hit |= kind == 0 and v == want
    elif mode == "enable" and t & 0x7F == 3 and len(b) >= pre + 9:
        hit = struct.unpack(">I", b[pre:pre + 4])[0] == want
    elif mode == "function" and t & 0x7F == 6:
        hit = b[pre:pre + 15].split(b"\0")[0].decode("latin1") == sys.argv[3]
    elif mode == "media" and t & 0x7F == 1 and len(b) >= pre + 4:
        hit = struct.unpack(">I", b[pre:pre + 4])[0] == want
    elif mode == "layout" and t & 0x7F == 5 and len(b) >= pre + 5:
        hit = struct.unpack(">I", b[pre:pre + 4])[0] == want
    if not hit:
        continue
    for l in lists.get(a, []):
        if not trig_of_list.get(l):
            print("action %d in list %d (no trigger runs this list)" % (a, l))
        for tr, kind in trig_of_list.get(l, []):
            for tl in lists.get(tr, []):
                print("action %d in list %d, trigger %d kind %d, trigger list %d, owner %s"
                      % (a, l, tr, kind, tl, owner.get(tl, "?")))
