| SPDX-License-Identifier: MPL-2.0
| fontwidth.s - "FontWidth": Times 11 measured and drawn, for comparing launches. GNU as MIT syntax,
| 68020, packaged by tools/mac/mkapp.py (make -C mac FontWidth; a test, not part of `all`).
|
| Backstory (23 November 2026, docs/08 item 134). Octavo drew Times 11 with the wrong letters ("tex1"
| for "text") on the first launch after a new build had been copied in, and QuickDraw's Times 11
| widths differed between launches. Times has no 11-point bitmap in System 7.1, so QuickDraw draws it
| from the TrueType outline. The suspects were the JIT running an old translation of code that had
| been replaced, and the heap. This program asks the same questions as Octavo with nothing of
| Octavo's in between, so that launches (after copying a new application over an old one, or just
| one after another) can be compared line by line in the kernel's log:
|   [MAC] FontWidth: Times is font N; RealFont 11 R, 12 R
|   [MAC] FontWidth: Times 11 CharWidth 32-79 <48 hex digits>       (each width in pixels, one digit;
|   [MAC] FontWidth: Times 11 CharWidth 80-126 <47 hex digits>       '+' for 16 or more)
|   [MAC] FontWidth: Times 11 TextWidth "text" W, "Hamburgefonstiv" W
|   [MAC] FontWidth: Times 11 "text Times" drawn, checksum XXXXXXXX
|   [MAC] FontWidth: |....##...|                                    (the drawing, rows 4-19, 100 pixels)
| and the same for Times 12 (a bitmap size) as the control. Then it quits: put it in Startup Items or
| open it with `tools/aurora68.py open`, and read the log.

	.text
	.globl	app_start

	.equ	_InitGraf,	0xA86E
	.equ	_InitFonts,	0xA8FE
	.equ	_InitWindows,	0xA912
	.equ	_InitCursor,	0xA850
	.equ	_OpenPort,	0xA86F
	.equ	_SetPortBits,	0xA875
	.equ	_PortSize,	0xA876
	.equ	_ClipRect,	0xA87B
	.equ	_RectRgn,	0xA8DF
	.equ	_EraseRect,	0xA8A3
	.equ	_MoveTo,	0xA893
	.equ	_DrawString,	0xA884
	.equ	_TextFont,	0xA887
	.equ	_TextFace,	0xA888
	.equ	_TextSize,	0xA88A
	.equ	_TextWidth,	0xA886
	.equ	_CharWidth,	0xA88D
	.equ	_GetFNum,	0xA900
	.equ	_RealFont,	0xA902
	.equ	_NewPtrClear,	0xA31E
	.equ	_ExitToShell,	0xA9F4
	.equ	MMU32Bit,	0x0CB2

	.equ	ROWB,	16		| the bitmap: 128 x 24 pixels, one bit
	.equ	ROWS,	24

| ---- log through the Pi's INFO service: `log "text"` (a C string), `logbuf` (a1 = C string) ----
	.macro	log text
	lea	9f(%pc),%a1
	bsr	do_log
	bra.s	8f
9:	.ascii	"\text"
	.byte	0
	.even
8:
	.endm

| registers: a2 = the port, a3 = the bitmap's bits, a4 = the line buffer, d7 = the font number
app_start:
	lea	-4(%a5),%a0
	move.l	%a0,-(%sp)
	.short	_InitGraf
	.short	_InitFonts
	.short	_InitWindows
	.short	_InitCursor
	log	"FontWidth: start"
	move.l	#108+ROWB*ROWS+256,%d0	| a GrafPort, the bits, a line buffer
	.short	_NewPtrClear
	move.l	%a0,%d0
	beq	quit
	move.l	%a0,%a2
	lea	108(%a2),%a3
	lea	ROWB*ROWS(%a3),%a4
	move.l	%a2,-(%sp)
	.short	_OpenPort
	lea	bitmap(%pc),%a0
	move.l	%a3,(%a0)
	move.l	%a0,-(%sp)
	.short	_SetPortBits
	move.w	#ROWB*8,-(%sp)
	move.w	#ROWS,-(%sp)
	.short	_PortSize
	pea	brect(%pc)
	.short	_ClipRect
	move.l	24(%a2),-(%sp)		| visRgn
	pea	brect(%pc)
	.short	_RectRgn

	pea	times(%pc)
	pea	fnum(%pc)
	.short	_GetFNum
	move.w	fnum(%pc),%d7
	| "FontWidth: Times is font N; RealFont 11 R, 12 R"
	move.l	%a4,%a0
	lea	s_font(%pc),%a1
	bsr	cat
	move.w	%d7,%d0
	bsr	dec
	lea	s_real(%pc),%a1
	bsr	cat
	moveq	#11,%d0
	bsr	realfont
	lea	s_comma12(%pc),%a1
	bsr	cat
	moveq	#12,%d0
	bsr	realfont
	clr.b	(%a0)
	move.l	%a4,%a1
	bsr	do_log

	moveq	#11,%d6
	bsr	measure
	moveq	#12,%d6
	bsr	measure
	log	"FontWidth: done"
quit:	.short	_ExitToShell

| realfont: d0 = size -> "1" or "0" at a0
realfont:
	move.l	%a0,-(%sp)
	clr.w	-(%sp)
	move.w	%d7,-(%sp)
	move.w	%d0,-(%sp)
	.short	_RealFont
	move.b	(%sp)+,%d0
	addq.l	#1,%sp
	move.l	(%sp)+,%a0
	tst.b	%d0
	sne	%d0
	and.b	#1,%d0
	add.b	#'0',%d0
	move.b	%d0,(%a0)+
	rts

| measure: d6 = the size. Widths, text widths, the drawing and its checksum, all logged.
measure:
	move.w	%d7,-(%sp)
	.short	_TextFont
	clr.w	-(%sp)
	.short	_TextFace
	move.w	%d6,-(%sp)
	.short	_TextSize
	| the widths of 32-126
	move.l	%a4,%a0
	bsr	head
	lea	s_cw(%pc),%a1
	bsr	cat
	moveq	#32,%d5
1:	move.l	%a0,-(%sp)
	clr.w	-(%sp)
	move.w	%d5,-(%sp)
	.short	_CharWidth
	move.w	(%sp)+,%d0
	move.l	(%sp)+,%a0
	bsr	hexdigit
	addq.w	#1,%d5
	cmp.w	#80,%d5			| the INFO service takes 127 characters a line: two lines
	bne.s	6f
	clr.b	(%a0)
	move.l	%a4,%a1
	bsr	do_log
	move.l	%a4,%a0
	bsr	head
	lea	s_cw2(%pc),%a1
	bsr	cat
6:	cmp.w	#127,%d5
	bcs.s	1b
	clr.b	(%a0)
	move.l	%a4,%a1
	bsr	do_log
	| TextWidth of two words
	move.l	%a4,%a0
	bsr	head
	lea	s_tw(%pc),%a1
	bsr	cat
	lea	w_text(%pc),%a1
	bsr	textwidth
	bsr	dec
	lea	s_tw2(%pc),%a1
	bsr	cat
	lea	w_hamb(%pc),%a1
	bsr	textwidth
	bsr	dec
	clr.b	(%a0)
	move.l	%a4,%a1
	bsr	do_log
	| the drawing
	pea	brect(%pc)
	.short	_EraseRect
	move.w	#2,-(%sp)
	move.w	#16,-(%sp)
	.short	_MoveTo
	pea	p_draw(%pc)
	.short	_DrawString
	move.l	%a3,%a0			| checksum: rotate and add every long
	moveq	#0,%d0
	move.w	#ROWB*ROWS/4-1,%d1
2:	rol.l	#5,%d0
	add.l	(%a0)+,%d0
	dbf	%d1,2b
	move.l	%d0,%d4
	move.l	%a4,%a0
	bsr	head
	lea	s_drawn(%pc),%a1
	bsr	cat
	move.l	%d4,%d0
	bsr	hex8
	clr.b	(%a0)
	move.l	%a4,%a1
	bsr	do_log
	| rows 4-19 as text, the first 100 pixels
	moveq	#4,%d5
3:	move.l	%a4,%a0
	lea	s_row(%pc),%a1
	bsr	cat
	move.w	%d5,%d0
	mulu.w	#ROWB,%d0
	lea	(%a3,%d0.l),%a1
	moveq	#0,%d1
4:	bfextu	(%a1){%d1:1},%d0
	move.b	#'.',%d2
	tst.l	%d0
	beq.s	5f
	move.b	#'#',%d2
5:	move.b	%d2,(%a0)+
	addq.w	#1,%d1
	cmp.w	#100,%d1
	bcs.s	4b
	move.b	#'|',(%a0)+
	clr.b	(%a0)
	move.l	%a4,%a1
	bsr	do_log
	addq.w	#1,%d5
	cmp.w	#20,%d5
	bcs.s	3b
	rts

| textwidth: a1 = Pascal string -> d0 its width in the port's font (a0 kept)
textwidth:
	move.l	%a0,-(%sp)
	moveq	#0,%d0
	move.b	(%a1)+,%d0
	clr.w	-(%sp)
	move.l	%a1,-(%sp)
	clr.w	-(%sp)
	move.w	%d0,-(%sp)
	.short	_TextWidth
	move.w	(%sp)+,%d0
	move.l	(%sp)+,%a0
	ext.l	%d0
	rts

| head: "FontWidth: Times NN " at a0
head:	lea	s_head(%pc),%a1
	bsr	cat
	move.w	%d6,%d0
	bsr	dec
	move.b	#' ',(%a0)+
	rts

| cat: C string a1 appended at a0
cat:	move.b	(%a1)+,(%a0)+
	bne.s	cat
	subq.l	#1,%a0
	rts
| dec: d0.w (0-65535) in decimal at a0
dec:	movem.l	%d1-%d2,-(%sp)
	and.l	#0xFFFF,%d0
	moveq	#0,%d2
1:	divu.w	#10,%d0
	swap	%d0
	move.w	%d0,-(%sp)
	clr.w	%d0
	swap	%d0
	addq.w	#1,%d2
	tst.w	%d0
	bne.s	1b
	subq.w	#1,%d2
2:	move.w	(%sp)+,%d1
	add.b	#'0',%d1
	move.b	%d1,(%a0)+
	dbf	%d2,2b
	movem.l	(%sp)+,%d1-%d2
	rts
| hexdigit: d0 (0-15; more is '+') at a0
hexdigit:
	cmp.w	#15,%d0
	bls.s	1f
	move.b	#'+',(%a0)+
	rts
1:	add.b	#'0',%d0
	cmp.b	#'9',%d0
	bls.s	2f
	addq.b	#7,%d0
2:	move.b	%d0,(%a0)+
	rts
| hex8: d0 in eight hex digits at a0
hex8:	move.l	%d3,-(%sp)
	move.l	%d0,%d3
	moveq	#7,%d1
1:	rol.l	#4,%d3
	move.l	%d3,%d0
	and.w	#15,%d0
	move.l	%d1,-(%sp)
	bsr	hexdigit
	move.l	(%sp)+,%d1
	dbf	%d1,1b
	move.l	(%sp)+,%d3
	rts

| ---- the INFO service ----
window:
	lea	0xF9000000,%a0
	tst.b	MMU32Bit
	bne.s	1f
	lea	0x00900000,%a0
1:	rts
| do_log: a1 = C string -> INFO command (block 0): cmd 2, buf, len
do_log:
	movem.l	%d0-%d1/%a0-%a1,-(%sp)
	bsr	window
	lea	0x1000(%a0),%a0
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
	move.l	#50000000,%d1		| the Pi reads the text when it gets to the command: wait for it
4:	cmp.l	4(%a0),%d0
	beq.s	5f
	subq.l	#1,%d1
	bne.s	4b
5:	movem.l	(%sp)+,%d0-%d1/%a0-%a1
	rts

| ---- data ----
	.even
bitmap:	.long	0			| baseAddr
	.short	ROWB			| rowBytes
brect:	.short	0, 0, ROWS, ROWB*8	| bounds (top, left, bottom, right), also the port's rectangle
fnum:	.short	0
times:	.byte	5
	.ascii	"Times"
w_text:	.byte	4
	.ascii	"text"
w_hamb:	.byte	15
	.ascii	"Hamburgefonstiv"
p_draw:	.byte	10
	.ascii	"text Times"
s_font:	.asciz	"FontWidth: Times is font "
s_real:	.asciz	"; RealFont 11 "
s_comma12: .asciz ", 12 "
s_head:	.asciz	"FontWidth: Times "
s_cw:	.asciz	"CharWidth 32-79 "
s_cw2:	.asciz	"CharWidth 80-126 "
s_tw:	.asciz	"TextWidth \"text\" "
s_tw2:	.asciz	", \"Hamburgefonstiv\" "
s_drawn: .asciz	"\"text Times\" drawn, checksum "
s_row:	.asciz	"FontWidth: |"
	.even
