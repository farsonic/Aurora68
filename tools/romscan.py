#!/usr/bin/env python3
"""Mac SE ROM structural triage: header, strings, drivers, resource map, SCSI/IWM/VIA address refs."""
import sys, struct, re
d = open(sys.argv[1], 'rb').read()
BASE = 0x400000
def u32(o): return struct.unpack('>I', d[o:o+4])[0]
def u16(o): return struct.unpack('>H', d[o:o+2])[0]
print(f"size {len(d):#x}  checksum {u32(0):08X}  reset PC {u32(4):08X}  version {u16(8):04X}")
print(f"header words @0x0A..0x20:", ' '.join(f"{u16(o):04X}" for o in range(0x0A, 0x20, 2)))
print(f"longs @0x10 {u32(0x10):08X} @0x14 {u32(0x14):08X} @0x18 {u32(0x18):08X} @0x1A {u32(0x1A):08X} @0x1C {u32(0x1C):08X}")
# Pascal/driver-name strings of interest
print("\n--- notable strings ---")
for s in [b'.Sony', b'.Sound', b'.SCSI', b'.AIn', b'.AOut', b'.BIn', b'.BOut', b'.Print', b'.Xpp', b'.ATP', b'.MPP', b'Macintosh SE', b'System', b'Finder', b'MacsBug', b'Welcome', b'rsrc', b'RSED', b'DRVR', b'ROM']:
    idx = [m.start() for m in re.finditer(re.escape(s), d)]
    if idx: print(f"{s.decode():14s} x{len(idx):<3} @ {', '.join(f'{BASE+i:06X}' for i in idx[:8])}")
# ROM resource map: look for ROM resource header magic 'RSED' and for resource type tags
print("\n--- 4-char tags (likely resource types) ---")
tags = {}
for m in re.finditer(rb'(DRVR|WDEF|CDEF|MDEF|LDEF|PACK|PTCH|ptch|FONT|NFNT|FOND|CURS|KCHR|KMAP|ICON|SICN|STR |INIT|cdev|snth|clut|gcrt|SERD|MBDF|FKEY|vers|lmgr|ADBS|CACH|INTL|itlk|itlc|itlb|itl0|itl1|itl2|itl4|RSED)', d):
    tags.setdefault(m.group(0).decode(), []).append(BASE+m.start())
for k,v in sorted(tags.items(), key=lambda kv: -len(kv[1])):
    print(f"{k!r:8} x{len(v):<3} first @ {', '.join(f'{a:06X}' for a in v[:6])}")
# Hardware address literals (24-bit addresses as immediates / absolute longs)
print("\n--- hardware address literal hits (count of 32-bit matches) ---")
hw = {'SCSI rd 580000':0x580000,'SCSI wr 580001':0x580001,'SCSI DACK 5A0000':0x5A0000,'SCSI DACK 5A0001':0x5A0001,'SCSI hsk 5C0000':0x5C0000,'SCSI hsk 5C0001':0x5C0001,
      'VIA EFE1FE':0xEFE1FE,'VIA EFE000':0xEFE000,'IWM DFE1FF':0xDFE1FF,'SCC rd 9FFFF8':0x9FFFF8,'SCC wr BFFFF9':0xBFFFF9,'ROM 400000':0x400000,'ROM 440000':0x440000,
      'ROM end 480000':0x480000,'RAM 400000 test':0x3FFFFF,'RAM top 800000':0x800000,'phase/test 600000':0x600000, 'SCSI 580000 blk 5A0000->':0x5A0004}
for name,a in hw.items():
    b = struct.pack('>I', a)
    n = len([m for m in re.finditer(re.escape(b), d)])
    if n: print(f"{name:26s} {a:08X} x{n}")
# Find candidate driver headers: DRVR header = flags(2) delay(2) emask(2) menu(2) open(2) prime(2) ctl(2) status(2) close(2) then pascal name
print("\n--- DRVR header candidates (offset, flags, name) ---")
for m in re.finditer(rb'\x05\.Sony|\x06\.Sound|\x05\.SCSI|\x04\.AIn|\x05\.AOut|\x04\.BIn|\x05\.BOut', d):
    o = m.start()-18
    if o >= 0:
        flags, delay, emask, menu, op, pr, ct, st, cl = struct.unpack('>9H', d[o:o+18])
        print(f"  {BASE+o:06X}: flags {flags:04X} delay {delay:04X} emask {emask:04X} menu {menu:04X} open {op:04X} prime {pr:04X} ctl {ct:04X} status {st:04X} close {cl:04X}  {d[m.start()+1:m.start()+1+d[m.start()]].decode()}")
