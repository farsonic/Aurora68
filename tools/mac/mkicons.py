#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""mkicons.py - the Aurora68 icon family: one motif, a glyph per program, every size and depth.

    mkicons.py --preview docs/img/aurora68-icons.png      the sheet of every icon, 1 bit and colour, 1x and 4x
    mkicons.py --list                                      the programs and documents it knows

and from the packagers (mkapp.py, elf2app.py, mkinit.py, elf2init.py --icons KEY):

    from mkicons import add_bundle
    flags = add_bundle(rf, 'play', b'PIPY', 'Aurora68 Play 1.1')    # BNDL, FREFs, signature, icons

Written on 21 November 2026, when the Macintosh programs were renamed "Aurora68 Xxxx" (docs/08 item
84). Every icon is drawn here, pixel by pixel, from nothing: no Apple icon was copied or traced.

The motif. An application is a rounded tile with a one-pixel shadow; across its top a night sky with
an aurora in it, a wave of light whose rays hang down like a curtain, the sky's lower edge a soft
wave too; below, on white, the program's glyph (a note for Play, a gauge for Status...). An extension
is the same tile cut as a puzzle piece (a knob on the right, a notch on the left), as System 7's own
extensions are drawn; a document is a page with its corner turned down, a narrow aurora along its top
and the glyph, with the kind of file in small letters for sound files.

The icons are designed at one bit first: the SE's screen shows only ICN# and ics#, which must read
on their own. Each pixel is an "ink" with two faces, its bit (black or white) and its colour; the
colour faces are aurora greens, blues and violets on a navy sky, put into the standard palettes:
the 8-bit system palette (the 6x6x6 cube with its ramps) for icl8/ics8, the 16 standard colours for
icl4/ics4. The Finder takes a family from the resources of one ID: ICN# (32x32 and its mask), ics#
(16x16 and its mask), icl4, icl8, ics4, ics8 (Inside Macintosh: More Macintosh Toolbox, "Icon
Utilities"; Inside Macintosh VI, "Finder Interface", for BNDL and FREF).

Files of one creator and type share one icon in the desktop database, so the three extensions that
share the creator 'PSMC' (Disk, Machine, Sound: creators stay as they were, so that nothing that knows
them breaks) carry the family's common extension icon in their bundle and their own glyph as a custom
icon (the family at ID -16455 and the Finder's hasCustomIcon flag), which System 7 shows; System 6
shows the common one.
"""
import math, os, struct, sys, zlib

# ---- the standard palettes ------------------------------------------------------------------------
def clut8():
    """The Macintosh 8-bit system palette: the 6x6x6 cube from white down (without black), then ten
    steps each of red, green, blue and grey without the cube's values, then black."""
    c = []
    for i in range(215):
        r, g, b = 5 - i // 36, 5 - (i // 6) % 6, 5 - i % 6
        c.append((r * 0x33, g * 0x33, b * 0x33))
    ramp = [0xEE, 0xDD, 0xBB, 0xAA, 0x88, 0x77, 0x55, 0x44, 0x22, 0x11]
    c += [(v, 0, 0) for v in ramp] + [(0, v, 0) for v in ramp] + [(0, 0, v) for v in ramp]
    c += [(v, v, v) for v in ramp] + [(0, 0, 0)]
    assert len(c) == 256
    return c

CLUT8 = clut8()
# the 16 standard colours (the system's 'clut' 4), 8 bits of each 16-bit component
CLUT4 = [(0xFF, 0xFF, 0xFF), (0xFC, 0xF3, 0x05), (0xFF, 0x64, 0x02), (0xDD, 0x08, 0x06),
         (0xF2, 0x08, 0x84), (0x46, 0x00, 0xA5), (0x00, 0x00, 0xD4), (0x02, 0xAB, 0xEA),
         (0x1F, 0xB7, 0x14), (0x00, 0x64, 0x11), (0x56, 0x2C, 0x05), (0x90, 0x71, 0x3A),
         (0xC0, 0xC0, 0xC0), (0x80, 0x80, 0x80), (0x40, 0x40, 0x40), (0x00, 0x00, 0x00)]

def nearest(pal, rgb):
    r, g, b = rgb
    return min(range(len(pal)), key=lambda i: 3 * (pal[i][0] - r) ** 2 + 4 * (pal[i][1] - g) ** 2 + 2 * (pal[i][2] - b) ** 2)

# ---- inks: (bit, colour) ----------------------------------------------------------------------------
WHITE, BLACK = (0xFF, 0xFF, 0xFF), (0, 0, 0)
INK_W = (0, WHITE)
INK_K = (1, BLACK)
SHADOW = (1, (0x33, 0x33, 0x66))
SKY = [(0x00, 0x00, 0x33), (0x00, 0x00, 0x66), (0x33, 0x00, 0x66)]        # top to bottom
# the aurora across the sky, left to right: green, sea green, cyan, blue, violet
AURORA = [(0x33, 0xFF, 0x66), (0x00, 0xFF, 0x99), (0x00, 0xFF, 0xCC), (0x33, 0xCC, 0xFF),
          (0x66, 0x99, 0xFF), (0x99, 0x66, 0xFF), (0xCC, 0x66, 0xFF)]
RAYS = [(0x00, 0x99, 0x66), (0x00, 0x99, 0x99), (0x00, 0x66, 0x99), (0x33, 0x33, 0x99), (0x66, 0x33, 0x99)]
GREEN, SEA, CYAN, BLUE, VIOLET = (0x00, 0xCC, 0x66), (0x00, 0x99, 0x66), (0x00, 0xCC, 0xFF), (0x33, 0x66, 0xFF), (0x99, 0x33, 0xFF)
NAVY, GOLD, BOARD = (0x00, 0x33, 0x99), (0xFF, 0xCC, 0x00), (0x00, 0x99, 0x33)


def along(cols, t):
    """a colour of the list at t in [0, 1]"""
    return cols[min(len(cols) - 1, max(0, int(t * len(cols))))]


class Canvas:
    def __init__(self, n):
        self.n = n
        self.px = [[None] * n for _ in range(n)]       # None: outside the icon (the mask's 0)
        self.forced_mask = set()

    def set(self, x, y, ink):
        if 0 <= x < self.n and 0 <= y < self.n:
            self.px[y][x] = ink

    def get(self, x, y):
        return self.px[y][x] if 0 <= x < self.n and 0 <= y < self.n else None

    def hline(self, x0, x1, y, ink):
        for x in range(x0, x1 + 1):
            self.set(x, y, ink)

    def vline(self, x, y0, y1, ink):
        for y in range(y0, y1 + 1):
            self.set(x, y, ink)

    def rect(self, x0, y0, x1, y1, ink):
        self.hline(x0, x1, y0, ink); self.hline(x0, x1, y1, ink)
        self.vline(x0, y0, y1, ink); self.vline(x1, y0, y1, ink)

    def fill(self, x0, y0, x1, y1, ink):
        for y in range(y0, y1 + 1):
            self.hline(x0, x1, y, ink)

    def line(self, x0, y0, x1, y1, ink):
        dx, dy = abs(x1 - x0), -abs(y1 - y0)
        sx, sy = (1 if x0 < x1 else -1), (1 if y0 < y1 else -1)
        err = dx + dy
        while True:
            self.set(x0, y0, ink)
            if (x0, y0) == (x1, y1):
                break
            e2 = 2 * err
            if e2 >= dy:
                err += dy; x0 += sx
            if e2 <= dx:
                err += dx; y0 += sy

    def disc(self, cx, cy, r, ink):
        for y in range(int(cy - r - 1), int(cy + r + 2)):
            for x in range(int(cx - r - 1), int(cx + r + 2)):
                if (x - cx) ** 2 + (y - cy) ** 2 <= r * r:
                    self.set(x, y, ink)

    def ring(self, cx, cy, r, w, ink, a0=0, a1=360):
        """the pixels whose centres lie within w/2 of the circle, between angles a0 and a1 (degrees,
        counter-clockwise from the right, y up)"""
        for y in range(int(cy - r - w - 1), int(cy + r + w + 2)):
            for x in range(int(cx - r - w - 1), int(cx + r + w + 2)):
                d = math.hypot(x - cx, y - cy)
                if abs(d - r) <= w / 2:
                    a = math.degrees(math.atan2(cy - y, x - cx)) % 360
                    if (a0 <= a <= a1) if a0 <= a1 else (a >= a0 or a <= a1):
                        self.set(x, y, ink)

    def poly(self, pts, ink):
        """a filled polygon: pixel centres inside it"""
        ys = [p[1] for p in pts]
        for y in range(int(min(ys)), int(max(ys)) + 1):
            yc = y + 0.5
            xs = []
            for i in range(len(pts)):
                (xa, ya), (xb, yb) = pts[i], pts[(i + 1) % len(pts)]
                if (ya <= yc < yb) or (yb <= yc < ya):
                    xs.append(xa + (yc - ya) * (xb - xa) / (yb - ya))
            xs.sort()
            for a, b in zip(xs[::2], xs[1::2]):
                for x in range(int(math.ceil(a - 0.5)), int(math.floor(b - 0.5)) + 1):
                    self.set(x, y, ink)

    def stamp(self, x0, y0, rows, legend):
        for j, row in enumerate(rows):
            for i, ch in enumerate(row):
                if ch in legend:
                    self.set(x0 + i, y0 + j, legend[ch])

    def text(self, x0, y0, s, ink):
        """the 3x5 letters below, one column apart"""
        x = x0
        for ch in s:
            g = FONT[ch]
            for j in range(5):
                for i in range(3):
                    if g[j] >> (2 - i) & 1:
                        self.set(x + i, y0 + j, ink)
            x += 4


FONT = {'A': [2, 5, 7, 5, 5], 'I': [7, 2, 2, 2, 7], 'F': [7, 4, 6, 4, 4], 'M': [5, 7, 7, 5, 5],
        'P': [6, 5, 6, 4, 4], 'W': [5, 5, 7, 7, 5], 'V': [5, 5, 5, 5, 2], '3': [6, 1, 2, 1, 6],
        '6': [3, 4, 6, 5, 2], '8': [2, 5, 2, 5, 2], 'D': [6, 5, 5, 5, 6], 'E': [7, 4, 6, 4, 7],
        'G': [3, 4, 5, 5, 3], 'H': [5, 5, 7, 5, 5], 'L': [4, 4, 4, 4, 7], 'N': [6, 5, 5, 5, 5],
        'O': [2, 5, 5, 5, 2], 'R': [6, 5, 6, 5, 5], 'S': [3, 4, 2, 1, 6], 'T': [7, 2, 2, 2, 2],
        'U': [5, 5, 5, 5, 7], 'Y': [5, 5, 2, 2, 2], 'C': [3, 4, 4, 4, 3], 'B': [6, 5, 6, 5, 6],
        'K': [5, 5, 6, 5, 5], 'X': [5, 5, 2, 5, 5], 'Z': [7, 1, 2, 4, 7], 'Q': [2, 5, 5, 6, 3],
        'J': [1, 1, 1, 5, 2], '-': [0, 0, 7, 0, 0], ' ': [0, 0, 0, 0, 0], '1': [2, 6, 2, 2, 7],
        '2': [6, 1, 2, 4, 7], '4': [5, 5, 7, 1, 1], '5': [7, 4, 6, 1, 6], '0': [2, 5, 5, 5, 2],
        '7': [7, 1, 2, 2, 2], '9': [2, 5, 3, 1, 6], '#': [5, 7, 5, 7, 5], '&': [2, 5, 2, 5, 3]}


# ---- the motif: tile, sky, aurora ---------------------------------------------------------------------
def rounded(c, x0, y0, x1, y1, r, ink, fill=None):
    """a rounded rectangle, its corners cut by r pixels on a curve"""
    def inside(x, y):
        cx = min(max(x, x0 + r), x1 - r); cy = min(max(y, y0 + r), y1 - r)
        return (x - cx) ** 2 + (y - cy) ** 2 <= (r + 0.45) ** 2
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            if inside(x, y):
                edge = not (inside(x - 1, y) and inside(x + 1, y) and inside(x, y - 1) and inside(x, y + 1))
                if edge:
                    c.set(x, y, ink)
                elif fill:
                    c.set(x, y, fill)


def shadow(c):
    """a one-pixel shadow under and to the right of whatever is drawn"""
    n = c.n
    add = []
    for y in range(n):
        for x in range(n):
            if c.get(x, y) is None and c.get(x - 1, y - 1) is not None and \
               (c.get(x - 1, y) is None or c.get(x, y - 1) is None or True):
                add.append((x, y))
    for x, y in add:
        if c.get(x - 1, y - 1) not in (None,) and c.get(x - 1, y - 1) != SHADOW:
            c.set(x, y, SHADOW)


def sky(c, x0, x1, top, bottom, wave, small=False):
    """The night sky between columns x0 and x1, rows top to the wave below `bottom`, inside what is
    already drawn (white), and the aurora in it."""
    w = x1 - x0
    edge = {}
    for x in range(x0, x1 + 1):
        t = (x - x0) / max(1, w)
        edge[x] = bottom + int(round(wave * math.sin(2 * math.pi * t * 0.9 + 0.6)))
        for y in range(top, edge[x] + 1):
            if c.get(x, y) == INK_W:
                k = (y - top) * len(SKY) // max(1, edge[x] - top + 1)
                c.set(x, y, (1, SKY[min(k, len(SKY) - 1)]))
    # the aurora: a curve of light, rays hanging from it on every other column
    for x in range(x0, x1 + 1):
        t = (x - x0) / max(1, w)
        if small:
            y = top + 1 + int(round(0.9 * math.sin(2 * math.pi * t * 1.1 + 2.2)))
            ray = 1 if x % 2 == 0 else 0
        else:
            y = top + 2 + int(round(1.6 * math.sin(2 * math.pi * t * 1.15 + 2.4)))
            ray = (2 + int(round(1.5 + 1.5 * math.sin(7.3 * t + 1.0)))) if x % 2 == 0 else 0
        if c.get(x, y) and c.get(x, y)[0] == 1 and c.get(x, y) != INK_K:
            c.set(x, y, (0, along(AURORA, t)))
            for k in range(1, ray + 1):
                if y + k < edge[x] - 0 and c.get(x, y + k) and c.get(x, y + k) != INK_K:
                    c.set(x, y + k, (0, along(RAYS, t)) if k > 1 or small else (0, along(AURORA, t)))
    # two stars in the larger sky
    if not small:
        for sx, sy in ((x0 + 3, top + 6), (x1 - 4, top + 1)):
            if c.get(sx, sy) and c.get(sx, sy)[0] == 1 and c.get(sx, sy) != INK_K:
                c.set(sx, sy, (0, (0xFF, 0xFF, 0xCC)))


def app_tile(n):
    c = Canvas(n)
    if n == 32:
        rounded(c, 1, 1, 29, 29, 4, INK_K, INK_W)
        sky(c, 2, 28, 2, 10, 1.0)
    else:
        rounded(c, 0, 0, 14, 14, 2, INK_K, INK_W)
        sky(c, 1, 13, 1, 4, 0.6, small=True)
    shadow(c)
    return c, ((3, 13, 27, 27) if n == 32 else (2, 6, 12, 13))       # the glyph's box: x0, y0, x1, y1


def ext_tile(n):
    """a puzzle piece: the tile with a knob on its right and a notch in its left side"""
    c = Canvas(n)
    if n == 32:
        rounded(c, 1, 1, 25, 29, 3, INK_K, INK_W)
        c.disc(26.0, 19.0, 3.3, INK_K); c.disc(25.5, 19.0, 2.3, INK_W)
        for y in range(16, 23):
            if c.get(25, y) == INK_K and abs(y - 19) <= 2:
                c.set(25, y, INK_W)
        c.disc(0.6, 19.0, 2.6, None)
        c.ring(0.6, 19.0, 2.6, 1.0, INK_K, 270, 90)
        for y in range(16, 23):                     # the outline where the notch meets the side
            if c.get(1, y) == INK_W and (c.get(0, y) is None or c.get(1, y - 1) is None or c.get(1, y + 1) is None):
                c.set(1, y, INK_K)
        sky(c, 2, 24, 2, 9, 0.8)
    else:
        rounded(c, 0, 0, 11, 14, 2, INK_K, INK_W)
        c.disc(12.2, 9.0, 2.2, INK_K); c.disc(11.8, 9.0, 1.2, INK_W)
        c.set(11, 9, INK_W); c.set(11, 8, INK_W); c.set(11, 10, INK_W)
        sky(c, 1, 10, 1, 4, 0.5, small=True)
    shadow(c)
    return c, ((3, 12, 22, 27) if n == 32 else (1, 6, 10, 13))


def doc_tile(n):
    """a page with its top right corner turned down"""
    c = Canvas(n)
    if n == 32:
        x0, x1, y0, y1, f = 4, 27, 1, 30, 6
    else:
        x0, x1, y0, y1, f = 1, 13, 0, 15, 4
    c.poly([(x0, y0), (x1 - f + 1, y0), (x1 + 1, y0 + f), (x1 + 1, y1 + 1), (x0, y1 + 1)], INK_W)
    c.hline(x0, x1 - f, y0, INK_K); c.vline(x0, y0, y1, INK_K); c.hline(x0, x1, y1, INK_K); c.vline(x1, y0 + f, y1, INK_K)
    c.line(x1 - f, y0, x1, y0 + f, INK_K)
    c.vline(x1 - f, y0, y0 + f, INK_K); c.hline(x1 - f, x1, y0 + f, INK_K)
    for y in range(y0 + 1, y0 + f):                 # the fold's inside
        for x in range(x1 - f + 1, x1):
            if c.get(x, y) == INK_W and x - (x1 - f) < y - y0:
                c.set(x, y, (0, (0xCC, 0xCC, 0xFF)))
    for y in range(y0, y0 + f):                     # nothing beyond the fold's diagonal
        for x in range(x1 - f + 1, x1 + 1):
            if x - (x1 - f) > y - y0:
                c.set(x, y, None)
    if n == 32:
        sky(c, x0 + 2, x1 - f - 2, y0 + 2, y0 + 6, 0.6)
        return c, (x0 + 2, 10, x1 - 2, 28)
    sky(c, x0 + 1, x1 - f - 1, y0 + 1, y0 + 3, 0.0, small=True)
    return c, (x0 + 2, 5, x1 - 1, 14)


# ---- the glyphs ---------------------------------------------------------------------------------------
def L(a=None, b=None, c=None):
    """the stamps' legend: # black; o white at one bit, colour a; * black at one bit, colour a;
    + black, colour b; - white, colour b; = black, colour c; x white both ways"""
    d = {'#': INK_K, 'x': INK_W}
    if a: d['o'] = (0, a); d['*'] = (1, a)
    if b: d['+'] = (1, b); d['-'] = (0, b)
    if c: d['='] = (1, c)
    return d


def centre(box, w, h):
    x0, y0, x1, y1 = box
    return x0 + (x1 - x0 + 1 - w) // 2, y0 + (y1 - y0 + 1 - h) // 2


def art(rows, legend):
    def draw(c, box):
        w = max(len(r) for r in rows); h = len(rows)
        x, y = centre(box, w, h)
        c.stamp(x, y, rows, legend)
    return draw


PLAY32 = [".......##########",
          ".......##########",
          ".......##......##",
          ".......##......##",
          ".......##......##",
          ".......##......##",
          ".......##......##",
          ".......##......##",
          ".....**##....**##",
          "...****##..****##",
          "..*****##.*****##",
          "..******..******.",
          "...***.....***..."]
PLAY16 = ["...#######",
          "...#.....#",
          "...#.....#",
          ".**#...**#",
          "***#..***#",
          "***...***."]

DISPLAY32 = ["###################",
             "#.................#",
             "#.ooooooo+ooooooo.#",
             "#.oooooo+++oooooo.#",
             "#.ooooooo+ooooooo.#",
             "#.oo+oooo+oooo+oo.#",
             "#.o+++++++++++++o.#",
             "#.oo+oooo+oooo+oo.#",
             "#.ooooooo+ooooooo.#",
             "#.oooooo+++oooooo.#",
             "#.ooooooo+ooooooo.#",
             "#.................#",
             "###################",
             "........###........",
             "....###########...."]
DISPLAY16 = ["#########",
             "#ooo+ooo#",
             "#o+++++o#",
             "#ooo+ooo#",
             "#########",
             "...###...",
             ".#######."]

SLOTS32 = ["#####################",
           "#ooooooooooooooooooo#",
           "#o#################o#",
           "#o#+++++++++++++++#o#",
           "#o#################o#",
           "#ooooooooooooooooooo#",
           "#o#################o#",
           "#o#+++++++++++++++#o#",
           "#o#################o#",
           "#ooooooooooooooooooo#",
           "#o#################o#",
           "#o#+++++++++++++++#o#",
           "#o#################o#",
           "#ooooooooooooooooooo#",
           "#####################"]
SLOTS16 = ["###########",
           "#ooooooooo#",
           "#o#######o#",
           "#ooooooooo#",
           "#o#######o#",
           "#ooooooooo#",
           "#o#######o#",
           "###########"]

LOG32 = ["############...",
         "#..........##..",
         "#.+++.****.#.#.",
         "#..........#..#",
         "#.+++.*****####",
         "#.............#",
         "#.+++.****.**.#",
         "#.............#",
         "#.+++.******..#",
         "#.............#",
         "#.+++.**.****.#",
         "#.............#",
         "#.+++.*****...#",
         "#.............#",
         "###############"]
LOG16 = ["######..",
         "#....##.",
         "#++.*###",
         "#......#",
         "#++.***#",
         "#......#",
         "#++.**.#",
         "########"]

NETWORK32 = ["###################",
             "#.+.+.+.+.+.+.+.+.#",
             "#.+.+.+.+.+.+.+.+.#",
             "#.................#",
             "#ooooooooooooooooo#",
             "##ooooooooooooooo##",
             ".##ooooooooooooo##.",
             "..##ooooooooooo##..",
             "...#############...",
             ".......#***#.......",
             ".......#***#.......",
             "........#*#........",
             "........#*#........",
             "........#*#........",
             "........#*#........"]
NETWORK16 = ["#########",
             "#+.+.+.+#",
             "#ooooooo#",
             ".#ooooo#.",
             "..#####..",
             "...#*#...",
             "...#*#...",
             "....#...."]

BTRUNE = ["...#...",
          "...##..",
          "...#.#.",
          "#..#..#",
          ".#.#.#.",
          "..###..",
          "...#...",
          "..###..",
          ".#.#.#.",
          "#..#..#",
          "...#.#.",
          "...##..",
          "...#..."]
BT16 = ["..*****..",
        ".***xx**.",
        ".*x*x*x*.",
        ".**xxx**.",
        ".**xxx**.",
        ".*x*x*x*.",
        ".***xx**.",
        "..*****.."]
WIFI16 = ["..*****..",
          ".*.....*.",
          "*..+++..*",
          "..+...+..",
          "....=....",
          "...===...",
          "....=...."]

DISK32 = ["##########...",
          "#.+.+.+.+.#..",
          "#.+.+.+.+..#.",
          "#.+.+.+.+...#",
          "#...........#",
          "#...........#",
          "##..........#",
          ".#..........#",
          "##..........#",
          "#.ooooooooo.#",
          "#.oo-o-o-oo.#",
          "#.ooooooooo.#",
          "#.ooooooooo.#",
          "#...........#",
          "#############"]
DISK16 = ["#####..",
          "#+++.#.",
          "#+++..#",
          "#.....#",
          "#ooooo#",
          "#ooooo#",
          "#######"]

MACHINE32 = [".###########.",
             "#...........#",
             "#.#########.#",
             "#.#ooooooo#.#",
             "#.#o-oo--o#.#",
             "#.#ooooooo#.#",
             "#.#ooooooo#.#",
             "#.#########.#",
             "#...........#",
             "#.......###.#",
             "#...........#",
             "#.*.........#",
             ".###########.",
             ".#.........#.",
             ".###########."]
MACHINE16 = [".#####.",
             "#.###.#",
             "#.#o#.#",
             "#.###.#",
             "#...#.#",
             ".#####.",
             ".#####."]

SETUP32 = ["........###########",
           "........#---------#",
           "#######.#---------#",
           "#ooooo#.#---------#",
           "#ooooo#.#---------#",
           "#ooooo#.#---------#",
           "#######.###########",
           "..###.......###....",
           "...................",
           "...#...........#...",
           "..##...........##..",
           ".#################.",
           "..##...........##..",
           "...#...........#..."]
SETUP16 = ["....#####",
           "###.#---#",
           "#o#.#---#",
           "###.#####",
           ".........",
           ".#.....#.",
           "#########",
           ".#.....#."]

MENUS32 = ["###################",
           "#######...#######..",
           "..#.............#..",
           "..#..*****...#...#..",
           "..#.*.....*..##..#",
           "..#...***....#.#.#",
           "..#..*...*...##..#",
           "..#....*.....#.#.#",
           "..#..........##..#",
           "..###############"]

OCTO = ["...###...",
        ".##...##.",
        "#..###..#"]


def g_play(n):
    return art(PLAY32 if n == 32 else PLAY16, L(VIOLET))

def g_display(n):
    return art(DISPLAY32 if n == 32 else DISPLAY16, L(BLUE, WHITE))

def g_video(n):
    """Aurora68 Video (23 November 2026): the display's glyph in the play button's violet, a film on the screen"""
    return art(DISPLAY32 if n == 32 else DISPLAY16, L(VIOLET, GOLD))

# Aurora68 PC (23 November 2026, docs/19-pc-card.md): a PC's screen with its prompt, "C:\>" and the cursor
PC32 = ["###################",
        "#.................#",
        "#.ooooooooooooooo.#",
        "#.o+o+o+o+ooooooo.#",
        "#.o+oooo+o+oooooo.#",
        "#.o+oo+o+oo+ooooo.#",
        "#.o+oooo+o+oooooo.#",
        "#.o+o+o+o+oo+++oo.#",
        "#.ooooooooooooooo.#",
        "#.ooooooooooooooo.#",
        "#.ooooooooooooooo.#",
        "#.................#",
        "###################",
        "........###........",
        "....###########...."]
PC16 = ["#########",
        "#+o++ooo#",
        "#+oo+ooo#",
        "#++o+o++#",
        "#########",
        "...###...",
        ".#######."]

def g_pc(n):
    """Aurora68 PC: the display's glyph, dark, with a prompt in green"""
    return art(PC32 if n == 32 else PC16, L(NAVY, GREEN))

def g_slots(n):
    return art(SLOTS32 if n == 32 else SLOTS16, L(BOARD, GOLD))

def g_log(n):
    return art(LOG32 if n == 32 else LOG16, L(NAVY, VIOLET))

def g_network(n):
    return art(NETWORK32 if n == 32 else NETWORK16, L(CYAN, GOLD))

def g_disk(n):
    return art(DISK32 if n == 32 else DISK16, L(VIOLET, WHITE if n == 32 else GOLD, None) if n == 32 else L(VIOLET, GOLD))

def g_machine(n):
    return art(MACHINE32 if n == 32 else MACHINE16, L(CYAN, WHITE))

def g_setup(n):
    return art(SETUP32 if n == 32 else SETUP16, L(BLUE, CYAN))


def g_status(n):
    """a gauge: a half dial, its scale from green to violet, the needle"""
    def draw(c, box):
        x0, y0, x1, y1 = box
        if n == 32:
            cx, cy, r = (x0 + x1) / 2, y1 - 2, 10.0
            for a in range(0, 181, 1):
                t = a / 180
                c.ring(cx, cy, r, 2.2, (1, along([VIOLET, BLUE, CYAN, GREEN], t)), a, a + 1)
            c.ring(cx, cy, r + 1.6, 1.0, INK_K, 0, 180)
            for a in (0, 30, 60, 90, 120, 150, 180):
                ca, sa = math.cos(math.radians(a)), math.sin(math.radians(a))
                c.line(int(round(cx + ca * (r - 3.5))), int(round(cy - sa * (r - 3.5))),
                       int(round(cx + ca * (r - 2.6))), int(round(cy - sa * (r - 2.6))), INK_K)
            c.hline(int(cx - r - 2), int(cx + r + 2), int(cy + 1), INK_K)
            c.line(int(cx), int(cy), int(round(cx + 7.5 * math.cos(math.radians(55)))), int(round(cy - 7.5 * math.sin(math.radians(55)))), (1, (0xCC, 0x00, 0x33)))
            c.line(int(cx) + 1, int(cy), int(round(cx + 1 + 7.5 * math.cos(math.radians(55)))), int(round(cy - 7.5 * math.sin(math.radians(55)))), (1, (0xCC, 0x00, 0x33)))
            c.disc(cx, cy, 1.6, INK_K)
        else:
            cx, cy, r = (x0 + x1) / 2, y1 - 1, 5.0
            for a in range(0, 181, 2):
                c.ring(cx, cy, r, 1.1, (1, along([VIOLET, BLUE, CYAN, GREEN], a / 180)), a, a + 2)
            c.hline(int(cx - r - 1), int(cx + r + 1), int(cy + 1), INK_K)
            c.line(int(cx), int(cy), int(cx) + 3, int(cy) - 4, (1, (0xCC, 0x00, 0x33)))
            c.set(int(cx), int(cy), INK_K)
    return draw


def g_wifi(n, scale=1.0, at=None):
    """three arcs rising from a dot"""
    def draw(c, box):
        x0, y0, x1, y1 = box
        if n == 32:
            cx, cy = (x0 + x1) / 2, y1 - 1.5
            for r, col in ((4.5, GREEN), (8.5, CYAN), (12.5, BLUE)):
                c.ring(cx, cy, r * scale, 2.2 * scale, (1, col), 45, 135)
            k = 1 if scale > 0.7 else 0
            c.fill(int(cx) - k, int(cy) - k, int(cx) + 1, int(cy) + 1, (1, GREEN))
        else:
            x, y = centre(box, 9, 7)
            c.stamp(x, y, WIFI16, L(BLUE, CYAN, GREEN))
    return draw


def g_bluetooth(n):
    def draw(c, box):
        x0, y0, x1, y1 = box
        if n == 32:
            w, h = 13, 15
            x, y = centre(box, w, h)
            cx, cy = x + 6, y + 7
            for yy in range(y, y + h):
                for xx in range(x, x + w):
                    if ((xx - cx) / 6.4) ** 2 + ((yy - cy) / 7.6) ** 2 <= 1.0:
                        c.set(xx, yy, (1, (0x00, 0x66, 0xCC)))
            c.stamp(x + 3, y + 1, BTRUNE, {'#': INK_W})
        else:
            x, y = centre(box, 9, 8)
            c.stamp(x, y, BT16, L((0x00, 0x66, 0xCC)))
    return draw


def g_ping(n):
    """a sonar's screen: rings, the sweep and an echo"""
    def draw(c, box):
        x0, y0, x1, y1 = box
        if n == 32:
            cx, cy, r = (x0 + x1) / 2, (y0 + y1) / 2, 7.2
            c.disc(cx, cy, r, (0, (0x00, 0x33, 0x33)))
            c.ring(cx, cy, r + 0.3, 1.0, INK_K)
            c.ring(cx, cy, 3.6, 1.0, (1, SEA))
            c.line(int(cx), int(cy), int(cx + 5), int(cy - 5), (1, GREEN))
            c.set(int(cx), int(cy), INK_K)
            c.disc(cx + 3.6, cy + 2.2, 1.2, (1, (0x99, 0xFF, 0x66)))
            for k in (1, 2):                        # the ping going out: arcs beyond the screen
                c.ring(cx, cy, r + 1.6 + 2.2 * k, 1.0, (1, CYAN), 20, 70)
        else:
            cx, cy, r = (x0 + x1) / 2 - 1, (y0 + y1) / 2 + 0.5, 3.6
            c.disc(cx, cy, r, (0, (0x00, 0x33, 0x33)))
            c.ring(cx, cy, r + 0.2, 0.9, INK_K)
            c.line(int(cx), int(cy), int(cx) + 2, int(cy) - 2, (1, GREEN))
            c.set(int(cx) + 1, int(cy) + 2, (1, (0x99, 0xFF, 0x66)))
            c.ring(cx, cy, r + 2.2, 0.9, (1, CYAN), 15, 75)
    return draw


def g_colour(n):
    """a colour wheel of six hues; at one bit, six shades: black, a grey, white, in turn"""
    hues = [(0xFF, 0x00, 0x00), (0xFF, 0xFF, 0x00), (0x00, 0xFF, 0x00), (0x00, 0xFF, 0xFF), (0x00, 0x00, 0xFF), (0xFF, 0x00, 0xFF)]
    def draw(c, box):
        x0, y0, x1, y1 = box
        r = 7.3 if n == 32 else 3.7
        cx, cy = (x0 + x1) / 2, (y0 + y1) / 2 + (0 if n == 32 else 0.5)
        for y in range(int(cy - r - 1), int(cy + r + 2)):
            for x in range(int(cx - r - 1), int(cx + r + 2)):
                d = math.hypot(x - cx, y - cy)
                if d <= r:
                    a = (math.degrees(math.atan2(cy - y, x - cx)) + 360 + 30) % 360
                    k = int(a // 60)
                    bit = [1, (x + y) & 1, 0, 1, (x + y) & 1, 0][k]
                    c.set(x, y, (bit, hues[k]))
        c.ring(cx, cy, r + 0.5, 1.0, INK_K)
    return draw


def g_screens(n):
    """two screens, one behind the other"""
    def draw(c, box):
        x0, y0, x1, y1 = box
        if n == 32:
            x, y = centre(box, 21, 15)
            c.fill(x, y, x + 13, y + 9, INK_W); c.rect(x, y, x + 13, y + 9, INK_K)
            c.fill(x + 2, y + 2, x + 11, y + 7, (0, VIOLET))
            c.fill(x + 7, y + 4, x + 20, y + 13, INK_W); c.rect(x + 7, y + 4, x + 20, y + 13, INK_K)
            c.fill(x + 9, y + 6, x + 18, y + 11, (0, CYAN))
            c.hline(x + 11, x + 16, y + 14, INK_K)
        else:
            x, y = centre(box, 10, 8)
            c.fill(x, y, x + 6, y + 4, INK_W); c.rect(x, y, x + 6, y + 4, INK_K)
            c.fill(x + 1, y + 1, x + 5, y + 3, (0, VIOLET))
            c.fill(x + 3, y + 2, x + 9, y + 6, INK_W); c.rect(x + 3, y + 2, x + 9, y + 6, INK_K)
            c.fill(x + 4, y + 3, x + 8, y + 5, (0, CYAN))
            c.hline(x + 5, x + 7, y + 7, INK_K)
    return draw


def g_depth(n):
    """three layers stacked: the depths a screen can take"""
    def draw(c, box):
        x0, y0, x1, y1 = box
        if n == 32:
            x, y = centre(box, 21, 15)
            layers = [(y + 9, (1, VIOLET)), (y + 5, (0, BLUE)), (y + 1, (0, GREEN))]
            for ly, ink in layers:
                pts = [(x + 5, ly), (x + 21, ly), (x + 16, ly + 5), (x, ly + 5)]
                c.poly(pts, ink if ink[0] == 0 else (1, ink[1]))
                if ink[0] == 1:                       # the bottom layer grey at one bit
                    for yy in range(ly, ly + 6):
                        for xx in range(x, x + 22):
                            p = c.get(xx, yy)
                            if p == ink:
                                c.set(xx, yy, ((xx + yy) & 1, ink[1]))
                c.line(x + 5, ly, x + 20, ly, INK_K); c.line(x + 20, ly, x + 15, ly + 5, INK_K)
                c.line(x + 15, ly + 5, x, ly + 5, INK_K); c.line(x, ly + 5, x + 5, ly, INK_K)
        else:
            x, y = centre(box, 11, 8)
            for ly, col in ((y + 5, VIOLET), (y + 3, BLUE), (y + 1, GREEN)):
                c.hline(x + 2, x + 10, ly - 1, INK_K)
                c.hline(x + 1, x + 9, ly, (0, col))
                c.set(x + 1, ly, INK_K); c.set(x + 9, ly, INK_K)
                c.hline(x, x + 8, ly + 1, INK_K)
    return draw


def g_sound(n):
    """a loudspeaker and its sound"""
    def draw(c, box):
        x0, y0, x1, y1 = box
        if n == 32:
            x, y = centre(box, 19, 15)
            c.poly([(x, y + 5), (x + 4, y + 5), (x + 9, y), (x + 9, y + 15), (x + 4, y + 10), (x, y + 10)], (1, VIOLET))
            c.fill(x + 1, y + 6, x + 3, y + 9, (0, (0xCC, 0x99, 0xFF)))
            for k, col in ((0, GREEN), (1, CYAN), (2, BLUE)):
                c.ring(x + 9, y + 7.5, 4.2 + 3.2 * k, 1.5, (1, col), -40 % 360, 40)
        else:
            x, y = centre(box, 8, 8)
            c.poly([(x, y + 2.5), (x + 2, y + 2.5), (x + 4.5, y), (x + 4.5, y + 8), (x + 2, y + 5.5), (x, y + 5.5)], (1, VIOLET))
            c.ring(x + 4, y + 4, 2.4, 1.0, (1, GREEN), 320, 40)
            c.ring(x + 4, y + 4, 4.3, 1.0, (1, BLUE), 320, 40)
    return draw


def g_menus(n):
    """the menu bar with the extras' marks: Bluetooth, Wi-Fi and the clock, as they stand in the bar (Menu Extras since
    23 November 2026; the clock face added then)"""
    def draw(c, box):
        x0, y0, x1, y1 = box
        if n == 32:
            x, y = centre(box, 19, 15)
            c.fill(x, y, x + 18, y + 2, INK_K)
            c.stamp(x, y + 6, ["..#.", "#.##", ".##.", "#.##", "..#."], {'#': (1, (0x00, 0x66, 0xCC))})
            g_wifi(32, 0.45)(c, (x + 4, y + 5, x + 12, y + 13))
            c.ring(x + 15.5, y + 9, 2.6, 1.0, INK_K)                       # the clock: its face and two hands
            c.vline(x + 15, y + 7, y + 9, (1, VIOLET)); c.hline(x + 15, x + 16, y + 9, (1, VIOLET))
        else:
            x, y = centre(box, 8, 8)
            c.hline(x, x + 7, y, INK_K)
            c.ring(x + 2.5, y + 7, 2.2, 1.0, (1, GREEN), 45, 135)
            c.ring(x + 2.5, y + 7, 4.4, 1.0, (1, CYAN), 50, 130)
            c.stamp(x + 5, y + 2, [".#.", "#.#", ".#.", "#.#", ".#."], {'#': (1, (0x00, 0x66, 0xCC))})
    return draw


def g_agent(n):
    """the Agent: a terminal with a prompt, and an arrow each way beside it (files in and out)"""
    def draw(c, box):
        if n == 32:
            x, y = centre(box, 20, 14)
            c.fill(x, y, x + 13, y + 10, (1, NAVY))           # the screen: black at one bit, the sky's navy in colour
            c.rect(x, y, x + 13, y + 10, INK_K)
            for k in range(3):                                # the prompt ">", white at one bit, aurora green in colour
                c.set(x + 2 + k, y + 2 + k, (0, GREEN)); c.set(x + 2 + k, y + 6 - k, (0, GREEN))
            c.hline(x + 6, x + 9, y + 6, (0, CYAN))           # the cursor
            c.hline(x + 5, x + 8, y + 11, INK_K); c.hline(x + 3, x + 10, y + 12, INK_K)   # its foot
            ax = x + 17                                       # up, then down
            c.vline(ax, y, y + 5, (1, VIOLET)); c.hline(ax - 2, ax + 2, y + 2, (1, VIOLET)); c.hline(ax - 1, ax + 1, y + 1, (1, VIOLET))
            c.vline(ax, y + 7, y + 12, (1, BLUE)); c.hline(ax - 2, ax + 2, y + 10, (1, BLUE)); c.hline(ax - 1, ax + 1, y + 11, (1, BLUE))
        else:
            x, y = centre(box, 10, 7)
            c.fill(x, y, x + 6, y + 5, (1, NAVY))
            c.set(x + 1, y + 1, (0, GREEN)); c.set(x + 2, y + 2, (0, GREEN)); c.set(x + 1, y + 3, (0, GREEN))
            c.hline(x + 3, x + 4, y + 3, (0, CYAN))
            c.hline(x + 2, x + 4, y + 6, INK_K)
            c.vline(x + 8, y, y + 2, (1, VIOLET)); c.set(x + 7, y + 1, (1, VIOLET)); c.set(x + 9, y + 1, (1, VIOLET))
            c.vline(x + 8, y + 4, y + 6, (1, BLUE)); c.set(x + 7, y + 5, (1, BLUE)); c.set(x + 9, y + 5, (1, BLUE))
    return draw


def g_common(n):
    """the extensions' common icon: 68"""
    def draw(c, box):
        x0, y0, x1, y1 = box
        if n == 32:
            x, y = centre(box, 14, 10)
            for i, ch in enumerate('68'):
                g = FONT[ch]
                for j in range(5):
                    for k in range(3):
                        if g[j] >> (2 - k) & 1:
                            c.fill(x + i * 8 + 2 * k, y + 2 * j, x + i * 8 + 2 * k + 1, y + 2 * j + 1, (1, [VIOLET, BLUE][i]))
        else:
            x, y = centre(box, 7, 5)
            c.text(x, y, '68', (1, VIOLET))
    return draw


def g_doc(kind):
    """a sound file: its glyph and its kind in small letters; the preferences: two sliders"""
    def draw(c, box):
        x0, y0, x1, y1 = box
        n = c.n
        if kind == 'pref':
            if n == 32:
                for k, (yy, at, col) in enumerate(((y0 + 3, 4, GREEN), (y0 + 9, 12, BLUE), (y0 + 15, 7, VIOLET))):
                    c.hline(x0 + 1, x1 - 1, yy, INK_K)
                    c.fill(x0 + at, yy - 2, x0 + at + 3, yy + 2, (1, col))
                    c.rect(x0 + at, yy - 2, x0 + at + 3, yy + 2, INK_K)
            else:
                for yy, at, col in ((y0 + 2, 1, GREEN), (y0 + 6, 5, VIOLET)):
                    c.hline(x0, x1 - 1, yy, INK_K)
                    c.fill(x0 + at, yy - 1, x0 + at + 1, yy + 1, (1, col))
            return
        label = {'MPG3': 'MP3', 'AIFF': 'AIFF', 'WAVE': 'WAV', 'FLAC': 'FLAC', 'OggS': 'OGG', 'M4A ': 'M4A', 'MPEG': 'MPG', 'MPG ': 'MPG'}[kind]
        if n == 32:
            if kind in ('MPG3', 'MPEG', 'MPG '):          # MP3, and Aurora68 Video's MPEG films (23 November 2026)
                c.stamp(x0 + 5, y0 + 1, PLAY16, L(VIOLET))
            else:
                cx = x0
                for i in range(x1 - x0 + 1):
                    a = int(round(4.2 * math.sin(i * 0.9) * (0.45 + 0.55 * math.sin(i * 0.23 + 0.4) ** 2)))
                    c.vline(x0 + i, y0 + 5 - abs(a), y0 + 5 + abs(a), (1, [GREEN, CYAN, BLUE, VIOLET][i * 4 // (x1 - x0 + 1)]))
            w = 4 * len(label) - 1
            c.text(x0 + (x1 - x0 + 1 - w) // 2, y1 - 6, label, INK_K)
        else:                                         # too small for the letters
            if kind in ('MPG3', 'MPEG', 'MPG '):
                c.stamp(x0, y0 + 2, PLAY16, L(VIOLET))
            else:
                for i in range(x1 - x0):
                    a = [1, 4, 2, 5, 3, 1, 4, 2, 3][i % 9]
                    c.vline(x0 + i, y0 + 4 - a, y0 + 4 + a, (1, [GREEN, CYAN, BLUE, VIOLET][i * 4 // (x1 - x0)]))
    return draw


# ---- the programs: their glyphs, kinds, documents -------------------------------------------------------
# key: (Finder name, kind 'app' or 'ext', glyph, [documents: (file type, icon key)])
PROGRAMS = {
    'play':      ('Aurora68 Play', 'app', g_play, [('MPG3', 'doc-MPG3'), ('AIFF', 'doc-AIFF'), ('WAVE', 'doc-WAVE'), ('FLAC', 'doc-FLAC'),
                  ('OggS', 'doc-OggS'), ('M4A ', 'doc-M4A '), ('pref', 'doc-pref')]),
    'display':   ('Aurora68 Display', 'app', g_display, [('pref', 'doc-pref')]),
    'video':     ('Aurora68 Video', 'app', g_video, [('MPEG', 'doc-MPEG'), ('MPG ', 'doc-MPG ')]),
    'pc':        ('Aurora68 PC', 'app', g_pc, []),
    'slots':     ('Aurora68 Slots', 'app', g_slots, [('pref', 'doc-pref')]),
    'log':       ('Aurora68 Log', 'app', g_log, []),
    'status':    ('Aurora68 Status', 'app', g_status, []),
    'network':   ('Aurora68 Network', 'app', g_network, []),
    'wifi':      ('Aurora68 Wi-Fi', 'app', g_wifi, []),
    'bluetooth': ('Aurora68 Bluetooth', 'app', g_bluetooth, []),
    'ping':      ('Aurora68 Ping', 'app', g_ping, []),
    'colour':    ('Aurora68 Colour', 'app', g_colour, []),
    'screens':   ('Aurora68 Screens', 'app', g_screens, []),
    'depth':     ('Aurora68 Depth', 'app', g_depth, []),
    'disk':      ('Aurora68 Disk', 'ext', g_disk, []),
    'machine':   ('Aurora68 Machine', 'ext', g_machine, []),
    'setup':     ('Aurora68 Screen Setup', 'ext', g_setup, []),
    'sound':     ('Aurora68 Sound', 'ext', g_sound, []),
    'menus':     ('Aurora68 Menu Extras', 'ext', g_menus, []),
    'agent':     ('Aurora68 Agent', 'ext', g_agent, []),
    'common':    ('Aurora68 extension', 'ext', g_common, []),
}
DOCS = {'doc-MPG3': 'MPG3', 'doc-AIFF': 'AIFF', 'doc-WAVE': 'WAVE', 'doc-FLAC': 'FLAC', 'doc-OggS': 'OggS', 'doc-M4A ': 'M4A ', 'doc-pref': 'pref', 'doc-MPEG': 'MPEG', 'doc-MPG ': 'MPG '}
# the extensions whose creator, 'PSMC', they share: the common icon in the bundle, their own as a custom icon
SHARED_CREATOR = {'disk', 'machine', 'sound'}


def canvas(key, n):
    if key in DOCS:
        c, box = doc_tile(n)
        g_doc(DOCS[key])(c, box)
        return c
    _, kind, glyph, _ = PROGRAMS[key]
    c, box = (app_tile if kind == 'app' else ext_tile)(n)
    glyph(n)(c, box)
    return c


# ---- the resources ---------------------------------------------------------------------------------------
def bits(c, mask=False):
    out = bytearray()
    for y in range(c.n):
        v = 0
        for x in range(c.n):
            p = c.px[y][x]
            v = v << 1 | (1 if (p is not None if mask else (p is not None and p[0])) else 0)
        out += v.to_bytes(c.n // 8, 'big')
    return bytes(out)


def colour(c, depth):
    pal = CLUT8 if depth == 8 else CLUT4
    idx = []
    for y in range(c.n):
        for x in range(c.n):
            p = c.px[y][x]
            if p is None:
                idx.append(0)                         # outside: white, the mask hides it
            elif p[1] == BLACK:
                idx.append(len(pal) - 1)
            elif p[1] == WHITE:
                idx.append(0)
            else:
                idx.append(nearest(pal, p[1]))
    if depth == 8:
        return bytes(idx)
    return bytes(idx[i] << 4 | idx[i + 1] for i in range(0, len(idx), 2))


def family(key):
    """{resource type: data} for the key's icon family"""
    big, small = canvas(key, 32), canvas(key, 16)
    return {b'ICN#': bits(big) + bits(big, True), b'ics#': bits(small) + bits(small, True),
            b'icl4': colour(big, 4), b'icl8': colour(big, 8), b'ics4': colour(small, 4), b'ics8': colour(small, 8)}


def add_family(rf, key, rid, name=None):
    for t, d in family(key).items():
        rf.add(t, rid, d, name=name)


def add_bundle(rf, key, creator, signature, ftype=b'APPL'):
    """The Finder's resources for program `key`: the signature resource (creator, ID 0), BNDL 128, an
    FREF per kind of file and an icon family per FREF (IDs 128 on), and for Aurora68 Play a '****' FREF
    (any file may be dropped on it). Returns the Finder flags the file needs (hasBundle, and
    hasCustomIcon for the extensions that share a creator)."""
    _, kind, _, docs = PROGRAMS[key]
    flags = 0x2000
    own = key
    if key in SHARED_CREATOR:
        own = 'common'                                # the bundle: what every 'PSMC' file shows
        add_family(rf, key, -16455)                   # the custom icon: this file's own
        flags |= 0x0400
    sig = signature.encode('mac_roman')
    rf.add(creator, 0, bytes([len(sig)]) + sig)
    frefs = [(ftype, own)] + [(t, k) for t, k in docs]
    if key == 'play':
        frefs.append((b'****', None))
    icn, fref = [], []
    keys = []
    for local, (t, k) in enumerate(frefs):
        t = t if isinstance(t, bytes) else t.encode('mac_roman')
        rf.add(b'FREF', 128 + local, t + struct.pack('>H', local) + b'\0')
        fref.append((local, 128 + local))
        if k is not None:
            if k not in keys:
                keys.append(k)
                add_family(rf, k, 128 + len(keys) - 1)
            icn.append((local, 128 + keys.index(k)))
        else:
            icn.append((local, 128))                  # '****': no icon of its own
    bndl = creator + struct.pack('>HH', 0, 1)
    bndl += b'ICN#' + struct.pack('>H', len(icn) - 1) + b''.join(struct.pack('>HH', a, b) for a, b in icn)
    bndl += b'FREF' + struct.pack('>H', len(fref) - 1) + b''.join(struct.pack('>HH', a, b) for a, b in fref)
    rf.add(b'BNDL', 128, bndl)
    return flags


# ---- the preview sheet ------------------------------------------------------------------------------------
def png(path, rows):
    h, w = len(rows), len(rows[0])
    raw = b''.join(b'\0' + bytes(v for px in r for v in px) for r in rows)
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    data = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + \
        chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')
    open(path, 'wb').write(data)


def preview(path):
    """One row per icon: ICN# at 1x and 4x on white, icl8 at 1x and 4x and icl4 at 4x on the grey of a
    colour desktop, ics# and ics8 at 4x; the name in small letters on the left."""
    keys = list(PROGRAMS) + list(DOCS)
    label_w, cell = 120, 32 * 4 + 12
    cols = ['1', '1x4', '8', '8x4', '4x4', 's1x4', 's8x4']
    widths = {'1': 44, '1x4': cell, '8': 44, '8x4': cell, '4x4': cell, 's1x4': 16 * 4 + 12, 's8x4': 16 * 4 + 12}
    W = label_w + sum(widths[k] for k in cols) + 8
    H = 34 + len(keys) * (32 * 4 + 10)
    img = [[(0xEE, 0xEE, 0xEE)] * W for _ in range(H)]
    def put(x, y, rgb):
        if 0 <= x < W and 0 <= y < H:
            img[y][x] = rgb
    def block(x0, y0, w, h, rgb):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                put(x, y, rgb)
    def text(x0, y0, s, rgb, z=2):
        x = x0
        for ch in s.upper():
            g = FONT.get(ch, FONT[' '])
            for j in range(5):
                for i in range(3):
                    if g[j] >> (2 - i) & 1:
                        block(x + i * z, y0 + j * z, z, z, rgb)
            x += 4 * z
    def draw(c, mode, x0, y0, z, bg):
        n = c.n
        block(x0 - 2, y0 - 2, n * z + 4, n * z + 4, bg)
        pal = CLUT8 if mode == 8 else CLUT4
        cdata = colour(c, mode) if mode in (4, 8) else None
        for y in range(n):
            for x in range(n):
                p = c.px[y][x]
                if p is None:
                    continue
                if mode == 1:
                    rgb = BLACK if p[0] else WHITE
                else:
                    if mode == 8:
                        i = cdata[y * n + x]
                    else:
                        b = cdata[(y * n + x) // 2]
                        i = b >> 4 if (y * n + x) % 2 == 0 else b & 15
                    rgb = pal[i]
                block(x0 + x * z, y0 + y * z, z, z, rgb)
    heads = {'1': 'ICN#', '1x4': 'ICN# X4', '8': 'ICL8', '8x4': 'ICL8 X4', '4x4': 'ICL4 X4', 's1x4': 'ICS#', 's8x4': 'ICS8'}
    x = label_w
    for k in cols:
        text(x, 10, heads[k], (0x33, 0x33, 0x33))
        x += widths[k]
    text(8, 10, 'AURORA68', (0x33, 0x00, 0x66))
    y = 34
    grey = (0xBB, 0xBB, 0xCC)
    for key in keys:
        big, small = canvas(key, 32), canvas(key, 16)
        name = PROGRAMS[key][0].replace('Aurora68 ', '') if key in PROGRAMS else key.replace('doc-', 'DOC ')
        text(8, y + 58, name[:13], (0, 0, 0))
        x = label_w
        for k in cols:
            if k == '1': draw(big, 1, x + 4, y + 48, 1, WHITE)
            if k == '1x4': draw(big, 1, x + 4, y, 4, WHITE)
            if k == '8': draw(big, 8, x + 4, y + 48, 1, grey)
            if k == '8x4': draw(big, 8, x + 4, y, 4, grey)
            if k == '4x4': draw(big, 4, x + 4, y, 4, grey)
            if k == 's1x4': draw(small, 1, x + 4, y + 32, 4, WHITE)
            if k == 's8x4': draw(small, 8, x + 4, y + 32, 4, grey)
            x += widths[k]
        y += 32 * 4 + 10
    png(path, img)
    print(f"{path}: {len(keys)} icons, {W}x{H}")


if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--preview', metavar='PNG')
    ap.add_argument('--list', action='store_true')
    a = ap.parse_args()
    if a.list:
        for k, (name, kind, _, docs) in PROGRAMS.items():
            print(f"{k:10} {kind}  {name}" + (f"  documents: {', '.join(t for t, _ in docs)}" if docs else ''))
    if a.preview:
        preview(a.preview)
