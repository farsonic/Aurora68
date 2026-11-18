"""ELF images for the classic Mac OS: a program linked at 0 with --emit-relocs, as one image
(.text, .data, the zeroed .bss up to _image_end) followed by a long count and the offsets of
every 32-bit absolute field; _reloc_off gets the table's offset. Shared by elf2app.py
(applications) and elf2init.py (system extensions). MPL-2.0."""
import struct, sys

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

