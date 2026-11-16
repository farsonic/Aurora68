| pinet.s — "PiNet": Mac application that exercises the PiStorm-SE mailbox from the Mac side.
| GNU as MIT syntax, 68020, packaged by tools/mac/mkapp.py. MPL-2.0.
|
| It opens a window and reports: the mailbox header (firmware, Mac RAM), the block devices, the
| Ethernet card found through the Slot Manager (SGetTypeSRsrc catNetwork/typeEtherNet), the card's
| address from EGetInfo, then sends an ARP request for 192.168.68.1 (tools/pinet.py's peer) and
| waits up to 3 seconds for the reply through ERead. Click or press a key to quit.

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
	.equ	_TextFont,	0xA887
	.equ	_TextSize,	0xA88A
	.equ	_GetNextEvent,	0xA970
	.equ	_SystemTask,	0xA9B4
	.equ	_ExitToShell,	0xA9F4
	.equ	_Control,	0xA004
	.equ	_OpenSlot,	0xA200		| _Open with the slot bit (bit 9)
	.equ	_SlotManager,	0xA06E
	.equ	_Delay,		0xA03B
	.equ	Ticks,		0x016A
	.equ	MMU32Bit,	0x0CB2

| ---- log through the Pi's INFO service: `log "text"` appears as [MAC] ... in the kernel log ----
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

| ---- our globals in the window-relative area below A5: use a static block instead (position independent) ----
	.equ	LINEH,		14

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
	| window
	clr.l	-(%sp)			| result
	clr.l	-(%sp)			| wStorage
	pea	wrect(%pc)
	pea	wtitle(%pc)
	st	-(%sp)			| visible
	move.w	#0,-(%sp)		| documentProc
	move.l	#-1,-(%sp)		| in front
	sf	-(%sp)			| no goAway
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
	move.w	#16,4(%a6)		| y
	move.w	#8,6(%a6)		| x

	lea	s_title(%pc),%a0
	bsr	say
	log	"PiNet: started"
	| --- mailbox header ---
	bsr	window
	cmp.l	#0x50534D42,(%a0)
	beq.s	1f
	lea	s_nopi(%pc),%a0
	bsr	say
	bra	wait
1:	lea	s_pi(%pc),%a0
	bsr	say_prefix
	bsr	window			| (say_prefix's a0 was the string, not the window)
	lea	24(%a0),%a1		| fw_version string in the header
	bsr	say_cstr
	log	"PiNet: firmware string shown"
	lea	s_blk(%pc),%a0
	bsr	say_prefix
	bsr	window
	lea	0x2000(%a0),%a0
	move.l	(%a0),%d0		| blocks of device 0
	lsr.l	#8,%d0
	lsr.l	#3,%d0			| MiB
	bsr	say_dec
	lea	s_mib(%pc),%a0
	bsr	say_pstr_nocr
	bsr	window
	lea	0x2000+16(%a0),%a1
	bsr	say_cstr
	log	"PiNet: PiDisk 0 shown; looking for the Ethernet card"
	| --- find the Ethernet card: SGetTypeSRsrc, spCategory 4, spCType 1, spTBMask 3 ---
	lea	spblock(%pc),%a0
	move.l	%a0,%a2
	moveq	#15,%d0
2:	clr.l	(%a0)+
	dbf	%d0,2b
	| SpBlock (Slots.h): spResult 0, spsPointer 4, spSize 8, spOffsetData 12, spIOFileName 16,
	| spsExecPBlk 20, spParamData 24, spMisc 28, spReserved 32, spIOReserved 36, spRefNum 38,
	| spCategory 40, spCType 42, spDrvrSW 44, spDrvrHW 46, spTBMask 48, spSlot 49, spID 50,
	| spExtDev 51, spHwDev 52, spByteLanes 53, spFlags 54, spKey 55. (The first version had the
	| category at 48 and the mask at 55 and got -344, "no more sResources", for its trouble.)
	move.w	#4,40(%a2)		| spCategory: catNetwork
	move.w	#1,42(%a2)		| spCType: typeEtherNet
	move.b	#3,48(%a2)		| spTBMask: ignore drSW and drHW
	move.l	#1,24(%a2)		| spParamData: fAll, include disabled sResources
	move.l	%a2,%a0
	moveq	#0x0C,%d0		| SGetTypeSRsrc (Slots.h: 0x0C; 0x2B is SUpdateSRT)
	.short	_SlotManager
	bsr	log_err			| "PiNet: result N" in the kernel log
	tst.w	%d0
	beq.s	3f
	lea	s_nocard(%pc),%a0
	bsr	say
	bra	wait
3:	lea	s_card(%pc),%a0
	bsr	say_prefix
	moveq	#0,%d0
	move.b	49(%a2),%d0		| spSlot
	bsr	say_dec
	lea	s_sp(%pc),%a0
	bsr	say_pstr_nocr
	moveq	#0,%d0
	move.b	50(%a2),%d0		| spID
	bsr	say_dec
	bsr	crlf
	| --- OpenSlot ".ENET" ---
	lea	pb(%pc),%a3
	move.l	%a3,%a0
	moveq	#15,%d0
4:	clr.l	(%a0)+
	dbf	%d0,4b
	lea	s_enet(%pc),%a0
	move.l	%a0,18(%a3)		| ioNamePtr
	move.b	#0,27(%a3)		| ioPermssn fsCurPerm
	move.b	49(%a2),34(%a3)		| ioSlot
	move.b	50(%a2),35(%a3)		| ioID
	move.l	%a3,%a0
	.short	_OpenSlot
	tst.w	%d0
	beq.s	5f
	lea	s_openerr(%pc),%a0
	bsr	say_prefix
	bsr	say_err
	bra	wait
5:	move.w	24(%a3),%d6		| ioRefNum
	lea	s_open(%pc),%a0
	bsr	say_prefix
	move.w	%d6,%d0
	ext.l	%d0
	bsr	say_sdec
	bsr	crlf
	| --- EGetInfo ---
	bsr	pbclear
	move.w	%d6,24(%a3)
	move.w	#252,26(%a3)
	lea	info(%pc),%a0
	move.l	%a0,30(%a3)
	move.w	#18,34(%a3)
	move.l	%a3,%a0
	.short	_Control
	tst.w	%d0
	bne	ctlerr
	lea	s_addr(%pc),%a0
	bsr	say_prefix
	lea	info(%pc),%a1
	bsr	say_mac
	bsr	crlf
	| --- attach ARP (0x0806) with the default handler, post an ERead, send an ARP request ---
	bsr	pbclear
	move.w	%d6,24(%a3)
	move.w	#247,26(%a3)
	move.w	#0x0806,28(%a3)
	clr.l	30(%a3)
	move.l	%a3,%a0
	.short	_Control
	tst.w	%d0
	bne	ctlerr
	lea	rdpb(%pc),%a4
	move.l	%a4,%a0
	moveq	#15,%d0
6:	clr.l	(%a0)+
	dbf	%d0,6b
	move.w	%d6,24(%a4)
	move.w	#250,26(%a4)		| ERead
	move.w	#0x0806,28(%a4)
	lea	rxbuf(%pc),%a0
	move.l	%a0,30(%a4)
	move.w	#1536,34(%a4)
	move.l	%a4,%a0
	.short	0xA404			| _Control ,ASYNC
	| ARP request frame: dst broadcast, src filled by the driver, type 0806
	lea	frame(%pc),%a0
	move.l	#0xFFFFFFFF,(%a0)
	move.w	#0xFFFF,4(%a0)
	move.w	#0x0806,12(%a0)
	move.l	#0x00010800,14(%a0)	| hw 1, proto 0800
	move.w	#0x0604,18(%a0)		| hlen 6 plen 4
	move.w	#1,20(%a0)		| request
	lea	info(%pc),%a1
	move.l	(%a1),22(%a0)		| sender hw
	move.w	4(%a1),26(%a0)
	move.l	#0xC0A84402,28(%a0)	| sender 192.168.68.2
	clr.l	32(%a0)			| target hw
	clr.w	36(%a0)
	move.l	#0xC0A84401,38(%a0)	| target 192.168.68.1
	lea	wds(%pc),%a1
	move.w	#42,(%a1)
	move.l	%a0,2(%a1)
	clr.w	6(%a1)
	bsr	pbclear
	move.w	%d6,24(%a3)
	move.w	#249,26(%a3)		| EWrite
	move.l	%a1,30(%a3)
	move.l	%a3,%a0
	.short	_Control
	tst.w	%d0
	bne	ctlerr
	lea	s_sent(%pc),%a0
	bsr	say
	| wait up to 3 s for the ERead to complete
	move.l	Ticks,%d5
	add.l	#180,%d5
7:	tst.w	16(%a4)			| ioResult
	ble.s	8f
	cmp.l	Ticks,%d5
	bhi.s	7b
	lea	s_timeout(%pc),%a0
	bsr	say
	bra	wait
8:	lea	s_reply(%pc),%a0
	bsr	say_prefix
	moveq	#0,%d0
	move.w	36(%a4),%d0		| eDataSize
	bsr	say_dec
	lea	s_from(%pc),%a0
	bsr	say_pstr_nocr
	lea	rxbuf(%pc),%a1
	lea	6(%a1),%a1
	bsr	say_mac
	bsr	crlf
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
	move.w	#0x002A,-(%sp)		| mouseDown | keyDown | autoKey
	pea	event(%pc)
	.short	_GetNextEvent
	tst.w	(%sp)+
	beq.s	loop
	.short	_ExitToShell

| ---- text output helpers (a6 -> vars: 4 = y) ----
say:					| a0 -> pstring, then newline
	bsr.s	say_pstr_nocr
crlf:
	add.w	#LINEH,4(%a6)
	move.w	#8,6(%a6)
	rts
say_prefix:				| a0 -> pstring, no newline
say_pstr_nocr:
	| running pen position: x in 6(%a6), y in 4(%a6); draw at (x, y)
	movem.l	%a0,-(%sp)
	move.w	6(%a6),-(%sp)
	move.w	4(%a6),-(%sp)
	.short	_MoveTo
	movem.l	(%sp)+,%a0
	move.l	%a0,-(%sp)
	.short	_DrawString
	| advance x by 6 pixels per character (Monaco 9 is monospaced)
	moveq	#0,%d0
	move.b	(%a0),%d0
	mulu	#6,%d0
	add.w	%d0,6(%a6)
	rts
say_cstr:				| a1 -> C string (max 60)
	lea	tmp(%pc),%a0
	moveq	#0,%d0
1:	move.b	(%a1)+,%d1
	beq.s	2f
	addq.w	#1,%d0
	move.b	%d1,(%a0,%d0.w)
	cmp.w	#60,%d0
	blt.s	1b
2:	move.b	%d0,(%a0)
	clr.b	1(%a0,%d0.w)		| NUL after the text so the Pi can log what was built
	movem.l	%a0-%a1,-(%sp)
	lea	1(%a0),%a1
	bsr	do_log
	movem.l	(%sp)+,%a0-%a1
	bsr	say
	rts
say_dec:				| d0 unsigned long -> decimal, no newline
	lea	tmp+20(%pc),%a0
	moveq	#0,%d1
1:	divull	#10,%d2:%d0		| GNU as: "divull" is the 32-bit form (ext $0002, remainder d2); "divul" encodes the 64-bit dividend (ext $0402)
	add.b	#'0',%d2
	move.b	%d2,-(%a0)
	addq.w	#1,%d1
	tst.l	%d0
	bne.s	1b
	move.b	%d1,-(%a0)
	movem.l	%a0-%a1/%d0,-(%sp)
	lea	tmp+20(%pc),%a1
	clr.b	(%a1)			| NUL after the digits: log them too
	lea	1(%a0),%a1
	bsr	do_log
	movem.l	(%sp)+,%a0-%a1/%d0
	bsr	say_pstr_nocr
	rts
say_sdec:
	tst.l	%d0
	bpl.s	say_dec
	neg.l	%d0
	move.l	%d0,-(%sp)
	lea	s_minus(%pc),%a0
	bsr	say_pstr_nocr
	move.l	(%sp)+,%d0
	bra.s	say_dec
say_err:				| d0.w = OSErr
	ext.l	%d0
	bsr	say_sdec
	bra	crlf
say_mac:				| a1 -> 6 bytes
	moveq	#5,%d2
1:	moveq	#0,%d0
	move.b	(%a1)+,%d0
	bsr	say_hex2
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
pbclear:
	movem.l	%a0/%d0,-(%sp)
	move.l	%a3,%a0
	moveq	#15,%d0
1:	clr.l	(%a0)+
	dbf	%d0,1b
	movem.l	(%sp)+,%a0/%d0
	rts
| do_log: a1 = C string -> INFO service command (block 0): cmd 2, buf, len; waits briefly
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
| log_err: d0.w = OSErr -> "PiNet: result -NNN" in the kernel log
log_err:
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
	divu	#100,%d0
	add.b	#'0',%d0
	move.b	%d0,1(%a0)
	clr.w	%d0
	swap	%d0
	divu	#10,%d0
	add.b	#'0',%d0
	move.b	%d0,2(%a0)
	swap	%d0
	add.b	#'0',%d0
	move.b	%d0,3(%a0)
	bsr	do_log
	movem.l	(%sp)+,%d0-%d2/%a0-%a1
	rts
err_text:
	.ascii	"PiNet: result"
err_digits:
	.ascii	" 000"
	.byte	0
	.even
window:
	lea	0xF9000000,%a0
	tst.b	MMU32Bit
	bne.s	1f
	lea	0x00900000,%a0
1:	rts

| ---- data (in the code segment; the app is position independent) ----
	.even
wrect:	.short	60, 40, 300, 480		| top, left, bottom, right
wtitle:	.byte	21
	.ascii	"PiNet - PiStorm-SE test"
	.even
s_title:  .byte 36
	.ascii "PiStorm-SE mailbox and .ENET test   "
	.even
s_nopi:	  .byte 32
	.ascii "No PiStorm mailbox at slot $9.  "
	.even
s_pi:	  .byte 10
	.ascii "Pi: fw = "
	.byte 32
	.even
s_blk:	  .byte 10
	.ascii "PiDisk 0: "
	.even
s_mib:	  .byte 5
	.ascii " MiB "
	.even
s_nocard: .byte 43
	.ascii "Slot Manager: no Ethernet sResource found. "
	.even
s_card:	  .byte 20
	.ascii "Ethernet card: slot "
	.even
s_sp:	  .byte 9
	.ascii " sRsrcID "
	.even
s_enet:	  .byte 5
	.ascii ".ENET"
	.even
s_openerr: .byte 17
	.ascii "OpenSlot failed: "
	.even
s_open:	  .byte 22
	.ascii ".ENET open, refNum =  "
	.even
s_addr:	  .byte 18
	.ascii "Ethernet address: "
	.even
s_ctlerr: .byte 15
	.ascii "Control error: "
	.even
s_sent:	  .byte 42
	.ascii "ARP request for 192.168.68.1 sent (EWrite)"
	.even
s_timeout: .byte 36
	.ascii "No ARP reply within 3 seconds.      "
	.even
s_reply:  .byte 11
	.ascii "ARP reply: "
	.even
s_from:	  .byte 12
	.ascii " bytes from "
	.even
s_minus:  .byte 1
	.ascii "-"
	.even
s_quit:	  .byte 30
	.ascii "Click or press a key to quit. "
	.even
vars:	.long 0, 0			| 4: y (word), 6: x (word)
	.even
spblock: .space 64
pb:	.space 64
rdpb:	.space 64
info:	.space 32
wds:	.space 16
frame:	.space 64
tmp:	.space 64
event:	.space 16
rxbuf:	.space 1536
app_end:
