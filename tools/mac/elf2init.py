#!/usr/bin/env python3
"""Pack a C system extension for the classic Mac OS (linked at 0 with --emit-relocs, its entry
first) as an INIT file, a MacBinary II file:

    elf2init.py wifimenu.elf --name "WiFi Menu" --creator PSWM -o WiFiMenu.bin

'INIT' 128 holds the image and its relocation table (tools/mac/macelf.py). The entry
(mac/wifimenu/entry.s) copies the image into a pointer in the system heap, adds the copy's
address to every field the table names, and calls the C side there, so the INIT resource itself
can go when the Start Manager is done with it. MPL-2.0.
"""
import argparse, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rsrc import ResourceFork, macbinary
from macelf import build_image

ap = argparse.ArgumentParser()
ap.add_argument('elf'); ap.add_argument('--name', required=True); ap.add_argument('--creator', required=True)
ap.add_argument('-o', required=True)
ap.add_argument('--icons', help="the extension's key in mkicons.py: bundle and icons (21 November 2026)")
ap.add_argument('--version', default='1.0')
a = ap.parse_args()
img, nrel = build_image(a.elf)
rf = ResourceFork()
rf.add(b'INIT', 128, img, attrs=0x50)          # system heap, locked
creator = a.creator.encode('mac_roman')[:4].ljust(4, b' ')
flags = 0
if a.icons:
    from mkicons import add_bundle
    flags = add_bundle(rf, a.icons, creator, f"{a.name} {a.version}", ftype=b'INIT')
open(a.o, 'wb').write(macbinary(a.name, b'INIT', creator, b'', rf.build(), finder_flags=flags))
print(f"{a.o}: INIT {len(img)} bytes, {nrel} relocations")
