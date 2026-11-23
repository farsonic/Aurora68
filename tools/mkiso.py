#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""tools/mkiso.py OUT.iso VOLUME FILE... - a small ISO 9660 image (level 1: 8.3 upper-case names, all in the root),
for the PC card's CD-ROM drive in tools/pccard_test.sh (docs/19-pc-card.md "Disks").

Backstory (23 November 2026). The PC card's Disks menu puts a CD image into the PC's CD-ROM drive while DOS runs; the
host test needs a CD to put in, and the machine it was written on had no genisoimage, mkisofs or xorriso. ECMA-119's
smallest form is a page of Python: sixteen empty sectors, the primary volume descriptor, the set terminator, the two
path tables (the root alone), the root directory, then each file's sectors."""
import os
import sys
import time

S = 2048


def both16(v): return v.to_bytes(2, 'little') + v.to_bytes(2, 'big')
def both32(v): return v.to_bytes(4, 'little') + v.to_bytes(4, 'big')


def stamp7(t):
    return bytes([t.tm_year - 1900, t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, 0])


def record(name, extent, size, is_dir, t):
    n = len(name)
    r = bytes([0, 0]) + both32(extent) + both32(size) + stamp7(t) + bytes([2 if is_dir else 0, 0, 0]) + both16(1) + bytes([n]) + name
    if len(r) % 2:
        r += b'\0'
    return bytes([len(r)]) + r[1:]


def main():
    out, vol, files = sys.argv[1], sys.argv[2].upper()[:32], sys.argv[3:]
    t = time.gmtime(0)                                      # a fixed date: the same files make the same image
    datas = [open(f, 'rb').read() for f in files]
    names = [os.path.basename(f).upper().encode('ascii') + b';1' for f in files]
    root, first = 20, 21
    extents, at = [], first
    for d in datas:
        extents.append(at)
        at += max(1, (len(d) + S - 1) // S)
    total = at
    rootdir = record(b'\0', root, S, True, t) + record(b'\1', root, S, True, t)
    for n, e, d in zip(names, extents, datas):
        rootdir += record(n, e, len(d), False, t)
    assert len(rootdir) <= S, 'too many files for one directory sector'
    date17 = b'1970010100000000\0'
    pvd = bytearray(S)
    pvd[0:7] = b'\x01CD001\x01'
    pvd[8:40] = b' ' * 32
    pvd[40:72] = vol.encode('ascii').ljust(32)
    pvd[80:88] = both32(total)
    pvd[120:124] = both16(1)
    pvd[124:128] = both16(1)
    pvd[128:132] = both16(S)
    pvd[132:140] = both32(10)
    pvd[140:144] = (18).to_bytes(4, 'little')
    pvd[148:152] = (19).to_bytes(4, 'big')
    pvd[156:190] = record(b'\0', root, S, True, t)
    for a, b in ((190, 318), (318, 446), (446, 574), (574, 702), (702, 739), (739, 776), (776, 813)):
        pvd[a:b] = b' ' * (b - a)
    for a in (813, 830, 864):
        pvd[a:a + 17] = date17
    pvd[847:864] = b'0000000000000000\0'
    pvd[881] = 1
    term = bytearray(S)
    term[0:7] = b'\xffCD001\x01'
    lpt = bytes([1, 0]) + root.to_bytes(4, 'little') + (1).to_bytes(2, 'little') + b'\0\0'
    mpt = bytes([1, 0]) + root.to_bytes(4, 'big') + (1).to_bytes(2, 'big') + b'\0\0'
    img = bytearray(S * total)
    img[16 * S:17 * S] = pvd
    img[17 * S:18 * S] = term
    img[18 * S:18 * S + len(lpt)] = lpt
    img[19 * S:19 * S + len(mpt)] = mpt
    img[root * S:root * S + len(rootdir)] = rootdir
    for e, d in zip(extents, datas):
        img[e * S:e * S + len(d)] = d
    open(out, 'wb').write(img)


if __name__ == '__main__':
    main()
