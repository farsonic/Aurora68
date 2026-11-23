#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""pnm2png.py IN.pnm OUT.png [--crop X,Y,W,H] [--scale N]: a binary PGM (P5) or PPM (P6) as a PNG, standard library only
(tools/pccard_test.sh's pictures of the PC card's window, 23 November 2026)."""
import struct, sys, zlib

def read_pnm(path):
    d = open(path, 'rb').read()
    parts, i = [], 0
    while len(parts) < 4:
        while d[i:i + 1].isspace(): i += 1
        if d[i:i + 1] == b'#':
            while d[i:i + 1] not in (b'\n', b''): i += 1
            continue
        j = i
        while not d[j:j + 1].isspace(): j += 1
        parts.append(d[i:j]); i = j
    i += 1
    kind, w, h = parts[0], int(parts[1]), int(parts[2])
    ch = 1 if kind == b'P5' else 3
    return w, h, ch, d[i:i + w * h * ch]

def write_png(path, w, h, ch, px):
    raw = b''.join(b'\0' + px[y * w * ch:(y + 1) * w * ch] for y in range(h))
    def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 0 if ch == 1 else 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')
    open(path, 'wb').write(png)

a = sys.argv[1:]
w, h, ch, px = read_pnm(a[0])
if '--crop' in a:
    x0, y0, cw, chh = (int(v) for v in a[a.index('--crop') + 1].split(','))
    px = b''.join(px[((y0 + y) * w + x0) * ch:((y0 + y) * w + x0 + cw) * ch] for y in range(chh)); w, h = cw, chh
if '--scale' in a:
    n = int(a[a.index('--scale') + 1])
    rows = []
    for y in range(h):
        r = b''.join(px[(y * w + x) * ch:(y * w + x + 1) * ch] * n for x in range(w))
        rows += [r] * n
    px = b''.join(rows); w, h = w * n, h * n
write_png(a[1], w, h, ch, px)
