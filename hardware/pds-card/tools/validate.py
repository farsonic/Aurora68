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
  12. the board design rules (4 layers and their stackup, track and via sizes, through vias, our own clearance check) and, if routed, how many connections are routed.
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
    info('6c. CPLD logic size', 'yosys 0.71 (generic 4-input LUT mapping, 25 Nov 2026): 145 LUTs, 117 flip-flops; '
         'a MAX II logic element is one LUT and one flip-flop, so 145 to about 220 LEs of the EPM240\'s 240. '
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
    pi_rear = min(D.pi_xy(0, 0)[1], D.pi_xy(0, D.PI_D)[1])
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
    bx = [D.pi_xy(u, v) for u in (0, D.PI_W) for v in (0, D.PI_D)]
    board_in = all(0 <= x <= D.APPLE_W and D.CARD_REAR_Y <= y <= D.APPLE_BODY_H for x, y in bx)
    over = []
    for name, u0, u1, v0, v1, h in D.PI_TOP_PARTS + D.PI_UNDERSIDE:
        cs = [D.pi_xy(u, v) for u in (u0, u1) for v in (v0, v1)]
        o = max(max(y for x, y in cs) - D.APPLE_BODY_H, D.CARD_REAR_Y - min(y for x, y in cs),
                -min(x for x, y in cs), max(x for x, y in cs) - D.APPLE_W)
        if o > 1e-6:
            over.append('%s %.1f mm' % (name, o))
    check('9d. the Pi 3A+\'s board stays over the card', board_in,
          'x %.1f-%.1f, y %.1f-%.1f' % (min(x for x, y in bx), max(x for x, y in bx), min(y for x, y in bx), max(y for x, y in bx)))
    if over:
        warn('9e. parts of the Pi standing past the card\'s edge', ', '.join(over) + ' beyond the front edge (Apple\'s outline): '
             'the price of the Pi beside J1 with the 3B\'s ports pointing away from it (README, "Placement")')
    else:
        check('9e. nothing of the Pi stands past the card\'s edge', True)
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
def check_fan(crts, partboxes, hmap):
    """10e. The optional blower M1: clear of the Pi, the keep-outs under its parts and the USB plug in its socket,
    under Apple's slope at its worst-case height, nothing else under it, its outlet facing the Pi, its lead able to
    reach J3; and where its air goes under the Pi (information)."""
    if 'M1' not in crts:
        check('10e. the fan M1', False, 'not on the board')
        return
    fb = crts['M1']
    bad = []
    xa, ya = D.pi_xy(0, 0); xb, yb = D.pi_xy(D.PI_W, D.PI_D)
    pi_box = (min(xa, xb), min(ya, yb), max(xa, xb), max(ya, yb))
    if geom.boxes_overlap(fb, pi_box):
        bad.append('under the Pi\'s board')
    for pn, pb in partboxes:
        if geom.boxes_overlap(fb, pb):
            bad.append('over the keep-out under the Pi\'s %s' % pn)
    name, u0, u1, v0, v1 = D.PI_USB_PLUG
    (p0x, p0y), (p1x, p1y) = D.pi_xy(u0, v0), D.pi_xy(u1, v1)
    plug = (min(p0x, p1x), min(p0y, p1y), max(p0x, p1x), max(p0y, p1y))
    gap_plug = min(abs(plug[0] - fb[2]), abs(plug[1] - fb[3])) if not geom.boxes_overlap(fb, plug) else -1
    if gap_plug < 0:
        bad.append('on the %s' % name)
    others = [r for r, b in crts.items() if r != 'M1' and geom.boxes_overlap(b, fb)]
    if others:
        bad.append('parts under it: ' + ', '.join(others))
    lim = D.height_limit(fb[1])
    if D.FAN_T_MAX > lim:
        bad.append('%.1f mm tall at y = %.1f, limit %.2f' % (D.FAN_T_MAX, fb[1], lim))
    ox, oy0, oy1 = D.fan_outlet()
    if not (ox < pi_box[0] and oy0 < pi_box[3] and oy1 > pi_box[1]):
        bad.append('its outlet does not face the Pi')
    lx, ly = D.fan_lead_exit()
    jx, jy = D.J3_AT
    reach = abs(jx - lx) + abs(jy - ly)
    if reach > D.FAN_LEAD - 15:
        bad.append('J3 %.0f mm from the lead\'s exit (lead %.0f mm)' % (reach, D.FAN_LEAD))
    check('10e. the fan M1: %.0f x %.0f x %.1f mm (worst case) at x %.1f-%.1f, y %.1f-%.1f: clear of the Pi, its keep-outs and '
          'the USB plug (%.1f mm), nothing under it, height %.1f of %.2f, outlet facing the Pi, J3 %.0f mm from its lead'
          % (D.FAN_SIZE, D.FAN_SIZE, D.FAN_T_MAX, fb[0], fb[2], fb[1], fb[3], gap_plug, D.FAN_T_MAX, lim, reach),
          not bad, '; '.join(bad))
    # where the air goes: the outlet's band against what stands in the 8.5 mm gap at the Pi's edge
    hdr = [pb for pn, pb in partboxes if pn == 'USB-A'][0]
    h6 = [D.pi_xy(61.5, 52.5)][0]
    wall = max(D.pi_header_pin(39)[1], D.pi_header_pin(40)[1]) + 1.27        # the socket's front face
    wall = max(wall, h6[1] + 2.75)                                            # and standoff H6's
    open0, open1 = wall, hdr[1]
    ov = max(0.0, min(oy1, open1) - max(oy0, open0))
    info('10f. the fan\'s air', 'outlet y %.1f-%.1f (%.0f mm) at x = %.1f, %.1f mm from the Pi\'s edge; the open channel under the '
         'Pi between its header (and standoff H6) and its USB socket is y %.1f-%.1f: %.1f mm of the outlet faces it. The SoC '
         '(y %.1f-%.1f) is behind the USB socket from this side: the air passes over the CPLD and along the SoC\'s rear '
         'face and top, not straight through it (README, "The fan")' % (
             oy0, oy1, oy1 - oy0, ox, pi_box[0] - ox, open0, open1, ov,
             min(D.pi_xy(0, 24.2)[1], D.pi_xy(0, 38.3)[1]), max(D.pi_xy(0, 24.2)[1], D.pi_xy(0, 38.3)[1])))



PI_OWN_PARTS = 2.0      # the Pi's own small parts on its components side (now facing the card), allowed for
UNDER_PI_MAX = D.PI_GAP - PI_OWN_PARTS - 0.5     # a card part under the Pi: 6.0 mm at most


def check_keepouts(fps, parts, pcb):
    areas = D.keepouts()
    hmap = {p.ref: p.height for p in parts}
    partboxes = []
    for name, u0, u1, v0, v1, h in D.PI_TOP_PARTS:
        if name == 'header':
            continue
        xa, ya = D.pi_xy(u0, v0); xb, yb = D.pi_xy(u1, v1)
        partboxes.append((name, (min(xa, xb), min(ya, yb), max(xa, xb), max(ya, yb))))
    crts = {}
    for ref, X, Y, ang, side, fp in fps:
        if side != 'F':
            continue
        c = geom.fp_courtyard(fp, 'F.CrtYd')
        if c:
            bx = geom.box_to_board(c, X, Y, ang)
            crts[ref] = (bx[0] - OX, bx[1] - OY, bx[2] - OX, bx[3] - OY)
    hits3b, hitsunder, hitsapple, under = [], [], [], []
    for (x0, y0, x1, y1, name, what) in areas:
        for ref, b in crts.items():
            if not geom.boxes_overlap(b, (x0, y0, x1, y1)):
                continue
            if name == 'PI3B_EXT':
                n = D.fan_notch()
                if not (ref == 'M1' and n[0] - 1e-6 <= b[0] and b[2] <= n[2] + 1e-6 and b[3] <= n[3] + 1e-6):
                    hits3b.append(ref)          # the fan, in its notch, is the one exception (taken off for a 3B)
            elif name == 'PI_3AP_UNDER' and not (ref == 'J2' or ref in ('H3', 'H4', 'H5', 'H6', 'H7', 'H8')):
                # a low part may sit under the Pi, clear of the keep-outs under the Pi's own taller parts
                over = [pn for pn, pb in partboxes if geom.boxes_overlap(b, pb)]
                if over or hmap.get(ref, 99) > UNDER_PI_MAX:
                    hitsunder.append('%s (%s)' % (ref, ', '.join(over) if over else '%.1f mm tall' % hmap.get(ref, 99)))
                else:
                    under.append(ref)
            elif name == 'APPLE_SUPPORT' and ref not in ('H1', 'H2'):
                hitsapple.append(ref)
    k3 = [a for a in areas if a[4] == 'PI3B_EXT'][0]
    kp = [a for a in areas if a[4] == 'PI3B_PORTS'][0]
    check('10a. no courtyard in the Pi 3B/3B+ keep-out: its board, USB and RJ45 with their overhang and a %.0f mm plug zone, '
          '+%.0f mm (x %.1f-%.1f, y %.1f-%.1f), less the optional fan\'s notch' % (D.PI3B_PLUG_ZONE, D.KEEP_MARGIN, k3[0], k3[2], k3[1], k3[3]),
          not hits3b, ', '.join(hits3b))
    check_fan(crts, partboxes, hmap)
    check('10b. under the Pi 3A+: only parts of %.1f mm or less, clear of the keep-outs under its USB, HDMI, audio, camera, '
          'display and SoC' % UNDER_PI_MAX, not hitsunder,
          ('; '.join(hitsunder) + '; ' if hitsunder else '') + 'under the Pi: J2, H3-H8, ' + ', '.join(sorted(under)))
    vias3b = []
    for v in sexpr.find_all(pcb, 'via'):
        at = sexpr.find(v, 'at')
        x, y = at[1] - OX, at[2] - OY
        if kp[0] <= x <= kp[2] and kp[1] <= y <= kp[3]:
            vias3b.append('(%.1f, %.1f)' % (x, y))
    check('10d. no vias over the Pi 3B/3B+\'s board and ports (x %.1f-%.1f, y %.1f-%.1f)' % (kp[0], kp[2], kp[1], kp[3]), not vias3b, '%d: %s' % (len(vias3b), ' '.join(vias3b[:8])))
    check('10c. nothing on Apple\'s card supports', not hitsapple, ', '.join(hitsapple))


# 12d ------------------------------------------------------------------------------------------------------
def track_graph(pcb):
    """Per net: an adjacency map of points joined by tracks (segment ends, T junctions, vias), lengths in mm."""
    netname = {s[1]: s[2] for s in sexpr.find_all(pcb, 'net')}
    segs = {}
    for sg in sexpr.find_all(pcb, 'segment'):
        n = netname[sexpr.find(sg, 'net')[1]]
        a, b = sexpr.find(sg, 'start'), sexpr.find(sg, 'end')
        segs.setdefault(n, []).append(((round(a[1], 3), round(a[2], 3)), (round(b[1], 3), round(b[2], 3))))
    graphs = {}
    for n, sl in segs.items():
        g = {}
        pts = {q for ab in sl for q in ab}
        for a, b in sl:
            # split a segment where another segment's end lies on it
            on = [a, b] + [q for q in pts if q not in (a, b) and _on_seg(q, a, b)]
            on.sort(key=lambda q: math.hypot(q[0] - a[0], q[1] - a[1]))
            for p, q in zip(on, on[1:]):
                d = math.hypot(q[0] - p[0], q[1] - p[1])
                g.setdefault(p, {})[q] = d
                g.setdefault(q, {})[p] = d
        graphs[n] = g
    return graphs


def _on_seg(q, a, b):
    dx, dy = b[0] - a[0], b[1] - a[1]
    L2 = dx * dx + dy * dy
    if L2 == 0:
        return False
    t = ((q[0] - a[0]) * dx + (q[1] - a[1]) * dy) / L2
    if t <= 0 or t >= 1:
        return False
    return math.hypot(q[0] - a[0] - t * dx, q[1] - a[1] - t * dy) < 0.01


def routed_length(graph, pa, pb, ra=0.8, rb=0.8):
    """Shortest track path from a pad at pa to a pad at pb (Dijkstra), or None."""
    import heapq
    if not graph:
        return None
    starts = [q for q in graph if math.hypot(q[0] - pa[0], q[1] - pa[1]) <= ra]
    goals = {q for q in graph if math.hypot(q[0] - pb[0], q[1] - pb[1]) <= rb}
    if not starts or not goals:
        return None
    dist = {q: math.hypot(q[0] - pa[0], q[1] - pa[1]) for q in starts}
    h = [(d, q) for q, d in dist.items()]
    heapq.heapify(h)
    while h:
        d, q = heapq.heappop(h)
        if d > dist.get(q, 1e9):
            continue
        if q in goals:
            return d + math.hypot(q[0] - pb[0], q[1] - pb[1])
        for r, w in graph[q].items():
            nd = d + w
            if nd < dist.get(r, 1e9):
                dist[r] = nd
                heapq.heappush(h, (nd, r))
    return None


def microstrip(w, h, er):
    """(Z0 ohm, pF/mm) of a track w wide h above its plane (Hammerstad's formulas; w/h < 1)."""
    u = w / h
    ee = (er + 1) / 2 + (er - 1) / 2 * ((1 + 12 / u) ** -0.5 + 0.04 * (1 - u) ** 2)
    z0 = 60 / math.sqrt(ee) * math.log(8 / u + u / 4)
    return z0, 1000.0 * math.sqrt(ee) / (299.792458 * z0)      # pF/mm: sqrt(eeff) / (c Z0), c in mm/ns
_PRE = [l for l in D.STACKUP if l[0] == 'dielectric 1'][0]
Z0_SIG, PF_PER_MM = microstrip(0.15, _PRE[2], _PRE[4])   # 0.15 mm on L1 over L2 (or L4 over L3), 0.21 mm of 7628
CBTD_PF = 12.0          # a CBTD3384 channel, on (TI's Cio(on), typical, rounded up)
CPLD_PF = 10.0          # a MAX II I/O pin (Intel: 8 pF, rounded up)


def check_lengths(pcb, nets):
    """Pi-to-CPLD nets (fail if GPCLK0 is over 30 mm), and the longest CPLD-to-latch and CPLD-to-switch nets.
    Routed track lengths where the tracks join the two pads; the straight line (marked ~) where they do not."""
    pcb_, pads, fps = netlist.board_netlist()
    graphs = track_graph(pcb)
    routed = bool(sexpr.find_all(pcb, 'segment'))
    def length(net, a, b):
        pa, pb = pads[a][0][1:], pads[b][0][1:]
        L = routed_length(graphs.get(net), pa, pb) if routed else None
        return (L, False) if L is not None else (math.hypot(pa[0] - pb[0], pa[1] - pb[1]), True)
    rows = []
    power = {'GND', '+3V3', '5V_PI', '+5V_SLOT'}
    nets = {n: ps for n, ps in nets.items() if n not in power}
    for net, ps in sorted(nets.items()):
        j2 = [p for p in ps if p[0] == 'J2']
        u1 = [p for p in ps if p[0] == 'U1']
        if j2 and u1:
            L, est = length(net, j2[0], u1[0])
            rows.append((net, L, est))
    # the clock: J2 pin 7 -> R12 -> U1 pin 12
    a, est_a = length('PI_CLK_GPIO', ('J2', '7'), ('R12', '1'))
    b, est_b = length('PI_CLK', ('R12', '2'), ('U1', '12'))
    clk = a + b + 1.6
    rows.append(('PI_CLK (GPCLK0, J2.7-R12-U1.12)', clk, est_a or est_b))
    rows.sort(key=lambda r: -r[1])
    fmt = lambda r: '%s %s%.1f' % (r[0], '~' if r[2] else '', r[1])
    check('12d. Pi-to-CPLD nets: GPCLK0 %s%.1f mm (30 at most), longest %s mm; %s' % ('~' if rows[[r[0] for r in rows].index(
        'PI_CLK (GPCLK0, J2.7-R12-U1.12)')][2] else '', clk, fmt(rows[0]), 'routed lengths' if routed else 'straight lines (unrouted)'),
          clk <= 30.0, ', '.join(fmt(r) for r in rows))
    lat, sw, slot = [], [], []
    for net, ps in nets.items():
        u1 = [p for p in ps if p[0] == 'U1']
        if not u1:
            continue
        for p in ps:
            if p[0] in ('U2', 'U3', 'U4', 'U5'):
                L, est = length(net, u1[0], p)
                lat.append((net + ' U1-' + p[0], L, est))
            if p[0] in ('U6', 'U7'):
                L, est = length(net, u1[0], p)
                sw.append((net, L, est))
    lat.sort(key=lambda r: -r[1]); sw.sort(key=lambda r: -r[1])
    info('12e. CPLD to latches (the latch strobes and the Pi\'s data bus: Pi speed)', 'longest %s mm; %d connections' % (fmt(lat[0]), len(lat)))
    # the Pi's data bus from the socket to every latch
    pl = []
    for net, ps in nets.items():
        j2 = [p for p in ps if p[0] == 'J2']
        if not j2:
            continue
        for p in ps:
            if p[0] in ('U2', 'U3', 'U4', 'U5'):
                L, est = length(net, j2[0], p)
                pl.append((net + ' J2-' + p[0], L, est))
    pl.sort(key=lambda r: -r[1])
    (check if True else warn)('12h. the Pi\'s data bus from its socket to the latches (aim: about 50 mm or less)', pl[0][1] <= 60.0,
                              'longest %s mm; over 50: %s' % (fmt(pl[0]), ', '.join(fmt(r) for r in pl if r[1] > 50) or 'none'))
    # Apple's 2-inch rule along the copper: from each slot pin to the card's first part on that line
    far = []
    worst = None
    for net, ps in nets.items():
        if not net.startswith('SE_'):
            continue
        j1 = [p for p in ps if p[0] == 'J1']
        others = [p for p in ps if p[0] != 'J1']
        if not j1 or not others:
            continue
        L = min(length(net, j1[0], p) for p in others)
        if worst is None or L[0] > worst[1]:
            worst = (net, L[0], L[1])
        if L[0] > 50.0:
            far.append(fmt((net, L[0], L[1])))
    check('11b. every slot signal reaches its latch, switch or resistor within 50 mm of copper (Apple\'s 2 inches)', not far,
          'longest %s mm%s' % (fmt(worst), ('; over: ' + ', '.join(far)) if far else ''))
    # the SE's lines through the CBTD3384 switches: plug to switch, switch to CPLD, and what they load the board with
    pairs = [('3', '2'), ('4', '5'), ('7', '6'), ('8', '9'), ('11', '10'), ('14', '15'), ('17', '16'), ('18', '19'), ('21', '20'), ('22', '23')]
    pin_net = {}
    for net, ps in nets.items():
        for p in ps:
            pin_net[p] = net
    loads, bad = [], []
    for sw_ in ('U6', 'U7'):
        for a, b in pairs:
            na, nb = pin_net.get((sw_, a)), pin_net.get((sw_, b))
            if not na or not nb:
                continue
            j1 = [p for p in nets[na] if p[0] == 'J1']
            u1 = [p for p in nets[nb] if p[0] == 'U1']
            if not j1 or not u1:
                continue
            L1, e1 = length(na, j1[0], (sw_, a))
            L2, e2 = length(nb, (sw_, b), u1[0])
            pf = PF_PER_MM * (L1 + L2) + CBTD_PF + CPLD_PF
            loads.append((na, L1 + L2, pf, e1 or e2))
            if pf > 30.0:
                bad.append(na)
    j1 = [p for p in nets['SE_C8M'] if p[0] == 'J1'][0]
    Lc1, ec1 = length('SE_C8M', j1, ('R3', '1'))
    Lc2, ec2 = length('M68K_CLK', ('R3', '2'), ('U1', '62'))
    c8 = PF_PER_MM * (Lc1 + Lc2 + 1.6) + CPLD_PF
    loads.sort(key=lambda r: -r[2])
    check('12i. the SE\'s lines through the switches: %s%.0f mm and %.1f pF at most (Apple: 30 pF); C8M %s%.0f mm, %.1f pF (Apple: 20 pF)' % (
        '~' if loads[0][3] else '', loads[0][1], loads[0][2], '~' if ec1 or ec2 else '', Lc1 + Lc2 + 1.6, c8),
          not bad and c8 <= 20.0,
          'estimate: %.3f pF/mm of 0.15 mm track (%.0f ohm microstrip over the plane 0.21 mm below), %.0f pF for the switch, %.0f pF for the CPLD pin; ' % (
              PF_PER_MM, Z0_SIG, CBTD_PF, CPLD_PF) + ', '.join('%s %s%.0f mm %.1f pF' % (n, '~' if e else '', L, c) for n, L, c, e in loads))
    # copper per layer: signals on L1 and L4 only, the clocks on L1 only (over L2's ground)
    netname = {x[1]: x[2] for x in sexpr.find_all(pcb, 'net')}
    lens = {}
    inner, clk_off = [], set()
    for sg in sexpr.find_all(pcb, 'segment'):
        a_, b_ = sexpr.find(sg, 'start'), sexpr.find(sg, 'end')
        lay = sexpr.find(sg, 'layer')[1]
        n = netname[sexpr.find(sg, 'net')[1]]
        lens[lay] = lens.get(lay, 0.0) + math.hypot(b_[1] - a_[1], b_[2] - a_[2])
        if lay in D.PLANES:
            inner.append(n)
        if n in D.CLOCK_NETS and lay != 'F.Cu':
            clk_off.add(n)
    if routed:
        check('12g. tracks on L1 and L4 only (L1 %.0f mm, L4 %.0f mm; L2 and L3 planes), %d vias; the clocks (%s) on L1 only, '
              'over L2\'s ground' % (lens.get('F.Cu', 0), lens.get('B.Cu', 0), len(sexpr.find_all(pcb, 'via')), ', '.join(D.CLOCK_NETS)),
              not inner and not clk_off, ('inner-layer tracks: ' + ', '.join(sorted(set(inner))) if inner else '') +
              (' clocks off L1: ' + ', '.join(sorted(clk_off)) if clk_off else ''))
    info('12f. CPLD to bus switches', 'longest %s mm. The CBTD3384 is a switch, not a buffer, so each such line adds its '
         'length (and the CPLD pin) to the SE\'s own line: see README "Placement"' % fmt(sw[0]))
    return rows, lat, sw


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
def _seg_seg(a, b, c, d):
    def o(p, q, r):
        return (q[0] - p[0]) * (r[1] - p[1]) - (q[1] - p[1]) * (r[0] - p[0])
    if o(a, b, c) * o(a, b, d) < 0 and o(c, d, a) * o(c, d, b) < 0:
        return 0.0
    import route
    return min(route.pt_seg(a, c, d), route.pt_seg(b, c, d), route.pt_seg(c, a, b), route.pt_seg(d, a, b))


def _seg_rect(a, b, cx, cy, hw, hh):
    import route
    if route.pt_rect(a, cx, cy, hw, hh) == 0 or route.pt_rect(b, cx, cy, hw, hh) == 0:
        return 0.0
    cs = [(cx - hw, cy - hh), (cx + hw, cy - hh), (cx + hw, cy + hh), (cx - hw, cy + hh)]
    return min([route.pt_rect(a, cx, cy, hw, hh), route.pt_rect(b, cx, cy, hw, hh)] +
               [_seg_seg(a, b, p, q) for p, q in zip(cs, cs[1:] + cs[:1])])


def check_clearance(items, minimum=0.15):
    """12k. A copper clearance check of our own (not KiCad's DRC): tracks, vias and pads of different nets on the same
    outer layer at least 0.15 mm apart (track edge to edge; pads as their rectangles). Pads of one footprint against each
    other, the zones and the planes' clearances are left to KiCad."""
    import route
    cell = 2.0
    grid = {}
    shapes = []
    for it in items:
        if it[0] == 'seg':
            a, b, r = it[2], it[3], it[4]
            box = (min(a[0], b[0]) - r, min(a[1], b[1]) - r, max(a[0], b[0]) + r, max(a[1], b[1]) + r)
            shapes.append((it, box, {it[5]}))
        elif it[0] == 'via':
            box = (it[2] - it[4], it[3] - it[4], it[2] + it[4], it[3] + it[4])
            shapes.append((it, box, set(route.CU)))
        elif it[0] == 'pad':
            box = (it[2] - it[4], it[3] - it[5], it[2] + it[4], it[3] + it[5])
            shapes.append((it, box, set(it[6])))
    for i, (it, box, lays) in enumerate(shapes):
        for gx in range(int((box[0] - minimum) // cell), int((box[2] + minimum) // cell) + 1):
            for gy in range(int((box[1] - minimum) // cell), int((box[3] + minimum) // cell) + 1):
                grid.setdefault((gx, gy), []).append(i)
    def dist(p, q):
        if p[0] == 'seg' and q[0] == 'seg':
            return _seg_seg(p[2], p[3], q[2], q[3]) - p[4] - q[4]
        if p[0] == 'seg' and q[0] == 'via':
            return route.pt_seg((q[2], q[3]), p[2], p[3]) - p[4] - q[4]
        if q[0] == 'pad' and q[7]:            # a round pad: as a via of its radius
            q = ('via', q[1], q[2], q[3], q[4])
            if p[0] == 'pad' and p[7]:
                p = ('via', p[1], p[2], p[3], p[4])
            elif p[0] == 'pad':
                return route.pt_rect((q[2], q[3]), p[2], p[3], p[4], p[5]) - q[4]
        if p[0] == 'seg' and q[0] == 'pad':
            return _seg_rect(p[2], p[3], q[2], q[3], q[4], q[5]) - p[4]
        if p[0] == 'via' and q[0] == 'via':
            return math.hypot(p[2] - q[2], p[3] - q[3]) - p[4] - q[4]
        if p[0] == 'via' and q[0] == 'pad':
            return route.pt_rect((p[2], p[3]), q[2], q[3], q[4], q[5]) - p[4]
        if p[0] == 'pad' and q[0] == 'pad':
            return None
        if p[0] == 'pad' and p[7]:
            p = ('via', p[1], p[2], p[3], p[4])
            return dist(p, q) if q[0] != 'via' else math.hypot(p[2] - q[2], p[3] - q[3]) - p[4] - q[4]
        return dist(q, p)
    seen, bad, worst = set(), [], 9.9
    for cellitems in grid.values():
        for x in range(len(cellitems)):
            for y in range(x + 1, len(cellitems)):
                i, j = cellitems[x], cellitems[y]
                if (i, j) in seen:
                    continue
                seen.add((i, j))
                (p, bp, lp), (q, bq, lq) = shapes[i], shapes[j]
                if p[1] == q[1] or not (lp & lq):
                    continue
                if bp[0] > bq[2] + minimum or bq[0] > bp[2] + minimum or bp[1] > bq[3] + minimum or bq[1] > bp[3] + minimum:
                    continue
                d = dist(p, q)
                if d is None:
                    continue
                worst = min(worst, d)
                if d < minimum - 0.005:
                    loc = p[2] if p[0] == 'seg' else (p[2], p[3])
                    bad.append('%s/%s %.3f mm at (%.1f, %.1f)' % (p[1] or '-', q[1] or '-', d, loc[0] - OX, loc[1] - OY))
    check('12k. copper clearance (our own check, not KiCad\'s DRC): tracks, vias and pads of different nets >= %.2f mm on each '
          'layer; closest %.3f mm' % (minimum, worst), not bad, '%d: %s' % (len(bad), '; '.join(bad[:8])))


def check_stitching(pcb, items):
    """12j. A ground via (or through-hole ground pad) within 2.5 mm of every decoupling capacitor's ground pad and of
    every CPLD ground pin; the slot plug's ground pins are through-hole, on L2 themselves."""
    gnd = [(it[2], it[3]) for it in items if it[0] == 'via' and it[1] == 'GND'] + \
          [(it[2], it[3]) for it in items if it[0] == 'pad' and it[1] == 'GND' and 'In1.Cu' in it[6]]
    far = []
    n = 0
    for fp in sexpr.find_all(pcb, 'footprint'):
        ref = [c[2] for c in sexpr.find_all(fp, 'property') if c[1] == 'Reference'][0]
        if not (ref.startswith('C') or ref == 'U1'):
            continue
        at = sexpr.find(fp, 'at')
        X, Y, ang = float(at[1]), float(at[2]), float(at[3]) if len(at) > 3 else 0.0
        for p in sexpr.find_all(fp, 'pad'):
            nn = sexpr.find(p, 'net')
            if not nn or nn[2] != 'GND' or str(p[2]) != 'smd':
                continue
            pat = sexpr.find(p, 'at')
            x, y = geom.to_board(X, Y, ang, float(pat[1]), float(pat[2]))
            n += 1
            d = min(math.hypot(x - gx, y - gy) for gx, gy in gnd) if gnd else 99
            if d > 2.5:
                far.append('%s.%s %.1f mm' % (ref, p[1], d))
    nst = len([v for v in sexpr.find_all(pcb, 'via') if 'stitch' in str(sexpr.find(v, 'uuid'))])
    check('12j. ground vias within 2.5 mm of all %d ground pads of the decoupling capacitors and the CPLD; J1\'s ground pins '
          'through-hole on L2' % n, not far, ', '.join(far[:10]))


def check_rules(pcb, nets):
    layers = [l for l in sexpr.find(pcb, 'layers')[1:] if l[1].endswith('.Cu')]
    stack = sexpr.find(sexpr.find(pcb, 'setup'), 'stackup')
    tot = sum(sexpr.find(l, 'thickness')[1] for l in sexpr.find_all(stack, 'layer') if sexpr.find(l, 'thickness')) if stack else 0
    zones = {(sexpr.find(z, 'layer')[1], sexpr.find(z, 'net_name')[1]) for z in sexpr.find_all(pcb, 'zone') if sexpr.find(z, 'layer')}
    planes_ok = all((l, n) in zones for l, n in D.PLANES.items())
    check('12a. four copper layers, L1 F.Cu signals, L2 In1.Cu GND plane, L3 In2.Cu +3.3 V plane, L4 B.Cu signals; stackup %.3f mm '
          '(JLC04161H-7628)' % tot, [l[1] for l in layers] == ['F.Cu', 'In1.Cu', 'In2.Cu', 'B.Cu'] and planes_ok and abs(tot - 1.6) < 0.05,
          ', '.join(l[1] for l in layers))
    segs = sexpr.find_all(pcb, 'segment')
    vias = sexpr.find_all(pcb, 'via')
    thin = [s for s in segs if sexpr.find(s, 'width')[1] < 0.15 - 1e-6]
    small = [v for v in vias if sexpr.find(v, 'drill')[1] < 0.3 - 1e-6 or sexpr.find(v, 'size')[1] < 0.6 - 1e-6]
    blind = [v for v in vias if [str(x) for x in sexpr.find(v, 'layers')[1:]] != ['F.Cu', 'B.Cu']]
    import route
    cu_items = route.copper_items(pcb)
    inpad = []
    for v in vias:
        at = sexpr.find(v, 'at')
        for it in cu_items:
            if it[0] == 'pad' and 'In1.Cu' not in it[6] and route.pt_rect((at[1], at[2]), it[2], it[3], it[4], it[5]) < 1e-6:
                inpad.append('(%.1f, %.1f)' % (at[1] - OX, at[2] - OY))
    check('12b. tracks >= 0.15 mm, vias 0.6 mm with 0.3 mm drills, all through vias (no blind or buried), none in a pad',
          not thin and not small and not blind and not inpad,
          '%d segments, %d vias%s' % (len(segs), len(vias), ('; in pads: ' + ' '.join(inpad[:6])) if inpad else ''))
    check_clearance(cu_items)
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
            pcbnets.setdefault(n[2], []).append(('tht' if str(p[2]) == 'thru_hole' else 'pad', x, y, max(size[1], size[2]) / 2 + 0.05))
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
            for nd in nodes:                 # a via in a pad's area (the router's fan-out to a plane lands beside it)
                if math.hypot(v[0] - nd[1], v[1] - nd[2]) <= nd[3]:
                    uf.u(('v', v), nd[0])
            if n in D.PLANES.values():       # a via of a plane's net is joined to the plane
                uf.u(('v', v), 'plane')
        if n in D.PLANES.values():           # and so is every through-hole pad of it
            for i, it in enumerate(items):
                if it[0] == 'tht':
                    uf.u(nodes[i][0], 'plane')
        roots = {uf.f(nd[0]) for nd in nodes}
        if len(roots) > 1:
            unrouted.append(n)
            open_conn[0] += len(roots) - 1
    total = len([n for n, it in pcbnets.items() if len(it) >= 2])
    ok = not unrouted
    check_stitching(pcb, cu_items)
    (check if ok else warn)('12c. routing: %d of %d nets fully connected by tracks and, for GND and +3.3 V, their planes through vias '
                            'and through-hole pads (the outer pours not counted), %d connections open' % (
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
    check_keepouts(fps, parts, pcb)
    check_lengths(pcb, nets)
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
