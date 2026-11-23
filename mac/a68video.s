| SPDX-License-Identifier: MPL-2.0
| a68video.s - ".A68Video", the driver of the Aurora68 Video Decoder card (slot $C), carried in the card's
| own declaration ROM and installed by the Slot Manager at start-up, as a NuBus card of 1992 carried its
| driver. 68k, GNU as MIT syntax, assembled with -m68020.
|
| Backstory (22 November 2026, docs/08 item 110). The video decoder card is a NuBus card that exists only
| in the Pi (emu68/src/psmac/psmac_videocard.c): a 1 MiB window in slot $C's space holding a stream
| channel (an MPEG stream decoded into a window of the Mac's, psmac_video.c) and a frame decoder (one
| QuickTime picture at a time into a pixel map, psmac_vframes.c). The first version (issue #23) had no
| driver: a program found the card with the Slot Manager and wrote the window itself. The owner then
| asked for it to be "essentially another NuBus card", usable "through traditional means": so this
| driver, whose Control and Status calls are the card's programming interface (docs/video-decoder-api.md),
| and on top of it the QuickTime components ("Aurora68 Video Decoder" extension, mac/vdec) and the
| programs. A program opens ".A68Video" with OpenDriver and makes PBControl / PBStatus calls; nothing
| else is needed, not even QuickTime.
|
|   Control 100csParam: a pointer to a frame request (struct a68v_freq, 128 bytes, psmac_video.h): copied
|              into a free slot of the card, carried out, its answer copied back (synchronous; the card
|              takes a millisecond or two). ioResult noErr, the card's own result in the request.
|   Control 101csParam: a word, 1 or 0: frame decoding on or off (off: requests answered "off", and the
|              QuickTime components hand every picture to QuickTime's own decompressors)
|   Control 110csParam: a long, a stream command (1 start, 2 pause, 3 resume, 4 stop); waits for the card
|   Control 111csParam: a pointer and a length: bytes for the stream's ring, as many as there is room for;
|              csParam+8 gets how many were taken
|   Control 112csParam: a pointer to six longs: screen, top, left, bottom, right, visible (the stream's
|              picture, in that screen's pixels)
|   Control 113csParam: a long, the volume (left << 16 | right, $0100 full)
|   Control 114csParam: a long, 1: the stream's last byte is in
|   Status 100 csParam: the window's address in this addressing mode, the stream block's version, the
|              frame decoder's version, frame decoding on (1) or off (0), the ring's size (five longs)
|   Status 101 csParam: a pointer to 64 bytes: the formats the card decodes, FourCCs, 0-terminated
|   Status 102 csParam: a pointer to 384 bytes: the stream block (state, position, statistics, message)
|   Status 103 csParam: a pointer to 64 bytes: the frame decoder's header (pictures decoded, drawn,
|              refused, sessions open, times)
| The numbers start at 100: Control 1 is the Device Manager's killCode (KillIO) and the low numbers are
| the standard calls of drivers, which this card's driver does not imitate.
| The window is at $Fs000000 in 32-bit mode and $s00000 in 24-bit mode (s the slot, from the DCE), worked
| out at every call: a program may run in either mode.
|
| Registers: the Device Manager passes the parameter block in A0 and the DCE in A1; D3-D7/A2-A6 are kept,
| and A0 and A1 are given back to IODone as they came (the first test ended in System error 14, "I/O system
| error": IODone found the card's request slot in A1 where the DCE should have been).

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
	| the card's window (emu68/src/psmac/psmac_video.h)
	.equ	A68V_MAGIC,	0x41363856	| 'A68V'
	.equ	A68F_MAGIC,	0x41363846	| 'A68F'
	.equ	B_VERSION,	0x04
	.equ	B_DONESEQ,	0x14
	.equ	B_TAIL,		0x18
	.equ	B_CMDSEQ,	0x80
	.equ	B_CMD,		0x84
	.equ	B_HEAD,		0x88
	.equ	B_EOF,		0x8C
	.equ	B_SCREEN,	0x90
	.equ	B_GEOMSEQ,	0xA8
	.equ	B_VOLUME,	0xAC
	.equ	RING,		0x80000
	.equ	RING_SIZE,	0x80000
	.equ	F_HDR,		0x1000
	.equ	F_VERSION,	0x1004
	.equ	F_ENABLED,	0x1014
	.equ	F_CODECS,	0x1040
	.equ	F_SLOTS,	0x1100
	.equ	F_NSLOTS,	8
	.equ	F_SLOTSIZE,	128
	.equ	WAIT_TICKS,	180		| three seconds: a picture takes milliseconds, a STOP a few

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
	.byte	9
	.ascii	".A68Video"
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

| ---- the log, through slot 9's INFO service (as piinput.s and pidisk.s) -----------------------------
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
	cmp.l	#A68V_MAGIC,(%a2)
	bne.s	1f
	cmp.l	#A68F_MAGIC,F_HDR(%a2)
	bne.s	1f
	log	"A68Video: the Aurora68 Video Decoder card's driver is open (stream channel, frame decoder)"
	moveq	#noErr,%d0
	bra.s	2f
1:	log	"A68Video: no card answered in slot $C's window"
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
	beq	c_frame
	cmp.w	#101,%d0
	beq.s	c_enable
	cmp.w	#110,%d0
	beq.s	c_command
	cmp.w	#111,%d0
	beq	c_write
	cmp.w	#112,%d0
	beq	c_place
	cmp.w	#113,%d0
	beq.s	c_volume
	cmp.w	#114,%d0
	beq.s	c_eof
	moveq	#controlErr,%d0
c_done:
	movem.l	(%sp)+,%a0-%a1
	movem.l	(%sp)+,%d3-%d4/%a2-%a4
	bra	iodone

c_enable:
	moveq	#0,%d1
	tst.w	(%a3)
	beq.s	1f
	moveq	#1,%d1
1:	move.l	%d1,F_ENABLED(%a2)
	moveq	#noErr,%d0
	bra.s	c_done
c_volume:
	move.l	(%a3),B_VOLUME(%a2)
	moveq	#noErr,%d0
	bra.s	c_done
c_eof:
	move.l	(%a3),B_EOF(%a2)
	moveq	#noErr,%d0
	bra.s	c_done
| a stream command, then the card's done_seq awaited
c_command:
	move.l	(%a3),B_CMD(%a2)
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
| the picture's place: six longs from the caller, then geom_seq + 1
c_place:
	move.l	(%a3),%a4
	lea	B_SCREEN(%a2),%a1
	moveq	#5,%d1
1:	move.l	(%a4)+,(%a1)+
	dbf	%d1,1b
	addq.l	#1,B_GEOMSEQ(%a2)
	moveq	#noErr,%d0
	bra	c_done
| bytes into the ring: as many as fit, in at most two pieces (the ring's end), head moved after them
c_write:
	move.l	(%a3),%a4		| the bytes
	move.l	4(%a3),%d3		| how many
	moveq	#0,%d4			| taken
1:	move.l	B_HEAD(%a2),%d1
	move.l	%d1,%d0
	sub.l	B_TAIL(%a2),%d0		| in the ring
	move.l	#RING_SIZE,%d2
	sub.l	%d0,%d2			| room
	beq.s	3f
	move.l	%d1,%d0
	and.l	#RING_SIZE-1,%d0	| where head is
	move.l	#RING_SIZE,%d1
	sub.l	%d0,%d1			| to the ring's end
	cmp.l	%d1,%d2
	bls.s	2f
	move.l	%d1,%d2
2:	move.l	%d3,%d1
	sub.l	%d4,%d1			| still to go
	beq.s	3f
	cmp.l	%d1,%d2
	bls.s	4f
	move.l	%d1,%d2
4:	lea	RING(%a2,%d0.l),%a1	| copy d2 bytes from a4 to a1
	move.l	%d2,%d1
5:	move.b	(%a4)+,(%a1)+
	subq.l	#1,%d1
	bne.s	5b
	add.l	%d2,%d4
	add.l	%d2,B_HEAD(%a2)		| after the bytes: the card takes head once it has stood
	cmp.l	%d3,%d4
	bcs.s	1b
3:	move.l	%d4,8(%a3)
	moveq	#noErr,%d0
	bra	c_done
| a frame request: a free slot (seq == done), the request's fields after seq and done copied in, seq + 1,
| done awaited, the slot's fields copied back
c_frame:
	move.l	(%a3),%a4
	lea	F_SLOTS(%a2),%a1
	moveq	#F_NSLOTS-1,%d1
1:	move.l	(%a1),%d0
	cmp.l	4(%a1),%d0
	beq.s	2f
	lea	F_SLOTSIZE(%a1),%a1
	dbf	%d1,1b
	moveq	#ioErr,%d0		| all eight busy: another program's asynchronous use (none yet)
	bra	c_done
2:	move.l	%d0,%d3
	addq.l	#1,%d3			| this request's seq
	moveq	#29,%d1			| longs 2 to 31
	lea	8(%a4),%a0
	lea	8(%a1),%a3
3:	move.l	(%a0)+,(%a3)+
	dbf	%d1,3b
	move.l	%d3,(%a1)
	move.l	Ticks,%d4
4:	cmp.l	4(%a1),%d3
	beq.s	5f
	move.l	Ticks,%d0
	sub.l	%d4,%d0
	cmp.l	#WAIT_TICKS,%d0
	bcs.s	4b
	moveq	#ioErr,%d0
	bra	c_done
5:	moveq	#29,%d1
	lea	8(%a1),%a0
	lea	8(%a4),%a3
6:	move.l	(%a0)+,(%a3)+
	dbf	%d1,6b
	move.l	%d3,(%a4)
	move.l	%d3,4(%a4)
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
	lea	F_CODECS(%a2),%a1
	moveq	#15,%d1
	cmp.w	#101,%d0
	beq.s	s_copy
	move.l	%a2,%a1
	moveq	#95,%d1			| 384 bytes
	cmp.w	#102,%d0
	beq.s	s_copy
	lea	F_HDR(%a2),%a1
	moveq	#15,%d1
	cmp.w	#103,%d0
	beq.s	s_copy
	moveq	#statusErr,%d0
s_done:
	movem.l	(%sp)+,%a0-%a1
	movem.l	(%sp)+,%d3-%d4/%a2-%a4
	bra.s	iodone
s_copy:
	move.l	(%a3),%a4
1:	move.l	(%a1)+,(%a4)+
	dbf	%d1,1b
	moveq	#noErr,%d0
	bra.s	s_done
s_info:
	move.l	%a2,(%a3)
	move.l	B_VERSION(%a2),4(%a3)
	move.l	F_VERSION(%a2),8(%a3)
	move.l	F_ENABLED(%a2),12(%a3)
	move.l	#RING_SIZE,16(%a3)
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
