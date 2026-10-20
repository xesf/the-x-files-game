#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 xesf
"""Lists 16-bit Windows (NE) executables for reverse engineering: the
earlier HyperBole games' VORTEX.EXE and QGATE.EXE, which IDA does not
handle here. Needs the capstone module (python3 -m pip install capstone).

  ne.py <exe> info                    segments, imports, entry points
  ne.py <exe> disasm <seg> [out]      one code segment, with far calls,
                                      imports and data strings resolved
  ne.py <exe> strings                 the strings of every segment (the
                                      compiler keeps literals in the code
                                      segments), with their offsets
  ne.py <exe> xref <string>           code that pushes the offset of a
                                      data string containing <string>
"""
import struct
import sys

import capstone


class NE:
    def __init__(self, path):
        self.d = d = open(path, 'rb').read()
        ne = struct.unpack_from('<I', d, 0x3C)[0]
        if d[ne:ne + 2] != b'NE':
            raise SystemExit('%s: not an NE executable' % path)
        self.ne = ne
        (self.entoff, self.entlen) = struct.unpack_from('<HH', d, ne + 4)
        self.autods = struct.unpack_from('<H', d, ne + 0x0E)[0]
        self.ip, self.cs = struct.unpack_from('<HH', d, ne + 0x14)
        nseg, nmod = struct.unpack_from('<HH', d, ne + 0x1C)
        segtab, rsrc, restab, modtab, imptab = struct.unpack_from('<HHHHH', d, ne + 0x22)
        self.nonres_off = struct.unpack_from('<I', d, ne + 0x2C)[0]
        self.nonres_len = struct.unpack_from('<H', d, ne + 0x20)[0]
        self.align = struct.unpack_from('<H', d, ne + 0x32)[0]
        self.segs = []
        for i in range(nseg):
            o, l, f, m = struct.unpack_from('<HHHH', d, ne + segtab + i * 8)
            self.segs.append({'off': o << self.align, 'len': l or (0x10000 if o else 0), 'flags': f, 'mem': m or 0x10000})
        self.modules = []
        for i in range(nmod):
            o = struct.unpack_from('<H', d, ne + modtab + i * 2)[0]
            q = ne + imptab + o
            self.modules.append(d[q + 1:q + 1 + d[q]].decode('latin-1'))
        self.imptab = ne + imptab
        self.names = {}  # ordinal -> name of this module's exports
        self._names(ne + restab)
        if self.nonres_off:
            self._names(self.nonres_off, first_is_desc=True)
        self.entries = self._entries()

    def _names(self, p, first_is_desc=False):
        d = self.d
        first = True
        while d[p]:
            n = d[p]
            name = d[p + 1:p + 1 + n].decode('latin-1')
            ordn = struct.unpack_from('<H', d, p + 1 + n)[0]
            if not first or not first_is_desc:
                if ordn:
                    self.names[ordn] = name
            first = False
            p += 3 + n

    def _entries(self):
        d, p, end, ordn, out = self.d, self.ne + self.entoff, self.ne + self.entoff + self.entlen, 1, {}
        while p < end:
            count, kind = d[p], d[p + 1]
            p += 2
            if count == 0:
                break
            for _ in range(count):
                if kind == 0:
                    pass
                elif kind == 0xFF:
                    seg, off = d[p + 3], struct.unpack_from('<H', d, p + 4)[0]
                    out[ordn] = (seg, off)
                    p += 6
                else:
                    off = struct.unpack_from('<H', d, p + 1)[0]
                    out[ordn] = (kind, off)
                    p += 3
                ordn += 1
        return out

    def seg_data(self, n):
        s = self.segs[n - 1]
        return self.d[s['off']:s['off'] + s['len']] if s['off'] else b''

    def relocs(self, n):
        """Fixups of a segment: offset -> description."""
        s = self.segs[n - 1]
        out = {}
        if not (s['flags'] & 0x100) or not s['off']:
            return out
        d, p = self.d, s['off'] + s['len']
        count = struct.unpack_from('<H', d, p)[0]
        p += 2
        for _ in range(count):
            src, flags, off, a, b = struct.unpack_from('<BBHHH', d, p)
            p += 8
            kind = flags & 3
            if kind == 0:  # internal
                desc = ('seg%d:%04X' % (a & 0xFF, b)) if (a & 0xFF) != 0xFF else ('entry %d' % b)
                if (a & 0xFF) == 0xFF and b in self.entries:
                    desc = 'seg%d:%04X' % self.entries[b]
            elif kind == 1:
                desc = '%s.%d' % (self.modules[a - 1], b)
            elif kind == 2:
                q = self.imptab + b
                desc = '%s.%s' % (self.modules[a - 1], self.d[q + 1:q + 1 + self.d[q]].decode('latin-1'))
            else:
                desc = 'osfixup'
            # follow the chain of offsets that share this fixup (additive bit clear)
            chain = off
            seen = 0
            seg_bytes = self.seg_data(n)
            while True:
                out[chain] = (src, desc)
                if flags & 4 or chain + 2 > len(seg_bytes) or seen > 4096:
                    break
                nxt = struct.unpack_from('<H', seg_bytes, chain)[0]
                if nxt == 0xFFFF or nxt == chain:
                    break
                chain = nxt
                seen += 1
        return out

    def strings(self, minlen=4, seg=None):
        data = self.seg_data(seg or self.autods)
        out, i = {}, 0
        while i < len(data):
            j = i
            while j < len(data) and 32 <= data[j] < 127:
                j += 1
            if j - i >= minlen and j < len(data) and data[j] == 0:
                out[i] = data[i:j].decode('latin-1')
                i = j + 1
            else:
                i = max(j, i + 1)
        return out


def info(ne):
    print('entry point seg%d:%04X, data segment %d' % (ne.cs, ne.ip, ne.autods))
    for i, s in enumerate(ne.segs, 1):
        print('seg%-3d %s file 0x%06X size %5d' % (i, 'DATA' if s['flags'] & 1 else 'CODE', s['off'], s['len']))
    print('imports:', ', '.join(ne.modules))
    for o in sorted(ne.entries):
        print('export %d %s seg%d:%04X' % (o, ne.names.get(o, ''), *ne.entries[o]))


def disasm(ne, n, out):
    code = ne.seg_data(n)
    rel = ne.relocs(n)
    strs = ne.strings()
    local = ne.strings(seg=n)  # string literals kept in the code segment
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)
    md.skipdata = True
    for ins in md.disasm(code, 0):
        note = []
        for k in range(ins.size):
            if ins.address + k in rel:
                src, desc = rel[ins.address + k]
                if src == 2 and desc.startswith('seg') and ins.mnemonic in ('lcall', 'ljmp'):
                    # a segment fixup: the offset is the instruction's own
                    desc = '%s:%04X' % (desc.split(':')[0], int(ins.op_str.split(',')[1], 16))
                note.append(desc)
                break
        if ins.mnemonic in ('push', 'mov') and ins.op_str.startswith(('0x', 'ax, 0x', 'word ptr')) and not note:
            try:
                v = int(ins.op_str.split('0x')[-1], 16)
                if v in local:
                    note.append('cs:' + repr(local[v]))
                elif v in strs:
                    note.append(repr(strs[v]))
            except ValueError:
                pass
        out.write('%04X  %-24s %-6s %s%s\n' % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str,
                                               ('    ; ' + ' '.join(note)) if note else ''))


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    ne = NE(sys.argv[1])
    cmd = sys.argv[2]
    if cmd == 'info':
        info(ne)
    elif cmd == 'strings':
        for n in range(1, len(ne.segs) + 1):
            for o, s in sorted(ne.strings(seg=n).items()):
                print('seg%d:%04X %s' % (n, o, s))
    elif cmd == 'disasm':
        out = open(sys.argv[4], 'w') if len(sys.argv) > 4 else sys.stdout
        disasm(ne, int(sys.argv[3]), out)
    elif cmd == 'xref':
        want = sys.argv[3].lower()
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)
        md.skipdata = True
        ds = {o: s for o, s in ne.strings().items() if want in s.lower()}
        for n, s in enumerate(ne.segs, 1):
            if s['flags'] & 1:
                continue
            strs = dict(ds)
            strs.update({o: t for o, t in ne.strings(seg=n).items() if want in t.lower()})
            for ins in md.disasm(ne.seg_data(n), 0):
                if ins.mnemonic in ('push', 'mov') and '0x' in ins.op_str:
                    try:
                        v = int(ins.op_str.split('0x')[-1], 16)
                    except ValueError:
                        continue
                    if v in strs:
                        print('seg%d:%04X  %s %s    ; %r' % (n, ins.address, ins.mnemonic, ins.op_str, strs[v]))
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
