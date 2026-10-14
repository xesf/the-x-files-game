"""Compares two 24/32-bit BMP files: python3 -I tools/re/bmpdiff.py a.bmp b.bmp"""
import struct
import sys


def load(path):
    d = open(path, "rb").read()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0] // 8
    row = (w * bpp + 3) & ~3
    rows = [d[off + y * row: off + y * row + w * bpp] for y in range(abs(h))]
    if h < 0:
        rows.reverse()
    return w, abs(h), [bytes(b for j in range(0, len(r), bpp) for b in r[j:j + 3]) for r in rows]


wa, ha, a = load(sys.argv[1])
wb, hb, b = load(sys.argv[2])
if (wa, ha) != (wb, hb):
    sys.exit(f"size differs: {wa}x{ha} vs {wb}x{hb}")
diffs = [abs(x - y) for ra, rb in zip(a, b) for x, y in zip(ra, rb)]
print(f"{wa}x{ha} max {max(diffs)} mean {sum(diffs) / len(diffs):.3f} over2 {sum(d > 2 for d in diffs)}")
