| machtest.s - "Mach Test": what Gestalt 'mach' and SysEnvirons answer this program. MPL-2.0.
| GNU as MIT syntax, 68020, packaged by tools/mac/mkapp.py under three names (make -C mac machtest;
| not part of `all`: a test).
|
| Aurora68 Machine (pimach.s) makes Gestalt 'mach' answer 5, the Macintosh SE, but gives A/UX Startup
| the ROM's own answer (11, the IIci), since A/UX Startup refuses the compact Macs' types (docs/08 item
| 150). Which program is asking is told by the current application's name and by its signature
| resource, so the one program is built three times:
|   build/MachTest-AUX.bin      named "A/UX Startup", creator SASH: the name and the creator match (11)
|   build/MachTest-SASH.bin     named "Renamed Startup", creator SASH, with its signature resource:
|                               the creator alone matches (11)
|   build/MachTest.bin          named "Mach Test", creator PIMT: neither (5)
| Each logs one line through the Pi's INFO service and quits: put them in Startup Items and read the
| kernel's log, e.g. "[MAC] Mach Test (A/UX Startup): Gestalt mach 11, SysEnvirons machineType 9".

	.text
	.globl	app_start
	.equ	_Gestalt,	0xA1AD
	.equ	_SysEnvirons,	0xA090
	.equ	_ExitToShell,	0xA9F4
	.equ	MMU32Bit,	0x0CB2
	.equ	CurApName,	0x0910

app_start:
	lea	msg(%pc),%a1
	lea	t_head(%pc),%a0
	bsr	puts
	lea	CurApName,%a0			| the application's name, a Pascal string
	moveq	#0,%d1
	move.b	(%a0)+,%d1
	cmp.w	#31,%d1
	bls.s	2f
	moveq	#31,%d1
	bra.s	2f
1:	move.b	(%a0)+,(%a1)+
2:	dbf	%d1,1b
	lea	t_gestalt(%pc),%a0
	bsr	puts
	move.l	#0x6D616368,%d0			| 'mach'
	.short	_Gestalt			| D0 = selector -> A0 = response, D0 = error
	tst.w	%d0
	beq.s	3f
	lea	t_err(%pc),%a0
	bsr	puts
	neg.w	%d0
	ext.l	%d0
	bra.s	4f
3:	move.l	%a0,%d0
4:	bsr	putdec
	lea	t_env(%pc),%a0
	bsr	puts
	lea	envrec(%pc),%a0
	moveq	#1,%d0				| the record's version
	.short	_SysEnvirons			| A0 = the record, D0 = version -> D0 = error
	lea	envrec(%pc),%a0
	moveq	#0,%d0
	move.w	2(%a0),%d0			| machineType
	bsr	putdec
	clr.b	(%a1)
	lea	msg(%pc),%a1
	bsr	do_log
	.short	_ExitToShell

| the Pi's log through the INFO service of the slot-9 window: a1 = C string
do_log:
	lea	0xF9000000,%a0
	tst.b	MMU32Bit
	bne.s	1f
	lea	0x00900000,%a0
1:	lea	0x1000(%a0),%a0
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
	move.l	#50000000,%d1			| the Pi reads the text when it gets to the command: wait for it
4:	cmp.l	4(%a0),%d0
	beq.s	5f
	subq.l	#1,%d1
	bne.s	4b
5:	rts
| a0 = C string into a1 (advanced)
puts:	move.b	(%a0)+,(%a1)+
	bne.s	puts
	subq.l	#1,%a1
	rts
| d0 unsigned, in decimal, at a1 (advanced)
putdec:	movem.l	%d1-%d2,-(%sp)
	moveq	#0,%d2
1:	divul.l	#10,%d1:%d0
	add.b	#'0',%d1
	move.b	%d1,-(%sp)
	addq.w	#1,%d2
	tst.l	%d0
	bne.s	1b
2:	move.b	(%sp)+,(%a1)+
	subq.w	#1,%d2
	bne.s	2b
	movem.l	(%sp)+,%d1-%d2
	rts

t_head:	.asciz	"Mach Test ("
t_gestalt: .asciz	"): Gestalt mach "
t_err:	.asciz	"error -"
t_env:	.asciz	", SysEnvirons machineType "
	.even
envrec:	.space	16
msg:	.space	128
