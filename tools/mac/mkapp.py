#!/usr/bin/env python3
"""Package a flat 68k code image as a classic Mac OS application (MacBinary).

    mkapp.py --code app.bin.raw --name PiNet --creator PINT -o PiNet.bin

CODE 0: the jump table header (above-A5 size, below-A5 size, jump table size, offset 32) and one
jump table entry pointing at offset 0 of CODE 1. CODE 1: 4-byte segment header (first JT entry
offset 0, 1 entry) followed by the code, which must be position independent and start with its
entry point. SIZE -1: flags (multi-finder aware, background null events off), 256 KiB preferred
and minimum. --icons KEY adds the Finder's bundle and the program's icon family from mkicons.py
(since 21 November 2026; without it the Finder shows a generic icon). --owner adds the signature resource alone.
"""
import argparse, os, struct, sys
sys.path.insert(0, os.path.dirname(__file__))
from rsrc import ResourceFork, macbinary

ap = argparse.ArgumentParser()
ap.add_argument('--code', required=True); ap.add_argument('--name', required=True)
ap.add_argument('--creator', default='PSMC'); ap.add_argument('-o', required=True)
ap.add_argument('--icons', help="the program's key in mkicons.py: bundle, file references, icons")
ap.add_argument('--version', default='1.0', help="the signature resource's text: NAME VERSION")
ap.add_argument('--owner', action='store_true', help="the signature resource (type = creator, ID 0) without the icons "
                "(--icons adds it anyway): Aurora68 Machine's test, mac/machtest.s, finds a program by it")
a = ap.parse_args()
code = open(a.code, 'rb').read()
code1 = struct.pack('>HH', 0, 1) + code
# jump table entry: offset of routine in segment (word), move.w #seg,-(sp); _LoadSeg
jt = struct.pack('>H', 0) + b'\x3f\x3c' + struct.pack('>H', 1) + b'\xa9\xf0'
code0 = struct.pack('>IIII', 0x20 + len(jt), 0x2000, len(jt), 0x20) + jt
size = struct.pack('>HII', 0x5880, 256 * 1024, 256 * 1024)
rf = ResourceFork()
rf.add(b'CODE', 0, code0)
rf.add(b'CODE', 1, code1)
rf.add(b'SIZE', -1, size)
creator = a.creator.encode('mac_roman')[:4].ljust(4, b' ')
flags = 0
if a.icons:
    from mkicons import add_bundle
    flags = add_bundle(rf, a.icons, creator, f"{a.name} {a.version}")
elif a.owner:
    sig = f"{a.name} {a.version}".encode('mac_roman')
    rf.add(creator, 0, bytes([len(sig)]) + sig)
open(a.o, 'wb').write(macbinary(a.name, b'APPL', creator, b'', rf.build(), finder_flags=flags))
print(f"{a.o}: CODE 1 {len(code1)} bytes")
