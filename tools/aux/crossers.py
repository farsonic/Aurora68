#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# crossers.py DIS: instructions of an objdump listing that start in one 4 KiB page and end in the next
import sys, re
prev = None
for line in open(sys.argv[1]):
    m = re.match(r'\s*([0-9a-f]+):\t((?:[0-9a-f]{4} )+)\s*\t?(.*)', line)
    if not m: continue
    a = int(m.group(1), 16); n = len(m.group(2).split()) * 2; txt = m.group(3).strip()
    if txt == '' and prev:          # continuation line of the previous instruction
        prev[1] += n; continue
    if prev and (prev[0] & 0xfff) + prev[1] > 0x1000: print('%6x: %2d bytes  %s' % (prev[0], prev[1], prev[2]))
    prev = [a, n, txt]
