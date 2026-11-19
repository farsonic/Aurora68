#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""mkpartimg.py: a small Apple-partitioned HFS disk image that boots no System but mounts beside
one, made from a base image's own driver.

    mkpartimg.py BASE.image OUT.img [--blocks 4096] [--label "Riptide Update"]
                 [--put FILE.bin[:FOLDER]] ... [--text FILE[:NAME]] ...

Why it exists. Test set 4 (19 November 2026) had to bring one file, the new Riptide, to a Mac that
was booting from a disk image on its SD card. A second image beside the first was the way
(cmdline.txt's mac.image=A,B: the first the startup disk, the next ones PiDisk volumes, docs/08
item 52), and the image that mounted everywhere was one shaped like the startup disk itself: the
base image's driver descriptor map, its partition map and the partitions before its HFS one
(on the test images an Iomega_DiskInfo and the Iomega Apple_Driver, blocks 0-490), then a fresh
HFS partition. This is that recipe, as it was done by hand then:

  - blocks 0 .. (start of the base's first Apple_HFS partition) - 1 copied as they are;
  - the driver descriptor map's block count (sbBlkCount) set to the new size;
  - the map kept up to and including that Apple_HFS entry; every entry's map count (pmMapBlkCnt)
    set to the new number of entries; the HFS entry's size (pmPartBlkCnt, pmDataCnt) set to
    --blocks; the entries after it (Iomega_backup, the second DiskInfo) cleared;
  - the HFS partition formatted with hformat and the files copied in with hcopy (MacBinary for
    --put, text with Mac line ends for --text).

Needs hfsutils. Standard library only. Made images mount with hmount and on the solo board, as a
SCSI disk with its driver and as a PiDisk volume (served as its first Apple_HFS partition).
"""
import argparse, os, struct, subprocess, sys

BLK = 512


def read_map(d):
    """The partition map entries of an image's first blocks: (block, type, start, count)."""
    if d[:2] != b'ER':
        sys.exit('mkpartimg: the base has no driver descriptor map (block 0 is not "ER"): not Apple-partitioned')
    out = []
    for i in range(1, 64):
        b = d[BLK * i:BLK * i + BLK]
        if len(b) < BLK or b[:2] != b'PM':
            break
        mapcnt, start, count = struct.unpack('>III', b[4:16])
        out.append((i, b[48:80].split(b'\0')[0].decode('latin-1'), start, count, mapcnt))
    if not out:
        sys.exit('mkpartimg: no partition map entries after block 0')
    return out


def run(*cmd):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode:
        sys.exit('mkpartimg: %s failed:\n%s' % (' '.join(cmd), r.stdout))
    return r.stdout


def main():
    ap = argparse.ArgumentParser(usage='mkpartimg.py BASE.image OUT.img [--blocks N] [--label NAME] '
                                       '[--put FILE.bin[:FOLDER]]... [--text FILE[:NAME]]...',
                                 description='A small Apple-partitioned HFS image with BASE\'s driver partitions.')
    ap.add_argument('base'); ap.add_argument('out')
    ap.add_argument('--blocks', type=int, default=4096, help='size of the HFS partition in 512-byte blocks (default 4096, 2 MB)')
    ap.add_argument('--label', default='Update', help='the volume name (at most 27 characters)')
    ap.add_argument('--put', action='append', default=[], metavar='FILE.bin[:FOLDER]', help='a MacBinary file, into FOLDER (made if missing) or the top level')
    ap.add_argument('--text', action='append', default=[], metavar='FILE[:NAME]', help='a text file, as a TEXT/ttxt document called NAME')
    a = ap.parse_args()

    if len(a.label) > 27:
        sys.exit('mkpartimg: an HFS volume name has at most 27 characters')
    with open(a.base, 'rb') as f:
        head = f.read(BLK * 64)
    entries = read_map(head)
    hfs = [e for e in entries if e[1] == 'Apple_HFS']
    if not hfs:
        sys.exit('mkpartimg: the base image has no Apple_HFS partition')
    hblk, _, hstart, _, _ = hfs[0]
    keep = [e for e in entries if e[0] <= hblk]
    for blk, typ, start, count, _ in keep[:-1]:
        if typ != 'Apple_partition_map' and start + count > hstart:
            sys.exit('mkpartimg: partition %s (%s) reaches past the HFS partition\'s start' % (blk, typ))
    total = hstart + a.blocks

    with open(a.base, 'rb') as f:
        img = bytearray(f.read(BLK * hstart))
    if len(img) != BLK * hstart:
        sys.exit('mkpartimg: the base image is shorter than its own map says')
    struct.pack_into('>I', img, 4, total)                               # sbBlkCount
    for blk, typ, start, count, _ in entries:
        o = BLK * blk
        if blk > hblk:
            img[o:o + BLK] = bytes(BLK)                                 # entries after the HFS one
            continue
        struct.pack_into('>I', img, o + 4, len(keep))                   # pmMapBlkCnt
        if blk == hblk:
            struct.pack_into('>I', img, o + 12, a.blocks)               # pmPartBlkCnt
            struct.pack_into('>I', img, o + 84, a.blocks)               # pmDataCnt
    tmp = a.out + '.part'
    with open(tmp, 'wb') as f:
        f.write(img)
        f.truncate(BLK * total)
    os.replace(tmp, a.out)

    run('hformat', '-l', a.label, a.out, '1')
    run('hmount', a.out, '1')
    try:
        for p in a.put:
            path, _, folder = p.partition(':') if not os.path.exists(p) else (p, '', '')
            if folder:
                subprocess.run(['hmkdir', ':' + folder], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            run('hcopy', '-m', path, ':' + (folder + ':' if folder else ''))
        for t in a.text:
            path, _, name = t.partition(':') if not os.path.exists(t) else (t, '', '')
            name = name or os.path.basename(path)
            run('hcopy', '-t', path, ':' + name)
            run('hattrib', '-t', 'TEXT', '-c', 'ttxt', ':' + name)
        listing = run('hls', '-l')
        vol = run('hvol')
    finally:
        subprocess.run(['humount'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print('%s: %d blocks (%d KB): %d map entries, %s blocks %d-%d "%s"' %
          (a.out, total, total // 2, len(keep), 'Apple_HFS', hstart, total - 1, a.label))
    for typ in [e[1] for e in keep]:
        print('  ' + typ)
    sys.stdout.write(listing)
    print([l for l in vol.splitlines() if 'free' in l][-1].strip() if 'free' in vol else '')


if __name__ == '__main__':
    main()
