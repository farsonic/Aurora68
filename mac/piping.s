| piping.s — "PiPing": MacTCP over the Pi's Ethernet, from the Macintosh. MPL-2.0.
| GNU as MIT syntax, 68020, packaged by tools/mac/mkapp.py into a plain application.
|
| What it is. Once MacTCP (Apple's TCP/IP stack of 1989-1996, a driver named .IPP plus a control
| panel) is installed and pointed at the Pi's Ethernet card, every IP program of the era works:
| this program is the smallest possible proof. It opens the .IPP driver, asks it for the address
| the control panel configured (ipctlGetAddr, csCode 15), and sends ICMP echo requests, the
| "ping" of RFC 792, to the testbed's host peer (ipctlEchoICMP, csCode 17: MacTCP builds the
| packet, our .ENET driver carries it, the peer at 192.168.68.1 answers, MacTCP delivers the
| reply to a completion routine with the ticks the request left and the reply arrived). Three
| pings, round trips in ticks, then the usual "click or press a key".
|
| The parameter blocks are from the MacTCP Programmer's Guide (1993): the I/O header is the
| Device Manager's (csCode at 26), ipctlGetAddr returns ourAddress at 28 and ourNetMask at 32;
| the echo call takes dest at 28, a WDS entry (length.w, pointer.l) at 32, timeout at 38,
| options at 40, optLength at 44, the completion routine at 46 and a user pointer at 50; the
| completion routine receives an ICMPParamBlock whose icmpEchoInfo starts at 50: echoRequestOut,
| echoReplyIn, an RDS entry for the echoed data, options, the user pointer.

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
	.equ	_Open,		0xA000
	.equ	_Control,	0xA004
	.equ	_ControlAsync,	0xA404
	.equ	Ticks,		0x016A
	.equ	MMU32Bit,	0x0CB2
	.equ	ioCompletion,	12
	.equ	ioResult,	16
	.equ	ioNamePtr,	18
	.equ	ioRefNum,	24
	.equ	csCode,		26
	.equ	ipctlGetAddr,	15
	.equ	ipctlEchoICMP,	17
	.equ	PEER_IP,	0xC0A84401	| 192.168.68.1: tools/pinet.py's peer
	.equ	PINGS,		3
	.equ	LINEH,		14

| ---- log through the Pi's INFO service ----
	.macro	log text
	movem.l	%d0-%d1/%a0-%a1,-(%sp)
	lea	9f(%pc),%a1
	bsr	do_log
	bra.s	8f
9:	.ascii	"\text"
	.byte	0
	.even
8:	movem.l	(%sp)+,%d0-%d1/%a0-%a1
	.endm

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
	move.w	#16,V_Y(%a6)
	move.w	#8,V_X(%a6)
	lea	s_title(%pc),%a0
	bsr	say
	log	"PiPing: started"

	| --- open the MacTCP driver ---
	lea	pb(%pc),%a3
	bsr	pbclear
	lea	s_ipp(%pc),%a0
	move.l	%a0,ioNamePtr(%a3)
	move.l	%a3,%a0
	.short	_Open
	bsr	log_err
	tst.w	%d0
	beq.s	1f
	lea	s_noipp(%pc),%a0
	bsr	say
	bra	wait
1:	move.w	ioRefNum(%a3),%d6
	lea	s_open(%pc),%a0
	bsr	say

	| --- our address: ipctlGetAddr ---
	bsr	pbclear
	move.w	%d6,ioRefNum(%a3)
	move.w	#ipctlGetAddr,csCode(%a3)
	move.l	%a3,%a0
	.short	_Control
	bsr	log_err
	tst.w	%d0
	bne	ctlerr
	lea	s_addr(%pc),%a0
	bsr	say_prefix
	move.l	28(%a3),%d0		| ourAddress
	bsr	say_ip
	lea	s_mask(%pc),%a0
	bsr	say_prefix
	move.l	32(%a3),%d0		| ourNetMask
	bsr	say_ip
	bsr	crlf

	| --- three echo requests, one at a time ---
	moveq	#PINGS,%d5
ping_loop:
	bsr	pbclear
	move.w	%d6,ioRefNum(%a3)
	move.w	#ipctlEchoICMP,csCode(%a3)
	move.l	#PEER_IP,28(%a3)	| dest
	move.w	#32,32(%a3)		| data: 32 bytes
	lea	payload(%pc),%a0
	move.l	%a0,34(%a3)
	move.w	#5,38(%a3)		| timeout: seconds
	clr.l	40(%a3)			| no options
	clr.w	44(%a3)
	lea	echo_done(%pc),%a0
	move.l	%a0,46(%a3)		| icmpCompletion
	move.l	%a6,50(%a3)		| userDataPtr: our vars (the completion routine has no A5 world)
	clr.w	V_DONE(%a6)
	lea	s_ping(%pc),%a0
	bsr	say_prefix
	move.l	%a3,%a0
	.short	_ControlAsync
	bsr	log_err
	tst.w	%d0
	bne	ctlerr
	| wait for the completion routine (or 10 s)
	move.l	Ticks,%d4
	add.l	#600,%d4
2:	.short	_SystemTask
	tst.w	V_DONE(%a6)
	bne.s	3f
	cmp.l	Ticks,%d4
	bhi.s	2b
	lea	s_stuck(%pc),%a0
	bsr	say
	bra.s	4f
3:	move.w	V_RESULT(%a6),%d0
	beq.s	5f
	lea	s_timeout(%pc),%a0
	bsr	say_prefix
	bsr	say_err
	bra.s	4f
5:	lea	s_reply(%pc),%a0
	bsr	say_prefix
	move.l	V_IN(%a6),%d0
	sub.l	V_OUT(%a6),%d0
	bsr	say_dec
	lea	s_ticks(%pc),%a0
	bsr	say
4:	subq.w	#1,%d5
	bne	ping_loop
	bra	wait

ctlerr:
	lea	s_ctlerr(%pc),%a0
	bsr	say_prefix
	bsr	say_err
wait:
	lea	s_quit(%pc),%a0
	bsr	say
loop:
	.short	_SystemTask
	clr.w	-(%sp)
	move.w	#0xFFFF,-(%sp)		| every event: disk inserts must reach the front application
	pea	event(%pc)
	.short	_GetNextEvent
	tst.w	(%sp)+
	beq.s	loop
	move.w	event(%pc),%d0
	cmp.w	#1,%d0
	beq.s	1f
	cmp.w	#3,%d0
	beq.s	1f
	cmp.w	#5,%d0
	bne.s	loop
1:	.short	_ExitToShell

| ---- the echo completion routine: A0 = ICMPParamBlock. Interrupt time: no traps, no heap.
|      It copies what the main loop shows and raises the flag; userDataPtr (offset 68) is vars.
echo_done:
	movem.l	%d0/%a1,-(%sp)
	move.l	68(%a0),%a1		| icmpEchoInfo.userDataPtr -> vars
	move.w	ioResult(%a0),V_RESULT(%a1)
	move.l	50(%a0),V_OUT(%a1)	| echoRequestOut
	move.l	54(%a0),V_IN(%a1)	| echoReplyIn
	move.w	#1,V_DONE(%a1)
	movem.l	(%sp)+,%d0/%a1
	rts

| ---- helpers ----
pbclear:				| a3 -> 64-byte block
	movem.l	%a0/%d0,-(%sp)
	move.l	%a3,%a0
	moveq	#15,%d0
1:	clr.l	(%a0)+
	dbf	%d0,1b
	movem.l	(%sp)+,%a0/%d0
	rts
say_ip:					| d0 = IP address -> a.b.c.d
	movem.l	%d1-%d4,-(%sp)
	move.l	%d0,%d3
	moveq	#3,%d4			| (not d2: say_dec uses d0-d2, and the first version looped for ever)
1:	rol.l	#8,%d3
	moveq	#0,%d0
	move.b	%d3,%d0
	bsr	say_dec
	tst.w	%d4
	beq.s	2f
	lea	s_dot(%pc),%a0
	bsr	say_pstr_nocr
2:	dbf	%d4,1b
	movem.l	(%sp)+,%d1-%d4
	rts

do_log:
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
	move.l	#200000,%d1
4:	cmp.l	4(%a0),%d0
	beq.s	5f
	subq.l	#1,%d1
	bne.s	4b
5:	rts
log_err:				| d0.w = OSErr -> "PiPing: result -NNN"
	movem.l	%d0-%d2/%a0-%a1,-(%sp)
	lea	err_text(%pc),%a1
	lea	err_digits(%pc),%a0
	move.w	%d0,%d1
	ext.l	%d1
	move.b	#' ',(%a0)
	tst.l	%d1
	bpl.s	1f
	neg.l	%d1
	move.b	#'-',(%a0)
1:	move.l	%d1,%d0
	divu	#10000,%d0
	add.b	#'0',%d0
	move.b	%d0,1(%a0)
	clr.w	%d0
	swap	%d0
	divu	#1000,%d0
	add.b	#'0',%d0
	move.b	%d0,2(%a0)
	clr.w	%d0
	swap	%d0
	divu	#100,%d0
	add.b	#'0',%d0
	move.b	%d0,3(%a0)
	clr.w	%d0
	swap	%d0
	divu	#10,%d0
	add.b	#'0',%d0
	move.b	%d0,4(%a0)
	swap	%d0
	add.b	#'0',%d0
	move.b	%d0,5(%a0)
	bsr	do_log
	movem.l	(%sp)+,%d0-%d2/%a0-%a1
	rts
err_text:
	.ascii	"PiPing: result"
err_digits:
	.ascii	" 00000"
	.byte	0
	.even
window:
	lea	0xF9000000,%a0
	tst.b	MMU32Bit
	bne.s	1f
	lea	0x00900000,%a0
1:	rts

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
	mulu	#6,%d0
	add.w	%d0,V_X(%a6)
	move.l	%a0,-(%sp)
	.short	_DrawString
	rts
say_dec:				| d0 unsigned long -> decimal
	lea	tmp+20(%pc),%a0
	moveq	#0,%d1
1:	divull	#10,%d2:%d0		| GNU as: the 32-bit form with the remainder in d2
	add.b	#'0',%d2
	move.b	%d2,-(%a0)
	addq.w	#1,%d1
	tst.l	%d0
	bne.s	1b
	move.b	%d1,-(%a0)
	bra	say_pstr_nocr
say_sdec:
	tst.l	%d0
	bpl.s	say_dec
	neg.l	%d0
	move.l	%d0,-(%sp)
	lea	s_minus(%pc),%a0
	bsr.s	say_pstr_nocr
	move.l	(%sp)+,%d0
	bra.s	say_dec
say_err:				| d0.w = OSErr, then a new line
	ext.l	%d0
	bsr	say_sdec
	bra	crlf

| ---- data ----
	.even
	.equ	V_Y,		0
	.equ	V_X,		2
	.equ	V_DONE,		4
	.equ	V_RESULT,	6
	.equ	V_OUT,		8
	.equ	V_IN,		12
vars:	.space	16
wrect:	.short	60, 40, 300, 480
wtitle:	.byte	23
	.ascii	"PiPing - MacTCP over Pi"
	.even
s_title:   .byte 34
	.ascii "MacTCP over the PiStorm's Ethernet"
	.even
s_ipp:	   .byte 4
	.ascii ".IPP"
	.even
s_noipp:   .byte 44
	.ascii "MacTCP (.IPP) could not be opened: installed?"
	.even
s_open:	   .byte 12
	.ascii "MacTCP open."
	.even
s_addr:	   .byte 12
	.ascii "My address: "
	.even
s_mask:	   .byte 7
	.ascii "  mask "
	.even
s_ping:	   .byte 22
	.ascii "Ping 192.168.68.1 ... "
	.even
s_reply:   .byte 9
	.ascii "reply in "
	.even
s_ticks:   .byte 6
	.ascii " ticks"
	.even
s_timeout: .byte 23
	.ascii "no reply, MacTCP said: "
	.even
s_stuck:   .byte 35
	.ascii "no completion within 10 s (stuck)  "
	.even
s_ctlerr:  .byte 15
	.ascii "Control error: "
	.even
s_quit:	   .byte 30
	.ascii "Click or press a key to quit. "
	.even
s_dot:	   .byte 1
	.ascii "."
	.even
s_minus:   .byte 1
	.ascii "-"
	.even
payload:   .ascii "PiStorm-SE Aurora68 ping payload"	| 32 bytes
	.even
pb:	.space 64
tmp:	.space 64
event:	.space 16
app_end:
