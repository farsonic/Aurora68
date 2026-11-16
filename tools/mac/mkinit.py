#!/usr/bin/env python3
"""Package a DRVR image and an INIT code image into a MacBinary system extension file.

    mkinit.py --drvr pidisk.bin.raw --init pidisk_init.bin.raw --name PiDisk -o PiDisk.bin

Resources: 'INIT' 128 (code, system heap, locked), 'DRVR' 128 named after the driver's own
name field (the first Pascal string after the 18-byte header). File type 'INIT', creator 'PSMC'.
"""
import argparse, os, sys
sys.path.insert(0, os.path.dirname(__file__))
from rsrc import ResourceFork, macbinary

ap = argparse.ArgumentParser()
ap.add_argument('--drvr', required=True); ap.add_argument('--init', required=True)
ap.add_argument('--name', required=True); ap.add_argument('-o', required=True)
a = ap.parse_args()
drvr = open(a.drvr, 'rb').read(); init = open(a.init, 'rb').read()
dname = drvr[19:19 + drvr[18]].decode('mac_roman')
rf = ResourceFork()
rf.add(b'DRVR', 128, drvr, name=dname, attrs=0x58)      # resSysHeap | resPurgeable? no: sysheap(0x40)|locked(0x10)|preload(0x04)? keep 0x58 = sysheap|locked|preload
rf.add(b'INIT', 128, init, attrs=0x58)
open(a.o, 'wb').write(macbinary(a.name, b'INIT', b'PSMC', b'', rf.build(), finder_flags=0))
print(f"{a.o}: DRVR '{dname}' {len(drvr)} bytes, INIT {len(init)} bytes")
