#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# ufswho.py DISK PARTSTART OFFSET...: which file of a big-endian FFS partition holds each byte offset of the disk image
import sys, struct
f = open(sys.argv[1], 'rb'); P = int(sys.argv[2]) * 512
offs = [int(x, 0) - P for x in sys.argv[3:]]
def rd(off, n): f.seek(P + off); return f.read(n)
sb = rd(4096, 2048)
g = lambda o: struct.unpack('>i', sb[o:o+4])[0]
sblkno, cblkno, iblkno, dblkno, cgoffset, cgmask = g(8), g(12), g(16), g(20), g(24), g(28)
bsize, fsize, frag = g(48), g(52), g(56); ipg, fpg = g(184), g(188); inopb = g(120); ncg = g(44)
def cgstart(c): return fpg * c + cgoffset * (c & ~cgmask)
def inode(ino):
    c = ino // ipg; fr = cgstart(c) + iblkno + ((ino % ipg) // inopb) * frag
    b = rd(fr * fsize + (ino % inopb) * 128, 128)
    mode, = struct.unpack('>H', b[0:2]); size, = struct.unpack('>q', b[8:16])
    return mode, size, struct.unpack('>12i', b[40:88]), struct.unpack('>3i', b[88:100])
want = {o // fsize: o for o in offs}
owner = {}
for ino in range(2, ncg * ipg):
    mode, size, db, ib = inode(ino)
    if mode & 0xF000 not in (0x8000, 0x4000): continue
    bl = [(a, i * bsize) for i, a in enumerate(db) if a]
    if ib[0]:
        bl += [(a, (12 + i) * bsize) for i, a in enumerate(struct.unpack('>%di' % (bsize // 4), rd(ib[0] * fsize, bsize))) if a]
    for a, fo in bl:
        for k in range(frag):
            if a + k in want: owner[want[a + k]] = (ino, fo + k * fsize + want[a + k] % fsize, size)
# names: walk the tree
names = {}
def data(ino):
    mode, size, db, ib = inode(ino); out = b''
    for a in db:
        if len(out) >= size: break
        out += rd(a * fsize, bsize) if a else bytes(bsize)
    return out[:size]
def walk(dino, path, depth=0):
    d = data(dino); i = 0
    while i + 8 <= len(d):
        ino, rl, nl = struct.unpack('>IHH', d[i:i+8])
        if rl == 0: break
        nm = d[i+8:i+8+nl].decode('latin1')
        if ino and ino < ncg * ipg and nm not in ('.', '..'):
            names.setdefault(ino, path + '/' + nm)
            if inode(ino)[0] & 0xF000 == 0x4000 and depth < 8: walk(ino, path + '/' + nm, depth + 1)
        i += rl
walk(2, '')
for o in offs:
    if o in owner: print('%x: inode %d %s at file offset %x (size %d)' % (o + P, owner[o][0], names.get(owner[o][0], '?'), owner[o][1], owner[o][2]))
    else: print('%x: no file found' % (o + P))
