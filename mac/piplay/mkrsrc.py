#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""mac/piplay/mkrsrc.py OUTDIR - PiPlay's Finder resources, written as files for elf2app.py --rsrc.

So that the Finder knows PiPlay: its signature ('PIPY' 0), a bundle (BNDL 128) with two file
references, the application itself (FREF 128, 'APPL') and every kind of document (FREF 129, '****':
any file may be dropped on PiPlay's icon, MP3s from a PC included, which have no type), and the icon
(ICN# 128: a loudspeaker and a note, drawn below, and its mask). With the bundle bit set the Finder
puts this into its desktop database and hands PiPlay a file dropped on it or opened with it as the
application's files (AppParmHandle), on System 6 and 7 alike. The layouts are Inside Macintosh VI's
(Finder Interface). Written on 21 November 2026.
"""
import os, struct, sys

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
ICON = """
................................
................................
.......................XX.......
.......................XXXX.....
.......................XX.XXX...
.......................XX...XX..
.............XX........XX....X..
............XXX........XX....X..
...........XXXX........XX...X...
..........XX.XX........XX.......
.........XX..XX........XX.......
..XXXXXXXX...XX........XX.......
..X.....X....XX........XX.......
..X.....X....XX....XXXXXX.......
..X.....X....XX...XXXXXXX.......
..X.....X....XX...XXXXXXX.......
..X.....X....XX....XXXXX........
..X.....X....XX.................
..X.....X....XX.................
..XXXXXXXX...XX.................
.........XX..XX.................
..........XX.XX.................
...........XXXX.................
............XXX.................
.............XX.................
................................
..XXXXXXXXXXXXXXXXXXXXXXXXXXXX..
..X..........................X..
..X..XX..XX..XX..XX..XX..XX..X..
..X..........................X..
..XXXXXXXXXXXXXXXXXXXXXXXXXXXX..
................................
""".split()
assert len(ICON) == 32 and all(len(r) == 32 for r in ICON)


def bits(rows):
    return b''.join(struct.pack('>I', int(r.replace('.', '0').replace('X', '1'), 2)) for r in rows)


def mask(rows):
    # the icon's outline filled: each row from its first to its last dot, which keeps the speaker's
    # inside and the card's inside white but selectable
    m = []
    for r in rows:
        if 'X' in r:
            a, b = r.index('X'), r.rindex('X')
            m.append('.' * a + 'X' * (b - a + 1) + '.' * (31 - b))
        else:
            m.append('.' * 32)
    return m


def put(name, data):
    open(os.path.join(out, name), 'wb').write(data)


put('icn128.bin', bits(ICON) + bits(mask(ICON)))
put('sig.bin', bytes([len('PiPlay 1.0')]) + b'PiPlay 1.0')
put('fref128.bin', b'APPL' + struct.pack('>H', 0) + b'\0')
put('fref129.bin', b'****' + struct.pack('>H', 1) + b'\0')
# BNDL: signature, its resource ID, number of types - 1; per type: the type, count - 1, (local ID, resource ID) pairs
put('bndl128.bin', b'PIPY' + struct.pack('>HH', 0, 1) +
    b'ICN#' + struct.pack('>HHHHH', 1, 0, 128, 1, 128) +
    b'FREF' + struct.pack('>HHHHH', 1, 0, 128, 1, 129))
