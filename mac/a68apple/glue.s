| SPDX-License-Identifier: MPL-2.0
| mac/a68apple/glue.s - Aurora68 Apple II's calls that the generated Toolbox glue (mac/c, mktraps.py) cannot make:
| the register-based ones (the Device Manager's Open, Control and Status for the card's driver .A68A2, the File
| Manager's calls on a parameter block), Gestalt, and the Pascal routines the Toolbox calls back (Standard File's file
| filter and dialog filter, the Apple Event Manager's handler), each turned into a GCC call of a C function.
|
| Backstory (24 November 2026, docs/20-apple-ii-card.md "Disks from the Mac"). Until this day the program used Aurora68
| Video's glue (mac/a68player/glue.s: the driver's three calls and SetFPos); disks from the Mac need the File Manager
| at any depth (HOpen by volume, folder and name; GetCatInfo for the "Apple II Disks" folder; GetFCBInfo for the
| program's own folder), Standard File with filters, and Apple events. Written from Inside Macintosh (Files, Toolbox
| Essentials, Interapplication Communication). The callbacks keep D2-D7/A2-A6 for the Toolbox; C keeps them too.
	.text
	.macro	pbcall name, trap
	.globl	\name
\name:	move.l	4(%sp),%a0
	.short	\trap
	ext.l	%d0
	rts
	.endm
	pbcall	drv_open, 0xA000		| _Open (a driver's name)
	pbcall	drv_control, 0xA004		| _Control
	pbcall	drv_status, 0xA005		| _Status
	pbcall	fm_HOpen, 0xA200		| _HOpen (ioVRefNum, ioDirID, ioNamePtr: a file at any depth)
	pbcall	fm_Read, 0xA002			| _Read (ioPosMode fsFromStart: at an offset)
	pbcall	fm_Write, 0xA003		| _Write
	pbcall	fm_Close, 0xA001		| _Close
	pbcall	fm_GetEOF, 0xA011		| _GetEOF
	pbcall	fm_FlushFile, 0xA045		| _FlushFile

| _HFSDispatch ($A260) with its selector in D0
	.macro	hfscall name, sel
	.globl	\name
\name:	move.l	4(%sp),%a0
	moveq	#\sel,%d0
	.short	0xA260
	ext.l	%d0
	rts
	.endm
	hfscall	fm_DirCreate, 6
	hfscall	fm_GetFCBInfo, 8
	hfscall	fm_GetCatInfo, 9

| short a2_gestalt(long selector, long *response): Gestalt ($A1AD), the selector in D0, the answer in A0
	.globl	a2_gestalt
a2_gestalt:
	move.l	4(%sp),%d0
	.short	0xA1AD
	move.l	8(%sp),%a1
	move.l	%a0,(%a1)
	ext.l	%d0
	rts

| pascal Boolean (ParmBlkPtr pb): Standard File's file filter, TRUE to leave the file out -> int c_sf_filter(void *pb)
	.globl	sf_filter_stub
sf_filter_stub:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)		| 44 bytes: the return address at 44(sp), pb at 48
	move.l	48(%sp),-(%sp)
	jsr	c_sf_filter
	addq.l	#4,%sp
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	move.l	(%sp)+,%a0
	addq.l	#4,%sp				| pb popped: the result's word on top
	move.b	%d0,(%sp)			| a BOOLEAN: the word's high byte
	jmp	(%a0)

| pascal Boolean (DialogPtr d, EventRecord *ev, short *item): Standard File's dialog filter (SFPGetFile), called for
| every event of its dialog, null events too -> int c_sf_dfilter(void *d, EventRecord *ev, short *item)
	.globl	sf_dfilter_stub
sf_dfilter_stub:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)		| item at 48(sp), ev 52, d 56
	move.l	48(%sp),-(%sp)
	move.l	56(%sp),-(%sp)
	move.l	64(%sp),-(%sp)
	jsr	c_sf_dfilter
	lea	12(%sp),%sp
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	move.l	(%sp)+,%a0
	lea	12(%sp),%sp
	move.b	%d0,(%sp)
	jmp	(%a0)

| pascal OSErr (const AppleEvent *ev, AppleEvent *reply, long refcon): an Apple event handler -> short c_ae_event(const
| void *ev, long refcon) (the refcon says which event: its ID)
	.globl	ae_handler_stub
ae_handler_stub:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)		| refcon at 48(sp), reply 52, ev 56
	move.l	48(%sp),-(%sp)
	move.l	60(%sp),-(%sp)
	jsr	c_ae_event
	addq.l	#8,%sp
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	move.l	(%sp)+,%a0
	lea	12(%sp),%sp
	move.w	%d0,(%sp)
	jmp	(%a0)
