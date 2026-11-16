| pistatus.s — "PiStatus": what the Pi is doing, from the Macintosh. MPL-2.0.
| GNU as MIT syntax, 68020, packaged by tools/mac/mkapp.py into a plain application.
|
| The kernel's service core writes a read-only status page once a second (Alice68's
| alice_status.h, at slot-$9 window + 0x50000 on the Mac): the build string, uptime, how busy
| each ARM core is and what it is for, the SoC temperature, the JIT's translation cache, and the
| network units' counters. On the Amiga, PrismMonitor and Alice68-Status read the same page;
| this is the Macintosh reader. It redraws once a second, as long as the page's seq moves.
|
| Reading rule (alice_status.h): seq is odd while the kernel writes; read seq, the fields, then
| seq again, and try again if they differ. Every field is a big-endian 32-bit word, so the 68k
| reads them as they are; 0xFFFFFFFF means "not measured on this machine".
|
| Keys: R redraw, Q quit.

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
	.equ	_EraseRect,	0xA8A3
	.equ	_TextFont,	0xA887
	.equ	_TextSize,	0xA88A
	.equ	_GetNextEvent,	0xA970
	.equ	_SystemTask,	0xA9B4
	.equ	_ExitToShell,	0xA9F4
	.equ	Ticks,		0x016A
	.equ	MMU32Bit,	0x0CB2
	.equ	keyDown,	3
	.equ	autoKey,	5
	.equ	portRect,	16

| ---- the status page (alice_status.h) ----
	.equ	STATUS_OFF,	0x50000
	.equ	ST_MAGIC,	0x41363853	| 'A68S'
	.equ	S_MAGIC,	0x000
	.equ	S_SEQ,		0x00C
	.equ	S_UPTIME,	0x010
	.equ	S_BUILD,	0x020		| 96 bytes
	.equ	S_CORE,		0x080		| 4 x { role, busy (hundredths of a percent), name[24] } = 32 bytes each
	.equ	CORE_SIZE,	32
	.equ	C_BUSY,		4
	.equ	C_NAME,		8
	.equ	S_TEMP,		0x100		| thousandths of a degree
	.equ	S_ARMHZ,	0x104
	.equ	S_JIT_TOTAL,	0x120
	.equ	S_JIT_FREE,	0x124
	.equ	S_JIT_UNITS,	0x128
	.equ	S_JIT_TRANS,	0x12C
	.equ	S_NET,		0x140		| 2 x { kind, link, rx_pkts, tx_pkts, rx_bytes, tx_bytes, rx_drop, tx_err } = 32 bytes
	.equ	NET_SIZE,	32
	.equ	N_KIND,		0
	.equ	N_LINK,		4
	.equ	N_RX_PKTS,	8
	.equ	N_TX_PKTS,	12
	.equ	N_RX_DROP,	24
	.equ	N_TX_ERR,	28
	.equ	UNKNOWN,	0xFFFFFFFF
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
	move.l	(%sp)+,%d7
	move.l	%d7,-(%sp)
	.short	_SetPort
	move.w	#4,-(%sp)
	.short	_TextFont
	move.w	#9,-(%sp)
	.short	_TextSize
	lea	vars(%pc),%a6
	bsr	redraw

loop:
	.short	_SystemTask
	clr.w	-(%sp)
	move.w	#0x002A,-(%sp)
	pea	event(%pc)
	.short	_GetNextEvent
	tst.w	(%sp)+
	bne.s	have_event
	bsr	page
	move.l	%a1,%d0
	beq.s	loop
	move.l	S_SEQ(%a1),%d0
	cmp.l	V_SEQ(%a6),%d0
	beq.s	loop
	btst	#0,%d0			| odd: being written
	bne.s	loop
	bsr	redraw
	bra.s	loop
have_event:
	lea	event(%pc),%a0
	cmp.w	#keyDown,(%a0)
	beq.s	1f
	cmp.w	#autoKey,(%a0)
	bne.s	loop
1:	move.b	5(%a0),%d0
	or.b	#0x20,%d0
	cmp.b	#'q',%d0
	beq.s	quit
	bsr	redraw
	bra.s	loop
quit:
	.short	_ExitToShell

| ---- the display ----
redraw:
	movem.l	%d0-%d4/%a0-%a3,-(%sp)
	move.l	%d7,%a0
	pea	portRect(%a0)
	.short	_EraseRect
	move.w	#14,V_Y(%a6)
	move.w	#8,V_X(%a6)
	lea	s_title(%pc),%a0
	bsr	say
	bsr	page
	move.l	%a1,%d0
	bne.s	1f
	lea	s_nopage(%pc),%a0
	bsr	say
	bra	redraw_end
1:	move.l	%a1,%a3
	move.l	S_SEQ(%a3),V_SEQ(%a6)
	lea	S_BUILD(%a3),%a1
	bsr	say_cstr
	lea	s_uptime(%pc),%a0
	bsr	say_prefix
	move.l	S_UPTIME(%a3),%d0
	divu	#1000,%d0		| whole seconds (fits a word below 65536 s; a day needs the long form)
	and.l	#0xFFFF,%d0
	bsr	say_dec
	lea	s_seconds(%pc),%a0
	bsr	say_prefix
	lea	s_temp(%pc),%a0
	bsr	say_prefix
	move.l	S_TEMP(%a3),%d0
	cmp.l	#UNKNOWN,%d0
	beq.s	2f
	divu	#1000,%d0
	and.l	#0xFFFF,%d0
	bsr	say_dec
	lea	s_degrees(%pc),%a0
	bsr	say
	bra.s	3f
2:	lea	s_unknown(%pc),%a0
	bsr	say
3:	| the four cores
	bsr	crlf
	lea	s_coreshead(%pc),%a0
	bsr	say
	lea	S_CORE(%a3),%a2
	moveq	#0,%d4
4:	move.w	#8,V_X(%a6)
	move.l	%d4,%d0
	bsr	say_dec
	move.w	#8+3*6,V_X(%a6)
	lea	C_NAME(%a2),%a1
	bsr	say_cstr_nocr
	move.w	#8+30*6,V_X(%a6)
	move.l	C_BUSY(%a2),%d0
	cmp.l	#UNKNOWN,%d0
	beq.s	5f
	divu	#100,%d0		| whole percent
	and.l	#0xFFFF,%d0
	bsr	say_dec
	lea	s_percent(%pc),%a0
	bsr	say_prefix
	bra.s	6f
5:	lea	s_unknown(%pc),%a0
	bsr	say_prefix
6:	bsr	crlf
	lea	CORE_SIZE(%a2),%a2
	addq.l	#1,%d4
	cmp.l	#4,%d4
	blo.s	4b
	| the JIT
	bsr	crlf
	lea	s_jit(%pc),%a0
	bsr	say_prefix
	move.l	S_JIT_UNITS(%a3),%d0
	bsr	say_dec
	lea	s_blocks(%pc),%a0
	bsr	say_prefix
	move.l	S_JIT_TRANS(%a3),%d0
	bsr	say_dec
	lea	s_translated(%pc),%a0
	bsr	say_prefix
	move.l	S_JIT_FREE(%a3),%d0
	lsr.l	#8,%d0
	lsr.l	#2,%d0			| KB
	bsr	say_dec
	lea	s_kbfree(%pc),%a0
	bsr	say
	| the network units
	bsr	crlf
	lea	s_nethead(%pc),%a0
	bsr	say
	lea	S_NET(%a3),%a2
	moveq	#0,%d4
7:	move.w	#8,V_X(%a6)
	lea	s_unitnames(%pc),%a0
	move.l	N_KIND(%a2),%d0
	cmp.l	#2,%d0
	bls.s	8f
	moveq	#0,%d0
8:	mulu	#10,%d0
	add.l	%d0,%a0
	bsr	say_prefix
	move.w	#8+12*6,V_X(%a6)
	lea	s_down(%pc),%a0
	tst.l	N_LINK(%a2)
	beq.s	9f
	lea	s_up(%pc),%a0
9:	bsr	say_prefix
	move.w	#8+18*6,V_X(%a6)
	move.l	N_RX_PKTS(%a2),%d0
	bsr	say_dec
	move.w	#8+30*6,V_X(%a6)
	move.l	N_TX_PKTS(%a2),%d0
	bsr	say_dec
	move.w	#8+42*6,V_X(%a6)
	move.l	N_RX_DROP(%a2),%d0
	bsr	say_dec
	move.w	#8+50*6,V_X(%a6)
	move.l	N_TX_ERR(%a2),%d0
	bsr	say_dec
	bsr	crlf
	lea	NET_SIZE(%a2),%a2
	addq.l	#1,%d4
	cmp.l	#2,%d4
	blo	7b
redraw_end:
	move.w	#230,V_Y(%a6)
	move.w	#8,V_X(%a6)
	lea	s_help(%pc),%a0
	bsr	say
	movem.l	(%sp)+,%d0-%d4/%a0-%a3
	rts

| ---- the page ----
window:
	lea	0xF9000000,%a0
	tst.b	MMU32Bit
	bne.s	1f
	lea	0x00900000,%a0
1:	rts
page:					| a1 = the status page, or 0 without a Pi
	bsr	window
	cmp.l	#0x50534D42,(%a0)
	bne.s	1f
	lea	STATUS_OFF(%a0),%a1
	cmp.l	#ST_MAGIC,S_MAGIC(%a1)
	bne.s	1f
	rts
1:	suba.l	%a1,%a1
	rts

| ---- text output (a6 -> vars) ----
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
	moveq	#0,%d0			| advance the pen before the trap: QuickDraw does not preserve A0
	move.b	(%a0),%d0
	mulu	#6,%d0			| Monaco 9: 6 pixels per character
	add.w	%d0,V_X(%a6)
	move.l	%a0,-(%sp)
	.short	_DrawString
	rts
say_cstr:
	bsr.s	say_cstr_nocr
	bra.s	crlf
say_cstr_nocr:
	lea	tmp(%pc),%a0
	moveq	#0,%d0
1:	move.b	(%a1)+,%d1
	beq.s	2f
	addq.w	#1,%d0
	move.b	%d1,(%a0,%d0.w)
	cmp.w	#70,%d0
	blt.s	1b
2:	move.b	%d0,(%a0)
	bra.s	say_pstr_nocr
say_dec:				| d0 unsigned long -> decimal
	lea	tmp+20(%pc),%a0
	moveq	#0,%d1
1:	divull	#10,%d2:%d0		| 32-bit form: quotient d0, remainder d2
	add.b	#'0',%d2
	move.b	%d2,-(%a0)
	addq.w	#1,%d1
	tst.l	%d0
	bne.s	1b
	move.b	%d1,-(%a0)
	bra	say_pstr_nocr

| ---- data ----
	.even
	.equ	V_Y,		0
	.equ	V_X,		2
	.equ	V_SEQ,		4
vars:	.space	16
wrect:	.short	50, 30, 300, 500
wtitle:	.byte	23
	.ascii	"PiStatus - the Pi's state"
	.even
s_title:    .byte 24
	.ascii "The PiStorm's status    "
	.even
s_nopage:   .byte 40
	.ascii "No Pi at slot $9, or no status page.    "
	.even
s_uptime:   .byte 8
	.ascii "Uptime: "
	.even
s_seconds:  .byte 4
	.ascii " s  "
	.even
s_temp:     .byte 13
	.ascii "Temperature: "
	.even
s_degrees:  .byte 2
	.ascii " C"
	.even
s_unknown:  .byte 12
	.ascii "not measured"
	.even
s_coreshead: .byte 36
	.ascii "#  ARM core                   busy  "
	.even
s_percent:  .byte 1
	.ascii "%"
	.even
s_jit:      .byte 5
	.ascii "JIT: "
	.even
s_blocks:   .byte 8
	.ascii " blocks,"
	.even
s_translated: .byte 12
	.ascii " translated,"
	.even
s_kbfree:   .byte 8
	.ascii " KB free"
	.even
s_nethead:  .byte 54
	.ascii "Unit        link  received    sent        dropped errors"
	.even
s_unitnames:				| 10 bytes each
	.byte 4
	.ascii "none     "
	.byte 4
	.ascii "WiFi     "
	.byte 8
	.ascii "Ethernet "
s_up:       .byte 2
	.ascii "up"
	.even
s_down:     .byte 4
	.ascii "down"
	.even
s_help:     .byte 36
	.ascii "R redraw   Q quit   (updates itself)"
	.even
tmp:	.space 96
event:	.space 16
app_end:
