#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# symann.py SYMS: annotate objdump lines on stdin with the names of addresses that are symbols
import sys, re
syms = {}
for l in open(sys.argv[1]):
    p = l.split()
    syms.setdefault(int(p[0], 16), p[-1])
for l in sys.stdin:
    l = l.rstrip('\n')
    m = re.match(r'\s*([0-9a-f]+):', l)
    if m and int(m.group(1), 16) in syms: print('\n%s:' % syms[int(m.group(1), 16)])
    names = [syms[int(x, 16)] for x in re.findall(r'0x([0-9a-f]{5,8})', l) if int(x, 16) in syms]
    print(l[:90] + ('    ; ' + ' '.join(names) if names else ''))
