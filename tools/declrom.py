#!/usr/bin/env python3
"""Parse a NuBus/pseudo-slot declaration ROM embedded at the END of a Mac ROM image (format block = last 20 bytes).
Lists sResources and, for video functional sResources, each mode's VPBlock geometry."""
import sys, struct
d=open(sys.argv[1],'rb').read()
fb=len(d)-20
diroff=int.from_bytes(d[fb:fb+4],'big',signed=True) if d[fb]&0x80 else (int.from_bytes(d[fb+1:fb+4],'big') - (1<<24 if d[fb+1]&0x80 else 0))
length,crc,fmt,rev=struct.unpack('>IIBB',d[fb+4:fb+14]); test=struct.unpack('>I',d[fb+14:fb+18])[0]; lanes=d[fb+19]
print(f"format block @{fb:06X}: dirOffset {diroff} (-> {fb+diroff:06X}) length {length} crc {crc:08X} fmt {fmt} rev {rev} test {test:08X} lanes {lanes:02X}")
def entries(off):
    """sResource / directory: 4-byte entries id(1) offset(3 signed) until id 0xFF"""
    out=[]
    while True:
        e=d[off:off+4]; sid=e[0]; val=int.from_bytes(e[1:4],'big',signed=True)
        if sid==0xFF: break
        out.append((sid,val,off)); off+=4
    return out
def cstr(o):
    e=d.index(b'\0',o); return d[o:e].decode('mac_roman','replace')
dirs=entries(fb+diroff)
for sid,off,eo in dirs:
    base=eo+off
    fields=entries(base)
    f={k:(v,o) for k,v,o in fields}
    typ=f.get(1); name=f.get(2)
    typ_s=d[typ[1]+typ[0]:typ[1]+typ[0]+8].hex() if typ else '?'
    name_s=cstr(name[1]+name[0]) if name else '?'
    print(f"\nsRsrc {sid:02X} @{base:06X}  type {typ_s}  name {name_s!r}  ids {[k for k,_,_ in fields]}")
    for k,v,o in fields:
        if 0x80<=k<=0x9F:   # video mode parameters (sRsrc id k): mode block with VPBlock at sub-id 1
            mb=o+v
            sub={kk:(vv,oo) for kk,vv,oo in entries(mb)}
            if 1 in sub:
                vp=sub[1][1]+sub[1][0]
                bs,bo,rb=struct.unpack('>IIH',d[vp:vp+10]); t,l,b,r=struct.unpack('>4h',d[vp+10:vp+18]); ps=struct.unpack('>H',d[vp+30:vp+32])[0]
                pd=sub.get(3); pd_s=d[pd[1]+pd[0]] if pd else -1
                print(f"   mode {k:02X}: {r-l}x{b-t} rowBytes {rb} depth {ps} baseOffset {bo:#x} pageCount {pd_s}")
