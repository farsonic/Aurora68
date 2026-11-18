#!/usr/bin/env python3
"""Pack a C program for the classic Mac OS (linked at 0 with --emit-relocs: mac/c/app.ld) as an
application, a MacBinary II file.

    elf2app.py prog.elf --name PiLog --creator PILG [--size KB] [--rsrc TYPE:ID:file ...] -o PiLog.bin

CODE 1 holds the image (.text, .data and the zero-filled .bss up to _image_end) and after it a
long count and the offsets of every 32-bit absolute field (R_68K_32 against a section symbol);
_reloc_off (mac/c/crt0.s) gets the table's offset and the program adds its load address to each
field when it starts. CODE 0 is the jump table with the one entry, the start of CODE 1; below
A5 there is room for QuickDraw's globals. SIZE -1: MultiFinder aware, takes suspend and resume
events, runs in the background (null events there too), 32-bit clean. MPL-2.0.
"""
import argparse, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rsrc import ResourceFork, macbinary

R_68K_32 = 1
SHN_ABS = 0xfff1

def elf_sections(d):
    assert d[:4] == b'\x7fELF' and d[4] == 1 and d[5] == 2, "not a 32-bit big-endian ELF"
    shoff, = struct.unpack('>I', d[32:36])
    shentsize, shnum, shstrndx = struct.unpack('>HHH', d[46:52])
    secs = []
    for i in range(shnum):
        f = struct.unpack('>IIIIIIIIII', d[shoff + i * shentsize: shoff + i * shentsize + 40])
        secs.append(dict(name=f[0], type=f[1], flags=f[2], addr=f[3], off=f[4], size=f[5], link=f[6], info=f[7]))
    strtab = secs[shstrndx]
    for s in secs:
        n = d[strtab['off'] + s['name']:]
        s['name'] = n[:n.index(b'\0')].decode()
    return secs

def symbols(d, secs):
    st = next(s for s in secs if s['type'] == 2)
    strs = secs[st['link']]
    out = []
    for i in range(st['size'] // 16):
        nm, val, sz, info, oth, shndx = struct.unpack('>IIIBBH', d[st['off'] + 16 * i: st['off'] + 16 * i + 16])
        n = d[strs['off'] + nm:]
        out.append((n[:n.index(b'\0')].decode(), val, shndx))
    return out

def build_image(path):
    d = open(path, 'rb').read()
    secs = elf_sections(d)
    syms = symbols(d, secs)
    sym = {n: v for n, v, _ in syms}
    end = sym['_image_end']
    img = bytearray(end)
    for s in secs:
        if s['flags'] & 2 and s['size'] and s['type'] == 1:
            img[s['addr']:s['addr'] + s['size']] = d[s['off']:s['off'] + s['size']]
    relocs = set()
    for s in secs:
        if s['type'] != 4 or not secs[s['info']]['flags'] & 2:
            continue
        for i in range(s['size'] // 12):
            off, info, add = struct.unpack('>IIi', d[s['off'] + 12 * i: s['off'] + 12 * i + 12])
            typ, si = info & 0xff, info >> 8
            shndx = syms[si][2] if si < len(syms) else 0
            if typ == R_68K_32:
                if shndx == SHN_ABS:
                    continue
                if off + 4 > end:
                    sys.exit(f"relocation at {off:#x} outside the image")
                relocs.add(off)
            elif typ in (2, 3, 6, 7, 8) and shndx != SHN_ABS:
                sys.exit(f"absolute relocation type {typ} at {off:#x}: build with -fno-pic, 32-bit addresses")
    img += b'\0' * ((4 - len(img) % 4) % 4)
    table = len(img)
    img += struct.pack('>I', len(relocs)) + b''.join(struct.pack('>I', o) for o in sorted(relocs))
    ro = sym['_reloc_off']
    img[ro:ro + 4] = struct.pack('>I', table)
    return bytes(img), len(relocs)

ap = argparse.ArgumentParser()
ap.add_argument('elf'); ap.add_argument('--name', required=True); ap.add_argument('--creator', required=True)
ap.add_argument('--size', type=int, default=384, help='the partition, KB')
ap.add_argument('--rsrc', action='append', default=[], help='TYPE:ID:file, another resource')
ap.add_argument('-o', required=True)
a = ap.parse_args()
img, nrel = build_image(a.elf)
code1 = struct.pack('>HH', 0, 1) + img
jt = struct.pack('>H', 0) + b'\x3f\x3c' + struct.pack('>H', 1) + b'\xa9\xf0'      # move.w #1,-(sp); _LoadSeg
code0 = struct.pack('>IIII', 0x20 + len(jt), 0x2000, len(jt), 0x20) + jt
rf = ResourceFork()
rf.add(b'CODE', 0, code0)
rf.add(b'CODE', 1, code1, attrs=0x20)          # preload
rf.add(b'SIZE', -1, struct.pack('>HII', 0x5880, a.size * 1024, a.size * 1024))
for r in a.rsrc:
    t, i, f = r.split(':', 2)
    rf.add(t.encode('mac_roman'), int(i), open(f, 'rb').read())
creator = a.creator.encode('mac_roman')[:4].ljust(4, b' ')
open(a.o, 'wb').write(macbinary(a.name, b'APPL', creator, b'', rf.build()))
print(f"{a.o}: CODE 1 {len(code1)} bytes, {nrel} relocations")
