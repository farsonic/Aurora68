# SPDX-License-Identifier: MPL-2.0
"""geom.py - footprint geometry shared by the generator, the validator, the renderer and the router glue.

KiCad's conventions: board y grows downwards; a footprint at (X, Y, angle) rotates its local items
counter-clockwise on screen by angle degrees; a pad's (at x y a) is its position in the footprint's frame,
with a the pad's own angle plus the footprint's.
"""

import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sexpr

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PRETTY = os.path.join(ROOT, 'lib', 'aurora68.pretty')


def rot(px, py, deg):
    t = math.radians(deg)
    c, s = math.cos(t), math.sin(t)
    return px * c + py * s, -px * s + py * c


def to_board(X, Y, deg, px, py):
    dx, dy = rot(px, py, deg)
    return X + dx, Y + dy


def load_fp(name):
    return sexpr.load(os.path.join(PRETTY, name + '.kicad_mod'))


def fp_pads(fp):
    """[(number, x, y, w, h, angle, shape, kind, drill, layers)] in the footprint's frame."""
    out = []
    for p in sexpr.find_all(fp, 'pad'):
        at = sexpr.find(p, 'at')
        size = sexpr.find(p, 'size')
        drill = sexpr.find(p, 'drill')
        layers = sexpr.find(p, 'layers')
        dval = 0.0
        if drill:
            nums = [v for v in drill[1:] if isinstance(v, (int, float))]
            dval = float(nums[0]) if nums else 0.0
        out.append((str(p[1]), float(at[1]), float(at[2]), float(size[1]), float(size[2]),
                    float(at[3]) if len(at) > 3 else 0.0, str(p[3]), str(p[2]), dval,
                    [str(l) for l in layers[1:]] if layers else []))
    return out


def fp_courtyard(fp, layer='F.CrtYd'):
    """Bounding box (x0, y0, x1, y1) of the courtyard in the footprint's frame, or None."""
    xs, ys = [], []
    for kind in ('fp_line', 'fp_rect', 'fp_poly', 'fp_circle', 'fp_arc'):
        for g in sexpr.find_all(fp, kind):
            lay = sexpr.find(g, 'layer')
            if not lay or lay[1] != layer:
                continue
            if kind == 'fp_circle':
                c = sexpr.find(g, 'center'); e = sexpr.find(g, 'end')
                r = math.hypot(e[1] - c[1], e[2] - c[2])
                xs += [c[1] - r, c[1] + r]; ys += [c[2] - r, c[2] + r]
                continue
            if kind == 'fp_poly':
                for xy in sexpr.find_all(sexpr.find(g, 'pts'), 'xy'):
                    xs.append(xy[1]); ys.append(xy[2])
                continue
            for k in ('start', 'end', 'mid'):
                v = sexpr.find(g, k)
                if v:
                    xs.append(v[1]); ys.append(v[2])
    if not xs:
        return None
    return min(xs), min(ys), max(xs), max(ys)


def box_to_board(box, X, Y, deg):
    x0, y0, x1, y1 = box
    pts = [to_board(X, Y, deg, x, y) for x, y in ((x0, y0), (x1, y0), (x1, y1), (x0, y1))]
    xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
    return min(xs), min(ys), max(xs), max(ys)


def boxes_overlap(a, b):
    return a[0] < b[2] and b[0] < a[2] and a[1] < b[3] and b[1] < a[3]
