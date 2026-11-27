#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# vread.py DUMP ROOT ADDR LEN [raw OUT]: read a 68030 logical range from a RAM dump through A/UX's tables
# (TC $82C07760: 4 KiB pages, 7/7/6 index bits, short descriptors; ROOT is the root table's physical address)
import sys, struct
d = open(sys.argv[1], 'rb').read()
root = int(sys.argv[2], 16); addr = int(sys.argv[3], 16); ln = int(sys.argv[4], 16)
def rd(a): return struct.unpack('>I', d[a:a+4])[0] if a + 4 <= len(d) else 0
def xlate(la):
    a = rd((root & ~15) + ((la >> 25) << 2))
    if a & 3 == 0: return None, 'A invalid %08x' % a
    if a & 3 == 1: return (a & ~0xff) + (la & 0x1ffffff), 'early A'
    b = rd((a & ~15) + (((la >> 18) & 0x7f) << 2))
    if b & 3 == 0: return None, 'B invalid %08x' % b
    if b & 3 == 1: return (b & ~0xff) + (la & 0x3ffff), 'early B'
    c = rd((b & ~15) + (((la >> 12) & 0x3f) << 2))
    if c & 3 != 1: return None, 'C %08x' % c
    return (c & ~0xfff) + (la & 0xfff), 'C %08x' % c
if len(sys.argv) > 5 and sys.argv[5] == 'raw':
    out = b''
    for la in range(addr, addr + ln):
        p, why = xlate(la & 0xffffffff)
        out += d[p:p+1] if p is not None and p < len(d) else b'\0'
    open(sys.argv[6], 'wb').write(out); sys.exit()
la = addr & ~3
while la < addr + ln:
    line = '%08x:' % (la & 0xffffffff)
    for k in range(8):
        p, why = xlate((la + 4 * k) & 0xffffffff)
        line += ' ' + ('%08x' % rd(p) if p is not None and p + 4 <= len(d) else '--------')
    p, why = xlate(la & 0xffffffff)
    print(line, ' [%s -> %s]' % (why, '%08x' % p if p is not None else 'none'))
    la += 32
