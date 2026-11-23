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
|
| Since 23 November 2026 it goes only on a second screen with room for it (640 by 460), and otherwise
| on the main screen, below the menu bar, made smaller where the main screen is small: on the SE's
| own 512 by 342 as its only screen (no HDMI) the window had been put at (40,660), off every screen,
| and with the HDMI as the main screen it went on the SE's screen and ran off its bottom and right.
| The picture is drawn for 600 by 400 and scaled to the window (each rectangle and line by W/600 and
| H/400), so on the SE it is 504 by 298; in one bit QuickDraw shows each colour as black or white.

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
	move.w	#24,%d0
	bsr	scale_y
	move.w	%d0,-(%sp)
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
	move.w	#372,%d0
	bsr	scale_y
	move.w	%d0,-(%sp)
	.short	_MoveTo
	lea	s_foot(%pc),%a0
	move.w	wdepth(%pc),%d0
	cmp.w	#1,%d0
	bne.s	1f
	lea	s_onebit(%pc),%a0	| a one-bit screen (the SE's) shows every colour as black or white: say so
1:	move.l	%a0,-(%sp)
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
	bra	paint_cell
cell8:
	move.w	%d6,%d0
	mulu	#72,%d0
	add.w	#10,%d0
	move.w	%d0,%d1
	add.w	#70,%d1
	bra	paint_cell
cell16:
	move.w	%d6,%d0
	mulu	#36,%d0
	add.w	#10,%d0
	move.w	%d0,%d1
	add.w	#34,%d1
	bra	paint_cell		| (it fell through into paint_cell until place_window came between)

| place_window: wrect on the first active screen that is not the main one and has room for the
| 600 x 400 picture (640 x 460), 20 pixels in from its left and 40 down from its top; with none, on
| the main screen 40 down (below the menu bar), as large as it allows up to 600 x 400. QuickDraw's
| screens are a list of GDevices from DeviceList ($8A8): gdFlags at 20 (bit 15 screenActive, bit 11
| mainScreen), gdNextGD at 30, gdRect at 34. MainDevice ($8A4) is the main screen's handle. Then
| wsize = the window's width and height, which paint_cell scales the picture to.
place_window:
	movem.l	%d3-%d4/%a2,-(%sp)
	lea	wrect(%pc),%a1
	move.l	0x8A8,%d0		| DeviceList: the first device's handle
	moveq	#7,%d2			| eight devices at most
1:	tst.l	%d0
	beq.s	5f
	move.l	%d0,%a0
	move.l	(%a0),%a0		| the GDevice
	move.w	20(%a0),%d1
	btst	#15,%d1
	beq.s	2f			| not active
	btst	#11,%d1
	bne.s	2f			| the main screen
	move.w	40(%a0),%d1		| its bottom
	sub.w	34(%a0),%d1		| less its top
	cmp.w	#460,%d1
	blt.s	2f			| too small (the SE's screen beside a larger main HDMI screen)
	move.w	38(%a0),%d1		| its right
	sub.w	36(%a0),%d1
	cmp.w	#640,%d1
	blt.s	2f
	bsr	note_depth
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
	bra.s	8f
2:	move.l	30(%a0),%d0		| gdNextGD
	dbf	%d2,1b
5:	move.l	0x8A4,%d0		| MainDevice
	beq.s	8f			| (none: the place above)
	move.l	%d0,%a0
	move.l	(%a0),%a0
	bsr	note_depth
	lea	34(%a0),%a2		| its gdRect: top, left, bottom, right
	move.w	(%a2),%d3
	add.w	#40,%d3			| the top, below the menu bar and the title bar
	move.w	%d3,(%a1)
	move.w	4(%a2),%d4
	subq.w	#4,%d4			| the room down to the bottom edge
	sub.w	%d3,%d4
	cmp.w	#400,%d4
	ble.s	6f
	move.w	#400,%d4
6:	add.w	%d3,%d4
	move.w	%d4,4(%a1)		| bottom
	move.w	2(%a2),%d3
	add.w	#4,%d3			| left: 4 in from the edge on a small screen,
	move.w	6(%a2),%d4
	sub.w	2(%a2),%d4
	cmp.w	#640,%d4
	blt.s	7f
	add.w	#16,%d3			| 20 where the whole picture fits
7:	move.w	%d3,2(%a1)
	move.w	6(%a2),%d4
	subq.w	#4,%d4
	sub.w	%d3,%d4
	cmp.w	#600,%d4
	ble.s	71f
	move.w	#600,%d4
71:	add.w	%d3,%d4
	move.w	%d4,6(%a1)		| right
8:	lea	wsize(%pc),%a0
	move.w	6(%a1),%d0
	sub.w	2(%a1),%d0
	move.w	%d0,(%a0)		| W
	move.w	4(%a1),%d0
	sub.w	(%a1),%d0
	move.w	%d0,2(%a0)		| H
	movem.l	(%sp)+,%d3-%d4/%a2
	rts

| note_depth: a0 = the chosen screen's GDevice -> wdepth = its pixel size (gdPMap at 22, a PixMap
| handle; pmPixelSize at 32). Keeps a0 and d0-d2.
note_depth:
	movem.l	%a1-%a2,-(%sp)
	move.l	22(%a0),%a2
	move.l	(%a2),%a2
	lea	wdepth(%pc),%a1		| (PC-relative: the code runs wherever the Segment Loader put it)
	move.w	32(%a2),(%a1)
	movem.l	(%sp)+,%a1-%a2
	rts

| scale_y: d0 = a height in the 600 x 400 picture -> in the window
scale_y:
	lea	wsize(%pc),%a0
	muls.w	2(%a0),%d0
	divs.w	#400,%d0
	rts

paint_cell:				| d0 left, d1 right, d2 top, d3 bottom, in the 600 x 400 picture
	lea	wsize(%pc),%a0		| to the window's W x H
	muls.w	(%a0),%d0
	divs.w	#600,%d0
	muls.w	(%a0),%d1
	divs.w	#600,%d1
	muls.w	2(%a0),%d2
	divs.w	#400,%d2
	muls.w	2(%a0),%d3
	divs.w	#400,%d3
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
wsize:	.short	600, 400		| the window's width and height (place_window)
wdepth:	.short	8			| its screen's bits per pixel (place_window)
wtitle:	.byte	15			| the window's title: the program's name (since 21 November 2026)
	.ascii	"Aurora68 Colour"
	.even
s_title: .byte 52
	.ascii	"Twelve hues, half strength, primaries, sixteen greys"
	.even
s_foot:  .byte 51
	.ascii	"Drawn by Color QuickDraw into the Pi's frame buffer"
	.even
s_onebit: .byte 49
	.ascii	"One bit here: each colour shows as black or white"
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
