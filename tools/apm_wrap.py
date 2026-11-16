#!/usr/bin/env python3
"""Wrap a bare HFS volume into a bootable Apple-Partition-Map image, reusing the DDR, partition
map, driver and auxiliary partitions of a donor image (e.g. a Zip disk image).
usage: apm_wrap.py donor.image volume.hfs out.image
Layout of the result: donor blocks [0, hfs_start) verbatim (DDR, PM, DiskInfo, Apple_Driver),
then the HFS volume (padded to 512), then the donor's partitions that followed the HFS one,
with PM entries and the DDR block count rewritten."""
import sys, struct
donor = bytearray(open(sys.argv[1], 'rb').read()); vol = open(sys.argv[2], 'rb').read()
vol += b'\0' * (-len(vol) % 512); vblocks = len(vol) // 512
pm = []
n = struct.unpack('>I', donor[512+4:512+8])[0]
for i in range(1, n+1):
    e = donor[512*i:512*(i+1)]
    pm.append(dict(idx=i, start=struct.unpack('>I', e[8:12])[0], count=struct.unpack('>I', e[12:16])[0],
                   typ=e[48:80].rstrip(b'\0').decode('mac_roman'), raw=bytearray(e)))
hfs = next(p for p in pm if p['typ'] == 'Apple_HFS')
head = donor[:hfs['start']*512]
tail_parts = [p for p in pm if p['start'] > hfs['start']]
out = bytearray(head) + bytearray(vol)
cur = hfs['start'] + vblocks
for p in tail_parts:
    out += donor[p['start']*512:(p['start']+p['count'])*512]
    p['new_start'] = cur; cur += p['count']
total = cur
# rewrite PM entries
for p in pm:
    e = p['raw']
    if p is hfs:
        struct.pack_into('>II', e, 8, p['start'], vblocks); struct.pack_into('>I', e, 84, vblocks)  # pmPartBlkCnt, pmDataCnt
    elif 'new_start' in p:
        struct.pack_into('>I', e, 8, p['new_start'])
    out[512*p['idx']:512*(p['idx']+1)] = e
struct.pack_into('>I', out, 4, total)  # DDR sbBlkCount
open(sys.argv[3], 'wb').write(out)
print(f"wrote {sys.argv[3]}: {total} blocks ({total*512/1024/1024:.1f} MiB); HFS at {hfs['start']} x {vblocks}; tail: " + ', '.join(f"{p['typ']}@{p['new_start']}" for p in tail_parts))
