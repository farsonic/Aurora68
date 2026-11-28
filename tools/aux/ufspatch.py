#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# ufspatch.py DISK PARTSTART PATH NEWFILE: replace a small file's contents in a big-endian 4.2BSD FFS (A/UX 3 UFS)
# partition of a disk image, in the fragments the file already has (the new length may not need more of them).
# For the testbed's copy of the A/UX disk only.
import sys, struct
f = open(sys.argv[1], 'r+b'); P = int(sys.argv[2]) * 512
def rd(off, n): f.seek(P + off); return f.read(n)
def wr(off, b): f.seek(P + off); f.write(b)
sb = rd(4096, 2048)
g = lambda o: struct.unpack('>i', sb[o:o+4])[0]
sblkno, cblkno, iblkno, dblkno, cgoffset, cgmask = g(8), g(12), g(16), g(20), g(24), g(28)
bsize, fsize, frag = g(48), g(52), g(56); ipg, fpg = g(184), g(188); inopb = g(120)
def cgstart(c): return fpg * c + cgoffset * (c & ~cgmask)
def inode_off(ino):
    c = ino // ipg; fr = cgstart(c) + iblkno + ((ino % ipg) // inopb) * frag
    return fr * fsize + (ino % inopb) * 128
def inode(ino):
    b = rd(inode_off(ino), 128)
    mode, = struct.unpack('>H', b[0:2]); size, = struct.unpack('>q', b[8:16])
    return mode, size, struct.unpack('>12i', b[40:88]), struct.unpack('>3i', b[88:100])
def data(ino):
    mode, size, db, ib = inode(ino); out = b''
    for a in db:
        if len(out) >= size: break
        out += rd(a * fsize, bsize) if a else bytes(bsize)
    return out[:size]
def lookup(dino, name):
    d = data(dino); i = 0
    while i < len(d):
        ino, rl, nl = struct.unpack('>IHH', d[i:i+8])
        if rl == 0: break
        if ino and d[i+8:i+8+nl].decode('latin1') == name: return ino
        i += rl
    return None
ino = 2
for part in [p for p in sys.argv[3].split('/') if p]:
    ino = lookup(ino, part)
    if ino is None: sys.exit('not found: ' + part)
mode, size, db, ib = inode(ino)
new = open(sys.argv[4], 'rb').read()
have = (size + fsize - 1) // fsize * fsize
if size > 12 * bsize or ib[0]: sys.exit('too large a file for this tool')
if len(new) > have: sys.exit('the new contents are %d bytes, the file has room for %d (it is %d now)' % (len(new), have, size))
left = new + bytes(have - len(new)); i = 0
while left:
    wr(db[i] * fsize, left[:bsize]); left = left[bsize:]; i += 1
wr(inode_off(ino) + 8, struct.pack('>q', len(new)))
print('%s: inode %d, %d bytes -> %d (room %d, fragment %d)' % (sys.argv[3], ino, size, len(new), have, fsize))
