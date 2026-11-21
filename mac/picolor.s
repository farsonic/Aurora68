| picolor.s — "PiColor": colour on the Pi's screen. MPL-2.0. GNU as MIT syntax, 68020.
|
| The second display (docs/10) is a 256-colour screen as far as QuickDraw is concerned; this
| program is the first thing to put colour on it. It opens a colour window (NewCWindow, so the
| port carries RGB colours; a NewWindow port would reduce everything to black and white) placed
| on the second screen, at global (40,660)-(440,1260), which is inside the Pi's rectangle
| (0,640)-(480,1280) that the 'scrn' resource gives it, and paints: twelve hues at full
| strength, the same at half strength, the primaries and secondaries, and a sixteen-step grey
| ramp, each with RGBForeColor and PaintRect. QuickDraw maps each colour to the nearest entry of
| the screen's colour table (the ROM gave the card the standard 8-bit table at boot), the driver
| handed that table to the Pi, and the service core shows the result on HDMI; `tools/vcshot.py`
| photographs it in the testbed. Click or press a key to quit.
|
| Since 20 November 2026 the window goes on the first active screen that is not the main one,
| wherever Monitors or the 'scrn' put it (the HDMI screen right of the SE's 512 x 342 on the SE
| board, the card's right of the IIci's built-in video in the testbed); on a Mac with one screen it
| stays at the place above.

	.text
	.globl	app_start
	.equ	_InitGraf,	0xA86E
	.equ	_InitFonts,	0xA8FE
	.equ	_InitWindows,	0xA912
	.equ	_InitMenus,	0xA930
	.equ	_TEInit,	0xA9CC
	.equ	_InitDialogs,	0xA97B
	.equ	_InitCursor,	0xA850
	.equ	_NewCWindow,	0xAA45
	.equ	_SetPort,	0xA873
	.equ	_MoveTo,	0xA893
	.equ	_DrawString,	0xA884
	.equ	_TextFont,	0xA887
	.equ	_TextSize,	0xA88A
	.equ	_TextFace,	0xA888
	.equ	_RGBForeColor,	0xAA14
	.equ	_PaintRect,	0xA8A2
	.equ	_FrameRect,	0xA8A1
	.equ	_SetRect,	0xA8A7
	.equ	_GetNextEvent,	0xA970
	.equ	_SystemTask,	0xA9B4
	.equ	_ExitToShell,	0xA9F4
	.equ	keyDown,	3
	.equ	mouseDown,	1

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
	bsr	place_window
	clr.l	-(%sp)			| result
	clr.l	-(%sp)			| wStorage
	pea	wrect(%pc)
	pea	wtitle(%pc)
	st	-(%sp)			| visible
	move.w	#0,-(%sp)		| documentProc
	move.l	#-1,-(%sp)		| in front
	sf	-(%sp)			| no goAway
	clr.l	-(%sp)			| refCon
	.short	_NewCWindow
	move.l	(%sp)+,%d7		| the window
	move.l	%d7,-(%sp)
	.short	_SetPort
	move.w	#0,-(%sp)		| Chicago
	.short	_TextFont
	move.w	#12,-(%sp)
	.short	_TextSize
	bsr	paint
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
	beq.s	quit
	cmp.w	#6,(%a0)		| updateEvt: paint again
	bne.s	loop
	bsr	paint
	bra.s	loop
quit:
	.short	_ExitToShell

| ---- the picture (in the window's local coordinates, 600 x 400) ----
paint:
	movem.l	%d0-%d7/%a0-%a4,-(%sp)
	bsr	black
	move.w	#20,-(%sp)
	move.w	#24,-(%sp)
	.short	_MoveTo
	pea	s_title(%pc)
	.short	_DrawString
	| row 1: twelve hues, full strength; row 2: the same at half strength
	lea	hues(%pc),%a3
	moveq	#0,%d6			| column
1:	move.l	%a3,%a0
	bsr	rgb_full
	move.w	#40,%d2			| top
	move.w	#110,%d3		| bottom
	bsr	cell
	move.l	%a3,%a0
	bsr	rgb_half
	move.w	#116,%d2
	move.w	#186,%d3
	bsr	cell
	addq.l	#6,%a3
	addq.w	#1,%d6
	cmp.w	#12,%d6
	blo.s	1b
	| row 3: eight primaries, secondaries, black and white
	lea	prims(%pc),%a3
	moveq	#0,%d6
2:	move.l	%a3,%a0
	bsr	rgb_full
	move.w	#196,%d2
	move.w	#266,%d3
	bsr	cell8
	addq.l	#6,%a3
	addq.w	#1,%d6
	cmp.w	#8,%d6
	blo.s	2b
	| row 4: a sixteen-step grey ramp
	moveq	#0,%d6
3:	move.w	%d6,%d0
	mulu	#0x1111,%d0		| 0, 0x1111 ... 0xFFFF
	lea	color(%pc),%a0
	move.w	%d0,(%a0)
	move.w	%d0,2(%a0)
	move.w	%d0,4(%a0)
	pea	color(%pc)
	.short	_RGBForeColor
	move.w	#276,%d2
	move.w	#346,%d3
	bsr	cell16
	addq.w	#1,%d6
	cmp.w	#16,%d6
	blo.s	3b
	bsr	black
	move.w	#20,-(%sp)
	move.w	#372,-(%sp)
	.short	_MoveTo
	pea	s_foot(%pc)
	.short	_DrawString
	movem.l	(%sp)+,%d0-%d7/%a0-%a4
	rts

| rgb_full / rgb_half: a0 -> three 16-bit components -> RGBForeColor (half: each halved)
rgb_full:
	lea	color(%pc),%a1
	move.w	(%a0)+,(%a1)+
	move.w	(%a0)+,(%a1)+
	move.w	(%a0)+,(%a1)+
	pea	color(%pc)
	.short	_RGBForeColor
	rts
rgb_half:
	lea	color(%pc),%a1
	moveq	#2,%d0
1:	move.w	(%a0)+,%d1
	lsr.w	#1,%d1
	move.w	%d1,(%a1)+
	dbf	%d0,1b
	pea	color(%pc)
	.short	_RGBForeColor
	rts
black:
	lea	color(%pc),%a0
	clr.w	(%a0)
	clr.w	2(%a0)
	clr.w	4(%a0)
	pea	color(%pc)
	.short	_RGBForeColor
	rts
| cell: column d6 of twelve (48 px each from x 10), rows d2..d3, painted; cell8: of eight (72 px);
| cell16: of sixteen (36 px)
cell:
	move.w	%d6,%d0
	mulu	#48,%d0
	add.w	#10,%d0
	move.w	%d0,%d1
	add.w	#46,%d1
	bra.s	paint_cell
cell8:
	move.w	%d6,%d0
	mulu	#72,%d0
	add.w	#10,%d0
	move.w	%d0,%d1
	add.w	#70,%d1
	bra.s	paint_cell
cell16:
	move.w	%d6,%d0
	mulu	#36,%d0
	add.w	#10,%d0
	move.w	%d0,%d1
	add.w	#34,%d1
| place_window: wrect on the first active screen that is not the main one, 20 pixels in from its
| left and 40 down from its top. QuickDraw's screens are a list of GDevices from DeviceList ($8A8):
| gdFlags at 20 (bit 15 screenActive, bit 11 mainScreen), gdNextGD at 30, gdRect at 34.
place_window:
	move.l	0x8A8,%d0		| DeviceList: the first device's handle
	moveq	#7,%d2			| eight devices at most
1:	tst.l	%d0
	beq.s	9f
	move.l	%d0,%a0
	move.l	(%a0),%a0		| the GDevice
	move.w	20(%a0),%d1
	btst	#15,%d1
	beq.s	2f			| not active
	btst	#11,%d1
	bne.s	2f			| the main screen
	lea	wrect(%pc),%a1
	move.w	34(%a0),%d1		| its top
	add.w	#40,%d1
	move.w	%d1,(%a1)
	add.w	#400,%d1
	move.w	%d1,4(%a1)
	move.w	36(%a0),%d1		| its left
	add.w	#20,%d1
	move.w	%d1,2(%a1)
	add.w	#600,%d1
	move.w	%d1,6(%a1)
	rts
2:	move.l	30(%a0),%d0		| gdNextGD
	dbf	%d2,1b
9:	rts

paint_cell:				| d0 left, d1 right, d2 top, d3 bottom
	pea	rect(%pc)
	move.w	%d0,-(%sp)		| SetRect takes left, top, right, bottom (not a Rect's order:
	move.w	%d2,-(%sp)		| the first version pushed top, left, bottom, right and painted
	move.w	%d1,-(%sp)		| the picture transposed)
	move.w	%d3,-(%sp)
	.short	_SetRect
	pea	rect(%pc)
	.short	_PaintRect
	rts

	.even
wrect:	.short	40, 660, 440, 1260	| on the Pi's screen: (0,640)-(480,1280) in the 'scrn'
wtitle:	.byte	15			| the window's title: the program's name (since 21 November 2026)
	.ascii	"Aurora68 Colour"
	.even
s_title: .byte 52
	.ascii	"Twelve hues, half strength, primaries, sixteen greys"
	.even
s_foot:  .byte 50
	.ascii	"Drawn by Color QuickDraw into the Pi's frame buffer"
	.even
color:	.short	0, 0, 0
rect:	.short	0, 0, 0, 0
hues:	.short	0xFFFF, 0x0000, 0x0000		| red
	.short	0xFFFF, 0x8000, 0x0000		| orange
	.short	0xFFFF, 0xFFFF, 0x0000		| yellow
	.short	0x8000, 0xFFFF, 0x0000		| lime
	.short	0x0000, 0xFFFF, 0x0000		| green
	.short	0x0000, 0xFFFF, 0x8000		| spring
	.short	0x0000, 0xFFFF, 0xFFFF		| cyan
	.short	0x0000, 0x8000, 0xFFFF		| azure
	.short	0x0000, 0x0000, 0xFFFF		| blue
	.short	0x8000, 0x0000, 0xFFFF		| violet
	.short	0xFFFF, 0x0000, 0xFFFF		| magenta
	.short	0xFFFF, 0x0000, 0x8000		| rose
prims:	.short	0xFFFF, 0x0000, 0x0000		| red
	.short	0x0000, 0xFFFF, 0x0000		| green
	.short	0x0000, 0x0000, 0xFFFF		| blue
	.short	0x0000, 0xFFFF, 0xFFFF		| cyan
	.short	0xFFFF, 0x0000, 0xFFFF		| magenta
	.short	0xFFFF, 0xFFFF, 0x0000		| yellow
	.short	0x0000, 0x0000, 0x0000		| black
	.short	0xFFFF, 0xFFFF, 0xFFFF		| white
event:	.space	16
app_end:
