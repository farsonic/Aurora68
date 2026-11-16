#!/usr/bin/env python3
"""List ROM resources of a Universal (Mac II-class) ROM.
ROM+0x1A -> pointer to the LAST entry of a linked list. Entry (24 bytes):
  flags(4) next(4) prev(4) dataOff(4) type(4) id(2) attrs(1) pad(1); follow prev until 0.
Each datum is preceded by a 12-byte 32-bit Memory Manager block header: tag(1) x(3) physSize(4) relHandle(4)."""
import sys, struct
d=open(sys.argv[1],'rb').read(); B=0x40800000
p=struct.unpack(">I",d[struct.unpack(">I",d[0x1A:0x1E])[0]:][:4])[0]  # ROM+0x1A -> ptr -> last entry
rows=[]
while p:
    flags,nxt,prv,data=struct.unpack('>4I',d[p:p+16]); typ=d[p+16:p+20].decode('mac_roman'); rid,attrs=struct.unpack('>hB',d[p+20:p+23])
    size=struct.unpack('>I',d[data-8:data-4])[0]
    rows.append((typ,rid,attrs,data,size,flags,p)); p=prv
for typ,rid,attrs,data,size,flags,p in sorted(rows,key=lambda r:r[3]):
    extra=''
    if typ=='DRVR':
        n=d[data+18]; extra=d[data+19:data+19+n].decode('mac_roman','replace')
    print(f"{typ!r:8} id {rid:6d} attrs {attrs:02X} flags {flags:08X} data {B+data:08X} size {size:7d}  {extra}")
print(f"\n{len(rows)} resources; data {B+min(r[3] for r in rows):08X}..{B+max(r[3]+r[4] for r in rows):08X}")
