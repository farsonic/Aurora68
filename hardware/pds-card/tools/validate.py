# SPDX-License-Identifier: MPL-2.0
"""validate.py - check Card P's KiCad files without KiCad. Not a substitute for KiCad's ERC and DRC.

Every check reads the written files back (with kiutils where it can parse them, and with tools/sexpr.py and
tools/netlist.py for the rest), and compares them with design.py, se-pds-pinout.csv, the RTL's pin file and
Apple's numbers:
   1. every file parses (kiutils: symbol library, each footprint, each schematic sheet, the board);
   2. the slot plug, pin by pin (tools/check_pinout.py);
   3. schematic and board agree: every pad's net is its pin's net;
   4. every net has at least two connections;
   5. no output-to-output conflicts (push-pull outputs or power outputs sharing a net), and on the slot's nets the
      card has only three-state, open-drain, switch (CBTD) or input pins;
   6. the CPLD: I/O count against the EPM240 (80) and EPM570 (76), no I/O on the EPM570's power pins, every port of
      rtl/aurora68_pds.v on the pin rtl/aurora68_pds.qsf gives it, with the net the schematic gives that pin;
   7. every IC's power pins connected, and a 100 nF (or larger) capacitor from each of its supply nets to GND
      within 12 mm on the board;
   8. outline, holes, plug and keep-outs against Apple's Figure 17-1;
   9. component heights against Figure 17-2's slope; the Pi on its socket included;
  10. keep-outs: no courtyard in the Pi 3B/3B+ USB and Ethernet area, nothing but the socket and standoffs under the
      Pi 3A+, nothing on Apple's card supports;
  11. the slot's signals reach a buffer or switch within 50 mm of the plug;
  12. the board design rules (2 layers, track and via sizes) and, if routed, how many connections are routed.
Prints a report; exit status 1 if any check fails. Warnings (W) are listed but do not fail.
"""

import csv
import math
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sexpr
import design as D
import geom
import netlist
from gen_kicad import OX, OY, PROJECT

ROOT = D.ROOT
RESULTS = []


def check(name, ok, detail=''):
    RESULTS.append(('PASS' if ok else 'FAIL', name, detail))
    return ok


def warn(name, detail):
    RESULTS.append(('W', name, detail))


def info(name, detail):
    RESULTS.append(('INFO', name, detail))


# 1 -------------------------------------------------------------------------------------------------------
def check_parse():
    try:
        import kiutils
        import importlib.metadata
        kiutils.__version__ = importlib.metadata.version('kiutils')
        from kiutils.symbol import SymbolLib
        from kiutils.footprint import Footprint
        from kiutils.schematic import Schematic
        from kiutils.board import Board
    except Exception as e:
        warn('kiutils', 'not installed (%s): python3 -m pip install --user kiutils; parsing with tools/sexpr.py only' % e)
        kiutils = None
    files = [os.path.join(ROOT, 'lib', 'aurora68.kicad_sym')]
    files += sorted(os.path.join(ROOT, 'lib', 'aurora68.pretty', f) for f in os.listdir(os.path.join(ROOT, 'lib', 'aurora68.pretty')))
    files += [os.path.join(ROOT, f) for f in netlist.sheet_files()]
    files.append(os.path.join(ROOT, PROJECT + '.kicad_pcb'))
    bad = []
    for f in files:
        try:
            sexpr.load(f)
        except Exception as e:
            bad.append('%s: %s' % (os.path.basename(f), e))
            continue
        if kiutils is None:
            continue
        try:
            if f.endswith('.kicad_sym'):
                lib = SymbolLib.from_file(f); n = len(lib.symbols)
            elif f.endswith('.kicad_mod'):
                Footprint.from_file(f)
            elif f.endswith('.kicad_sch'):
                Schematic.from_file(f)
            elif f.endswith('.kicad_pcb'):
                b = Board.from_file(f)
                info('kiutils board', '%d footprints, %d nets, %d graphic items, %d zones, %d tracks/vias' % (
                    len(b.footprints), len(b.nets), len(b.graphicItems), len(b.zones), len(b.traceItems)))
        except Exception as e:
            bad.append('%s (kiutils): %s: %s' % (os.path.basename(f), type(e).__name__, e))
    check('1. every file parses (sexpr%s)' % (' and kiutils %s' % getattr(kiutils, '__version__', '') if kiutils else ''),
          not bad, '%d files' % len(files) + ('; ' + '; '.join(bad[:6]) if bad else ''))


# 3, 4, 5 ---------------------------------------------------------------------------------------------------
def check_nets(pins, nets, pads):
    mism = []
    for (ref, num), (net, name, typ, fname) in pins.items():
        if ref.startswith('#'):
            continue
        pad = pads.get((ref, num))
        if pad is None:
            mism.append('%s.%s has no pad' % (ref, num))
            continue
        for (pnet, x, y) in pad:
            if (pnet or None) != (net or None):
                mism.append('%s.%s: schematic %s, board %s' % (ref, num, net, pnet))
    for (ref, num) in pads:
        if (ref, num) not in pins and not ref.startswith('H') and num != 'MP':
            mism.append('pad %s.%s has no schematic pin' % (ref, num))
    check('3. schematic and board nets agree, pad by pad', not mism, '%d pins; %s' % (len(pins), '; '.join(mism[:8])))
    single = [n for n, ps in nets.items() if len([p for p in ps if not p[0].startswith('#')]) < 2]
    check('4. every net has at least two connections', not single, '%d nets; single: %s' % (len(nets), ', '.join(single[:10])))
    conflicts = []
    slotbad = []
    for n, ps in nets.items():
        types = [(r, p, pins[(r, p)][2]) for r, p in ps]
        outs = [t for t in types if t[2] == 'output']
        pouts = [t for t in types if t[2] == 'power_out' and not t[0].startswith('#')]
        if len(outs) > 1 or len(pouts) > 1 or (outs and pouts):
            conflicts.append('%s: %s' % (n, ', '.join('%s.%s(%s)' % t for t in outs + pouts)))
        if n.startswith('SE_'):
            for r, p, t in types:
                if r == 'J1':
                    continue
                if t not in ('tri_state', 'open_collector', 'open_emitter', 'passive', 'input', 'bidirectional'):
                    slotbad.append('%s.%s %s on %s' % (r, p, t, n))
                if t == 'bidirectional' and not r.startswith('U'):
                    slotbad.append('%s.%s bidirectional on %s' % (r, p, n))
    check('5a. no output-to-output conflicts (push-pull or power outputs sharing a net)', not conflicts, '; '.join(conflicts[:6]))
    check('5b. the card meets the slot only through three-state latch outputs, the CBTD switches or inputs', not slotbad,
          '; '.join(slotbad[:6]))


# 6 -------------------------------------------------------------------------------------------------------
def check_cpld(pins):
    used = [(num, net) for (ref, num), (net, name, typ, f) in pins.items() if ref == 'U1' and net]
    io = [(num, net) for num, net in used if not (net in ('+3V3', 'GND') or net.startswith('JTAG_'))]
    on_power = [num for num, net in io if int(num) in (37, 39, 88, 90)]
    check('6a. CPLD I/O count: %d used of 80 (EPM240) / 76 (EPM570)' % len(io), len(io) <= 76 and not on_power,
          'none on pins 37/39/88/90' if not on_power else 'I/O on %s' % on_power)
    qsf = {}
    for line in open(os.path.join(ROOT, 'rtl', 'aurora68_pds.qsf')):
        m = re.match(r'set_location_assignment PIN_(\d+) -to (\S+)', line)
        if m:
            qsf[m.group(2)] = int(m.group(1))
    src = open(os.path.join(ROOT, 'rtl', 'aurora68_pds.v')).read()
    body = src[src.index('module aurora68_pds'):src.index(');', src.index('module aurora68_pds'))]
    ports = []
    for m in re.finditer(r'^\s*(input|output|inout)\s+(?:reg\s+)?(?:\[(\d+):(\d+)\]\s+)?(\w+)', body, re.M):
        name = m.group(4)
        if m.group(2):
            hi, lo = int(m.group(2)), int(m.group(3))
            ports += ['%s[%d]' % (name, i) for i in range(lo, hi + 1)]
        else:
            ports.append(name)
    missing = [p for p in ports if p not in qsf]
    extra = [p for p in qsf if p not in ports]
    wrong = []
    for p, pin in qsf.items():
        net = pins.get(('U1', str(pin)), (None,))[0]
        if net != D.port_net(p):
            wrong.append('%s on pin %d: schematic net %s' % (p, pin, net))
    check('6b. RTL ports = qsf pins = schematic nets (%d ports)' % len(ports), not missing and not extra and not wrong,
          '; '.join(['missing ' + ', '.join(missing)] * bool(missing) + ['extra ' + ', '.join(extra)] * bool(extra) + wrong[:5]))
    info('6c. CPLD logic size', 'yosys 0.71 (generic 4-input LUT mapping, 24 Nov 2026): 154 LUTs, 122 flip-flops; '
         'a MAX II logic element is one LUT and one flip-flop, so 154 to about 230 LEs of the EPM240\'s 240. '
         'Quartus has the last word (rtl/README.md)')


# 7 -------------------------------------------------------------------------------------------------------
def check_power(parts, pins, pads):
    from gen_kicad import symbol_pins
    bad, nodec = [], []
    caps = [p for p in parts if p.sym == 'C']
    def cap_ok(ic, net):
        xs = [xy for (r, n), v in pads.items() if r == ic.ref for xy in v]
        best = 1e9
        for c in caps:
            if set(c.nets.values()) != {net, 'GND'}:
                continue
            cx, cy = c.x + OX, c.y + OY
            for (pn, x, y) in xs:
                if pn == net:
                    best = min(best, math.hypot(x - cx, y - cy))
        return best
    for p in parts:
        if not (p.ref.startswith('U')):
            continue
        sp = symbol_pins(p.sym)
        pw = [(n, nm) for n, nm, t in sp if t in ('power_in',)]
        for n, nm in pw:
            if not p.nets.get(n):
                bad.append('%s pin %s (%s) not connected' % (p.ref, n, nm))
        supplies = sorted({p.nets[n] for n, nm in pw if p.nets.get(n) and p.nets[n] != 'GND'})
        if p.sym in ('AMS1117-3.3', 'LM66100DCK'):
            supplies = sorted(set(p.nets[n] for n, nm, t in sp if t in ('power_in', 'power_out') and p.nets.get(n) not in (None, 'GND')))
        for s in supplies:
            d = cap_ok(p, s)
            if d > 12.0:
                nodec.append('%s on %s: nearest capacitor %.1f mm' % (p.ref, s, d))
    check('7a. every IC\'s power pins are connected', not bad, '; '.join(bad[:6]))
    check('7b. every IC has a capacitor from each supply to GND within 12 mm', not nodec, '; '.join(nodec[:6]))


# 8 -------------------------------------------------------------------------------------------------------
def check_outline(pcb, pads, fps):
    edges = [s for s in sexpr.find_all(pcb, 'gr_line') if sexpr.find(s, 'layer')[1] == 'Edge.Cuts']
    pts = []
    for e in edges:
        for k in ('start', 'end'):
            v = sexpr.find(e, k)
            pts.append((v[1] - OX, v[2] - OY))
    xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
    inside = min(xs) >= -1e-6 and max(xs) <= D.APPLE_W + 1e-6 and min(ys) >= -1e-6 and max(ys) <= D.APPLE_H + 1e-6
    # nothing below the 110.11 line except the tab (x >= 195.29)
    tab_ok = all(not (y > D.APPLE_BODY_H + 1e-6 and x < D.APPLE_TAB_X - 1e-6) for x, y in pts)
    check('8a. the outline is inside Apple\'s maximum (213.25 x 120.42, body 110.11, tab from x = 195.29)', inside and tab_ok,
          'board %.2f x %.2f mm (x %.2f-%.2f, y %.2f-%.2f)' % (max(xs) - min(xs), max(ys) - min(ys), min(xs), max(xs), min(ys), max(ys)))
    closed = len(edges) == len(D.card_outline())
    ch = [math.hypot(b[0] - a[0], b[1] - a[1]) for a, b in zip(D.card_outline(), D.card_outline()[1:] + D.card_outline()[:1])]
    chamfers = [l for l in ch if abs(l - D.APPLE_CHAMFER * math.sqrt(2)) < 0.01]
    check('8b. outline closed, five 2.0 mm x 45 degree chamfers', closed and len(chamfers) == 5, '%d edges, %d chamfers' % (len(edges), len(chamfers)))
    thick = sexpr.find(sexpr.find(pcb, 'general'), 'thickness')[1]
    check('8c. board thickness %.2f mm (Apple: 1.60)' % thick, abs(thick - 1.60) < 1e-6)
    holes = []
    for ref, X, Y, ang, side, fp in fps:
        for p in sexpr.find_all(fp, 'pad'):
            if p[2] == 'np_thru_hole':
                at = sexpr.find(p, 'at')
                x, y = geom.to_board(X, Y, ang, at[1], at[2])
                holes.append((ref, x - OX, y - OY, sexpr.find(p, 'drill')[1]))
    ok = True; det = []
    for (ax, ay) in D.APPLE_HOLES:
        m = [h for h in holes if abs(h[1] - ax) < 0.01 and abs(h[2] - ay) < 0.01 and abs(h[3] - D.APPLE_HOLE_D) < 0.001]
        ok &= bool(m)
        det.append('(%.2f, %.2f) %s' % (ax, ay, 'found' if m else 'MISSING'))
    for (ax, ay) in D.PLUG_FIX_HOLES:
        m = [h for h in holes if abs(h[1] - ax) < 0.01 and abs(h[2] - ay) < 0.01]
        ok &= bool(m)
    check('8d. Apple\'s two 3.86 mm card-support holes at x = 3.43, 50.17 apart; the plug\'s 2.85 mm fixing holes 90.0 apart', ok, '; '.join(det))
    strip = [ref for ref, X, Y, ang, side, fp in fps if (lambda c: c and geom.box_to_board(c, X, Y, ang)[1] - OY < D.APPLE_REAR_STRIP)(
        geom.fp_courtyard(fp, 'B.CrtYd' if side == 'B' else 'F.CrtYd'))]
    check('8e. nothing in Apple\'s 16.00 mm rear strip (the card starts at %.1f mm)' % D.CARD_REAR_Y, not strip, ', '.join(strip))


# 9 -------------------------------------------------------------------------------------------------------
def check_heights(parts, fps):
    rows, fails, warns = [], [], []
    hmap = {p.ref: p.height for p in parts}
    for ref, X, Y, ang, side, fp in fps:
        if side == 'B':
            h = hmap.get(ref, 0.0)       # the plug's press-fit tails on the top side
            crt = geom.fp_courtyard(fp, 'B.CrtYd')
        else:
            h = hmap.get(ref, 0.0)
            crt = geom.fp_courtyard(fp, 'F.CrtYd')
        if not crt or h <= 0:
            continue
        bx = geom.box_to_board(crt, X, Y, ang)
        y_rear = max(D.CARD_REAR_Y, bx[1] - OY)
        lim = D.height_limit(y_rear)
        rows.append((ref, h, lim, y_rear))
        if h > lim + 1e-9:
            fails.append('%s %.2f mm at y = %.1f, limit %.2f (over by %.2f)' % (ref, h, y_rear, lim, h - lim))
    # the Pi on its socket: its back, its underside's parts, its own parts hanging towards the card
    pi_rear = D.PI_Y0
    lim = D.height_limit(pi_rear)
    rows.append(('Pi 3A+ back', D.PI_TOP, lim, pi_rear))
    if D.PI_TOP > lim + 1e-9:
        fails.append('the Pi\'s back %.2f mm at its rear edge y = %.2f, limit %.2f' % (D.PI_TOP, pi_rear, lim))
    for name, u0, u1, v0, v1, hh in D.PI_UNDERSIDE:
        ys = [D.pi_xy(0, v)[1] for v in (v0, v1)]
        top = D.PI_TOP + hh
        l2 = D.height_limit(min(ys))
        rows.append(('Pi ' + name, top, l2, min(ys)))
        if top > l2 + 1e-9:
            fails.append('Pi %s %.2f at y = %.1f, limit %.2f' % (name, top, min(ys), l2))
    hang = []
    for name, u0, u1, v0, v1, hh in D.PI_TOP_PARTS:
        clearance = D.PI_GAP - hh
        hang.append('%s %.1f (%.1f mm clear)' % (name, hh, clearance))
        if name not in ('header',) and clearance < 0.4:
            fails.append('the Pi\'s %s (%.1f mm) leaves %.2f mm above the card (needs 0.4)' % (name, hh, clearance))
    # the Pi and every part of it that stands out past its board's edge stay over the card
    us = [u for part in D.PI_TOP_PARTS + D.PI_UNDERSIDE for u in part[1:3]] + [0.0, D.PI_W]
    vs = [v for part in D.PI_TOP_PARTS + D.PI_UNDERSIDE for v in part[3:5]] + [0.0, D.PI_D]
    corners = [D.pi_xy(u, v) for u in (min(us), max(us)) for v in (min(vs), max(vs))]
    px = [c[0] for c in corners]; py = [c[1] for c in corners]
    inside = min(px) >= 0 and max(px) <= D.APPLE_W and min(py) >= D.CARD_REAR_Y and max(py) <= D.APPLE_BODY_H
    check('9d. the Pi 3A+, its overhanging sockets and its microSD card stay over the card', inside,
          'x %.1f-%.1f, y %.1f-%.1f (card body: x 0-213.25, y %.1f-110.11)' % (min(px), max(px), min(py), max(py), D.CARD_REAR_Y))
    margin = min(r[2] - r[1] for r in rows)
    check('9a. heights within Figure 17-2 (4.61 mm at the rear edge rising to 15.58 at the front), %d items' % len(rows), not fails,
          'smallest margin %.2f mm; ' % margin + '; '.join(fails[:6]))
    info('9b. the Pi 3A+ turned components-down: gap %.1f mm (socket %.1f + its header body %.1f), board %.1f, back at %.1f mm' % (
        D.PI_GAP, D.SOCKET_H, D.PI_HEADER_BODY, D.PI_PCB_T, D.PI_TOP), 'its parts towards the card: ' + ', '.join(hang))
    # the 3B / 3B+ in the same place: their USB and RJ45 point at the card
    for name, u0, u1, v0, v1, hh in D.PI3B_EXT:
        over = hh - D.PI_GAP
        warn('9c. %s in this mounting' % name, '%.1f mm tall below the Pi\'s board, the gap is %.1f mm: it would hit the card by %.1f mm, '
             'so a 3B/3B+ does not fit this card in the SE (bench use on a %.0f mm riser only; README)' % (hh, D.PI_GAP, over, math.ceil(over + 0.5)))
    return rows


# 10 ------------------------------------------------------------------------------------------------------
def check_keepouts(fps):
    areas = D.keepouts()
    crts = {}
    for ref, X, Y, ang, side, fp in fps:
        if side != 'F':
            continue
        c = geom.fp_courtyard(fp, 'F.CrtYd')
        if c:
            bx = geom.box_to_board(c, X, Y, ang)
            crts[ref] = (bx[0] - OX, bx[1] - OY, bx[2] - OX, bx[3] - OY)
    hits3b, hitsunder, hitsapple = [], [], []
    for (x0, y0, x1, y1, name, what) in areas:
        for ref, b in crts.items():
            if not geom.boxes_overlap(b, (x0, y0, x1, y1)):
                continue
            if name == 'PI3B_EXT':
                hits3b.append(ref)
            elif name == 'PI_3AP_UNDER' and not (ref == 'J2' or ref in ('H3', 'H4', 'H5', 'H6', 'H7', 'H8')):
                hitsunder.append(ref)
            elif name == 'APPLE_SUPPORT' and ref not in ('H1', 'H2'):
                hitsapple.append(ref)
    k3 = [a for a in areas if a[4] == 'PI3B_EXT'][0]
    check('10a. no courtyard in the Pi 3B/3B+ USB/Ethernet keep-out (x %.1f-%.1f, y %.1f-%.1f)' % (k3[0], k3[2], k3[1], k3[3]),
          not hits3b, ', '.join(hits3b))
    check('10b. nothing but the socket and standoffs under the Pi 3A+', not hitsunder, ', '.join(hitsunder))
    check('10c. nothing on Apple\'s card supports', not hitsapple, ', '.join(hitsapple))


# 11 ------------------------------------------------------------------------------------------------------
def check_buffers(nets, pads):
    worst, far = 0.0, []
    for n, ps in nets.items():
        if not n.startswith('SE_'):
            continue
        j = [pads[(r, p)][0] for r, p in ps if r == 'J1']
        if not j:
            continue
        jx, jy = j[0][1], j[0][2]
        for r, p in ps:
            if r == 'J1':
                continue
            for (pn, x, y) in pads[(r, p)]:
                d = math.hypot(x - jx, y - jy)
                worst = max(worst, d)
                if d > 50.0:
                    far.append('%s %s.%s %.1f mm' % (n, r, p, d))
    check('11. every slot signal meets its latch, switch or resistor within 50 mm of the plug pin', not far,
          'farthest %.1f mm (straight line; route lengths are longer)%s' % (worst, ('; ' + '; '.join(far[:5])) if far else ''))


# 12 ------------------------------------------------------------------------------------------------------
def check_rules(pcb, nets):
    layers = [l for l in sexpr.find(pcb, 'layers')[1:] if l[2] == 'signal']
    check('12a. two copper layers', len(layers) == 2, ', '.join(l[1] for l in layers))
    segs = sexpr.find_all(pcb, 'segment')
    vias = sexpr.find_all(pcb, 'via')
    thin = [s for s in segs if sexpr.find(s, 'width')[1] < 0.15 - 1e-6]
    small = [v for v in vias if sexpr.find(v, 'drill')[1] < 0.3 - 1e-6 or sexpr.find(v, 'size')[1] < 0.6 - 1e-6]
    check('12b. tracks >= 0.15 mm, vias >= 0.6 mm with >= 0.3 mm drills', not thin and not small,
          '%d segments, %d vias' % (len(segs), len(vias)))
    if not segs:
        warn('12c. routing', 'the board is not routed')
        return
    # connectivity of the routed copper, net by net (pads, segment ends, vias; zones not counted)
    pcbnets = {}
    for fp in sexpr.find_all(pcb, 'footprint'):
        at = sexpr.find(fp, 'at')
        X, Y, ang = float(at[1]), float(at[2]), float(at[3]) if len(at) > 3 else 0.0
        for p in sexpr.find_all(fp, 'pad'):
            n = sexpr.find(p, 'net')
            if not n:
                continue
            pat = sexpr.find(p, 'at')
            x, y = geom.to_board(X, Y, ang, pat[1], pat[2])
            size = sexpr.find(p, 'size')
            pcbnets.setdefault(n[2], []).append(('pad', x, y, max(size[1], size[2]) / 2 + 0.05))
    netname = {s[1]: s[2] for s in sexpr.find_all(pcb, 'net')}
    segs_by = {}
    for s in segs:
        n = netname[sexpr.find(s, 'net')[1]]
        a, b = sexpr.find(s, 'start'), sexpr.find(s, 'end')
        segs_by.setdefault(n, []).append(((a[1], a[2]), (b[1], b[2])))
    vias_by = {}
    for v in vias:
        n = netname[sexpr.find(v, 'net')[1]]
        at = sexpr.find(v, 'at')
        vias_by.setdefault(n, []).append((at[1], at[2]))
    unrouted = []
    open_conn = [0]
    for n, items in pcbnets.items():
        if len(items) < 2:
            continue
        uf = netlist.UF()
        nodes = [('p%d' % i, it[1], it[2], it[3]) for i, it in enumerate(items)]
        for j, (a, b) in enumerate(segs_by.get(n, [])):
            uf.u(('s', a), ('s', b))
            for nd in nodes:
                for q in (a, b):
                    if math.hypot(q[0] - nd[1], q[1] - nd[2]) <= nd[3]:
                        uf.u(nd[0], ('s', q))
        pts = [q for ab in segs_by.get(n, []) for q in ab]
        # an end lying on another segment (a T junction) joins it
        def on_seg(q, a, b):
            dx, dy = b[0] - a[0], b[1] - a[1]
            L2 = dx * dx + dy * dy
            if L2 == 0:
                return math.hypot(q[0] - a[0], q[1] - a[1]) < 0.01
            t = max(0.0, min(1.0, ((q[0] - a[0]) * dx + (q[1] - a[1]) * dy) / L2))
            return math.hypot(q[0] - a[0] - t * dx, q[1] - a[1] - t * dy) < 0.01
        sl = segs_by.get(n, [])
        for q in pts:
            for (a, b) in sl:
                if on_seg(q, a, b):
                    uf.u(('s', q), ('s', a))
        for v in vias_by.get(n, []):
            for q in pts:
                if math.hypot(q[0] - v[0], q[1] - v[1]) < 0.01:
                    uf.u(('v', v), ('s', q))
        roots = {uf.f(nd[0]) for nd in nodes}
        if len(roots) > 1:
            unrouted.append(n)
            open_conn[0] += len(roots) - 1
    total = len([n for n, it in pcbnets.items() if len(it) >= 2])
    ok = not unrouted
    (check if ok else warn)('12c. routing: %d of %d nets fully connected by tracks (ground pours not counted), %d connections open' % (
                            total - len(unrouted), total, open_conn[0]),
                            ', '.join(unrouted) if unrouted else 'all')


def main():
    parts = D.build()
    check_parse()
    import check_pinout
    import io
    import contextlib
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = check_pinout.main()
    check('2. slot plug: every pin against se-pds-pinout.csv, docs/22, the schematic and the board', rc == 0,
          buf.getvalue().strip().splitlines()[-1])
    pins, nets, problems = netlist.schematic_netlist()
    check('2b. the schematic reads back cleanly (no unconnected, unflagged pins; no wire with two names)', not problems,
          '; '.join(problems[:5]))
    pcb, pads, fps = netlist.board_netlist()
    check_nets(pins, nets, pads)
    check_cpld(pins)
    check_power(parts, pins, pads)
    check_outline(pcb, pads, fps)
    rows = check_heights(parts, fps)
    check_keepouts(fps)
    check_buffers(nets, pads)
    check_rules(pcb, nets)
    nf = 0
    for st, name, det in RESULTS:
        print('%-4s %s%s' % (st, name, (': ' + det) if det else ''))
        nf += st == 'FAIL'
    print('%d checks, %d failed, %d warnings' % (len([r for r in RESULTS if r[0] in ('PASS', 'FAIL')]), nf,
                                                  len([r for r in RESULTS if r[0] == 'W'])))
    with open(os.path.join(ROOT, 'img', 'heights.csv'), 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['item', 'height_mm', 'limit_mm', 'y_rear_mm', 'margin_mm'])
        for r in sorted(rows, key=lambda r: r[2] - r[1]):
            w.writerow([r[0], '%.2f' % r[1], '%.2f' % r[2], '%.2f' % r[3], '%.2f' % (r[2] - r[1])])
    return 1 if nf else 0


if __name__ == '__main__':
    sys.exit(main())
