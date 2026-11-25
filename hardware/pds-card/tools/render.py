# SPDX-License-Identifier: MPL-2.0
"""render.py - pictures of Card P without KiCad: the board (PNG and SVG) and each schematic sheet (PNG).

Reads the generated .kicad_pcb and .kicad_sch files back (so the pictures show what is in the files, not what
the generator meant), and draws: the card's outline and Apple's maximum outline with its rear strip, Apple's
height limit as contours, keep-out areas, every footprint's courtyard and pads, tracks and vias if routed,
the Pi 3A+ and the 3B extension, references. Pillow is the only dependency (python3 -m pip install pillow).

    python3 tools/render.py           # img/board.png, img/board.svg, img/sch-*.png
"""

import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sexpr
import design as D
import geom

from PIL import Image, ImageDraw, ImageFont

ROOT = D.ROOT
IMG = os.path.join(ROOT, 'img')
PROJECT = 'aurora68-card-p'


def load_font(size):
    for f in ('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf', '/usr/share/fonts/dejavu/DejaVuSans.ttf'):
        if os.path.exists(f):
            return ImageFont.truetype(f, size)
    return ImageFont.load_default()


class Canvas:
    """The same drawing calls for PNG (Pillow) and SVG (text)."""
    def __init__(self, w, h, scale, ox, oy):
        self.s, self.ox, self.oy = scale, ox, oy
        self.im = Image.new('RGB', (int(w * scale), int(h * scale)), 'white')
        self.d = ImageDraw.Draw(self.im, 'RGBA')
        self.svg = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %.2f %.2f">' %
                    (int(w * scale), int(h * scale), w, h), '<rect width="100%" height="100%" fill="white"/>']
        self.w, self.h = w, h

    def P(self, x, y):
        return ((x - self.ox) * self.s, (y - self.oy) * self.s)

    def poly(self, pts, outline=None, fill=None, width=1):
        q = [self.P(*p) for p in pts]
        self.d.polygon(q, outline=outline, fill=fill, width=width)
        f = ('rgba(%d,%d,%d,%.2f)' % (fill[0], fill[1], fill[2], fill[3] / 255.0)) if fill else 'none'
        o = ('rgb(%d,%d,%d)' % outline[:3]) if outline else 'none'
        self.svg.append('<polygon points="%s" fill="%s" stroke="%s" stroke-width="%.2f"/>' % (
            ' '.join('%.3f,%.3f' % (x - self.ox, y - self.oy) for x, y in pts), f, o, width / self.s))

    def rect(self, x0, y0, x1, y1, **kw):
        self.poly([(x0, y0), (x1, y0), (x1, y1), (x0, y1)], **kw)

    def line(self, x0, y0, x1, y1, color, width=1, dash=False):
        if dash:
            n = max(1, int(math.hypot(x1 - x0, y1 - y0) / 2.0))
            for i in range(0, n, 2):
                a, b = i / n, min(1.0, (i + 1) / n)
                self.d.line([self.P(x0 + (x1 - x0) * a, y0 + (y1 - y0) * a), self.P(x0 + (x1 - x0) * b, y0 + (y1 - y0) * b)],
                            fill=color, width=width)
        else:
            self.d.line([self.P(x0, y0), self.P(x1, y1)], fill=color, width=width)
        self.svg.append('<line x1="%.3f" y1="%.3f" x2="%.3f" y2="%.3f" stroke="rgb(%d,%d,%d)" stroke-width="%.3f"%s/>' % (
            x0 - self.ox, y0 - self.oy, x1 - self.ox, y1 - self.oy, color[0], color[1], color[2], width / self.s,
            ' stroke-dasharray="1,1"' if dash else ''))

    def circle(self, x, y, r, outline=None, fill=None, width=1):
        cx, cy = self.P(x, y)
        rr = r * self.s
        self.d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], outline=outline, fill=fill, width=width)
        self.svg.append('<circle cx="%.3f" cy="%.3f" r="%.3f" fill="%s" stroke="%s" stroke-width="%.3f"/>' % (
            x - self.ox, y - self.oy, r, ('rgb(%d,%d,%d)' % fill[:3]) if fill else 'none',
            ('rgb(%d,%d,%d)' % outline[:3]) if outline else 'none', width / self.s))

    def text(self, x, y, s, color=(0, 0, 0), size=10, anchor='la'):
        self.d.text(self.P(x, y), s, fill=color, font=load_font(size), anchor=anchor)
        a = {'la': 'start', 'ma': 'middle', 'mm': 'middle', 'ra': 'end', 'lm': 'start'}.get(anchor, 'start')
        self.svg.append('<text x="%.3f" y="%.3f" font-family="sans-serif" font-size="%.2f" text-anchor="%s" fill="rgb(%d,%d,%d)">%s</text>' % (
            x - self.ox, y - self.oy + size / self.s * 0.8, size / self.s, a, color[0], color[1], color[2],
            s.replace('&', '&amp;').replace('<', '&lt;')))

    def save(self, png, svg=None):
        self.im.save(png, optimize=True)
        if svg:
            with open(svg, 'w') as f:
                f.write('\n'.join(self.svg + ['</svg>']) + '\n')


def board_items(pcb):
    fps = []
    for fp in sexpr.find_all(pcb, 'footprint'):
        at = sexpr.find(fp, 'at')
        X, Y, ang = float(at[1]), float(at[2]), float(at[3]) if len(at) > 3 else 0.0
        side = sexpr.find(fp, 'layer')[1][0]
        ref = [c[2] for c in sexpr.find_all(fp, 'property') if c[1] == 'Reference'][0]
        crt = geom.fp_courtyard(fp, 'B.CrtYd' if side == 'B' else 'F.CrtYd')
        fps.append((ref, X, Y, ang, side, crt, geom.fp_pads(fp), fp))
    return fps


def render_board():
    pcb = sexpr.load(os.path.join(ROOT, PROJECT + '.kicad_pcb'))
    from gen_kicad import OX, OY
    pad = 8.0
    c = Canvas(D.APPLE_W + 2 * pad, D.APPLE_H + 2 * pad + 22, 6.0, OX - pad, OY - pad)
    # Apple's maximum outline and rear strip
    apple = [(0, 0), (D.APPLE_W, 0), (D.APPLE_W, D.APPLE_H), (D.APPLE_TAB_X, D.APPLE_H), (D.APPLE_TAB_X, D.APPLE_BODY_H),
             (0, D.APPLE_BODY_H)]
    A = lambda pts: [(x + OX, y + OY) for x, y in pts]
    c.poly(A(apple), outline=(150, 150, 150), width=1)
    c.rect(OX, OY, OX + D.APPLE_W, OY + D.APPLE_REAR_STRIP, outline=(150, 150, 150), fill=(200, 200, 200, 90))
    c.text(OX + 2, OY + 6, "Apple's rear strip: no components (16 mm); this card's edge is at 21 mm", (90, 90, 90), 11)
    # the board
    c.poly(A(D.card_outline()), outline=(0, 90, 0), fill=(225, 245, 225, 255), width=3)
    # height contours
    for h in range(6, 16, 1):
        y = (h - D.H_REAR) / (D.H_FRONT - D.H_REAR) * D.APPLE_BODY_H
        if y < D.CARD_REAR_Y:
            continue
        c.line(OX, OY + y, OX + D.APPLE_W, OY + y, (120, 160, 220), 1, dash=True)
        c.text(OX + 0.6, OY + y - 2.6, '%d mm' % h, (90, 120, 200), 9)
    # rule areas
    for z in sexpr.find_all(pcb, 'zone'):
        name = sexpr.find(z, 'name')
        pts = [(xy[1], xy[2]) for xy in sexpr.find_all(sexpr.find(sexpr.find(z, 'polygon'), 'pts'), 'xy')]
        if sexpr.find(z, 'keepout'):
            col = (230, 60, 60, 60) if 'PI3B' in name[1] else (240, 150, 40, 60)
            c.poly(pts, outline=col[:3], fill=col, width=1)
    # the Pi outlines
    xa, ya = D.pi_xy(0, 0); xb, yb = D.pi_xy(65, 56)
    xa, xb, ya, yb = min(xa, xb), max(xa, xb), min(ya, yb), max(ya, yb)
    c.rect(xa + OX, ya + OY, xb + OX, yb + OY, outline=(200, 0, 120), width=2)
    c.text(xa + OX + 1.5, ya + OY + 1.5, 'Pi 3A+ (components down, 8.5 mm above the card; top at %.1f mm)' % D.PI_TOP, (200, 0, 120), 11)
    for (x0, y0, x1, y1, name, what) in D.keepouts():
        if name == 'PI3B_EXT':
            c.text(x0 + OX + 0.8, y0 + OY + 8, 'Pi 3B/3B+', (200, 30, 30), 10)
            c.text(x0 + OX + 0.8, y0 + OY + 11, 'USB/RJ45', (200, 30, 30), 10)
            c.text(x0 + OX + 0.8, y0 + OY + 14, 'keep-out', (200, 30, 30), 10)
    # tracks and vias
    for seg in sexpr.find_all(pcb, 'segment'):
        s, e, w = sexpr.find(seg, 'start'), sexpr.find(seg, 'end'), sexpr.find(seg, 'width')[1]
        lay = sexpr.find(seg, 'layer')[1]
        c.line(s[1], s[2], e[1], e[2], (200, 40, 40) if lay == 'F.Cu' else (40, 80, 220), max(1, int(w * c.s)))
    for v in sexpr.find_all(pcb, 'via'):
        at = sexpr.find(v, 'at')
        c.circle(at[1], at[2], 0.3, fill=(60, 60, 60))
    # footprints
    for ref, X, Y, ang, side, crt, pads, fp in board_items(pcb):
        if crt:
            bx = geom.box_to_board(crt, X, Y, ang)
            col = (40, 40, 160) if side == 'F' else (160, 40, 160)
            c.rect(*bx, outline=col, fill=col + (25,), width=2)
        for (num, px, py, w, h, pa, shape, kind, drill, layers) in pads:
            x, y = geom.to_board(X, Y, ang, px, py)
            if kind == 'np_thru_hole':
                c.circle(x, y, drill / 2, outline=(0, 0, 0), fill=(255, 255, 255), width=2)
                continue
            r = min(w, h) / 2
            c.circle(x, y, max(r, 0.2), fill=(190, 140, 0) if kind == 'smd' else (150, 110, 30))
        if crt:
            bx = geom.box_to_board(crt, X, Y, ang)
            size = 11 if (bx[2] - bx[0]) > 6 else 8
            c.text((bx[0] + bx[2]) / 2, (bx[1] + bx[3]) / 2, ref, (0, 0, 0), size, 'mm')
    # plug orientation
    x, y = D.plug_pin_xy('A', 1)
    c.text(x + OX + 1.5, y + OY + 1.0, 'A1', (160, 0, 160), 11)
    x, y = D.plug_pin_xy('C', 32)
    c.text(x + OX - 6, y + OY - 4.0, 'C32', (160, 0, 160), 11)
    yl = OY + D.APPLE_H + 4
    c.text(OX, yl, 'Aurora68 Card P (draft): top view as Apple\'s Figure 17-1. Front of the SE at the bottom, rear at the top; '
           'the slot plug J1 (purple, underside) at the right.', (0, 0, 0), 12)
    c.text(OX, yl + 4, 'Blue dashed: Apple\'s height limit (Fig. 17-2), 4.61 mm at the rear edge to 15.58 mm at the front. '
           'Orange: keep-outs under the Pi\'s parts; red: the Pi 3B/3B+ USB and Ethernet keep-out.', (0, 0, 0), 12)
    c.text(OX, yl + 8, 'Generated by tools/render.py from aurora68-card-p.kicad_pcb.', (0, 0, 0), 12)
    c.save(os.path.join(IMG, 'board.png'), os.path.join(IMG, 'board.svg'))


def render_models():
    """The board from above with the generated 3D models' boxes drawn where KiCad would put them (the Pi 3A+ on its
    socket, the plug's body), to check them against the keep-outs: img/board-models.png."""
    import re
    render_board()
    from gen_kicad import OX, OY
    im = Image.open(os.path.join(IMG, 'board.png')).convert('RGB')
    d = ImageDraw.Draw(im, 'RGBA')
    pad, sc = 8.0, 6.0
    P = lambda x, y: ((x - (OX - pad)) * sc, (y - (OY - pad)) * sc)
    pcb = sexpr.load(os.path.join(ROOT, PROJECT + '.kicad_pcb'))
    for fp in sexpr.find_all(pcb, 'footprint'):
        at = sexpr.find(fp, 'at')
        X, Y = float(at[1]), float(at[2])
        for m in sexpr.find_all(fp, 'model'):
            path = m[1]
            if 'aurora68.3dshapes' not in path:
                continue
            f = os.path.join(ROOT, 'lib', 'aurora68.3dshapes', os.path.basename(path))
            for t in re.finditer(r'translation ([-\d.]+) ([-\d.]+) ([-\d.]+).*?diffuseColor ([\d.]+) ([\d.]+) ([\d.]+).*?size ([\d.]+) ([\d.]+) ([\d.]+)',
                                 open(f).read()):
                cx, cy, cz, r, g, b, sx, sy, sz = [float(v) for v in t.groups()]
                x0, x1 = X + (cx - sx / 2) * 2.54, X + (cx + sx / 2) * 2.54
                y0, y1 = Y - (cy + sy / 2) * 2.54, Y - (cy - sy / 2) * 2.54
                d.rectangle([P(x0, y0), P(x1, y1)], outline=(int(r * 255), int(g * 255), int(b * 255)), width=3,
                            fill=(int(r * 255), int(g * 255), int(b * 255), 60))
    d.text((20, im.size[1] - 40), 'The generated 3D models (tools/vrml.py) drawn from above: the Pi 3A+ components-down on its '
           'socket and the plug body under J1. The red Pi 3B/3B+ zone stays clear of parts.', fill=(0, 0, 0), font=load_font(13))
    im.save(os.path.join(IMG, 'board-models.png'), optimize=True)
    render_board()


def render_sheet(fname):
    sch = sexpr.load(os.path.join(ROOT, fname))
    paper = sexpr.find(sch, 'paper')[1]
    W, H = {'A3': (420, 297), 'A2': (594, 420), 'A4': (297, 210)}[paper]
    c = Canvas(W, H, 4.0, 0, 0)
    c.rect(5, 5, W - 5, H - 5, outline=(120, 0, 0), width=2)
    libs = {}
    ls = sexpr.find(sch, 'lib_symbols')
    for s in sexpr.find_all(ls, 'symbol'):
        libs[s[1]] = s
    for t in sexpr.find_all(sch, 'text'):
        at = sexpr.find(t, 'at')
        c.text(at[1], at[2] - 3, t[1], (0, 0, 0), 14)
    for sym in sexpr.find_all(sch, 'symbol'):
        lid = sexpr.find(sym, 'lib_id')[1]
        at = sexpr.find(sym, 'at')
        X, Y = at[1], at[2]
        lib = libs.get(lid)
        if lib is None:
            continue
        for sub in sexpr.find_all(lib, 'symbol'):
            for r in sexpr.find_all(sub, 'rectangle'):
                s, e = sexpr.find(r, 'start'), sexpr.find(r, 'end')
                c.rect(X + s[1], Y - s[2], X + e[1], Y - e[2], outline=(130, 0, 0), fill=(255, 255, 210, 255), width=2)
            for r in sexpr.find_all(sub, 'circle'):
                ce, rad = sexpr.find(r, 'center'), sexpr.find(r, 'radius')[1]
                c.circle(X + ce[1], Y - ce[2], rad, outline=(130, 0, 0), width=2)
            for pin in sexpr.find_all(sub, 'pin'):
                pa = sexpr.find(pin, 'at'); ln = sexpr.find(pin, 'length')[1]
                x0, y0 = X + pa[1], Y - pa[2]
                dx = ln if pa[3] == 0 else -ln if pa[3] == 180 else 0
                dy = -ln if pa[3] == 90 else ln if pa[3] == 270 else 0
                c.line(x0, y0, x0 + dx, y0 + dy, (130, 0, 0), 2)
                nm = sexpr.find(pin, 'name')[1].replace('~{', '/').replace('}', '')
                num = sexpr.find(pin, 'number')[1]
                if nm != '~':
                    if pa[3] == 0:
                        c.text(x0 + dx + 0.6, y0, nm, (0, 100, 100), 8, 'lm')
                    elif pa[3] == 180:
                        c.text(x0 + dx - 0.6, y0, nm, (0, 100, 100), 8, 'rm' if False else 'lm') if False else \
                            c.text(x0 + dx - 0.6 - len(nm) * 1.1, y0, nm, (0, 100, 100), 8, 'lm')
                    c.text(x0 + dx / 2, y0 - 1.6, num, (130, 0, 0), 7, 'ma')
        for prop in sexpr.find_all(sym, 'property'):
            if prop[1] in ('Reference', 'Value'):
                e = sexpr.find(prop, 'effects')
                if e and sexpr.find(e, 'hide'):
                    continue
                pa = sexpr.find(prop, 'at')
                c.text(pa[1], pa[2], prop[2], (0, 0, 0) if prop[1] == 'Reference' else (80, 80, 80), 11, 'mm')
    for w in sexpr.find_all(sch, 'wire'):
        pts = sexpr.find_all(sexpr.find(w, 'pts'), 'xy')
        c.line(pts[0][1], pts[0][2], pts[1][1], pts[1][2], (0, 130, 0), 2)
    for kind, col in (('label', (0, 0, 160)), ('global_label', (160, 0, 120))):
        for l in sexpr.find_all(sch, kind):
            at = sexpr.find(l, 'at')
            name = l[1]
            if at[3] == 180:
                c.text(at[1] - 0.6 - len(name) * 1.25, at[2], name, col, 9, 'lm')
            elif at[3] == 270:
                c.text(at[1], at[2] + 1.0, name, col, 9, 'ma')
            else:
                c.text(at[1] + 0.6, at[2], name, col, 9, 'lm')
    for nc in sexpr.find_all(sch, 'no_connect'):
        at = sexpr.find(nc, 'at')
        c.line(at[1] - 0.8, at[2] - 0.8, at[1] + 0.8, at[2] + 0.8, (0, 0, 200), 2)
        c.line(at[1] - 0.8, at[2] + 0.8, at[1] + 0.8, at[2] - 0.8, (0, 0, 200), 2)
    for sh in sexpr.find_all(sch, 'sheet'):
        at, sz = sexpr.find(sh, 'at'), sexpr.find(sh, 'size')
        c.rect(at[1], at[2], at[1] + sz[1], at[2] + sz[2], outline=(120, 0, 120), width=3)
        for prop in sexpr.find_all(sh, 'property'):
            pa = sexpr.find(prop, 'at')
            c.text(pa[1], pa[2] - (3 if prop[1] == 'Sheetname' else 0), prop[2], (120, 0, 120), 13)
    tb = sexpr.find(sch, 'title_block')
    title = sexpr.find(tb, 'title')[1]
    c.text(W - 200, H - 14, title, (0, 0, 0), 16)
    c.text(W - 200, H - 9, 'rev %s  %s' % (sexpr.find(tb, 'rev')[1], sexpr.find(tb, 'date')[1]), (0, 0, 0), 12)
    out = os.path.join(IMG, 'sch-' + fname.replace('.kicad_sch', '') + '.png')
    c.save(out)


def main():
    os.makedirs(IMG, exist_ok=True)
    render_models()
    for f in [PROJECT + '.kicad_sch'] + ['%s.kicad_sch' % s for s, _, _ in D.SHEETS]:
        render_sheet(f)
    print('pictures in', IMG)


if __name__ == '__main__':
    main()
