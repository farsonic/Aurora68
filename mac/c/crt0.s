| mac/c/crt0.s - the start of a C application for the classic Mac OS (mac/c/README.md).
| MPL-2.0. The program is linked at 0 (mac/c/app.ld) and packed by tools/mac/elf2app.py into
| CODE 1, whose entry point is the first byte here. At the start it adds its load address to
| every 32-bit absolute field (the table elf2app.py puts after the image), flushes the
| instruction cache, initialises the Toolbox managers and calls main(); main's return quits.
| The Segment Loader keeps CODE 1 locked while the program runs, and the program never calls
| UnloadSeg, so the fixed-up image stays where it is.

	.text
	.globl	_start
	.globl	_reloc_off
_start:
	bra.s	1f
_reloc_off:
	.long	0			| filled in by elf2app.py: the offset of the relocation table
1:	lea	_start(%pc),%a3
	move.l	_reloc_off(%pc),%d2
	lea	(%a3,%d2.l),%a4
	move.l	(%a4)+,%d3
	move.l	%a3,%d4
	bra.s	3f
2:	move.l	(%a4)+,%d2
	add.l	%d4,(%a3,%d2.l)
3:	subq.l	#1,%d3
	bcc.s	2b
	cmp.w	#0x0605,0x15A		| SysVersion: FlushInstructionCache (HWPriv 1) from 6.0.5
	bcs.s	4f
	moveq	#1,%d0
	.short	0xA198			| _HWPriv
4:	.short	0xA063			| _MaxApplZone
	moveq	#7,%d7
5:	.short	0xA036			| _MoreMasters (a few blocks of master pointers, early)
	dbf	%d7,5b
	pea	-4(%a5)
	.short	0xA86E			| _InitGraf
	.short	0xA8FE			| _InitFonts
	move.l	#0x0000FFFF,%d0
	.short	0xA032			| _FlushEvents (all of them)
	.short	0xA912			| _InitWindows
	.short	0xA930			| _InitMenus
	.short	0xA9CC			| _TEInit
	clr.l	-(%sp)
	.short	0xA97B			| _InitDialogs
	.short	0xA850			| _InitCursor
	bsr.l	main
	.short	0xA9F4			| _ExitToShell

| Pascal callbacks into C. A routine the Toolbox calls (a control's action procedure, a dialog
| filter) takes its arguments the Pascal way and pops them itself; these turn the call into a
| GCC one. The C function keeps D2-D7/A2-A6 as the Toolbox expects.

| pascal void (ControlHandle ctl, short part) -> void c_action(void *ctl, int part)
	.globl	pas_ctl_action
pas_ctl_action:
	move.w	4(%sp),%d0
	ext.l	%d0
	move.l	6(%sp),%a0
	move.l	%d0,-(%sp)
	move.l	%a0,-(%sp)
	bsr.l	c_ctl_action
	addq.l	#8,%sp
	move.l	(%sp)+,%a0
	addq.l	#6,%sp
	jmp	(%a0)

| void *mac_newptr(long size): NewPtrClear in the application heap, NULL when there is no room
	.globl	mac_newptr
mac_newptr:
	move.l	4(%sp),%d0
	.short	0xA31E			| _NewPtr,clear
	move.l	%a0,%d0
	rts
