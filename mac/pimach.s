| pimach.s — "PiMach": tell the Mac it is a Macintosh SE. MPL-2.0. GNU as MIT syntax, 68020.
|
| The IIci ROM identifies the machine it was made for: its Gestalt Manager answers the 'mach'
| selector with 11, gestaltMacIIci, and "About This Macintosh", the Finder and every program
| that asks get the IIci's name and picture. The machine is a Macintosh SE with a PiStorm in its
| 68000 socket, and its owner may prefer it to say so. Changing the ROM's box table would also
| change the ROM's own decisions about the hardware around it, which are correct for the IIci
| it thinks it is (docs/00); the Gestalt answer can be changed on its own, with the Gestalt
| Manager's ReplaceGestalt, from an extension that runs after the Gestalt Manager exists and
| before the Finder asks. That is this file: an INIT that installs a 'mach' selector function
| returning 5, gestaltMacSE (Inside Macintosh: Operating System Utilities, "Gestalt Manager";
| the machine types are in Gestalt.h: gestaltClassic 1 ... gestaltMacSE 5, gestaltMacIIci 11).
|
| SysEnvirons, the older call that System 6's Finder and pre-Gestalt programs use, gets a tail
| patch that turns its "IIci" into "SE" as well. Both routines must outlive the INIT, so they are
| copied into a block in the system heap.
| Everything else the ROM says about itself (processor, ROM version, slots) is left as it is:
| a program that asks for the processor still hears 68030, which is what the kernel presents.
| Exact SANE (22 November 2026, docs/08 item 101): the IIci ROM's SANE packages, PACK 4 (FP68K) and
| PACK 5 (Elems68K), are the versions for a 68881/68882 and compute with FPU instructions, which
| Emu68 carries out in 64-bit doubles: extended precision loses eleven bits, and a program that
| refines a result until it settles in extended precision (Photoshop at its start) may never stop.
| The SE ROM's packages do extended arithmetic exactly in integer instructions. The kernel reads
| them from the SE's own ROM (or an image named by mac.serom=) and hands them over with the INFO
| service's PSMB_INFO_SANE; sane_setup below copies each into a locked system-heap handle and puts
| it in AppPacks ($0AB8 + 4n), where the ROM's package dispatcher ($40826206 for _Pack4) looks.
| A short test runs before and after and goes to the Pi's log: (1 + 2^-60) - 1 in extended (2^-60
| when exact, 0 in doubles), Newton's square root of 2 (steps to settle), additions a second.
| mac.sane=rom on the Pi keeps the IIci ROM's SANE (the test still runs once).
| Build: make -C mac (PiMach.bin); copy into the System Folder (System 6) or Extensions (7).

	.text
	.globl	init_start
	.equ	_NewPtrSys,	0xA51E
	.equ	_ReplaceGestalt, 0xA5AD
	.equ	_Gestalt,	0xA1AD
	.equ	_NGetTrapAddress, 0xA346	| OS-trap form (bit 9 set)
	.equ	_NSetTrapAddress, 0xA247
	.equ	trapSysEnvirons, 0xA090
	.equ	envMacIIci,	9		| SysEnvirons' own machine codes (Inside Macintosh: Operating
	.equ	envSE,		3		| System Utilities, "System Environment Record"), not Gestalt's
	.ifdef	MACH
	.equ	gestaltMacSE,	MACH		| another type for experiments (make MACH=11 is a IIci again)
	.else
	.equ	gestaltMacSE,	5
	.endif

init_start:
	movem.l	%d0-%d2/%a0-%a2,-(%sp)
	move.l	#sel_end-sel_start,%d0
	.short	_NewPtrSys
	move.l	%a0,%d0
	beq.s	done			| no memory: stay a IIci
	move.l	%a0,%a1
	lea	sel_start(%pc),%a2
	move.l	#sel_end-sel_start,%d1
1:	move.b	(%a2)+,(%a1)+
	subq.l	#1,%d1
	bne.s	1b
	move.l	%a0,%a2			| the block: the selector function first, then the SysEnvirons patch
	move.l	#0x6D616368,%d0		| 'mach'
	.short	_ReplaceGestalt	| A0 = the new function, D0 = the selector; returns A0 = the old one
	tst.w	%d0
	bne.s	bad
	| SysEnvirons too: the System 6 Finder's "About the Finder" and programs older than the
	| Gestalt Manager ask it, and it answers from the ROM's box table. A tail patch: the old
	| routine runs, then an answer of "IIci" in the record's machineType word becomes "SE".
	move.w	#trapSysEnvirons,%d0
	.short	_NGetTrapAddress	| D0 = trap -> A0 = its routine
	move.l	%a0,env_old-sel_start(%a2)
	lea	env_patch-sel_start(%a2),%a0
	move.w	#trapSysEnvirons,%d0
	.short	_NSetTrapAddress	| A0 = routine, D0 = trap
	| read it back and say so in the Pi's log: "PiMach: Gestalt mach now N" (one digit, 1-9)
	move.l	#0x6D616368,%d0
	.short	_Gestalt		| D0 = selector -> A0 = response, D0 = error
	tst.w	%d0
	bne.s	bad
	move.l	%a0,%d0
	add.b	#'0',%d0
	lea	ok_digit(%pc),%a0
	move.b	%d0,(%a0)
	lea	ok_text(%pc),%a1
	bsr	do_log
done:
	bsr	sane_setup
	movem.l	(%sp)+,%d0-%d2/%a0-%a2
	rts
bad:	lea	bad_text(%pc),%a1
	bsr	do_log
	bra.s	done

| the Pi's log through the INFO service of the slot-9 window (as pidisk_init.s does it):
| a1 = NUL-terminated text; waits for the service core's answer, bounded
do_log:
	lea	0xF9000000,%a0
	tst.b	0x0CB2			| MMU32Bit: 24-bit mode uses the slot's 24-bit alias
	bne.s	1f
	lea	0x00900000,%a0
1:	lea	0x1000(%a0),%a0		| INFO command block
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

| ---- exact SANE: the SE ROM's packages in AppPacks (see the top of the file) ----
	.equ	_NewHandleSys,	0xA522
	.equ	_HLock,		0xA029
	.equ	_DisposHandle,	0xA023
	.equ	_GetToolTrapAddress, 0xA746
	.equ	_SetToolTrapAddress, 0xA647
	.equ	_FP68K,		0xA9EB
	.equ	Ticks,		0x016A
	.equ	AppPacks,	0x0AB8
	.equ	INFO_SANE,	13
	.equ	FOADD,		0x0000		| SANE operations, extended operands (Apple Numerics Manual)
	.equ	FOSUB,		0x0002
	.equ	FOMUL,		0x0004
	.equ	FODIV,		0x0006
	.equ	FOCMP,		0x0008

	.macro	fpop op, src, dst		| dst := dst op src, through _FP68K
	pea	\src(%pc)
	pea	\dst(%pc)
	move.w	#\op,-(%sp)
	.short	_FP68K
	.endm
	.macro	copy10 src, dst			| one 10-byte extended number
	lea	\src(%pc),%a0
	lea	\dst(%pc),%a1
	move.l	(%a0)+,(%a1)+
	move.l	(%a0)+,(%a1)+
	move.w	(%a0),(%a1)
	.endm

sane_setup:
	movem.l	%d0-%d7/%a0-%a4,-(%sp)
	lea	t_rom(%pc),%a2
	bsr	sane_report			| the IIci ROM's SANE, as it is
	moveq	#4,%d3
	bsr	install_pack
	tst.l	%d0
	bne.s	9f
	moveq	#5,%d3
	bsr	install_pack
	lea	t_se(%pc),%a2
	bsr	sane_report			| and the SE ROM's
9:	movem.l	(%sp)+,%d0-%d7/%a0-%a4
	rts

| d3 = 4 or 5: the package from the Pi into a system-heap handle in AppPacks; d0 = 0, else a status
install_pack:
	moveq	#INFO_SANE,%d0
	move.l	%d3,%d1
	sub.l	%a1,%a1
	moveq	#0,%d2
	bsr	info				| -> d0 status, d1 the size
	tst.l	%d0
	bne	no_pack
	move.l	%d1,%d4
	move.l	%d4,%d0
	.short	_NewHandleSys			| d0 = size -> a0 = handle
	tst.w	%d0
	bne	no_mem
	move.l	%a0,%a3
	.short	_HLock
	moveq	#INFO_SANE,%d0
	move.l	%d3,%d1
	move.l	(%a3),%a1
	move.l	%d4,%d2
	bsr	info
	tst.l	%d0
	bne.s	1f
	cmp.l	%d4,%d1
	beq.s	2f
1:	move.l	%a3,%a0
	.short	_DisposHandle
	moveq	#-4,%d0
	bra	no_pack
2:	move.l	%d3,%d0				| AppPacks[n]: the old handle kept for the log and the trap test
	lsl.l	#2,%d0
	lea	AppPacks,%a4
	add.l	%d0,%a4
	move.l	(%a4),%d5			| old handle
	move.l	%a3,(%a4)
	move.w	#0x01E7,%d0			| _Pack4 is $A9EB, _Pack5 $A9EC
	add.w	%d3,%d0
	.short	_GetToolTrapAddress		| d0 = trap -> a0
	move.l	%a0,%d6
	| a trap that goes straight into the old package (a patch that skips the dispatcher) goes to the new one
	tst.l	%d5
	beq.s	3f
	move.l	%d5,%a0
	move.l	(%a0),%d7			| the old package's address
	move.l	%d6,%d0
	sub.l	%d7,%d0
	cmp.l	#0x4000,%d0
	bhs.s	3f
	move.l	(%a3),%a0
	move.w	#0x01E7,%d0
	add.w	%d3,%d0
	.short	_SetToolTrapAddress		| a0 = routine, d0 = trap
	move.l	(%a3),%d6
3:	| "A68 Machine: SANE PACK n from the SE ROM: SIZE bytes at ADDR (was OLD), trap TRAP"
	lea	msg(%pc),%a1
	lea	t_pack(%pc),%a0
	bsr	puts
	move.b	%d3,%d0
	add.b	#'0',%d0
	move.b	%d0,(%a1)+
	lea	t_from(%pc),%a0
	bsr	puts
	move.l	%d4,%d0
	bsr	putdec
	lea	t_at(%pc),%a0
	bsr	puts
	move.l	(%a3),%d0
	bsr	puthex
	lea	t_was(%pc),%a0
	bsr	puts
	moveq	#0,%d0
	tst.l	%d5
	beq.s	4f
	move.l	%d5,%a0
	move.l	(%a0),%d0
4:	bsr	puthex
	lea	t_trap(%pc),%a0
	bsr	puts
	move.l	%d6,%d0
	bsr	puthex
	clr.b	(%a1)
	lea	msg(%pc),%a1
	bsr	do_log
	moveq	#0,%d0
	rts
no_mem:	moveq	#-3,%d0
no_pack:
	move.l	%d0,-(%sp)
	lea	msg(%pc),%a1
	lea	t_none(%pc),%a0
	bsr	puts
	move.l	(%sp),%d0
	neg.l	%d0
	bsr	putdec
	lea	t_none2(%pc),%a0
	bsr	puts
	clr.b	(%a1)
	lea	msg(%pc),%a1
	bsr	do_log
	move.l	(%sp)+,%d0
	rts

| the test, then one log line: a2 = which SANE (a C string)
sane_report:
	bsr	fp_check			| d5 result kind, d6 Newton steps, d7 additions in 20 ticks
	lea	msg(%pc),%a1
	lea	t_sane(%pc),%a0
	bsr	puts
	move.l	%a2,%a0
	bsr	puts
	lea	t_tiny(%pc),%a0
	bsr	puts
	lea	t_exact(%pc),%a0
	tst.l	%d5
	beq.s	1f
	lea	t_zero(%pc),%a0
	cmp.l	#1,%d5
	beq.s	1f
	lea	t_other(%pc),%a0
1:	bsr	puts
	lea	t_newton(%pc),%a0
	bsr	puts
	move.l	%d6,%d0
	bsr	putdec
	lea	t_adds(%pc),%a0
	bsr	puts
	move.l	%d7,%d0
	mulu.l	#3,%d0				| 20 ticks are a third of a second
	bsr	putdec
	lea	t_persec(%pc),%a0
	bsr	puts
	clr.b	(%a1)
	lea	msg(%pc),%a1
	bra	do_log

fp_check:
	copy10	c_tiny, v_y			| y = 1 + 2^-60
	fpop	FOSUB, c_one, v_y		| y = y - 1
	lea	v_y(%pc),%a0
	moveq	#2,%d5				| something else
	move.w	(%a0),%d0
	bne.s	1f
	tst.l	2(%a0)
	bne.s	3f
	tst.l	6(%a0)
	bne.s	3f
	moveq	#1,%d5				| zero: the 64-bit result
	bra.s	3f
1:	cmp.w	#0x3FC3,%d0			| 2^-60: exponent 16383 - 60, the integer bit alone
	bne.s	3f
	cmp.l	#0x80000000,2(%a0)
	bne.s	3f
	tst.l	6(%a0)
	bne.s	3f
	moveq	#0,%d5
3:	| Newton: x := (x + 2/x) / 2 from 1, until it stops changing (at most 60 steps)
	copy10	c_one, v_x
	moveq	#0,%d6
4:	copy10	v_x, v_old
	copy10	c_two, v_t
	fpop	FODIV, v_x, v_t			| t = 2 / x
	fpop	FOADD, v_x, v_t			| t = t + x
	fpop	FOMUL, c_half, v_t		| t = t / 2
	copy10	v_t, v_x
	addq.l	#1,%d6
	fpop	FOCMP, v_old, v_x		| CCR Z: x = old
	beq.s	5f
	cmp.l	#60,%d6
	blt.s	4b
5:	| additions a third of a second can hold
	copy10	c_one, v_t
	moveq	#0,%d7
	move.l	Ticks,%d4
	add.l	#20,%d4
6:	fpop	FOADD, c_tiny, v_t
	addq.l	#1,%d7
	cmp.l	Ticks,%d4
	bhi.s	6b
	rts

| the INFO service: d0.w command, d1 arg0, a1 buffer, d2 length -> d0 status, d1 result.
| Waits for the Pi's answer for up to five seconds (the SE's ROM is read over the wire meanwhile).
info:
	movem.l	%d3/%a2,-(%sp)
	lea	0xF9000000,%a2
	tst.b	0x0CB2			| MMU32Bit: 24-bit mode uses the slot's 24-bit alias
	bne.s	1f
	lea	0x00900000,%a2
1:	lea	0x1000(%a2),%a2
	move.w	%d0,8(%a2)
	move.l	%d1,12(%a2)
	move.l	%a1,28(%a2)
	move.l	%d2,32(%a2)
	move.l	(%a2),%d0
	addq.l	#1,%d0
	move.l	%d0,(%a2)
	move.l	Ticks,%d3
	add.l	#300,%d3
2:	cmp.l	4(%a2),%d0
	beq.s	3f
	cmp.l	Ticks,%d3
	bhi.s	2b
	moveq	#-8,%d0			| no answer: PSMB_E_BUSY
	bra.s	4f
3:	move.l	36(%a2),%d0
	move.l	40(%a2),%d1
4:	movem.l	(%sp)+,%d3/%a2
	rts

| strings into the message: a0 = C string, a1 = where (advanced)
puts:	move.b	(%a0)+,(%a1)+
	bne.s	puts
	subq.l	#1,%a1
	rts
| d0 unsigned, in decimal, at a1 (advanced)
putdec:	movem.l	%d1-%d2,-(%sp)
	moveq	#0,%d2
1:	divul.l	#10,%d1:%d0		| d0 / 10 -> d0, remainder d1
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
| d0 as eight hex digits at a1 (advanced)
puthex:	movem.l	%d1-%d2,-(%sp)
	moveq	#7,%d2
1:	rol.l	#4,%d0
	move.b	%d0,%d1
	and.b	#15,%d1
	add.b	#'0',%d1
	cmp.b	#'9',%d1
	bls.s	2f
	addq.b	#7,%d1
2:	move.b	%d1,(%a1)+
	dbf	%d2,1b
	movem.l	(%sp)+,%d1-%d2
	rts

	.even
c_one:	.word	0x3FFF
	.long	0x80000000,0x00000000
c_two:	.word	0x4000
	.long	0x80000000,0x00000000
c_half:	.word	0x3FFE
	.long	0x80000000,0x00000000
c_tiny:	.word	0x3FFF			| 1 + 2^-60
	.long	0x80000000,0x00000008
v_y:	.space	10
v_x:	.space	10
v_old:	.space	10
v_t:	.space	10
t_rom:	.asciz	"the IIci ROM's"
t_se:	.asciz	"the SE ROM's"
t_sane:	.asciz	"A68 Machine: SANE, "
t_tiny:	.asciz	": (1 + 2^-60) - 1 = "
t_exact: .asciz	"2^-60 (exact)"
t_zero:	.asciz	"0 (64-bit)"
t_other: .asciz	"something else"
t_newton: .asciz ", square root of 2 settled in "
t_adds:	.asciz	" steps, "
t_persec: .asciz " additions a second"
t_pack:	.asciz	"A68 Machine: SANE PACK "
t_from:	.asciz	" from the SE ROM installed: "
t_at:	.asciz	" bytes at "
t_was:	.asciz	" (the ROM's was at "
t_trap:	.asciz	"), trap at "
t_none:	.asciz	"A68 Machine: SANE: the IIci ROM's kept (Pi status -"
t_none2: .asciz	")"
	.even
msg:	.space	160
	.even
ok_text:	.ascii	"A68 Machine: Gestalt mach now "
ok_digit:	.ascii	"?"
	.byte	0
bad_text:	.ascii	"A68 Machine: ReplaceGestalt or Gestalt failed"
	.byte	0
	.even

| the selector function: FUNCTION (selector: OSType; VAR response: LongInt): OSErr, Pascal
| stack: 4(sp) response pointer, 8(sp) selector, 12(sp) the result word
sel_start:
	move.l	4(%sp),%a0
	move.l	#gestaltMacSE,(%a0)
	clr.w	12(%sp)			| noErr
	rtd	#8
| the SysEnvirons patch: A0 = the record, D0.w = the version asked for; returns D0 = error.
| The old routine's address is kept right behind it (filled in by the INIT).
env_patch:
	move.l	%a0,-(%sp)
	move.l	env_old(%pc),%a1
	jsr	(%a1)
	move.l	(%sp)+,%a0
	cmp.w	#envMacIIci,2(%a0)	| machineType
	bne.s	1f
	move.w	#envSE,2(%a0)
1:	rts
env_old:	.long	0
sel_end:
