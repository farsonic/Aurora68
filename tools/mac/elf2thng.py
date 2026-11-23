#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""elf2thng.py - a QuickTime component file for the classic Mac OS from a C component linked at 0 with
--emit-relocs (its entry first: mac/vdec/entry.s), as a MacBinary II file:

    elf2thng.py vdec.elf --name "Aurora68 Video Decoder" -o Aurora68-VideoDecoder.bin

The file is of type 'thng', which QuickTime registers by itself when it finds one in the Extensions folder
at start-up (Inside Macintosh: More Macintosh Toolbox, "Component Manager": RegisterComponentResourceFile).
It holds the code once, 'cdec' 128 (the image and its relocation table, tools/mac/macelf.py; entry.s fixes
itself up wherever the Component Manager loads it), and one 'thng' resource per decompressor, each naming
that code: componentType 'imdc', the format as subtype, manufacturer 'A68v', the decompressors' codecInfo
flags with cmpWantsRegisterMessage (the component declines registration where the card is absent), and its
name and description as 'STR ' resources. Written on 22 November 2026 (docs/08 item 110). MPL-2.0.
"""
import argparse, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rsrc import ResourceFork, macbinary
from macelf import build_image

FORMATS = [('cvid', 'Cinepak'), ('rpza', 'Apple Video'), ('rle ', 'Animation'), ('smc ', 'Graphics')]
DOES = 0x3F | 1 << 10                   # codecInfoDoes1..32, codecInfoDoesTemporal
WANTS_REGISTER = 1 << 31                # cmpWantsRegisterMessage

ap = argparse.ArgumentParser()
ap.add_argument('elf'); ap.add_argument('--name', required=True); ap.add_argument('-o', required=True)
ap.add_argument('--creator', default='A68v')
ap.add_argument('--version', default='1.0')
a = ap.parse_args()
img, nrel = build_image(a.elf)
rf = ResourceFork()
rf.add(b'cdec', 128, img, attrs=0x00)


def pstr(s):
    b = s.encode('mac_roman')
    return bytes([len(b)]) + b


for i, (sub, name) in enumerate(FORMATS):
    rid = 128 + i
    rf.add(b'STR ', 128 + 2 * i, pstr(f'{name} (Aurora68 Video Decoder)'))
    rf.add(b'STR ', 129 + 2 * i, pstr(f'{name} pictures decoded on the Pi by the Aurora68 Video Decoder card'))
    thng = struct.pack('>4s4s4sII', b'imdc', sub.encode(), b'A68v', DOES | WANTS_REGISTER, 0)
    thng += struct.pack('>4sh', b'cdec', 128)                 # the code
    thng += struct.pack('>4sh', b'STR ', 128 + 2 * i)         # its name
    thng += struct.pack('>4sh', b'STR ', 129 + 2 * i)         # its description
    thng += struct.pack('>4sh', b'\0\0\0\0', 0)               # no icon
    rf.add(b'thng', rid, thng)
vers = a.version.split('.')
major, minor = int(vers[0]), int(vers[1]) if len(vers) > 1 else 0
short = a.version.encode('mac_roman')
long_ = f'{a.version}, the Aurora68 project'.encode('mac_roman')
rf.add(b'vers', 1, struct.pack('>BBBBH', major, minor << 4, 0x80, 0, 0) + bytes([len(short)]) + short + bytes([len(long_)]) + long_)
creator = a.creator.encode('mac_roman')[:4].ljust(4, b' ')
open(a.o, 'wb').write(macbinary(a.name, b'thng', creator, b'', rf.build(), finder_flags=0))
print(f"{a.o}: QuickTime components {', '.join(n for _, n in FORMATS)}; code {len(img)} bytes, {nrel} relocations")
