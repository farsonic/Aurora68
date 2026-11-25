#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""mkblank.py - a blank hard disk image for a virtual SCSI disk (scsi.disk=NAME:ID, docs/08 item 148).

25 November 2026. A/UX installs onto a SCSI disk its own installer partitions: HD SC Setup (the A/UX
version) writes the Apple partition map, the driver and the A/UX partitions itself, so the image starts as
nothing but zeros of the wanted size. This makes that file. On Linux and macOS it is sparse (no blocks
written until something is), so a 2 GB image costs nothing until it is copied onto the card, where it takes
its full size (FAT32 has no sparse files: the copy writes the zeros).

    python3 tools/mac/mkblank.py AUX.hda 2G          2 GiB (2,147,483,648 bytes)
    python3 tools/mac/mkblank.py AUX.hda 1500M       1500 MiB
    python3 tools/mac/mkblank.py AUX.hda 1000000000  bytes (rounded down to 512)

Then put the file in the SD card's root and add scsi.disk=AUX.hda:ID (a free SCSI ID, 0 to 6) to
cmdline.txt. The Mac sees an empty SCSI disk at that ID. Mind the limits: a file on a FAT32 card is at most
4 GiB less a byte; A/UX 3 wants at least about 160 MB for a full installation, and its HD SC Setup asks for
no more than 2 GB. The same can be done with dd:  dd if=/dev/zero of=AUX.hda bs=1M count=0 seek=2048
Standard library only.
"""
import os, sys


def size_of(s):
    s = s.strip().upper()
    mult = 1
    for suf, m in (('K', 1 << 10), ('M', 1 << 20), ('G', 1 << 30)):
        if s.endswith(suf) or s.endswith(suf + 'B'):
            s = s[:s.index(suf)]
            mult = m
            break
    v = int(float(s) * mult)
    return v - v % 512


def main(argv):
    if len(argv) != 3 or argv[1].startswith('-'):
        raise SystemExit('usage: mkblank.py NAME SIZE   (SIZE: bytes, or with K, M or G)')
    name, size = argv[1], size_of(argv[2])
    if size < 1 << 20:
        raise SystemExit('a disk of less than 1 MB is no use')
    if size > (4 << 30) - 512:
        raise SystemExit('a file on a FAT32 card is at most 4 GiB less a byte: choose a smaller size')
    if os.path.exists(name):
        raise SystemExit(f'{name} exists: not overwritten (move it aside first)')
    with open(name, 'wb') as f:
        f.truncate(size)
    print(f'{name}: {size} bytes ({size // 512} blocks of 512, {size / (1 << 20):.0f} MB), all zeros: '
          f'copy it to the card\'s root and add scsi.disk={os.path.basename(name)}:ID to cmdline.txt')
    if size > 2 << 30:
        print('note: HD SC Setup and A/UX 3 may not use more than 2 GB of it')


if __name__ == '__main__':
    main(sys.argv)
