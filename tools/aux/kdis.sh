#!/bin/bash
# SPDX-License-Identifier: MPL-2.0
# kdis.sh DUMP VA_START LEN [PHYS_OF_VA_START]: disassemble 68k code from a RAM dump (A/UX kernel: phys = va - 0x10000000 + 0x5c000)
D=$1; VA=$(($2)); LEN=$(($3)); PH=${4:-$((VA - 0x10000000 + 0x5c000))}; PH=$(($PH))
T=$(mktemp "${TMPDIR:-/tmp}/kdis.XXXXXX")
dd if=$D of=$T bs=1 skip=$PH count=$LEN 2>/dev/null
m68k-linux-gnu-objdump -D -b binary -m m68k:68030 --adjust-vma=$VA $T | tail -n +8
rm -f "$T"
