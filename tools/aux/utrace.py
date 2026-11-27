#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# utrace.py FILE [LAST]: print the tail of a mac.usertrace ring (oldest first); events are marked
import sys, struct
d = open(sys.argv[1], 'rb').read()
magic, n, N = struct.unpack('>III', d[:12])
assert magic == 0x55545243
last = int(sys.argv[2]) if len(sys.argv) > 2 else 200
cnt = min(n, N)
start = n - cnt
out = []
for i in range(max(start, n - last), n):
    o = 32 + (i % N) * 32
    w = struct.unpack('>8I', d[o:o+32])
    if w[1] & 0x80000000:
        kind = {1: 'DATA', 2: 'FETCH', 3: 'HELPER'}.get((w[1] >> 24) & 15, '?')
        print('%7d  ** BUSERR %-6s PC %08x SSW %04x USP/A7 %08x addr %08x SR %04x A6 %08x frame %08x' % (i, kind, w[0], w[1] & 0xffff, w[2], w[3], w[4], w[5], w[6]))
    else:
        print('%7d  PC %08x SR %04x A7 %08x A6 %08x A0 %08x A1 %08x D0 %08x D1 %08x' % (i, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]))
