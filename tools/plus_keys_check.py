#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""plus_keys_check.py - did every key of the Plus keyboard reach the Mac, in order?

    tools/plus_keys_check.py MAME_LOG EMU68_LOG

MAME_LOG holds mame/plus_keys.lua's "[plus_keys] expect XX down|up (name)" lines, the transitions the
script made on MAME's M0110A, with the ADB key code each should become; EMU68_LOG the kernel's
"[PSMAC:PLUS] keyboard: RR -> ADB key XX down|up" lines (mac.board=plus with mac.pluskeylog=400, so
that every transition is logged). The two sequences are lined up and every difference is printed. The
M0110A sends keypad =, /, * and + with a Shift of its own around them (MAME's pluskbd.cpp): Shift
transitions the script did not make are reported apart, not as errors. Exit status 0 when nothing is
missing, extra or out of order.
"""
import difflib, re, sys

def main():
    if len(sys.argv) != 3:
        print(__doc__); return 2
    exp = [(int(m.group(1), 16), m.group(2), m.group(3))
           for m in re.finditer(r'\[plus_keys\] expect ([0-9a-f]{2}) (down|up) \(([^)]*)\)', open(sys.argv[1], errors='replace').read())]
    got = [(int(m.group(2), 16), m.group(3), m.group(1))
           for m in re.finditer(r'\[PSMAC:PLUS\] keyboard: ([0-9a-f]{2}) -> ADB key ([0-9a-f]{2}) (down|up)', open(sys.argv[2], errors='replace').read())]
    if not exp:
        print("no [plus_keys] expect lines in", sys.argv[1]); return 2
    shift_expected = any(k == 0x38 for k, _, _ in exp)
    # the keyboard's own Shift around keypad = / * + : set aside when the script pressed Shift elsewhere only
    extra_shift = []
    got_main = []
    for i, g in enumerate(got):
        if g[0] == 0x38:
            nxt = got[i + 1][0] if i + 1 < len(got) else None
            prv = got[i - 1][0] if i > 0 else None
            if nxt in (0x51, 0x4b, 0x43, 0x45) or prv in (0x51, 0x4b, 0x43, 0x45):
                extra_shift.append(g); continue
        got_main.append(g)
    a = ['%02x %s' % (k, d) for k, d, _ in exp]
    b = ['%02x %s' % (k, d) for k, d, _ in got_main]
    names = {('%02x %s' % (k, d)): n for k, d, n in exp}
    raws = {}
    for k, d, r in got_main: raws.setdefault('%02x %s' % (k, d), r)
    bad = 0
    for op, i1, i2, j1, j2 in difflib.SequenceMatcher(a=a, b=b, autojunk=False).get_opcodes():
        if op == 'equal': continue
        bad += 1
        for x in a[i1:i2]: print('  expected, not received: %s (%s)' % (x, names.get(x, '?')))
        for x in b[j1:j2]: print('  received, not expected: %s (raw %s)' % (x, raws.get(x, '?')))
    print('%d transitions pressed, %d received (%d of them the keyboard\'s own Shift around keypad = / * +); %s'
          % (len(exp), len(got), len(extra_shift), 'all of them, in order' if not bad else '%d differences' % bad))
    return 0 if not bad else 1

if __name__ == '__main__':
    sys.exit(main())
