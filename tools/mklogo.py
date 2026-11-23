#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""mklogo.py - the Aurora68 logo as pixels for the kernel's HDMI splash (emu68/src/psmac/psmac_splash_logo.h).

23 November 2026. The owner asked for "a subtle splash screen on the HDMI monitor that has the Aurora logo and
version" in place of the kernel's log (docs/08 item 128). The approved logo is docs/img/aurora68-logo.svg (a blended
aurora of the 1980s Apple colours, the same SVG tools/recovery/page.html has inline); the kernel has no SVG
renderer, so this script draws the SVG itself, in pure Python, and writes the pixels as a C array:

    python3 tools/mklogo.py                  # writes emu68/src/psmac/psmac_splash_logo.h
    python3 tools/mklogo.py --check          # (make -C mac check) the header in the tree is what the script makes
    python3 tools/mklogo.py --png out.png    # the same pixels as a PNG, to look at

How it draws. The SVG is small and is read as the fixed drawing it is (its numbers are below, copied from the file;
--check also confirms the file still says them): a purple square clipped to a rounded rectangle, six circles of the
Apple colours blurred into one another (feGaussianBlur, stdDeviation 4.2), a soft white band (blurred 1.1, half
opaque) and thin vertical stripes under a curve, both faded out downwards by a gradient mask, and a black rounded
outline. Shapes are covered exactly along x and at four rows per pixel; the blurs are what SVG 1.1 says a renderer
may use and browsers do use, three box blurs of d = floor(s * 3 * sqrt(2 pi) / 4 + 0.5), in linear light
(color-interpolation-filters defaults to linearRGB). The result is blended over the splash's background, which the
kernel fills the rest of the screen with, and written as 192 x 192 R,G,B bytes; the kernel scales it to the screen.

Standard library only. The logo is the project's own artwork (MPL-2.0 with the rest of the tools); the header is
generated, not a third-party blob.
"""
import math, os, struct, sys, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SVG = os.path.join(HERE, '..', 'docs', 'img', 'aurora68-logo.svg')
DST = os.path.join(HERE, '..', 'emu68', 'src', 'psmac', 'psmac_splash_logo.h')

N = 192                      # the header's size in pixels (the kernel scales from it: 96 to 256 on screen)
SUB = 4                      # rows sampled per pixel for the shapes' edges
BG = (0x0e, 0x0f, 0x12)      # the splash's background: near-black, a touch of blue (psmac_splash.c says the same)

# the SVG's drawing, in its 32 x 32 user units (docs/img/aurora68-logo.svg)
RR = (1.25, 1.25, 29.5, 29.5, 7.0)                       # the rounded rectangle: clip and outline
STROKE_W = 1.2
PURPLE = '#963d97'
CIRCLES = [(4, 4, 9, '#61bb46'), (20, 2, 8, '#fdb827'), (31, 12, 8, '#f5821f'),
           (26, 27, 9, '#e03a3e'), (10, 31, 9, '#963d97'), (1, 18, 8, '#009ddc')]
BLUR_BIG = 4.2
BLUR_BAND = 1.1
BAND = 'M-2 13 C5 7 10 12 16 9 S26 5 34 9 V15 C27 11.5 21 15.5 15 13 S5 12.5 -2 16Z'     # white, opacity .5, blurred
STRIPES = 'M-2 13 C5 7 10 12 16 9 S26 5 34 9 V26 H-2Z'                                    # the stripe pattern's area
STRIPE_PERIOD, STRIPE_W, STRIPE_A = 1.9, 0.8, 0.14
MASK_FROM, MASK_TO = 0.25, 0.8                           # the mask's gradient: opaque to .25, clear from .8 (of 32)
# the words of the SVG these numbers come from: --check fails if the file no longer has them
SVG_WORDS = ['rx="7"', 'stdDeviation="4.2"', 'stdDeviation="1.1"', 'width="1.9"', 'width=".8"', 'fill-opacity=".14"',
             'offset=".25"', 'offset=".8"', 'stroke-width="1.2"', 'fill="#963d97"', BAND.split(' V')[0]] + \
            ['<circle cx="%g" cy="%g" r="%g" fill="%s"/>' % c for c in CIRCLES]


def hexrgb(h):
    return tuple(int(h[i:i + 2], 16) / 255.0 for i in (1, 3, 5))


def to_lin(c):
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def to_srgb(c):
    c = 0.0 if c < 0 else 1.0 if c > 1 else c
    return c * 12.92 if c <= 0.0031308 else 1.055 * c ** (1 / 2.4) - 0.055


# ---- paths: M, C, S, V, H, Z (absolute), flattened into polygons ----
def parse_path(d):
    toks, num = [], ''
    for ch in d + ' ':
        if ch.isalpha() and ch not in 'e':
            if num: toks.append(float(num)); num = ''
            toks.append(ch)
        elif ch in ' ,':
            if num: toks.append(float(num)); num = ''
        elif ch == '-' and num and num[-1] != 'e':
            toks.append(float(num)); num = '-'
        else:
            num += ch
    pts, cur, last_c2, i, cmd = [], (0.0, 0.0), None, 0, None
    def take(k):
        nonlocal i
        v = toks[i:i + k]; i += k
        return v
    while i < len(toks):
        if isinstance(toks[i], str):
            cmd = toks[i]; i += 1
        if cmd == 'M':
            x, y = take(2); cur = (x, y); pts.append(cur); last_c2 = None
        elif cmd in 'CS':
            if cmd == 'C':
                x1, y1, x2, y2, x, y = take(6)
            else:
                x2, y2, x, y = take(4)
                x1, y1 = (2 * cur[0] - last_c2[0], 2 * cur[1] - last_c2[1]) if last_c2 else cur
            p0 = cur
            for k in range(1, 65):
                t = k / 64.0; u = 1 - t
                pts.append((u * u * u * p0[0] + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x,
                            u * u * u * p0[1] + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y))
            cur = (x, y); last_c2 = (x2, y2)
        elif cmd == 'V':
            (y,) = take(1); cur = (cur[0], y); pts.append(cur); last_c2 = None
        elif cmd == 'H':
            (x,) = take(1); cur = (x, cur[1]); pts.append(cur); last_c2 = None
        elif cmd == 'Z':
            pass
        else:
            raise SystemExit('mklogo.py: path command %s not handled' % cmd)
    return pts


def spans_at(poly, y):
    """the x intervals inside poly (nonzero winding) along the line y, in user units"""
    xs = []
    n = len(poly)
    for k in range(n):
        (x0, y0), (x1, y1) = poly[k], poly[(k + 1) % n]
        if y0 == y1: continue
        if (y0 <= y < y1) or (y1 <= y < y0):
            xs.append((x0 + (y - y0) * (x1 - x0) / (y1 - y0), 1 if y1 > y0 else -1))
    xs.sort()
    out, w, start = [], 0, None
    for x, dr in xs:
        nw = w + dr
        if w == 0 and nw != 0: start = x
        elif w != 0 and nw == 0: out.append((start, x))
        w = nw
    return out


def coverage(poly, w, h, ox, oy, scale, clip=None):
    """per-pixel coverage of poly on a w x h grid whose pixel (0,0) starts at user (ox, oy); clip(a, b) splits a span"""
    cov = [0.0] * (w * h)
    for py in range(h):
        for s in range(SUB):
            y = oy + (py + (s + 0.5) / SUB) / scale
            for a, b in spans_at(poly, y):
                for a2, b2 in (clip(a, b) if clip else [(a, b)]):
                    pa, pb = (a2 - ox) * scale, (b2 - ox) * scale
                    if pb <= 0 or pa >= w: continue
                    pa, pb = max(pa, 0.0), min(pb, float(w))
                    i0, i1 = int(pa), min(int(pb), w - 1)
                    row = py * w
                    if i0 == i1:
                        cov[row + i0] += (pb - pa) / SUB
                    else:
                        cov[row + i0] += (i0 + 1 - pa) / SUB
                        for i in range(i0 + 1, i1): cov[row + i] += 1.0 / SUB
                        cov[row + i1] += (pb - i1) / SUB
    return cov


def stripes(a, b):
    out = []
    k = math.floor(a / STRIPE_PERIOD)
    while k * STRIPE_PERIOD < b:
        s0, s1 = max(a, k * STRIPE_PERIOD), min(b, k * STRIPE_PERIOD + STRIPE_W)
        if s1 > s0: out.append((s0, s1))
        k += 1
    return out


# ---- the box blurs of SVG 1.1's feGaussianBlur ----
def box_pass(src, w, h, d, offset, horizontal):
    """one box blur of width d along rows or columns; the box covers [i - d//2 + offset, ... + d) (offset -1, 0)"""
    out = [0.0] * (w * h)
    lo = -(d // 2) + offset          # the box's first sample, relative to the output pixel
    n, m = (w, h) if horizontal else (h, w)
    for j in range(m):
        base, step = (j * w, 1) if horizontal else (j, w)
        line = [src[base + i * step] for i in range(n)]
        pre = [0.0] * (n + 1)
        for i in range(n): pre[i + 1] = pre[i] + line[i]
        for i in range(n):
            a, b = i + lo, i + lo + d
            a, b = max(a, 0), min(b, n)
            out[base + i * step] = (pre[b] - pre[a]) / d if b > a else 0.0
    return out


def gauss(chan, w, h, sigma_px):
    d = int(math.floor(sigma_px * 3 * math.sqrt(2 * math.pi) / 4 + 0.5))
    for horizontal in (True, False):
        if d % 2:
            for _ in range(3): chan = box_pass(chan, w, h, d, 0, horizontal)
        else:   # two boxes of d, one offset left of the pixel and one right, then one of d + 1 centred
            chan = box_pass(chan, w, h, d, 0, horizontal)
            chan = box_pass(chan, w, h, d, 1, horizontal)
            chan = box_pass(chan, w, h, d + 1, 0, horizontal)
    return chan


def rrect_sd(x, y):
    rx, ry, rw, rh, r = RR
    cx, cy = rx + rw / 2, ry + rh / 2
    qx, qy = abs(x - cx) - (rw / 2 - r), abs(y - cy) - (rh / 2 - r)
    return math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) - r


def render():
    s = N / 32.0
    npx = N * N
    # 1. the circles, on a grid wide enough for the blur's reach, blurred premultiplied in linear light
    sig = BLUR_BIG * s
    d = int(math.floor(sig * 3 * math.sqrt(2 * math.pi) / 4 + 0.5))
    m = 3 * (d // 2 + 1) + 2
    W = N + 2 * m
    layer = [[0.0] * (W * W) for _ in range(4)]          # premultiplied R, G, B (linear) and alpha
    for cx, cy, r, col in CIRCLES:
        lin = [to_lin(c) for c in hexrgb(col)]
        for py in range(W):
            y = (py - m + 0.5) / s
            for px in range(W):
                x = (px - m + 0.5) / s
                a = 0.5 - (math.hypot(x - cx, y - cy) - r) * s
                if a <= 0: continue
                a = 1.0 if a > 1 else a
                i = py * W + px
                for c in range(3): layer[c][i] = lin[c] * a + layer[c][i] * (1 - a)
                layer[3][i] = a + layer[3][i] * (1 - a)
    layer = [gauss(ch, W, W, sig) for ch in layer]
    # 2. the image so far: the purple square, the blurred circles over it (sRGB)
    purple = hexrgb(PURPLE)
    img = [[purple[c]] * npx for c in range(3)]
    for py in range(N):
        for px in range(N):
            j = (py + m) * W + px + m; i = py * N + px
            a = layer[3][j]
            if a <= 0: continue
            a = min(a, 1.0)
            for c in range(3):
                img[c][i] = to_srgb(layer[c][j] / a) * a + img[c][i] * (1 - a)
    # 3. the masked group: the band (white, .5, blurred within its filter region) and the stripes over it
    band = parse_path(BAND)
    bx0, bx1 = min(p[0] for p in band), max(p[0] for p in band)
    by0, by1 = min(p[1] for p in band), max(p[1] for p in band)
    reg = (bx0 - 0.2 * (bx1 - bx0), by0 - 0.2 * (by1 - by0), bx1 + 0.2 * (bx1 - bx0), by1 + 0.2 * (by1 - by0))
    sig2 = BLUR_BAND * s
    d2 = int(math.floor(sig2 * 3 * math.sqrt(2 * math.pi) / 4 + 0.5))
    m2 = 3 * (d2 // 2 + 1) + 2
    W2 = N + 2 * m2
    ba = coverage(band, W2, W2, -m2 / s, -m2 / s, s)
    ba = gauss([min(v, 1.0) * 0.5 for v in ba], W2, W2, sig2)
    sa = coverage(parse_path(STRIPES), N, N, 0.0, 0.0, s, clip=stripes)
    for py in range(N):
        y = (py + 0.5) / s
        t = y / 32.0
        mask = 1.0 if t <= MASK_FROM else 0.0 if t >= MASK_TO else 1.0 - (t - MASK_FROM) / (MASK_TO - MASK_FROM)
        for px in range(N):
            x = (px + 0.5) / s
            i = py * N + px
            a1 = ba[(py + m2) * W2 + px + m2] if reg[0] <= x <= reg[2] and reg[1] <= y <= reg[3] else 0.0
            a2 = min(sa[i], 1.0) * STRIPE_A
            a = (a2 + a1 * (1 - a2)) * mask                  # both white: the group is white at this alpha
            for c in range(3): img[c][i] = a + img[c][i] * (1 - a)
    # 4. clipped to the rounded rectangle, the black outline over it, then over the background
    out = bytearray(npx * 3)
    for py in range(N):
        y = (py + 0.5) / s
        for px in range(N):
            x = (px + 0.5) / s
            i = py * N + px
            sd = rrect_sd(x, y)
            clip = min(max(0.5 - sd * s, 0.0), 1.0)
            ink = min(max(0.5 - (abs(sd) - STROKE_W / 2) * s, 0.0), 1.0)
            for c in range(3):
                v = img[c][i] * clip * (1 - ink)                     # the picture inside, black outline over it
                a = clip + ink - clip * ink
                v = v + (BG[c] / 255.0) * (1 - a)
                out[i * 3 + c] = max(0, min(255, int(v * 255 + 0.5)))
    return bytes(out)


def header(pix):
    rows = []
    for k in range(0, len(pix), 72):
        rows.append('    ' + ','.join(str(b) for b in pix[k:k + 72]) + ',')
    return ('/* SPDX-License-Identifier: MPL-2.0\n'
            ' * psmac_splash_logo.h - the Aurora68 logo for the HDMI splash (psmac_splash.c), %d x %d R,G,B bytes, blended\n'
            ' * over the splash\'s background #%02X%02X%02X. Generated by tools/mklogo.py from docs/img/aurora68-logo.svg, the\n'
            ' * project\'s own artwork: do not edit, change the SVG or the script and run it (make -C mac check compares). */\n'
            '#define SPLASH_LOGO_N %d\n'
            '#define SPLASH_BG_RGB 0x%02X%02X%02Xu\n'
            'static const unsigned char splash_logo_rgb[%d] = {\n%s\n};\n'
            % (N, N, BG[0], BG[1], BG[2], N, BG[0], BG[1], BG[2], len(pix), '\n'.join(rows)))


def read_header(path):
    text = open(path).read()
    body = text[text.index('{') + 1:text.rindex('}')]
    return bytes(int(v) for v in body.replace('\n', ' ').split(',') if v.strip())


def png(path, pix, w, h):
    raw = b''.join(b'\0' + pix[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def main(argv):
    svg = open(SVG).read()
    missing = [w for w in SVG_WORDS if w not in svg]
    if missing:
        sys.exit('mklogo.py: docs/img/aurora68-logo.svg has changed (%s not found): bring the numbers in this script up to date'
                 % ', '.join(missing))
    pix = render()
    if '--png' in argv:
        png(argv[argv.index('--png') + 1], pix, N, N)
        return
    if '--check' in argv:
        old = read_header(DST) if os.path.exists(DST) else b''
        # a byte may differ by one where another machine's pow() rounds the other way
        if len(old) != len(pix) or any(abs(a - b) > 1 for a, b in zip(old, pix)):
            sys.exit('mklogo.py: %s is not what tools/mklogo.py makes: run it' % os.path.relpath(DST))
        print('mklogo.py: the splash logo header matches the SVG')
        return
    open(DST, 'w').write(header(pix))
    print('mklogo.py: wrote %s (%d bytes of pixels)' % (os.path.relpath(DST), len(pix)))


if __name__ == '__main__':
    main(sys.argv[1:])
