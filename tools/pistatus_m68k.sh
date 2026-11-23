#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
#
# tools/pistatus_m68k.sh - Aurora68 Status 2.0's host test (tools/pistatus_test.c with mac/pistatus/model.c) as a
# 68020 program: big-endian, compiled by the Mac program's compiler with its -O2, run under qemu-m68k (user mode),
# as tools/play_core_m68k.sh does for Play's core (whose shim gives it printf and the string calls). Skipped when
# qemu-m68k or m68k-linux-gnu-gcc is not installed. make -C mac check runs it. 23 November 2026.
set -e
here=$(cd "$(dirname "$0")" && pwd)
top=$(dirname "$here")
B=${1:-$top/mac/build}
if ! command -v qemu-m68k >/dev/null 2>&1 || ! command -v m68k-linux-gnu-gcc >/dev/null 2>&1; then
	echo "pistatus_m68k: qemu-m68k or m68k-linux-gnu-gcc not installed, skipped"
	exit 0
fi
mkdir -p "$B"
m68k-linux-gnu-gcc -m68020 -O2 -std=gnu99 -ffreestanding -fno-builtin -fno-pic -fno-pie -static -nostdlib -Wall -Wextra \
	-Wno-unused-parameter -DA68_FREESTANDING -I "$here" -I "$top/emu68/src/alice" -Wl,-e,_start \
	-o "$B/pistatus_test.m68k" "$here/pistatus_test.c" "$here/play_core_shim.c" "$top/mac/pistatus/model.c" -lgcc
st=0
out=$(qemu-m68k -cpu m68020 "$B/pistatus_test.m68k") || st=$?
echo "$out" | sed 's/^pistatus_test:/pistatus_test (68020, big-endian):/'
exit $st
