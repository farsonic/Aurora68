| SPDX-License-Identifier: MPL-2.0
| mac/iotest/glue.s - the File Manager and Device Manager calls of "Aurora68 IO Test" that the C
| kit's crt0.s does not have (mac/c/README.md): short io_PBxxx(void *pb), synchronous, the result
| also in the block's ioResult. Register-based OS traps: the block in A0, the result in D0.
	.text
	.macro	pbcall name, trap
	.globl	\name
\name:	move.l	4(%sp),%a0
	.short	\trap
	ext.l	%d0
	rts
	.endm
	pbcall	io_PBOpen, 0xA000
	pbcall	io_PBClose, 0xA001
	pbcall	io_PBRead, 0xA002
	pbcall	io_PBWrite, 0xA003
	pbcall	io_PBControl, 0xA004
	pbcall	io_PBStatus, 0xA005
	pbcall	io_PBGetVInfo, 0xA007
	pbcall	io_PBCreate, 0xA008
	pbcall	io_PBDelete, 0xA009
	pbcall	io_PBUnmountVol, 0xA00E
	pbcall	io_PBFlushVol, 0xA013
	pbcall	io_PBEject, 0xA017

| the INFO block is shared with the drivers' logs: a line is written with interrupts masked
	.globl	io_get_sr
io_get_sr:
	moveq	#0,%d0
	move.w	%sr,%d0
	rts
	.globl	io_set_sr
io_set_sr:
	move.w	6(%sp),%sr
	rts

| The Sound Manager (Pascal calls: room for the result, the arguments left to right, a BOOLEAN as a
| byte in the high half of a word) and NewHandle (an OS call: the size in D0, the handle in A0).
| D2 is kept, as GCC expects; the traps may change it.
| short io_SndNewChannel(void **chan, short synth, long init, void *userRoutine)
	.globl	io_SndNewChannel
io_SndNewChannel:
	move.l	%d2,-(%sp)
	clr.w	-(%sp)
	move.l	10(%sp),-(%sp)		| chan
	move.w	20(%sp),-(%sp)		| synth (the low word of its slot)
	move.l	24(%sp),-(%sp)		| init
	move.l	32(%sp),-(%sp)		| userRoutine
	.short	0xA807
	move.w	(%sp)+,%d0
	ext.l	%d0
	move.l	(%sp)+,%d2
	rts
| short io_SndPlay(void *chan, void *sndHandle, int async)
	.globl	io_SndPlay
io_SndPlay:
	move.l	%d2,-(%sp)
	clr.w	-(%sp)
	move.l	10(%sp),-(%sp)		| chan
	move.l	18(%sp),-(%sp)		| the sound's handle
	move.b	29(%sp),-(%sp)		| async: the low byte of its slot, as a BOOLEAN
	.short	0xA805
	move.w	(%sp)+,%d0
	ext.l	%d0
	move.l	(%sp)+,%d2
	rts
| short io_SndDisposeChannel(void *chan, int quitNow)
	.globl	io_SndDisposeChannel
io_SndDisposeChannel:
	move.l	%d2,-(%sp)
	clr.w	-(%sp)
	move.l	10(%sp),-(%sp)		| chan
	move.b	21(%sp),-(%sp)		| quitNow
	.short	0xA801
	move.w	(%sp)+,%d0
	ext.l	%d0
	move.l	(%sp)+,%d2
	rts
| void **io_NewHandle(long size)
	.globl	io_NewHandle
io_NewHandle:
	move.l	%d2,-(%sp)
	move.l	8(%sp),%d0
	.short	0xA122
	move.l	%a0,%d0
	move.l	(%sp)+,%d2
	rts
