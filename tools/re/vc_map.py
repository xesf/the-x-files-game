# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 xesf and contributors
"""Prints a map of the views reachable from a view: their moves, the
objects and characters in them, and what clicking each one does.

usage: python3 -I tools/re/vc_map.py XFILES.HDB <view> [max views]

Useful to turn a walkthrough into a play script: find the view, then
the object whose click plays the right clip or opens the right
close-up.
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
d, rec, u32, u16 = env["d"], env["records"], env["u32"], env["u16"]


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


def cls(i):
    return rec.get(i, (0, 0))[0]


def off(i):
    return rec.get(i, (0, 0))[1]


def lst(i):
    """IDs of a list object (any class whose first field is a list)."""
    o = off(i)
    return ids(u32(o + 10)) if o and cls(i) in (0x2E, 0x34, 0x36, 0x38, 0x42, 0x4E, 0x52) else []


def asset_name(a):
    o = off(a)
    if cls(a) != 0x35 or not o:
        return "?"
    m, n = u32(o + 6), u32(o + 10)
    b = d[m:m + n]
    if not b:
        return "?"
    # A path is a token holding its length in decimal, then the path.
    k = int(b[1:1 + b[0]] or b"0")
    p = 1 + b[0]
    return b[p:p + k].decode("latin1").replace("\x7f", "/")


def media_list_name(l):
    a = lst(l)
    return asset_name(a[0]) if a else "#%d" % l


def describe_actions(l, depth=0):
    out = []
    for a in lst(l):
        o = off(a)
        if cls(a) != 0x41 or not o:
            continue
        m, n, t = u32(o + 6), u32(o + 10), d[o + 14]
        pre = 12 if t & 0x80 else 0
        cond = "if " if pre else ""
        b = d[m + pre:m + n]
        t &= 0x7F
        if t == 1 and len(b) >= 4:
            out.append(cond + "play " + media_list_name(struct.unpack(">I", b[:4])[0]).split("/")[-1])
        elif t == 3 and len(b) >= 9:
            out.append(cond + ("enable" if b[8] else "disable") + " #%d" % struct.unpack(">I", b[:4])[0])
        elif t == 4 and len(b) >= 4:
            out.append(cond + "go to view %d" % struct.unpack(">I", b[:4])[0])
        elif t == 5 and len(b) >= 5:
            out.append(cond + ("open" if b[4] == 0 else "close") + " layout %d" % struct.unpack(">I", b[:4])[0])
        elif t == 6:
            out.append(cond + b[:15].split(b"\0")[0].decode("latin1") + "()")
    return out


def triggers(tl, kind=0):
    acts = []
    for t in lst(tl):
        o = off(t)
        if cls(t) == 0x51 and o and d[o + 10] == kind:
            acts += describe_actions(u32(o + 6))
    return acts


def view_name(v):
    o = off(v)
    if cls(v) != 0x2B or not o:
        return "?"
    a = lst(u32(o + 10))
    return asset_name(a[0]).replace("Navs/", "") if a else "?"


def name_text(n):
    o = off(n)
    if cls(n) != 0x2F or not o:
        return "?"
    m, k = u32(o + 6), u32(o + 10)
    return d[m:m + k].split(b"\0")[0].decode("latin1")


def character_name(cv):
    o = off(cv)
    ch = u32(o + 6) if o else 0
    co = off(ch)
    return name_text(u32(co + 6)) if cls(ch) == 0x2C and co else "?"


def summary(acts, limit=6):
    s = "; ".join(acts[:limit])
    return s + ("; ..." if len(acts) > limit else "")


def main():
    start = int(sys.argv[2])
    limit = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    seen, todo = set(), [start]
    while todo and len(seen) < limit:
        v = todo.pop(0)
        if v in seen or cls(v) != 0x2B:
            continue
        seen.add(v)
        o = off(v)
        # VCView: direction, assets, characters, navs, explorables, triggers
        chars, navs, expl = u32(o + 14), u32(o + 18), u32(o + 22)
        print("view %d  %s" % (v, view_name(v)))
        for nv in lst(navs):
            no = off(nv)
            target = u32(no + 6) if no else 0
            print("  move %d -> %d  %s" % (nv, target, view_name(target)))
            todo.append(target)
        for e in lst(expl):
            eo = off(e)
            r = struct.unpack(">4h", d[eo + 10:eo + 18])
            print("  object %d at (%d,%d)-(%d,%d): %s" % ((e,) + r + (summary(triggers(u32(eo + 18))),)))
        for c in lst(chars):
            co = off(c)
            r = struct.unpack(">4h", d[co + 14:co + 22])
            print("  character %d (%s) at (%d,%d)-(%d,%d): %s" % ((c, character_name(c)) + r + (summary(triggers(u32(co + 34))),)))


main()
