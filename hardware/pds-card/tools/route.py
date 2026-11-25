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
VIA = 'Via[0-3]_600:300_um'
CU = ('F.Cu', 'In1.Cu', 'In2.Cu', 'B.Cu')
CLASSES = {  # name: (width um, clearance um, nets)
    'Power': (600, 200, ['+5V_SLOT', '5V_F', '5V_PANEL', '5V_PI']),
    'Supply': (250, 150, ['GND', '+3V3', '5V_FAN']),
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
    def run(a, b):
        """a 45-degree leg then a straight one, from a to b"""
        dx, dy = b[0] - a[0], b[1] - a[1]
        d = min(abs(dx), abs(dy))
        knee = (a[0] + math.copysign(d, dx), a[1] + math.copysign(d, dy))
        return [a, knee, b] if 0.01 < math.hypot(knee[0] - b[0], knee[1] - b[1]) and d > 0.01 else [a, b]
    # R12 to the CPLD: 1 mm straight down first, so that the 45-degree leg passes clear of R1's lower pad (+3.3 V;
    # straight from R12 it clipped that pad's corner, found by validate.py's clearance check, 12k)
    r2d = (r2[0], r2[1] + 1.0)
    return [('PI_CLK_GPIO', run(j, r1)), ('PI_CLK', [r2] + run(r2d, u))]


FAN_W = 0.25


def plane_fanout(pcb):
    """A via beside every SMD pad of a plane's net (GND to L2, +3.3 V to L3), joined to it by a short 0.25 mm stub,
    placed before the router runs: the stitching the decoupling capacitors and the CPLD want, and fewer connections for
    the router. Each via goes outward from its footprint first (then the other sides, then further out), 0.2 mm clear
    of every other pad, hole and fan-out via, never in a pad. Returns [(net, (pad x, y), (via x, y))]."""
    items = copper_items(pcb)
    pads = [it for it in items if it[0] == 'pad']
    holes = [it for it in items if it[0] == 'hole']
    vias = []
    out = []
    centres = {}
    for fp in sexpr.find_all(pcb, 'footprint'):
        at = sexpr.find(fp, 'at')
        X, Y, ang = float(at[1]), float(at[2]), float(at[3]) if len(at) > 3 else 0.0
        for p in sexpr.find_all(fp, 'pad'):
            pat = sexpr.find(p, 'at')
            centres[tuple(round(v, 3) for v in geom.to_board(X, Y, ang, float(pat[1]), float(pat[2])))] = (X, Y)
    nogo = []
    for (x0, y0, x1, y1, name, what) in D.keepouts():
        if name in ('PI3B_PORTS', 'APPLE_SUPPORT'):
            nogo.append((x0 + OX, y0 + OY, x1 + OX, y1 + OY))
    R = 0.3
    clock = [(a, b) for n, pts in clock_path(pcb) for a, b in zip(pts, pts[1:])]     # the hand-routed GPCLK0
    def clear_of_clock(p, q):
        for a, b in clock:
            for t in (0.0, 0.25, 0.5, 0.75, 1.0):
                if pt_seg((p[0] + t * (q[0] - p[0]), p[1] + t * (q[1] - p[1])), a, b) < CLOCK_W / 2 + FAN_W / 2 + 0.2:
                    return False
            if pt_seg(q, a, b) < CLOCK_W / 2 + R + 0.2:
                return False
        return True
    for it in pads:
        net, cx, cy, hw, hh, lays = it[1], it[2], it[3], it[4], it[5], it[6]
        if net not in D.PLANES.values() or lays != ['F.Cu']:
            continue
        fx, fy = centres.get((round(cx, 3), round(cy, 3)), (cx, cy))
        dirs = [(1, 0), (-1, 0), (0, 1), (0, -1)]
        dirs.sort(key=lambda d: -(d[0] * (cx - fx) + d[1] * (cy - fy)))       # outward from the part first
        if hw > hh:                                                           # a long pad: off its ends first
            dirs.sort(key=lambda d: d[1] != 0)
        elif hh > hw:
            dirs.sort(key=lambda d: d[0] != 0)
        done = False
        for extra in (0.0, 0.4, 0.8, 1.2):
            for dx, dy in dirs:
                ext = hw if dx else hh
                vx, vy = cx + dx * (ext + R + 0.2 + extra), cy + dy * (ext + R + 0.2 + extra)
                ex, ey = cx + dx * ext, cy + dy * ext                       # the stub: pad edge to via
                ok = not any(b[0] - R <= vx <= b[2] + R and b[1] - R <= vy <= b[3] + R for b in nogo) and \
                    clear_of_clock((ex, ey), (vx, vy))
                for q in pads:
                    if not ok:
                        break
                    if q is it:
                        continue
                    if pt_rect((vx, vy), q[2], q[3], q[4], q[5]) < R + (0.05 if q[1] == net else 0.2):
                        ok = False
                    elif q[1] != net and min(pt_rect((ex + t * (vx - ex), ey + t * (vy - ey)), q[2], q[3], q[4], q[5])
                                             for t in (0.0, 0.25, 0.5, 0.75, 1.0)) < FAN_W / 2 + 0.15:
                        ok = False
                for h in holes:
                    if ok and math.hypot(vx - h[1], vy - h[2]) < h[3] + R + 0.3:
                        ok = False
                for v in vias:
                    if ok and math.hypot(vx - v[0], vy - v[1]) < 2 * R + 0.2:
                        ok = False
                if ok:
                    vias.append((vx, vy))
                    out.append((net, (cx, cy), (vx, vy)))
                    done = True
                    break
            if done:
                break
    return out


def bare_board(pcb):
    return [c for c in pcb if not (isinstance(c, list) and c and c[0] in ('segment', 'via'))]


def fanout_wiring(pcb):
    out = []
    for net, (px, py), (vx, vy) in plane_fanout(pcb):
        out.append(S('wire', S('path', Sym('F.Cu'), um(FAN_W), um(px), -um(py), um(vx), -um(vy)), S('net', net), S('type', Sym('fix'))))
        out.append(S('via', VIA, um(vx), -um(vy), S('net', net), S('type', Sym('fix'))))
    return out


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
                shp = [S('shape', S('circle', Sym(l), um(w))) for l in (CU if tht else ('F.Cu',))]
            else:
                name = 'Rect[%s]Pad_%dx%d_um' % ('A' if tht else 'T', um(w), um(h))
                shp = [S('shape', S('rect', Sym(l), -um(w / 2), -um(h / 2), um(w / 2), um(h / 2)))
                       for l in (CU if tht else ('F.Cu',))]
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
    padstacks[VIA] = S('padstack', VIA, *[S('shape', S('circle', Sym(l), 600)) for l in CU], S('attach', Sym('off')))
    edge = []
    for g in sexpr.find_all(pcb, 'gr_line'):
        if sexpr.find(g, 'layer')[1] == 'Edge.Cuts':
            a = sexpr.find(g, 'start')
            edge.append((a[1], a[2]))
    edge.append(edge[0])
    boundary = S('boundary', S('path', Sym('pcb'), 0, *[v for x, y in edge for v in (um(x), -um(y))]))
    structure = S('structure', *[S('layer', Sym(l), S('type', Sym('power' if l in D.PLANES else 'signal')),
                                   S('property', S('index', i))) for i, l in enumerate(CU)], boundary)
    # the inner layers are planes: L2 GND, L3 +3.3 V; the router reaches them with vias and routes nothing on them
    inset = [(x, y) for x, y in edge]
    for l, n in D.PLANES.items():
        structure.append(S('plane', n, S('polygon', Sym(l), 0, *[v for x, y in inset for v in (um(x), -um(y))])))
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
    clocks = [n for n in D.CLOCK_NETS if n in nets]          # L1 only, over L2's ground
    inclass |= set(clocks)
    network.append(S('class', 'Clock', *clocks, S('circuit', S('use_via', VIA), S('use_layer', Sym('F.Cu'))),
                     S('rule', S('width', 250), S('clearance', 200))))
    rest = [n for n in sorted(nets) if n not in inclass and len(nets[n]) >= 2]
    network.append(S('class', 'kicad_default', '', *rest, S('circuit', S('use_via', VIA)),
                     S('rule', S('width', DEFAULT[0]), S('clearance', DEFAULT[1]))))
    lib = S('library', *images, *padstacks.values())
    dsn = S('pcb', PROJECT + '.dsn',
            S('parser', S('string_quote', Sym('"')), S('space_in_quoted_tokens', Sym('on')), S('host_cad', "Aurora68 gen_kicad"),
              S('host_version', '1')),
            S('resolution', Sym('um'), 10), S('unit', Sym('um')), structure, S('placement', *places), lib, network,
            S('wiring', *(clock_wiring(pcb) + fanout_wiring(bare_board(pcb)) + (board_wiring(pcb) if continue_from_board else []))))
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
    fan = plane_fanout(pcb)                  # from the bare board, as export made it
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
    # the plane fan-out, also fixed wiring (made from the board before the router's copper was added)
    for name, (px, py), (vx, vy) in fan:
        n = netno[name]
        pcb.append(S('segment', S('start', round(px, 4), round(py, 4)), S('end', round(vx, 4), round(vy, 4)), S('width', FAN_W),
                     S('layer', 'F.Cu'), S('net', n), S('uuid', uid('fanseg', name, px, py))))
        pcb.append(S('via', S('at', round(vx, 4), round(vy, 4)), S('size', 0.6), S('drill', 0.3), S('layers', 'F.Cu', 'B.Cu'),
                     S('net', n), S('uuid', uid('fanvia', name, vx, vy))))
        nseg += 1; nvia += 1
    nst = stitch(pcb, netno['GND'])
    sexpr.save(PCB, pcb)
    print('imported %d segments and %d vias into %s; %d ground stitching vias added' % (nseg, nvia, PCB, nst))


STITCH_PITCH = 5.0


def copper_items(pcb):
    """Every piece of copper on the board in board millimetres: ('pad', net, cx, cy, hw, hh, layers),
    ('seg', net, a, b, half width, layer), ('via', net, x, y, r), ('hole', x, y, r)."""
    netname = {x[1]: x[2] for x in sexpr.find_all(pcb, 'net')}
    out = []
    for fp in sexpr.find_all(pcb, 'footprint'):
        at = sexpr.find(fp, 'at')
        X, Y, ang = float(at[1]), float(at[2]), float(at[3]) if len(at) > 3 else 0.0
        for p in sexpr.find_all(fp, 'pad'):
            pat = sexpr.find(p, 'at')
            x, y = geom.to_board(X, Y, ang, float(pat[1]), float(pat[2]))
            size = sexpr.find(p, 'size')
            w, h = float(size[1]), float(size[2])
            pa = (float(pat[3]) if len(pat) > 3 else 0.0) % 180
            if abs(pa - 90) < 1:
                w, h = h, w
            if str(p[2]) == 'np_thru_hole':
                out.append(('hole', x, y, float(sexpr.find(p, 'drill')[1]) / 2))
                continue
            n = sexpr.find(p, 'net')
            lays = ['F.Cu'] if str(p[2]) == 'smd' else list(CU)
            rnd = str(p[3]) == 'circle' or (str(p[3]) == 'oval' and abs(w - h) < 1e-6)
            out.append(('pad', n[2] if n else '', x, y, w / 2, h / 2, lays, rnd))
    for sg in sexpr.find_all(pcb, 'segment'):
        a, b = sexpr.find(sg, 'start'), sexpr.find(sg, 'end')
        out.append(('seg', netname[sexpr.find(sg, 'net')[1]], (a[1], a[2]), (b[1], b[2]), sexpr.find(sg, 'width')[1] / 2,
                    sexpr.find(sg, 'layer')[1]))
    for v in sexpr.find_all(pcb, 'via'):
        at = sexpr.find(v, 'at')
        out.append(('via', netname[sexpr.find(v, 'net')[1]], at[1], at[2], sexpr.find(v, 'size')[1] / 2))
    return out


def pt_seg(p, a, b):
    dx, dy = b[0] - a[0], b[1] - a[1]
    L2 = dx * dx + dy * dy
    t = 0.0 if L2 == 0 else max(0.0, min(1.0, ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / L2))
    return math.hypot(p[0] - a[0] - t * dx, p[1] - a[1] - t * dy)


def pt_rect(p, cx, cy, hw, hh):
    dx, dy = max(abs(p[0] - cx) - hw, 0.0), max(abs(p[1] - cy) - hh, 0.0)
    return math.hypot(dx, dy)


def stitch(pcb, gndno):
    """Ground stitching vias on a STITCH_PITCH grid wherever one fits: clear of all other copper by the
    clearance, of every footprint's courtyard (no vias under parts), of the board's edge, of the holes, and out of
    the via keep-out over the Pi 3B's ports. They tie the outer ground pours to L2."""
    items = copper_items(pcb)
    crts = []
    for fp in sexpr.find_all(pcb, 'footprint'):
        at = sexpr.find(fp, 'at')
        X, Y, ang = float(at[1]), float(at[2]), float(at[3]) if len(at) > 3 else 0.0
        lay = sexpr.find(fp, 'layer')[1]
        c = geom.fp_courtyard(fp, 'B.CrtYd' if lay == 'B.Cu' else 'F.CrtYd')
        if c:
            crts.append(geom.box_to_board(c, X, Y, ang))
    ko = []
    for (x0, y0, x1, y1, name, what) in D.keepouts():
        if name in ('PI3B_PORTS', 'APPLE_SUPPORT'):
            ko.append((x0 + OX, y0 + OY, x1 + OX, y1 + OY))
    outline = [(x + OX, y + OY) for x, y in D.card_outline()]
    def inside(p):
        c = False
        for (x1, y1), (x2, y2) in zip(outline, outline[1:] + outline[:1]):
            if (y1 > p[1]) != (y2 > p[1]) and p[0] < x1 + (p[1] - y1) * (x2 - x1) / (y2 - y1):
                c = not c
        return c and min(pt_seg(p, a, b) for a, b in zip(outline, outline[1:] + outline[:1])) > 1.5
    R, CLR = 0.3, 0.2
    added = 0
    xs = [x for x, y in outline]; ys = [y for x, y in outline]
    y = min(ys) + 2.5
    while y < max(ys):
        x = min(xs) + 2.5
        while x < max(xs):
            p = (x, y)
            ok = inside(p) and not any(b[0] - 0.3 <= x <= b[2] + 0.3 and b[1] - 0.3 <= y <= b[3] + 0.3 for b in crts + ko)
            if ok:
                for it in items:
                    if it[0] == 'pad':
                        d = pt_rect(p, it[2], it[3], it[4], it[5]) - R
                    elif it[0] == 'seg':
                        d = pt_seg(p, it[2], it[3]) - it[4] - R
                    elif it[0] == 'via':
                        d = math.hypot(p[0] - it[2], p[1] - it[3]) - it[4] - R
                    else:
                        d = math.hypot(p[0] - it[1], p[1] - it[2]) - it[3] - R - 0.3
                    if d < CLR + 0.05:
                        ok = False
                        break
            if ok:
                pcb.append(S('via', S('at', round(x, 4), round(y, 4)), S('size', 0.6), S('drill', 0.3),
                             S('layers', 'F.Cu', 'B.Cu'), S('net', gndno), S('uuid', uid('stitch', x, y))))
                items.append(('via', 'GND', x, y, R))
                added += 1
            x += STITCH_PITCH
        y += STITCH_PITCH
    return added


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
