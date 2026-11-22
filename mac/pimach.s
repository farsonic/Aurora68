| pimach.s — "PiMach": tell the Mac it is a Macintosh SE (or Plus). MPL-2.0. GNU as MIT syntax, 68020.
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
| On the Plus board (mac.board=plus, 22 November 2026) the answers are the Plus's instead: Gestalt
| 4, gestaltMacPlus, and SysEnvirons' envMacPlus, 2; on the original Macintosh (mac.board=128k,
| 512k: gestaltClassic 1, envMac -1; 512ke: gestaltMac512KE 3, env512KE 1). The INIT reads which board it is on from the
| slot-9 window's header (psmb.h, struct psmb_header.board at offset 56, PSMB_BOARD_PLUS = 3);
| an older kernel leaves that word 0 and the answer stays the SE's.
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
	.equ	envMacPlus,	2
	.equ	env512KE,	1
	.equ	envMac,		-1		| the 128K and 512K
	.equ	gestaltMacPlus,	4
	.equ	gestaltMac512KE, 3
	.equ	gestaltClassic,	1		| Gestalt.h's name for the original Macintosh, 128K and 512K
	.equ	boardPlus,	3		| psmb.h PSMB_BOARD_PLUS
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
	beq.w	done			| no memory: stay a IIci
	move.l	%a0,%a1
	lea	sel_start(%pc),%a2
	move.l	#sel_end-sel_start,%d1
1:	move.b	(%a2)+,(%a1)+
	subq.l	#1,%d1
	bne.s	1b
	move.l	%a0,%a2			| the block: the selector function first, then the SysEnvirons patch
	.ifndef	MACH
	| the Plus board? the slot-9 window's header says (psmb.h: 'PSMB' at 0, the board at 56)
	lea	0xF9000000,%a1
	tst.b	0x0CB2			| MMU32Bit: 24-bit mode uses the slot's 24-bit alias
	bne.s	2f
	lea	0x00900000,%a1
2:	cmp.l	#0x50534D42,(%a1)
	bne.s	3f
	move.l	56(%a1),%d0		| the board: 3 Plus, 4 128K, 5 512K, 6 512Ke (psmb.h PSMB_BOARD_*)
	subq.l	#boardPlus,%d0
	bcs.s	3f			| 0-2: the IIci testbed, solo, the SE: the SE's answers
	cmp.l	#3,%d0
	bhi.s	3f
	add.w	%d0,%d0
	move.w	board_mach(%pc,%d0.w),%d1
	move.l	%d1,mach_val-sel_start(%a2)	| (high word clear: D1 was cleared above as the copy's counter)
	move.w	board_env(%pc,%d0.w),env_val-sel_start(%a2)
3:	move.l	%a2,%a0
	.endif
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
	movem.l	(%sp)+,%d0-%d2/%a0-%a2
	rts
| by board, from PSMB_BOARD_PLUS on: Gestalt 'mach' and SysEnvirons' machineType
board_mach:	.word	gestaltMacPlus, gestaltClassic, gestaltClassic, gestaltMac512KE
board_env:	.word	envMacPlus, envMac, envMac, env512KE
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
	move.l	mach_val(%pc),(%a0)
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
	move.w	env_val(%pc),2(%a0)
1:	rts
env_old:	.long	0
mach_val:	.long	gestaltMacSE		| the answers, set by the INIT for the board it is on
env_val:	.word	envSE
sel_end:
