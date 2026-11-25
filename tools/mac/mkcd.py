#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""mkcd.py - CD-ROM images of 2048-byte sectors, for testing the virtual CD-ROM drive (scsi.cd=, docs/08 item 148).

25 November 2026. Two kinds, both made from nothing (no CD is downloaded or copied):

    python3 tools/mac/mkcd.py iso OUT.iso FILE...
        an ISO 9660 volume (level 1: the files in the root directory under 8.3 names), as a PC or a Unix
        machine writes a data CD: the system area, the primary volume descriptor at sector 16, the
        terminator, both path tables, the root directory, then the files, each in whole sectors.
    python3 tools/mac/mkcd.py hfs OUT.cdr SIZE_MB [--name NAME] [FILE...]
        a Macintosh CD: block 0 a driver descriptor ('ER', 512-byte blocks, no driver), an Apple partition
        map (Apple_partition_map, Apple_HFS) in 512-byte blocks as Apple's own CDs have it, an HFS volume made
        by hformat in the second partition, the FILEs copied into it with hcopy -m (MacBinary keeps type,
        creator and both forks) or raw; the size a multiple of 2048. Needs hfsutils.
    python3 tools/mac/mkcd.py pad IMAGE OUT.cdr
        an existing disk image (an Apple-partitioned System disk with a driver, say) padded to whole 2048-byte
        sectors: a startable "CD" for the IIci ROM, which reads a CD's block 0 and its driver in 512-byte
        blocks (the drive starts at 512, as Apple's do).

Every sector of an ISO file that is not the file's is zeros. Standard library only (but hfsutils for hfs).
"""
import os, struct, subprocess, sys, time

SEC = 2048


def both16(v): return struct.pack('<H', v) + struct.pack('>H', v)
def both32(v): return struct.pack('<I', v) + struct.pack('>I', v)


def dtime(t):
    g = time.gmtime(t)
    return bytes([g.tm_year - 1900, g.tm_mon, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec, 0])


def ltime(t):
    g = time.gmtime(t)
    return time.strftime('%Y%m%d%H%M%S', g).encode() + b'00' + b'\0'


def dirrec(extent, size, flags, name, t):
    n = len(name)
    r = struct.pack('<BB', 33 + n + (1 - n % 2), 0) + both32(extent) + both32(size) + dtime(t) + bytes([flags, 0, 0]) + both16(1) + bytes([n]) + name
    if n % 2 == 0:
        r += b'\0'
    return r


def iso_name(path, taken):
    base = os.path.basename(path).upper()
    stem, _, ext = base.rpartition('.') if '.' in base else (base, '', '')
    ok = lambda s: ''.join(c if c.isalnum() or c == '_' else '_' for c in s)
    stem, ext = ok(stem)[:8] or 'FILE', ok(ext)[:3]
    name = stem + ('.' + ext if ext else '')
    k = 1
    while name in taken:
        k += 1
        tail = '_' + str(k)
        name = stem[:8 - len(tail)] + tail + ('.' + ext if ext else '')
    taken.add(name)
    return name


def make_iso(out, files, label='AURORA68_TEST'):
    t = time.time()
    taken, ents = set(), []
    for f in files:
        ents.append((iso_name(f, taken), f, os.path.getsize(f)))
    ents.sort()
    root_lba, lpt_lba, mpt_lba = 20, 18, 19
    # the root directory: . and .., then a record per file
    recs = []
    nxt = root_lba + 1
    for nm, f, size in ents:
        recs.append((nm, nxt, size))
        nxt += (size + SEC - 1) // SEC
    root = dirrec(root_lba, SEC, 2, b'\0', t) + dirrec(root_lba, SEC, 2, b'\1', t)
    for nm, lba, size in recs:
        r = dirrec(lba, size, 0, (nm + ';1').encode(), t)
        if len(root) % SEC + len(r) > SEC:
            raise SystemExit('too many files for one directory sector (this writer keeps it to one)')
        root += r
    total = nxt
    pt = lambda order: (struct.pack('<BBIH' if order == '<' else '>BBIH', 1, 0, root_lba, 1) + b'\0\0')
    pvd = bytearray(SEC)
    pvd[0:7] = b'\x01CD001\x01'
    pvd[8:40] = b' ' * 32
    pvd[40:72] = label.ljust(32)[:32].encode()
    pvd[80:88] = both32(total)
    pvd[120:124] = both16(1); pvd[124:128] = both16(1); pvd[128:132] = both16(SEC)
    pvd[132:140] = both32(10)
    pvd[140:144] = struct.pack('<I', lpt_lba); pvd[148:152] = struct.pack('>I', mpt_lba)
    pvd[156:190] = dirrec(root_lba, SEC, 2, b'\0', t)
    for a, b in ((190, 318), (318, 446), (446, 574), (574, 702)):
        pvd[a:b] = b' ' * (b - a)
    pvd[318:318 + 16] = b'AURORA68 MKCD.PY'
    for a in (702, 739, 776):
        pvd[a:a + 37] = b' ' * 37
    pvd[813:830] = ltime(t); pvd[830:847] = ltime(t)
    pvd[847:864] = b'0' * 16 + b'\0'; pvd[864:881] = b'0' * 16 + b'\0'
    pvd[881] = 1
    term = bytearray(SEC); term[0:7] = b'\xffCD001\x01'
    with open(out, 'wb') as o:
        o.write(bytes(16 * SEC))
        o.write(pvd); o.write(term)
        o.write(pt('<').ljust(SEC, b'\0')); o.write(pt('>').ljust(SEC, b'\0'))
        o.write(root.ljust(SEC, b'\0'))
        for nm, f, size in ents:
            with open(f, 'rb') as i:
                d = i.read()
            o.write(d + bytes((-len(d)) % SEC))
    print(f'{out}: ISO 9660, {total} sectors of 2048 ({total * SEC} bytes), {len(ents)} file(s): ' + ', '.join(e[0] for e in ents))


def pm_entry(n_entries, start, count, name, ptype, status=0x37):
    e = bytearray(512)
    e[0:2] = b'PM'
    struct.pack_into('>III', e, 4, n_entries, start, count)
    e[16:16 + len(name)] = name.encode('mac_roman')
    e[48:48 + len(ptype)] = ptype.encode('mac_roman')
    struct.pack_into('>II', e, 80, 0, count)       # pmLgDataStart, pmDataCnt
    struct.pack_into('>I', e, 88, status)          # valid, allocated, in use, readable, writable
    return bytes(e)


def make_hfs(out, size_mb, name, files):
    blocks = (int(size_mb * (1 << 20)) // SEC) * (SEC // 512)
    hfs_start = 64
    ddr = bytearray(512)
    ddr[0:2] = b'ER'
    struct.pack_into('>HI', ddr, 2, 512, blocks)   # sbBlkSize, sbBlkCount
    with open(out, 'wb') as o:
        o.write(ddr)
        o.write(pm_entry(2, 1, 63, 'Apple', 'Apple_partition_map'))
        o.write(pm_entry(2, hfs_start, blocks - hfs_start, name, 'Apple_HFS', 0x7f))
        o.truncate(blocks * 512)
    subprocess.run(['hformat', '-l', name, out, '1'], check=True, stdout=subprocess.DEVNULL)
    subprocess.run(['hmount', out, '1'], check=True, stdout=subprocess.DEVNULL)
    try:
        for f in files:                            # MacBinary where it is one, else the bytes as they are
            if not f.lower().endswith('.bin') or subprocess.run(['hcopy', '-m', f, ':'], stderr=subprocess.DEVNULL).returncode:
                subprocess.run(['hcopy', '-r', f, ':'], check=True)
        subprocess.run(['hls', '-l'], check=True)
    finally:
        subprocess.run(['humount'], check=False)
    print(f'{out}: Apple partition map, HFS "{name}" from block {hfs_start}, {blocks // 4} sectors of 2048 ({blocks * 512} bytes)')


def pad(image, out):
    with open(image, 'rb') as i:
        d = i.read()
    d += bytes((-len(d)) % SEC)
    with open(out, 'wb') as o:
        o.write(d)
    print(f'{out}: {image} padded to {len(d) // SEC} sectors of 2048 ({len(d)} bytes)')


def main(a):
    if len(a) >= 3 and a[1] == 'iso':
        make_iso(a[2], a[3:])
    elif len(a) >= 4 and a[1] == 'hfs':
        name, files, rest = 'Aurora68 CD', [], a[4:]
        while rest:
            if rest[0] == '--name' and len(rest) > 1:
                name, rest = rest[1], rest[2:]
            else:
                files.append(rest[0]); rest = rest[1:]
        make_hfs(a[2], float(a[3]), name, files)
    elif len(a) == 4 and a[1] == 'pad':
        pad(a[2], a[3])
    else:
        raise SystemExit('usage: mkcd.py iso OUT FILE... | hfs OUT SIZE_MB [--name NAME] [FILE...] | pad IMAGE OUT')


if __name__ == '__main__':
    main(sys.argv)
