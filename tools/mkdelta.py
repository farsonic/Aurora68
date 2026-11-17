#!/usr/bin/env python3
"""Write rom/patches/iici-368CADFE.delta.json: the byte ranges that differ between the stock
IIci ROM and the patched one, with the checksums, so that make-container.py can patch a user's
stock ROM on any machine without the m68k assembler rompatch.py needs (found out the first time a
user ran make-container.py on a Mac: no m68k-linux-gnu-as there, docs/08 item 28).
    tools/mkdelta.py [original.rom patched.rom out.json]
"""
import json, sys
orig, patched, out = (sys.argv[1:4] if len(sys.argv) == 4 else
                      ('rom/original/MacIIci_368CADFE.rom', 'rom/patched/MacIIci_psmac.rom', 'rom/patches/iici-368CADFE.delta.json'))
a = open(orig, 'rb').read(); b = open(patched, 'rb').read()
assert len(a) == len(b)
runs = []; i = 0
while i < len(a):
    if a[i] != b[i]:
        j = i
        while j < len(a) and (a[j] != b[j] or (j + 1 < len(a) and a[j + 1] != b[j + 1])): j += 1
        runs.append([i, b[i:j].hex()]); i = j
    else: i += 1
json.dump({"name": "Macintosh IIci ROM 368CADFE -> Aurora68 (the patch set rom/patches/iici.json, assembled)",
           "note": "Byte ranges that differ between the stock image and the patched one, so make-container.py can apply them without the m68k assembler rompatch.py needs. Regenerate with tools/mkdelta.py after changing the patch set.",
           "input_checksum": a[:4].hex().upper(), "output_checksum": b[:4].hex().upper(), "size": len(a), "runs": runs},
          open(out, 'w'), indent=1)
print(f"{out}: {len(runs)} runs, {sum(len(r[1]) // 2 for r in runs)} bytes, {a[:4].hex().upper()} -> {b[:4].hex().upper()}")
