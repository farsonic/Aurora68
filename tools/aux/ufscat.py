#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# ufscat.py DISK PARTSTART PATH: print a file from a big-endian 4.2BSD FFS (A/UX 3 UFS) partition of a disk image
import sys, struct
f = open(sys.argv[1], 'rb'); P = int(sys.argv[2]) * 512
def rd(off, n): f.seek(P + off); return f.read(n)
sb = rd(4096, 2048)
g = lambda o: struct.unpack('>i', sb[o:o+4])[0]
sblkno, cblkno, iblkno, dblkno, cgoffset, cgmask = g(8), g(12), g(16), g(20), g(24), g(28)
bsize, fsize, frag = g(48), g(52), g(56); ipg, fpg = g(184), g(188); inopb = g(120)
def cgstart(c): return fpg * c + cgoffset * (c & ~cgmask)
def inode(ino):
    c = ino // ipg; fr = cgstart(c) + iblkno + ((ino % ipg) // inopb) * frag
    b = rd(fr * fsize + (ino % inopb) * 128, 128)
    mode, = struct.unpack('>H', b[0:2]); size, = struct.unpack('>q', b[8:16])
    db = struct.unpack('>12i', b[40:88]); ib = struct.unpack('>3i', b[88:100])
    return mode, size, db, ib
def blocks(db, ib, size):
    out = []
    for a in db: out.append(a)
    if ib[0]:
        out += list(struct.unpack('>%di' % (bsize // 4), rd(ib[0] * fsize, bsize)))
    return out
def data(ino):
    mode, size, db, ib = inode(ino); bl = blocks(db, ib, size); out = b''
    for a in bl:
        if len(out) >= size: break
        out += rd(a * fsize, bsize) if a else bytes(bsize)
    return mode, out[:size]
def lookup(dino, name):
    mode, d = data(dino); i = 0
    while i < len(d):
        ino, rl, nl = struct.unpack('>IHH', d[i:i+8])
        if rl == 0: break
        if d[i+8:i+8+nl].decode('latin1') == name: return ino
        i += rl
    return None
ino = 2
for part in [p for p in sys.argv[3].split('/') if p]:
    ino = lookup(ino, part)
    if ino is None: sys.exit('not found: ' + part)
mode, d = data(ino)
if len(sys.argv) > 4 and sys.argv[4] == 'ls':
    i = 0
    while i < len(d):
        ino2, rl, nl = struct.unpack('>IHH', d[i:i+8])
        if rl == 0: break
        print(ino2, d[i+8:i+8+nl].decode('latin1')); i += rl
else:
    sys.stdout.buffer.write(d)
