| SPDX-License-Identifier: MPL-2.0
| mmutest.s - the 68030 MMU's page faults, from the 68k's side: a bare test "ROM" for the solo board. MPL-2.0.
| GNU as MIT syntax, 68030.
|
| 25 November 2026 (docs/23-mmu.md, the first spike of the real-MMU work). The kernel's mac.mmu=full turns a
| translated access to a page the guest's tables mark invalid into a 68030 bus error (psmac_mmufault.c). This
| program checks that the way System 7's Virtual Memory and A/UX would use it: it builds its own 68030 tables
| (4 KiB pages, TIA/TIB/TIC 4/8/8, everything mapped one to one), marks a page invalid, touches it, and its
| bus-error handler maps the page again, flushes and returns with RTE; the access must then complete. The
| cases are the ones where a restart is hard: a plain read and write, MOVE (An)+,(Am)+ whose second access
| faults after the first has moved An, MOVEM reading across into an invalid page, MOVEM -(An) faulting after
| its first store has moved An, a read-modify-write, ADDX -(An),-(Am) with X clear and set, CMPM, a fault in
| user mode, condition codes set just before the faulting instruction, and a hundred faults in a row. Each
| checks the data, the registers, the number of faults and, where it matters, the frame (format $A, the PC of
| the faulting instruction, the special status word).
|
| It needs no Apple software: the kernel loads any ROM image (psmac_bus.c), and this one is a few KiB that the
| kernel repeats over the ROM's 512 KiB. Run it with
|     make -C mac mmutest
|     ROM=mac/build/mmutest.rom tools/run-solo.sh 45 mac.mmu=full
| (run-solo.sh still names the usual disk image and declaration ROM; this program reads neither) and read the
| result in the kernel log:
|     [MAC] MMU test: 50 passed, 0 failed, 112 bus errors taken
| Without mac.mmu=full the bus errors are never delivered (the shim reads $BAD0BAD0 and drops writes), so the
| tests fail; that run shows the stock behaviour. After the tests the program stays in a benchmark loop
| (copy, arithmetic, MOVEM, BSR/RTS) whose speed the heartbeat reports ("JIT: N M instructions in 5 s"),
| which is how the cost of the option is measured.

	.text
	.equ	VBRT,	0x00060000		| vector table
	.equ	TBLA,	0x00061000		| level A: 16 short descriptors (256 MiB each)
	.equ	TBLB,	0x00061100		| level B for the first 256 MiB: 256 (1 MiB each)
	.equ	TBLC,	0x00061800		| level C for 1-2 MiB: 256 (4 KiB each), the test window
	.equ	ROOT,	0x00062100		| CRP (8 bytes) and TC
	.equ	V_COUNT, 0x00062000		| bus errors the handler took
	.equ	V_FMT,	0x00062004		| last frame's format/vector word
	.equ	V_SSW,	0x00062006		| last frame's special status word
	.equ	V_PC,	0x00062008		| last frame's PC
	.equ	V_ADDR,	0x0006200C		| last frame's fault address
	.equ	V_TOTAL, 0x00062010		| all bus errors
	.equ	PASS,	0x00062018
	.equ	FAIL,	0x0006201C
	.equ	FPTR,	0x00062020		| end of the failure list
	.equ	R0,	0x00062200		| results saved right after an instruction
	.equ	MSG,	0x00063000		| a log line
	.equ	FAILS,	0x00063100		| the failure list (names and values)
	.equ	SSP,	0x00070000
	.equ	USP,	0x00180000
	.equ	WIN,	0x00100000		| the window that table C maps page by page
	.equ	BUF,	0x00090000		| benchmark buffers
	.equ	MBOX,	0xF9001000		| the card's INFO command block (docs/07)

	.macro	page	n			| address of test page n in the window
	.long	WIN + \n * 0x1000
	.endm

	.macro	want	name, src, val		| a long (any source) against a value
	move.l	\src,%d0
	move.l	#\val,%d1
	lea	\name(%pc),%a1
	bsr	want_eq
	.endm

	.macro	wantpc	name, label		| the last frame's PC against a label's address
	move.l	V_PC,%d0
	lea	\label(%pc),%a1
	move.l	%a1,%d1
	lea	\name(%pc),%a1
	bsr	want_eq
	.endm

	.macro	inval	n			| mark test page n invalid and flush
	move.l	#\n,%d0
	bsr	invalidate
	.endm

rom:	.long	SSP				| reset: SSP, PC (the kernel starts the 68k from these)
	.long	0x40800000 + (start - rom)

start:	move.w	#0x2700,%sr
	lea	SSP,%sp
	lea	VBRT,%a0			| vectors: everything to "unexpected", 2 to the bus-error handler, 32 to trap0
	movec	%a0,%vbr
	lea	unexpected(%pc),%a1
	move.w	#255,%d0
1:	move.l	%a1,(%a0)+
	dbra	%d0,1b
	lea	berr(%pc),%a1
	move.l	%a1,VBRT+8
	lea	trap0(%pc),%a1
	move.l	%a1,VBRT+0x80
	clr.l	V_COUNT
	clr.l	V_TOTAL
	clr.l	PASS
	clr.l	FAIL
	move.l	#FAILS,FPTR
	clr.b	FAILS

	| the tables: everything one to one; level A entry 0 -> table B, B entry 1 -> table C
	lea	TBLA,%a0
	move.l	#TBLB+2,(%a0)+			| DT 2: a table of 4-byte descriptors
	move.l	#0x10000001,%d0			| DT 1: page descriptor (early termination)
	moveq	#14,%d1
2:	move.l	%d0,(%a0)+
	add.l	#0x10000000,%d0
	dbra	%d1,2b
	lea	TBLB,%a0
	moveq	#1,%d0
	move.w	#255,%d1
3:	move.l	%d0,(%a0)+
	add.l	#0x00100000,%d0
	dbra	%d1,3b
	move.l	#TBLC+2,TBLB+4
	lea	TBLC,%a0
	move.l	#WIN+1,%d0
	move.w	#255,%d1
4:	move.l	%d0,(%a0)+
	add.l	#0x1000,%d0
	dbra	%d1,4b
	lea	ROOT,%a0
	move.l	#0x7FFF0002,(%a0)		| CRP: no limit, DT 2
	move.l	#TBLA,4(%a0)
	move.l	#0x80C04880,8(%a0)		| TC: E, PS 4 KiB, IS 0, TIA 4, TIB 8, TIC 8
	pmove	(%a0),%crp
	lea	8(%a0),%a0
	pmove	(%a0),%tc

	| ---- T1: a read from an invalid page ----
	move.l	#0x11111111,WIN+0x1000+0x10
	inval	1
	clr.l	V_COUNT
	lea	WIN+0x1000+0x10,%a0
t1:	move.l	(%a0),%d2
	move.l	%d2,R0
	want	n_t1, R0, 0x11111111
	want	n_t1c, V_COUNT, 1
	wantpc	n_t1pc, t1
	moveq	#0,%d0
	move.w	V_FMT,%d0
	want	n_t1f, %d0, 0xA008
	moveq	#0,%d0
	move.w	V_SSW,%d0
	want	n_t1s, %d0, 0x0145		| DF, read, long, supervisor data
	want	n_t1a, V_ADDR, WIN+0x1000+0x10

	| ---- T2: a write ----
	inval	2
	clr.l	V_COUNT
	lea	WIN+0x2000+0x20,%a0
	move.l	#0x22222222,%d1
t2:	move.l	%d1,(%a0)
	want	n_t2, WIN+0x2000+0x20, 0x22222222
	want	n_t2c, V_COUNT, 1
	moveq	#0,%d0
	move.w	V_SSW,%d0
	want	n_t2s, %d0, 0x0105		| DF, write, long, supervisor data

	| ---- T3: MOVE (An)+,(Am)+, the second access faulting after the first moved An ----
	move.l	#0x33333333,WIN+0x3000+0x100
	inval	4
	clr.l	V_COUNT
	lea	WIN+0x3000+0x100,%a0
	lea	WIN+0x4000+0x40,%a1
t3:	move.l	(%a0)+,(%a1)+
	move.l	%a0,R0
	move.l	%a1,R0+4
	want	n_t3, WIN+0x4000+0x40, 0x33333333
	want	n_t3a0, R0, WIN+0x3000+0x104
	want	n_t3a1, R0+4, WIN+0x4000+0x44
	want	n_t3c, V_COUNT, 1
	wantpc	n_t3pc, t3

	| ---- T4: MOVEM (An)+ reading across into an invalid page ----
	lea	WIN+0x6000-16,%a0
	move.l	#0x40000001,%d0
	moveq	#7,%d1
5:	move.l	%d0,(%a0)+
	addq.l	#1,%d0
	dbra	%d1,5b
	inval	6
	clr.l	V_COUNT
	lea	WIN+0x6000-16,%a0
t4:	movem.l	(%a0)+,%d0-%d5/%a2-%a3
	movem.l	%d0-%d5/%a2-%a3,R0
	move.l	%a0,R0+32
	want	n_t4d0, R0, 0x40000001
	want	n_t4d3, R0+12, 0x40000004
	want	n_t4d4, R0+16, 0x40000005
	want	n_t4a3, R0+28, 0x40000008
	want	n_t4a0, R0+32, WIN+0x6000+16
	want	n_t4c, V_COUNT, 1

	| ---- T5: MOVEM -(An) faulting after its first store moved An ----
	inval	8
	clr.l	V_COUNT
	lea	WIN+0x8000+16,%a1
	move.l	#0x50000001,%d0
	move.l	#0x50000002,%d1
	move.l	#0x50000003,%d2
	move.l	#0x50000004,%d3
	move.l	#0x50000005,%d4
	move.l	#0x50000006,%d5
	move.l	#0x50000007,%a2
	move.l	#0x50000008,%a3
t5:	movem.l	%d0-%d5/%a2-%a3,-(%a1)
	move.l	%a1,R0
	want	n_t5a1, R0, WIN+0x8000-16
	want	n_t5m0, WIN+0x8000-16, 0x50000001
	want	n_t5m4, WIN+0x8000, 0x50000005
	want	n_t5m7, WIN+0x8000+12, 0x50000008
	want	n_t5c, V_COUNT, 1
	wantpc	n_t5pc, t5

	| ---- T6: a read-modify-write, ADDQ #1,(An)+ ----
	move.l	#41,WIN+0x9000+8
	inval	9
	clr.l	V_COUNT
	lea	WIN+0x9000+8,%a0
t6:	addq.l	#1,(%a0)+
	move.l	%a0,R0
	want	n_t6, WIN+0x9000+8, 42
	want	n_t6a0, R0, WIN+0x9000+12
	want	n_t6c, V_COUNT, 1

	| ---- T7: ADDX -(An),-(Am), X clear then set: X must survive the restart ----
	move.l	#5,WIN+0xA000+0x10
	move.l	#7,WIN+0xB000+0x10
	inval	11
	clr.l	V_COUNT
	lea	WIN+0xA000+0x14,%a0
	lea	WIN+0xB000+0x14,%a1
	move.w	#0,%ccr
t7:	addx.l	-(%a0),-(%a1)
	move.l	%a0,R0
	move.l	%a1,R0+4
	want	n_t7, WIN+0xB000+0x10, 12
	want	n_t7a0, R0, WIN+0xA000+0x10
	want	n_t7a1, R0+4, WIN+0xB000+0x10
	want	n_t7c, V_COUNT, 1
	move.l	#7,WIN+0xB000+0x20
	inval	11
	clr.l	V_COUNT
	lea	WIN+0xA000+0x14,%a0
	lea	WIN+0xB000+0x24,%a1
	move.w	#0x10,%ccr
t7x:	addx.l	-(%a0),-(%a1)
	want	n_t7x, WIN+0xB000+0x20, 13
	want	n_t7xc, V_COUNT, 1

	| ---- T8: CMPM (An)+,(Am)+ ----
	move.l	#9,WIN+0xC000
	move.l	#9,WIN+0xD000
	inval	13
	clr.l	V_COUNT
	lea	WIN+0xC000,%a0
	lea	WIN+0xD000,%a1
t8:	cmpm.l	(%a0)+,(%a1)+
	seq	%d4
	move.l	%a0,R0
	move.l	%a1,R0+4
	and.l	#0xFF,%d4
	want	n_t8z, %d4, 0xFF
	want	n_t8a0, R0, WIN+0xC004
	want	n_t8a1, R0+4, WIN+0xD004
	want	n_t8c, V_COUNT, 1

	| ---- T9: a fault in user mode: the frame on the supervisor stack, the user stack kept ----
	move.l	#0x99999999,WIN+0x20000+0x30
	inval	0x20
	clr.l	V_COUNT
	lea	USP,%a0
	move.l	%a0,%usp
	lea	WIN+0x20000+0x30,%a2
	moveq	#0,%d4
	andi.w	#0xDFFF,%sr			| user mode
t9:	move.l	(%a2),%d4
	move.l	%sp,%d5
	trap	#0				| back to supervisor mode
	move.l	%d4,R0
	move.l	%d5,R0+4
	move.l	%sp,R0+8
	want	n_t9, R0, 0x99999999
	want	n_t9usp, R0+4, USP
	want	n_t9ssp, R0+8, SSP
	want	n_t9c, V_COUNT, 1
	moveq	#0,%d0
	move.w	V_SSW,%d0
	want	n_t9s, %d0, 0x0141		| DF, read, long, user data

	| ---- T10: condition codes set just before the faulting instruction (MOVEA leaves them alone) ----
	move.l	#0x0A0A0A0A,WIN+0x21000+0x10
	inval	0x21
	clr.l	V_COUNT
	lea	WIN+0x21000+0x10,%a0
	moveq	#0,%d3
	tst.l	%d3				| Z set
t10:	movea.l	(%a0),%a2
	seq	%d4
	and.l	#0xFF,%d4
	move.l	%a2,R0
	want	n_t10z, %d4, 0xFF
	want	n_t10, R0, 0x0A0A0A0A
	inval	0x21
	lea	WIN+0x21000+0x10,%a0
	moveq	#1,%d3
	tst.l	%d3				| Z clear
t10b:	movea.l	(%a0),%a2
	seq	%d4
	and.l	#0xFF,%d4
	want	n_t10nz, %d4, 0
	want	n_t10c, V_COUNT, 2

	| ---- T11: a hundred faults in a row, over 32 pages ----
	lea	WIN+0x40000,%a0			| the counters, one long at the start of each page
	moveq	#31,%d1
5:	clr.l	(%a0)
	lea	0x1000(%a0),%a0
	dbra	%d1,5b
	clr.l	V_COUNT
	moveq	#0,%d6
	moveq	#99,%d7
6:	move.l	%d6,%d0
	and.l	#31,%d0
	add.l	#0x40,%d0
	move.l	%d0,%d3				| page index
	bsr	invalidate
	move.l	%d3,%d0
	lsl.l	#8,%d0
	lsl.l	#4,%d0
	add.l	#WIN,%d0
	move.l	%d0,%a0
	move.l	(%a0),%d1			| faults
	addq.l	#1,%d1
	move.l	%d1,(%a0)
	addq.l	#1,%d6
	dbra	%d7,6b
	want	n_t11c, V_COUNT, 100
	want	n_t11, WIN+0x40000, 4		| page 0x40 was touched 4 times (pages 0x40-0x5F, 100 passes)

	| ---- the result ----
	lea	MSG,%a1
	lea	s_res(%pc),%a0
	bsr	puts
	move.l	PASS,%d0
	bsr	putdec
	lea	s_passed(%pc),%a0
	bsr	puts
	move.l	FAIL,%d0
	bsr	putdec
	lea	s_failed(%pc),%a0
	bsr	puts
	move.l	V_TOTAL,%d0
	bsr	putdec
	lea	s_taken(%pc),%a0
	bsr	puts
	lea	MSG,%a1
	bsr	do_log
	tst.l	FAIL
	beq.s	7f
	lea	FAILS,%a1
	bsr	do_log
7:	lea	MSG,%a1
	lea	s_bench(%pc),%a0
	bsr	puts
	lea	MSG,%a1
	bsr	do_log

	| ---- the benchmark, forever: the heartbeat's instruction count is the measurement ----
bench:	lea	BUF,%a4
	moveq	#0,%d5
8:	lea	(%a4),%a0
	lea	0x400(%a4),%a1
	moveq	#63,%d6
9:	move.l	(%a0)+,(%a1)+
	add.l	%d6,%d1
	eor.l	%d1,%d2
	dbra	%d6,9b
	bsr.s	bsub
	addq.l	#1,%d5
	bra.s	8b
bsub:	movem.l	%d0-%d3,-(%sp)
	move.l	%d5,%d0
	mulu.w	#13,%d0
	add.l	%d0,%d1
	lsr.l	#3,%d1
	movem.l	(%sp)+,%d0-%d3
	rts

| ---- the bus-error handler: what VM's does, in miniature ----
berr:	movem.l	%d0-%d1/%a0,-(%sp)
	addq.l	#1,V_TOTAL
	move.w	12+6(%sp),V_FMT
	move.w	12+10(%sp),V_SSW
	move.l	12+2(%sp),V_PC
	move.l	12+16(%sp),%d0
	move.l	%d0,V_ADDR
	move.l	%d0,%d1
	sub.l	#WIN,%d1
	cmp.l	#0x00100000,%d1
	bcc.s	1f				| not in the window: nothing this handler can map
	lsr.l	#8,%d1
	lsr.l	#4,%d1
	lsl.l	#2,%d1
	lea	TBLC,%a0
	and.l	#0xFFFFF000,%d0
	addq.l	#1,%d0				| DT 1: valid again
	move.l	%d0,(%a0,%d1.l)
	pflusha
	addq.l	#1,V_COUNT
	movem.l	(%sp)+,%d0-%d1/%a0
	rte
1:	lea	MSG,%a1
	lea	s_stray(%pc),%a0
	bsr	puts
	move.l	V_ADDR,%d0
	bsr	puthex
	lea	MSG,%a1
	bsr	do_log
2:	stop	#0x2700
	bra.s	2b

trap0:	ori.w	#0x2000,(%sp)			| return from TRAP #0 in supervisor mode
	rte

unexpected:
	lea	MSG,%a1
	lea	s_unexp(%pc),%a0
	bsr	puts
	moveq	#0,%d0
	move.w	6(%sp),%d0
	bsr	puthex
	lea	s_at(%pc),%a0
	bsr	puts
	move.l	2(%sp),%d0
	bsr	puthex
	lea	MSG,%a1
	bsr	do_log
1:	stop	#0x2700
	bra.s	1b

| d0 = test page index: mark it invalid in table C and flush
invalidate:
	lea	TBLC,%a0
	lsl.l	#2,%d0
	clr.l	(%a0,%d0.l)
	pflusha
	rts

| d0 got, d1 wanted, a1 the name: count, and list a failure as "name=value "
want_eq:
	cmp.l	%d1,%d0
	bne.s	1f
	addq.l	#1,PASS
	rts
1:	addq.l	#1,FAIL
	movem.l	%d0/%a0-%a1,-(%sp)
	move.l	FPTR,%a0
	cmp.l	#FAILS+100,%a0			| one log line's worth
	bcc.s	2f
	exg	%a0,%a1
	bsr	puts
	move.b	#'=',(%a1)+
	move.l	(%sp),%d0
	bsr	puthex
	move.b	#' ',(%a1)+
	clr.b	(%a1)
	move.l	%a1,FPTR
2:	movem.l	(%sp)+,%d0/%a0-%a1
	rts

| the string at a0 to a1 (a1 left on its terminating zero)
puts:	move.b	(%a0)+,(%a1)+
	bne.s	puts
	subq.l	#1,%a1
	rts
| d0 in decimal at a1
putdec:	movem.l	%d0-%d2,-(%sp)
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
	clr.b	(%a1)
	movem.l	(%sp)+,%d0-%d2
	rts
| d0 in hexadecimal (8 digits) at a1
puthex:	movem.l	%d0-%d2,-(%sp)
	moveq	#7,%d2
1:	rol.l	#4,%d0
	move.b	%d0,%d1
	and.b	#15,%d1
	add.b	#'0',%d1
	cmp.b	#'9',%d1
	bls.s	2f
	addq.b	#7,%d1
2:	move.b	%d1,(%a1)+
	dbra	%d2,1b
	clr.b	(%a1)
	movem.l	(%sp)+,%d0-%d2
	rts

| the string at a1 to the Pi's log (the card's INFO service, as mac/fputest.s does)
do_log:	movem.l	%d0-%d1/%a0,-(%sp)
	lea	MBOX,%a0
	moveq	#0,%d1
1:	tst.b	(%a1,%d1.w)
	beq.s	2f
	addq.w	#1,%d1
	bra.s	1b
2:	move.w	#2,8(%a0)
	move.l	%a1,28(%a0)
	move.l	%d1,32(%a0)
	move.l	(%a0),%d0
	addq.l	#1,%d0
	move.l	%d0,(%a0)
	move.l	#50000000,%d1
3:	cmp.l	4(%a0),%d0
	beq.s	4f
	subq.l	#1,%d1
	bne.s	3b
4:	movem.l	(%sp)+,%d0-%d1/%a0
	rts

s_res:	.asciz	"MMU test: "
s_passed: .asciz " passed, "
s_failed: .asciz " failed, "
s_taken: .asciz	" bus errors taken"
s_bench: .asciz	"MMU test: benchmark loop running (heartbeat JIT line)"
s_stray: .asciz	"MMU test: bus error outside the test window at "
s_unexp: .asciz	"MMU test: unexpected exception, format/vector "
s_at:	.asciz	" at PC "
n_t1:	.asciz	"t1"
n_t1c:	.asciz	"t1count"
n_t1pc:	.asciz	"t1pc"
n_t1f:	.asciz	"t1fmt"
n_t1s:	.asciz	"t1ssw"
n_t1a:	.asciz	"t1addr"
n_t2:	.asciz	"t2"
n_t2c:	.asciz	"t2count"
n_t2s:	.asciz	"t2ssw"
n_t3:	.asciz	"t3"
n_t3a0:	.asciz	"t3a0"
n_t3a1:	.asciz	"t3a1"
n_t3c:	.asciz	"t3count"
n_t3pc:	.asciz	"t3pc"
n_t4d0:	.asciz	"t4d0"
n_t4d3:	.asciz	"t4d3"
n_t4d4:	.asciz	"t4d4"
n_t4a3:	.asciz	"t4a3"
n_t4a0:	.asciz	"t4a0"
n_t4c:	.asciz	"t4count"
n_t5a1:	.asciz	"t5a1"
n_t5m0:	.asciz	"t5m0"
n_t5m4:	.asciz	"t5m4"
n_t5m7:	.asciz	"t5m7"
n_t5c:	.asciz	"t5count"
n_t5pc:	.asciz	"t5pc"
n_t6:	.asciz	"t6"
n_t6a0:	.asciz	"t6a0"
n_t6c:	.asciz	"t6count"
n_t7:	.asciz	"t7"
n_t7a0:	.asciz	"t7a0"
n_t7a1:	.asciz	"t7a1"
n_t7c:	.asciz	"t7count"
n_t7x:	.asciz	"t7x"
n_t7xc:	.asciz	"t7xcount"
n_t8z:	.asciz	"t8z"
n_t8a0:	.asciz	"t8a0"
n_t8a1:	.asciz	"t8a1"
n_t8c:	.asciz	"t8count"
n_t9:	.asciz	"t9"
n_t9usp: .asciz	"t9usp"
n_t9ssp: .asciz	"t9ssp"
n_t9c:	.asciz	"t9count"
n_t9s:	.asciz	"t9ssw"
n_t10z:	.asciz	"t10z"
n_t10:	.asciz	"t10"
n_t10nz: .asciz	"t10nz"
n_t10c:	.asciz	"t10count"
n_t11c:	.asciz	"t11count"
n_t11:	.asciz	"t11"
	.even
