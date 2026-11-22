#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""swap_driver.py - give an Apple-partitioned disk image another disk's SCSI driver (Apple_Driver43).

22 November 2026. Every card image of this project descends from the first test image, which was made
from an Iomega Zip disk: its partition map holds Iomega_DiskInfo, Iomega's Zip 4.2 driver (patched by
tools/iomega_patch.py to take any disk), the HFS volume, Iomega_backup and a second DiskInfo. On the
Pi's own disk path that works (psmac_board_solo.c answers Iomega's vendor command $06 as a Zip drive
does). On the SE's real SCSI bus the driver talks to a BlueSCSI, which is no Zip drive, and the owner's
SE offered to initialize the disk. Apple's driver (Apple_Driver43, from HD SC Setup or any disk it
formatted) drives any SCSI disk.

This keeps the image's HFS volume byte for byte and puts it behind the donor's driver map, partition
map and driver partition (the donor's blocks before its HFS partition): the result has the donor's
layout, the image's volume, and nothing of Iomega's. The donor is a disk of the owner's own (Apple's
driver is Apple's and is not in this repository), e.g. the BlueSCSI's own startup disk image.

    python3 tools/mac/swap_driver.py DONOR.hda IMAGE OUT.hda          IMAGE's volume, DONOR's driver
    python3 tools/mac/swap_driver.py --show IMAGE                     its partitions and driver

Standard library only.
"""
import struct, sys


def partitions(d):
    """(DDR drivers [(block, size, type)], [entry dicts]) of an image's first blocks"""
    if d[:2] != b'ER':
        raise SystemExit('no driver descriptor (block 0 does not start with ER): not an Apple-partitioned image')
    drivers = [struct.unpack('>IHH', d[18 + 8 * i:26 + 8 * i]) for i in range(struct.unpack('>H', d[16:18])[0])]
    if d[512:514] != b'PM':
        raise SystemExit('no partition map at block 1')
    n = struct.unpack('>I', d[516:520])[0]
    out = []
    for i in range(1, n + 1):
        e = d[512 * i:512 * (i + 1)]
        out.append({'index': i, 'start': struct.unpack('>I', e[8:12])[0], 'count': struct.unpack('>I', e[12:16])[0],
                    'name': e[16:48].rstrip(b'\0').decode('mac_roman'), 'type': e[48:80].rstrip(b'\0').decode('mac_roman'),
                    'raw': bytearray(e)})
    return drivers, out


def show(path):
    with open(path, 'rb') as f:
        head = f.read(512 * 128)
    drivers, parts = partitions(head)
    print(f'{path}: drivers {drivers}')
    for p in parts:
        print(f'  {p["index"]:2} {p["type"]:22} {p["name"]:20} blocks {p["start"]}-{p["start"] + p["count"] - 1}')
    kinds = {p['type'] for p in parts}
    print('  driver: ' + ('Iomega (Zip): not for a BlueSCSI' if any(t.startswith('Iomega') for t in kinds) else
                          'Apple_Driver43' if 'Apple_Driver43' in kinds else 'other'))


def main():
    if len(sys.argv) == 3 and sys.argv[1] == '--show':
        return show(sys.argv[2])
    if len(sys.argv) != 4:
        raise SystemExit(__doc__.split('\n\n')[-2])
    donor_path, image_path, out_path = sys.argv[1:]
    with open(donor_path, 'rb') as f:
        donor_head = f.read(512 * 256)
    ddrv, dparts = partitions(donor_head)
    if not any(p['type'] == 'Apple_Driver43' for p in dparts):
        raise SystemExit(f'{donor_path} has no Apple_Driver43 partition to give')
    dhfs = next(p for p in dparts if p['type'] == 'Apple_HFS')
    if any(p['start'] < dhfs['start'] and p['start'] + p['count'] > dhfs['start'] for p in dparts if p is not dhfs):
        raise SystemExit('the donor has a partition overlapping its HFS one: not a layout to copy')
    with open(image_path, 'rb') as f:
        img = f.read()
    _, iparts = partitions(img)
    ihfs = [p for p in iparts if p['type'] == 'Apple_HFS']
    if len(ihfs) != 1:
        raise SystemExit(f'{image_path} has {len(ihfs)} HFS partitions; one is needed')
    ihfs = ihfs[0]
    vol = img[ihfs['start'] * 512:(ihfs['start'] + ihfs['count']) * 512]
    if vol[1024:1026] != b'BD':
        raise SystemExit(f'{image_path}: the HFS partition has no volume signature (BD) at its block 2')
    head = bytearray(donor_head[:dhfs['start'] * 512])
    # the donor's map, with its HFS entry sized to the volume and anything after it dropped
    keep = [p for p in dparts if p['start'] < dhfs['start'] or p is dhfs]
    hfs_end = dhfs['start'] + ihfs['count']
    for p in dparts:                                  # entries after the HFS one: emptied
        if p not in keep:
            head[512 * p['index']:512 * (p['index'] + 1)] = bytes(512)
    for p in keep:
        e = p['raw']
        struct.pack_into('>I', e, 4, len(keep))      # pmMapBlkCnt: the entries in the map
        if p is dhfs:
            struct.pack_into('>II', e, 8, dhfs['start'], ihfs['count'])
            struct.pack_into('>II', e, 80, 0, ihfs['count'])        # pmDataStart, pmDataCnt
        head[512 * p['index']:512 * (p['index'] + 1)] = e
    # renumber: the kept entries must be 1..n with no gap
    entries = sorted(keep, key=lambda p: p['index'])
    if [p['index'] for p in entries] != list(range(1, len(entries) + 1)):
        raise SystemExit('the donor\'s map has entries after its HFS one in between: not a layout to copy')
    struct.pack_into('>I', head, 4, hfs_end)          # sbBlkCount: the disk's blocks
    with open(out_path, 'wb') as f:
        f.write(head)
        f.write(vol)
    print(f'{out_path}: {image_path}\'s volume ({ihfs["count"]} blocks, {ihfs["count"] * 512 / 1048576:.1f} MiB) behind '
          f'{donor_path}\'s Apple_Driver43; {hfs_end} blocks in all')
    show(out_path)


if __name__ == '__main__':
    main()
