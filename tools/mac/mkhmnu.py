#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Balloon Help for a program's menus: 'hmnu' resources from a small text file, and a 'vers'.

    mkhmnu.py balloons.txt -o build/pidisplay   ->  build/pidisplay-hmnu-128.bin, -129.bin, ...
    mkhmnu.py --vers 1.0 "1.0, Aurora68 2026" -o build/pidisplay-vers.bin

System 7's Help Manager shows a menu's balloons from the 'hmnu' resource whose ID is the menu's
(Inside Macintosh: More Macintosh Toolbox, "Help Manager", "The Menu Help Resource"). The compiled
resource: a header (the Help Manager's version 2, the options long 0, the balloon definition 0 =
the standard 'WDEF' 126, the variation 0, the count of the components that follow the missing-items
component), then components, each starting with its own length in bytes (the length word included)
and its kind: 1 HMStringItem, four Pascal strings (enabled, dimmed, checked, marked) padded to a word;
256 HMSkipItem, nothing more. The missing-items component, the menu title's, then one per item; a
separator line is an item too (a skip). One more skip ends the list, so that whichever way the
Help Manager counts (with or without the missing-items component) the last item has its balloon.

The text file: "menu N" starts a menu; "title ENABLED | DIMMED", "item ENABLED | DIMMED" (or
"item -" for a separator, "item = TEXT" for the same text either way) follow in the menu's order.
Lines starting with # are comments. Text is Mac Roman; "..." in it becomes the ellipsis.
"""
import argparse, struct, sys

def pstr(s):
    b = s.replace('...', '…').encode('mac_roman')
    if len(b) > 255:
        sys.exit(f"balloon text too long ({len(b)} bytes): {s[:40]}...")
    return bytes([len(b)]) + b

def string_item(enabled, dimmed):
    body = struct.pack('>H', 1) + pstr(enabled) + pstr(dimmed) + pstr('') + pstr('')
    if (len(body) + 2) & 1:
        body += b'\0'
    return struct.pack('>H', len(body) + 2) + body

SKIP = struct.pack('>HH', 4, 256)

def item(spec):
    spec = spec.strip()
    if spec == '-':
        return SKIP
    if spec.startswith('='):
        t = spec[1:].strip()
        return string_item(t, t)
    e, _, d = spec.partition('|')
    return string_item(e.strip(), d.strip() or e.strip())

ap = argparse.ArgumentParser()
ap.add_argument('spec', nargs='?')
ap.add_argument('--vers', nargs=2, metavar=('SHORT', 'LONG'))
ap.add_argument('-o', required=True)
a = ap.parse_args()
if a.vers:
    major, _, minor = a.vers[0].partition('.')
    bcd = lambda n: (n // 10) << 4 | n % 10
    v = struct.pack('>BBBBH', bcd(int(major)), (int(minor or 0) & 15) << 4, 0x80, 0, 0) + pstr(a.vers[0]) + pstr(a.vers[1])
    open(a.o, 'wb').write(v)
    sys.exit(0)
menus, cur = {}, None
for line in open(a.spec, encoding='utf-8'):
    line = line.rstrip('\n')
    if not line.strip() or line.lstrip().startswith('#'):
        continue
    word, _, rest = line.strip().partition(' ')
    if word == 'menu':
        cur = int(rest); menus[cur] = {'title': None, 'items': []}
    elif word == 'title':
        menus[cur]['title'] = item(rest)
    elif word == 'item':
        menus[cur]['items'].append(item(rest))
    else:
        sys.exit(f"{a.spec}: not understood: {line}")
for mid, m in menus.items():
    comps = [m['title'] or SKIP] + m['items'] + [SKIP]
    data = struct.pack('>HIhhH', 2, 0, 0, 0, len(comps)) + SKIP + b''.join(comps)
    out = f"{a.o}-hmnu-{mid}.bin"
    open(out, 'wb').write(data)
    print(f"{out}: {len(m['items'])} items, {len(data)} bytes")
