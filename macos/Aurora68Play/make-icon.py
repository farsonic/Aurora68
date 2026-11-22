#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""make-icon.py - the macOS Aurora68 Play's icon: a vinyl record under an aurora, in the modern Mac's style.

    python3 make-icon.py OUT.iconset          the PNGs iconutil turns into AppIcon.icns (make-app.sh)
    python3 make-icon.py --preview OUT.png    the 1024-pixel icon beside its 256, 64, 32 and 16, on light and dark

Backstory. The first version (22 November 2026, with the macOS app) scaled the SE's 32x32 pixel-art icon
(tools/mac/mkicons.py) up by whole pixels. The owner then asked for a "Photorealistic Icon for MacOS
(modern)", the same day. So the icon is now painted here, procedurally, with nothing taken from anyone
else's artwork: Apple's icon grid (an 824-pixel rounded square, a superellipse, on a 1024 canvas, with a
soft shadow under it), a night sky with the aurora that names the project (curtains that are bright at
their lower edge and fade upward, green turning violet, with vertical rays), stars and dark hills, and in
front a black vinyl record: fine grooves, the gaps between songs, the two glossy wedges a record shows
under a light (tinted by the aurora it reflects), a soft specular, a rim catching the light, a soft
shadow on the sky, and a label in the aurora's green. Every size is rendered from the 1024-pixel picture
(Lanczos), so the record and its label stay legible at 16 pixels.

It needs numpy and Pillow (GitHub's macOS runner installs them in a virtual environment for make-app.sh,
.github/workflows/macos-player.yml). Without them it falls back to the old pixel-art icon, and says so.
"""
import os, struct, sys, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SIZES = [('16x16', 16), ('16x16@2x', 32), ('32x32', 32), ('32x32@2x', 64), ('128x128', 128),
         ('128x128@2x', 256), ('256x256', 256), ('256x256@2x', 512), ('512x512', 512), ('512x512@2x', 1024)]
S = 1024                                  # the canvas; Apple's grid: the shape is 824 wide, 100 in from each side


# ---- the painted icon (numpy, Pillow) ----------------------------------------------------------------
def paint():
    import numpy as np
    from PIL import Image, ImageFilter

    y, x = np.mgrid[0:S, 0:S].astype(np.float64) + 0.5
    rng = np.random.default_rng(68)

    def smooth(e0, e1, v):
        t = np.clip((v - e0) / (e1 - e0), 0.0, 1.0)
        return t * t * (3 - 2 * t)

    def blur(a, r):
        im = Image.fromarray(np.clip(a * 255, 0, 255).astype(np.uint8), 'L').filter(ImageFilter.GaussianBlur(r))
        return np.asarray(im).astype(np.float64) / 255

    def over(dst, colour, alpha):
        a = alpha[..., None]
        return dst * (1 - a) + np.asarray(colour, np.float64) * a

    # the shape: a superellipse of exponent 5, close to Apple's continuous-corner rounded square
    half, n = 412.0, 5.0
    f = (np.abs(x - 512) / half) ** n + (np.abs(y - 512) / half) ** n
    shape = np.clip(0.5 - half * (f ** (1 / n) - 1), 0, 1)

    # the sky: deep blue at the top, violet near the horizon
    t = (y - 100) / 824
    sky = np.stack([8 + 22 * t, 12 + 6 * t, 40 + 18 * t], -1)

    # stars, more of them high up
    stars = np.zeros((S, S))
    for _ in range(90):
        sx, sy = rng.uniform(120, 904), rng.uniform(110, 500)
        if rng.random() < (sy - 110) / 520:
            continue
        b, rad = rng.uniform(0.35, 1.0), rng.uniform(1.2, 2.6)
        stars += b * np.exp(-((x - sx) ** 2 + (y - sy) ** 2) / (2 * rad ** 2))
    sky = over(sky, (235, 240, 255), np.clip(stars, 0, 1))

    # the aurora: curtains, bright at their wavy lower edge, fading upward, with rays
    glow = np.zeros((S, S, 3))
    curtains = [  # base height, wave amplitude, wave frequency, phase, height of fade, strength, upper colour
        (430, 48, 0.0085, 0.6, 170, 1.00, (150, 90, 255)),
        (360, 36, 0.0120, 2.4, 120, 0.75, (90, 140, 255)),
        (505, 30, 0.0070, 4.1, 150, 0.55, (200, 110, 255)),
    ]
    for base, amp, fr, ph, up, k, top in curtains:
        edge = base + amp * np.sin(fr * x + ph) + 0.35 * amp * np.sin(2.7 * fr * x + 1.3 * ph)
        d = edge - y                                    # above the edge: positive
        body = np.where(d >= 0, np.exp(-d / up), np.exp(-(d / 10) ** 2))
        rays = 0.55 + 0.45 * (0.5 + 0.5 * np.sin(0.11 * x + 3 * np.sin(0.013 * x + ph)) *
                              (0.6 + 0.4 * np.sin(0.037 * x + 2 * ph)))
        a = k * body * rays
        mix = np.clip(d / (up * 1.4), 0, 1)[..., None]  # green at the edge, the upper colour above
        col = (1 - mix) * np.array([70, 255, 170.0]) + mix * np.array(top, np.float64)
        glow += col * a[..., None]
    sky = 255 - (255 - sky) * np.exp(-glow / 255 * 1.15)  # added as light: screen-like, never clipping hard

    # the aurora's light scattered low in the sky, above the hills
    sky = over(sky, (40, 150, 130), 0.16 * np.exp(-((y - 760) / 90.0) ** 2))

    # hills along the bottom, a faint green reflected on their tops
    ridge = 790 + 40 * np.sin(0.006 * x + 0.8) + 18 * np.sin(0.019 * x + 2.0)
    hills = smooth(-1.0, 1.0, y - ridge)
    sky = over(sky, (6, 8, 22), hills)
    sky = over(sky, (60, 200, 150), 0.25 * np.exp(-((y - ridge) / 3.0) ** 2))

    img = sky

    # the record
    cx, cy, R = 512.0, 598.0, 268.0
    dx, dy = x - cx, y - cy
    r = np.hypot(dx, dy)
    disc = np.clip(R - r + 0.5, 0, 1)
    shadow = blur(np.clip(R - np.hypot(x - cx, y - cy - 22) + 0.5, 0, 1), 26) * 0.65
    img = over(img, (0, 0, 6), shadow * shape)

    theta = np.arctan2(dy, dx)
    groove = 0.5 + 0.5 * np.sin(2 * np.pi * r / 3.2)
    gaps = sum(np.exp(-((r - g) / 2.2) ** 2) for g in (132, 171, 204, 236))
    lead = smooth(R - 14, R - 10, r)                    # the smooth lead-in at the edge
    base = 11 + 5 * groove * (1 - lead) + 10 * gaps
    vinyl = np.stack([base, base, base * 1.08], -1)
    light = -0.75                                       # the light: from the upper left
    wedge1 = np.abs(np.cos(theta - light)) ** 10 * (np.cos(theta - light) > 0)
    wedge2 = np.abs(np.cos(theta - light)) ** 10 * (np.cos(theta - light) < 0)
    grain = 0.55 + 0.45 * groove
    falloff = smooth(95, 150, r)                        # none on the label
    vinyl = over(vinyl, (150, 255, 210), 0.42 * wedge1 * grain * falloff)
    vinyl = over(vinyl, (190, 150, 255), 0.30 * wedge2 * grain * falloff)
    spec = np.exp(-((x - (cx - 120)) ** 2 + (y - (cy - 140)) ** 2) / (2 * 75.0 ** 2))
    vinyl = over(vinyl, (255, 255, 255), 0.10 * spec * falloff)
    rim = np.exp(-((r - (R - 1.5)) / 1.6) ** 2) * (0.35 + 0.65 * np.clip(-np.sin(theta) * 0.8 + 0.4, 0, 1))
    vinyl = over(vinyl, (200, 230, 255), 0.32 * rim)

    # the label: the aurora's green, two printed rings, a little shade, the spindle hole
    lab_r = 94.0
    lab = np.clip(lab_r - r + 0.5, 0, 1)
    u = np.clip(r / lab_r, 0, 1)[..., None]
    lcol = (1 - u) * np.array([90, 240, 175.0]) + u * np.array([20, 130, 140.0])
    lcol = lcol * (1 - 0.18 * np.clip((dy / lab_r)[..., None], 0, 1))
    for ring, w in ((80.0, 1.4), (34.0, 1.2)):
        lcol = over(lcol, (8, 60, 70), 0.55 * np.exp(-((r - ring) / w) ** 2))
    lcol = over(lcol, (255, 255, 255), 0.18 * np.exp(-((x - (cx - 30)) ** 2 + (y - (cy - 35)) ** 2) / (2 * 30.0 ** 2)))
    vinyl = over(vinyl, (6, 40, 46), np.exp(-((r - lab_r - 1.5) / 2.0) ** 2) * 0.8)
    vinyl = vinyl * (1 - lab[..., None]) + lcol * lab[..., None]
    hole = np.clip(9.0 - r + 0.5, 0, 1)
    vinyl = over(vinyl, (4, 6, 10), hole)
    vinyl = over(vinyl, (180, 255, 230), 0.5 * np.exp(-((r - 9.5) / 1.0) ** 2) * (dy > 0))
    img = img * (1 - disc[..., None]) + vinyl * disc[..., None]

    # the shape's light: brighter at the top, a fine rim, a little darker at the bottom
    img = over(img, (255, 255, 255), 0.07 * smooth(560, 100, y))
    edge = np.clip(1 - np.abs(half * (f ** (1 / n) - 1) + 2.0) / 1.8, 0, 1)
    img = over(img, (255, 255, 255), 0.22 * edge * smooth(520, 140, y))
    img = over(img, (0, 0, 0), 0.10 * smooth(700, 924, y))

    # the shadow under the shape, inside the canvas as macOS icons have it
    f2 = (np.abs(x - 512) / half) ** n + (np.abs(y - 524) / half) ** n
    drop = blur(np.clip(0.5 - half * (f2 ** (1 / n) - 1), 0, 1), 14)
    alpha = np.clip(shape + drop * 0.45 * (1 - shape), 0, 1)
    rgb = img * shape[..., None]                        # premultiplied, then out again
    out = np.zeros((S, S, 4))
    out[..., 3] = alpha
    out[..., :3] = np.where(alpha[..., None] > 0, rgb / np.maximum(alpha[..., None], 1e-6), 0)
    return Image.fromarray(np.clip(out * [1, 1, 1, 255], 0, 255).astype(np.uint8), 'RGBA')


def sized(master, size):
    from PIL import Image, ImageFilter
    im = master if size == S else master.resize((size, size), Image.LANCZOS)
    if size <= 32:                                      # a little crisper where every pixel counts
        im = im.filter(ImageFilter.UnsharpMask(radius=0.6, percent=60, threshold=0))
    return im


def preview(master, path):
    from PIL import Image
    w = 1024 + 40 + 256 + 20 + 64 + 20 + 32 + 20 + 16 + 40
    sheet = Image.new('RGBA', (w + 20, 1024 + 40 + 300), (236, 236, 240, 255))
    dark = Image.new('RGBA', (w - 1024 - 40, 300), (30, 30, 34, 255))
    sheet.paste(dark, (1024 + 60, 1024 + 40))
    sheet.alpha_composite(master, (20, 20))
    xs = 1024 + 60
    for size in (256, 64, 32, 16):
        im = sized(master, size)
        sheet.alpha_composite(im, (xs, 20 + (256 - size) // 2))
        sheet.alpha_composite(im, (xs, 1024 + 40 + (300 - size) // 2))
        xs += size + 20
    sheet.save(path)


# ---- the old pixel-art icon, where numpy or Pillow are missing ---------------------------------------
def png_rgba(path, rows):
    h, w = len(rows), len(rows[0])
    raw = b''.join(b'\0' + bytes(v for px in r for v in px) for r in rows)

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def render_pixels(size):
    sys.path.insert(0, os.path.join(HERE, '..', '..', 'tools', 'mac'))
    import mkicons
    n = 16 if size <= 16 else 32
    c = mkicons.canvas('play', n)
    scale = max(1, int(size * 0.82) // n)
    pad = (size - n * scale) // 2
    rows = [[(0, 0, 0, 0)] * size for _ in range(size)]
    for yy in range(n):
        for xx in range(n):
            p = c.px[yy][xx]
            if p is None:
                continue
            for dy in range(scale):
                row = rows[pad + yy * scale + dy]
                for dx in range(scale):
                    row[pad + xx * scale + dx] = p[1] + (255,)
    return rows


def main():
    args = sys.argv[1:]
    if len(args) == 2 and args[0] == '--preview':
        preview(paint(), args[1])
        print(f'{args[1]}: the icon at 1024, 256, 64, 32 and 16 pixels')
        return
    if len(args) != 1:
        sys.exit(__doc__)
    out = args[0]
    os.makedirs(out, exist_ok=True)
    try:
        master = paint()
    except ImportError as e:
        print(f'make-icon.py: {e}: the pixel-art icon instead (pip install numpy pillow for the painted one)')
        for name, size in SIZES:
            png_rgba(os.path.join(out, f'icon_{name}.png'), render_pixels(size))
        return
    for name, size in SIZES:
        sized(master, size).save(os.path.join(out, f'icon_{name}.png'))
    print(f'{out}: {len(SIZES)} icons, painted')


if __name__ == '__main__':
    main()
