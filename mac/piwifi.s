| piwifi.s — "PiWiFi": the Pi's WiFi, driven from the Macintosh. MPL-2.0.
| GNU as MIT syntax, 68020, packaged by tools/mac/mkapp.py into a plain application.
|
| What it is. The Macintosh SE never had WiFi; the Pi under the PiStorm has a Broadcom chip that
| Alice68's driver brings up on the service core (docs/07). The driver is controlled through unit 0
| of the network board in the slot-$9 window: a state word, a message for people, a command box
| (one command at a time: fill cmd and arg, add one to cmd_seq, wait for cmd_done to catch up), and
| a table of scan results. On the Amiga a Workbench program called Alice68-Card does this; this is
| the Macintosh counterpart, kept deliberately plain (a text window and single-letter commands)
| so it runs under System 6 without a resource compiler. A control panel with a proper dialog can
| follow once the path is proven on hardware.
|
| Keys:  S scan and list networks   J join (asks for the name and the password)
|        D disconnect   O switch the radio on or off   R redraw   Q quit
|
| Everything the program shows comes from the board; nothing is cached, so a network joined from
| the kernel's own cmdline.txt (wifi.ssid=...) shows up as connected just the same.

	.text
	.globl	app_start

| ---- traps ----
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
	.equ	_DrawChar,	0xA883
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
	.equ	portRect,	16		| in the GrafPort

| ---- the network board (alice_net.h) ----
	.equ	BOARD_OFF,	0x20000		| inside the slot-$9 window
	.equ	AN_MAGIC,	0x414E4554	| 'ANET'? no: the header's magic is checked by the ENET driver; here we check the window's
	.equ	UNIT0,		0x400		| struct an_unit of unit 0, the WiFi
	.equ	SCAN,		0x1800		| struct an_scan[36], 56 bytes each
	.equ	SCAN_SIZE,	56
	.equ	SC_SSID,	8
	.equ	SC_RSSI,	44
	.equ	SC_CHANNEL,	48
	.equ	SC_SECURITY,	52
	.equ	U_KIND,		0
	.equ	U_STATE,	4
	.equ	U_STATE_SEQ,	8
	.equ	U_MAC,		20
	.equ	U_MSG,		28
	.equ	U_SSID,		220
	.equ	U_RSSI,		264
	.equ	U_CHANNEL,	268
	.equ	U_SCAN_COUNT,	416
	.equ	U_CMD_SEQ,	424
	.equ	U_CMD,		428
	.equ	U_CMD_DONE,	432
	.equ	U_CMD_RESULT,	436
	.equ	U_ARG,		440
	.equ	U_ADMIN_OFF,	628
	.equ	CMD_JOIN,	2
	.equ	CMD_SCAN,	3
	.equ	CMD_DISCONNECT,	4
	.equ	CMD_ENABLE,	12
	.equ	J_SSID,		0		| struct an_join inside arg: ssid[36], pass[68], country[4]
	.equ	J_PASS,		36
	.equ	J_COUNTRY,	104
	.equ	LINEH,		12

| the Pi's log through the INFO service (as the drivers do it): a1 = NUL-terminated text
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
	clr.l	-(%sp)			| result
	clr.l	-(%sp)			| wStorage
	pea	wrect(%pc)
	pea	wtitle(%pc)
	st	-(%sp)			| visible
	move.w	#0,-(%sp)		| documentProc
	move.l	#-1,-(%sp)		| in front
	sf	-(%sp)			| no goAway (Q quits)
	clr.l	-(%sp)			| refCon
	.short	_NewWindow
	move.l	(%sp)+,%d7		| WindowPtr
	move.l	%d7,-(%sp)
	.short	_SetPort
	move.w	#4,-(%sp)		| Monaco
	.short	_TextFont
	move.w	#9,-(%sp)
	.short	_TextSize
	lea	vars(%pc),%a6
	bsr	redraw

| ---- the main loop: keys, and a redraw whenever the board's state changes ----
loop:
	.short	_SystemTask
	clr.w	-(%sp)
	move.w	#0xFFFF,-(%sp)		| every event (disk inserts must reach the front application; the rest is ignored)
	pea	event(%pc)
	.short	_GetNextEvent
	tst.w	(%sp)+
	bne.s	have_event
	| no event: has the unit's state_seq moved since the last redraw?
	bsr	unit
	move.l	%a1,%d0
	beq.s	loop
	move.l	U_STATE_SEQ(%a1),%d0
	cmp.l	V_SEQ(%a6),%d0
	beq.s	loop
	bsr	redraw
	bra.s	loop
have_event:
	lea	event(%pc),%a0
	cmp.w	#keyDown,(%a0)
	beq.s	1f
	cmp.w	#autoKey,(%a0)
	bne.s	loop
1:	move.b	5(%a0),%d0		| the character
	or.b	#0x20,%d0		| lower case
	cmp.b	#'q',%d0
	beq	quit
	cmp.b	#'r',%d0
	beq.s	do_redraw
	cmp.b	#'s',%d0
	beq	do_scan
	cmp.b	#'j',%d0
	beq	do_join
	cmp.b	#'d',%d0
	beq	do_disconnect
	cmp.b	#'o',%d0
	beq	do_onoff
	bra.s	loop
do_redraw:
	bsr	redraw
	bra.s	loop
quit:
	.short	_ExitToShell

| ---- commands ----
do_scan:
	lea	s_scanning(%pc),%a0
	bsr	status_line
	moveq	#CMD_SCAN,%d0
	move.l	#20*60,%d1		| 20 s
	bsr	command
	move.w	#1,V_SHOWSCAN(%a6)
	bsr	redraw
	bra	loop

do_join:
	log	"A68 Wi-Fi: join: asking for the network name"
	bsr	unit
	move.l	%a1,%d0
	beq	loop
	| the join record goes straight into the command box's argument area
	lea	U_ARG(%a1),%a2
	moveq	#26,%d0			| 108 bytes cleared
1:	clr.l	(%a2)+
	dbf	%d0,1b
	lea	U_ARG(%a1),%a2
	lea	s_askssid(%pc),%a0
	bsr	status_line
	lea	J_SSID(%a2),%a0
	moveq	#32,%d0
	bsr	readline
	tst.w	%d0
	bmi	cancelled
	log	"A68 Wi-Fi: join: name read, asking for the password"
	lea	s_askpass(%pc),%a0
	bsr	status_line
	lea	J_PASS(%a2),%a0
	moveq	#63,%d0
	bsr	readline
	tst.w	%d0
	bmi	cancelled
	log	"A68 Wi-Fi: join: password read, joining"
	lea	s_joining(%pc),%a0
	bsr	status_line
	moveq	#CMD_JOIN,%d0
	move.l	#40*60,%d1		| 40 s: association, the four-way handshake, DHCP is not ours
	bsr	command
	bsr	redraw
	bra	loop
cancelled:
	log	"A68 Wi-Fi: join: cancelled (Escape)"
	bsr	redraw
	bra	loop

do_log:
	bsr	window
	lea	0x1000(%a0),%a0		| INFO command block
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

do_disconnect:
	moveq	#CMD_DISCONNECT,%d0
	move.l	#10*60,%d1
	bsr	command
	bsr	redraw
	bra	loop

do_onoff:
	bsr	unit
	move.l	%a1,%d0
	beq	loop
	moveq	#1,%d0
	tst.l	U_ADMIN_OFF(%a1)
	bne.s	1f			| off now -> on
	moveq	#0,%d0			| on now -> off
1:	move.l	%d0,U_ARG(%a1)		| arg[0..3]: 1 on, 0 off
	moveq	#CMD_ENABLE,%d0
	move.l	#10*60,%d1
	bsr	command
	bsr	redraw
	bra	loop

| command: d0 = command, d1 = timeout in ticks; the argument is already in U_ARG.
| Returns d0 = cmd_result (0 ok, negative = the driver's error), or -1 after the timeout.
command:
	movem.l	%d1-%d4/%a0-%a2,-(%sp)
	bsr	unit
	move.l	%a1,%d2
	beq.s	9f
	move.l	%a1,%a2			| d3, d4 and a2: Toolbox traps keep D3-D7 and A2-A6, and _SystemTask
	move.l	%d0,U_CMD(%a2)		| in the loop below clobbered the sequence number when it sat in D2
	move.l	U_CMD_SEQ(%a2),%d4	| (every command then "failed" with -1 after its timeout, although
	addq.l	#1,%d4			| the board had long done it: the first scan in the testbed)
	move.l	%d4,U_CMD_SEQ(%a2)	| publishes the command
	move.l	Ticks,%d3
	add.l	%d1,%d3
1:	.short	_SystemTask		| let the rest of the machine breathe while we wait
	cmp.l	U_CMD_DONE(%a2),%d4
	beq.s	2f
	cmp.l	Ticks,%d3
	bhi.s	1b
	moveq	#-1,%d0
	bra.s	8f
2:	move.l	U_CMD_RESULT(%a2),%d0
8:	move.l	%d0,V_RESULT(%a6)
	movem.l	(%sp)+,%d1-%d4/%a0-%a2
	rts
9:	moveq	#-1,%d0
	bra.s	8b

| ---- the display ----
| redraw: erase the window and show everything the board says
redraw:
	movem.l	%d0-%d3/%a0-%a3,-(%sp)
	move.l	%d7,%a0
	pea	portRect(%a0)
	.short	_EraseRect
	move.w	#14,V_Y(%a6)
	move.w	#8,V_X(%a6)
	lea	s_title(%pc),%a0
	bsr	say
	bsr	unit
	move.l	%a1,%d0
	bne.s	1f
	lea	s_noboard(%pc),%a0
	bsr	say
	bra	redraw_help
1:	move.l	%a1,%a3
	move.l	U_STATE_SEQ(%a3),V_SEQ(%a6)
	| "State: CONNECTED" from the table of names
	lea	s_state(%pc),%a0
	bsr	say_prefix
	move.l	U_STATE(%a3),%d0
	cmp.l	#8,%d0
	bls.s	2f
	moveq	#9,%d0
2:	lea	state_names(%pc),%a0
	mulu	#12,%d0
	add.l	%d0,%a0
	bsr	say
	| the driver's message
	lea	s_msg(%pc),%a0
	bsr	say_prefix
	lea	U_MSG(%a3),%a1
	bsr	say_cstr
	| when connected: network, signal, channel
	cmp.l	#5,U_STATE(%a3)
	bne.s	3f
	lea	s_network(%pc),%a0
	bsr	say_prefix
	lea	U_SSID(%a3),%a1
	bsr	say_cstr
	lea	s_signal(%pc),%a0
	bsr	say_prefix
	move.l	U_RSSI(%a3),%d0
	bsr	say_sdec
	lea	s_dbm(%pc),%a0
	bsr	say_prefix
	move.l	U_CHANNEL(%a3),%d0
	bsr	say_dec
	bsr	crlf
3:	lea	s_addr(%pc),%a0
	bsr	say_prefix
	lea	U_MAC(%a3),%a1
	bsr	say_mac
	bsr	crlf
	| the last command's result, if it failed
	tst.l	V_RESULT(%a6)
	beq.s	4f
	lea	s_cmderr(%pc),%a0
	bsr	say_prefix
	move.l	V_RESULT(%a6),%d0
	bsr	say_sdec
	bsr	crlf
4:	| scan results: name, signal, channel, security
	tst.w	V_SHOWSCAN(%a6)
	beq	redraw_help
	bsr	crlf
	lea	s_scanhead(%pc),%a0
	bsr	say
	move.l	U_SCAN_COUNT(%a3),%d3
	beq.s	6f
	cmp.l	#14,%d3
	bls.s	5f
	moveq	#14,%d3			| what fits in the window
5:	bsr	board
	lea	SCAN(%a0),%a2
	subq.l	#1,%d3
7:	cmp.w	#216,V_Y(%a6)		| and never into the status line (baseline 228): with a failed
	bhi	redraw_help		| command's line, 13 or 14 networks reached it (23 November 2026)
	move.w	#8,V_X(%a6)
	lea	SC_SSID(%a2),%a1
	bsr	say_cstr_nocr
	move.w	#8+34*6,V_X(%a6)
	move.l	SC_RSSI(%a2),%d0
	bsr	say_sdec
	move.w	#8+40*6,V_X(%a6)
	move.l	SC_CHANNEL(%a2),%d0
	bsr	say_dec
	move.w	#8+45*6,V_X(%a6)
	move.l	SC_SECURITY(%a2),%d0
	bsr	say_security
	bsr	crlf
	lea	SCAN_SIZE(%a2),%a2
	dbf	%d3,7b
	bra.s	redraw_help
6:	lea	s_nonets(%pc),%a0
	bsr	say
redraw_help:
	move.w	#250,V_Y(%a6)
	move.w	#8,V_X(%a6)
	lea	s_help(%pc),%a0
	bsr	say
	movem.l	(%sp)+,%d0-%d3/%a0-%a3
	rts

| status_line: a0 = pstring shown on the status line (second from the bottom), used while waiting
status_line:
	movem.l	%d0/%a0-%a1,-(%sp)
	move.l	%a0,%a1
	move.l	%d7,%a0
	move.w	#228,V_Y(%a6)
	move.w	#8,V_X(%a6)
	lea	erase_status(%pc),%a0
	pea	(%a0)
	.short	_EraseRect
	move.l	%a1,%a0
	bsr	say_prefix
	movem.l	(%sp)+,%d0/%a0-%a1
	rts

| say_security: d0 = SEC_* bits (fmac.h): 1 open, 2 WEP, 4 WPA, 8 WPA2
say_security:
	lea	s_open(%pc),%a0
	btst	#3,%d0
	beq.s	1f
	lea	s_wpa2(%pc),%a0
	bra.s	3f
1:	btst	#2,%d0
	beq.s	2f
	lea	s_wpa(%pc),%a0
	bra.s	3f
2:	btst	#1,%d0
	beq.s	3f
	lea	s_wep(%pc),%a0
3:	bra	say_prefix

| ---- a line editor on the status line: a0 = C-string buffer, d0 = maximum characters.
|      Return finishes (d0 = length), Escape cancels (d0 = -1), Delete erases. ----
readline:
	movem.l	%d1-%d5/%a0-%a2,-(%sp)	| the length and the limit live in D4/D5: traps keep D3-D7, not D2
	move.l	%a0,%a2
	move.w	%d0,%d5			| max
	moveq	#0,%d4			| length
rl_loop:
	.short	_SystemTask
	clr.w	-(%sp)
	move.w	#0xFFFF,-(%sp)		| every event; only keys are used here
	pea	event(%pc)
	.short	_GetNextEvent
	tst.w	(%sp)+
	beq.s	rl_loop
	lea	event(%pc),%a0
	cmp.w	#keyDown,(%a0)
	beq.s	rl_key
	cmp.w	#autoKey,(%a0)
	bne.s	rl_loop
rl_key:	move.b	5(%a0),%d1		| the character
	cmp.b	#0x0D,%d1		| Return
	beq.s	rl_done
	cmp.b	#0x03,%d1		| Enter
	beq.s	rl_done
	cmp.b	#0x1B,%d1		| Escape
	beq.s	rl_cancel
	cmp.b	#0x08,%d1		| Delete
	bne.s	rl_char
	tst.w	%d4
	beq.s	rl_loop
	subq.w	#1,%d4
	clr.b	(%a2,%d4.w)
	| step back and blank the character
	sub.w	#6,V_X(%a6)
	move.w	V_X(%a6),-(%sp)
	move.w	V_Y(%a6),-(%sp)
	.short	_MoveTo
	move.w	#' ',-(%sp)
	.short	_DrawChar
	bra.s	rl_loop
rl_char:
	cmp.b	#' ',%d1
	blo.s	rl_loop
	cmp.w	%d5,%d4
	bhs.s	rl_loop
	move.b	%d1,(%a2,%d4.w)
	addq.w	#1,%d4
	clr.b	(%a2,%d4.w)
	move.w	V_X(%a6),-(%sp)
	move.w	V_Y(%a6),-(%sp)
	.short	_MoveTo
	move.w	%d1,-(%sp)
	.short	_DrawChar
	add.w	#6,V_X(%a6)
	bra	rl_loop
rl_done:
	move.w	%d4,%d0
	ext.l	%d0
	bra.s	rl_out
rl_cancel:
	moveq	#-1,%d0
rl_out:
	movem.l	(%sp)+,%d1-%d5/%a0-%a2
	rts

| ---- the board ----
window:
	lea	0xF9000000,%a0
	tst.b	MMU32Bit
	bne.s	1f
	lea	0x00900000,%a0
1:	rts
board:					| a0 = the network board
	bsr	window
	lea	BOARD_OFF(%a0),%a0
	rts
unit:					| a1 = unit 0, or 0 when there is no Pi or no WiFi unit
	bsr	window
	cmp.l	#0x50534D42,(%a0)	| 'PSMB'
	bne.s	1f
	lea	BOARD_OFF+UNIT0(%a0),%a1
	tst.l	U_KIND(%a1)
	beq.s	1f
	rts
1:	suba.l	%a1,%a1
	rts

| ---- text output (a6 -> vars) ----
say:					| a0 -> pstring, then a new line
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
say_cstr:				| a1 -> C string (at most 60 shown), then a new line
	bsr.s	say_cstr_nocr
	bra.s	crlf
say_cstr_nocr:
	lea	tmp(%pc),%a0
	moveq	#0,%d0
1:	move.b	(%a1)+,%d1
	beq.s	2f
	addq.w	#1,%d0
	move.b	%d1,(%a0,%d0.w)
	cmp.w	#60,%d0
	blt.s	1b
2:	move.b	%d0,(%a0)
	bra.s	say_pstr_nocr
say_dec:				| d0 unsigned long -> decimal
	lea	tmp+20(%pc),%a0
	moveq	#0,%d1
1:	divull	#10,%d2:%d0		| GNU as: "divull" is the 32-bit form (ext $0002, remainder d2); "divul" encodes the 64-bit dividend (ext $0402)
	add.b	#'0',%d2
	move.b	%d2,-(%a0)
	addq.w	#1,%d1
	tst.l	%d0
	bne.s	1b
	move.b	%d1,-(%a0)
	bra.s	say_pstr_nocr
say_sdec:				| d0 signed long
	tst.l	%d0
	bpl.s	say_dec
	neg.l	%d0
	move.l	%d0,-(%sp)
	lea	s_minus(%pc),%a0
	bsr.s	say_pstr_nocr
	move.l	(%sp)+,%d0
	bra.s	say_dec
say_mac:				| a1 -> 6 bytes, as 12 hex digits
	moveq	#5,%d2
1:	moveq	#0,%d0
	move.b	(%a1)+,%d0
	bsr.s	say_hex2
	dbf	%d2,1b
	rts
say_hex2:
	lea	tmp(%pc),%a0
	move.b	#2,(%a0)
	move.b	%d0,%d1
	lsr.b	#4,%d1
	bsr.s	nib
	move.b	%d1,1(%a0)
	move.b	%d0,%d1
	and.b	#15,%d1
	bsr.s	nib
	move.b	%d1,2(%a0)
	movem.l	%d0-%d2/%a1,-(%sp)
	bsr	say_pstr_nocr
	movem.l	(%sp)+,%d0-%d2/%a1
	rts
nib:	add.b	#'0',%d1
	cmp.b	#'9',%d1
	bls.s	1f
	addq.b	#7,%d1
1:	rts

| ---- data (the application is position independent; everything lives in the code segment) ----
	.even
	.equ	V_Y,		0
	.equ	V_X,		2
	.equ	V_SEQ,		4
	.equ	V_RESULT,	8
	.equ	V_SHOWSCAN,	12
vars:	.space	16
wrect:	.short	50, 30, 320, 500		| top, left, bottom, right
erase_status: .short 218, 4, 244, 468
wtitle:	.byte	14			| the window's title: the program's name (since 21 November 2026)
	.ascii	"Aurora68 Wi-Fi"
	.even
s_title:    .byte 31
	.ascii "The PiStorm's WiFi (unit 0)    "
	.even
s_noboard:  .byte 46
	.ascii "No Pi at slot $9, or the kernel has no WiFi.  "
	.even
s_state:    .byte 7
	.ascii "State: "
	.even
state_names:				| 12 bytes each: length byte + 11 characters
	.byte 3
	.ascii "OFF        "
	.byte 8
	.ascii "STARTING   "
	.byte 4
	.ascii "IDLE       "
	.byte 8
	.ascii "SCANNING   "
	.byte 7
	.ascii "JOINING    "
	.byte 9
	.ascii "CONNECTED  "
	.byte 5
	.ascii "RETRY      "
	.byte 6
	.ascii "FAILED     "
	.byte 11
	.ascii "NO FIRMWARE"
	.byte 7
	.ascii "UNKNOWN    "
s_msg:      .byte 5
	.ascii "Pi:  "
	.even
s_network:  .byte 9
	.ascii "Network: "
	.even
s_signal:   .byte 8
	.ascii "Signal: "
	.even
s_dbm:      .byte 14
	.ascii " dBm, channel "
	.even
s_addr:     .byte 9
	.ascii "Address: "
	.even
s_cmderr:   .byte 21
	.ascii "Last command failed: "
	.even
s_scanhead: .byte 54
	.ascii "Network                           Signal  Ch  Security"
	.even
s_nonets:   .byte 19
	.ascii "No networks found. "
	.even
s_scanning: .byte 12
	.ascii "Scanning... "
	.even
s_joining:  .byte 11
	.ascii "Joining... "
	.even
s_askssid:  .byte 32
	.ascii "Network name, then Return:      "
	.even
s_askpass:  .byte 32
	.ascii "Password (empty if open):       "
	.even
s_help:     .byte 58
	.ascii "S scan   J join   D disconnect   O radio on/off   Q quit  "
	.even
s_open:     .byte 4
	.ascii "open"
	.even
s_wep:      .byte 3
	.ascii "WEP"
	.even
s_wpa:      .byte 3
	.ascii "WPA"
	.even
s_wpa2:     .byte 4
	.ascii "WPA2"
	.even
s_minus:    .byte 1
	.ascii "-"
	.even
tmp:	.space 64
event:	.space 16
app_end:
