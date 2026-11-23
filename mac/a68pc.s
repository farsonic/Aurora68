| SPDX-License-Identifier: MPL-2.0
| a68pc.s - ".A68PC", the driver of the Aurora68 PC Card (slot $D), carried in the card's own declaration ROM and
| installed by the Slot Manager at start-up, as .A68Video is for the video decoder card. 68k, GNU as MIT syntax,
| assembled with -m68020.
|
| Backstory (23 November 2026, docs/19-pc-card.md). The PC card is a NuBus card that exists only in the Pi
| (emu68/src/psmac/psmac_pc.c): a PC (Aurora86: Bochs with Libreboot's firmware) running on a Pi core, its screen
| drawn by the card into a window of the Mac's, its keyboard and mouse fed from the Mac. Its window (1 MiB at
| $FD000000, $D00000 in 24-bit mode) holds the block of psmac_pc.h; a program may work it directly once it has
| the address, and "Aurora68 PC" does for the place and the input, which change often. These calls are the card's
| programming interface for everyone else, numbered from 100 as .A68Video's are:
|
|   Control 100csParam: three longs, a command (psmac_pc.h A68P_C_*: 1 power on, 2 power off, 3 reset, 4 pause,
|              5 resume, 6 Control-Alt-Delete, 7 redraw, 8 attach, 9 detach, 10 rescan) and its two arguments;
|              waits for the card to take it (done_seq), at most three seconds
|   Control 112csParam: a pointer to six longs: screen, top, left, bottom, right, visible (the window's content
|              in that screen's pixels); geom_seq + 1
|   Control 113csParam: a pointer to an input event (16 bytes, struct a68p_in); ioErr when the ring is full
|   Control 114csParam: four longs: scale, scroll_x, scroll_y, options; geom_seq + 1
|   Control 115csParam: a long: focus (1 the PC has the keyboard and mouse, 0 the Mac)
|   Status 100 csParam: the window's address in this addressing mode, the block's version, state, error (longs)
|   Status 101 csParam: a pointer to 384 bytes: the block (state, speed, natural size, message)
| The window is at $Fs000000 in 32-bit mode and $s00000 in 24-bit mode (s the slot, from the DCE), worked out at
| every call. Registers as in .A68Video: A0 and A1 given back to IODone as they came, D3-D7/A2-A6 kept.

	.text
	.globl	drvr_start

	.equ	MMU32Bit,	0x0CB2
	.equ	Ticks,		0x016A
	.equ	JIODone,	0x08FC
	.equ	dCtlSlot,	40
	.equ	csCode,		26
	.equ	csParam,	28
	.equ	noErr,		0
	.equ	openErr,	-23
	.equ	controlErr,	-17
	.equ	statusErr,	-18
	.equ	ioErr,		-36
	| the card's block (emu68/src/psmac/psmac_pc.h)
	.equ	A68P_MAGIC,	0x41363850	| 'A68P'
	.equ	B_VERSION,	0x04
	.equ	B_DONESEQ,	0x0C
	.equ	B_STATE,	0x10
	.equ	B_ERROR,	0x14
	.equ	B_INTAIL,	0x54
	.equ	B_CMDSEQ,	0x80
	.equ	B_CMD,		0x84
	.equ	B_SCREEN,	0x90
	.equ	B_GEOMSEQ,	0xA8
	.equ	B_SCALE,	0xB8
	.equ	B_FOCUS,	0xC8
	.equ	B_INHEAD,	0xCC
	.equ	INPUT,		0x400
	.equ	IN_SLOTS,	64
	.equ	WAIT_TICKS,	180

drvr_start:
	.short	0x4C00			| dNeedLock|dStatEnable|dCtlEnable
	.short	0
	.short	0
	.short	0
	.short	d_open-drvr_start
	.short	d_prime-drvr_start
	.short	d_control-drvr_start
	.short	d_status-drvr_start
	.short	d_close-drvr_start
	.byte	6
	.ascii	".A68PC"
	.even

| a2 = the card's window, from the DCE in a1 and the addressing mode
window:
	moveq	#0,%d0
	move.b	dCtlSlot(%a1),%d0
	tst.b	MMU32Bit
	beq.s	1f
	ror.l	#8,%d0			| $0000000s -> $0s000000
	or.l	#0xF0000000,%d0		| $Fs000000
	bra.s	2f
1:	swap	%d0			| $s0000 ...
	lsl.l	#4,%d0			| ... $s00000
2:	move.l	%d0,%a2
	rts

| ---- the log, through slot 9's INFO service (as a68video.s) -------------------------------------------
	.macro	log text
	movem.l	%d0-%d1/%a0-%a1,-(%sp)
	lea	9f(%pc),%a1
	bsr	do_log
	bra.s	8f
9:	.ascii	"\text"
	.byte	0
	.even
8:	movem.l	(%sp)+,%d0-%d1/%a0-%a1
	.endm
do_log:
	move.l	#0xF9000000,%a0
	tst.b	MMU32Bit
	bne.s	1f
	move.l	#0x00900000,%a0
1:	cmp.l	#0x50534D42,(%a0)	| 'PSMB': the mailbox is there
	bne.s	5f
	lea	0x1000(%a0),%a0
	moveq	#0,%d1
2:	tst.b	(%a1,%d1.w)
	beq.s	3f
	addq.w	#1,%d1
	bra.s	2b
3:	move.w	#2,8(%a0)
	move.l	%a1,28(%a0)
	move.l	%d1,32(%a0)
	move.l	(%a0),%d0
	addq.l	#1,%d0
	move.l	%d0,(%a0)
	move.l	#200000,%d1
4:	cmp.l	4(%a0),%d0
	beq.s	5f
	subq.l	#1,%d1
	bne.s	4b
5:	rts

| ---- Open: the card answers in its window --------------------------------------------------------------
d_open:
	movem.l	%a2,-(%sp)
	bsr	window
	cmp.l	#A68P_MAGIC,(%a2)
	bne.s	1f
	log	"A68PC: the Aurora68 PC Card's driver is open"
	moveq	#noErr,%d0
	bra.s	2f
1:	log	"A68PC: no card answered in slot $D's window"
	moveq	#openErr,%d0
2:	movem.l	(%sp)+,%a2
	rts

d_close:
	moveq	#noErr,%d0
	rts
d_prime:
	moveq	#ioErr,%d0
	bra	iodone

| ---- Control ------------------------------------------------------------------------------------------
d_control:
	movem.l	%d3-%d4/%a2-%a4,-(%sp)
	movem.l	%a0-%a1,-(%sp)		| the parameter block and the DCE, which IODone wants back in A0 and A1
	bsr	window
	lea	csParam(%a0),%a3
	move.w	csCode(%a0),%d0
	cmp.w	#100,%d0
	beq.s	c_command
	cmp.w	#112,%d0
	beq.s	c_place
	cmp.w	#113,%d0
	beq.s	c_input
	cmp.w	#114,%d0
	beq	c_view
	cmp.w	#115,%d0
	beq	c_focus
	moveq	#controlErr,%d0
c_done:
	movem.l	(%sp)+,%a0-%a1
	movem.l	(%sp)+,%d3-%d4/%a2-%a4
	bra	iodone

| a command and its two arguments, then cmd_seq + 1 and the card's done_seq awaited
c_command:
	lea	B_CMD(%a2),%a1
	move.l	(%a3)+,(%a1)+
	move.l	(%a3)+,(%a1)+
	move.l	(%a3)+,(%a1)+
	move.l	B_CMDSEQ(%a2),%d3
	addq.l	#1,%d3
	move.l	%d3,B_CMDSEQ(%a2)
	move.l	Ticks,%d4
1:	cmp.l	B_DONESEQ(%a2),%d3
	beq.s	2f
	move.l	Ticks,%d0
	sub.l	%d4,%d0
	cmp.l	#WAIT_TICKS,%d0
	bcs.s	1b
	moveq	#ioErr,%d0
	bra.s	c_done
2:	moveq	#noErr,%d0
	bra.s	c_done
| the window's place: six longs from the caller, then geom_seq + 1
c_place:
	move.l	(%a3),%a4
	lea	B_SCREEN(%a2),%a1
	moveq	#5,%d1
1:	move.l	(%a4)+,(%a1)+
	dbf	%d1,1b
	addq.l	#1,B_GEOMSEQ(%a2)
	moveq	#noErr,%d0
	bra.s	c_done
| an input event into the ring at in_head, if there is room (in_head - in_tail < 64), then in_head + 1
c_input:
	move.l	(%a3),%a4
	move.l	B_INHEAD(%a2),%d3
	move.l	%d3,%d0
	sub.l	B_INTAIL(%a2),%d0
	cmp.l	#IN_SLOTS,%d0
	bcs.s	1f
	moveq	#ioErr,%d0
	bra.s	c_done
1:	move.l	%d3,%d0
	and.l	#IN_SLOTS-1,%d0
	lsl.l	#4,%d0
	lea	INPUT(%a2,%d0.l),%a1
	move.l	(%a4)+,(%a1)+
	move.l	(%a4)+,(%a1)+
	move.l	(%a4)+,(%a1)+
	move.l	(%a4)+,(%a1)+
	addq.l	#1,%d3
	move.l	%d3,B_INHEAD(%a2)
	moveq	#noErr,%d0
	bra	c_done
| scale, scroll_x, scroll_y, options, then geom_seq + 1
c_view:
	lea	B_SCALE(%a2),%a1
	move.l	(%a3)+,(%a1)+
	move.l	(%a3)+,(%a1)+
	move.l	(%a3)+,(%a1)+
	move.l	(%a3)+,(%a1)+
	addq.l	#1,B_GEOMSEQ(%a2)
	moveq	#noErr,%d0
	bra	c_done
c_focus:
	move.l	(%a3),B_FOCUS(%a2)
	moveq	#noErr,%d0
	bra	c_done

| ---- Status -------------------------------------------------------------------------------------------
d_status:
	movem.l	%d3-%d4/%a2-%a4,-(%sp)
	movem.l	%a0-%a1,-(%sp)
	bsr	window
	lea	csParam(%a0),%a3
	move.w	csCode(%a0),%d0
	cmp.w	#100,%d0
	beq.s	s_info
	cmp.w	#101,%d0
	beq.s	s_block
	moveq	#statusErr,%d0
s_done:
	movem.l	(%sp)+,%a0-%a1
	movem.l	(%sp)+,%d3-%d4/%a2-%a4
	bra.s	iodone
s_block:
	move.l	(%a3),%a4
	move.l	%a2,%a1
	moveq	#95,%d1			| 384 bytes
1:	move.l	(%a1)+,(%a4)+
	dbf	%d1,1b
	moveq	#noErr,%d0
	bra.s	s_done
s_info:
	move.l	%a2,(%a3)
	move.l	B_VERSION(%a2),4(%a3)
	move.l	B_STATE(%a2),8(%a3)
	move.l	B_ERROR(%a2),12(%a3)
	moveq	#noErr,%d0
	bra.s	s_done

| ---- common exit: immediate calls return, queued calls go through IODone ----
iodone:
	btst	#9,6(%a0)
	beq.s	1f
	rts
1:	move.l	JIODone,-(%sp)
	rts
drvr_end:
