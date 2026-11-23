| SPDX-License-Identifier: MPL-2.0
| tools/jitrom/jitrom.s - a stand-in ROM for the solo board that tests how the JIT notices changed code.
| GNU as MIT syntax, 68040, linked at $40800000 (tools/jitrom.sh builds it, boots it and checks it).
|
| Backstory (23 November 2026, docs/08 item 134). Octavo once drew Times 11 with the wrong letters
| on the first launch after a new build had been copied in, and a relaunch fixed it: the suspect was
| a translation of the old program's code that survived the new code's arrival. Emu68 checks a
| translated RAM block again (a CRC of its bytes) on its first entry after the Mac has cleared its
| instruction cache through CACR (psmac_icache_epoch, M68k_Translator.c). This program needs no
| Apple ROM: the kernel starts it like the IIci ROM, it writes small routines into RAM at $10000,
| runs them, changes them the way a loader would, clears the cache as Mac OS does on a 68030
| (CACR CI|CD) and runs them again, logging each answer through the INFO service. Cases 1-3 change
| only the branch that closes a loop: the CRC's range ended where the translator went next, the
| loop's start, so the branch was outside it and the old loop ran on (fixed the same day). Case 4
| replaces the whole routine with a flush. Case 5 replaces it with no flush at all: the stale answer
| (1) is the designed behaviour without mac.jitcheck=1, the new one (2) with it.
	.text
	.globl	_start
_start:
	.long	0x00080000		| SSP
	.long	reset			| PC

	.equ	MBOX,	0xF9001000
	.equ	X,	0x00010000	| where the test code goes

	.macro	logv text
	lea	9f(%pc),%a1
	bsr	do_logv
	bra.s	8f
9:	.ascii	"\text"
	.byte	0
	.even
8:
	.endm

	.macro	flush
	move.l	#0x0808,%d0		| CACR: CI and CD, as Mac OS clears a 68030's caches
	movec	%d0,%cacr
	.endm

	.macro	put from, to
	lea	\from(%pc),%a0
	lea	\to(%pc),%a1
	bsr	copy
	.endm

	.macro	run
	moveq	#3,%d1
	moveq	#7,%d2
	jsr	X
	.endm

reset:
	move.w	#0x2700,%sr
	lea	0x00080000,%sp
1:	cmp.l	#0x50534D42,MBOX-0x1000	| wait for the mailbox ('PSMB')
	bne.s	1b
	move.l	#20000000,%d3		| give the service core a moment after the magic appears
2:	subq.l	#1,%d3
	bne.s	2b
	moveq	#0,%d0
	logv	"JITROM: start"

| ---- 1. a loop whose closing Bcc changes, nothing else ----
	put	t1, t1_end
	flush
	run
	logv	"JITROM: 1a loop closed by bne (expect 00000005)"
	move.b	#0x63,X+(t1br-t1)	| bne.s -> bls.s: one pass more
	flush
	run
	logv	"JITROM: 1b the same loop closed by bls, after a flush (expect 00000006)"

| ---- 2. a loop whose closing DBcc's register changes ----
	put	t2, t2_end
	flush
	run
	logv	"JITROM: 2a dbf d1 (expect 00000004)"
	move.w	#0x51CA,X+(t2br-t2)	| dbf d1 -> dbf d2
	flush
	run
	logv	"JITROM: 2b dbf d2, after a flush (expect 00000008)"

| ---- 3. the closing bne.w's displacement word changes ----
	put	t3, t3_end
	flush
	run
	logv	"JITROM: 3a bcs.w back to the second addq (expect 00000009)"
	move.w	#(t3l2-(t3br+2)),X+(t3br+2-t3)	| now back to the first addq
	flush
	run
	logv	"JITROM: 3b bcs.w back to the first addq, after a flush (expect 0000000A)"

| ---- 4. the whole code replaced, with a flush ----
	put	t4a, t4a_end
	flush
	run
	logv	"JITROM: 4a moveq #1 (expect 00000001)"
	put	t4b, t4b_end
	flush
	run
	logv	"JITROM: 4b moveq #2 after a flush (expect 00000002)"

| ---- 5. the whole code replaced, no flush (stale is the designed behaviour) ----
	put	t4a, t4a_end
	flush
	run
	put	t4b, t4b_end
	run
	logv	"JITROM: 5 moveq #2 without a flush (00000002 new, 00000001 stale)"

	logv	"JITROM: done"
99:	bra.s	99b

copy:	move.l	#X,%a2
1:	move.w	(%a0)+,(%a2)+
	cmp.l	%a1,%a0
	bcs.s	1b
	rts

| ---- the templates, copied to X and run there ----
t1:	moveq	#0,%d0
1:	addq.l	#1,%d0
	cmpi.l	#5,%d0
t1br:	bne.s	1b
	rts
t1_end:

t2:	moveq	#0,%d0
	nop
	nop
1:	addq.l	#1,%d0
t2br:	dbf	%d1,1b
	rts
t2_end:

t3:	moveq	#0,%d0
t3l2:	addq.l	#1,%d0
t3l1:	addq.l	#1,%d0
	cmpi.l	#9,%d0
t3br:	.short	0x6500			| bcs.w
	.short	t3l1-(t3br+2)
	rts
t3_end:

t4a:	moveq	#1,%d0
	rts
t4a_end:
t4b:	moveq	#2,%d0
	rts
t4b_end:

| ---- the log: a1 = C string, d0 = value -> "text XXXXXXXX" ----
do_logv:
	movem.l	%d0-%d2/%a0-%a2,-(%sp)
	lea	-160(%sp),%sp
	move.l	%sp,%a2
1:	move.b	(%a1)+,(%a2)+
	bne.s	1b
	move.b	#' ',-1(%a2)
	moveq	#7,%d2
2:	rol.l	#4,%d0
	move.b	%d0,%d1
	and.b	#15,%d1
	add.b	#'0',%d1
	cmp.b	#'9',%d1
	bls.s	3f
	addq.b	#7,%d1
3:	move.b	%d1,(%a2)+
	dbf	%d2,2b
	clr.b	(%a2)
	move.l	%sp,%a1
	lea	MBOX,%a0
	moveq	#0,%d1
4:	tst.b	(%a1,%d1.w)
	beq.s	5f
	addq.w	#1,%d1
	bra.s	4b
5:	move.l	%d1,%d2
10:	move.w	#2,8(%a0)
	move.l	%a1,28(%a0)
	move.l	%d2,32(%a0)
	move.l	(%a0),%d0
	addq.l	#1,%d0
	move.l	%d0,(%a0)
	move.l	#50000000,%d1
6:	cmp.l	4(%a0),%d0
	beq.s	7f
	subq.l	#1,%d1
	bne.s	6b
	bra.s	10b			| not answered (the service core was not there yet): again
7:	lea	160(%sp),%sp
	movem.l	(%sp)+,%d0-%d2/%a0-%a2
	rts

	.org	0x80000-4
	.long	0
