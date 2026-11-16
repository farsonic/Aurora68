#!/usr/bin/env python3
"""Make the Iomega 4.2 Zip driver on a Zip disk image accept any SCSI disk.
Patches the 'bne' after the strncmp(product,"ZIP 100",7) in Zip100::Make (driver partition
starting at block 91 -> blob offset 0xC116) to a NOP. usage: iomega_patch.py in.image out.image"""
import sys, struct
d = bytearray(open(sys.argv[1], 'rb').read())
drv_block = struct.unpack('>I', d[18:22])[0]           # DDR ddBlock
off = drv_block*512 + 0xC116
if d[off:off+2] != b'\x66\x20': sys.exit(f"unexpected bytes {d[off:off+2].hex()} at {off:#x}; different driver version?")
d[off:off+2] = b'\x4e\x71'
open(sys.argv[2], 'wb').write(d); print(f"patched {off:#x}")
