#!/usr/bin/env python3
"""Recompute the Mac SE/Plus style ROM checksum (word sum of bytes 4..end) and write it to offset 0.
usage: romfix.py in.rom out.rom"""
import sys, struct
d = bytearray(open(sys.argv[1], 'rb').read())
s = sum(struct.unpack('>H', d[i:i+2])[0] for i in range(4, len(d) - (len(d) & 1), 2)) & 0xFFFFFFFF
old = struct.unpack('>I', d[:4])[0]
d[:4] = struct.pack('>I', s)
open(sys.argv[2], 'wb').write(d)
print(f"checksum {old:08X} -> {s:08X} ({'unchanged' if old == s else 'updated'}), {len(d)} bytes")
