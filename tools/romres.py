#!/usr/bin/env python3
"""Parse the Macintosh SE ROM resource map.
ROM header offset 0x1A holds a 32-bit ROM-relative pointer P. P points at a 4-byte prefix;
a standard 28-byte Resource Manager map header starts at P+4:
  +0x00 dataOffset +0x04 mapOffset +0x08 dataLength +0x0C mapLength (copy of file header)
  +0x10 nextMapHandle +0x14 fileRef +0x16 attrs +0x18 typeListOff +0x1A nameListOff (both map-relative)
Type list: count-1 (2), then 8-byte entries: type(4) count-1(2) refListOff(2, rel. to type list).
Ref entry (12): ID(2) nameOff(2; FFFF=none) attrs(1) dataOff(3, ROM-relative) reserved(4).
Each datum is preceded by an 8-byte fake Memory Manager block header: tag|size24(4) relHandle(4).
"""
import sys, struct
d = open(sys.argv[1], 'rb').read()
BASE = 0x400000
P = struct.unpack('>I', d[0x1A:0x1E])[0]
m = P + 4
dataOff, mapOff, dataLen, mapLen = struct.unpack('>4I', d[m:m+16])
tlo, nlo = struct.unpack('>HH', d[m+0x18:m+0x1C])
print(f"map prefix @ROM+{P:05X}: {d[P:P+4].hex()}  map header @ {BASE+m:06X}: dataOff {dataOff:X} mapOff {mapOff:X} dataLen {dataLen:X} mapLen {mapLen:X} attrs {struct.unpack('>H',d[m+0x16:m+0x18])[0]:04X}")
tl = m + tlo; nl = m + nlo
ntypes = struct.unpack('>H', d[tl:tl+2])[0] + 1
print(f"type list {BASE+tl:06X}  name list {BASE+nl:06X}  {ntypes} types\n")
rows = []
for t in range(ntypes):
    o = tl + 2 + t*8
    typ = d[o:o+4].decode('mac_roman'); n = struct.unpack('>H', d[o+4:o+6])[0] + 1; rlo = struct.unpack('>H', d[o+6:o+8])[0]
    for r in range(n):
        e = tl + rlo + r*12
        rid, noff = struct.unpack('>hH', d[e:e+4]); attrs = d[e+4]; doff = int.from_bytes(d[e+5:e+8], 'big')
        name = '' if noff == 0xFFFF else d[nl+noff+1:nl+noff+1+d[nl+noff]].decode('mac_roman', 'replace')
        blk = d[doff-8:doff]; size = int.from_bytes(blk[1:4], 'big')
        rows.append((typ, rid, name, attrs, doff, size, blk.hex()))
for typ, rid, name, attrs, doff, size, blk in sorted(rows, key=lambda r: r[4]):
    print(f"{typ!r:8} id {rid:6d} attrs {attrs:02X} data {BASE+doff:06X}..{BASE+doff+size:06X} size {size:6d}  blk {blk}  {name}")
print(f"\n{len(rows)} resources; data spans {BASE+min(r[4] for r in rows):06X}..{BASE+max(r[4]+r[5] for r in rows):06X}")
i = len(d)
while i > 0 and d[i-1] in (0, 0xFF): i -= 1
print(f"ROM end fill: last non-fill byte at ROM+{i-1:05X}; {len(d)-i} fill bytes")
