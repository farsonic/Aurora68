#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""make-icon.py - the macOS Aurora68 Play's icon: the SE's Aurora68 Play icon, as pixel art.

    python3 make-icon.py OUT.iconset          the PNGs iconutil turns into AppIcon.icns (make-app.sh)

Written on 22 November 2026 with the macOS app. The project's icons are drawn pixel by pixel in
tools/mac/mkicons.py (the aurora over a night sky, a note for Play); this takes the same drawing, the
colour face of its 32x32 (and, for the smallest sizes, 16x16) icon, and scales it up by whole pixels,
so the modern Mac shows the 1987 icon crisp at every size, on the transparent margin macOS icons
leave. Standard library only.
"""
import os, struct, sys, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'tools', 'mac'))
import mkicons  # noqa: E402

SIZES = [('16x16', 16), ('16x16@2x', 32), ('32x32', 32), ('32x32@2x', 64), ('128x128', 128),
         ('128x128@2x', 256), ('256x256', 256), ('256x256@2x', 512), ('512x512', 512), ('512x512@2x', 1024)]


def png_rgba(path, rows):
    h, w = len(rows), len(rows[0])
    raw = b''.join(b'\0' + bytes(v for px in r for v in px) for r in rows)

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def render(size):
    n = 16 if size <= 16 else 32
    c = mkicons.canvas('play', n)
    scale = max(1, int(size * 0.82) // n)
    pad = (size - n * scale) // 2
    rows = [[(0, 0, 0, 0)] * size for _ in range(size)]
    for y in range(n):
        for x in range(n):
            p = c.px[y][x]
            if p is None:
                continue
            r, g, b = p[1]
            for dy in range(scale):
                row = rows[pad + y * scale + dy]
                for dx in range(scale):
                    row[pad + x * scale + dx] = (r, g, b, 255)
    return rows


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for name, size in SIZES:
        png_rgba(os.path.join(out, f'icon_{name}.png'), render(size))
    print(f'{out}: {len(SIZES)} icons')


if __name__ == '__main__':
    main()
