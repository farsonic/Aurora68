| fputest.s - "Aurora68 FPU Test": the exact FPU checked from the 68k's side, in the testbed. MPL-2.0.
| GNU as MIT syntax, 68020 with a 68881.
|
| 22 November 2026 (issue #27, docs/17-exact-fpu.md). tools/fpu_test.c checks the exact FPU's C on the
| host; this checks it where it runs, behind the JIT's call (M68k_LINEF.c, EMIT_FPU_exact): the
| instructions as a program writes them, in the addressing modes programs use, with the rounding modes
| and precisions of the FPCR, the flags of the FPSR, FMOVEM's orders, FSAVE's frame, a branch on a
| comparison, an enabled exception taken through its vector, and the packed decimal format. Each result
| is compared with the bits the 68881 gives (worked out with 60-digit decimal arithmetic or known exactly)
| and the outcome goes to the Pi's log through the card's INFO service, as A68 Machine's SANE test does:
|     [MAC] FPU test: 22 passed, 0 failed
|     [MAC] FPU test: 200000 FADD and FMUL in N ticks; 20000 FSIN in N ticks; ...
| With mac.fpu=double the same INIT shows what Emu68's doubles get wrong. It is a testbed tool, not part of
| the release: make -C mac fputest builds build/Aurora68-FPUTest.bin; tools/mac/aurora68-layout.py install
| puts it in Extensions like any other.

	.text
	.globl	init_start
	.equ	Ticks,	0x016A
	.ifdef	SPEEDONLY
	.equ	NARITH,	1000000			| ten times the loops: Emu68's doubles finish the short ones within a tick
	.equ	NSIN,	200000
	.else
	.equ	NARITH,	100000
	.equ	NSIN,	20000
	.endif

	.macro	expect name, se, hi, lo		| FP0 against an extended value (68881 memory format)
	fmove.x	%fp0,(%a3)
	lea	\name(%pc),%a0
	move.l	#\se,%d0
	move.l	#\hi,%d1
	move.l	#\lo,%d2
	bsr	check
	.endm

	.macro	expect_l name, value		| d0 against a long
	lea	\name(%pc),%a0
	move.l	#\value,%d1
	bsr	check_l
	.endm

init_start:
	movem.l	%d0-%d7/%a0-%a6,-(%sp)
	| no FPU (mac.fpu=none, docs/08 item 103): HWCfgFlags' hwCbFPU clear, as Gestalt tests it. Its first FPU
	| instruction would take the line-F exception and the System's handler would stop the Mac with error 90
	btst	#4,0x0B22
	bne.s	1f
	lea	t_nofpu(%pc),%a1
	bsr	do_log
	movem.l	(%sp)+,%d0-%d7/%a0-%a6
	rts
1:	fmovem.x %fp0-%fp7,-(%sp)
	fmove.l	%fpcr,-(%sp)
	lea	v_r(%pc),%a3			| a3: the result buffer (12 bytes), a4: the failure list
	lea	fails(%pc),%a4
	clr.b	(%a4)
	moveq	#0,%d6				| d6 passed, d7 failed
	moveq	#0,%d7
	fmove.l	#0,%fpcr
	fmove.l	#0,%fpsr
	.ifndef	SPEEDONLY		| make fputest-speed: the timing alone, for mac.fpu=double, which the checks upset

	| (1 + 2^-60) - 1: 2^-60 exactly (doubles give 0), and no flags
	fmove.x	c_tiny(%pc),%fp0
	fsub.x	c_one(%pc),%fp0
	fmove.l	%fpsr,%d5
	expect	n_tiny, 0x3FC3, 0x80000000, 0
	move.l	%d5,%d0
	expect_l n_tinyfl, 0

	| 1/3 in the four rounding modes and three precisions
	fmove.x	c_one(%pc),%fp0
	fdiv.x	c_three(%pc),%fp0
	expect	n_rn, 0x3FFD, 0xAAAAAAAA, 0xAAAAAAAB
	fmove.l	#0x10,%fpcr
	fmove.x	c_one(%pc),%fp0
	fdiv.x	c_three(%pc),%fp0
	expect	n_rz, 0x3FFD, 0xAAAAAAAA, 0xAAAAAAAA
	fmove.l	#0x20,%fpcr
	fmove.x	c_mone(%pc),%fp0
	fdiv.x	c_three(%pc),%fp0
	expect	n_rm, 0xBFFD, 0xAAAAAAAA, 0xAAAAAAAB
	fmove.l	#0x30,%fpcr
	fmove.x	c_one(%pc),%fp0
	fdiv.x	c_three(%pc),%fp0
	expect	n_rp, 0x3FFD, 0xAAAAAAAA, 0xAAAAAAAB
	fmove.l	#0x40,%fpcr
	fmove.x	c_one(%pc),%fp0
	fdiv.x	c_three(%pc),%fp0
	expect	n_single, 0x3FFD, 0xAAAAAB00, 0
	fmove.l	#0x80,%fpcr
	fmove.x	c_one(%pc),%fp0
	fdiv.x	c_three(%pc),%fp0
	expect	n_double, 0x3FFD, 0xAAAAAAAA, 0xAAAAA800
	fmove.l	#0,%fpcr

	| square root, e, sin 1, atan 1, FMOVECR's pi
	fsqrt.x	c_two(%pc),%fp0
	expect	n_sqrt, 0x3FFF, 0xB504F333, 0xF9DE6484
	fetox.x	c_one(%pc),%fp0
	expect	n_etox, 0x4000, 0xADF85458, 0xA2BB4A9B
	fsin.x	c_one(%pc),%fp0
	expect	n_sin, 0x3FFE, 0xD76AA478, 0x48677021
	fatan.x	c_one(%pc),%fp0
	expect	n_atan, 0x3FFE, 0xC90FDAA2, 0x2168C235
	fmovecr.x #0,%fp0
	expect	n_pi, 0x4000, 0xC90FDAA2, 0x2168C235
	flogn.x	c_two(%pc),%fp0
	expect	n_ln2, 0x3FFE, 0xB17217F7, 0xD1CF79AC

	| 0/0: the 68881's default NaN, OPERR, IOP, the NAN condition
	fmove.l	#0,%fpsr
	fmove.x	c_zero(%pc),%fp0
	fdiv.x	c_zero(%pc),%fp0
	fmove.l	%fpsr,%d5			| before the FMOVE out of expect, which sets the exception byte afresh
	expect	n_nan, 0x7FFF, 0xFFFFFFFF, 0xFFFFFFFF
	move.l	%d5,%d0
	expect_l n_nanfl, 0x01002080

	| the memory format's exponent 0: 2^-16383 with the integer bit, times 1.5
	fmove.x	c_den(%pc),%fp0
	fmul.x	c_onehalf(%pc),%fp0
	expect	n_den, 0x0000, 0xC0000000, 0

	| packed decimal in: 1.2345678901234567E+3
	fmove.p	c_packed(%pc),%fp0
	expect	n_packed, 0x4009, 0x9A522C27, 0xE87EF946

	| integer out: 2.5 to nearest is 2, -2.5 toward minus infinity is -3; .L to a data register and .W to memory
	fmove.x	c_twohalf(%pc),%fp0
	fmove.l	%fp0,%d0
	expect_l n_int, 2
	fmove.l	#0x20,%fpcr
	fneg.x	%fp0
	fmove.w	%fp0,(%a3)
	move.w	(%a3),%d0
	ext.l	%d0
	expect_l n_intrm, 0xFFFFFFFD
	fmove.l	#0,%fpcr

	| a comparison and a branch
	fmove.x	c_one(%pc),%fp1
	fmove.x	c_two(%pc),%fp2
	moveq	#0,%d0
	fcmp.x	%fp2,%fp1			| 1 against 2
	fbolt	1f
	moveq	#1,%d0
1:	fbogt	2f
	expect_l n_branch, 0
	bra.s	3f
2:	moveq	#2,%d0
	expect_l n_branch, 0
3:
	| FMOVEM: every register out with -(sp), cleared, back with (sp)+
	fmove.x	c_three(%pc),%fp5
	fmovem.x %fp0-%fp7,-(%sp)
	fmove.x	c_zero(%pc),%fp5
	fmovem.x (%sp)+,%fp0-%fp7
	fmove.x	%fp5,%fp0
	expect	n_movem, 0x4000, 0xC0000000, 0

	| FSAVE after an FPU instruction: the 68882's idle frame
	fnop
	move.l	%sp,%a5
	fsave	-(%sp)
	move.w	(%sp),%d0
	ext.l	%d0
	frestore (%sp)+
	cmp.l	%a5,%sp
	beq.s	4f
	moveq	#-1,%d0				| the frame's size was not stepped over
4:	expect_l n_fsave, 0x1F38

	| an enabled exception: OPERR through vector 52, the handler counting it
	movec	%vbr,%a5
	move.l	52*4(%a5),-(%sp)
	lea	trap_h(%pc),%a0
	move.l	%a0,52*4(%a5)
	lea	v_trap(%pc),%a0
	clr.l	(%a0)
	fmove.l	#0x2000,%fpcr
	fmove.x	c_zero(%pc),%fp0
	fdiv.x	c_zero(%pc),%fp0
	nop
	fmove.l	#0,%fpcr
	move.l	(%sp)+,52*4(%a5)
	move.l	v_trap(%pc),%d0
	expect_l n_trap, 1

	| the summary
	lea	msg(%pc),%a1
	lea	t_head(%pc),%a0
	bsr	puts
	move.l	%d6,%d0
	bsr	putdec
	lea	t_passed(%pc),%a0
	bsr	puts
	move.l	%d7,%d0
	bsr	putdec
	lea	t_failed(%pc),%a0
	bsr	puts
	lea	fails(%pc),%a0
	bsr	puts
	clr.b	(%a1)
	lea	msg(%pc),%a1
	bsr	do_log
	.endif

	| speed: register arithmetic, memory operands, a transcendental, a mixed loop
	lea	msg(%pc),%a1
	lea	t_speed(%pc),%a0
	bsr	puts
	fmove.x	c_one(%pc),%fp0
	fmove.x	c_onehalf(%pc),%fp1
	fmove.x	c_twothirds(%pc),%fp2
	move.l	Ticks,%d4
	move.l	#NARITH,%d3
5:	fadd.x	%fp1,%fp0
	fmul.x	%fp2,%fp0
	subq.l	#1,%d3
	bne.s	5b
	move.l	Ticks,%d0
	sub.l	%d4,%d0
	bsr	putdec
	lea	t_regs(%pc),%a0
	bsr	puts
	lea	c_onehalf(%pc),%a2
	move.l	Ticks,%d4
	move.l	#NARITH,%d3
6:	fadd.x	(%a2),%fp0
	fmul.d	c_dhalf(%pc),%fp0
	subq.l	#1,%d3
	bne.s	6b
	move.l	Ticks,%d0
	sub.l	%d4,%d0
	bsr	putdec
	lea	t_mem(%pc),%a0
	bsr	puts
	move.l	Ticks,%d4
	move.l	#NSIN,%d3
7:	fsin.x	%fp1,%fp0
	subq.l	#1,%d3
	bne.s	7b
	move.l	Ticks,%d0
	sub.l	%d4,%d0
	bsr	putdec
	lea	t_sin(%pc),%a0
	bsr	puts
	clr.b	(%a1)
	lea	msg(%pc),%a1
	bsr	do_log

	fmove.l	(%sp)+,%fpcr
	fmovem.x (%sp)+,%fp0-%fp7
	movem.l	(%sp)+,%d0-%d7/%a0-%a6
	rts

trap_h:	move.l	%a0,-(%sp)
	lea	v_trap(%pc),%a0
	addq.l	#1,(%a0)
	move.l	(%sp)+,%a0
	rte

| the result at (a3) against d0 (sign and exponent), d1, d2; a0 the test's name
check:	cmp.w	(%a3),%d0
	bne.s	1f
	cmp.l	4(%a3),%d1
	bne.s	1f
	cmp.l	8(%a3),%d2
	bne.s	1f
	addq.l	#1,%d6
	rts
1:	addq.l	#1,%d7
	move.l	%a1,-(%sp)
	move.l	%a4,%a1
	bsr	puts
	move.b	#'=',(%a1)+
	moveq	#0,%d0
	move.w	(%a3),%d0
	bsr	puthex
	move.b	#':',(%a1)+
	move.l	4(%a3),%d0
	bsr	puthex
	move.l	8(%a3),%d0
	bsr	puthex
	move.b	#' ',(%a1)+
	clr.b	(%a1)
	move.l	%a1,%a4
	move.l	(%sp)+,%a1
	rts

| d0 against d1; a0 the test's name
check_l: cmp.l	%d0,%d1
	bne.s	1f
	addq.l	#1,%d6
	rts
1:	addq.l	#1,%d7
	move.l	%a1,-(%sp)
	move.l	%a4,%a1
	bsr	puts
	move.b	#'=',(%a1)+
	bsr	puthex
	move.b	#' ',(%a1)+
	clr.b	(%a1)
	move.l	%a1,%a4
	move.l	(%sp)+,%a1
	rts

| the message at a1 to the Pi's log (the card's INFO service, as mac/pimach.s does)
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

puts:	move.b	(%a0)+,(%a1)+
	bne.s	puts
	subq.l	#1,%a1
	rts
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
c_one:	.long	0x3FFF0000,0x80000000,0x00000000
c_mone:	.long	0xBFFF0000,0x80000000,0x00000000
c_two:	.long	0x40000000,0x80000000,0x00000000
c_three: .long	0x40000000,0xC0000000,0x00000000
c_zero:	.long	0x00000000,0x00000000,0x00000000
c_onehalf: .long 0x3FFF0000,0xC0000000,0x00000000
c_twohalf: .long 0x40000000,0xA0000000,0x00000000
c_twothirds: .long 0x3FFE0000,0xAAAAAAAA,0xAAAAAAAB
c_tiny:	.long	0x3FFF0000,0x80000000,0x00000008	| 1 + 2^-60
c_den:	.long	0x00000000,0x80000000,0x00000000	| 2^-16383: exponent 0, the integer bit set
c_packed: .long	0x00030001,0x23456789,0x01234567	| 1.2345678901234567E+3
c_dhalf: .long	0x3FE00000,0x00000000			| 0.5 as a double
v_r:	.space	12
v_trap:	.long	0
n_tiny:	.asciz	"tiny"
n_tinyfl: .asciz "tiny-fpsr"
n_rn:	.asciz	"1/3-RN"
n_rz:	.asciz	"1/3-RZ"
n_rm:	.asciz	"-1/3-RM"
n_rp:	.asciz	"1/3-RP"
n_single: .asciz "1/3-single"
n_double: .asciz "1/3-double"
n_sqrt:	.asciz	"sqrt2"
n_etox:	.asciz	"etox1"
n_sin:	.asciz	"sin1"
n_atan:	.asciz	"atan1"
n_pi:	.asciz	"fmovecr-pi"
n_ln2:	.asciz	"ln2"
n_nan:	.asciz	"0/0"
n_nanfl: .asciz	"0/0-fpsr"
n_den:	.asciz	"denormal"
n_packed: .asciz "packed"
n_int:	.asciz	"fmove.l"
n_intrm: .asciz	"fmove.w-RM"
n_branch: .asciz "fbcc"
n_movem: .asciz	"fmovem"
n_fsave: .asciz	"fsave"
n_trap:	.asciz	"operr-trap"
t_head:	.asciz	"FPU test: "
t_nofpu: .asciz	"FPU test: no FPU (HWCfgFlags hwCbFPU clear), nothing tested"
t_passed: .asciz " passed, "
t_failed: .asciz " failed "
	.ifdef	SPEEDONLY
t_speed: .asciz	"FPU test speed: 2000000 FADD and FMUL (registers) in "
t_regs:	.asciz	" ticks, 2000000 (memory, .X and .D) in "
t_mem:	.asciz	" ticks, 200000 FSIN in "
	.else
t_speed: .asciz	"FPU test speed: 200000 FADD and FMUL (registers) in "
t_regs:	.asciz	" ticks, 200000 (memory, .X and .D) in "
t_mem:	.asciz	" ticks, 20000 FSIN in "
	.endif

t_sin:	.asciz	" ticks"
	.even
fails:	.space	400
msg:	.space	500
