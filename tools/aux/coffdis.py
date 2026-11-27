#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# coffdis.py FILE OUT: disassemble an A/UX COFF object's .text with its symbols and relocations beside the lines
import sys, struct, subprocess, tempfile, os, re
d = open(sys.argv[1], 'rb').read()
magic, nscns, timdat, symptr, nsyms, opthdr, flags = struct.unpack('>HHIIIHH', d[:20])
strtab = symptr + nsyms * 18
def name_of(b):
    if b[:4] == b'\0\0\0\0':
        o = struct.unpack('>I', b[4:8])[0]; e = d.index(b'\0', strtab + o); return d[strtab + o:e].decode('latin1')
    return b.rstrip(b'\0').decode('latin1')
syms = {}; i = 0; labels = {}
while i < nsyms:
    e = d[symptr + i * 18: symptr + i * 18 + 18]
    n = name_of(e[:8]); val, scn, typ, cls, naux = struct.unpack('>IhHBB', e[8:18])
    syms[i] = (n, val, scn, cls)
    if scn == 1 and cls in (2, 3) and n != '.text': labels[val] = n
    i += 1 + naux
o = 20 + opthdr; rel = {}; text = None
for k in range(nscns):
    nm = d[o:o+8].rstrip(b'\0').decode(); paddr, vaddr, size, scnptr, relptr, lnnoptr, nreloc, nlnno, fl = struct.unpack('>IIIIIIHHI', d[o+8:o+40])
    if nm == '.text':
        text = d[scnptr:scnptr+size]
        for r in range(nreloc):
            va, si, ty = struct.unpack('>IIH', d[relptr + r * 10: relptr + r * 10 + 10])
            s = syms.get(si); rel[va] = (s[0] if s else '?', ty)
    o += 40
t = tempfile.NamedTemporaryFile(delete=False, dir=os.path.dirname(sys.argv[2])); t.write(text); t.close()
out = subprocess.run(['m68k-linux-gnu-objdump', '-D', '-b', 'binary', '-m', 'm68k:68030', t.name], capture_output=True, text=True).stdout
os.unlink(t.name)
lines = out.splitlines()[7:]
res = []
for idx, l in enumerate(lines):
    m = re.match(r'\s*([0-9a-f]+):\s+((?:[0-9a-f]{4} )+)', l)
    if not m: res.append(l); continue
    a = int(m.group(1), 16); n = len(m.group(2).split()) * 2
    if a in labels: res.append('\n%s:' % labels[a])
    rs = ['%s@+%d' % (rel[x][0], x - a) for x in range(a, a + n) if x in rel]
    res.append(l + ('    ; ' + ', '.join(rs) if rs else ''))
open(sys.argv[2], 'w').write('\n'.join(res) + '\n')
