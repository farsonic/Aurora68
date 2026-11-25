| SPDX-License-Identifier: MPL-2.0
| vmtest.s - "Aurora68 VM Test": System 7's Virtual Memory, exercised from an application. MPL-2.0.
| GNU as MIT syntax, 68020, packaged by tools/mac/mkapp.py with a 26 MB partition (make vmtest; not in the release).
|
| 26 November 2026 (docs/23-mmu.md, phase 2). With mac.mmu=full the kernel gives the Mac a complete 68030 PMMU and
| System 7.1's Virtual Memory runs on it. This program shows that pages really leave for the VM storage file and come
| back intact: on a Mac with 8 MB of RAM and 32 MB of virtual memory it allocates 24 blocks of 1 MB, writes every
| longword with a value made from its own address and the pass (so a page that came back from the wrong place, or
| not at all, is seen), then reads all of it back twice, forwards and then backwards (the backwards pass starts with
| the blocks the forward pass pushed out first). Each pass is timed in ticks, the mismatches are counted and the
| written and read values are summed; the sums must agree. The results go to the window and to the kernel's log
| through the card's INFO service ("[MAC] VM test: ...", as the other programs log). A click or a key quits.
|
| Registers across the passes: a2 the table of block pointers, d3 the number of blocks, d5 the pass's seed, d6 the
| running sum, d7 the mismatches. Traps clobber d0-d2/a0-a1.

	.text
	.globl	app_start

	.equ	_InitGraf,	0xA86E
	.equ	_InitFonts,	0xA8FE
	.equ	_InitWindows,	0xA912
	.equ	_InitMenus,	0xA930
	.equ	_TEInit,	0xA9CC
	.equ	_InitDialogs,	0xA97B
	.equ	_InitCursor,	0xA850
	.equ	_NewWindow,	0xA913
	.equ	_SetPort,	0xA873
	.equ	_MoveTo,	0xA893
	.equ	_DrawString,	0xA884
	.equ	_TextFont,	0xA887
	.equ	_TextSize,	0xA88A
	.equ	_GetNextEvent,	0xA970
	.equ	_SystemTask,	0xA9B4
	.equ	_ExitToShell,	0xA9F4
	.equ	_NewPtr,	0xA11E
	.equ	_FreeMem,	0xA01C
	.equ	_Gestalt,	0xA1AD
	.equ	_MaxApplZone,	0xA063
	.equ	_MemoryDispatch, 0xA05C
	.equ	Ticks,		0x016A
	.equ	MMU32Bit,	0x0CB2
	.equ	BLOCKS,		24
	.equ	BLOCKSZ,	0x100000
	.equ	LINEH,		12

app_start:
	lea	-4(%a5),%a0
	move.l	%a0,-(%sp)
	.short	_InitGraf
	.short	_InitFonts
	.short	_InitWindows
	.short	_InitMenus
	.short	_TEInit
	clr.l	-(%sp)
	.short	_InitDialogs
	.short	_InitCursor
	clr.l	-(%sp)
	clr.l	-(%sp)
	pea	wrect(%pc)
	pea	wtitle(%pc)
	st	-(%sp)
	move.w	#0,-(%sp)
	move.l	#-1,-(%sp)
	sf	-(%sp)
	clr.l	-(%sp)
	.short	_NewWindow
	move.l	(%sp)+,%d0
	move.l	%d0,-(%sp)
	.short	_SetPort
	move.w	#4,-(%sp)
	.short	_TextFont
	move.w	#9,-(%sp)
	.short	_TextSize
	lea	vars(%pc),%a6
	move.w	#14,V_Y(%a6)
	.short	_MaxApplZone			| the whole partition, so that FreeMem says what is there
	lea	app_start(%pc),%a0		| HoldMemory on this segment: the kernel reads the log lines from it
	lea	seg_end(%pc),%a1		| (the card's INFO service, as a DMA would), so it must stay resident
	sub.l	%a0,%a1
	moveq	#0,%d0				| selector 0: HoldMemory (Inside Macintosh VI, "Virtual Memory Manager")
	.short	_MemoryDispatch
	move.w	%d0,V_HOLD(%a6)

	| --- what the system says: Gestalt 'vm  ', 'ram ', 'lram', and the free memory ---
	lea	s_hdr(%pc),%a0
	bsr	line_start
	move.l	#0x766d2020,%d0			| 'vm  '
	bsr	gestalt
	bsr	line_dec
	lea	s_ram(%pc),%a0
	bsr	line_str
	move.l	#0x72616d20,%d0			| 'ram '
	bsr	gestalt
	moveq	#10,%d1
	lsr.l	%d1,%d0
	bsr	line_dec
	lea	s_lram(%pc),%a0
	bsr	line_str
	move.l	#0x6c72616d,%d0			| 'lram'
	bsr	gestalt
	moveq	#10,%d1
	lsr.l	%d1,%d0
	bsr	line_dec
	lea	s_free(%pc),%a0
	bsr	line_str
	.short	_FreeMem
	moveq	#10,%d1
	lsr.l	%d1,%d0
	bsr	line_dec
	lea	s_k(%pc),%a0
	bsr	line_str
	lea	s_hold(%pc),%a0
	bsr	line_str
	move.w	V_HOLD(%a6),%d0
	ext.l	%d0
	bsr	line_dec
	bsr	line_end

	| --- the blocks ---
	lea	blocks(%pc),%a2
	moveq	#0,%d3
1:	move.l	#BLOCKSZ,%d0
	.short	_NewPtr
	tst.w	%d0
	bne.s	2f
	move.l	%a0,(%a2,%d3.w*4)
	addq.w	#1,%d3
	cmp.w	#BLOCKS,%d3
	blt.s	1b
2:	lea	s_alloc(%pc),%a0
	bsr	line_start
	move.l	%d3,%d0
	bsr	line_dec
	lea	s_blocks(%pc),%a0
	bsr	line_str
	move.l	(%a2),%d0
	bsr	line_hex
	lea	s_dash(%pc),%a0
	bsr	line_str
	move.l	%d3,%d0
	subq.l	#1,%d0
	bmi.s	3f
	move.l	(%a2,%d0.w*4),%d0
	add.l	#BLOCKSZ-1,%d0
	bsr	line_hex
3:	bsr	line_end
	tst.w	%d3
	beq	done

	| --- pass 1: write ---
	move.l	#0x5A5AA5A5,%d5
	moveq	#0,%d6
	moveq	#0,%d7
	move.l	Ticks,V_T0(%a6)
	moveq	#0,%d4
10:	move.l	(%a2,%d4.w*4),%a0
	move.l	#BLOCKSZ/4-1,%d1
11:	move.l	%a0,%d0
	eor.l	%d5,%d0
	add.l	%d0,%d6
	move.l	%d0,(%a0)+
	subq.l	#1,%d1
	bpl.s	11b
	addq.w	#1,%d4
	cmp.w	%d3,%d4
	blt.s	10b
	move.l	%d6,V_SUMW(%a6)
	lea	s_write(%pc),%a0
	bsr	report

	| --- pass 2: read forwards ---
	moveq	#0,%d6
	moveq	#0,%d7
	move.l	Ticks,V_T0(%a6)
	moveq	#0,%d4
20:	bsr	check_block
	addq.w	#1,%d4
	cmp.w	%d3,%d4
	blt.s	20b
	lea	s_fwd(%pc),%a0
	bsr	report

	| --- pass 3: read backwards ---
	moveq	#0,%d6
	moveq	#0,%d7
	move.l	Ticks,V_T0(%a6)
	move.w	%d3,%d4
30:	subq.w	#1,%d4
	bmi.s	31f
	bsr	check_block
	bra.s	30b
31:	lea	s_back(%pc),%a0
	bsr	report

done:	lea	s_quit(%pc),%a0
	bsr	line_start
	bsr	line_end
loop:
	.short	_SystemTask
	clr.w	-(%sp)
	move.w	#0xFFFF,-(%sp)
	pea	event(%pc)
	.short	_GetNextEvent
	tst.w	(%sp)+
	beq.s	loop
	move.w	event(%pc),%d0
	cmp.w	#1,%d0
	beq.s	1f
	cmp.w	#3,%d0
	bne.s	loop
1:	.short	_ExitToShell

| block d4: every longword against its expected value; d6 += value, d7 += mismatches
check_block:
	move.l	(%a2,%d4.w*4),%a0
	move.l	#BLOCKSZ/4-1,%d1
1:	move.l	%a0,%d0
	eor.l	%d5,%d0
	move.l	(%a0)+,%d2
	add.l	%d2,%d6
	cmp.l	%d0,%d2
	beq.s	2f
	addq.l	#1,%d7
2:	subq.l	#1,%d1
	bpl.s	1b
	rts

| a0 = the pass's name: "<name>: N ticks, M mismatches, sum S (written W)"
report:
	move.l	Ticks,%d0
	sub.l	V_T0(%a6),%d0
	move.l	%d0,V_T1(%a6)
	bsr	line_start
	move.l	V_T1(%a6),%d0
	bsr	line_dec
	lea	s_ticks(%pc),%a0
	bsr	line_str
	move.l	%d7,%d0
	bsr	line_dec
	lea	s_mis(%pc),%a0
	bsr	line_str
	move.l	%d6,%d0
	bsr	line_hex
	lea	s_wsum(%pc),%a0
	bsr	line_str
	move.l	V_SUMW(%a6),%d0
	bsr	line_hex
	lea	s_close(%pc),%a0
	bsr	line_str
	bra	line_end

| gestalt: d0 = selector -> d0 = response (0 if the call fails)
gestalt:
	.short	_Gestalt
	tst.w	%d0
	bne.s	1f
	move.l	%a0,%d0
	rts
1:	moveq	#0,%d0
	rts

| ---- one line of text, built in the C string 'line' (logged) and drawn as a Pascal string ----
line_start:				| a0 = C string: the line starts with "VM test: " and it
	lea	line+1(%pc),%a1
	move.l	%a1,V_P(%a6)
	move.l	%a0,-(%sp)
	lea	s_pre(%pc),%a0
	bsr.s	line_str
	move.l	(%sp)+,%a0
line_str:				| a0 = C string, appended
	move.l	V_P(%a6),%a1
1:	move.b	(%a0)+,(%a1)+
	bne.s	1b
	subq.l	#1,%a1
	move.l	%a1,V_P(%a6)
	rts
line_dec:				| d0 unsigned -> decimal, appended
	movem.l	%d1-%d2,-(%sp)
	lea	tmp+12(%pc),%a0
	clr.b	(%a0)
1:	move.l	%d0,%d1
	divull	#10,%d2:%d1		| d1 = d0 / 10, d2 = d0 % 10 (the 32-bit form, see piblue.s)
	add.b	#'0',%d2
	move.b	%d2,-(%a0)
	move.l	%d1,%d0
	bne.s	1b
	bsr.s	line_str
	movem.l	(%sp)+,%d1-%d2
	rts
line_hex:				| d0 -> eight hex digits, appended
	movem.l	%d1-%d2,-(%sp)
	lea	tmp(%pc),%a0
	moveq	#7,%d2
1:	rol.l	#4,%d0
	move.b	%d0,%d1
	and.b	#15,%d1
	add.b	#'0',%d1
	cmp.b	#'9',%d1
	bls.s	2f
	addq.b	#7,%d1
2:	move.b	%d1,(%a0)+
	dbra	%d2,1b
	clr.b	(%a0)
	lea	tmp(%pc),%a0
	bsr.s	line_str
	movem.l	(%sp)+,%d1-%d2
	rts
line_end:				| log it, draw it, next line
	movem.l	%d0-%d2/%a0-%a1,-(%sp)
	lea	line+1(%pc),%a1
	bsr	do_log
	lea	line(%pc),%a0
	move.l	V_P(%a6),%d0
	sub.l	%a0,%d0
	subq.l	#1,%d0
	move.b	%d0,(%a0)
	move.w	#6,-(%sp)
	move.w	V_Y(%a6),-(%sp)
	.short	_MoveTo
	pea	line(%pc)
	.short	_DrawString
	add.w	#LINEH,V_Y(%a6)
	movem.l	(%sp)+,%d0-%d2/%a0-%a1
	rts

| the C string at a1 to the Pi's log (the card's INFO service, docs/07)
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
	move.l	#2000000,%d1
4:	cmp.l	4(%a0),%d0
	beq.s	5f
	subq.l	#1,%d1
	bne.s	4b
5:	rts

wtitle:	.byte	23
	.ascii	"Aurora68 VM Test 1.0  "
	.even
wrect:	.short	44, 10, 300, 630	| top, left, bottom, right
s_pre:	.asciz	"VM test: "
s_hdr:	.asciz	"Gestalt vm "
s_ram:	.asciz	", RAM "
s_lram:	.asciz	" K, logical RAM "
s_free:	.asciz	" K, FreeMem "
s_k:	.asciz	" K"
s_hold:	.asciz	", HoldMemory result "
s_alloc: .asciz	"allocated "
s_blocks: .asciz " blocks of 1 MB, "
s_dash:	.asciz	"-"
s_write: .asciz	"pass 1, write: "
s_fwd:	.asciz	"pass 2, read forwards: "
s_back:	.asciz	"pass 3, read backwards: "
s_ticks: .asciz	" ticks, "
s_mis:	.asciz	" mismatches, sum "
s_wsum:	.asciz	" (written "
s_close: .asciz	")"
s_quit:	.asciz	"done; click or press a key to quit"
	.even
	.equ	V_Y,	0
	.equ	V_P,	4
	.equ	V_T0,	8
	.equ	V_T1,	12
	.equ	V_SUMW,	16
	.equ	V_HOLD,	20
vars:	.space	24
event:	.space	16
tmp:	.space	16
blocks:	.space	4 * BLOCKS
line:	.space	160
	.even
seg_end:
