#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
#
# tools/piinput_m68k.sh - .PiInput's ring-draining code (mac/piinput.s) run on a 68020 under qemu-m68k, against
# tools/piinput_test.c: the driver assembled as it is for the declaration ROM, except that the Mac's low memory
# (KeyMap, MBState, RawMouse ...) is a block of the test's, _PostEvent a routine of the test's that notes what was
# posted, and the key trace's log calls are off (they need the card's window). Written on 24 November 2026 for
# docs/08 item 139 (the owner's "it's like I have some key held down"). Skipped without qemu-m68k or the cross
# compiler, like tools/pistatus_m68k.sh. make -C mac check runs it.
set -e
here=$(cd "$(dirname "$0")" && pwd)
top=$(dirname "$here")
B=${1:-$top/mac/build}
if ! command -v qemu-m68k >/dev/null 2>&1 || ! command -v m68k-linux-gnu-gcc >/dev/null 2>&1; then
	echo "piinput_m68k: qemu-m68k or m68k-linux-gnu-gcc not installed, skipped"
	exit 0
fi
mkdir -p "$B"
# low memory into the test's block, the trap into a call, the trace quiet; the globals' offsets exported
sed -E \
	-e 's/^(	\.equ	(MMU32Bit|MBState|MTemp|RawMouse|CrsrNew|CrsrCouple|CrsrPin|DeviceList|CrsrDevice|CrsrBusy|Mouse|JCrsrTask|KeyMap|KeyLast|KeyTime|KeyRepTime|Ticks),[	 ]+)(0x0[0-9A-Fa-f]{3})/\1lowmem+\3/' \
	-e 's/^	\.short	_PostEvent/	jsr	test_post_event/' \
	-e 's/^trace_left:	\.word	32/trace_left:	.word	0/' \
	"$top/mac/piinput.s" > "$B/piinput_test.s"
cat >> "$B/piinput_test.s" <<'ASM'
	.globl	drain, t_offsets, test_post_event
	.even
t_offsets:	.long	G_RING, G_MODS, G_BUTTONS, G_CAPS, G_KMOWN, G_BTNMODS, G_SIZE
test_post_event:			| a0 = what, d0 = message, as _PostEvent takes them: noted, noErr
	movem.l	%d0-%d1/%a0-%a1,-(%sp)
	move.l	%d0,-(%sp)
	move.l	%a0,-(%sp)
	jsr	record_post
	addq.l	#8,%sp
	movem.l	(%sp)+,%d0-%d1/%a0-%a1
	moveq	#0,%d0
	rts
	.section .note.GNU-stack,"",@progbits
ASM
m68k-linux-gnu-as -m68020 -o "$B/piinput_test_drv.o" "$B/piinput_test.s"
m68k-linux-gnu-gcc -m68020 -O2 -std=gnu99 -ffreestanding -fno-builtin -fno-pic -fno-pie -static -nostdlib -Wall -Wextra \
	-Wno-unused-parameter -I "$here" -I "$top/emu68/src/alice" -Wl,-e,_start \
	-o "$B/piinput_test.m68k" "$here/piinput_test.c" "$here/play_core_shim.c" "$B/piinput_test_drv.o" -lgcc
st=0
out=$(qemu-m68k -cpu m68020 "$B/piinput_test.m68k") || st=$?
echo "$out"
exit $st
