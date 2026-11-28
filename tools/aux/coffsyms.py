#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# coffsyms.py FILE: an A/UX COFF file's symbols (value, section, class, name) and each section's relocations
import sys, struct
d = open(sys.argv[1], 'rb').read()
magic, nscns, timdat, symptr, nsyms, opthdr, flags = struct.unpack('>HHIIIHH', d[:20])
strtab = symptr + nsyms * 18
def name_of(b):
    if b[:4] == b'\0\0\0\0':
        o = struct.unpack('>I', b[4:8])[0]
        e = d.index(b'\0', strtab + o)
        return d[strtab + o:e].decode('latin1')
    return b.rstrip(b'\0').decode('latin1')
syms = []
i = 0
while i < nsyms:
    e = d[symptr + i * 18: symptr + i * 18 + 18]
    n = name_of(e[:8]); val, scn, typ, cls, naux = struct.unpack('>IhHBB', e[8:18])
    syms.append((i, n, val, scn, typ, cls))
    i += 1 + naux
symidx = {s[0]: s for s in syms}
if len(sys.argv) > 2 and sys.argv[2] == 'syms':
    for s in sorted(syms, key=lambda s: (s[3], s[2])):
        if s[5] in (2, 3):
            print('%08x scn %2d cls %d %s' % (s[2], s[3], s[5], s[1]))
    sys.exit()
o = 20 + opthdr
for k in range(nscns):
    nm = d[o:o+8].rstrip(b'\0').decode(); paddr, vaddr, size, scnptr, relptr, lnnoptr, nreloc, nlnno, fl = struct.unpack('>IIIIIIHHI', d[o+8:o+40])
    for r in range(nreloc):
        va, si, ty = struct.unpack('>IIH', d[relptr + r * 10: relptr + r * 10 + 10])
        s = symidx.get(si)
        print('reloc %s %08x type %d -> %s' % (nm, va, ty, s[1] if s else '?%d' % si))
    o += 40
