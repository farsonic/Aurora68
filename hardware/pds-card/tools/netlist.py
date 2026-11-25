# SPDX-License-Identifier: MPL-2.0
"""netlist.py - read the written KiCad files back into netlists, without KiCad.

schematic_netlist() follows the hierarchical schematic as KiCad would for these files: every symbol's pin
positions from its embedded library symbol, wires joining points, local labels naming a net within a sheet,
global labels across sheets, no-connect flags. It does not handle rotated or mirrored symbols, buses or
hierarchical pins, which gen_kicad.py never writes (it says so if it meets one).

board_netlist() reads the pads' nets and absolute positions from the .kicad_pcb.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sexpr
import geom

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROJECT = 'aurora68-card-p'


class UF:
    def __init__(self):
        self.p = {}

    def f(self, a):
        self.p.setdefault(a, a)
        while self.p[a] != a:
            self.p[a] = self.p[self.p[a]]
            a = self.p[a]
        return a

    def u(self, a, b):
        self.p[self.f(a)] = self.f(b)


def key(x, y):
    return (round(x, 3), round(y, 3))


def lib_pins(sym):
    out = []
    for sub in sexpr.find_all(sym, 'symbol'):
        for pin in sexpr.find_all(sub, 'pin'):
            at = sexpr.find(pin, 'at')
            out.append((sexpr.find(pin, 'number')[1], sexpr.find(pin, 'name')[1], str(pin[1]), at[1], at[2]))
    return out


def sheet_files():
    root = sexpr.load(os.path.join(ROOT, PROJECT + '.kicad_sch'))
    return [PROJECT + '.kicad_sch'] + [sexpr.find_all(s, 'property')[1][2] for s in sexpr.find_all(root, 'sheet')]


def schematic_netlist():
    """{(ref, pin): (net or None, pin name, pin type, sheet)}, {net: [(ref, pin)]}, problems."""
    problems = []
    pins = {}
    globals_ = UF()
    sheet_nets = []
    for fname in sheet_files():
        sch = sexpr.load(os.path.join(ROOT, fname))
        libs = {s[1]: s for s in sexpr.find_all(sexpr.find(sch, 'lib_symbols'), 'symbol')}
        uf = UF()
        names = {}
        nc = set()
        pinpos = []
        for w in sexpr.find_all(sch, 'wire'):
            pts = sexpr.find_all(sexpr.find(w, 'pts'), 'xy')
            a, b = key(pts[0][1], pts[0][2]), key(pts[1][1], pts[1][2])
            uf.u(a, b)
        for kind in ('label', 'global_label'):
            for l in sexpr.find_all(sch, kind):
                at = sexpr.find(l, 'at')
                k = key(at[1], at[2])
                uf.f(k)
                names.setdefault(k, set()).add((l[1], kind == 'global_label'))
        for n in sexpr.find_all(sch, 'no_connect'):
            at = sexpr.find(n, 'at')
            nc.add(key(at[1], at[2]))
        for sym in sexpr.find_all(sch, 'symbol'):
            lib = libs.get(sexpr.find(sym, 'lib_id')[1])
            at = sexpr.find(sym, 'at')
            if len(at) > 3 and at[3]:
                problems.append('%s: rotated symbol not handled' % fname)
            if sexpr.find(sym, 'mirror'):
                problems.append('%s: mirrored symbol not handled' % fname)
            ref = [p[2] for p in sexpr.find_all(sym, 'property') if p[1] == 'Reference'][0]
            for num, name, typ, px, py in lib_pins(lib):
                k = key(at[1] + px, at[2] - py)
                uf.f(k)
                pinpos.append((ref, num, name, typ, k))
        # a wire endpoint lying on another wire's middle would need a junction; gen_kicad never draws one
        groups = {}
        for k in list(uf.p):
            groups.setdefault(uf.f(k), []).append(k)
        gname = {}
        for g, ks in groups.items():
            nm = set()
            for k in ks:
                nm |= names.get(k, set())
            if len({n for n, _ in nm}) > 1:
                problems.append('%s: one wire group carries two names %s' % (fname, sorted(nm)))
            gname[g] = sorted(nm)[0] if nm else None
        for ref, num, name, typ, k in pinpos:
            g = uf.f(k)
            nm = gname.get(g)
            others = [x for x in pinpos if uf.f(x[4]) == g and x is not None and (x[0], x[1]) != (ref, num)]
            if nm is None:
                if k in nc:
                    pins[(ref, num)] = (None, name, typ, fname)
                elif others:
                    pins[(ref, num)] = ('unnamed:%s:%s.%s' % (fname, ref, num), name, typ, fname)
                else:
                    pins[(ref, num)] = (None, name, typ, fname)
                    if not ref.startswith('#'):
                        problems.append('%s: %s pin %s (%s) is unconnected and has no no-connect flag' % (fname, ref, num, name))
            else:
                pins[(ref, num)] = (nm[0], name, typ, fname)
    nets = {}
    for (ref, num), (net, name, typ, fname) in pins.items():
        if net:
            nets.setdefault(net, []).append((ref, num))
    return pins, nets, problems


def board_netlist():
    """{(ref, pad): (net, x, y)} with board coordinates, and the footprint list."""
    pcb = sexpr.load(os.path.join(ROOT, PROJECT + '.kicad_pcb'))
    pads = {}
    fps = []
    for fp in sexpr.find_all(pcb, 'footprint'):
        at = sexpr.find(fp, 'at')
        X, Y, ang = float(at[1]), float(at[2]), float(at[3]) if len(at) > 3 else 0.0
        ref = [c[2] for c in sexpr.find_all(fp, 'property') if c[1] == 'Reference'][0]
        side = sexpr.find(fp, 'layer')[1][0]
        fps.append((ref, X, Y, ang, side, fp))
        for p in sexpr.find_all(fp, 'pad'):
            pat = sexpr.find(p, 'at')
            x, y = geom.to_board(X, Y, ang, float(pat[1]), float(pat[2]))
            n = sexpr.find(p, 'net')
            num = str(p[1])
            if num == '':
                continue
            pads.setdefault((ref, num), []).append((n[2] if n else None, x, y))
    return pcb, pads, fps
