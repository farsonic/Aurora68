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
from gen_kicad import PROJECT, uid

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


def export():
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
    structure.append(S('via', VIA))
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
            S('resolution', Sym('um'), 10), S('unit', Sym('um')), structure, S('placement', *places), lib, network, S('wiring'))
    os.makedirs(RDIR, exist_ok=True)
    text = sexpr.dump(dsn) + '\n'
    with open(DSN, 'w') as f:
        f.write(text)
    print('wrote', DSN, '(%d nets)' % len(network))
    return hashlib.sha1(text.encode()).hexdigest()


def import_ses():
    with open(DSN) as f:
        dsn_hash = hashlib.sha1(f.read().encode()).hexdigest()
    new_hash = export()
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
        export()
    elif sys.argv[1] == 'import':
        import_ses()
    elif sys.argv[1] == 'run':
        run(sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else 'java')
    else:
        raise SystemExit(__doc__)
