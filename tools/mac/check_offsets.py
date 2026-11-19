#!/usr/bin/env python3
"""Compare the struct offsets in a 68k source with the ones the kernel's header gives.

    check_offsets.py mac/enet.s build/an_offsets        (an_offsets built from tools/mac/an_offsets.c)

Every `.equ NAME, VALUE` in the source whose NAME the offsets program prints must carry the same
value; the first mismatch fails the build. See tools/mac/an_offsets.c for why this exists.
"""
import re, subprocess, sys

src, prog = sys.argv[1], sys.argv[2]
want = {}
for line in subprocess.run([prog], capture_output=True, text=True, check=True).stdout.splitlines():
    name, val = line.split()
    want[name] = int(val)
bad = 0
text = open(src).read()
# enet.s is assembled twice: the WiFi driver (.ifdef WIFI) uses unit 0's offsets, which the header
# has no names for; the header's AN_OFF_UNIT1/RX1/TX1 are unit 1's, the other branch's
text = re.sub(r'^\s*\.ifdef\s+WIFI\b.*?^\s*\.else\b', '', text, flags=re.M | re.S)
for m in re.finditer(r'^\s*\.equ\s+(\w+),\s*(0x[0-9A-Fa-f]+|\d+)', text, re.M):
    name, val = m.group(1), int(m.group(2), 0)
    if name in want and want[name] != val:
        print(f"{src}: {name} is {val}, the header says {want[name]}")
        bad += 1
print(f"{src}: offsets match alice_net.h" if not bad else f"{src}: {bad} mismatch(es)")
sys.exit(1 if bad else 0)
