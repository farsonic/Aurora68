#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""mkexfat.py - an exFAT volume made from a directory tree, for the host tests of the kernel's FAT and
exFAT reader (emu68/src/psmac/psmac_fatfs.c) and its HFS view (psmac_hfsview.c).

Why: mkfs.exfat makes an empty volume and mtools writes FAT only, and the test machine cannot mount
exFAT. So this writes the whole volume itself, after Microsoft's exFAT specification (2019): the main
and backup boot regions with their checksum sector, one FAT, the allocation bitmap, an up-case table,
the root directory with the label, bitmap and up-case entries, and a File / Stream Extension / File
Name entry set for every file and folder, with the set checksum and the name hash. Files are
contiguous and marked NoFatChain, as macOS and Windows write them; a file or folder whose name contains
"frag" gets its clusters spread out with gaps between them and a FAT chain, so both ways of finding
clusters are tested. fsck.exfat (exfatprogs) checks the result in the test script.

usage: mkexfat.py OUT.img SIZE_MB SOURCE_DIR [--label NAME] [--cluster BYTES] [--mbr | --gpt] [--nfd]
  --mbr puts an MBR in front, the volume in partition 1 (type $07) at 1 MiB, as a stick from a PC has.
  --gpt a GUID partition table as Disk Utility on a modern Mac writes one: a 20 MB EFI system partition at
        1 MiB (left empty here: the test script formats it FAT16, to see that the reader passes it over),
        then the volume as a Microsoft basic data partition.
  --nfd names decomposed (NFD), as macOS writes them.
Standard library only, like everything in tools/.
"""
import os, struct, sys, time, unicodedata, uuid, zlib

SEC = 512

def boot_sum(data):
    c = 0
    for i, b in enumerate(data):
        if i in (106, 107, 112):
            continue
        c = (((c >> 1) | ((c & 1) << 31)) + b) & 0xffffffff
    return c

def sum32(data):
    c = 0
    for b in data:
        c = (((c >> 1) | ((c & 1) << 31)) + b) & 0xffffffff
    return c

def sum16(data, skip=()):
    c = 0
    for i, b in enumerate(data):
        if i in skip:
            continue
        c = (((c >> 1) | ((c & 1) << 15)) + b) & 0xffff
    return c

def up1(i):
    """the character's single upper-case form, or itself (no surrogates, no expansions such as German sharp s)"""
    if 0xd800 <= i <= 0xdfff:
        return i
    u = chr(i).upper()
    return ord(u) if len(u) == 1 and ord(u) < 0x10000 else i

def upcase_table():
    """the up-case table, compressed: identity runs as $FFFF, length (section 7.2.5.1)"""
    out = []
    i = 0
    while i < 0x10000:
        if up1(i) == i:
            j = i
            while j < 0x10000 and up1(j) == j:
                j += 1
            if j - i > 2:
                out += [0xffff, j - i]
                i = j
                continue
        out.append(up1(i))
        i += 1
    return struct.pack('<%dH' % len(out), *out)

def dos_time(t):
    lt = time.localtime(t)
    if lt.tm_year < 1980:
        lt = time.localtime(315532800)
    return ((lt.tm_year - 1980) << 25 | lt.tm_mon << 21 | lt.tm_mday << 16 | lt.tm_hour << 11 | lt.tm_min << 5 | lt.tm_sec // 2)

class Vol:
    def __init__(self, size_mb, cluster):
        self.cs = cluster
        self.total = size_mb * 2048
        self.fat_off = 128
        self.heap_off = 0
        n = self.total // (cluster // SEC)
        self.fat_len = ((n + 2) * 4 + SEC - 1) // SEC
        self.heap_off = ((self.fat_off + self.fat_len + 127) // 128) * 128
        self.ccount = (self.total - self.heap_off) // (cluster // SEC)
        self.fat = [0] * (self.ccount + 2)
        self.fat[0], self.fat[1] = 0xfffffff8, 0xffffffff
        self.used = [False] * (self.ccount + 2)
        self.next = 2
        self.out = None                     # the image, open from the start: data goes straight in
        self.base = 0

    def alloc(self, n, frag):
        cl = []
        while len(cl) < n:
            if self.next >= self.ccount + 2:
                sys.exit('mkexfat: volume full')
            cl.append(self.next)
            self.used[self.next] = True
            self.next += 2 if frag else 1
        if frag:
            for a, b in zip(cl, cl[1:]):
                self.fat[a] = b
            self.fat[cl[-1]] = 0xffffffff
        return cl

    def at(self, c):
        return self.base + (self.heap_off + (c - 2) * (self.cs // SEC)) * SEC

    def write(self, cl, payload):
        for k, c in enumerate(cl):
            self.out.seek(self.at(c)); self.out.write(payload[k * self.cs:(k + 1) * self.cs])

    def write_file(self, cl, path):
        """a file's data a cluster at a time; clusters of zeros are skipped (the image is sparse), so a
        sparse source of gigabytes costs neither memory nor disk"""
        with open(path, 'rb') as f:
            for c in cl:
                d = f.read(self.cs)
                if d.strip(b'\0'):
                    self.out.seek(self.at(c)); self.out.write(d)

def name_entries(name, attr, first, length, contig, mtime, ctime):
    u16 = name.encode('utf-16-le')
    nlen = len(u16) // 2
    up = struct.pack('<%dH' % nlen, *[up1(x) for x in struct.unpack('<%dH' % nlen, u16)])
    nh = sum16(up)
    nn = (nlen + 14) // 15
    f = bytearray(32)
    f[0] = 0x85; f[1] = 1 + nn
    struct.pack_into('<H', f, 4, attr)
    struct.pack_into('<III', f, 8, ctime, mtime, mtime)
    s = bytearray(32)
    s[0] = 0xc0; s[1] = 0x01 | (0x02 if contig else 0); s[3] = nlen
    struct.pack_into('<H', s, 4, nh)
    struct.pack_into('<Q', s, 8, length)
    struct.pack_into('<I', s, 20, first)
    struct.pack_into('<Q', s, 24, length)
    ents = [f, s]
    for k in range(nn):
        e = bytearray(32)
        e[0] = 0xc1
        part = u16[k * 30:(k + 1) * 30]
        e[2:2 + len(part)] = part
        ents.append(e)
    blob = b''.join(ents)
    struct.pack_into('<H', f, 2, sum16(blob, skip=(2, 3)))
    return b''.join(ents)

def build_dir(v, path, is_root, label=None):
    """the directory's entries; files and folders below are written first"""
    ents = b''
    if is_root:
        lab = bytearray(32); lab[0] = 0x83
        ln = (label or 'EXFAT')[:11]
        lab[1] = len(ln); lab[2:2 + 2 * len(ln)] = ln.encode('utf-16-le')
        ents += bytes(lab)
        ents += v.bitmap_entry + v.upcase_entry
    for name in sorted(os.listdir(path)):
        full = os.path.join(path, name)
        st = os.stat(full)
        frag = 'frag' in name
        nfc = unicodedata.normalize('NFC', name) if '--nfd' not in sys.argv else unicodedata.normalize('NFD', name)
        if os.path.isdir(full):
            sub = build_dir(v, full, False)
            n = max(1, (len(sub) + v.cs - 1) // v.cs)
            cl = v.alloc(n, frag)
            v.write(cl, sub.ljust(n * v.cs, b'\0'))
            ents += name_entries(nfc, 0x10, cl[0], n * v.cs, not frag, dos_time(st.st_mtime), dos_time(st.st_mtime))
        else:
            size = st.st_size
            if size:
                n = (size + v.cs - 1) // v.cs
                cl = v.alloc(n, frag)
                v.write_file(cl, full)
                first = cl[0]
            else:
                first = 0
            attr = 0x20 | (0x02 if name.startswith('hidden') else 0)
            ents += name_entries(nfc, attr, first, size, not frag and bool(size), dos_time(st.st_mtime), dos_time(st.st_mtime))
    return ents

def main():
    a = sys.argv[1:]
    out, size_mb, src = a[0], int(a[1]), a[2]
    label = a[a.index('--label') + 1] if '--label' in a else 'EXFAT'
    cs = int(a[a.index('--cluster') + 1]) if '--cluster' in a else 4096
    mbr = '--mbr' in a
    gpt = '--gpt' in a
    v = Vol(size_mb - (1 if mbr else 22 if gpt else 0), cs)
    part = 2048 if mbr else 2048 + 40960 if gpt else 0
    v.base = part * SEC
    v.out = open(out, 'w+b')
    v.out.truncate((v.total + part) * SEC)
    # cluster 2.. : the bitmap, the up-case table, then the root (FAT chain, as the root always has)
    bm_bytes = (v.ccount + 7) // 8
    bm_cl = v.alloc((bm_bytes + cs - 1) // cs, False)
    up = upcase_table()
    up_cl = v.alloc((len(up) + cs - 1) // cs, False)
    for cl in (bm_cl, up_cl):
        for a1, b1 in zip(cl, cl[1:]):
            v.fat[a1] = b1
        v.fat[cl[-1]] = 0xffffffff
    e = bytearray(32); e[0] = 0x81
    struct.pack_into('<IQ', e, 20, bm_cl[0], bm_bytes)
    v.bitmap_entry = bytes(e)
    e = bytearray(32); e[0] = 0x82
    struct.pack_into('<I', e, 4, sum32(up))
    struct.pack_into('<IQ', e, 20, up_cl[0], len(up))
    v.upcase_entry = bytes(e)
    v.write(up_cl, up.ljust(len(up_cl) * cs, b'\0'))
    # the root: entries are known only after its children are placed, so its clusters come after them
    root = build_dir(v, src, True, label)
    rn = max(1, (len(root) + cs - 1) // cs)
    rcl = v.alloc(rn, False)
    for a1, b1 in zip(rcl, rcl[1:]):
        v.fat[a1] = b1
    v.fat[rcl[-1]] = 0xffffffff
    v.write(rcl, root.ljust(rn * cs, b'\0'))
    bm = bytearray(bm_bytes)
    for c in range(2, v.ccount + 2):
        if v.used[c]:
            bm[(c - 2) // 8] |= 1 << ((c - 2) % 8)
    v.write(bm_cl, bytes(bm).ljust(len(bm_cl) * cs, b'\0'))
    # the boot region
    bs = bytearray(512)
    bs[0:3] = b'\xeb\x76\x90'; bs[3:11] = b'EXFAT   '
    struct.pack_into('<QQIIIIIIHH', bs, 64, part, v.total, v.fat_off, v.fat_len, v.heap_off, v.ccount, rcl[0], 0x1234abcd, 0x0100, 0)
    bs[108] = 9; bs[109] = (cs // SEC).bit_length() - 1; bs[110] = 1; bs[111] = 0x80
    bs[112] = min(100, 100 * (v.next - 2) // v.ccount)
    bs[510:512] = b'\x55\xaa'
    region = bytes(bs)
    for _ in range(8):
        region += b'\0' * 508 + b'\x00\x00\x55\xaa'
    region += b'\0' * 1024
    c = boot_sum(region)
    region += struct.pack('<I', c) * 128
    with v.out as f:
        if mbr:
            m = bytearray(512)
            m[446:462] = bytes([0, 0, 0, 0, 0x07, 0, 0, 0]) + struct.pack('<II', part, v.total)
            m[510:512] = b'\x55\xaa'
            f.seek(0); f.write(m)
        if gpt:
            last = v.total + part + 2048 - 1
            f.truncate((last + 1) * SEC)
            m = bytearray(512)
            m[446:462] = bytes([0, 0, 2, 0, 0xee, 0xff, 0xff, 0xff]) + struct.pack('<II', 1, min(last, 0xffffffff))
            m[510:512] = b'\x55\xaa'
            f.seek(0); f.write(m)
            ents = bytearray(128 * 128)
            for k, (t, st, en, nm) in enumerate([('C12A7328-F81F-11D2-BA4B-00A0C93EC93B', 2048, 2048 + 40960 - 1, 'EFI System Partition'),
                                                ('EBD0A0A2-B9E5-4433-87C0-68B6B72699C7', part, part + v.total - 1, 'Stick')]):
                struct.pack_into('<16s16sQQQ', ents, 128 * k, uuid.UUID(t).bytes_le, uuid.uuid4().bytes_le, st, en, 0)
                n16 = nm.encode('utf-16-le'); ents[128 * k + 56:128 * k + 56 + len(n16)] = n16
            h = bytearray(512)
            struct.pack_into('<8sIIIIQQQQ16sQIII', h, 0, b'EFI PART', 0x10000, 92, 0, 0, 1, last, 34, last - 33,
                             uuid.uuid4().bytes_le, 2, 128, 128, zlib.crc32(ents))
            struct.pack_into('<I', h, 16, zlib.crc32(bytes(h[:92])))
            f.seek(SEC); f.write(h)
            f.seek(2 * SEC); f.write(ents)
        base = part * SEC
        f.seek(base); f.write(region)
        f.seek(base + 12 * SEC); f.write(region)
        f.seek(base + v.fat_off * SEC)
        f.write(struct.pack('<%dI' % len(v.fat), *v.fat))
    print('mkexfat: %s, %d MB, %d clusters of %d bytes, %d used' % (out, size_mb, v.ccount, cs, sum(v.used)))

if __name__ == '__main__':
    main()
