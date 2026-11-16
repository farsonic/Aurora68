| piblue.s — "PiBlue": the Pi's Bluetooth, driven from the Macintosh. MPL-2.0.
| GNU as MIT syntax, 68020, packaged by tools/mac/mkapp.py into a plain application.
|
| What it is. Alice68's Bluetooth host (emu68/src/alice/bt/) runs on the service core and talks
| to the Pi's chip over the PL011 UART; keyboards and mice it pairs with end up as Macintosh
| input through the input ring and the .PiInput driver (docs/09). What people need from the host
| is small: see its state, scan for devices, pair with one (and read the passkey it shows), forget
| one, reconnect one. The host exposes exactly that in `struct an_bt` at offset 0x3800 of the
| network board (alice_net.h): a state word, a message, the paired devices ("keys": address,
| name, kind), the devices found by the last scan, and a command box (cmd, cmd_addr, cmd_seq,
| cmd_done, cmd_result) that takes one command at a time. On the Amiga, Alice68-Card drives it;
| this is the Macintosh counterpart, a text window with single-letter commands like PiWiFi.
|
| Keys:  S scan     P pair with found device n     C connect paired device n
|        F forget paired device n      R redraw     Q quit
| A command that wants a device asks for its number on the status line.

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
	.equ	portRect,	16

| ---- struct an_bt (alice_net.h; offsets from tools/mac/an_offsets.c's sibling in the scratchpad) ----
	.equ	BOARD_OFF,	0x20000
	.equ	BT,		0x3800
	.equ	B_STATE,	0
	.equ	B_SCANNING,	4
	.equ	B_PAIRING,	8
	.equ	B_PASSKEY,	12
	.equ	B_PASSKEY_SHOWN, 16
	.equ	B_KEYS_GEN,	20
	.equ	B_NKEYS,	24
	.equ	B_NFOUND,	28
	.equ	B_CMD,		32
	.equ	B_CMD_SEQ,	36
	.equ	B_CMD_DONE,	40
	.equ	B_CMD_RESULT,	44
	.equ	B_CMD_ADDR,	48		| 8 bytes
	.equ	B_MSG,		56		| 64 bytes
	.equ	B_BDADDR,	120		| 8 bytes
	.equ	B_KEYS,		128		| 6 x 72: addr 0, key 8, type 24, flags 28, name 32
	.equ	KEY_SIZE,	72
	.equ	K_TYPE,		24
	.equ	K_FLAGS,	28
	.equ	K_NAME,		32
	.equ	B_FOUND,	560		| 12 x 56: addr 0, cod 8, rssi 12, name 16
	.equ	FOUND_SIZE,	56
	.equ	F_RSSI,		12
	.equ	F_NAME,		16
	.equ	MAXKEYS,	6
	.equ	MAXFOUND,	12
	.equ	CMD_SCAN,	1
	.equ	CMD_PAIR,	2
	.equ	CMD_FORGET,	3
	.equ	CMD_CONNECT,	4
	.equ	BTK_CONNECTED,	0
	.equ	BTK_KEYBOARD,	1
	.equ	BTK_MOUSE,	2
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
	move.w	#0xFFFF,-(%sp)		| every event (disk inserts must reach the front application; the rest is ignored)
	pea	event(%pc)
	.short	_GetNextEvent
	tst.w	(%sp)+
	bne.s	have_event
	| redraw when the host's visible state changes: a hash of the words people see
	bsr	bt
	move.l	%a1,%d0
	beq.s	loop
	bsr	hash
	cmp.l	V_HASH(%a6),%d0
	beq.s	loop
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
	beq	quit
	cmp.b	#'r',%d0
	beq.s	do_redraw
	cmp.b	#'s',%d0
	beq	do_scan
	cmp.b	#'p',%d0
	beq	do_pair
	cmp.b	#'c',%d0
	beq	do_connect
	cmp.b	#'f',%d0
	beq	do_forget
	bra.s	loop
do_redraw:
	bsr	redraw
	bra.s	loop
quit:
	.short	_ExitToShell

| hash: a1 = an_bt -> d0 = a word over the fields the display shows (so redraws follow changes)
hash:
	move.l	B_STATE(%a1),%d0
	add.l	B_SCANNING(%a1),%d0
	add.l	B_PAIRING(%a1),%d0
	add.l	B_PASSKEY_SHOWN(%a1),%d0
	rol.l	#5,%d0
	add.l	B_KEYS_GEN(%a1),%d0
	add.l	B_NFOUND(%a1),%d0
	add.l	B_CMD_DONE(%a1),%d0
	rol.l	#7,%d0
	add.l	B_MSG(%a1),%d0
	add.l	B_MSG+4(%a1),%d0
	add.l	B_MSG+8(%a1),%d0
	rts

| ---- commands ----
do_scan:
	lea	s_scanning(%pc),%a0
	bsr	status_line
	moveq	#CMD_SCAN,%d0
	move.l	#20*60,%d1
	bsr	command
	bsr	redraw
	bra	loop

| pair / connect / forget take a device: found[n] for pair, keys[n] for the other two
do_pair:
	lea	s_whichfound(%pc),%a0
	bsr	status_line
	bsr	readnumber
	bmi	cancelled
	bsr	bt
	move.l	%a1,%d1
	beq	cancelled
	cmp.l	B_NFOUND(%a1),%d0
	bhs	cancelled
	mulu	#FOUND_SIZE,%d0
	lea	B_FOUND(%a1,%d0.l),%a0
	bsr	set_addr
	lea	s_pairing(%pc),%a0
	bsr	status_line
	moveq	#CMD_PAIR,%d0
	move.l	#60*60,%d1		| a minute: the passkey has to be typed on the device
	bsr	command
	bsr	redraw
	bra	loop
do_connect:
	moveq	#CMD_CONNECT,%d2
	bra.s	keyed_command
do_forget:
	moveq	#CMD_FORGET,%d2
keyed_command:
	lea	s_whichkey(%pc),%a0
	bsr	status_line
	bsr	readnumber
	bmi	cancelled
	bsr	bt
	move.l	%a1,%d1
	beq	cancelled
	cmp.l	B_NKEYS(%a1),%d0
	bhs	cancelled
	mulu	#KEY_SIZE,%d0
	lea	B_KEYS(%a1,%d0.l),%a0
	bsr	set_addr
	lea	s_working(%pc),%a0
	bsr	status_line
	move.l	%d2,%d0
	move.l	#30*60,%d1
	bsr	command
	bsr	redraw
	bra	loop
cancelled:
	bsr	redraw
	bra	loop

| set_addr: a0 -> a record starting with the 8-byte address -> cmd_addr
set_addr:
	bsr	bt
	move.l	(%a0),B_CMD_ADDR(%a1)
	move.l	4(%a0),B_CMD_ADDR+4(%a1)
	rts

| command: d0 = command, d1 = timeout in ticks -> d0 = result (0 ok, < 0 error, -1 timeout)
command:
	movem.l	%d1-%d3/%a0-%a1,-(%sp)
	bsr	bt
	move.l	%a1,%d2
	beq.s	9f
	move.l	%d0,B_CMD(%a1)
	move.l	B_CMD_SEQ(%a1),%d2
	addq.l	#1,%d2
	move.l	%d2,B_CMD_SEQ(%a1)
	move.l	Ticks,%d3
	add.l	%d1,%d3
1:	.short	_SystemTask
	cmp.l	B_CMD_DONE(%a1),%d2
	beq.s	2f
	cmp.l	Ticks,%d3
	bhi.s	1b
	moveq	#-1,%d0
	bra.s	8f
2:	move.l	B_CMD_RESULT(%a1),%d0
8:	move.l	%d0,V_RESULT(%a6)
	movem.l	(%sp)+,%d1-%d3/%a0-%a1
	rts
9:	moveq	#-1,%d0
	bra.s	8b

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
	bsr	bt
	move.l	%a1,%d0
	bne.s	1f
	lea	s_noboard(%pc),%a0
	bsr	say
	bra	redraw_help
1:	move.l	%a1,%a3
	bsr	hash
	move.l	%d0,V_HASH(%a6)
	lea	s_state(%pc),%a0
	bsr	say_prefix
	move.l	B_STATE(%a3),%d0
	cmp.l	#4,%d0
	bls.s	2f
	moveq	#5,%d0
2:	lea	state_names(%pc),%a0
	mulu	#12,%d0
	add.l	%d0,%a0
	bsr	say_prefix
	tst.l	B_SCANNING(%a3)
	beq.s	3f
	lea	s_scanningtag(%pc),%a0
	bsr	say_prefix
3:	tst.l	B_PAIRING(%a3)
	beq.s	4f
	lea	s_pairingtag(%pc),%a0
	bsr	say_prefix
4:	bsr	crlf
	lea	s_msg(%pc),%a0
	bsr	say_prefix
	lea	B_MSG(%a3),%a1
	bsr	say_cstr
	lea	s_addr(%pc),%a0
	bsr	say_prefix
	lea	B_BDADDR(%a3),%a1
	bsr	say_bdaddr
	bsr	crlf
	| the passkey, while the host shows one
	tst.l	B_PASSKEY_SHOWN(%a3)
	beq.s	5f
	lea	s_passkey(%pc),%a0
	bsr	say_prefix
	move.l	B_PASSKEY(%a3),%d0
	bsr	say_dec
	bsr	crlf
5:	tst.l	V_RESULT(%a6)
	beq.s	6f
	lea	s_cmderr(%pc),%a0
	bsr	say_prefix
	move.l	V_RESULT(%a6),%d0
	bsr	say_sdec
	bsr	crlf
6:	| paired devices
	bsr	crlf
	lea	s_keyshead(%pc),%a0
	bsr	say
	move.l	B_NKEYS(%a3),%d3
	beq.s	8f
	cmp.l	#MAXKEYS,%d3
	bls.s	7f
	moveq	#MAXKEYS,%d3
7:	lea	B_KEYS(%a3),%a2
	moveq	#0,%d4
71:	move.w	#8,V_X(%a6)
	move.l	%d4,%d0
	bsr	say_dec
	move.w	#8+3*6,V_X(%a6)
	lea	K_NAME(%a2),%a1
	bsr	say_cstr_nocr
	move.w	#8+30*6,V_X(%a6)
	move.l	%a2,%a1
	bsr	say_bdaddr
	move.w	#8+44*6,V_X(%a6)
	move.l	K_FLAGS(%a2),%d0
	bsr	say_kind
	bsr	crlf
	lea	KEY_SIZE(%a2),%a2
	addq.l	#1,%d4
	cmp.l	%d3,%d4
	blo.s	71b
	bra.s	9f
8:	lea	s_nokeys(%pc),%a0
	bsr	say
9:	| found devices
	bsr	crlf
	lea	s_foundhead(%pc),%a0
	bsr	say
	move.l	B_NFOUND(%a3),%d3
	beq.s	11f
	cmp.l	#MAXFOUND,%d3
	bls.s	10f
	moveq	#MAXFOUND,%d3
10:	lea	B_FOUND(%a3),%a2
	moveq	#0,%d4
101:	move.w	#8,V_X(%a6)
	move.l	%d4,%d0
	bsr	say_dec
	move.w	#8+3*6,V_X(%a6)
	lea	F_NAME(%a2),%a1
	bsr	say_cstr_nocr
	move.w	#8+30*6,V_X(%a6)
	move.l	%a2,%a1
	bsr	say_bdaddr
	move.w	#8+44*6,V_X(%a6)
	move.l	F_RSSI(%a2),%d0
	bsr	say_sdec
	bsr	crlf
	lea	FOUND_SIZE(%a2),%a2
	addq.l	#1,%d4
	cmp.l	%d3,%d4
	blo.s	101b
	bra.s	redraw_help
11:	lea	s_nofound(%pc),%a0
	bsr	say
redraw_help:
	move.w	#300,V_Y(%a6)
	move.w	#8,V_X(%a6)
	lea	s_help(%pc),%a0
	bsr	say
	movem.l	(%sp)+,%d0-%d4/%a0-%a3
	rts

| say_kind: d0 = key flags -> "keyboard", "mouse", "device", with " (connected)" when it is
say_kind:
	movem.l	%d0,-(%sp)
	lea	s_device(%pc),%a0
	btst	#BTK_KEYBOARD,%d0
	beq.s	1f
	lea	s_keyboard(%pc),%a0
	bra.s	2f
1:	btst	#BTK_MOUSE,%d0
	beq.s	2f
	lea	s_mouse(%pc),%a0
2:	bsr	say_prefix
	movem.l	(%sp)+,%d0
	btst	#BTK_CONNECTED,%d0
	beq.s	3f
	lea	s_connected(%pc),%a0
	bsr	say_prefix
3:	rts

status_line:
	movem.l	%d0/%a0-%a1,-(%sp)
	move.l	%a0,%a1
	move.w	#278,V_Y(%a6)
	move.w	#8,V_X(%a6)
	pea	erase_status(%pc)
	.short	_EraseRect
	move.l	%a1,%a0
	bsr	say_prefix
	movem.l	(%sp)+,%d0/%a0-%a1
	rts

| readnumber: one or two digits typed on the status line, Return -> d0 (N flag set = cancelled)
readnumber:
	movem.l	%d1-%d2/%a0,-(%sp)
	moveq	#0,%d2
1:	.short	_SystemTask
	clr.w	-(%sp)
	move.w	#0xFFFF,-(%sp)		| every event; only keys are used here
	pea	event(%pc)
	.short	_GetNextEvent
	tst.w	(%sp)+
	beq.s	1b
	lea	event(%pc),%a0
	cmp.w	#keyDown,(%a0)
	beq.s	11f
	cmp.w	#autoKey,(%a0)
	bne.s	1b
11:	move.b	5(%a0),%d1
	cmp.b	#0x0D,%d1
	beq.s	3f
	cmp.b	#0x03,%d1
	beq.s	3f
	cmp.b	#0x1B,%d1
	beq.s	4f
	cmp.b	#'0',%d1
	blo.s	1b
	cmp.b	#'9',%d1
	bhi.s	1b
	move.w	V_X(%a6),-(%sp)
	move.w	V_Y(%a6),-(%sp)
	.short	_MoveTo
	move.w	%d1,-(%sp)
	.short	_DrawChar
	add.w	#6,V_X(%a6)
	sub.b	#'0',%d1
	ext.w	%d1
	ext.l	%d1
	mulu	#10,%d2
	add.l	%d1,%d2
	bra.s	1b
3:	move.l	%d2,%d0
	bra.s	5f
4:	moveq	#-1,%d0
5:	movem.l	(%sp)+,%d1-%d2/%a0
	tst.l	%d0
	rts

| ---- the board ----
window:
	lea	0xF9000000,%a0
	tst.b	MMU32Bit
	bne.s	1f
	lea	0x00900000,%a0
1:	rts
bt:					| a1 = struct an_bt, or 0 without a Pi
	bsr	window
	cmp.l	#0x50534D42,(%a0)
	bne.s	1f
	lea	BOARD_OFF+BT(%a0),%a1
	rts
1:	suba.l	%a1,%a1
	rts

| ---- text output (a6 -> vars), the same helpers as PiWiFi ----
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
	cmp.w	#60,%d0
	blt.s	1b
2:	move.b	%d0,(%a0)
	bra.s	say_pstr_nocr
say_dec:
	lea	tmp+20(%pc),%a0
	moveq	#0,%d1
1:	divull	#10,%d2:%d0		| GNU as: "divull" is the 32-bit form (ext $0002, remainder d2); "divul" encodes the 64-bit dividend (ext $0402)
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
say_bdaddr:				| a1 -> 6 bytes, as AA:BB:CC:DD:EE:FF
	moveq	#5,%d2
1:	moveq	#0,%d0
	move.b	(%a1)+,%d0
	bsr.s	say_hex2
	tst.w	%d2
	beq.s	2f
	movem.l	%d2/%a1,-(%sp)
	lea	s_colon(%pc),%a0
	bsr	say_pstr_nocr
	movem.l	(%sp)+,%d2/%a1
2:	dbf	%d2,1b
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

| ---- data ----
	.even
	.equ	V_Y,		0
	.equ	V_X,		2
	.equ	V_HASH,		4
	.equ	V_RESULT,	8
vars:	.space	16
wrect:	.short	40, 30, 360, 500
erase_status: .short 268, 4, 292, 468
wtitle:	.byte	27
	.ascii	"PiBlue - the Pi's Bluetooth"
	.even
s_title:    .byte 26
	.ascii "The PiStorm's Bluetooth   "
	.even
s_noboard:  .byte 22
	.ascii "No Pi at slot $9.     "
	.even
s_state:    .byte 7
	.ascii "State: "
	.even
state_names:
	.byte 3
	.ascii "OFF        "
	.byte 11
	.ascii "NO FIRMWARE"
	.byte 8
	.ascii "STARTING   "
	.byte 5
	.ascii "READY      "
	.byte 6
	.ascii "FAILED     "
	.byte 7
	.ascii "UNKNOWN    "
s_scanningtag: .byte 11
	.ascii "  scanning "
	.even
s_pairingtag: .byte 10
	.ascii "  pairing "
	.even
s_msg:      .byte 5
	.ascii "Pi:  "
	.even
s_addr:     .byte 9
	.ascii "Address: "
	.even
s_passkey:  .byte 32
	.ascii "Passkey to type on the device:  "
	.even
s_cmderr:   .byte 21
	.ascii "Last command failed: "
	.even
s_keyshead: .byte 52
	.ascii "#  Paired device              Address        Kind    "
	.even
s_nokeys:   .byte 20
	.ascii "   (none paired yet)"
	.even
s_foundhead: .byte 52
	.ascii "#  Found by the last scan     Address        Signal  "
	.even
s_nofound:  .byte 22
	.ascii "   (nothing found yet)"
	.even
s_scanning: .byte 20
	.ascii "Scanning (10 s)...  "
	.even
s_pairing:  .byte 42
	.ascii "Pairing: type the passkey on the device... "
	.even
s_working:  .byte 11
	.ascii "Working... "
	.even
s_whichfound: .byte 34
	.ascii "Pair with found device number:    "
	.even
s_whichkey: .byte 30
	.ascii "Paired device number:         "
	.even
s_help:     .byte 56
	.ascii "S scan   P pair n   C connect n   F forget n   Q quit   "
	.even
s_keyboard: .byte 8
	.ascii "keyboard"
	.even
s_mouse:    .byte 5
	.ascii "mouse"
	.even
s_device:   .byte 6
	.ascii "device"
	.even
s_connected: .byte 12
	.ascii " (connected)"
	.even
s_colon:    .byte 1
	.ascii ":"
	.even
s_minus:    .byte 1
	.ascii "-"
	.even
tmp:	.space 64
event:	.space 16
app_end:
