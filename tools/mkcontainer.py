#!/usr/bin/env python3
"""Build the Aurora68 initramfs container: a cpio "newc" archive of the Macintosh ROM, the slot-$9
declaration ROM and the Pi's firmware files, loaded by the Pi firmware as `initramfs` next to
kernel8.img (config.txt: `initramfs psmac.cpio followkernel`).

    tools/mkcontainer.py -o psmac.cpio rom/patched/MacIIci_psmac.rom mac/build/psmac.declrom \\
        firmware/brcmfmac43455-sdio.bin firmware/brcmfmac43455-sdio.txt firmware/brcmfmac43455-sdio.clm_blob \\
        firmware/BCM4345C0.hcd

The kernel (emu68/src/psmac/psmac_bus.c, walk_container) sorts the entries by suffix: *.rom is the
Macintosh ROM, *.declrom the declaration ROM, anything else a firmware file for the Alice68 modules,
found by its base name. A bare ROM file (no cpio header) still works, as it did on the first day.
The archive is uncompressed: the Pi firmware does not unpack it, the kernel reads it in place.
"""
import argparse, os, sys

def newc_entry(name, data, ino):
    nsize = len(name) + 1
    hdr = ('070701' + '%08x' % ino + '%08x' % 0o100644 + '%08x' * 2 % (0, 0) + '%08x' % 1 + '%08x' % 0 +
           '%08x' % len(data) + '%08x' * 4 % (0, 0, 0, 0) + '%08x' % nsize + '%08x' % 0).encode()
    out = hdr + name.encode() + b'\0'
    out += b'\0' * ((4 - len(out) % 4) % 4)
    out += data
    out += b'\0' * ((4 - len(out) % 4) % 4)
    return out

ap = argparse.ArgumentParser()
ap.add_argument('-o', required=True)
ap.add_argument('files', nargs='+')
a = ap.parse_args()
blob = b''
for i, f in enumerate(a.files):
    data = open(f, 'rb').read()
    blob += newc_entry(os.path.basename(f), data, i + 1)
    print(f"  {os.path.basename(f)}: {len(data)} bytes")
blob += newc_entry('TRAILER!!!', b'', len(a.files) + 1)
open(a.o, 'wb').write(blob)
print(f"{a.o}: {len(blob)} bytes, {len(a.files)} files")
