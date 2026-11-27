| pivideo.s — ".Display_Video_PiStorm": the second display, a NuBus video card whose monitor is the
| Pi's HDMI output. MPL-2.0. GNU as MIT syntax, 68020.
|
| Backstory. Apple's video driver interface (Inside Macintosh: Devices, "Video Drivers"; Designing
| Cards and Drivers, "Video Cards") is small and old: a card declares its modes in its declaration
| ROM (tools/mac/mkdeclrom.py --video), the Start Manager opens the driver at boot, and from then on
| QuickDraw draws straight into the frame buffer while the driver answers a dozen Control and Status
| calls: which depth (cscSetMode / cscGetMode), the colour table (cscSetEntries / cscGetEntries),
| a grey screen after a mode change (cscGrayPage), gamma (ignored here), luminance mapping
| (cscSetGray), and whether the card should raise its vertical-blank interrupt (cscSetInterrupt).
| The hardware behind this driver is the service core: the frame buffer is the top 512 KiB of the
| slot window (MinorBaseOS 0x80000: $F9080000 in 32-bit mode, $980000 in 24-bit mode, which is
| how the ROM boots and how System 6 stays; the driver asks MMU32Bit which it is), the
| colour table and the depth live in the display block at window + $58000 (psmb.h, struct
| psmb_display), and the Pi converts the picture to its VideoCore frame buffer sixty times a
| second (emu68/src/psmac/psmac_display.c) and raises the slot interrupt at each tick while the
| driver asks for it. That interrupt is the card's VBL: the handler below calls DoVBLTask for
| slot 9, which runs QuickDraw's cursor task and whatever SlotVInstall queued for this screen.
|
| Modes 128 to 133 are 1, 2, 4, 8, 16 and 32 bits; the depth, pitch and frame buffer address of each
| come from the block's tables, which the Pi fills for the screen size in use (mac.screen=WxH or the
| VideoCore's). Not here yet: gamma tables, the sPRAMInit record for a remembered depth.
|
| Assembled twice (20 November 2026). On the SE board the Mac has two screens (docs/10, "Two screens
| on the SE"): the SE's own, the slot-9 card's, which this driver serves as before, and the HDMI
| screen, a second card in slot $A whose driver is this same source assembled with --defsym
| SCREEN2=1: another name (".Display_Video_PiHDMI"), the second display block (window + $59000,
| psmb.h), its VBL queue that of slot $A, and "PiHDMI:" in front of its log lines. The slot $A card
| is a declaration ROM and nothing more: the driver reaches the window at slot 9's addresses and
| takes slot 9's interrupt, where each handler claims only its own block's tick.
|
| Its own slot's interrupt (27 November 2026, docs/26). A/UX's kernel opens every video card's driver
| itself and keeps one slot interrupt handler per slot: the HDMI driver's SIntInstall on slot 9, beside
| the SE screen driver's, ended in "panic: Multiple interrupt handlers for slot 9", which is what locked
| the SE up under A/UX with the HDMI screen on. So the HDMI driver now installs its handler on its own
| card's slot, $A, and tells the Pi so in its display block (irq_slot, written before VBLs are wanted);
| the Pi then raises slot $A's bit of the RBV's slot register for its ticks instead of slot 9's line, as
| a real card in slot $A would. A driver from before writes nothing there and keeps slot 9.
|
| Its frames in its own slot's space (27 November 2026, docs/26). A/UX files each video card under the
| slot its frame address lies in; the HDMI card's pixels were at slot 9's addresses ($F9988000,
| $90000000), so A/UX took the HDMI card for slot 9's and drew the SE screen's pointer into the HDMI
| frame. The kernel now gives this card's modes addresses in slot $A ($FAA88000 for the small ones,
| $FA000000 for the big region's, the same Pi memory) when this driver carries the marker after its
| name, which says it takes slot $A's megabyte as reachable in 24-bit mode; and under A/UX the driver
| refuses a mode the calling Macintosh environment cannot reach (reach, below).

	.text
	.globl	drvr_start

	.equ	WIN32,		0xF9000000	| the slot window in 32-bit mode ...
	.equ	WIN24,		0x00900000	| ... and in 24-bit mode, where slot 9 is $900000-$9FFFFF (the slot
	.ifdef	SCREEN2				| number sits in bits 20-23, not 24-27: $F9080000 is RAM there)
	.equ	OFF_DISP,	0x59000		| the HDMI screen's block on the SE card (psmb.h, PSMB_OFF_DISPLAY2)
	.else
	.equ	OFF_DISP,	0x58000
	.endif
	.equ	OFF_FRAME,	0x80000
	.equ	FRAME_SIZE,	0x80000
	.equ	MMU32Bit,	0x0CB2
	.equ	DISP_MAGIC,	0x41363856	| 'A68V'
	.equ	D_MAGIC,	0
	.equ	D_WIDTH,	8
	.equ	D_HEIGHT,	12
	.equ	D_DEPTH,	16
	.equ	D_ROWBYTES,	20
	.equ	D_FLAGS,	32
	.equ	D_VBL_ENABLE,	36
	.equ	D_VBL_PENDING,	40
	.equ	D_IRQ_SLOT,	56		| the slot our handler is on, for the Pi (psmb.h irq_slot; 0 in drivers before it)
	.equ	D_AUX,		60		| Pi: not 0 while A/UX's tables are on (psmb.h aux, docs/26)
	.equ	D_CLUT_SEQ,	48
	.equ	D_CLUT,		64
	.equ	D_MODE_BASE,	1088		| 8 longs: the frame buffer's 68k address per mode 128+i (0: none)
	.equ	D_MODE_ROWBYTES, 1120		| 8 longs: the row pitch per mode
	.equ	D_BIG_BASE,	1152
	.equ	LAST_MODE_IX,	5		| modes 128..133: 1, 2, 4, 8, 16, 32 bits
	.equ	DISP_ENABLED,	1
	.equ	DISP_GRAY,	2

	.ifdef	SCREEN2
	.equ	SLOT,		10		| the slot whose interrupt our handler is on and the Pi raises: the card's own, $A
	.equ	VBL_SLOT,	10		| the slot whose VBL queue this screen's ticks run: its card's, $A
	.else
	.equ	SLOT,		9
	.equ	VBL_SLOT,	9
	.endif
	.equ	ioTrap,		6
	.equ	csCode,		26
	.equ	csParam,	28
	.equ	dCtlStorage,	20
	.equ	noQueueBit,	9
	.equ	JIODone,	0x08FC
	.equ	noErr,		0
	.equ	controlErr,	-17
	.equ	statusErr,	-18
	.equ	paramErr,	-50
	.equ	_NewPtrSysClear, 0xA71E
	.equ	_SIntInstall,	0xA075
	.equ	_SIntRemove,	0xA076
	.equ	JVBLTask,	0x0D28		| low memory: the Vertical Retrace Manager's DoVBLTask (D0.w = slot runs that
						| slot's VBL queue), called through this vector rather than as trap $A072

| video driver control codes (Video.h)
	.equ	cscReset,	0
	.equ	cscKillIO,	1
	.equ	cscSetMode,	2
	.equ	cscSetEntries,	3
	.equ	cscSetGamma,	4
	.equ	cscGrayPage,	5
	.equ	cscSetGray,	6
	.equ	cscSetInterrupt, 7
	.equ	cscDirectSetEntries, 8
	.equ	cscSetDefaultMode, 9
| status codes
	.equ	cscGetMode,	2
	.equ	cscGetEntries,	3
	.equ	cscGetPageCnt,	4
	.equ	cscGetPageBase,	5
	.equ	cscGetGray,	6
	.equ	cscGetInterrupt, 7
	.equ	cscGetGamma,	8
	.equ	cscGetDefaultMode, 9
| the records csParam points at
	.equ	csMode,		0		| VDPageInfo: csMode.w, csData.l, csPage.w, csBaseAddr.l
	.equ	csData,		2
	.equ	csPage,		6
	.equ	csBaseAddr,	8
	.equ	csTable,	0		| VDSetEntryRecord: csTable.l, csStart.w, csCount.w
	.equ	csStart,	4
	.equ	csCount,	6
	.equ	firstVidMode,	128		| mode ids: 128 one bit, 129 two, 130 four, 131 eight

	.equ	G_SIQ,		0		| SlotIntQElement: sqLink 4, sqType 2, sqPrio 2, sqAddr 4, sqParm 4
	.equ	G_MODE,		16		| word: the current mode id
	.equ	G_GRAY,		18		| word: luminance mapping on
	.equ	G_INT,		20		| word: VBL interrupts wanted
	.equ	G_INTS,		22		| long: VBL interrupts taken
	.equ	G_SIZE,		26

drvr_start:
	.short	0x4C00			| dNeedLock | dStatEnable | dCtlEnable
	.short	0, 0, 0			| drvrDelay, drvrEMask, drvrMenu
	.short	d_open-drvr_start
	.short	d_prime-drvr_start
	.short	d_control-drvr_start
	.short	d_status-drvr_start
	.short	d_close-drvr_start
	.ifdef	SCREEN2
	.byte	21
	.ascii	".Display_Video_PiHDMI"
	.even
	.ascii	"A68:HDMI frames in slot A"	| for the kernel: this driver knows its own slot's space (below)
	.else
	.byte	22
	.ascii	".Display_Video_PiStorm"
	.endif
	.even

| the name in front of every log line, so two drivers on one card can be told apart in the Pi's log
	.macro	log_name
	.ifdef	SCREEN2
	.ascii	"PiHDMI: "
	.else
	.ascii	"PiVideo: "
	.endif
	.endm

	.macro	log text
	movem.l	%d0-%d1/%a0-%a1,-(%sp)
	lea	9f(%pc),%a1
	bsr	do_log
	bra.s	8f
9:	log_name
	.ascii	"\text"
	.byte	0
	.even
8:	movem.l	(%sp)+,%d0-%d1/%a0-%a1
	.endm

| ---- open: find the display block, announce the driver, install the VBL handler ----
d_open:
	movem.l	%d1-%d7/%a0-%a4,-(%sp)
	move.l	%a1,%a4			| DCE
	log	"open"
	tst.l	dCtlStorage(%a4)
	bne	open_ok
	bsr	disp
	move.l	%a0,%a3
	cmp.l	#DISP_MAGIC,D_MAGIC(%a3)
	bne	open_fail		| the service core has no display block: no card
	move.l	#G_SIZE,%d0
	.short	_NewPtrSysClear
	move.l	%a0,%d0
	beq	open_fail
	move.l	%a0,dCtlStorage(%a4)
	move.l	%a0,%a2			| globals
	move.w	#firstVidMode,G_MODE(%a2)
	move.l	#1,D_DEPTH(%a3)
	move.l	D_MODE_ROWBYTES(%a3),D_ROWBYTES(%a3)
	| the one-bit table: 0 white, 1 black (QuickDraw's convention for indexed modes)
	move.l	#0x00FFFFFF,D_CLUT(%a3)
	clr.l	D_CLUT+4(%a3)
	addq.l	#1,D_CLUT_SEQ(%a3)
	bsr	gray_fill
	| the VBL handler: a slot interrupt handler of our own (Inside Macintosh V, SIntInstall)
	lea	G_SIQ(%a2),%a0
	move.w	#6,4(%a0)		| sqType = sIQType
	move.w	#1,6(%a0)		| sqPrio: between the network driver's and the catch-all
	lea	slot_int(%pc),%a1
	move.l	%a1,8(%a0)		| sqAddr
	move.l	%a2,12(%a0)		| sqParm: our globals, handed back in A1
	moveq	#SLOT,%d0
	.short	_SIntInstall
	tst.w	%d0
	bne.s	1f
	log	"VBL handler installed"
1:
	.ifdef	SCREEN2
	move.l	#SLOT,D_IRQ_SLOT(%a3)	| the slot the Pi is to raise for our ticks: before they are wanted
	.endif
	move.l	#1,D_VBL_ENABLE(%a3)	| a video card's Open turns its VBL interrupts on (Designing Cards and
	move.w	#1,G_INT(%a2)		| Drivers); cscSetInterrupt is how the system turns them off later. Without
					| this the cursor never appears on the second screen: QuickDraw draws it
					| from the screen's slot VBL task, which only DoVBLTask from our handler runs.
					| Only once the handler is in: a tick before it raised slot 9's line with
					| nobody to lower it (20 November 2026, docs/10)
	move.l	#DISP_ENABLED,D_FLAGS(%a3)	| the Pi starts converting frames
	log	"1 bit; the frame buffer and the size are the display block's"
open_ok:
	moveq	#noErr,%d0
	movem.l	(%sp)+,%d1-%d7/%a0-%a4
	rts
open_fail:
	log	"open failed (no display block)"
	moveq	#-1,%d0			| openErr
	movem.l	(%sp)+,%d1-%d7/%a0-%a4
	rts

d_prime:
	moveq	#paramErr,%d0
	bra	iodone

d_close:
	move.l	dCtlStorage(%a1),%d0
	beq.s	1f
	move.l	%d0,%a2
	bsr	disp
	move.l	%a0,%a1
	move.l	%a2,%a0
	clr.l	D_FLAGS(%a1)
	clr.l	D_VBL_ENABLE(%a1)
	.ifdef	SCREEN2
	clr.l	D_IRQ_SLOT(%a1)
	.endif
	lea	G_SIQ(%a0),%a0
	moveq	#SLOT,%d0
	.short	_SIntRemove
1:	moveq	#noErr,%d0
	rts

| ---- control ----
d_control:
	movem.l	%d1-%d7/%a0-%a4,-(%sp)
	move.l	%a0,%a3			| PB
	move.l	%a1,%a4			| DCE
	move.l	dCtlStorage(%a4),%a2	| globals
	bsr	disp			| a0 = the display block
	move.l	csParam(%a3),%a1	| the record
	move.w	csCode(%a3),%d0
	move.w	#'c',%d1
	bsr	trace			| "PiVideo: c NN mode M" in the Pi's log: what the system asks, and when
	cmp.w	#cscReset,%d0
	beq.s	c_reset
	cmp.w	#cscKillIO,%d0
	beq	c_ok
	cmp.w	#cscSetMode,%d0
	beq.s	c_setmode
	cmp.w	#cscSetEntries,%d0
	beq	c_setentries
	cmp.w	#cscDirectSetEntries,%d0
	beq	c_setentries
	cmp.w	#cscSetGamma,%d0
	beq	c_ok			| no gamma table yet: the Pi shows the colours as given
	cmp.w	#cscGrayPage,%d0
	beq	c_graypage
	cmp.w	#cscSetGray,%d0
	beq	c_setgray
	cmp.w	#cscSetInterrupt,%d0
	beq	c_setint
	cmp.w	#cscSetDefaultMode,%d0
	beq	c_ok			| no PRAM yet: the card wakes up in one bit
	moveq	#controlErr,%d0
	bra	c_out
c_reset:
	move.w	#firstVidMode,G_MODE(%a2)
	move.l	#1,D_DEPTH(%a0)
	move.l	D_MODE_ROWBYTES(%a0),D_ROWBYTES(%a0)
	clr.w	G_GRAY(%a2)
	bsr	gray_fill
	bra	c_ok
c_setmode:
	| csMode: 128..133 (1, 2, 4, 8, 16, 32 bits); csPage must be 0 (one page); returns csBaseAddr.
	| The depth, the row pitch and the frame buffer's address come from the block's mode tables:
	| the Pi decided them for this screen size (indexed 640 x 480 modes live in the window where
	| a 24-bit system can reach them, everything else in the big region).
	move.w	csMode(%a1),%d1
	sub.w	#firstVidMode,%d1
	bmi	c_param
	cmp.w	#LAST_MODE_IX,%d1
	bhi	c_param
	tst.w	csPage(%a1)
	bne	c_param
	lea	D_MODE_BASE(%a0),%a3
	move.l	(%a3,%d1.w*4),%d2
	beq	c_param			| no such mode at this screen size
	.ifdef	SCREEN2
	move.l	%d2,%d0
	bsr	reach			| a mode whose pixels the caller cannot reach is refused (Monitors offers every
	bne	c_param			| mode the card has; 20 November 2026; A/UX, 27 November 2026)
	.else				| (the one-screen and SE screen drivers as they were, byte for byte)
	tst.b	MMU32Bit
	bne.s	1f
	and.l	#0xFFF00000,%d2		| in 24-bit mode only the slot's megabyte is reachable: a mode in the big
	cmp.l	#0xF9900000,%d2		| region ($90000000) would put QuickDraw's pixels on the exception vectors
	bne	c_param			| (Monitors offers every mode the card has; 20 November 2026)
1:
	.endif
	move.w	csMode(%a1),G_MODE(%a2)
	moveq	#1,%d2
	lsl.w	%d1,%d2			| depth 1, 2, 4, 8, 16, 32
	move.l	%d2,D_DEPTH(%a0)
	lea	D_MODE_ROWBYTES(%a0),%a3
	move.l	(%a3,%d1.w*4),D_ROWBYTES(%a0)
	bsr	frame_addr
	bra	c_ok
c_setentries:
	| csTable -> ColorSpec[]: value.w, red.w, green.w, blue.w; csStart -1: use the values as indexes;
	| csCount is the number of entries minus one
	move.l	csTable(%a1),%d0
	beq	c_param
	move.l	%d0,%a1
	move.l	csParam(%a3),%d0
	move.l	%d0,%a3			| a3 = the record now (the PB pointer is on the stack)
	move.w	csStart(%a3),%d3
	move.w	csCount(%a3),%d4
	moveq	#0,%d5			| index when csStart >= 0
	move.w	%d3,%d5
1:	move.w	(%a1),%d6		| value
	tst.w	%d3
	bmi.s	2f
	move.w	%d5,%d6			| sequential
2:	and.w	#0xFF,%d6
	move.w	2(%a1),%d0		| red: the high byte of each 16-bit component
	lsr.w	#8,%d0
	moveq	#0,%d7
	move.b	%d0,%d7
	lsl.l	#8,%d7
	move.w	4(%a1),%d0		| green
	lsr.w	#8,%d0
	move.b	%d0,%d7
	lsl.l	#8,%d7
	move.w	6(%a1),%d0		| blue
	lsr.w	#8,%d0
	move.b	%d0,%d7
	lea	D_CLUT(%a0),%a2
	move.l	%d7,(%a2,%d6.w*4)
	addq.l	#8,%a1
	addq.w	#1,%d5
	dbf	%d4,1b
	addq.l	#1,D_CLUT_SEQ(%a0)
	bra	c_ok
c_graypage:
	tst.w	csPage(%a1)
	bne	c_param
	bsr	gray_fill
	bra	c_ok
c_setgray:
	moveq	#0,%d1
	move.b	csMode(%a1),%d1		| 0 colour, 1 luminance
	move.w	%d1,G_GRAY(%a2)
	move.l	D_FLAGS(%a0),%d0
	and.l	#~DISP_GRAY,%d0
	tst.w	%d1
	beq.s	1f
	or.l	#DISP_GRAY,%d0
1:	move.l	%d0,D_FLAGS(%a0)
	addq.l	#1,D_CLUT_SEQ(%a0)	| the Pi rebuilds its table with or without the mapping
	bra	c_ok
c_setint:
	moveq	#0,%d1
	move.b	csMode(%a1),%d1		| 0 enable, 1 disable
	eor.w	#1,%d1			| -> 1 = wanted
	move.w	%d1,G_INT(%a2)
	move.l	%d1,D_VBL_ENABLE(%a0)
	bra	c_ok
c_param:
	moveq	#paramErr,%d0
	bra.s	c_out
c_ok:
	moveq	#noErr,%d0
c_out:
	movem.l	(%sp)+,%d1-%d7/%a0-%a4	| a0 = PB, a1 = DCE again for IODone
	bra	iodone

| ---- status ----
d_status:
	movem.l	%d1-%d7/%a0-%a4,-(%sp)
	move.l	%a0,%a3
	move.l	%a1,%a4
	move.l	dCtlStorage(%a4),%a2
	bsr	disp
	move.l	csParam(%a3),%a1
	move.w	csCode(%a3),%d0
	move.w	#'s',%d1
	bsr	trace
	cmp.w	#cscGetMode,%d0
	beq.s	s_getmode
	cmp.w	#cscGetEntries,%d0
	beq.s	s_getentries
	cmp.w	#cscGetPageCnt,%d0
	beq	s_pagecnt
	cmp.w	#cscGetPageBase,%d0
	beq	s_pagebase
	cmp.w	#cscGetGray,%d0
	beq	s_getgray
	cmp.w	#cscGetInterrupt,%d0
	beq	s_getint
	cmp.w	#cscGetDefaultMode,%d0
	beq	s_defmode
	moveq	#statusErr,%d0		| cscGetGamma and the rest
	bra	c_out
s_getmode:
	move.w	G_MODE(%a2),csMode(%a1)
	clr.w	csPage(%a1)
	bsr	frame_addr
	bra	c_ok
s_getentries:
	move.l	csTable(%a1),%d0
	beq	c_param
	move.w	csStart(%a1),%d3
	move.w	csCount(%a1),%d4
	move.l	%d0,%a1
	moveq	#0,%d5
	move.w	%d3,%d5
	lea	D_CLUT(%a0),%a2
1:	move.w	(%a1),%d6
	tst.w	%d3
	bmi.s	2f
	move.w	%d5,%d6
	move.w	%d6,(%a1)
2:	and.w	#0xFF,%d6
	move.l	(%a2,%d6.w*4),%d7	| 0x00RRGGBB -> three 16-bit components (byte doubled)
	move.l	%d7,%d0
	swap	%d0
	bsr	wide
	move.w	%d0,2(%a1)
	move.l	%d7,%d0
	lsr.l	#8,%d0
	bsr	wide
	move.w	%d0,4(%a1)
	move.l	%d7,%d0
	bsr	wide
	move.w	%d0,6(%a1)
	addq.l	#8,%a1
	addq.w	#1,%d5
	dbf	%d4,1b
	bra	c_ok
s_pagecnt:
	move.w	#1,csPage(%a1)
	bra	c_ok
s_pagebase:
	tst.w	csPage(%a1)
	bne	c_param
	bsr	frame_addr
	bra	c_ok
| frame_addr: csBaseAddr of the record at a1 = the frame buffer, always in its 32-bit form. The ROM's
| own built-in video answers $FBB08000 in 24-bit mode and QuickDraw works with the low 24 bits there,
| $B08000; ours is $F9980000, the slot's 24-bit megabyte seen from 32-bit space, $980000 in 24-bit.
| (The first version answered $980000 and the ROM made $F9980000 of it by itself, which told the
| convention; the kernel maps the window at $F9900000 as well as $F9000000 so both forms reach it.)
frame_addr:
	movem.l	%d0/%a0/%a3,-(%sp)
	bsr	disp
	moveq	#0,%d0
	move.w	G_MODE(%a2),%d0
	sub.w	#firstVidMode,%d0
	lea	D_MODE_BASE(%a0),%a3
	move.l	(%a3,%d0.w*4),csBaseAddr(%a1)
	movem.l	(%sp)+,%d0/%a0/%a3
	rts
s_getgray:
	move.w	G_GRAY(%a2),%d0
	move.b	%d0,csMode(%a1)
	bra	c_ok
s_getint:
	move.w	G_INT(%a2),%d0
	eor.w	#1,%d0			| 0 = enabled, 1 = disabled
	move.b	%d0,csMode(%a1)
	bra	c_ok
s_defmode:
	move.b	#firstVidMode,csMode(%a1)
	bra	c_ok
| wide: d0.b -> d0.w = the byte in both halves (8-bit component to 16-bit)
wide:
	and.w	#0xFF,%d0
	move.w	%d0,%d1
	lsl.w	#8,%d1
	or.w	%d1,%d0
	rts

| ---- the grey screen: a 50% pattern of index 0 and the depth's last index, rows alternating; the
| direct modes get a flat mid grey. Rows and pitch from the block, the buffer from the mode table.
gray_fill:
	movem.l	%d0-%d4/%a0-%a2,-(%sp)
	bsr	disp
	move.l	%a0,%a1
	moveq	#0,%d0
	move.w	G_MODE(%a2),%d0
	sub.w	#firstVidMode,%d0
	lea	D_MODE_BASE(%a1),%a0
	move.l	(%a0,%d0.w*4),%d4
	beq	9f
	.ifdef	SCREEN2
	move.l	%d4,%d0			| a frame the Mac cannot reach now is left alone: the big region at $90000000
	move.l	%a1,%a0			| in 24-bit mode would come out as $000000, the exception vectors
	bsr	reach
	bne	9f
	.else
	tst.b	MMU32Bit
	bne.s	5f
	move.l	%d4,%d0			| 24-bit mode reaches only the slot's own megabyte ($F99xxxxx is $9xxxxx);
	and.l	#0xFFF00000,%d0		| the big region at $90000000 would come out as $000000, the
	cmp.l	#0xF9900000,%d0		| exception vectors, so a direct mode is left alone until the
	bne	9f			| Mac is in 32-bit mode
5:
	.endif
	move.l	%d4,%a0			| the frame buffer
	move.l	D_DEPTH(%a1),%d0
	move.b	#0x88,%d1		| 1 bit: 10001000 / 00100010, a 25% grey: told apart from the desktop's
	move.b	#0x22,%d2		| 50% pattern in a picture of the Pi's screen (the real cards used 50%)
	cmp.l	#1,%d0
	beq.s	1f
	move.b	#0x33,%d1		| 2 bits: 00 11 00 11 / 11 00 11 00
	move.b	#0xCC,%d2
	cmp.l	#2,%d0
	beq.s	1f
	move.b	#0x0F,%d1		| 4 bits
	move.b	#0xF0,%d2
	cmp.l	#4,%d0
	beq.s	1f
	move.b	#0x00,%d1		| 8 bits: 00 FF / FF 00, two bytes a step
	move.b	#0xFF,%d2
	cmp.l	#8,%d0
	beq.s	1f
	move.b	#0x42,%d1		| 16 bits: 0x4210 = 0 10000 10000 10000, mid grey, word by word (0x7BDE was nearly white)
	move.b	#0x10,%d2
	cmp.l	#16,%d0
	beq.s	1f
	move.b	#0x80,%d1		| 32 bits: 00 80 80 80 as 80 80 80 00 pairs is close enough: mid grey
	move.b	#0x80,%d2
1:	move.l	D_HEIGHT(%a1),%d3	| rows
2:	move.l	D_ROWBYTES(%a1),%d0
	btst	#0,%d3
	beq.s	3f
	cmp.l	#8,D_DEPTH(%a1)
	bhi.s	3f			| direct modes: no alternation
	exg	%d1,%d2			| odd rows start with the other byte
3:	bsr.s	row_fill
	btst	#0,%d3
	beq.s	4f
	cmp.l	#8,D_DEPTH(%a1)
	bhi.s	4f
	exg	%d1,%d2
4:	subq.l	#1,%d3
	bne.s	2b
9:	movem.l	(%sp)+,%d0-%d4/%a0-%a2
	rts
| row_fill: a0 -> row (advanced), d0 = bytes, d1/d2 = the two bytes; 8-bit and wider rows alternate them
row_fill:
	movem.l	%d0/%d3,-(%sp)
	move.l	D_DEPTH(%a1),%d3
	cmp.l	#8,%d3
	bhs.s	2f
1:	move.b	%d1,(%a0)+
	subq.l	#1,%d0
	bne.s	1b
	bra.s	9f
2:	move.b	%d1,(%a0)+
	move.b	%d2,(%a0)+
	subq.l	#2,%d0
	bne.s	2b
9:	movem.l	(%sp)+,%d0/%d3
	rts

| ---- the slot interrupt handler: the card's VBL ----
| A1 = sqParm (globals). Ours only when the Pi flagged a tick: clear it, run our card's VBL queue
| (the cursor, SlotVInstall tasks), say so with D0 = 1.
slot_int:
	movem.l	%d1-%d2/%a0-%a2,-(%sp)
	move.l	%a1,%a2
	bsr	disp
	tst.l	D_VBL_PENDING(%a0)
	beq.s	1f
	clr.l	D_VBL_PENDING(%a0)
	addq.l	#1,G_INTS(%a2)
	cmp.l	#1,G_INTS(%a2)
	bne.s	2f
	log	"first VBL interrupt"
2:	moveq	#VBL_SLOT,%d0
	move.l	JVBLTask,%a0		| DoVBLTask through its low-memory vector, as Apple's own video drivers call it
	jsr	(%a0)			| (Designing Cards and Drivers' sample driver), not as trap $A072: A/UX runs a
					| card's driver in its kernel and refuses that trap there ("Attempt to use
					| unsupported A-line trap: A072", then a panic at the first VBL; 26 November 2026,
					| docs/23 section 15), and the vector is the faster path under Mac OS too
	moveq	#1,%d0
	bra.s	3f
1:	moveq	#0,%d0
3:	movem.l	(%sp)+,%d1-%d2/%a0-%a2
	rts

| reach: d0 = a mode's frame address, a0 = the display block -> Z set (d0 = 0) if the Mac that asked can draw there.
| A frame in the card's own megabyte ($F99xxxxx, the HDMI card's $FAAxxxxx) is reachable in either addressing mode;
| anything else only in 32-bit mode. Under A/UX (27 November 2026, docs/26) this driver runs in A/UX's kernel, whose
| low memory is not the Macintosh environment's, and the Pi says so in the block (D_AUX): then the addressing mode
| is the calling environment's own MMU32Bit, read from its address space with MOVES (user data), and in 32-bit mode
| only the card's own 16 MiB is reachable, which is what A/UX maps for it ($90000000, the super slot space, is not).
| A refusal is paramErr, the Monitors control panel then keeps the depth it had.
	.ifdef	SCREEN2
reach:
	movem.l	%d1/%a1,-(%sp)
	move.l	%d0,%d1
	and.l	#0xFFF00000,%d1
	cmp.l	#0xF9900000,%d1
	beq.s	8f
	.ifdef	SCREEN2
	cmp.l	#0xFAA00000,%d1
	beq.s	8f
	.endif
	tst.l	D_AUX(%a0)
	bne.s	2f
	tst.b	MMU32Bit		| Mac OS: its own low memory says
	beq.s	9f
	bra.s	8f
2:	movec	%sfc,%a1		| A/UX: the calling Macintosh environment's MMU32Bit
	moveq	#1,%d1
	movec	%d1,%sfc
	move.l	%a1,-(%sp)
	lea	MMU32Bit,%a1
	moves.b	(%a1),%d1
	move.l	(%sp)+,%a1
	movec	%a1,%sfc
	tst.b	%d1
	beq.s	9f
	and.l	#0xFF000000,%d0		| and then only the card's own slot space
	.ifdef	SCREEN2
	cmp.l	#0xFA000000,%d0
	.else
	cmp.l	#0xF9000000,%d0
	.endif
	bne.s	9f
8:	moveq	#0,%d0
	movem.l	(%sp)+,%d1/%a1
	rts
9:	moveq	#1,%d0
	movem.l	(%sp)+,%d1/%a1
	rts
	.endif

| trace: d0.w = csCode, d1.b = 'c' or 's' -> the log line "PiVideo: c NN mode M" (M: 24 or 32-bit
| addressing at the time). The first 48 calls only; it tells how the system drives a video card.
trace:
	movem.l	%d0-%d2/%a0-%a1,-(%sp)
	move.w	trace_left(%pc),%d2
	beq.s	9f
	subq.w	#1,%d2
	lea	trace_left(%pc),%a1
	move.w	%d2,(%a1)
	lea	trace_cs(%pc),%a0
	move.b	%d1,(%a0)+
	addq.l	#1,%a0
	moveq	#0,%d2
	move.b	%d0,%d2
	move.b	%d2,%d1
	lsr.b	#4,%d1
	add.b	#'0',%d1
	cmp.b	#'9',%d1
	bls.s	1f
	addq.b	#7,%d1
1:	move.b	%d1,(%a0)+
	and.b	#15,%d2
	add.b	#'0',%d2
	cmp.b	#'9',%d2
	bls.s	2f
	addq.b	#7,%d2
2:	move.b	%d2,(%a0)
	lea	trace_mode(%pc),%a0
	move.b	#'2',(%a0)
	move.b	#'4',1(%a0)
	tst.b	MMU32Bit
	beq.s	3f
	move.b	#'3',(%a0)
	move.b	#'2',1(%a0)
3:	lea	trace_text(%pc),%a1
	bsr	do_log
9:	movem.l	(%sp)+,%d0-%d2/%a0-%a1
	rts
trace_left:	.word	48
trace_text:	log_name
trace_cs:	.ascii	"c NN mode "
trace_mode:	.ascii	"MM"
	.byte	0
	.even

| window: a0 = the slot window for the addressing mode the Mac is in now
window:
	lea	WIN32,%a0
	tst.b	MMU32Bit
	bne.s	1f
	lea	WIN24,%a0
1:	rts
| disp: a0 = the display block; frame: a0 = the frame buffer (both for the current mode)
disp:
	bsr.s	window
	lea	OFF_DISP(%a0),%a0
	rts
frame:
	bsr.s	window
	lea	OFF_FRAME(%a0),%a0
	rts

| ---- common exit: immediate calls return, queued calls go through IODone ----
iodone:
	btst	#noQueueBit,ioTrap(%a0)
	beq.s	1f
	rts
1:	move.l	JIODone,-(%sp)
	rts

| ---- the Pi's log through the INFO service (as the other drivers do it) ----
do_log:
	lea	WIN32,%a0
	tst.b	0x0CB2
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
	move.l	#200000,%d1
4:	cmp.l	4(%a0),%d0
	beq.s	5f
	subq.l	#1,%d1
	bne.s	4b
5:	rts
