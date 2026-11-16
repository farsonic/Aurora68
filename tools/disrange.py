#!/usr/bin/env python3
"""Print disassembly lines whose address is in [lo,hi). usage: disrange.py file.dis lo hi [lo hi ...] (hex)"""
import sys,re
lines=open(sys.argv[1]).read().splitlines()
rng=[(int(sys.argv[i],16),int(sys.argv[i+1],16)) for i in range(2,len(sys.argv),2)]
for lo,hi in rng:
    print(f"=== {lo:08x}..{hi:08x} ===")
    for l in lines:
        m=re.match(r'\s*([0-9a-f]+):',l)
        if m and lo<=int(m.group(1),16)<hi: print(l[:100])
    print()
