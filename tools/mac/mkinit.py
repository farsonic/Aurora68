#!/usr/bin/env python3
"""Package an INIT code image, and optionally a DRVR image, into a MacBinary system extension file.

    mkinit.py --drvr pidisk.bin.raw --init pidisk_init.bin.raw --name PiDisk -o PiDisk.bin

Resources: 'INIT' 128 (code, system heap, locked), 'DRVR' 128 named after the driver's own
name field (the first Pascal string after the 18-byte header). File type 'INIT', creator 'PSMC'
(--creator). --icons KEY: the Finder's bundle and icons from mkicons.py (21 November 2026).
"""
import argparse, os, sys
sys.path.insert(0, os.path.dirname(__file__))
from rsrc import ResourceFork, macbinary

ap = argparse.ArgumentParser()
ap.add_argument('--drvr'); ap.add_argument('--init', required=True)
ap.add_argument('--name', required=True); ap.add_argument('-o', required=True)
ap.add_argument('--creator', default='PSMC')
ap.add_argument('--icons', help="the extension's key in mkicons.py")
ap.add_argument('--version', default='1.0')
a = ap.parse_args()
init = open(a.init, 'rb').read()
rf = ResourceFork()
dname = None
if a.drvr:
    drvr = open(a.drvr, 'rb').read()
    dname = drvr[19:19 + drvr[18]].decode('mac_roman')
    rf.add(b'DRVR', 128, drvr, name=dname, attrs=0x58)  # sysheap | locked | preload
rf.add(b'INIT', 128, init, attrs=0x58)
creator = a.creator.encode('mac_roman')[:4].ljust(4, b' ')
flags = 0
if a.icons:
    from mkicons import add_bundle
    flags = add_bundle(rf, a.icons, creator, f"{a.name} {a.version}", ftype=b'INIT')
open(a.o, 'wb').write(macbinary(a.name, b'INIT', creator, b'', rf.build(), finder_flags=flags))
print(f"{a.o}: " + (f"DRVR '{dname}' {len(drvr)} bytes, " if dname else "") + f"INIT {len(init)} bytes")
