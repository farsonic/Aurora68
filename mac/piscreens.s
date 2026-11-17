| piscreens.s — "PiScreens": QuickDraw's list of screens, for the second display's bring-up. MPL-2.0.
| GNU as MIT syntax, 68020, packaged by tools/mac/mkapp.py.
|
| QuickDraw keeps every screen as a GDevice (Inside Macintosh V, "Graphics Devices"): its rectangle
| on the desktop, flags (active, main screen, ...), its PixMap (base address, row bytes, depth) and
| the driver's reference number. The Pi's video card (docs/10) is found and set up by the ROM, but
| whether the system then draws on it depends on what ended up in this list: a screen that is not
| active, or whose base address was taken in 24-bit mode and kept after the switch to 32-bit, is
| exactly what this program shows and nothing else would. Lines: one per device, then MMU32Bit.

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
	.equ	_GetDeviceList,	0xAA29		| Pascal functions: a long result slot, then the call
	.equ	_GetNextDevice,	0xAA2A
	.equ	_GetMainDevice,	0xAA2D
	.equ	_SlotManager,	0xA06E
	.equ	UTableBase,	0x011C
	.equ	MMU32Bit,	0x0CB2
	.equ	keyDown,	3
	.equ	autoKey,	5
	.equ	mouseDown,	1
	.equ	LINEH,		12
| GDevice (handle -> pointer -> record)
	.equ	gdRefNum,	0
	.equ	gdType,		4
	.equ	gdFlags,	20
	.equ	gdPMap,		22
	.equ	gdNextGD,	30
	.equ	gdRect,		34
| PixMap
	.equ	pmBaseAddr,	0
	.equ	pmRowBytes,	4
	.equ	pmPixelSize,	32

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
	move.l	(%sp)+,%d7
	move.l	%d7,-(%sp)
	.short	_SetPort
	move.w	#4,-(%sp)
	.short	_TextFont
	move.w	#9,-(%sp)
	.short	_TextSize
	lea	vars(%pc),%a6
	move.w	#14,V_Y(%a6)
	move.w	#8,V_X(%a6)
	lea	s_title(%pc),%a0
	bsr	say
	lea	s_head(%pc),%a0
	bsr	say
	clr.l	-(%sp)
	.short	_GetMainDevice
	move.l	(%sp)+,%d6		| the main screen's handle
	clr.l	-(%sp)
	.short	_GetDeviceList
	move.l	(%sp)+,%d5		| the first handle
	moveq	#0,%d4			| device number
next_dev:
	tst.l	%d5
	beq	done
	move.l	%d5,%a0
	move.l	(%a0),%a3		| the GDevice
	move.l	%d4,%d0
	bsr	say_dec
	lea	s_sep(%pc),%a0
	bsr	say_prefix
	| rect: top left bottom right
	move.w	gdRect(%a3),%d0
	ext.l	%d0
	bsr	say_sdec
	move.w	gdRect+2(%a3),%d0
	ext.l	%d0
	bsr	say_sdec
	move.w	gdRect+4(%a3),%d0
	ext.l	%d0
	bsr	say_sdec
	move.w	gdRect+6(%a3),%d0
	ext.l	%d0
	bsr	say_sdec
	lea	s_flags(%pc),%a0
	bsr	say_prefix
	moveq	#0,%d0
	move.w	gdFlags(%a3),%d0
	moveq	#3,%d1
	bsr	say_hex
	lea	s_depth(%pc),%a0
	bsr	say_prefix
	move.l	gdPMap(%a3),%a0
	move.l	(%a0),%a2		| the PixMap
	moveq	#0,%d0
	move.w	pmPixelSize(%a2),%d0
	bsr	say_dec
	lea	s_base(%pc),%a0
	bsr	say_prefix
	move.l	pmBaseAddr(%a2),%d0
	moveq	#7,%d1
	bsr	say_hex
	lea	s_rowbytes(%pc),%a0
	bsr	say_prefix
	moveq	#0,%d0
	move.w	pmRowBytes(%a2),%d0
	and.w	#0x3FFF,%d0
	bsr	say_dec
	lea	s_ref(%pc),%a0
	bsr	say_prefix
	move.w	gdRefNum(%a3),%d0
	ext.l	%d0
	bsr	say_sdec
	cmp.l	%d6,%d5
	bne.s	1f
	lea	s_main(%pc),%a0
	bsr	say_prefix
1:	bsr	crlf
	addq.l	#1,%d4
	cmp.l	#8,%d4
	bhs.s	done			| eight is plenty; a cycle in the list would never end
	move.l	gdNextGD(%a3),%d5	| the next device's handle, straight from the record
	bra	next_dev
done:
	| the video sResources as the ROM's 'scrn' loader enumerates them (sNextTypeSRsrc, category
	| 3 type 1 software 1, any hardware), with what it compares: spDrvrHW, spSlot, and the driver's
	| dCtlDevBase from its DCE (the unit table entry for spRefNum). docs/10 says why.
	bsr	crlf
	lea	s_srsrc(%pc),%a0
	bsr	say
	lea	spblock(%pc),%a0
	move.l	#0x00030001,40(%a0)	| spCategory 3, spCType 1
	move.w	#1,44(%a0)		| spDrvrSW
	move.b	#1,48(%a0)		| spTBMask: match category, type and software only
	clr.b	49(%a0)
	clr.b	50(%a0)
	clr.b	51(%a0)
next_sr:
	lea	spblock(%pc),%a0
	moveq	#21,%d0			| sNextTypeSRsrc
	.short	_SlotManager
	tst.w	%d0
	bne	sr_done
	lea	spblock(%pc),%a0
	lea	s_slot(%pc),%a0
	bsr	say_prefix
	lea	spblock(%pc),%a0
	moveq	#0,%d0
	move.b	49(%a0),%d0		| spSlot
	bsr	say_dec
	lea	s_id(%pc),%a0
	bsr	say_prefix
	lea	spblock(%pc),%a0
	moveq	#0,%d0
	move.b	50(%a0),%d0		| spID
	bsr	say_dec
	lea	s_hw(%pc),%a0
	bsr	say_prefix
	lea	spblock(%pc),%a0
	moveq	#0,%d0
	move.w	46(%a0),%d0		| spDrvrHW
	moveq	#3,%d1
	bsr	say_hex
	lea	s_ref(%pc),%a0
	bsr	say_prefix
	lea	spblock(%pc),%a0
	move.w	38(%a0),%d0		| spRefNum
	ext.l	%d0
	bsr	say_sdec
	lea	spblock(%pc),%a0
	move.w	38(%a0),%d0
	not.w	%d0
	asl.w	#2,%d0
	move.l	UTableBase,%a1
	move.l	(%a1,%d0.w),%a1		| the DCE handle
	move.l	(%a1),%a3		| the DCE (in a3: the Toolbox keeps A2-A6, not A1, across the text calls)
	lea	s_devbase(%pc),%a0
	bsr	say_prefix
	move.l	42(%a3),%d0		| dCtlDevBase
	moveq	#7,%d1
	bsr	say_hex
	lea	s_dslot(%pc),%a0
	bsr	say_prefix
	moveq	#0,%d0
	move.b	40(%a3),%d0		| dCtlSlot
	bsr	say_dec
	lea	s_sep(%pc),%a0
	bsr	say_prefix
	moveq	#0,%d0
	move.b	41(%a3),%d0		| dCtlSlotId
	bsr	say_dec
	bsr	crlf
	bra	next_sr
sr_done:
	bsr	crlf
	lea	s_mmu(%pc),%a0
	bsr	say_prefix
	moveq	#0,%d0
	move.b	MMU32Bit,%d0
	bsr	say_dec
	bsr	crlf
	lea	s_help(%pc),%a0
	bsr	say
loop:
	.short	_SystemTask
	clr.w	-(%sp)
	move.w	#0xFFFF,-(%sp)
	pea	event(%pc)
	.short	_GetNextEvent
	tst.w	(%sp)+
	beq.s	loop
	lea	event(%pc),%a0
	cmp.w	#keyDown,(%a0)
	beq.s	quit
	cmp.w	#mouseDown,(%a0)
	bne.s	loop
quit:
	.short	_ExitToShell

| ---- text ----
say:
	bsr.s	say_pstr_nocr
crlf:
	add.w	#LINEH,V_Y(%a6)
	move.w	#8,V_X(%a6)
	rts
say_prefix:
say_pstr_nocr:
	movem.l	%a0,-(%sp)
	move.w	V_X(%a6),-(%sp)
	move.w	V_Y(%a6),-(%sp)
	.short	_MoveTo
	movem.l	(%sp)+,%a0
	moveq	#0,%d0
	move.b	(%a0),%d0
	mulu	#6,%d0
	add.w	%d0,V_X(%a6)
	move.l	%a0,-(%sp)
	.short	_DrawString
	rts
say_sdec:				| d0 signed long -> " -123"
	lea	tmp(%pc),%a0
	move.b	#' ',1(%a0)
	move.b	#1,(%a0)
	move.l	%d0,-(%sp)
	bsr.s	say_pstr_nocr		| (clobbers d0)
	move.l	(%sp)+,%d0
	tst.l	%d0
	bpl.s	say_dec
	neg.l	%d0
	lea	tmp(%pc),%a0
	move.b	#'-',1(%a0)
	move.b	#1,(%a0)
	move.l	%d0,-(%sp)
	bsr.s	say_pstr_nocr
	move.l	(%sp)+,%d0
say_dec:				| d0 unsigned long -> decimal
	lea	tmp+20(%pc),%a0
	moveq	#0,%d1
1:	divull	#10,%d2:%d0
	add.b	#'0',%d2
	move.b	%d2,-(%a0)
	addq.w	#1,%d1
	tst.l	%d0
	bne.s	1b
	move.b	%d1,-(%a0)
	bra	say_pstr_nocr
say_hex:				| d0 = value, d1 = digits - 1
	lea	tmp+1(%pc),%a0
	move.w	%d1,%d2
	addq.w	#1,%d2
	move.b	%d2,-1(%a0)
	lea	(%a0,%d2.w),%a0
1:	move.b	%d0,%d2
	and.b	#15,%d2
	add.b	#'0',%d2
	cmp.b	#'9',%d2
	bls.s	2f
	addq.b	#7,%d2
2:	move.b	%d2,-(%a0)
	lsr.l	#4,%d0
	dbf	%d1,1b
	lea	tmp(%pc),%a0
	bra	say_pstr_nocr

	.even
	.equ	V_Y,		0
	.equ	V_X,		2
vars:	.space	8
wrect:	.short	40, 20, 300, 620
wtitle:	.byte	25
	.ascii	"PiScreens - the GDevices"
	.even
s_title: .byte 44
	.ascii	"QuickDraw's screens (the GDevice list), now:"
	.even
s_head:  .byte 46
	.ascii	"#  top left bottom right  flags depth base..."
	.even
s_sep:   .byte 1
	.ascii	":"
	.even
s_flags: .byte 7
	.ascii	" flags "
	.even
s_depth: .byte 7
	.ascii	" depth "
	.even
s_base:  .byte 6
	.ascii	" base "
	.even
s_rowbytes: .byte 4
	.ascii	" rb "
	.even
s_ref:   .byte 5
	.ascii	" ref "
	.even
s_main:  .byte 7
	.ascii	" (main)"
	.even
s_srsrc: .byte 46
	.ascii	"Video sResources (sNextTypeSRsrc order):      "
	.even
s_slot:  .byte 5
	.ascii	"slot "
	.even
s_id:    .byte 4
	.ascii	" id "
	.even
s_hw:    .byte 4
	.ascii	" hw "
	.even
s_devbase: .byte 9
	.ascii	" devbase "
	.even
s_dslot: .byte 6
	.ascii	" dctl "
	.even
spblock: .space 60
s_mmu:   .byte 30
	.ascii	"MMU32Bit (1 = 32-bit mode):   "
	.even
s_help:  .byte 23
	.ascii	"Click or a key to quit."
	.even
tmp:	.space 96
event:	.space 16
app_end:
