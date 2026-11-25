# SPDX-License-Identifier: MPL-2.0
"""gen_kicad.py - write Card P's KiCad 8 project from tools/design.py.

    python3 tools/gen_kicad.py            # everything below, in hardware/pds-card/
    python3 tools/route.py                # then, optionally, Freerouting's tracks into the board

Writes:
    lib/aurora68.kicad_sym                the symbols (bodies drawn here; pin tables from design.py)
    lib/aurora68.pretty/Aurora68_*.kicad_mod   the footprints made here (plug, Pi socket, holes)
    aurora68-card-p.kicad_pro, sym-lib-table, fp-lib-table
    aurora68-card-p.kicad_sch + slot/cpld/pi/power/panel.kicad_sch    the hierarchical schematic
    aurora68-card-p.kicad_pcb             outline, keep-outs, holes, every footprint placed, nets, ground zones
    bom.csv                               the parts list
    rtl/aurora68_pds.qsf                  the CPLD's pin assignments (the same table as the schematic's)

UUIDs are derived from names (uuid5), so a regeneration changes only what changed.
"""

import csv
import json
import math
import os
import sys
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sexpr
from sexpr import Sym, S
import design as D
import geom

ROOT = D.ROOT
PROJECT = 'aurora68-card-p'
NS = uuid.UUID('6f1c0b52-4c2a-4f0e-9d1a-a1b2c3d4e5f6')
OX, OY = 30.0, 10.0                   # Apple's (0, 0) on the board's page
DATE = '2026-11-24'
REV = 'P0.1 (draft)'
GRID = 2.54


def uid(*names):
    return str(uuid.uuid5(NS, '/'.join(str(n) for n in names)))


def font(size=1.27, thick=None):
    f = S('font', S('size', size, size))
    if thick:
        f.append(S('thickness', thick))
    return f


def effects(size=1.27, justify=None, hide=False, thick=None):
    e = S('effects', font(size, thick))
    if justify:
        e.append([Sym('justify')] + [Sym(j) for j in justify.split()])
    if hide:
        e.append(S('hide', Sym('yes')))
    return e


# ========================================================================================================
# Symbols
# ========================================================================================================
def symbol_pins(name):
    if name == 'SE_PDS_96':
        rows = D.slot_pins()
        return [(r['row'] + r['pin'], r['signal'], {'Power': 'passive', 'Ground': 'passive', 'Reserved': 'no_connect',
                 'NC': 'no_connect'}.get(r['direction'], 'passive' if r['signal'].startswith('+') else 'bidirectional'))
                for r in rows]
    return D.SYMBOLS[name][0]()


def symbol_sides(name, pins):
    by = {p[0]: p for p in pins}
    if name == 'EPM240_570_T100':
        return [by[str(i)] for i in range(1, 51)], [by[str(i)] for i in range(51, 101)]
    if name == '74LVC16373_TSSOP48':
        nm = {p[1]: [] for p in pins}
        for p in pins:
            nm[p[1]].append(p)
        left = nm['1LE'] + nm['~{1OE}'] + [nm['1D%d' % i][0] for i in range(1, 9)] + nm['2LE'] + nm['~{2OE}'] + \
            [nm['2D%d' % i][0] for i in range(1, 9)] + nm['VCC']
        right = [nm['1Q%d' % i][0] for i in range(1, 9)] + [nm['2Q%d' % i][0] for i in range(1, 9)] + nm['GND']
        return left, right
    if name == 'SN74CBTD3384_TSSOP24':
        left = [p for p in pins if 'A' in p[1] or 'OE' in p[1] or p[1] in ('VCC', 'GND')]
        right = [p for p in pins if 'B' in p[1] and 'OE' not in p[1]]
        return left, right
    if name == 'SE_PDS_96':
        order = ['A%d' % i for i in range(1, 33)] + ['B%d' % i for i in range(1, 17)]
        order2 = ['B%d' % i for i in range(17, 33)] + ['C%d' % i for i in range(1, 33)]
        return [by[k] for k in order], [by[k] for k in order2]
    if name == 'RPI_GPIO_SOCKET_2x20':
        return [by[str(i)] for i in range(1, 41, 2)], [by[str(i)] for i in range(2, 41, 2)]
    if name == 'LM66100DCK':
        return [by['1'], by['3'], by['2']], [by['6'], by['5'], by['4']]
    if name == 'AMS1117-3.3':
        return [by['3'], by['1']], [by['2']]
    if name == 'AO3400A':
        return [by['1']], [by['3'], by['2']]
    if name in ('R', 'C', 'Polyfuse', 'LED', 'D_Schottky'):
        return [pins[0]], [pins[1]]
    if name == 'PWR_FLAG':
        return [], []
    return list(pins), []


def symbol_geometry(name):
    """(left pins with y, right pins with y, half width, top, bottom, pin length) in library units (y up)."""
    pins = symbol_pins(name)
    left, right = symbol_sides(name, pins)
    n = max(len(left), len(right), 1)
    def textw(ps):
        return max([len(p[1].replace('~{', '').replace('}', '')) for p in ps] + [0]) * 1.0
    small = name in ('R', 'C', 'Polyfuse', 'LED', 'D_Schottky')
    if small:
        hw = 2 * GRID
    else:
        hw = math.ceil((textw(left) + textw(right) + 4.0) / (2 * GRID)) * GRID
        hw = max(hw, 2 * GRID)
    top = math.ceil((n - 1) / 2.0) * GRID
    plen = GRID
    ly = [(p, top - i * GRID) for i, p in enumerate(left)]
    ry = [(p, top - i * GRID) for i, p in enumerate(right)]
    bottom = top - (n - 1) * GRID
    return ly, ry, hw, top, bottom, plen


def make_symbol(name, libname=None):
    """A KiCad 8 symbol. libname: the name as embedded in a schematic (aurora68:NAME)."""
    full = libname or name
    pins, prefix, descr, ds = D.SYMBOLS[name][0], D.SYMBOLS[name][1], D.SYMBOLS[name][2], D.SYMBOLS[name][3]
    ly, ry, hw, top, bottom, plen = symbol_geometry(name)
    is_power = name == 'PWR_FLAG'
    sym = S('symbol', full)
    if is_power:
        sym.append(S('power'))
    sym.append(S('pin_names', S('offset', 0.508 if name in ('R', 'C', 'Polyfuse', 'LED', 'D_Schottky') else 1.016)))
    sym += [S('exclude_from_sim', Sym('no')), S('in_bom', Sym('no' if is_power or name == 'MountingHole' else 'yes')),
            S('on_board', Sym('no' if is_power else 'yes'))]
    sym.append(S('property', 'Reference', prefix, S('at', 0, top + 2.54, 0), effects()))
    sym.append(S('property', 'Value', name, S('at', 0, bottom - 2.54, 0), effects()))
    sym.append(S('property', 'Footprint', '', S('at', 0, bottom - 5.08, 0), effects(hide=True)))
    sym.append(S('property', 'Datasheet', ds, S('at', 0, 0, 0), effects(hide=True)))
    sym.append(S('property', 'Description', descr, S('at', 0, 0, 0), effects(hide=True)))
    body = S('symbol', '%s_0_1' % name)
    if is_power:
        body.append(S('polyline', S('pts', S('xy', 0, 0), S('xy', 0, 1.27), S('xy', -1.016, 1.905), S('xy', 0, 2.54),
                                     S('xy', 1.016, 1.905), S('xy', 0, 1.27)),
                      S('stroke', S('width', 0), S('type', Sym('default'))), S('fill', S('type', Sym('none')))))
    elif name == 'MountingHole':
        body.append(S('circle', S('center', 0, 0), S('radius', 1.778), S('stroke', S('width', 0.254), S('type', Sym('default'))),
                      S('fill', S('type', Sym('none')))))
    else:
        h2 = 1.016 if name in ('R', 'C', 'Polyfuse', 'LED', 'D_Schottky') else GRID / 2
        body.append(S('rectangle', S('start', -hw, top + h2), S('end', hw, bottom - h2),
                      S('stroke', S('width', 0.254), S('type', Sym('default'))), S('fill', S('type', Sym('background')))))
    sym.append(body)
    unit = S('symbol', '%s_1_1' % name)
    if is_power:
        unit.append(S('pin', Sym('power_out'), Sym('line'), S('at', 0, 0, 90), S('length', 0),
                      S('name', '~', effects()), S('number', '1', effects())))
    for (p, y) in ly:
        unit.append(S('pin', Sym(p[2]), Sym('line'), S('at', -hw - plen, y, 0), S('length', plen),
                      S('name', p[1], effects()), S('number', p[0], effects())))
    for (p, y) in ry:
        unit.append(S('pin', Sym(p[2]), Sym('line'), S('at', hw + plen, y, 180), S('length', plen),
                      S('name', p[1], effects()), S('number', p[0], effects())))
    sym.append(unit)
    return sym


def write_symbol_lib(names):
    lib = S('kicad_symbol_lib', S('version', 20231120), S('generator', 'aurora68_gen_kicad'), S('generator_version', '8.0'))
    for n in names:
        lib.append(make_symbol(n))
    sexpr.save(os.path.join(ROOT, 'lib', 'aurora68.kicad_sym'), lib)


# ========================================================================================================
# Footprints made here
# ========================================================================================================
def fp_header(name, descr, tags, attr):
    return S('footprint', name, S('version', 20240108), S('generator', 'aurora68_gen_kicad'),
             S('generator_version', '8.0'), S('layer', 'F.Cu'), S('descr', descr), S('tags', tags),
             S('property', 'Reference', 'REF**', S('at', 0, -3, 0), S('layer', 'F.SilkS'), effects(1, thick=0.15)),
             S('property', 'Value', name, S('at', 0, 3, 0), S('layer', 'F.Fab'), effects(1, thick=0.15)),
             S('property', 'Footprint', '', S('at', 0, 0, 0), S('layer', 'F.Fab'), S('hide', Sym('yes')), effects(1, thick=0.15)),
             S('property', 'Datasheet', '', S('at', 0, 0, 0), S('layer', 'F.Fab'), S('hide', Sym('yes')), effects(1, thick=0.15)),
             S('property', 'Description', '', S('at', 0, 0, 0), S('layer', 'F.Fab'), S('hide', Sym('yes')), effects(1, thick=0.15)),
             S('attr', *[Sym(a) for a in attr.split()]))


def fp_rect(x0, y0, x1, y1, layer, width=0.05):
    return S('fp_rect', S('start', x0, y0), S('end', x1, y1), S('stroke', S('width', width), S('type', Sym('solid'))),
             S('fill', Sym('none')), S('layer', layer))


def fp_line(x0, y0, x1, y1, layer, width=0.12):
    return S('fp_line', S('start', x0, y0), S('end', x1, y1), S('stroke', S('width', width), S('type', Sym('solid'))),
             S('layer', layer))


def fp_circle(cx, cy, r, layer, width=0.05):
    return S('fp_circle', S('center', cx, cy), S('end', cx + r, cy), S('stroke', S('width', width), S('type', Sym('solid'))),
             S('fill', Sym('none')), S('layer', layer))


def fp_text(text, x, y, layer, size=1.0):
    return S('fp_text', Sym('user'), text, S('at', x, y, 0), S('layer', layer), effects(size, thick=0.15))


def din_plug(board_side=True):
    """The card's 96-pin plug, mounted on the underside (Figure 17-4). In the board it is placed on B.Cu at
    rotation 0 with its origin on row B's centre; local coordinates are then the board's own: row A at
    +2.54 mm (towards the board's right edge), pin 1 at +39.37 mm (towards the front). The library copy is
    the same footprint seen from the top (KiCad mirrors x when it flips a footprint left/right)."""
    m = 1 if board_side else -1
    F = 'B' if board_side else 'F'
    fp = fp_header('Aurora68_DIN41612_C_3x32_Male_Straight_PressFit_Bottom',
                   "Macintosh SE PDS plug: DIN 41612 type C, 96 pins (rows a, b, c), male, straight, press-fit, on the card's "
                   "underside (Designing Cards and Drivers, 3rd ed., Figure 17-4: 2.54 mm grid, fixing holes 2.85 mm at 90.0 mm). "
                   "Pad 'A1' is row a pin 1. Press-fit holes 1.0 mm finished (check the connector's datasheet).",
                   'DIN41612 96 Macintosh SE PDS', 'through_hole')
    for row, dx in (('A', 2.54), ('B', 0.0), ('C', -2.54)):
        for pin in range(1, 33):
            y = (16.5 - pin) * 2.54
            fp.append(S('pad', '%s%d' % (row, pin), Sym('thru_hole'), Sym('rect' if (row, pin) == ('A', 1) else 'circle'),
                        S('at', m * dx, y), S('size', 1.6, 1.6), S('drill', 1.0), S('layers', '*.Cu', '*.Mask'),
                        S('remove_unused_layers', Sym('no'))))
    for (x, y) in D.PLUG_FIX_HOLES:
        fp.append(S('pad', '', Sym('np_thru_hole'), Sym('circle'), S('at', m * (x - D.ROW_X['B']), y - D.PLUG_CENTRE_Y),
                    S('size', 2.85, 2.85), S('drill', 2.85), S('layers', '*.Cu', '*.Mask')))
    fp.append(fp_rect(-5.55 * 1, -47.5, 5.55, 47.5, F + '.Fab', 0.1))
    fp.append(fp_rect(-6.05, -48.0, 6.05, 48.0, F + '.CrtYd', 0.05))
    fp.append(fp_line(m * 4.2, 39.37 + 1.6, m * 4.2, 39.37 - 1.6, F + '.SilkS', 0.15))
    fp.append(fp_text('A1', m * 4.8, 42.0, F + '.SilkS', 1.0))
    fp.append(fp_text('row A (logic board edge)', m * 0.0, -44.0, F + '.Fab', 0.8))
    fp.append(model(MODELS + 'Aurora68_DIN41612_C_3x32_Male_Straight.wrl'))   # a symmetric body: no orientation to get wrong
    if board_side:                      # on the underside: texts on the back layers, mirrored
        for c in fp:
            if isinstance(c, list) and c and c[0] == 'property':
                lay = sexpr.find(c, 'layer')
                lay[1] = lay[1].replace('F.', 'B.')
                e = sexpr.find(c, 'effects')
                e.append(S('justify', Sym('mirror')))
    return fp


def pi_socket():
    """The 2x20 socket for the Pi turned components-down. Origin on pin 1; the pins where the inverted Pi's
    header pins land (design.pi_header_pin)."""
    fp = fp_header('Aurora68_PiSocket_2x20_Inverted',
                   '2x20 female socket, 2.54 mm, 6.0 mm body, for a Raspberry Pi 3A+ or Zero 2 W plugged in components-down. '
                   'Pin numbers are the Pi\'s; the pattern is the Pi\'s header seen from above with the Pi turned over about its '
                   'short axis, so it is the mirror image of an ordinary Pi HAT socket. Open-bottom contacts: the Pi\'s 6.0 mm '
                   'pins must seat fully (README, "The Pi").', 'Raspberry Pi socket inverted', 'through_hole')
    x1, y1 = D.pi_header_pin(1)
    xs, ys = [], []
    for n in range(1, 41):
        x, y = D.pi_header_pin(n)
        x -= x1; y -= y1
        xs.append(x); ys.append(y)
        fp.append(S('pad', str(n), Sym('thru_hole'), Sym('rect' if n == 1 else 'oval'), S('at', round(x, 4), round(y, 4)),
                    S('size', 1.7, 1.7), S('drill', 1.0), S('layers', '*.Cu', '*.Mask'), S('remove_unused_layers', Sym('no'))))
    x0, x1b, y0, y1b = min(xs) - 1.27, max(xs) + 1.27, min(ys) - 1.27, max(ys) + 1.27
    fp.append(fp_rect(x0, y0, x1b, y1b, 'F.Fab', 0.1))
    fp.append(fp_rect(x0 - 0.5, y0 - 0.5, x1b + 0.5, y1b + 0.5, 'F.CrtYd', 0.05))
    fp.append(fp_rect(x0 - 0.12, y0 - 0.12, x1b + 0.12, y1b + 0.12, 'F.SilkS', 0.12))
    fp.append(fp_line(1.6, 1.6, 1.6, -0.2, 'F.SilkS', 0.15))
    fp.append(fp_text('Pi pin 1', 4.0, 2.8, 'F.SilkS', 0.8))
    # KiCad's 2x20 socket model (8.5 mm), turned so its odd pins run along x as here, and scaled to the 6.0 mm body;
    # then the Pi itself, components down, from tools/vrml.py
    fp.append(model(KMODELS + 'Connector_PinSocket_2.54mm.3dshapes/PinSocket_2x20_P2.54mm_Vertical.step',
                    scale=(1, 1, round(D.SOCKET_H / 8.5, 4)), rotate=(0, 0, 90)))
    fp.append(model(MODELS + 'Aurora68_RPi3Aplus_ComponentsDown.wrl'))
    return fp


def npth_hole(name, d, keep, descr, mdl=None):
    fp = fp_header(name, descr, 'mounting hole', 'exclude_from_pos_files exclude_from_bom')
    if mdl:
        fp.append(mdl)
    fp.append(S('pad', '', Sym('np_thru_hole'), Sym('circle'), S('at', 0, 0), S('size', d, d), S('drill', d),
                S('layers', '*.Cu', '*.Mask')))
    fp.append(fp_circle(0, 0, keep / 2.0, 'F.CrtYd'))
    fp.append(fp_circle(0, 0, keep / 2.0, 'F.Fab', 0.1))
    return fp


MODELS = '${KIPRJMOD}/lib/aurora68.3dshapes/'
KMODELS = '${KICAD8_3DMODEL_DIR}/'


def model(path, offset=(0, 0, 0), scale=(1, 1, 1), rotate=(0, 0, 0)):
    return S('model', path, S('offset', S('xyz', *offset)), S('scale', S('xyz', *scale)), S('rotate', S('xyz', *rotate)))


def write_models():
    """Simple VRML models made here (KiCad's VRML unit is 0.1 inch): the slot plug's body (KiCad has no straight
    male 3x32 type C model) and the Pi 3A+ turned components-down, attached to the Pi socket's footprint so the
    stack can be seen in KiCad's 3D viewer."""
    import vrml
    os.makedirs(os.path.join(ROOT, 'lib', 'aurora68.3dshapes'), exist_ok=True)
    vrml.write_plug(os.path.join(ROOT, 'lib', 'aurora68.3dshapes', 'Aurora68_DIN41612_C_3x32_Male_Straight.wrl'))
    vrml.write_pi3aplus(os.path.join(ROOT, 'lib', 'aurora68.3dshapes', 'Aurora68_RPi3Aplus_ComponentsDown.wrl'))


def write_footprints():
    out = {
        'Aurora68_DIN41612_C_3x32_Male_Straight_PressFit_Bottom': din_plug(board_side=False),
        'Aurora68_PiSocket_2x20_Inverted': pi_socket(),
        'Aurora68_AppleCardSupport_3.86mm': npth_hole('Aurora68_AppleCardSupport_3.86mm', D.APPLE_HOLE_D, 8.0,
                                                      "Hole for the Macintosh SE logic board's expansion card supports, 3.86 mm "
                                                      "(Designing Cards and Drivers, 3rd ed., Figures 17-1 and 17-5)"),
        'Aurora68_PiStandoff_M2.5': npth_hole('Aurora68_PiStandoff_M2.5', D.PI_HOLE_D, 5.5,
                                              'M2.5 hole for a Raspberry Pi standoff (8.5 mm), 5.5 mm keep-out for the standoff',
                                              model(KMODELS + 'Mounting_Wuerth.3dshapes/Mounting_Wuerth_WA-SMSI-M2.5_H8.5mm_9774085151.step')),
    }
    for name, fp in out.items():
        sexpr.save(os.path.join(geom.PRETTY, name + '.kicad_mod'), fp)
    write_models()


# ========================================================================================================
# Schematic
# ========================================================================================================
def net_sheets(parts):
    where = {}
    for p in parts:
        for pin, net in p.nets.items():
            if net:
                where.setdefault(net, set()).add(p.sheet)
    return where


POWER_NETS = {'GND', '+3V3', '5V_PI', '+5V_SLOT', '5V_F', '5V_FAN', '5V_PANEL'}


def place_symbols(parts):
    """Positions on each sheet: columns of symbols, labels allowed for on both sides."""
    pos = {}
    sheets = {}
    for p in parts:
        sheets.setdefault(p.sheet, []).append(p)
    paper = {}
    for sh, plist in sheets.items():
        order = sorted(plist, key=lambda p: (-len(symbol_pins(p.sym)), p.ref))
        x, y, colw, maxh = 40.0, 40.0, 0.0, 0.0
        limit_h = 230.0 if sh != 'cpld' else 360.0
        for p in order:
            ly, ry, hw, top, bottom, plen = symbol_geometry(p.sym)
            labl = max([len(p.nets.get(q[0][0]) or '') for q in ly] + [0]) * 1.1 + 8
            labr = max([len(p.nets.get(q[0][0]) or '') for q in ry] + [0]) * 1.1 + 8
            w = labl + 2 * (hw + plen) + labr
            h = (top - bottom) + 15.24
            if y + h > limit_h and y > 40.0:
                x += colw + 10.0; y = 40.0; colw = 0.0
            cx = math.ceil((x + labl + hw + plen) / GRID) * GRID
            cy = math.ceil((y + top + 7.62) / GRID) * GRID
            pos[p.ref] = (cx, cy)
            y += h
            colw = max(colw, w)
            maxh = max(maxh, y)
        paper[sh] = 'A2' if sh == 'cpld' else 'A3'
    return pos, paper


def sheet_file(sh):
    return '%s.kicad_sch' % sh


def write_schematics(parts):
    where = net_sheets(parts)
    pos, paper = place_symbols(parts)
    root_uuid = uid('root')
    sheet_uuid = {sh: uid('sheet', sh) for sh, _, _ in D.SHEETS}
    used_syms = sorted(set(p.sym for p in parts) | {'PWR_FLAG'})

    def lib_symbols(names):
        ls = S('lib_symbols')
        for n in names:
            ls.append(make_symbol(n, 'aurora68:' + n))
        return ls

    flags_done = set()
    for idx, (sh, title, what) in enumerate(D.SHEETS, start=2):
        items = []
        plist = [p for p in parts if p.sheet == sh]
        names = sorted(set(p.sym for p in plist) | {'PWR_FLAG'})
        for p in plist:
            cx, cy = pos[p.ref]
            ly, ry, hw, top, bottom, plen = symbol_geometry(p.sym)
            su = uid('sym', p.ref)
            fpname = 'aurora68:' + p.fp
            sym = S('symbol', S('lib_id', 'aurora68:' + p.sym), S('at', cx, cy, 0), S('unit', 1),
                    S('exclude_from_sim', Sym('no')), S('in_bom', Sym('no' if p.sym == 'MountingHole' else 'yes')),
                    S('on_board', Sym('yes')), S('dnp', Sym('no')), S('uuid', su),
                    S('property', 'Reference', p.ref, S('at', cx, cy - top - 3.81, 0), effects()),
                    S('property', 'Value', p.value, S('at', cx, cy - bottom + 3.81, 0), effects()),
                    S('property', 'Footprint', fpname, S('at', cx, cy - bottom + 6.35, 0), effects(hide=True)),
                    S('property', 'Datasheet', D.SYMBOLS[p.sym][3], S('at', cx, cy, 0), effects(hide=True)),
                    S('property', 'Description', D.SYMBOLS[p.sym][2], S('at', cx, cy, 0), effects(hide=True)))
            for k, v in (('MPN', p.mpn), ('LCSC', p.lcsc), ('Height_mm', '%.2f' % p.height), ('Note', p.note)):
                if v:
                    sym.append(S('property', k, v, S('at', cx, cy, 0), effects(hide=True)))
            for (q, y) in ly + ry:
                sym.append(S('pin', q[0], S('uuid', uid('pin', p.ref, q[0]))))
            sym.append(S('instances', S('project', PROJECT, S('path', '/%s/%s' % (root_uuid, sheet_uuid[sh]),
                                                              S('reference', p.ref), S('unit', 1)))))
            items.append(sym)
            for side, plist2 in ((-1, ly), (1, ry)):
                for (q, y) in plist2:
                    px = cx + side * (hw + plen)
                    py = cy - y
                    net = p.nets.get(q[0])
                    if not net:
                        items.append(S('no_connect', S('at', px, py), S('uuid', uid('nc', p.ref, q[0]))))
                        continue
                    ex = px + side * GRID
                    items.append(S('wire', S('pts', S('xy', px, py), S('xy', ex, py)),
                                   S('stroke', S('width', 0), S('type', Sym('default'))), S('uuid', uid('w', p.ref, q[0]))))
                    ang = 0 if side > 0 else 180
                    just = 'left' if side > 0 else 'right'
                    if len(where[net]) > 1 or net in POWER_NETS:
                        items.append(S('global_label', net, S('shape', Sym('passive' if net in POWER_NETS else 'bidirectional')),
                                       S('at', ex, py, ang), S('fields_autoplaced', Sym('yes')), effects(justify=just),
                                       S('uuid', uid('gl', p.ref, q[0])),
                                       S('property', 'Intersheetrefs', '${INTERSHEET_REFS}', S('at', ex, py, 0),
                                         effects(justify=just, hide=True))))
                    else:
                        items.append(S('label', net, S('at', ex, py, ang), S('fields_autoplaced', Sym('yes')),
                                       effects(justify=just + ' bottom'), S('uuid', uid('l', p.ref, q[0]))))
        # PWR_FLAGs on the nets fed only by passive pins (fuses, the plug), so that ERC sees them driven
        flag_nets = [n for n in ('GND', '+5V_SLOT', '5V_F', '5V_FAN', '5V_PANEL') if sh in where.get(n, ()) and n not in flags_done]
        fx, fy = 20.0, 270.0 if paper[sh] == 'A3' else 400.0
        for i, n in enumerate(flag_nets):
            flags_done.add(n)
            x = fx + i * 30.0
            ref = '#FLG%02d' % (len(flags_done))
            items.append(S('symbol', S('lib_id', 'aurora68:PWR_FLAG'), S('at', x, fy, 0), S('unit', 1),
                           S('exclude_from_sim', Sym('no')), S('in_bom', Sym('no')), S('on_board', Sym('no')), S('dnp', Sym('no')),
                           S('uuid', uid('flag', n)),
                           S('property', 'Reference', ref, S('at', x, fy - 5.08, 0), effects(hide=True)),
                           S('property', 'Value', 'PWR_FLAG', S('at', x, fy - 3.81, 0), effects()),
                           S('property', 'Footprint', '', S('at', x, fy, 0), effects(hide=True)),
                           S('property', 'Datasheet', '', S('at', x, fy, 0), effects(hide=True)),
                           S('property', 'Description', '', S('at', x, fy, 0), effects(hide=True)),
                           S('pin', '1', S('uuid', uid('flagpin', n))),
                           S('instances', S('project', PROJECT, S('path', '/%s/%s' % (root_uuid, sheet_uuid[sh]),
                                                                  S('reference', ref), S('unit', 1))))))
            items.append(S('wire', S('pts', S('xy', x, fy), S('xy', x, fy + GRID)),
                           S('stroke', S('width', 0), S('type', Sym('default'))), S('uuid', uid('flagw', n))))
            items.append(S('global_label', n, S('shape', Sym('passive')), S('at', x, fy + GRID, 270),
                           S('fields_autoplaced', Sym('yes')), effects(justify='right'), S('uuid', uid('flagl', n)),
                           S('property', 'Intersheetrefs', '${INTERSHEET_REFS}', S('at', x, fy + GRID, 0),
                             effects(justify='right', hide=True))))
        items.append(S('text', 'Card P, %s: %s. Generated by tools/gen_kicad.py from tools/design.py; edit those, not this file.'
                       % (title, what), S('exclude_from_sim', Sym('no')), S('at', 20.0, 20.0, 0), effects(2.0, justify='left bottom'),
                       S('uuid', uid('text', sh))))
        sch = S('kicad_sch', S('version', 20231120), S('generator', 'eeschema'), S('generator_version', '8.0'),
                S('uuid', sheet_uuid[sh]), S('paper', paper[sh]),
                S('title_block', S('title', 'Aurora68 Card P: %s' % title), S('date', DATE), S('rev', REV),
                  S('company', 'Aurora68'), S('comment', 1, 'Draft for review: not checked by ERC/DRC in KiCad, not built'),
                  S('comment', 2, 'Macintosh SE processor direct slot card for a Raspberry Pi 3A+ (README.md)')),
                lib_symbols(names))
        sch += items
        sexpr.save(os.path.join(ROOT, sheet_file(sh)), sch)

    # the root sheet: the five sheets, and a note
    root = S('kicad_sch', S('version', 20231120), S('generator', 'eeschema'), S('generator_version', '8.0'),
             S('uuid', root_uuid), S('paper', 'A3'),
             S('title_block', S('title', 'Aurora68 Card P (Macintosh SE PDS, Raspberry Pi 3A+)'), S('date', DATE), S('rev', REV),
               S('company', 'Aurora68'), S('comment', 1, 'Draft for review: not checked by ERC/DRC in KiCad, not built'),
               S('comment', 2, 'Derived in part from the PiStorm Rev B (MIT); see README.md for credits and licences')),
             S('lib_symbols'))
    for i, (sh, title, what) in enumerate(D.SHEETS):
        x, y = 30.0 + (i % 3) * 120.0, 50.0 + (i // 3) * 70.0
        root.append(S('sheet', S('at', x, y), S('size', 100.0, 45.0), S('fields_autoplaced', Sym('yes')),
                      S('stroke', S('width', 0.1524), S('type', Sym('solid'))), S('fill', S('color', 0, 0, 0, 0.0)),
                      S('uuid', sheet_uuid[sh]),
                      S('property', 'Sheetname', title, S('at', x, y - 0.7, 0), effects(justify='left bottom')),
                      S('property', 'Sheetfile', sheet_file(sh), S('at', x, y + 45.6, 0), effects(justify='left top')),
                      S('instances', S('project', PROJECT, S('path', '/' + root_uuid, S('page', str(i + 2)))))))
        root.append(S('text', what, S('exclude_from_sim', Sym('no')), S('at', x + 3, y + 22, 0), effects(1.5, justify='left bottom'),
                      S('uuid', uid('roottext', sh))))
    root.append(S('text', 'Connectivity between sheets is by global labels. The slot plug J1 is on the card\'s underside; its pin '
                  'names come from se-pds-pinout.csv (Apple, Designing Cards and Drivers, 3rd ed., Table 13-1); '
                  'tools/check_pinout.py checks every pin.', S('exclude_from_sim', Sym('no')), S('at', 30.0, 200.0, 0),
                  effects(1.5, justify='left bottom'), S('uuid', uid('roottext', 'note'))))
    root.append(S('sheet_instances', S('path', '/', S('page', '1'))))
    sexpr.save(os.path.join(ROOT, PROJECT + '.kicad_sch'), root)
    return root_uuid, sheet_uuid


# ========================================================================================================
# Board
# ========================================================================================================
LAYERS = [(0, 'F.Cu', 'signal'), (31, 'B.Cu', 'signal'), (32, 'B.Adhes', 'user', 'B.Adhesive'),
          (33, 'F.Adhes', 'user', 'F.Adhesive'), (34, 'B.Paste', 'user'), (35, 'F.Paste', 'user'),
          (36, 'B.SilkS', 'user', 'B.Silkscreen'), (37, 'F.SilkS', 'user', 'F.Silkscreen'), (38, 'B.Mask', 'user'),
          (39, 'F.Mask', 'user'), (40, 'Dwgs.User', 'user', 'User.Drawings'), (41, 'Cmts.User', 'user', 'User.Comments'),
          (42, 'Eco1.User', 'user', 'User.Eco1'), (43, 'Eco2.User', 'user', 'User.Eco2'), (44, 'Edge.Cuts', 'user'),
          (45, 'Margin', 'user'), (46, 'B.CrtYd', 'user', 'B.Courtyard'), (47, 'F.CrtYd', 'user', 'F.Courtyard'),
          (48, 'B.Fab', 'user'), (49, 'F.Fab', 'user')]


def B(x, y):
    return round(x + OX, 4), round(y + OY, 4)


def net_table(parts):
    nets = sorted({n for p in parts for n in p.nets.values() if n})
    nets.sort(key=lambda n: (n != 'GND', n))
    return {n: i + 1 for i, n in enumerate(nets)}


def place_footprint(p, netno, path):
    """A board footprint for part p: the library footprint, placed, nets on its pads."""
    if p.fp.startswith('Aurora68_DIN41612'):
        fp = din_plug(board_side=True)
    else:
        fp = geom.load_fp(p.fp)
    X, Y = B(p.x, p.y)
    out = S('footprint', 'aurora68:' + p.fp, S('layer', 'B.Cu' if p.side == 'B' else 'F.Cu'), S('uuid', uid('fp', p.ref)),
            S('at', X, Y, p.rot) if p.rot else S('at', X, Y))
    pin_type = {q[0]: q[2] for q in symbol_pins(p.sym)} if D.SYMBOLS[p.sym][0] or p.sym == 'SE_PDS_96' else {}
    for c in fp[2:]:
        if not isinstance(c, list):
            continue
        k = c[0]
        if k in ('version', 'generator', 'generator_version', 'layer'):
            continue
        if k == 'property':
            c = [x for x in c]
            if c[1] == 'Reference':
                c[2] = p.ref
            elif c[1] == 'Value':
                c[2] = p.value
            elif c[1] == 'Footprint':
                c[2] = 'aurora68:' + p.fp
            at = sexpr.find(c, 'at')
            if at and p.rot:
                at[:] = S('at', at[1], at[2], (at[3] if len(at) > 3 else 0) + p.rot)
            c.append(S('uuid', uid('prop', p.ref, c[1])))
            out.append(c)
            continue
        if k == 'descr':
            out.append(c)
            out.append(S('path', path))
            continue
        if k == 'pad':
            c = [x for x in c]
            at = sexpr.find(c, 'at')
            ang = (at[3] if len(at) > 3 else 0) + p.rot
            at[:] = S('at', at[1], at[2], ang) if ang else S('at', at[1], at[2])
            num = str(c[1])
            net = p.nets.get(num)
            if num == '' or c[2] == 'np_thru_hole':
                pass
            elif net:
                c.append(S('net', netno[net], net))
                c.append(S('pinfunction', num))
                c.append(S('pintype', pin_type.get(num, 'passive')))
            c.append(S('uuid', uid('pad', p.ref, num, at[1], at[2])))
            out.append(c)
            continue
        if k == 'fp_text':
            c = [x for x in c]
            at = sexpr.find(c, 'at')
            if p.rot:
                at[:] = S('at', at[1], at[2], (at[3] if len(at) > 3 else 0) + p.rot)
            out.append(c)
            continue
        out.append(c)
    if p.ref == 'J1':
        pass
    return out


def gr_poly(pts, layer, width=0.1, fill=False):
    return S('gr_poly', S('pts', *[S('xy', *B(x, y)) for x, y in pts]),
             S('stroke', S('width', width), S('type', Sym('solid'))), S('fill', Sym('solid' if fill else 'none')),
             S('layer', layer), S('uuid', uid('poly', layer, pts[0], pts[1])))


def gr_rect(x0, y0, x1, y1, layer, width=0.15, key=''):
    return S('gr_rect', S('start', *B(x0, y0)), S('end', *B(x1, y1)), S('stroke', S('width', width), S('type', Sym('solid'))),
             S('fill', Sym('none')), S('layer', layer), S('uuid', uid('rect', layer, x0, y0, key)))


def gr_text(text, x, y, layer, size=1.2, angle=0):
    return S('gr_text', text, S('at', *B(x, y), angle), S('layer', layer), S('uuid', uid('text', layer, text, x, y)),
             S('effects', font(size, 0.15), S('justify', Sym('left'))))


def rule_area(name, pts, layers='F&B.Cu', footprints=True, tracks=True, vias=True, pads=True, pour=True):
    def a(v):
        return Sym('allowed' if v else 'not_allowed')
    return S('zone', S('net', 0), S('net_name', ''), S('layers', layers), S('uuid', uid('rule', name, pts[0])), S('name', name),
             S('hatch', Sym('edge'), 0.5), S('connect_pads', S('clearance', 0)), S('min_thickness', 0.25),
             S('filled_areas_thickness', Sym('no')),
             S('keepout', S('tracks', a(tracks)), S('vias', a(vias)), S('pads', a(pads)), S('copperpour', a(pour)),
               S('footprints', a(not footprints))),
             S('fill', S('thermal_gap', 0.5), S('thermal_bridge_width', 0.5)),
             S('polygon', S('pts', *[S('xy', *B(x, y)) for x, y in pts])))


def rect_pts(x0, y0, x1, y1):
    return [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]


def pi_under_areas():
    """Rule areas (no footprints) under the 3A+'s parts that hang towards the card."""
    out = []
    for name, u0, u1, v0, v1, h in D.PI_TOP_PARTS:
        if name == 'header':
            continue
        xa, ya = D.pi_xy(u0, v0)
        xb, yb = D.pi_xy(u1, v1)
        out.append(('PI_' + name.split()[0].upper().replace('-', ''), rect_pts(min(xa, xb), min(ya, yb), max(xa, xb), max(ya, yb))))
    return out


def write_board(parts, root_uuid, sheet_uuid):
    netno = net_table(parts)
    pcb = S('kicad_pcb', S('version', 20240108), S('generator', 'pcbnew'), S('generator_version', '8.0'),
            S('general', S('thickness', D.APPLE_THICKNESS), S('legacy_teardrops', Sym('no'))), S('paper', 'A3'),
            S('title_block', S('title', 'Aurora68 Card P (Macintosh SE PDS, Raspberry Pi 3A+)'), S('date', DATE), S('rev', REV),
              S('company', 'Aurora68'), S('comment', 1, 'Draft for review: DRC not run in KiCad; not built'),
              S('comment', 2, '2 layers, 1.6 mm FR-4, HASL; 0.15 mm track/space minimum, 0.3 mm drills minimum')))
    layers = S('layers')
    for l in LAYERS:
        layers.append([l[0], l[1], Sym(l[2])] + ([l[3]] if len(l) > 3 else []))
    pcb.append(layers)
    pcb.append(S('setup', S('pad_to_mask_clearance', 0), S('allow_soldermask_bridges_in_footprints', Sym('no')),
                 S('aux_axis_origin', *B(0, 0)), S('grid_origin', *B(0, 0))))
    pcb.append(S('net', 0, ''))
    for n, i in sorted(netno.items(), key=lambda kv: kv[1]):
        pcb.append(S('net', i, n))
    for p in parts:
        path = '/%s/%s' % (sheet_uuid[p.sheet], uid('sym', p.ref))
        pcb.append(place_footprint(p, netno, path))
    # outline
    pts = D.card_outline()
    for i in range(len(pts)):
        a, b = pts[i], pts[(i + 1) % len(pts)]
        pcb.append(S('gr_line', S('start', *B(*a)), S('end', *B(*b)), S('stroke', S('width', 0.1), S('type', Sym('default'))),
                     S('layer', 'Edge.Cuts'), S('uuid', uid('edge', i))))
    # Apple's maximum outline and rear strip, for reference, on User.Drawings
    apple = [(0, 0), (D.APPLE_W, 0), (D.APPLE_W, D.APPLE_H), (D.APPLE_TAB_X, D.APPLE_H), (D.APPLE_TAB_X, D.APPLE_BODY_H),
             (0, D.APPLE_BODY_H)]
    pcb.append(gr_poly(apple, 'Dwgs.User', 0.1))
    pcb.append(gr_rect(0, 0, D.APPLE_W, D.APPLE_REAR_STRIP, 'Dwgs.User', 0.1, 'strip'))
    pcb.append(gr_text("Apple's maximum outline (Fig. 17-1); hatched strip 0-16 mm: no components (cut off on this card)",
                       2, -2, 'Dwgs.User', 1.5))
    # height contours every 2 mm (User.Comments)
    for h in range(6, 16, 2):
        y = (h - D.H_REAR) / (D.H_FRONT - D.H_REAR) * D.APPLE_BODY_H
        pcb.append(S('gr_line', S('start', *B(0, y)), S('end', *B(D.APPLE_W, y)), S('stroke', S('width', 0.05), S('type', Sym('dash'))),
                     S('layer', 'Cmts.User'), S('uuid', uid('hline', h))))
        pcb.append(gr_text('max %d mm' % h, 0.5, y - 0.4, 'Cmts.User', 1.0))
    # the Pi 3A+, the Zero 2 W and the 3B extension on the silkscreen
    xa, ya = D.pi_xy(0, 0); xb, yb = D.pi_xy(65, 56)
    xa, xb, ya, yb = min(xa, xb), max(xa, xb), min(ya, yb), max(ya, yb)
    pcb.append(gr_rect(xa, ya, xb, yb, 'F.SilkS', 0.15, 'pi3ap'))
    pcb.append(gr_text('Raspberry Pi 3A+ here, components DOWN', xa + 2, ya + 3, 'F.SilkS', 1.2))
    pcb.append(gr_text('(Zero 2 W: the front 30 mm)', xa + 2, ya + 5, 'F.SilkS', 1.0))
    xz, yz = D.pi_xy(0, 26)
    pcb.append(gr_rect(xa, yz, xb, yb, 'F.Fab', 0.1, 'z2w'))
    for (x0, y0, x1, y1, name, what) in D.keepouts():
        if name == 'PI3B_EXT':
            pcb.append(gr_rect(x0, y0, x1, y1, 'F.SilkS', 0.15, 'pi3b'))
            pcb.append(gr_text('Pi 3B/3B+ USB/Ethernet and their plugs:', x0 + 1, y0 + 4, 'F.SilkS', 1.0))
            pcb.append(gr_text('keep clear, no parts (3B/3B+ bench use only)', x0 + 1, y0 + 6, 'F.SilkS', 1.0))
            # no footprints or pads anywhere in it; tracks, vias and the pour may pass (vias: see PI3B_PORTS)
            pcb.append(rule_area(name, rect_pts(x0, y0, x1, y1), footprints=True, pads=False))
        elif name == 'PI3B_PORTS':
            pcb.append(rule_area(name, rect_pts(x0, y0, x1, y1), footprints=False, vias=False))   # no vias under the ports
        elif name == 'APPLE_SUPPORT':
            cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
            r = (x1 - x0) / 2
            poly = [(cx + r * math.cos(math.radians(a)), cy + r * math.sin(math.radians(a))) for a in range(0, 360, 30)]
            pcb.append(rule_area(name + '_%d' % int(cy), poly, footprints=True))
    for name, pts in pi_under_areas():
        pcb.append(rule_area(name, pts, footprints=True))
    pcb.append(gr_text('FRONT edge: parts up to 15.58 mm (Apple Fig. 17-2)', 60, 112.6 - 1.2, 'Cmts.User', 1.2))
    pcb.append(gr_text('REAR: 4.61 mm at the rear edge of Apple\'s outline', 60, 23.0, 'Cmts.User', 1.2))
    pcb.append(gr_text('Aurora68 Card P  rev P0.1 draft  (Macintosh SE PDS)', 140, 24.5, 'F.SilkS', 1.5))
    pcb.append(gr_text('J1 on the UNDERSIDE: A1 front, row A outside', 150, 113.0, 'F.SilkS', 1.0))
    pcb.append(gr_text('FRONT', 100, 108.2, 'F.SilkS', 1.5))
    pcb.append(gr_text('REAR', 100, 23.5, 'F.SilkS', 1.5))
    # ground: both layers
    zone_pts = [(1.0, D.CARD_REAR_Y + 1.0), (D.APPLE_W - 1.0, D.CARD_REAR_Y + 1.0), (D.APPLE_W - 1.0, D.APPLE_H - 1.0),
                (D.APPLE_TAB_X + 1.0, D.APPLE_H - 1.0), (D.APPLE_TAB_X + 1.0, D.APPLE_BODY_H - 1.0), (1.0, D.APPLE_BODY_H - 1.0)]
    for layer in ('B.Cu', 'F.Cu'):
        pcb.append(S('zone', S('net', netno['GND']), S('net_name', 'GND'), S('layer', layer), S('uuid', uid('gndzone', layer)),
                     S('name', 'GND_' + layer[0]), S('hatch', Sym('edge'), 0.5), S('priority', 0 if layer == 'F.Cu' else 1),
                     S('connect_pads', S('clearance', 0.3)), S('min_thickness', 0.25), S('filled_areas_thickness', Sym('no')),
                     S('fill', S('thermal_gap', 0.5), S('thermal_bridge_width', 0.5)),
                     S('polygon', S('pts', *[S('xy', *B(x, y)) for x, y in zone_pts]))))
    sexpr.save(os.path.join(ROOT, PROJECT + '.kicad_pcb'), pcb)
    return netno


# ========================================================================================================
# Project, tables, BOM, the CPLD's pin file
# ========================================================================================================
def write_project():
    pro = {
        "board": {"design_settings": {
            "defaults": {"board_outline_line_width": 0.1, "copper_line_width": 0.2, "silk_line_width": 0.15},
            "rules": {"min_clearance": 0.15, "min_track_width": 0.15, "min_via_diameter": 0.6, "min_through_hole_diameter": 0.3,
                      "min_hole_to_hole": 0.25, "min_copper_edge_clearance": 0.3, "min_silk_clearance": 0.0,
                      "allow_blind_buried_vias": False, "allow_microvias": False},
            "track_widths": [0.0, 0.2, 0.3, 0.5, 0.8, 1.0],
            "via_dimensions": [{"diameter": 0.0, "drill": 0.0}, {"diameter": 0.6, "drill": 0.3}, {"diameter": 0.8, "drill": 0.4}]},
            "layer_presets": [], "viewports": []},
        "boards": [],
        "libraries": {"pinned_footprint_libs": [], "pinned_symbol_libs": []},
        "meta": {"filename": PROJECT + ".kicad_pro", "version": 1},
        "net_settings": {
            "classes": [
                {"name": "Default", "clearance": 0.15, "track_width": 0.15, "via_diameter": 0.6, "via_drill": 0.3,
                 "diff_pair_gap": 0.25, "diff_pair_width": 0.2, "wire_width": 6, "bus_width": 12, "line_style": 0,
                 "microvia_diameter": 0.3, "microvia_drill": 0.1, "pcb_color": "rgba(0, 0, 0, 0.000)",
                 "schematic_color": "rgba(0, 0, 0, 0.000)"},
                {"name": "Supply", "clearance": 0.15, "track_width": 0.25, "via_diameter": 0.6, "via_drill": 0.3,
                 "diff_pair_gap": 0.25, "diff_pair_width": 0.2, "wire_width": 6, "bus_width": 12, "line_style": 0,
                 "microvia_diameter": 0.3, "microvia_drill": 0.1, "pcb_color": "rgba(0, 0, 0, 0.000)",
                 "schematic_color": "rgba(0, 0, 0, 0.000)"},
                {"name": "Power", "clearance": 0.2, "track_width": 0.6, "via_diameter": 0.6, "via_drill": 0.3,
                 "diff_pair_gap": 0.25, "diff_pair_width": 0.2, "wire_width": 6, "bus_width": 12, "line_style": 0,
                 "microvia_diameter": 0.3, "microvia_drill": 0.1, "pcb_color": "rgba(0, 0, 0, 0.000)",
                 "schematic_color": "rgba(0, 0, 0, 0.000)"}],
            "meta": {"version": 3},
            "netclass_patterns": [{"netclass": "Supply", "pattern": p} for p in ("GND", "+3V3")] +
                                 [{"netclass": "Power", "pattern": p} for p in
                                  ("5V_PI", "+5V_SLOT", "5V_F", "5V_FAN", "5V_PANEL", "FAN_GND_SW")]},
        "pcbnew": {"page_layout_descr_file": ""},
        "schematic": {"legacy_lib_dir": "", "legacy_lib_list": []},
        "sheets": [[uid('root'), "Root"]] + [[uid('sheet', sh), title] for sh, title, _ in D.SHEETS],
        "text_variables": {}}
    with open(os.path.join(ROOT, PROJECT + '.kicad_pro'), 'w') as f:
        json.dump(pro, f, indent=2)
        f.write('\n')
    with open(os.path.join(ROOT, 'sym-lib-table'), 'w') as f:
        f.write('(sym_lib_table\n\t(version 7)\n\t(lib (name "aurora68")(type "KiCad")(uri "${KIPRJMOD}/lib/aurora68.kicad_sym")'
                '(options "")(descr "Aurora68 Card P symbols"))\n)\n')
    with open(os.path.join(ROOT, 'fp-lib-table'), 'w') as f:
        f.write('(fp_lib_table\n\t(version 7)\n\t(lib (name "aurora68")(type "KiCad")(uri "${KIPRJMOD}/lib/aurora68.pretty")'
                '(options "")(descr "Aurora68 Card P footprints"))\n)\n')


def write_bom(parts):
    groups = {}
    for p in parts:
        key = (p.value, p.fp, p.mpn, p.lcsc)
        groups.setdefault(key, []).append(p)
    def refkey(r):
        import re
        m = re.match(r'([A-Z#]+)(\d+)', r)
        return (m.group(1), int(m.group(2))) if m else (r, 0)
    rows = []
    for (value, fp, mpn, lcsc), ps in groups.items():
        refs = sorted((p.ref for p in ps), key=refkey)
        kind = 'none (drilled hole)' if ps[0].sym == 'MountingHole' else \
            'through-hole' if any(s in fp for s in ('PinHeader', 'PiSocket', 'DIN41612')) else 'SMD (top)'
        rows.append({'References': ' '.join(refs), 'Qty': len(refs), 'Value': value, 'Footprint': fp,
                     'Description': D.SYMBOLS[ps[0].sym][2], 'MPN / requirement': mpn, 'LCSC': lcsc,
                     'Assembly': kind, 'Height_mm': '%.2f' % ps[0].height,
                     'Note': '; '.join(sorted(set(p.note for p in ps if p.note)))})
    rows.sort(key=lambda r: refkey(r['References'].split()[0]))
    with open(os.path.join(ROOT, 'bom.csv'), 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        for r in rows:
            w.writerow(r)
    return rows


def write_qsf():
    lines = ['# SPDX-License-Identifier: MIT',
             '# aurora68_pds.qsf - pin assignments for Card P\'s CPLD, written by tools/gen_kicad.py from tools/design.py',
             '# (the same table the schematic is drawn from). Every signal the classic PiStorm has is on the PiStorm\'s',
             '# own pin (reference/pistorm/rtl/pistorm.qsf); the slot card\'s new signals are on pins the PiStorm leaves free.',
             '# The footprint is wired like the PiStorm\'s for an EPM570 (pins 37, 39, 88, 90 power), which an EPM240',
             '# also fits (those pins are I/O on the EPM240 and are left unused): try the EPM240 first.',
             'set_global_assignment -name FAMILY "MAX II"',
             'set_global_assignment -name DEVICE EPM240T100C5',
             'set_global_assignment -name TOP_LEVEL_ENTITY aurora68_pds',
             'set_global_assignment -name VERILOG_FILE aurora68_pds.v',
             'set_global_assignment -name SDC_FILE aurora68_pds.sdc',
             'set_global_assignment -name RESERVE_ALL_UNUSED_PINS "AS INPUT TRI-STATED WITH WEAK PULL-UP"',
             'set_global_assignment -name RESERVE_PIN "AS INPUT TRI-STATED" -to 37',
             'set_global_assignment -name STRATIX_DEVICE_IO_STANDARD "3.3-V LVTTL"',
             'set_global_assignment -name POWER_UP_LEVEL "AS SPECIFIED IN DESIGN"', '']
    for port, pin in sorted(D.CPLD_PINS.items(), key=lambda kv: kv[1]):
        note = D.PISTORM_PIN_NOTES.get(pin, '')
        lines.append('set_location_assignment PIN_%d -to %s%s' % (pin, port, ('    # ' + note) if note else ''))
    lines.append('set_global_assignment -name USE_CONFIGURATION_DEVICE OFF')
    with open(os.path.join(ROOT, 'rtl', 'aurora68_pds.qsf'), 'w') as f:
        f.write('\n'.join(lines) + '\n')
    with open(os.path.join(ROOT, 'rtl', 'aurora68_pds.sdc'), 'w') as f:
        f.write('# SPDX-License-Identifier: MIT\n# The Pi\'s GPCLK0, 200 MHz; every slot input is synchronised to it.\n'
                'create_clock -name PI_CLK -period 5.000 [get_ports PI_CLK]\n'
                'set_false_path -from [get_ports {M68K_* SLOT_PWR FAN_TACH STOCK_SW_n PI_RD PI_WR PI_A*}]\n')


def main():
    parts = D.build()
    write_footprints()
    write_symbol_lib(sorted(set(p.sym for p in parts) | {'PWR_FLAG'}))
    root_uuid, sheet_uuid = write_schematics(parts)
    write_board(parts, root_uuid, sheet_uuid)
    write_project()
    rows = write_bom(parts)
    write_qsf()
    print('%d parts, %d BOM lines, %d nets' % (len(parts), len(rows), len(net_table(parts))))


if __name__ == '__main__':
    main()
