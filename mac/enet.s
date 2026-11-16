| enet.s — ".Network_EtherNet_PiStorm": the Macintosh Ethernet driver of PiStorm-SE (Aurora68).
| 68k, GNU as MIT syntax, 68020. MPL-2.0.
|
| Backstory. Classic Mac OS networking (MacTCP, EtherTalk, Open Transport's ENET link) talks to
| Ethernet cards through one driver interface, Apple's ".ENET" (Inside Macintosh: Networking,
| chapter 11): nine Control calls, a protocol table, and a protocol-handler calling convention
| from the 1987 EtherTalk card. Any card whose declaration ROM carries a catNetwork/typeEtherNet
| sResource with a driver in its sDriverDir is found by the ".ENET" shell and opened with OpenSlot.
| This driver is that driver for the Pi: it speaks the Alice68 network board protocol underneath
| (alice_net.h, shared with the user's Amiga kernel): a 128 KB region of the slot-$9 window with a
| command box per unit and rings of 2 KB frame slots whose indexes count up for ever. Unit 1 is
| the Pi's Ethernet (GENET on a Pi 4, the host file bridge in the QEMU testbed); unit 0 is WiFi,
| which a control panel will join to a network; both look the same to this driver.
|
| Received frames are taken from the unit's receive ring by a VBL task (60 Hz, interrupt level,
| the way a card's receive interrupt would) and delivered either to the protocol handler attached
| for the frame's type (A3/A4/D1 convention, ReadPacket/ReadRest) or to the first pending ERead of
| that type (Apple's "default protocol handler" mode, which MacTCP uses).

	.text
	.globl	drvr_start

| ---- the window (psmb.h) and the Alice68 network board inside it (alice_net.h) ----
	.equ	WIN24,		0x00900000
	.equ	WIN32,		0xF9000000
	.equ	PSMB_MAGIC,	0x50534D42
	.equ	OFF_SERVICES,	0x0100		| 8 x 32 bytes: type.w id.w cmd_off.l caps.l param[5].l
	.equ	SVC_ALICE,	6		| service entry 3: param[0] = board offset
	.equ	AN_MAGIC,	0x4136384E	| 'A68N'
	.equ	AN_OFF_UNIT1,	0x0C00		| struct an_unit of unit 1 (Ethernet), 2 KB
	.equ	AN_OFF_RX1,	0x0C000		| 16 receive slots of 2 KB
	.equ	AN_OFF_TX1,	0x1A000		| 12 transmit slots of 2 KB
	.equ	AN_RX_SLOTS,	16
	.equ	AN_TX_SLOTS,	12
	.equ	AN_SLOT_BYTES,	0x800
	.equ	AN_HDR_IRQ,	12
	.equ	AN_HDR_DRVVER,	20		| an_hdr.amiga_ver: the Mac driver's version, once open (the kernel raises the slot interrupt only then)
| struct an_unit offsets (alice_net.h; `make check` compares them with the header the kernel compiles)
	.equ	U_KIND,		0
	.equ	U_STATE,	4
	.equ	U_LINK,		12
	.equ	U_MAC,		20		| 8 bytes, 6 used
	.equ	U_RX_PKTS,	384
	.equ	U_CMD_SEQ,	424
	.equ	U_CMD,		428
	.equ	U_CMD_DONE,	432
	.equ	U_CMD_RESULT,	436
	.equ	U_ARG,		440		| 128 bytes
	.equ	U_RX_HEAD,	568
	.equ	U_RX_TAIL,	572
	.equ	U_TX_HEAD,	576
	.equ	U_TX_TAIL,	580
	.equ	U_RX_ON,	584
| commands
	.equ	AN_CMD_START,	1
	.equ	AN_CMD_RXON,	6
	.equ	AN_CMD_RXOFF,	7
	.equ	AN_CMD_FILTER,	8
	.equ	AN_FILT_PROMISC, 1
	.equ	AN_FILT_ALLMULTI, 2

| ---- Mac OS ----
	.equ	MMU32Bit,	0x0CB2
	.equ	JIODone,	0x08FC
	.equ	ioTrap,		6
	.equ	ioCompletion,	12
	.equ	ioResult,	16
	.equ	csCode,		26
	.equ	eProtType,	28
	.equ	eMultiAddr,	28
	.equ	ePointer,	30
	.equ	eBuffSize,	34
	.equ	eDataSize,	36
	.equ	dCtlFlags,	4
	.equ	dCtlQHead,	8
	.equ	dCtlQTail,	12
	.equ	dCtlStorage,	20
	.equ	dCtlRefNum,	24
	.equ	noQueueBit,	9
	.equ	drvrActiveBit,	7
	.equ	noErr,		0
	.equ	controlErr,	-17
	.equ	statusErr,	-18
	.equ	ioErr,		-36
	.equ	memFullErr,	-108
	.equ	eMultiErr,	-91
	.equ	eLenErr,	-92
	.equ	LAPProtErr,	-94
	.equ	excessCollsns,	-95
	.equ	cbNotFound,	-1102
	.equ	reqAborted,	-1105
	.equ	buf2SmallErr,	-3101
	.equ	_NewPtrSysClear,0xA71E
	.equ	_DisposePtr,	0xA01F
	.equ	_VInstall,	0xA033
	.equ	_VRemove,	0xA034

| ---- driver globals (dCtlStorage) ----
	.equ	G_DCE,		0		| long: our DCE
	.equ	G_ADDR,		4		| 6 bytes: our Ethernet address
	.equ	G_PAD,		10
	.equ	G_OVERFLOW,	12		| 3 longs for EGetInfo
	.equ	G_TIMEOUT,	16
	.equ	G_BADDEST,	20
	.equ	G_BOARD,	24		| long: the Alice68 network board (68k address)
	.equ	G_INRX,		28		| word: receive poll in progress (reentrancy guard)
	.equ	G_VBL,		30		| VBLTask: qLink 4, qType 2, vblAddr 4, vblCount 2, vblPhase 2 = 14 bytes
	.equ	G_VBLSELF,	44		| long: globals pointer, right after the VBL task record
	.equ	G_CURPTR,	48		| long: frame data pointer for ReadPacket/ReadRest
	.equ	G_CURLEFT,	52		| word: bytes left
	.equ	G_NMULTI,	54		| word
	.equ	G_MULTI,	56		| 8 x 6 bytes
	.equ	G_PROT,		104		| 16 x PROT_SIZE
	.equ	PROT_TYPE,	0		| word (0 = 802.3; 0xFFFF = free)
	.equ	PROT_HANDLER,	2		| long (0 = default handler: ERead)
	.equ	PROT_RDQ,	6		| long: first pending ERead PB (linked by qLink)
	.equ	PROT_SIZE,	10
	.equ	NPROT,		16
	.equ	G_SIQ,		G_PROT+NPROT*PROT_SIZE	| SlotIntQElement (16 bytes): the slot-9 interrupt handler
	.equ	G_INTS,		G_SIQ+16	| long: slot interrupts taken
	.equ	G_SIZE,		G_INTS+4
	.equ	_SIntInstall,	0xA075		| its own OS trap (Inside Macintosh V): A0 = SlotIntQElement, D0.w = slot
	.equ	sIQType,	6
	.equ	PI_SLOT,	9

| ---- DRVR header ----
drvr_start:
	.short	0x4C00			| dNeedLock | dStatEnable | dCtlEnable
	.short	0, 0, 0
	.short	d_open-drvr_start
	.short	d_prime-drvr_start
	.short	d_control-drvr_start
	.short	d_status-drvr_start
	.short	d_close-drvr_start
	.byte	24
	.ascii	".Network_EtherNet_PiStorm"
	.even
	.short	0x0100			| version (the .ENET shell picks the newest driver)

| ---- log through the Pi's INFO service: `log "text"` (uses local labels 8 and 9) ----
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

| ---- helpers ----
window:					| a0 = window base
	lea	WIN32,%a0
	tst.b	MMU32Bit
	bne.s	1f
	lea	WIN24,%a0
1:	rts

| strip24: d0 = logical address made safe for 24-bit mode
strip24:
	tst.b	MMU32Bit
	bne.s	1f
	and.l	#0x00FFFFFF,%d0
1:	rts

| unitcmd: a3 = globals, d1 = command, arg already written -> d0 = cmd_result (0 ok), ioErr on timeout
unitcmd:
	movem.l	%a0/%d2-%d3,-(%sp)
	move.l	G_BOARD(%a3),%a0
	lea	AN_OFF_UNIT1(%a0),%a0
	move.l	%d1,U_CMD(%a0)
	move.l	U_CMD_SEQ(%a0),%d2
	addq.l	#1,%d2
	move.l	%d2,U_CMD_SEQ(%a0)
	move.l	#4000000,%d3
1:	cmp.l	U_CMD_DONE(%a0),%d2
	beq.s	2f
	subq.l	#1,%d3
	bne.s	1b
	moveq	#ioErr,%d0
	bra.s	3f
2:	move.l	U_CMD_RESULT(%a0),%d0
3:	movem.l	(%sp)+,%a0/%d2-%d3
	rts

| ---- Open ----
d_open:
	movem.l	%d1-%d7/%a0-%a4,-(%sp)
	move.l	%a1,%a4			| DCE
	tst.l	dCtlStorage(%a4)
	bne	open_ok
	log	"ENET: open"
	move.l	#G_SIZE,%d0
	.short	_NewPtrSysClear
	move.l	%a0,%d0
	beq	open_mem
	move.l	%a0,dCtlStorage(%a4)
	move.l	%a0,%a3			| globals
	move.l	%a4,G_DCE(%a3)
	move.l	%a3,G_VBLSELF(%a3)
	bsr	window
	cmp.l	#PSMB_MAGIC,(%a0)
	bne	open_fail
	| find the Alice68 boards through the service table: entry with type 6
	lea	OFF_SERVICES(%a0),%a1
	moveq	#7,%d0
1:	cmp.w	#SVC_ALICE,(%a1)
	beq.s	2f
	lea	32(%a1),%a1
	dbf	%d0,1b
	bra	open_fail
2:	move.l	12(%a1),%d0		| param[0]: board offset in the window
	add.l	%a0,%d0
	move.l	%d0,G_BOARD(%a3)
	move.l	%d0,%a1
	cmp.l	#AN_MAGIC,(%a1)
	bne	open_fail
	clr.l	AN_HDR_IRQ(%a1)		| whatever happened before we existed is not ours to service
	lea	AN_OFF_UNIT1(%a1),%a1
	tst.l	U_KIND(%a1)
	beq	open_fail		| no Ethernet unit
	log	"ENET: board found"
	| protocol table empty
	lea	G_PROT(%a3),%a2
	moveq	#NPROT-1,%d0
3:	move.w	#0xFFFF,PROT_TYPE(%a2)
	clr.l	PROT_HANDLER(%a2)
	clr.l	PROT_RDQ(%a2)
	lea	PROT_SIZE(%a2),%a2
	dbf	%d0,3b
	| bring the unit up and start taking frames from where the Pi is now
	moveq	#AN_CMD_START,%d1
	bsr	unitcmd
	move.l	U_RX_HEAD(%a1),U_RX_TAIL(%a1)
	moveq	#AN_CMD_RXON,%d1
	bsr	unitcmd
	move.l	U_MAC(%a1),G_ADDR(%a3)
	move.w	U_MAC+4(%a1),G_ADDR+4(%a3)
	| The card's interrupt (docs/07): the kernel raises the slot-9 line while the board's irq
	| flag is set; the ROM's level-2 handler runs the routines queued with SIntInstall. The
	| receive path runs from there, and from a VBL task as the safety net (every tick until the
	| first interrupt, then every 15 ticks).
	lea	G_SIQ(%a3),%a0
	clr.l	(%a0)
	move.w	#sIQType,4(%a0)
	move.w	#100,6(%a0)
	lea	slot_int(%pc),%a1
	move.l	%a1,8(%a0)
	move.l	%a3,12(%a0)		| sqParm: globals, back in A1
	move.w	#PI_SLOT,%d0
	.short	_SIntInstall
	tst.w	%d0
	beq.s	1f
	log	"ENET: SIntInstall failed"
1:	lea	G_VBL(%a3),%a0
	clr.l	(%a0)
	move.w	#1,4(%a0)		| vType
	lea	rx_vbl(%pc),%a1
	move.l	%a1,6(%a0)
	move.w	#1,10(%a0)
	move.w	#0,12(%a0)
	.short	_VInstall
	tst.w	%d0
	bne	open_fail
	| only now, with the handler installed, tell the kernel we are here: from this write on the
	| board's irq flag raises the slot-9 interrupt (announcing earlier, before SIntInstall, left
	| one boot in a level-2 storm with nobody to clear the flag)
	move.l	G_BOARD(%a3),%a0
	move.l	#0x00010000,AN_HDR_DRVVER(%a0)
	log	"ENET: open ok"
open_ok:
	moveq	#noErr,%d0
	bra.s	open_ret
open_mem:
	moveq	#memFullErr,%d0
	bra.s	open_ret
open_fail:
	log	"ENET: open failed"
	moveq	#ioErr,%d0
open_ret:
	movem.l	(%sp)+,%d1-%d7/%a0-%a4
	rts

| ---- Close ----
d_close:
	movem.l	%a0/%a3/%d1,-(%sp)
	move.l	dCtlStorage(%a1),%d0
	beq.s	1f
	move.l	%d0,%a3
	moveq	#AN_CMD_RXOFF,%d1
	bsr	unitcmd
	lea	G_VBL(%a3),%a0
	.short	_VRemove
	move.l	%a3,%a0
	.short	_DisposePtr
	clr.l	dCtlStorage(%a1)
1:	movem.l	(%sp)+,%a0/%a3/%d1
	moveq	#noErr,%d0
	rts

d_prime:
	moveq	#controlErr,%d0
	bra	iodone
d_status:
	moveq	#statusErr,%d0
	bra	iodone

| ---- Control dispatch: a0 = PB, a1 = DCE ----
d_control:
	movem.l	%d1-%d7/%a2-%a6,-(%sp)
	move.l	%a0,%a2			| PB
	move.l	%a1,%a4			| DCE
	move.l	dCtlStorage(%a4),%a3	| globals
	move.l	%a3,%d0
	beq	ctl_err
	move.w	csCode(%a2),%d0
	cmp.w	#1,%d0			| KillIO
	beq	ctl_noerr
	sub.w	#245,%d0
	blt	ctl_err
	cmp.w	#8,%d0
	bgt	ctl_err
	add.w	%d0,%d0
	move.w	ctl_table(%pc,%d0.w),%d0
	jmp	ctl_table(%pc,%d0.w)
ctl_table:
	.short	c_delmulti-ctl_table	| 245
	.short	c_addmulti-ctl_table	| 246
	.short	c_attachph-ctl_table	| 247
	.short	c_detachph-ctl_table	| 248
	.short	c_write-ctl_table	| 249
	.short	c_read-ctl_table	| 250
	.short	c_rdcancel-ctl_table	| 251
	.short	c_getinfo-ctl_table	| 252
	.short	c_setgeneral-ctl_table	| 253
ctl_err:
	moveq	#controlErr,%d0
	bra	ctl_done
ctl_noerr:
	moveq	#noErr,%d0
ctl_done:
	bsr	log_queue		| "ENET: done pb X qhead Y flags Z" (diagnostics for the Device Manager's queue)
	move.l	%a2,%a0			| the parameter block and the DCE back in A0/A1: iodone reads
	move.l	%a4,%a1			| ioTrap through A0 and IODone wants the DCE in A1 (EGetInfo
	movem.l	(%sp)+,%d1-%d7/%a2-%a6	| had left its buffer pointers there: the first call returned to 0)
	bra	iodone

| ESetGeneral: we always allow 1514-byte writes
c_setgeneral:
	bra.s	ctl_noerr

| EGetInfo: ePointer -> address (6) + overflow, timeout, bad destination (3 longs)
c_getinfo:
	move.l	ePointer(%a2),%d0
	bsr	strip24
	move.l	%d0,%a0
	move.w	eBuffSize(%a2),%d1
	moveq	#18,%d2
	cmp.w	%d2,%d1
	bge.s	1f
	move.w	%d1,%d2
1:	move.w	%d2,eDataSize(%a2)
	lea	G_ADDR(%a3),%a1
	bra.s	3f
2:	move.b	(%a1)+,(%a0)+
3:	dbf	%d2,2b
	bra.s	ctl_noerr

| EAttachPH: eProtType, ePointer = handler or 0
c_attachph:
	log	"ENET: EAttachPH"
	move.w	eProtType(%a2),%d1
	bsr	find_prot
	bne.s	1f
	moveq	#LAPProtErr,%d0		| already attached
	bra	ctl_done
1:	move.w	#0xFFFF,%d1
	bsr	find_prot		| a free entry (type $FFFF)? Z set = found
	beq.s	2f
	moveq	#LAPProtErr,%d0		| table full (the first version had this branch inverted and
	bra	ctl_done		| wrote the new protocol past the table, into the interrupt queue element)
2:	move.w	eProtType(%a2),PROT_TYPE(%a1)
	move.l	ePointer(%a2),%d0
	bsr	strip24
	move.l	%d0,PROT_HANDLER(%a1)
	clr.l	PROT_RDQ(%a1)
	bra	ctl_noerr

| EDetachPH
c_detachph:
	move.w	eProtType(%a2),%d1
	bsr	find_prot
	beq.s	1f
	moveq	#LAPProtErr,%d0
	bra	ctl_done
1:	move.w	#0xFFFF,PROT_TYPE(%a1)
	move.l	%a1,-(%sp)
2:	move.l	(%sp),%a1
	move.l	PROT_RDQ(%a1),%d0
	beq.s	3f
	move.l	%d0,%a0
	move.l	(%a0),PROT_RDQ(%a1)
	move.w	#reqAborted,%d0
	bsr	complete_pb
	bra.s	2b
3:	addq.l	#4,%sp
	bra	ctl_noerr

| ERead: queue the PB on its protocol's read list (default handler mode only)
c_read:
	log	"ENET: ERead"
	move.w	eProtType(%a2),%d1
	bsr	find_prot
	beq.s	1f
	moveq	#LAPProtErr,%d0
	bra	ctl_done
1:	tst.l	PROT_HANDLER(%a1)
	beq.s	2f
	moveq	#LAPProtErr,%d0		| a handler is attached: ERead not allowed
	bra	ctl_done
2:	cmp.w	#14,eBuffSize(%a2)
	bge.s	3f
	move.w	#buf2SmallErr,%d0
	bra	ctl_done
3:	btst	#noQueueBit,ioTrap(%a2)
	beq.s	4f
	moveq	#controlErr,%d0		| must be asynchronous
	bra	ctl_done
4:	| take the PB out of the DCE queue so other calls can proceed, then append to the read list
	bsr	dequeue_pb
	clr.l	(%a2)
	move.w	#1,ioResult(%a2)
	lea	PROT_RDQ(%a1),%a0
5:	tst.l	(%a0)
	beq.s	6f
	move.l	(%a0),%a0
	bra.s	5b
6:	move.l	%a2,(%a0)
	log	"ENET: ERead pending, dequeued"
	movem.l	(%sp)+,%d1-%d7/%a2-%a6
	rts				| no IODone: the PB completes when a frame arrives

| ERdCancel: ePointer -> the ERead PB
c_rdcancel:
	move.l	ePointer(%a2),%d0
	bsr	strip24
	move.l	%d0,%d2
	lea	G_PROT(%a3),%a1
	moveq	#NPROT-1,%d3
1:	lea	PROT_RDQ(%a1),%a0
2:	move.l	(%a0),%d0
	beq.s	3f
	cmp.l	%d0,%d2
	beq.s	4f
	move.l	%d0,%a0
	bra.s	2b
3:	lea	PROT_SIZE(%a1),%a1
	dbf	%d3,1b
	move.w	#cbNotFound,%d0
	bra	ctl_done
4:	move.l	%d2,%a1
	move.l	(%a1),(%a0)		| unlink
	move.l	%d2,%a0
	move.w	#reqAborted,%d0
	bsr	complete_pb
	bra	ctl_noerr

| EWrite: gather the WDS straight into the next transmit slot, insert the source address, bump tx_head
c_write:
	log	"ENET: EWrite"
	move.l	G_BOARD(%a3),%d6	| the board (not in A4: A4 is the DCE and must reach iodone intact)
	move.l	%d6,%a0
	lea	AN_OFF_UNIT1(%a0),%a5	| unit
	move.l	U_TX_HEAD(%a5),%d3
	move.l	%d3,%d4
	sub.l	U_TX_TAIL(%a5),%d4
	cmp.l	#AN_TX_SLOTS,%d4
	blt.s	0f
	moveq	#excessCollsns,%d0	| ring full
	bra	ctl_done
0:	move.l	%d3,%d4
	divu	#AN_TX_SLOTS,%d4
	swap	%d4			| remainder = slot index
	and.l	#0xFFFF,%d4
	mulu	#AN_SLOT_BYTES,%d4
	move.l	%d6,%a6
	lea	AN_OFF_TX1(%a6),%a6
	add.l	%d4,%a6			| the slot: len, flags, data
	lea	8(%a6),%a1		| data
	move.l	ePointer(%a2),%d0
	bsr	strip24
	move.l	%d0,%d7			| the WDS, walked through d7 (a0 is the data source below)
	move.l	%d0,%a0
	moveq	#0,%d2			| total
	move.w	(%a0),%d1
	cmp.w	#14,%d1
	blt	w_len
1:	move.l	%d7,%a0
	move.w	(%a0)+,%d1		| entry: length, pointer
	beq.s	3f
	move.l	(%a0)+,%d0
	move.l	%a0,%d7
	bsr	strip24
	move.l	%d0,%a0			| the entry's data
	ext.l	%d1
	add.l	%d1,%d2
	cmp.l	#1514,%d2
	bgt	w_len
	bra.s	2f
4:	move.b	(%a0)+,(%a1)+
2:	dbf	%d1,4b
	bra.s	1b
3:	lea	8(%a6),%a1
	move.l	G_ADDR(%a3),6(%a1)	| source address
	move.w	G_ADDR+4(%a3),10(%a1)
	cmp.l	#60,%d2
	bge.s	5f
6:	clr.b	(%a1,%d2.l)		| pad short frames with zeros
	addq.l	#1,%d2
	cmp.l	#60,%d2
	blt.s	6b
5:	move.l	%d2,(%a6)		| slot.len
	clr.l	4(%a6)			| slot.flags
	addq.l	#1,%d3
	move.l	%d3,U_TX_HEAD(%a5)	| publish
	bra	ctl_noerr
w_len:
	moveq	#eLenErr,%d0
	bra	ctl_done

| EAddMulti / EDelMulti: table of 8 addresses; the Pi accepts all multicast when any is registered
c_addmulti:
	lea	eMultiAddr(%a2),%a0
	bsr	find_multi
	beq.s	1f
	move.w	G_NMULTI(%a3),%d0
	cmp.w	#8,%d0
	bge.s	2f
	mulu	#6,%d0
	lea	G_MULTI(%a3,%d0.w),%a1
	move.l	(%a0)+,(%a1)+
	move.w	(%a0),(%a1)
	addq.w	#1,G_NMULTI(%a3)
1:	bsr	set_filter
	bra	ctl_noerr
2:	moveq	#eMultiErr,%d0
	bra	ctl_done
c_delmulti:
	lea	eMultiAddr(%a2),%a0
	bsr	find_multi
	bne.s	2b
	| remove entry d0 by moving the last one over it
	move.w	G_NMULTI(%a3),%d1
	subq.w	#1,%d1
	move.w	%d1,G_NMULTI(%a3)
	mulu	#6,%d1
	lea	G_MULTI(%a3,%d1.w),%a0
	move.l	(%a0)+,(%a1)+
	move.w	(%a0),(%a1)
	bsr	set_filter
	bra	ctl_noerr

| find_multi: a0 -> 6-byte address; returns Z set and a1 -> entry, d0 = index if found
find_multi:
	movem.l	%d1/%a0,-(%sp)
	moveq	#0,%d0
	lea	G_MULTI(%a3),%a1
1:	cmp.w	G_NMULTI(%a3),%d0
	bge.s	3f
	move.l	(%a0),%d1
	cmp.l	(%a1),%d1
	bne.s	2f
	move.w	4(%a0),%d1
	cmp.w	4(%a1),%d1
	beq.s	4f
2:	addq.l	#6,%a1
	addq.w	#1,%d0
	bra.s	1b
3:	moveq	#1,%d1			| clear Z
	tst.w	%d1
4:	movem.l	(%sp)+,%d1/%a0
	rts
| set_filter: AN_CMD_FILTER with ALLMULTI when any multicast address is registered
set_filter:
	movem.l	%d1-%d2/%a0,-(%sp)
	move.l	G_BOARD(%a3),%a0
	lea	AN_OFF_UNIT1(%a0),%a0
	moveq	#0,%d2
	tst.w	G_NMULTI(%a3)
	beq.s	1f
	moveq	#AN_FILT_ALLMULTI,%d2
1:	move.l	%d2,U_ARG(%a0)
	moveq	#AN_CMD_FILTER,%d1
	bsr	unitcmd
	movem.l	(%sp)+,%d1-%d2/%a0
	rts

| find_prot: d1 = type -> Z set and a1 -> entry if found
find_prot:				| d1 = type -> a1 = its entry, Z set; Z clear when there is none
	lea	G_PROT(%a3),%a1
	moveq	#NPROT-1,%d0
1:	cmp.w	PROT_TYPE(%a1),%d1
	beq.s	2f
	lea	PROT_SIZE(%a1),%a1
	dbf	%d0,1b
	moveq	#1,%d0			| not found: Z clear
	rts
2:	moveq	#0,%d0			| found: Z set (the first version restored d1 from the stack
	rts				| last, and that move set Z from d1, not from the search)

| dequeue_pb: remove a2 (the PB being served, head of the DCE queue a4) from the queue and clear
| drvrActive so the next request can start at once.
dequeue_pb:
	move.l	(%a2),%d0		| qLink
	move.l	%d0,dCtlQHead(%a4)
	bne.s	1f
	clr.l	dCtlQTail(%a4)
1:	bclr	#drvrActiveBit,dCtlFlags+1(%a4)
	rts

| complete_pb: a0 = PB (already unlinked), d0.w = result: set ioResult, call the completion routine
complete_pb:
	movem.l	%d1-%d2/%a0-%a1,-(%sp)
	move.w	%d0,ioResult(%a0)
	move.l	ioCompletion(%a0),%d1
	beq.s	1f
	move.l	%d1,%a1
	ext.l	%d0
	jsr	(%a1)			| A0 = PB, D0 = result
1:	movem.l	(%sp)+,%d1-%d2/%a0-%a1
	rts

| ---- receive: from the slot interrupt (A1 = globals; D0 = 1 "ours") or the VBL task ----
slot_int:
	movem.l	%d1-%d7/%a0-%a6,-(%sp)
	move.l	%a1,%a3
	addq.l	#1,G_INTS(%a3)
	cmp.l	#1,G_INTS(%a3)
	bne.s	1f
	log	"ENET: first slot interrupt"
1:	| Was it ours? The ROM runs the slot's handlers in order and stops at the first that
	| answers nonzero, so answering "yes" when the board's flag was clear starves the other
	| handlers on slot 9 (the input driver's, whose ring then stayed full and kept the line up:
	| the 68k spent the rest of that boot in the level-2 dispatcher).
	move.l	G_BOARD(%a3),%a0
	move.l	AN_HDR_IRQ(%a0),%d2	| nonzero: the board wanted us
	bsr	rx_body			| clears the flag, takes the frames
	move.l	%d2,%d0
	movem.l	(%sp)+,%d1-%d7/%a0-%a6
	rts
rx_vbl:
	movem.l	%d0-%d7/%a0-%a6,-(%sp)
	move.l	14(%a0),%a3		| globals (G_VBLSELF follows the task record)
	move.w	#1,%d0
	tst.l	G_INTS(%a3)
	beq.s	1f
	move.w	#15,%d0
1:	move.w	%d0,G_VBL+10(%a3)	| rearm
	bsr	rx_body
	movem.l	(%sp)+,%d0-%d7/%a0-%a6
	rts
| rx_body: A3 = globals; trashes everything (callers save). Clears the board's irq flag first so
| that a frame arriving while we work sets it again and the line stays up.
rx_body:
	tst.w	G_INRX(%a3)
	bne	rx_out
	move.w	#1,G_INRX(%a3)
	move.l	%a3,%a6
	move.l	G_BOARD(%a3),%a5
	clr.l	AN_HDR_IRQ(%a5)
	lea	AN_OFF_UNIT1(%a5),%a5	| unit
	moveq	#16,%d7			| frames per tick at most
rx_loop:
	move.l	U_RX_HEAD(%a5),%d0	| head (Pi)
	move.l	U_RX_TAIL(%a5),%d1	| tail (ours)
	cmp.l	%d0,%d1
	beq	rx_end
	move.l	%d1,%d2
	divu	#AN_RX_SLOTS,%d2
	swap	%d2
	and.l	#0xFFFF,%d2
	mulu	#AN_SLOT_BYTES,%d2
	move.l	G_BOARD(%a6),%a2
	lea	AN_OFF_RX1(%a2),%a2
	add.l	%d2,%a2			| slot: len, flags, data
	move.l	(%a2),%d3		| frame length
	lea	8(%a2),%a2		| data
	cmp.l	#14,%d3
	blt	rx_next
	| protocol: type field > 1500 -> type, else 0 (802.3)
	move.w	12(%a2),%d1
	cmp.w	#1500,%d1
	bhi.s	1f
	moveq	#0,%d1
1:	bsr	find_prot
	bne	rx_next			| nobody wants it
	move.l	PROT_HANDLER(%a1),%d0
	beq.s	rx_default
	| protocol handler: A3 -> first byte after the 14-byte header, D1 = bytes left, A4 -> ReadPacket
	move.l	%d0,%a0
	lea	14(%a2),%a1
	move.l	%a1,G_CURPTR(%a6)
	move.l	%d3,%d1
	sub.w	#14,%d1
	move.w	%d1,G_CURLEFT(%a6)
	move.l	%a1,%a3
	lea	readpacket(%pc),%a4
	movem.l	%a2/%a5/%d7,-(%sp)
	jsr	(%a0)
	movem.l	(%sp)+,%a2/%a5/%d7
	bra.s	rx_next
rx_default:
	| first pending ERead
	move.l	PROT_RDQ(%a1),%d0
	beq.s	rx_next
	move.l	%d0,%a0
	move.l	(%a0),PROT_RDQ(%a1)	| unlink
	move.l	ePointer(%a0),%d0
	move.l	%a0,-(%sp)
	bsr	strip24
	move.l	%d0,%a1			| buffer
	move.l	(%sp)+,%a0
	moveq	#0,%d4
	move.w	eBuffSize(%a0),%d4
	moveq	#noErr,%d5
	cmp.l	%d4,%d3
	ble.s	2f
	move.l	%d4,%d3			| truncate
	move.w	#buf2SmallErr,%d5
2:	move.w	%d3,eDataSize(%a0)
	move.l	%a2,%a4
	bra.s	4f
3:	move.b	(%a4)+,(%a1)+
4:	dbf	%d3,3b
	move.w	%d5,%d0
	move.l	%a6,%a3
	bsr	complete_pb
rx_next:
	move.l	%a6,%a3
	addq.l	#1,U_RX_TAIL(%a5)	| taken
	subq.w	#1,%d7
	bne	rx_loop
rx_end:
	clr.w	G_INRX(%a6)
rx_out:
	rts

| ---- ReadPacket (A4) / ReadRest (2(A4)) for protocol handlers; state in A6 = globals ----
readpacket:
	bra.s	rp_packet
	bra.s	rp_rest
rp_packet:				| D3 = bytes wanted, A3 = buffer
	move.l	G_CURPTR(%a6),%a0
	moveq	#0,%d1
	move.w	G_CURLEFT(%a6),%d1
	cmp.l	%d1,%d3
	bhi.s	rp_err
	sub.l	%d3,%d1
	move.w	%d1,G_CURLEFT(%a6)
	bra.s	2f
1:	move.b	(%a0)+,(%a3)+
2:	dbf	%d3,1b
	move.l	%a0,G_CURPTR(%a6)
	moveq	#0,%d3			| Z set: OK
	rts
rp_err:
	moveq	#eLenErr,%d0
	tst.w	%d3			| Z clear
	rts
rp_rest:				| D3 = buffer size, A3 = buffer
	move.l	G_CURPTR(%a6),%a0
	moveq	#0,%d1
	move.w	G_CURLEFT(%a6),%d1
	move.l	%d3,%d2
	cmp.l	%d1,%d2
	ble.s	1f
	move.l	%d1,%d2			| copy what is left
1:	sub.l	%d1,%d3			| result: <0 did not fit, >0 unused
	bra.s	3f
2:	move.b	(%a0)+,(%a3)+
3:	dbf	%d2,2b
	clr.w	G_CURLEFT(%a6)
	moveq	#0,%d0			| Z set
	rts

| log_queue: the PB being completed (a2), the DCE queue head and the flags (a4), as hex
log_queue:
	movem.l	%d0-%d2/%a0-%a1,-(%sp)
	move.l	%a2,%d0
	lea	lq_pb(%pc),%a0
	bsr	put_hex8
	move.l	dCtlQHead(%a4),%d0
	lea	lq_head(%pc),%a0
	bsr	put_hex8
	moveq	#0,%d0
	move.w	dCtlFlags(%a4),%d0
	lea	lq_flags(%pc),%a0
	bsr	put_hex8
	lea	lq_text(%pc),%a1
	bsr	do_log
	movem.l	(%sp)+,%d0-%d2/%a0-%a1
	rts
put_hex8:				| d0.l -> 8 hex digits at a0
	movem.l	%d0-%d2,-(%sp)
	moveq	#7,%d2
1:	rol.l	#4,%d0
	move.b	%d0,%d1
	and.b	#15,%d1
	add.b	#'0',%d1
	cmp.b	#'9',%d1
	bls.s	2f
	addq.b	#7,%d1
2:	move.b	%d1,(%a0)+
	dbf	%d2,1b
	movem.l	(%sp)+,%d0-%d2
	rts
lq_text:
	.ascii	"ENET: done pb "
lq_pb:
	.ascii	"00000000 qhead "
lq_head:
	.ascii	"00000000 flags "
lq_flags:
	.ascii	"00000000"
	.byte	0
	.even

| ---- common exit ----
iodone:
	btst	#noQueueBit,ioTrap(%a0)
	beq.s	1f
	log	"ENET: immediate call returns"
	rts
1:	log	"ENET: queued call -> IODone"
	move.l	JIODone,-(%sp)
	rts
	.even
drvr_end:
