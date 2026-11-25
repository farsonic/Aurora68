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
|     ROM=mac/build/mmutest.rom tools/run-solo.sh 60 mac.mmu=full
| (run-solo.sh still names the usual disk image and declaration ROM; this program reads neither) and read the
| result in the kernel log:
|     [MAC] MMU test: 127 passed, 0 failed, 122 bus errors taken
| (50 cases after phase 1, 86 after phase 2: T12-T18, the PMMU's other instructions; 127 after phase 3, 26 November
| 2026: T19-T24 and T26, two processes with their own CRPs and the same pages on other frames, data and code, a loop of
| 4000 context switches, SRE with the supervisor's own tables, MOVES across the spaces and into CPU space, PFLUSH by
| function code and mask, an exception's frame pushed through the supervisor's tables, code run from a page that
| faults on its fetch; docs/23-mmu.md section 12).
| mmutest-halt.rom ends with T25 instead of the benchmark: an exception whose frame cannot be pushed, after which the
| kernel must log the 68030's double bus fault and stop running the 68k.
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
	.equ	V_MMUSR, 0x00062030		| PTEST's result (PMOVE MMUSR)
	.equ	V_TT,	0x00062034		| a TT register's value for PMOVE
	.equ	V_CTAB,	0x00062040		| the level C table the handler validates a user fault in
	.equ	V_STAB,	0x00062044		| ... and a supervisor fault in (TBLC3 while SRE is on)
	.equ	V_SAVESP, 0x00062048		| the supervisor stack while T24 uses another
	.equ	V_FC7,	0x0006204C		| bus errors in CPU space (MOVES with FC 7)
	.equ	R0,	0x00062200		| results saved right after an instruction
	.equ	TBLA2,	0x00064000		| phase 3: process B's tables (A, B, C as the first tree's)
	.equ	TBLB2,	0x00064100
	.equ	TBLC2,	0x00064800
	.equ	TBLA3,	0x00065000		| the supervisor's tables (SRP, with TC.SRE)
	.equ	TBLB3,	0x00065100
	.equ	TBLC3,	0x00065800
	.equ	ROOT2,	0x00066000		| process B's CRP
	.equ	ROOT3,	0x00066010		| the SRP
	.equ	TCSRE,	0x00066020		| TC with SRE
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
	move.l	#TBLC,V_CTAB
	move.l	#TBLC,V_STAB
	clr.l	V_FC7

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
	want	n_t1f, %d0, 0xB008		| a read: the long frame (phase 2; phase 1 gave $A)
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
	moveq	#0,%d0
	move.w	V_FMT,%d0
	want	n_t2f, %d0, 0xA008		| a write: the short frame

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

	| ======== phase 2 (26 November 2026): the PMMU's other instructions and bits ========
	| (without mac.mmu=full no bus error has been taken by now, and these use PMOVE forms the old shim never had:
	|  they are skipped, so that the stock kernel reaches the benchmark below as before)
	tst.l	V_TOTAL
	beq	p2skip

	| ---- T12: PTEST and MMUSR (B L S W I M T N), and the address of the last descriptor ----
	move.l	#WIN+0x60000+1,TBLC+0x60*4	| valid, U and M clear
	pflusha
	lea	WIN+0x60000,%a0
	ptestr	#5,(%a0),#7,%a3
	pmove	%psr,V_MMUSR
	moveq	#0,%d0
	move.w	V_MMUSR,%d0
	want	n_t12v, %d0, 0x0003		| valid: three levels (A, B, C), nothing else
	want	n_t12va, %a3, TBLC+0x60*4	| A3: the level C descriptor
	clr.l	TBLC+0x61*4			| invalid
	pflusha
	lea	WIN+0x61000,%a0
	ptestr	#5,(%a0),#7,%a3
	pmove	%psr,V_MMUSR
	moveq	#0,%d0
	move.w	V_MMUSR,%d0
	want	n_t12i, %d0, 0x0403		| I, three levels
	move.l	#WIN+0x62000+0x15,TBLC+0x62*4	| WP and M
	pflusha
	lea	WIN+0x62000,%a0
	ptestw	#5,(%a0),#7,%a3
	pmove	%psr,V_MMUSR
	moveq	#0,%d0
	move.w	V_MMUSR,%d0
	want	n_t12w, %d0, 0x0A03		| W and M, three levels
	ptestr	#5,(%a0),#1,%a3
	pmove	%psr,V_MMUSR
	moveq	#0,%d0
	move.w	V_MMUSR,%d0
	want	n_t12l, %d0, 0x0001		| level 1 only: one level, the search not finished
	want	n_t12la, %a3, TBLA		| A3: the level A descriptor

	| ---- T13: U and M, as a 68030 sets them in the descriptors ----
	move.l	#WIN+0x63000+1,TBLC+0x63*4	| U and M clear
	pflusha
	tst.l	WIN+0x63000			| a read sets U only
	move.l	TBLC+0x63*4,%d0
	and.l	#0x18,%d0
	want	n_t13r, %d0, 0x08
	move.l	#0x13131313,WIN+0x63000+4	| the first write sets M
	move.l	TBLC+0x63*4,%d0
	and.l	#0x18,%d0
	want	n_t13w, %d0, 0x18
	want	n_t13d, WIN+0x63000+4, 0x13131313
	move.l	#WIN+0x63000+0x11,TBLC+0x63*4	| the guest clears U and flushes: the next read sets it again
	pflusha
	tst.l	WIN+0x63000
	move.l	TBLC+0x63*4,%d0
	and.l	#0x18,%d0
	want	n_t13u, %d0, 0x18
	move.l	#WIN+0x63000+0x09,TBLC+0x63*4	| ... and M: the next write sets it again
	pflusha
	tst.l	WIN+0x63000
	move.l	#0x31313131,WIN+0x63000+8
	move.l	TBLC+0x63*4,%d0
	and.l	#0x18,%d0
	want	n_t13m, %d0, 0x18
	move.l	#WIN+0x64000+1,TBLC+0x64*4	| PLOADR sets U, PLOADW U and M, without an access
	pflusha
	lea	WIN+0x64000,%a0
	ploadr	#5,(%a0)
	move.l	TBLC+0x64*4,%d0
	and.l	#0x18,%d0
	want	n_t13pr, %d0, 0x08
	ploadw	#5,(%a0)
	move.l	TBLC+0x64*4,%d0
	and.l	#0x18,%d0
	want	n_t13pw, %d0, 0x18

	| ---- T14: a write to a write-protected page is a bus error; a read is not ----
	move.l	#0x14141414,WIN+0x65000+8
	move.l	#WIN+0x65000+5,TBLC+0x65*4	| valid, WP
	pflusha
	clr.l	V_COUNT
	move.l	WIN+0x65000+8,%d2
	want	n_t14r, %d2, 0x14141414
	want	n_t14c0, V_COUNT, 0
	lea	WIN+0x65000+8,%a0
	move.l	#0x41414141,%d1
t14:	move.l	%d1,(%a0)			| the handler maps the page again without WP
	want	n_t14, WIN+0x65000+8, 0x41414141
	want	n_t14c, V_COUNT, 1
	wantpc	n_t14pc, t14
	moveq	#0,%d0
	move.w	V_SSW,%d0
	want	n_t14s, %d0, 0x0105		| DF, write, long, supervisor data

	| ---- T15: PFLUSH by address and by function code drop what they name ----
	move.l	#0x15151515,WIN+0x66000
	tst.l	WIN+0x66000			| in the ATC now
	clr.l	TBLC+0x66*4			| invalid in the tables
	lea	WIN+0x66000,%a0
	pflush	#0,#0,(%a0)			| any function code, this page
	clr.l	V_COUNT
	move.l	WIN+0x66000,%d2
	want	n_t15a, %d2, 0x15151515
	want	n_t15ac, V_COUNT, 1
	move.l	#0x51515151,WIN+0x67000
	tst.l	WIN+0x67000
	clr.l	TBLC+0x67*4
	pflush	#5,#7				| supervisor data
	clr.l	V_COUNT
	move.l	WIN+0x67000,%d2
	want	n_t15f, %d2, 0x51515151
	want	n_t15fc, V_COUNT, 1

	| ---- T16: TT0, a transparent window over the tables ----
	clr.l	TBLC+0x68*4			| page 0x68 invalid in the tables
	pflusha
	move.l	#0x00008107,V_TT		| logical $00xxxxxx one to one: enabled, reads and writes, every FC
	pmove	V_TT,%tt0
	clr.l	V_COUNT
	move.l	#0x16161616,WIN+0x68000
	move.l	WIN+0x68000,%d2
	want	n_t16, %d2, 0x16161616
	want	n_t16c, V_COUNT, 0
	lea	WIN+0x68000,%a0
	ptestr	#5,(%a0),#0
	pmove	%psr,V_MMUSR
	moveq	#0,%d0
	move.w	V_MMUSR,%d0
	want	n_t16t, %d0, 0x0040		| PTEST level 0: T
	clr.l	V_TT
	pmove	V_TT,%tt0			| off: the tables say invalid again
	move.l	WIN+0x68000,%d2
	want	n_t16o, %d2, 0x16161616
	want	n_t16oc, V_COUNT, 1

	| ---- T17: a PMOVE whose operand's page is invalid: the bus error comes from the instruction ----
	move.l	#0x00008107,WIN+0x69000+0x10	| the operand, then its page invalid
	clr.l	TBLC+0x69*4
	pflusha
	clr.l	V_COUNT
	lea	WIN+0x69000,%a0
t17:	pmove	0x10(%a0),%tt1			| the handler maps the page; the PMOVE runs again and loads TT1
	pmove	%tt1,V_TT
	want	n_t17c, V_COUNT, 1
	wantpc	n_t17pc, t17
	want	n_t17a, V_ADDR, WIN+0x69000+0x10
	clr.l	V_TT
	pmove	V_TT,%tt1

	| ---- T18: an FPU operand on an invalid page: the exact FPU's helper (the default) delivers the bus error ----
	move.l	#1818,WIN+0x6A000+0x20
	inval	0x6A
	clr.l	V_COUNT
	lea	WIN+0x6A000+0x20,%a0
	fmove.l	#0,%fp0
t18:	fmove.l	(%a0),%fp0			| the handler maps the page; the FMOVE runs again
	fmove.l	%fp0,%d2
	want	n_t18, %d2, 1818
	want	n_t18c, V_COUNT, 1
	wantpc	n_t18pc, t18
	want	n_t18a, V_ADDR, WIN+0x6A000+0x20

	| ======== phase 3 (26 November 2026): address spaces, SRE, MOVES, PFLUSH by function code ========

	| ---- T19: two processes, two CRPs, the same logical pages on different frames, data and code ----
	lea	TBLA2,%a2
	lea	TBLB2,%a3
	lea	TBLC2,%a4
	bsr	mktree
	move.l	#0xAAAA0019,WIN+0xD0000		| the frames, written one to one first
	move.l	#0xBBBB0019,WIN+0xD2000
	move.l	#0x203CA0A0,WIN+0xD1000		| "move.l #$A0A0A0A0,d0; rts"
	move.l	#0xA0A04E75,WIN+0xD1004
	move.l	#0x203CB0B0,WIN+0xD3000		| "move.l #$B0B0B0B0,d0; rts"
	move.l	#0xB0B04E75,WIN+0xD3004
	move.l	#WIN+0xD0000+1,TBLC+0xC0*4	| process A: page C0 -> D0, C1 -> D1
	move.l	#WIN+0xD1000+1,TBLC+0xC1*4
	move.l	#WIN+0xD2000+1,TBLC2+0xC0*4	| process B: page C0 -> D2, C1 -> D3
	move.l	#WIN+0xD3000+1,TBLC2+0xC1*4
	lea	ROOT2,%a0
	move.l	#0x7FFF0002,(%a0)
	move.l	#TBLA2,4(%a0)
	pflusha
	clr.l	V_COUNT
	lea	ROOT2,%a0
	pmove	(%a0),%crp			| process B
	move.l	WIN+0xC0000,%d2
	jsr	WIN+0xC1000
	move.l	%d0,%d3
	lea	ROOT,%a0
	pmove	(%a0),%crp			| process A
	move.l	WIN+0xC0000,%d4
	jsr	WIN+0xC1000
	move.l	%d0,%d5
	lea	ROOT2,%a0
	pmove	(%a0),%crp			| B again (its space is kept)
	move.l	WIN+0xC0000,%d6
	jsr	WIN+0xC1000
	move.l	%d0,%d7
	move.l	#0x19191919,WIN+0xC0000		| a write in B's page C0 (frame D2)
	lea	ROOT,%a0
	pmove	(%a0),%crp
	movem.l	%d2-%d7,R0
	want	n_t19b, R0, 0xBBBB0019
	want	n_t19bc, R0+4, 0xB0B0B0B0
	want	n_t19a, R0+8, 0xAAAA0019
	want	n_t19ac, R0+12, 0xA0A0A0A0
	want	n_t19b2, R0+16, 0xBBBB0019
	want	n_t19bc2, R0+20, 0xB0B0B0B0
	want	n_t19w, WIN+0xD2000, 0x19191919	| B's frame changed ...
	want	n_t19wa, WIN+0xC0000, 0xAAAA0019	| ... A's page did not
	want	n_t19c, V_COUNT, 0

	| ---- T20: a context-switch loop, 2000 round trips (the kernel's switch counter and times show the cost) ----
	lea	s_t20(%pc),%a0
	lea	MSG,%a1
	bsr	puts
	lea	MSG,%a1
	bsr	do_log
	lea	ROOT,%a2
	lea	ROOT2,%a3
	moveq	#0,%d6
	move.w	#1999,%d7
1:	pmove	(%a3),%crp
	add.l	WIN+0xC0000,%d6
	jsr	WIN+0xC1000
	add.l	%d0,%d6
	pmove	(%a2),%crp
	add.l	WIN+0xC0000,%d6
	jsr	WIN+0xC1000
	add.l	%d0,%d6
	dbra	%d7,1b
	lea	s_t20e(%pc),%a0
	lea	MSG,%a1
	bsr	puts
	lea	MSG,%a1
	bsr	do_log
	want	n_t20, %d6, 0xAF8017A0		| 2000 x ($19191919 + $B0B0B0B0 + $AAAA0019 + $A0A0A0A0), modulo 2^32
	want	n_t20c, V_COUNT, 0

	| ---- T21: SRE: the supervisor's own root, the same page on another frame in each mode ----
	move.l	#0xC2C2C2C2,WIN+0xC2000		| frames written one to one first
	move.l	#0xC3C3C3C3,WIN+0xC3000
	move.l	#0x55550021,WIN+0xD4000
	move.l	#0x5555AA21,WIN+0xD5000
	clr.l	WIN+0xD4004
	clr.l	WIN+0xD5004
	move.l	#0x33330023,WIN+0xD6000
	move.l	#0x3333AA23,WIN+0xD7000
	lea	TBLA3,%a2
	lea	TBLB3,%a3
	lea	TBLC3,%a4
	bsr	mktree
	move.l	#WIN+0xD4000+1,TBLC+0xC2*4	| user (CRP): page C2 -> D4
	move.l	#WIN+0xD5000+1,TBLC3+0xC2*4	| supervisor (SRP): page C2 -> D5
	clr.l	TBLC+0xC4*4			| page C4: the supervisor's only (T24)
	lea	ROOT3,%a0
	move.l	#0x7FFF0002,(%a0)
	move.l	#TBLA3,4(%a0)
	pmove	(%a0),%srp
	move.l	#TBLC3,V_STAB
	move.l	#0x82C04880,TCSRE		| the same TC with SRE
	lea	TCSRE,%a0
	pmove	(%a0),%tc
	clr.l	V_COUNT
	move.l	WIN+0xC2000,%d2			| supervisor
	moveq	#0,%d4
	andi.w	#0xDFFF,%sr
	move.l	WIN+0xC2000,%d4			| user
	trap	#0
	move.l	%d2,R0
	move.l	%d4,R0+4
	want	n_t21s, R0, 0x5555AA21
	want	n_t21u, R0+4, 0x55550021
	want	n_t21c, V_COUNT, 0

	| ---- T22: MOVES: the user's space from supervisor mode, SFC/DFC 5, CPU space, a refused user page ----
	moveq	#1,%d0
	movec	%d0,%sfc
	movec	%d0,%dfc
	lea	WIN+0xC2000,%a0
	moves.l	(%a0),%d2			| the user's page C2 (frame D4)
	move.l	#0x22222222,%d1
	moves.l	%d1,4(%a0)			| into it
	moveq	#5,%d0
	movec	%d0,%sfc
	moves.l	(%a0),%d3			| the supervisor's (frame D5)
	movem.l	%d2-%d3,R0
	want	n_t22r, R0, 0x55550021
	want	n_t22s, R0+4, 0x5555AA21
	want	n_t22w, WIN+0xD4004, 0x22222222	| the user's frame got it ...
	want	n_t22ws, WIN+0xC2004, 0		| ... the supervisor's page (D5) did not
	clr.l	V_COUNT
	moveq	#7,%d0
	movec	%d0,%sfc
	lea	WIN+0xC2000,%a0
t22c:	moves.l	(%a0),%d2			| CPU space: a bus error; the handler sets SFC to 1 and the MOVES runs again
	move.l	%d2,R0
	want	n_t22c, R0, 0x55550021
	want	n_t22cc, V_COUNT, 1
	want	n_t22c7, V_FC7, 1
	moveq	#0,%d0
	move.w	V_SSW,%d0
	want	n_t22cs, %d0, 0x0147		| DF, read, long, FC 7
	wantpc	n_t22cpc, t22c
	clr.l	TBLC+0xC2*4			| the user's page C2 invalid
	pflusha
	clr.l	V_COUNT
	moveq	#1,%d0
	movec	%d0,%sfc
	lea	WIN+0xC2000,%a0
t22i:	moves.l	(%a0),%d2			| a bus error with FC 1; the handler maps C2 one to one in the user's tables
	move.l	%d2,R0
	want	n_t22i, R0, 0xC2C2C2C2
	want	n_t22ic, V_COUNT, 1
	moveq	#0,%d0
	move.w	V_SSW,%d0
	want	n_t22is, %d0, 0x0141		| DF, read, long, user data
	want	n_t22ia, V_ADDR, WIN+0xC2000

	| ---- T23: PFLUSH by function code and mask flushes one mode's space and leaves the other's ----
	move.l	#WIN+0xD6000+1,TBLC+0xC3*4	| page C3: user -> D6, supervisor -> D7
	move.l	#WIN+0xD7000+1,TBLC3+0xC3*4
	pflusha
	move.l	WIN+0xC3000,%d2			| both in their ATC (the spaces' mappings)
	andi.w	#0xDFFF,%sr
	move.l	WIN+0xC3000,%d4
	trap	#0
	clr.l	TBLC+0xC3*4			| both invalid in the tables, nothing flushed yet
	clr.l	TBLC3+0xC3*4
	pflush	#1,#4				| the user's entries only (FC2 clear)
	clr.l	V_COUNT
	move.l	WIN+0xC3000,%d3			| supervisor: the old entry still answers
	andi.w	#0xDFFF,%sr
t23u:	move.l	WIN+0xC3000,%d5			| user: flushed and invalid, a bus error (mapped one to one by the handler)
	trap	#0
	movem.l	%d2-%d5,R0
	want	n_t23s, R0, 0x3333AA23
	want	n_t23u, R0+8, 0x33330023
	want	n_t23s2, R0+4, 0x3333AA23
	want	n_t23u2, R0+12, 0xC3C3C3C3
	want	n_t23uc, V_COUNT, 1
	moveq	#0,%d0
	move.w	V_SSW,%d0
	want	n_t23us, %d0, 0x0141
	move.l	#WIN+0xD6000+1,TBLC+0xC3*4	| again, the other way round
	move.l	#WIN+0xD7000+1,TBLC3+0xC3*4
	pflusha
	move.l	WIN+0xC3000,%d2
	andi.w	#0xDFFF,%sr
	move.l	WIN+0xC3000,%d4
	trap	#0
	clr.l	TBLC+0xC3*4
	clr.l	TBLC3+0xC3*4
	pflush	#4,#4				| the supervisor's entries only
	clr.l	V_COUNT
	andi.w	#0xDFFF,%sr
	move.l	WIN+0xC3000,%d5			| user: the old entry still answers
	trap	#0
t23s:	move.l	WIN+0xC3000,%d3			| supervisor: a bus error, mapped one to one in the supervisor's tables
	movem.l	%d2-%d5,R0
	want	n_t23bs, R0+4, 0xC3C3C3C3
	want	n_t23bu, R0+12, 0x33330023
	want	n_t23bc, V_COUNT, 1
	moveq	#0,%d0
	move.w	V_SSW,%d0
	want	n_t23bss, %d0, 0x0145
	wantpc	n_t23bpc, t23s

	| ---- T24: an exception from user mode pushes its frame through the supervisor's tables ----
	| (page C4 is valid in the SRP's tables only: a push that searched the CRP's would fault, a double bus fault)
	move.l	%sp,V_SAVESP
	lea	WIN+0xC4800,%sp			| the supervisor stack on page C4
	pflusha
	clr.l	V_COUNT
	andi.w	#0xDFFF,%sr
	trap	#0				| the frame: $C47F8-$C47FF
	move.l	%sp,%d2
	move.l	V_SAVESP,%sp
	move.l	%d2,R0
	want	n_t24sp, R0, WIN+0xC4800
	moveq	#0,%d0
	move.w	WIN+0xC47F8+6,%d0
	want	n_t24f, %d0, 0x0080		| format 0, vector 32 (TRAP #0)
	want	n_t24c, V_COUNT, 0

	| back to one root for both modes
	lea	ROOT+8,%a0
	pmove	(%a0),%tc
	move.l	#TBLC,V_STAB
	moveq	#1,%d0
	movec	%d0,%sfc
	movec	%d0,%dfc

	| ---- T26: code on a page the tables refuse: the fetch faults, the handler maps the page, the code runs ----
	move.l	#0x203C2626,WIN+0xC6000		| "move.l #$26262626,d0; rts", written as data, never run before
	move.l	#0x26264E75,WIN+0xC6004
	inval	0xC6
	clr.l	V_COUNT
	moveq	#0,%d0
	jsr	WIN+0xC6000
	move.l	%d0,R0
	want	n_t26, R0, 0x26262626
	want	n_t26c, V_COUNT, 1
	moveq	#0,%d0
	move.w	V_FMT,%d0
	want	n_t26f, %d0, 0xB008		| the long frame
	moveq	#0,%d0
	move.w	V_SSW,%d0
	want	n_t26s, %d0, 0x5006		| FB, RB, supervisor program
	want	n_t26a, V_ADDR, WIN+0xC6000	| the stage B address
	want	n_t26pc, V_PC, WIN+0xC6000

p2skip:

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
7:
	.ifdef	HALTTEST
	| ---- T25 (make mmutest-halt): an exception whose frame cannot be pushed: a 68030 halts ----
	lea	MSG,%a1
	lea	s_halt(%pc),%a0
	bsr	puts
	lea	MSG,%a1
	bsr	do_log
	clr.l	TBLC+0xC5*4			| page C5 invalid
	pflusha
	lea	WIN+0xC5800,%sp
	andi.w	#0xDFFF,%sr
	trap	#0				| the push faults: double bus fault
	lea	MSG,%a1
	lea	s_nohalt(%pc),%a0
	bsr	puts
	lea	MSG,%a1
	bsr	do_log
	.endif
	lea	MSG,%a1
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
	btst	#0,12+10(%sp)			| DF clear: an instruction fetch, whose address is the stage B address (T26)
	bne.s	5f
	move.l	12+36(%sp),%d0
5:	move.l	%d0,V_ADDR
	moveq	#7,%d1
	and.w	12+10(%sp),%d1
	cmp.w	#7,%d1
	bne.s	3f
	addq.l	#1,V_FC7			| CPU space (T22): SFC back to user data, and the MOVES again
	moveq	#1,%d1
	movec	%d1,%sfc
	addq.l	#1,V_COUNT
	movem.l	(%sp)+,%d0-%d1/%a0
	rte
3:	move.l	%d0,%d1
	sub.l	#WIN,%d1
	cmp.l	#0x00100000,%d1
	bcc.s	1f				| not in the window: nothing this handler can map
	lsr.l	#8,%d1
	lsr.l	#4,%d1
	lsl.l	#2,%d1
	move.l	V_CTAB,%a0			| the user's tables or the supervisor's (SRE, T21-T24) by the SSW's FC
	btst	#2,12+11(%sp)
	beq.s	4f
	move.l	V_STAB,%a0
4:	and.l	#0xFFFFF000,%d0
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

| a2/a3/a4 = level A/B/C tables: a tree like the first (everything one to one, table C for the window)
mktree:	movem.l	%d0-%d1/%a0-%a1,-(%sp)
	lea	TBLA,%a0
	move.l	%a2,%a1
	moveq	#15,%d1
1:	move.l	(%a0)+,(%a1)+
	dbra	%d1,1b
	move.l	%a3,%d0
	addq.l	#2,%d0
	move.l	%d0,(%a2)
	lea	TBLB,%a0
	move.l	%a3,%a1
	move.w	#255,%d1
2:	move.l	(%a0)+,(%a1)+
	dbra	%d1,2b
	move.l	%a4,%d0
	addq.l	#2,%d0
	move.l	%d0,4(%a3)
	move.l	%a4,%a1
	move.l	#WIN+1,%d0
	move.w	#255,%d1
3:	move.l	%d0,(%a1)+
	add.l	#0x1000,%d0
	dbra	%d1,3b
	movem.l	(%sp)+,%d0-%d1/%a0-%a1
	rts

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
	moveq	#0,%d1				| wait for the answer (2^32 turns): since the soft TLB
3:	cmp.l	4(%a0),%d0			| the tests end before the kernel's mailbox service starts
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
n_t19b:	.asciz	"t19b"
n_t19bc: .asciz	"t19bcode"
n_t19a:	.asciz	"t19a"
n_t19ac: .asciz	"t19acode"
n_t19b2: .asciz	"t19b2"
n_t19bc2: .asciz "t19bcode2"
n_t19w:	.asciz	"t19write"
n_t19wa: .asciz	"t19writea"
n_t19c:	.asciz	"t19count"
n_t20:	.asciz	"t20sum"
n_t20c:	.asciz	"t20count"
n_t21s:	.asciz	"t21super"
n_t21u:	.asciz	"t21user"
n_t21c:	.asciz	"t21count"
n_t22r:	.asciz	"t22read"
n_t22s:	.asciz	"t22sfc5"
n_t22w:	.asciz	"t22write"
n_t22ws: .asciz	"t22writesuper"
n_t22c:	.asciz	"t22fc7"
n_t22cc: .asciz	"t22fc7count"
n_t22c7: .asciz	"t22fc7seen"
n_t22cs: .asciz	"t22fc7ssw"
n_t22cpc: .asciz "t22fc7pc"
n_t22i:	.asciz	"t22invalid"
n_t22ic: .asciz	"t22invcount"
n_t22is: .asciz	"t22invssw"
n_t22ia: .asciz	"t22invaddr"
n_t23s:	.asciz	"t23super"
n_t23u:	.asciz	"t23user"
n_t23s2: .asciz	"t23superkept"
n_t23u2: .asciz	"t23userflushed"
n_t23uc: .asciz	"t23count"
n_t23us: .asciz	"t23ssw"
n_t23bs: .asciz	"t23superflushed"
n_t23bu: .asciz	"t23userkept"
n_t23bc: .asciz	"t23bcount"
n_t23bss: .asciz "t23bssw"
n_t23bpc: .asciz "t23bpc"
n_t24sp: .asciz	"t24sp"
n_t24f:	.asciz	"t24frame"
n_t24c:	.asciz	"t24count"
n_t26:	.asciz	"t26"
n_t26c:	.asciz	"t26count"
n_t26f:	.asciz	"t26fmt"
n_t26s:	.asciz	"t26ssw"
n_t26a:	.asciz	"t26addr"
n_t26pc: .asciz	"t26pc"
s_t20:	.asciz	"MMU test: context-switch loop (4000 PMOVEs to CRP) starts"
s_t20e:	.asciz	"MMU test: context-switch loop done"
s_halt:	.asciz	"MMU test: T25, an exception whose frame cannot be pushed: the 68030 should halt now"
s_nohalt: .asciz "MMU test: T25 FAILED, the 68030 did not halt"
n_t12v:	.asciz	"t12valid"
n_t12va: .asciz	"t12an"
n_t12i:	.asciz	"t12invalid"
n_t12w:	.asciz	"t12wp"
n_t12l:	.asciz	"t12level1"
n_t12la: .asciz	"t12an1"
n_t13r:	.asciz	"t13read"
n_t13w:	.asciz	"t13write"
n_t13d:	.asciz	"t13data"
n_t13u:	.asciz	"t13u"
n_t13m:	.asciz	"t13m"
n_t13pr: .asciz	"t13ploadr"
n_t13pw: .asciz	"t13ploadw"
n_t14r:	.asciz	"t14read"
n_t14c0: .asciz	"t14readcount"
n_t14:	.asciz	"t14"
n_t14c:	.asciz	"t14count"
n_t14pc: .asciz	"t14pc"
n_t14s:	.asciz	"t14ssw"
n_t15a:	.asciz	"t15addr"
n_t15ac: .asciz	"t15addrcount"
n_t15f:	.asciz	"t15fc"
n_t15fc: .asciz	"t15fccount"
n_t16:	.asciz	"t16"
n_t16c:	.asciz	"t16count"
n_t16t:	.asciz	"t16mmusr"
n_t16o:	.asciz	"t16off"
n_t16oc: .asciz	"t16offcount"
n_t18:	.asciz	"t18"
n_t18c:	.asciz	"t18count"
n_t18pc: .asciz	"t18pc"
n_t18a:	.asciz	"t18addr"
n_t17c:	.asciz	"t17count"
n_t17pc: .asciz	"t17pc"
n_t17a:	.asciz	"t17addr"
n_t1:	.asciz	"t1"
n_t1c:	.asciz	"t1count"
n_t1pc:	.asciz	"t1pc"
n_t1f:	.asciz	"t1fmt"
n_t1s:	.asciz	"t1ssw"
n_t1a:	.asciz	"t1addr"
n_t2:	.asciz	"t2"
n_t2c:	.asciz	"t2count"
n_t2f:	.asciz	"t2fmt"
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
