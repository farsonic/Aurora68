# SPDX-License-Identifier: MPL-2.0
"""fetch_kicad_lib.py - take the few footprints and symbols Card P uses from KiCad's official libraries.

KiCad's libraries (https://gitlab.com/kicad/libraries, CC-BY-SA 4.0 with the KiCad library exception) are
read one file at a time through GitLab's API: no clone. Their master branch is in the KiCad 9/10 format, so
the footprints are rewritten for KiCad 8 (version 20240108: the tokens KiCad 8 does not know are dropped,
fp_rect fills spelled as KiCad 8 spells them, 3D model paths pointed at KICAD8_3DMODEL_DIR) and saved into
lib/aurora68.pretty with a note of their origin in the description. Of the symbols only the pin tables are
kept (lib/kicad-pin-tables.csv: number, name, electrical type); gen_kicad.py draws its own symbol bodies.

Run once with network access; the results are committed, so regenerating the design needs no network:
    python3 tools/fetch_kicad_lib.py [CACHE_DIR]
"""

import csv
import json
import os
import sys
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sexpr
from sexpr import Sym

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SYM_PROJECT, FP_PROJECT = 21545491, 21601606      # kicad/libraries/kicad-symbols, kicad-footprints

FOOTPRINTS = [
    'Package_QFP/TQFP-100_14x14mm_P0.5mm', 'Package_SO/TSSOP-48_6.1x12.5mm_P0.5mm',
    'Package_SO/TSSOP-24_4.4x7.8mm_P0.65mm', 'Package_TO_SOT_SMD/SOT-223-3_TabPin2',
    'Package_TO_SOT_SMD/SOT-363_SC-70-6', 'Package_TO_SOT_SMD/SOT-23', 'Resistor_SMD/R_0603_1608Metric',
    'Capacitor_SMD/C_0603_1608Metric', 'Capacitor_SMD/C_0805_2012Metric', 'Fuse/Fuse_1812_4532Metric',
    'Fuse/Fuse_1206_3216Metric', 'LED_SMD/LED_0603_1608Metric', 'Diode_SMD/D_SOD-123',
    'Connector_PinHeader_2.54mm/PinHeader_1x02_P2.54mm_Vertical',
    'Connector_PinHeader_2.54mm/PinHeader_1x02_P2.54mm_Horizontal',
    'Connector_PinHeader_2.54mm/PinHeader_1x03_P2.54mm_Horizontal',
    'Connector_PinHeader_2.54mm/PinHeader_1x04_P2.54mm_Horizontal',
    'Connector_FFC-FPC/TE_1-84952-0_1x10-1MP_P1.0mm_Horizontal',
]
SYMBOLS = ['CPLD_Altera/EPM570T100', 'Connector/Raspberry_Pi_2_3', 'Power_Management/LM66100DCK']

DROP = {'embedded_fonts', 'duplicate_pad_numbers_are_jumpers', 'jumper_pad_groups', 'component_classes',
        'embedded_files', 'tenting', 'zone_layer_connections'}


def fetch(project, path, cache):
    local = os.path.join(cache, path.replace('/', '__'))
    if not os.path.exists(local):
        url = 'https://gitlab.com/api/v4/projects/%d/repository/files/%s/raw?ref=master' % (
            project, urllib.parse.quote(path, safe=''))
        with urllib.request.urlopen(url) as r:
            data = r.read()
        with open(local, 'wb') as f:
            f.write(data)
    with open(local, encoding='utf-8') as f:
        return f.read()


def to_kicad8(node):
    if not isinstance(node, list):
        return node
    out = []
    for c in node:
        if isinstance(c, list) and c and c[0] in DROP:
            continue
        out.append(to_kicad8(c))
    if out and out[0] == 'version':
        out = [Sym('version'), 20240108]
    elif out and out[0] == 'generator_version':
        out = [Sym('generator_version'), '8.0']
    elif out and out[0] == 'generator':
        out = [Sym('generator'), 'kicad-footprint-generator']
    elif out and out[0] == 'fill' and len(out) == 2 and out[1] in ('no', 'yes'):
        out = [Sym('fill'), Sym('solid' if out[1] == 'yes' else 'none')]
    elif out and out[0] == 'model' and len(out) > 1 and isinstance(out[1], str):
        p = out[1]
        for v in ('${KICAD10_3DMODEL_DIR}', '${KICAD9_3DMODEL_DIR}', '${KICAD7_3DMODEL_DIR}'):
            p = p.replace(v, '${KICAD8_3DMODEL_DIR}')
        out[1] = p
    return out


def main():
    cache = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, '.kicad-cache')
    os.makedirs(cache, exist_ok=True)
    pretty = os.path.join(ROOT, 'lib', 'aurora68.pretty')
    os.makedirs(pretty, exist_ok=True)
    for p in FOOTPRINTS:
        lib, name = p.split('/')
        t = sexpr.parse(fetch(FP_PROJECT, '%s.pretty/%s.kicad_mod' % (lib, name), cache))
        t = to_kicad8(t)
        d = sexpr.find(t, 'descr')
        note = ' [From KiCad\'s official library %s:%s, CC-BY-SA 4.0 with the KiCad library exception; ' \
               'rewritten for KiCad 8 by tools/fetch_kicad_lib.py]' % (lib, name)
        if d:
            d[1] = d[1] + note
        else:
            t.insert(2, [Sym('descr'), note.strip()])
        sexpr.save(os.path.join(pretty, name + '.kicad_mod'), t)
        print('footprint', name)
    rows = []
    for p in SYMBOLS:
        lib, name = p.split('/')
        t = sexpr.parse(fetch(SYM_PROJECT, '%s.kicad_symdir/%s.kicad_sym' % (lib, name), cache))
        pins = []
        def walk(n):
            for c in n:
                if isinstance(c, list):
                    if c and c[0] == 'pin':
                        pins.append(c)
                    else:
                        walk(c)
        walk(t)
        seen = set()
        for pin in pins:
            num = sexpr.find(pin, 'number')[1]
            if num in seen:
                continue
            seen.add(num)
            rows.append({'symbol': name, 'library': lib, 'number': num, 'name': sexpr.find(pin, 'name')[1],
                         'type': str(pin[1])})
        print('symbol', name, len(seen), 'pins')
    with open(os.path.join(ROOT, 'lib', 'kicad-pin-tables.csv'), 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=['symbol', 'library', 'number', 'name', 'type'])
        w.writeheader()
        for r in rows:
            w.writerow(r)


if __name__ == '__main__':
    main()
