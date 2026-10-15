# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 xesf
"""Checks that action operands and enable targets resolve in the GAM.

usage: python3 -I tools/re/check_refs.py XFILES.HDB XFILES.GAM
"""
import collections
import struct
import sys

here = __file__.rsplit("/", 1)[0]
code = open(here + "/hdb_layouts.py").read().split("want = ")[0]


def load(path):
    env = {"sys": sys, "struct": struct, "collections": collections, "bisect": __import__("bisect")}
    exec(code.replace("sys.argv[1]", repr(path)), env)
    return env["d"], env["records"]


hd, hrec = load(sys.argv[1])
gd, grec = load(sys.argv[2])
be32 = lambda d, o: struct.unpack_from(">I", d, o)[0]

var_class = collections.Counter()
enable_gam = collections.Counter()
enable_hdb = collections.Counter()
for oid, (cls, off) in hrec.items():
    if cls != 0x41 or not off:
        continue
    mark, n = be32(hd, off + 6), be32(hd, off + 10)
    t = hd[off + 14]
    if not mark:
        continue
    p = hd[mark:mark + n]
    pre = 12 if t & 0x80 else 0
    exprs = []
    if pre:
        exprs.append(p[:11])
    body = p[pre:]
    if t & 0x7F == 0 and len(body) == 11:
        exprs.append(body)
    for e in exprs:
        for val, kind in ((be32(e, 0), e[8]), (be32(e, 4), e[9])):
            if kind == 0:
                var_class[grec.get(val, (None,))[0]] += 1
    if t & 0x7F == 3 and len(body) == 9:
        target = be32(body, 0)
        enable_gam[grec.get(target, (None,))[0]] += 1
        enable_hdb[hrec.get(target, (None,))[0]] += 1

fmt = lambda c: {(f"0x{k:02X}" if k is not None else "missing"): v for k, v in c.items()}
print("variable operands, class in GAM:", fmt(var_class))
print("enable targets, class in GAM:", fmt(enable_gam))
print("enable targets, class in HDB:", fmt(enable_hdb))
