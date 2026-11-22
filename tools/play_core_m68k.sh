#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
#
# tools/play_core_m68k.sh - Aurora68 Play's core tests (tools/play_core_test.c, its units) as a 68020 program:
# big-endian, 32-bit longs, compiled by the classic Play's own compiler with its flags, run under qemu-m68k (user
# mode). The host test runs the same code on a little-endian 64-bit machine (as the macOS Play does); this one
# checks what the SE runs. No C library: tools/play_core_shim.c gives the test its printf and string calls.
# Skipped when qemu-m68k or m68k-linux-gnu-gcc is not installed.
#     sh tools/play_core_m68k.sh [BUILD_DIR]          (make -C mac check runs it with mac/build)
# Written on 22 November 2026 (docs/08 item 107).
set -e
here=$(cd "$(dirname "$0")" && pwd)
top=$(dirname "$here")
B=${1:-$top/mac/build}
if ! command -v qemu-m68k >/dev/null 2>&1 || ! command -v m68k-linux-gnu-gcc >/dev/null 2>&1; then
	echo "play_core_m68k: qemu-m68k or m68k-linux-gnu-gcc not installed, skipped"
	exit 0
fi
mkdir -p "$B"
m68k-linux-gnu-gcc -m68020 -Os -std=gnu99 -ffreestanding -fno-builtin -fno-pic -fno-pie -static -nostdlib -Wall -Wextra \
	-Wno-unused-parameter -Wno-type-limits -DA68_FREESTANDING -I "$top/play/core/include" -I "$here" -Wl,-e,_start \
	-o "$B/play_core_test.m68k" "$here/play_core_test.c" "$here/play_core_shim.c" "$top"/play/core/*.c -lgcc
st=0
out=$(qemu-m68k -cpu m68020 "$B/play_core_test.m68k") || st=$?
echo "$out" | sed 's/^play_core_test:/play_core_test (68020, big-endian):/'
exit $st
