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
import argparse, os, sys, subprocess, zlib

STOCK_IICI_CRC = None     # filled below: the CRC-32 of the stock 368CADFE image is not needed, the ROM's own checksum word is

def is_stock_iici(data):
    """A 512 KiB IIci ROM whose checksum word is Apple's (368CADFE): unpatched, needs the patch set."""
    return len(data) == 0x80000 and data[:4] == bytes.fromhex('368CADFE')

def mac_checksum(d):
    """Apple's ROM checksum: the 32-bit sum of the 16-bit words from offset 4 to the end."""
    return sum(int.from_bytes(d[i:i + 2], 'big') for i in range(4, len(d), 2)) & 0xffffffff

def patch_rom(path):
    """Apply the kernel's patch set (timing calibration from the Pi, docs/06). First choice: the
    assembled delta next to this script (patches/iici-368CADFE.delta.json, byte ranges and the
    expected checksum), which needs nothing but Python; the first user to run this on a Mac had no
    m68k assembler for rompatch.py. Second choice: rompatch.py with the source patch set, where
    the assembler exists. Returns the patched image, or None if neither is available."""
    import json
    here = os.path.dirname(os.path.abspath(__file__))
    delta = os.path.join(here, 'patches', 'iici-368CADFE.delta.json')
    if os.path.exists(delta):
        d = json.load(open(delta))
        img = bytearray(open(path, 'rb').read())
        if len(img) != d['size'] or img[:4].hex().upper() != d['input_checksum']:
            sys.exit("the ROM is not the image the delta was made for")
        for off, hx in d['runs']:
            b = bytes.fromhex(hx); img[off:off + len(b)] = b
        if img[:4].hex().upper() != d['output_checksum'] or mac_checksum(img) != int(d['output_checksum'], 16):
            sys.exit("the patched ROM's checksum is not the expected one: the delta file is damaged")
        return bytes(img)
    tool = os.path.join(here, 'rompatch.py')
    pset = os.path.join(here, 'patches', 'iici.json')
    if not (os.path.exists(tool) and os.path.exists(pset)):
        return None
    out = path + '.patched'
    r = subprocess.run([sys.executable, tool, pset, path, out], capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout); print(r.stderr, file=sys.stderr)
        sys.exit("ROM patching failed")
    data = open(out, 'rb').read()
    os.unlink(out)
    return data

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
ap.add_argument('--no-patch', action='store_true', help='pack the ROM as given, even a stock one')
ap.add_argument('files', nargs='+')
a = ap.parse_args()
blob = b''
for i, f in enumerate(a.files):
    data = open(f, 'rb').read()
    if f.lower().endswith('.rom') and is_stock_iici(data) and not a.no_patch:
        # A stock IIci ROM: the kernel needs the patch set (the timing calibration taken from the
        # Pi instead of measured with loops the JIT runs too fast; docs/06). 0.1 and 0.2 packed the
        # stock image when given one and left System 7.1 to a zero divide on some boots.
        patched = patch_rom(f)
        if patched is None:
            sys.exit("the ROM is a stock 368CADFE image and patches/iici-368CADFE.delta.json is not next to this script: cannot patch it")
        print(f"  {os.path.basename(f)}: stock 368CADFE image, patch set applied (checksum now {patched[:4].hex().upper()})")
        data = patched
    blob += newc_entry(os.path.basename(f), data, i + 1)
    print(f"  {os.path.basename(f)}: {len(data)} bytes")
blob += newc_entry('TRAILER!!!', b'', len(a.files) + 1)
open(a.o, 'wb').write(blob)
print(f"{a.o}: {len(blob)} bytes, {len(a.files)} files")
