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

23 November 2026 (docs/08 item 113): a fifth decompressor, 'mpg1' (MPEG-1, the import's own type), and the
MPEG-1 movie import itself, two 'thng' resources of type 'eat ' for the file types 'MPEG' and 'MPG ', the
same code: canMovieImportFiles, canMovieImportInPlace (the movie's samples stay in the file; MoviePlayer
opens it rather than offering to convert it), canMovieImportValidateFile. An import registers wherever
QuickTime is: without the card it still opens the file, to say in the movie that the card is needed.
"""
import argparse, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rsrc import ResourceFork, macbinary
from macelf import build_image

FORMATS = [('cvid', 'Cinepak'), ('rpza', 'Apple Video'), ('rle ', 'Animation'), ('smc ', 'Graphics'), ('mpg1', 'MPEG-1')]
DOES = 0x3F | 1 << 10                   # codecInfoDoes1..32, codecInfoDoesTemporal
WANTS_REGISTER = 1 << 31                # cmpWantsRegisterMessage
# the movie import (QuickTimeComponents.h): canMovieImportFiles, canMovieImportValidateFile, canMovieImportInPlace
IMPORT_FLAGS = 1 << 1 | 1 << 9 | 1 << 11
IMPORTS = [('MPEG', 'MPEG-1 film'), ('MPG ', 'MPEG-1 film')]
COMPONENTS = [(b'imdc', sub, name, f'{name} (Aurora68 Video Decoder)',
               f'{name} pictures decoded on the Pi by the Aurora68 Video Decoder card', DOES | WANTS_REGISTER) for sub, name in FORMATS] + \
             [(b'eat ', sub, name, f'{name} ({sub.strip()}, Aurora68 Video Decoder)',
               'MPEG-1 films (.mpg) opened as QuickTime movies, their pictures and sound decoded by the Aurora68 Video Decoder card',
               IMPORT_FLAGS) for sub, name in IMPORTS]

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


for i, (ctype, sub, name, title, about, flags) in enumerate(COMPONENTS):
    rid = 128 + i
    rf.add(b'STR ', 128 + 2 * i, pstr(title))
    rf.add(b'STR ', 129 + 2 * i, pstr(about))
    thng = struct.pack('>4s4s4sII', ctype, sub.encode(), b'A68v', flags, 0)
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
print(f"{a.o}: QuickTime decompressors {', '.join(n for _, n in FORMATS)}; MPEG-1 import for {', '.join(repr(s) for s, _ in IMPORTS)}; "
      f"code {len(img)} bytes, {nrel} relocations")
