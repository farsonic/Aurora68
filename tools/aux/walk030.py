#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# walk a 68030 translation tree in a RAM dump: walk030.py DUMP TC ROOTHI ROOTLO ADDR [ADDR...]
import sys, struct
d = open(sys.argv[1], 'rb').read()
tc = int(sys.argv[2], 16); rh = int(sys.argv[3], 16); rl = int(sys.argv[4], 16)
def rd(a): return struct.unpack('>I', d[a:a+4])[0] if a + 4 <= len(d) else None
ps = (tc >> 20) & 15; is_ = (tc >> 16) & 15
ti = [(tc >> 12) & 15, (tc >> 8) & 15, (tc >> 4) & 15, tc & 15]
def walk(la):
    out = []
    dt = rh & 3; tbl = rl & ~15; shift = 32 - is_
    lvl = 0
    while True:
        if dt == 0: out.append('invalid'); return out
        if dt == 1: out.append('page (early) %08x' % tbl); return out
        if lvl >= 4 or ti[lvl] == 0: out.append('end of levels with table dt %d' % dt); return out
        w = ti[lvl]; shift -= w
        idx = (la >> shift) & ((1 << w) - 1)
        size = 4 if dt == 2 else 8
        a = tbl + idx * size
        if size == 4:
            v = rd(a); 
            if v is None: out.append('L%d @%08x outside dump' % (lvl, a)); return out
            ndt = v & 3; addr = v & ~15 if ndt != 1 else v & ~0xff
            out.append('L%d[%x] @%08x = %08x dt%d%s%s%s' % (lvl, idx, a, v, ndt, ' WP' if v & 4 else '', ' U' if v & 8 else '', ' M' if (ndt==1 and v & 16) else ''))
        else:
            v0 = rd(a); v1 = rd(a + 4)
            if v0 is None: out.append('L%d @%08x outside dump' % (lvl, a)); return out
            ndt = v0 & 3; addr = v1 & ~15 if ndt != 1 else v1 & ~0xff
            out.append('L%d[%x] @%08x = %08x:%08x dt%d%s%s%s lim %s%x' % (lvl, idx, a, v0, v1, ndt, ' S' if v0 & 0x100 else '', ' WP' if v0 & 4 else '', ' U' if v0 & 8 else '', 'L' if v0 >> 31 else 'U', (v0 >> 16) & 0x7fff))
        lvl += 1
        if ndt == 1:
            off = la & ((1 << shift) - 1)
            out.append('-> phys %08x' % (addr + off)); return out
        dt = ndt; tbl = addr
for x in sys.argv[5:]:
    la = int(x, 16)
    print('%08x:' % la); [print('   ', s) for s in walk(la)]
