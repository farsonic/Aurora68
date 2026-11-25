# SPDX-License-Identifier: MPL-2.0
"""route.py - route Card P with Freerouting, through a Specctra DSN written here and its SES read back.

    python3 tools/route.py export                 # route/aurora68-card-p.dsn from the .kicad_pcb
    java -jar freerouting-2.2.4-executable.jar --gui.enabled=false -de route/aurora68-card-p.dsn \
         -do route/aurora68-card-p.ses -mp 40       # Freerouting 2.2.x needs Java 25 (app.freerouting on Maven Central)
    python3 tools/route.py import                 # the SES's tracks and vias into the .kicad_pcb
    python3 tools/route.py run FREEROUTING.jar [JAVA]   # all three

The SES is kept in route/, so `gen_kicad.py` followed by `route.py import` rebuilds the routed board without
running Freerouting again (as long as the placement has not changed: import refuses an SES whose DSN is not the
one export writes now). KiCad's own Specctra exporter would do the same job; this one exists because KiCad is
not installed where the design was made. Each footprint becomes one image with its pads already turned to the
board's orientation, placed at rotation 0; NPTH holes become keep-outs; GND is routed as well as poured, so its
connection does not depend on the zones alone.
"""

import hashlib
import math
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sexpr
from sexpr import Sym, S
import geom
import design as D
from gen_kicad import PROJECT, uid, OX, OY

ROOT = D.ROOT
RDIR = os.path.join(ROOT, 'route')
DSN = os.path.join(RDIR, PROJECT + '.dsn')
SES = os.path.join(RDIR, PROJECT + '.ses')
PCB = os.path.join(ROOT, PROJECT + '.kicad_pcb')
VIA = 'Via[0-1]_600:300_um'
CLASSES = {  # name: (width um, clearance um, nets)
    'Power': (600, 200, ['+5V_SLOT', '5V_F', '5V_FAN', '5V_PANEL', 'FAN_GND_SW', '5V_PI']),
    'Supply': (250, 150, ['GND', '+3V3']),
}
DEFAULT = (150, 150)


def um(v):
    return round(v * 1000.0, 1)


CLOCK_W = 0.25


def clock_path(pcb):
    """GPCLK0, routed first and by hand: J2 pin 7 straight to R12 (22 ohm at the source), then from R12 at 45
    degrees and straight along to the CPLD's pin 12, on the top layer over the bottom layer's ground. Returns
    [(net, [(x, y), ...])] in board millimetres."""
    pads = {}
    for fp in sexpr.find_all(pcb, 'footprint'):
        at = sexpr.find(fp, 'at')
        X, Y, ang = float(at[1]), float(at[2]), float(at[3]) if len(at) > 3 else 0.0
        ref = [c[2] for c in sexpr.find_all(fp, 'property') if c[1] == 'Reference'][0]
        for p in sexpr.find_all(fp, 'pad'):
            pat = sexpr.find(p, 'at')
            pads[(ref, str(p[1]))] = geom.to_board(X, Y, ang, float(pat[1]), float(pat[2]))
    j, r1, r2, u = pads[('J2', '7')], pads[('R12', '1')], pads[('R12', '2')], pads[('U1', '12')]
    dy = r2[1] - u[1]
    knee = (r2[0] + dy, u[1])
    return [('PI_CLK_GPIO', [j, r1]), ('PI_CLK', [r2, knee, u])]


def clock_wiring(pcb):
    out = []
    for net, pts in clock_path(pcb):
        out.append(S('wire', S('path', Sym('F.Cu'), um(CLOCK_W), *[v for x, y in pts for v in (um(x), -um(y))]),
                     S('net', net), S('type', Sym('fix'))))
    return out


def board_wiring(pcb):
    """The board's present tracks and vias as Specctra wiring, for a second run that continues from the first."""
    netname = {x[1]: x[2] for x in sexpr.find_all(pcb, 'net')}
    clock = {n for n, _ in clock_path(pcb)}
    out = []
    for sg in sexpr.find_all(pcb, 'segment'):
        n = netname[sexpr.find(sg, 'net')[1]]
        if n in clock:
            continue
        a, b = sexpr.find(sg, 'start'), sexpr.find(sg, 'end')
        out.append(S('wire', S('path', Sym(sexpr.find(sg, 'layer')[1]), um(sexpr.find(sg, 'width')[1]), um(a[1]), -um(a[2]),
                                um(b[1]), -um(b[2])), S('net', n), S('type', Sym('route'))))
    for v in sexpr.find_all(pcb, 'via'):
        n = netname[sexpr.find(v, 'net')[1]]
        at = sexpr.find(v, 'at')
        out.append(S('via', VIA, um(at[1]), -um(at[2]), S('net', n), S('type', Sym('route'))))
    return out


def dsn_key(text):
    return hashlib.sha1(text[:text.index('(wiring')].encode()).hexdigest()


def export(continue_from_board=False):
    pcb = sexpr.load(PCB)
    nets = {}
    images, places, padstacks, keepouts = [], [], {}, []
    for fp in sexpr.find_all(pcb, 'footprint'):
        at = sexpr.find(fp, 'at')
        X, Y, ang = float(at[1]), float(at[2]), float(at[3]) if len(at) > 3 else 0.0
        ref = [c[2] for c in sexpr.find_all(fp, 'property') if c[1] == 'Reference'][0]
        img = S('image', 'IMG_' + ref)
        seen = {}
        for p in sexpr.find_all(fp, 'pad'):
            pat = sexpr.find(p, 'at')
            px, py = geom.rot(float(pat[1]), float(pat[2]), ang)
            size = sexpr.find(p, 'size')
            w, h = float(size[1]), float(size[2])
            pa = (float(pat[3]) if len(pat) > 3 else 0.0) % 180
            if abs(pa - 90) < 1:
                w, h = h, w
            kind, shape = str(p[2]), str(p[3])
            if kind == 'np_thru_hole':
                d = float(sexpr.find(p, 'drill')[1])
                keepouts.append(S('keepout', '', S('circle', Sym('signal'), um(d + 0.4), um(X + px), -um(Y + py))))
                continue
            layers = [str(l) for l in sexpr.find(p, 'layers')[1:]]
            tht = kind == 'thru_hole'
            if shape == 'circle' or (shape == 'oval' and abs(w - h) < 1e-6):
                name = 'Round[%s]Pad_%d_um' % ('A' if tht else 'T', um(w))
                shp = [S('shape', S('circle', Sym(l), um(w))) for l in (('F.Cu', 'B.Cu') if tht else ('F.Cu',))]
            else:
                name = 'Rect[%s]Pad_%dx%d_um' % ('A' if tht else 'T', um(w), um(h))
                shp = [S('shape', S('rect', Sym(l), -um(w / 2), -um(h / 2), um(w / 2), um(h / 2)))
                       for l in (('F.Cu', 'B.Cu') if tht else ('F.Cu',))]
            padstacks[name] = S('padstack', name, *shp, S('attach', Sym('off')))
            num = str(p[1])
            k = seen.get(num, 0)
            seen[num] = k + 1
            pid = num if k == 0 else '%s@%d' % (num, k)
            img.append(S('pin', name, pid, um(px), -um(py)))
            n = sexpr.find(p, 'net')
            if n:
                nets.setdefault(n[2], []).append('%s-%s' % (ref, pid))
        images.append(img)
        places.append(S('component', 'IMG_' + ref, S('place', ref, um(X), -um(Y), Sym('front'), 0)))
    padstacks[VIA] = S('padstack', VIA, S('shape', S('circle', Sym('F.Cu'), 600)), S('shape', S('circle', Sym('B.Cu'), 600)),
                       S('attach', Sym('off')))
    edge = []
    for g in sexpr.find_all(pcb, 'gr_line'):
        if sexpr.find(g, 'layer')[1] == 'Edge.Cuts':
            a = sexpr.find(g, 'start')
            edge.append((a[1], a[2]))
    edge.append(edge[0])
    boundary = S('boundary', S('path', Sym('pcb'), 0, *[v for x, y in edge for v in (um(x), -um(y))]))
    structure = S('structure', S('layer', Sym('F.Cu'), S('type', Sym('signal')), S('property', S('index', 0))),
                  S('layer', Sym('B.Cu'), S('type', Sym('signal')), S('property', S('index', 1))), boundary)
    structure += keepouts
    # no vias over the Pi 3B/3B+'s board and ports (their sockets' shells could press on the card there)
    for (x0, y0, x1, y1, name, what) in D.keepouts():
        if name == 'PI3B_PORTS':
            X0, Y0, X1, Y1 = x0 + OX, y0 + OY, x1 + OX, y1 + OY
            structure.append(S('via_keepout', '', S('polygon', Sym('signal'), 0, um(X0), -um(Y0), um(X1), -um(Y0),
                                                    um(X1), -um(Y1), um(X0), -um(Y1), um(X0), -um(Y0))))
    structure.append(S('via', VIA))
    # (Freerouting 2.2.4 rejects an autoroute_settings scope here, so the top layer cannot be preferred from the DSN:
    # with it the board read as empty. The bottom layer's use is reported by validate.py instead.)
    structure.append(S('rule', S('width', DEFAULT[0]), S('clearance', DEFAULT[1]), S('clearance', 100, S('type', Sym('smd_smd')))))
    network = S('network')
    for n in sorted(nets):
        if len(nets[n]) < 2:
            continue
        network.append(S('net', n, S('pins', *[Sym(x) for x in nets[n]])))
    inclass = set()
    for cname, (w, c, ns) in CLASSES.items():
        members = [n for n in ns if n in nets]
        inclass |= set(members)
        network.append(S('class', cname, *members, S('circuit', S('use_via', VIA)), S('rule', S('width', w), S('clearance', c))))
    rest = [n for n in sorted(nets) if n not in inclass and len(nets[n]) >= 2]
    network.append(S('class', 'kicad_default', '', *rest, S('circuit', S('use_via', VIA)),
                     S('rule', S('width', DEFAULT[0]), S('clearance', DEFAULT[1]))))
    lib = S('library', *images, *padstacks.values())
    dsn = S('pcb', PROJECT + '.dsn',
            S('parser', S('string_quote', Sym('"')), S('space_in_quoted_tokens', Sym('on')), S('host_cad', "Aurora68 gen_kicad"),
              S('host_version', '1')),
            S('resolution', Sym('um'), 10), S('unit', Sym('um')), structure, S('placement', *places), lib, network,
            S('wiring', *(clock_wiring(pcb) + (board_wiring(pcb) if continue_from_board else []))))
    os.makedirs(RDIR, exist_ok=True)
    text = sexpr.dump(dsn) + '\n'
    with open(DSN, 'w') as f:
        f.write(text)
    print('wrote', DSN, '(%d nets)' % len(network))
    return dsn_key(text)


def import_ses():
    with open(DSN) as f:
        dsn_text = f.read()
    dsn_hash = dsn_key(dsn_text)
    new_hash = export()
    with open(DSN, 'w') as f:                # keep the DSN the session was made from
        f.write(dsn_text)
    if new_hash != dsn_hash:
        raise SystemExit('the placement changed since the SES was made: route again (route.py run ...)')
    ses = sexpr.load(SES)
    routes = sexpr.find(ses, 'routes')
    res = sexpr.find(routes, 'resolution')
    scale = float(res[2]) * 1000.0           # SES units per mm
    pcb = sexpr.load(PCB)
    netno = {s[2]: s[1] for s in sexpr.find_all(pcb, 'net')}
    pcb[:] = [c for c in pcb if not (isinstance(c, list) and c and c[0] in ('segment', 'via'))]
    nseg = nvia = 0
    for net in sexpr.find_all(sexpr.find(routes, 'network_out'), 'net'):
        name = str(net[1])
        n = netno[name]
        for w in sexpr.find_all(net, 'wire'):
            path = sexpr.find(w, 'path')
            layer, width = str(path[1]), float(path[2]) / scale
            coords = [float(v) for v in path[3:] if isinstance(v, (int, float))]
            pts = [(coords[i] / scale, -coords[i + 1] / scale) for i in range(0, len(coords) - 1, 2)]
            for a, b in zip(pts, pts[1:]):
                if a == b:
                    continue
                pcb.append(S('segment', S('start', round(a[0], 4), round(a[1], 4)), S('end', round(b[0], 4), round(b[1], 4)),
                             S('width', round(width, 4)), S('layer', layer), S('net', n), S('uuid', uid('seg', name, a, b, layer))))
                nseg += 1
        for v in sexpr.find_all(net, 'via'):
            x, y = float(v[2]) / scale, -float(v[3]) / scale
            pcb.append(S('via', S('at', round(x, 4), round(y, 4)), S('size', 0.6), S('drill', 0.3), S('layers', 'F.Cu', 'B.Cu'),
                         S('net', n), S('uuid', uid('via', name, x, y))))
            nvia += 1
    # the hand-routed clock: Freerouting keeps fixed wiring but does not write it into its session
    for name, pts in clock_path(pcb):
        n = netno[name]
        for a, b in zip(pts, pts[1:]):
            pcb.append(S('segment', S('start', round(a[0], 4), round(a[1], 4)), S('end', round(b[0], 4), round(b[1], 4)),
                         S('width', CLOCK_W), S('layer', 'F.Cu'), S('net', n), S('uuid', uid('clockseg', name, a, b))))
            nseg += 1
    sexpr.save(PCB, pcb)
    print('imported %d segments and %d vias into %s' % (nseg, nvia, PCB))


def run(jar, java='java', passes=40):
    export()
    cmd = [java, '-jar', jar, '--gui.enabled=false', '-de', DSN, '-do', SES, '-mp', str(passes)]
    print(' '.join(cmd))
    subprocess.run(cmd, check=True)
    import_ses()


if __name__ == '__main__':
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    if sys.argv[1] == 'export':
        export('--continue' in sys.argv)
    elif sys.argv[1] == 'import':
        import_ses()
    elif sys.argv[1] == 'run':
        run(sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else 'java')
    else:
        raise SystemExit(__doc__)
