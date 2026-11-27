#!/bin/bash
# SPDX-License-Identifier: MPL-2.0
# unixdis.sh UNIX SYMS VA LEN: disassemble LEN bytes of an A/UX 3.1.1 kernel file (/unix, COFF) at the virtual address
# VA, with the names of SYMS (coffsyms.py UNIX syms > SYMS) beside the lines. The three sections' places in the file
# are read from its headers. Needs m68k-linux-gnu-objdump. The output is Apple's code: keep it out of the repository.
set -e
U=$1; SYMS=$2; VA=$(($3)); LEN=$(($4)); HERE=$(dirname "$0")
OFF=$(python3 - "$U" $VA <<'PY'
import sys, struct
d = open(sys.argv[1], 'rb').read(); va = int(sys.argv[2])
nscns, = struct.unpack('>H', d[2:4]); opthdr, = struct.unpack('>H', d[16:18]); o = 20 + opthdr
for i in range(nscns):
    paddr, vaddr, size, scnptr = struct.unpack('>IIII', d[o+8:o+24])
    if scnptr and vaddr <= va < vaddr + size: print(va - vaddr + scnptr); break
    o += 40
PY
)
[ -n "$OFF" ] || { echo "no section of $U holds $3" >&2; exit 1; }
T=$(mktemp); trap 'rm -f "$T"' EXIT
dd if="$U" of="$T" bs=1 skip=$OFF count=$LEN 2>/dev/null
m68k-linux-gnu-objdump -D -b binary -m m68k:68030 --adjust-vma=$VA "$T" | tail -n +8 | python3 "$HERE/symann.py" "$SYMS"
