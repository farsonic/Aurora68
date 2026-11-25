# SPDX-License-Identifier: MPL-2.0
"""check_pinout.py - the card's slot plug against Apple's pinout, pin by pin.

se-pds-pinout.csv is the single source for the SE's 96-pin slot (Designing Cards and Drivers for the Macintosh
Family, 3rd ed., Figure 13-1 and Table 13-1). This checks, for all 96 pins:
  1. the CSV against the table in docs/22 section 3 (the same pinout, transcribed separately on 24 November 2026);
  2. the schematic's J1 (slot.kicad_sch, read back from the file): the pin's name is the CSV's signal, and its net is
     the card's net for that signal, or nothing for the pins the card leaves alone (reserved, spare, unused rails);
  3. the board's J1 pad: the same net, and the pad where Apple's Figure 17-1 puts that pin (row B 200.66 mm right
     of the card-support holes, the plug's centre 14.61 mm above the lower hole, row A towards the logic board's
     edge, pin 1 towards the front).
Exit status 0 when everything matches.
"""

import csv
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import design as D
import netlist

ROOT = D.ROOT
REPO = os.path.dirname(os.path.dirname(ROOT))
UNUSED = {'+12V', '-5V', '-12V', 'Reserved', 'Spare', 'C16M', '/HALT', '/EXT.DTK', '/PMCYC'}   # /PMCYC: not needed (docs/22 section 10)


def docs22_table():
    path = os.path.join(REPO, 'docs', '22-pds-card.md')
    out = {}
    if not os.path.exists(path):
        return None
    for line in open(path, encoding='utf-8'):
        m = re.match(r'\|\s*(\d+)\s*\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|\s*$', line)
        if m and 1 <= int(m.group(1)) <= 32:
            pin = int(m.group(1))
            for row, val in zip('CBA', m.groups()[1:]):
                out[(row, pin)] = val.strip()
    return out if len(out) == 96 else None


def norm(s):
    s = s.strip().replace('Ground', 'GND').replace('Ext.DTK/', '/EXT.DTK').replace('R/W/', 'R/W')  # docs/22 keeps Adams's R/W/
    if s.endswith('/') and not s.startswith('/'):
        s = '/' + s[:-1]
    return s.upper()


def main():
    fails, checks = [], 0
    rows = list(csv.DictReader(open(os.path.join(ROOT, 'se-pds-pinout.csv'), newline='')))
    if len(rows) != 96 or len({(r['row'], r['pin']) for r in rows}) != 96:
        fails.append('the CSV does not have 96 distinct pins')
    t22 = docs22_table()
    if t22 is None:
        print('note: docs/22 table not found or incomplete; step 1 skipped')
    else:
        for r in rows:
            checks += 1
            if norm(t22[(r['row'], int(r['pin']))]) != norm(r['signal']):
                fails.append('CSV %s%s %s, docs/22 says %s' % (r['row'], r['pin'], r['signal'], t22[(r['row'], int(r['pin']))]))
    pins, nets, problems = netlist.schematic_netlist()
    pcb, pads, fps = netlist.board_netlist()
    from gen_kicad import OX, OY
    for r in rows:
        k = r['row'] + r['pin']
        checks += 3
        sch = pins.get(('J1', k))
        if not sch:
            fails.append('J1 pin %s missing from the schematic' % k)
            continue
        net, name, typ, fname = sch
        if name != r['signal']:
            fails.append('J1 pin %s is named %s in the schematic, the CSV says %s' % (k, name, r['signal']))
        want = None if r['signal'] in UNUSED else D.slot_net(r['signal'])
        if net != want:
            fails.append('J1 pin %s (%s): schematic net %s, expected %s' % (k, r['signal'], net, want))
        pad = pads.get(('J1', k))
        if not pad:
            fails.append('J1 pad %s missing from the board' % k)
            continue
        pnet, x, y = pad[0]
        if pnet != net:
            fails.append('J1 pad %s: board net %s, schematic net %s' % (k, pnet, net))
        ax, ay = D.plug_pin_xy(r['row'], int(r['pin']))
        if abs(x - OX - ax) > 0.01 or abs(y - OY - ay) > 0.01:
            fails.append('J1 pad %s at (%.2f, %.2f), Apple puts it at (%.2f, %.2f)' % (k, x - OX, y - OY, ax, ay))
    a1 = pads[('J1', 'A1')][0]; c1 = pads[('J1', 'C1')][0]; a32 = pads[('J1', 'A32')][0]
    checks += 2
    if not a1[1] > c1[1]:
        fails.append('row A is not towards the logic board\'s edge (right) of row C')
    if not a1[2] > a32[2]:
        fails.append('pin 1 is not towards the front of pin 32')
    for f in fails:
        print('FAIL', f)
    print('slot pinout: %d checks, %d failures (96 pins; CSV, docs/22, schematic, board)' % (checks, len(fails)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
