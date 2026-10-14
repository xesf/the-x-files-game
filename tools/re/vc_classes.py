"""Lists the VirtualCinema classes registered at startup.

Each registration allocates a class descriptor and calls the register
function with: class ID, a flag, the class name, the factory function,
0 and 'null'. This scans the registration function for that call pattern.

usage: python3 -I tools/re/vc_classes.py ida/xfiles.exe
"""
import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from pe import PE  # noqa: E402

REGISTER_FN = 0x442FF0
START, END = 0x498060, 0x4999A9


def main():
    pe = PE(sys.argv[1])
    code = pe.read(START, END - START)
    name_ptr = None
    rows = []
    for i in range(len(code) - 5):
        va = START + i
        # mov esi/edx, [name_ptr]   (8B 35 / 8B 15 + abs32)
        if code[i] == 0x8B and code[i + 1] in (0x35, 0x15, 0x0D, 0x05, 0x1D, 0x3D):
            p = struct.unpack_from("<I", code, i + 2)[0]
            if 0x6B8B00 <= p < 0x6B8D00:
                name_ptr = p
        # push offset factory; push reg (name); push flag; push id; call register
        if (code[i] == 0x68 and 0x50 <= code[i + 5] <= 0x57
                and code[i + 6] == 0x6A and code[i + 8] == 0x6A):
            factory = struct.unpack_from("<I", code, i + 1)[0]
            flag, cid = code[i + 7], code[i + 9]
            j = code.find(b"\xE8", i + 10, i + 20)
            if j < 0:
                continue
            target = START + j + 5 + struct.unpack_from("<i", code, j + 1)[0]
            if target != REGISTER_FN:
                continue
            name = pe.cstr(pe.u32(name_ptr)) if name_ptr else "?"
            rows.append((cid, name, factory, flag, va))
            name_ptr = None
    for cid, name, factory, flag, va in sorted(rows):
        print(f"0x{cid:02X}  {name:24s} factory=0x{factory:06X} flag={flag}  at 0x{va:06X}")
    print(f"{len(rows)} classes", file=sys.stderr)


main()
