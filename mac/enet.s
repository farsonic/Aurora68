| enet.s — ".Network_EtherNet_PiStorm": Apple-style Ethernet driver (.ENET interface) over the
| PiStorm-SE mailbox net service (psmb.h). 68k, GNU as MIT syntax, 68020. MPL-2.0.
|
| Loaded by the .ENET shell through the slot-$9 declaration ROM when MacTCP / EtherTalk / Open
| Transport open the card (OpenSlot ".ENET", slot 9). Implements the control calls of Inside
| Macintosh: Networking ch. 11: ESetGeneral 253, EGetInfo 252, ERdCancel 251, ERead 250, EWrite 249,
| EDetachPH 248, EAttachPH 247, EAddMulti 246, EDelMulti 245. Received frames are taken from the
| Pi's receive ring by a VBL task (60 Hz; interrupt level, like a card's receive interrupt) and
| delivered either to the protocol handler attached for the type (A3/A4/D1 convention, ReadPacket /
| ReadRest) or to the first pending ERead of that type (default handler mode).

	.text
	.globl	drvr_start

| ---- psmb.h ----
	.equ	WIN24,		0x00900000
	.equ	WIN32,		0xF9000000
	.equ	PSMB_MAGIC,	0x50534D42
	.equ	OFF_SERVICES,	0x0100
	.equ	OFF_CMD,	0x1000
	.equ	NET_SVC,	2		| service index of the net service
	.equ	CMD_NET,	OFF_CMD+NET_SVC*64
	.equ	OFF_NETRX,	0x4000
	.equ	RX_SLOTS,	32
	.equ	RX_FRAME,	1536
	.equ	RX_SLOT_SIZE,	8+RX_FRAME
	.equ	NET_SEND,	1
	.equ	NET_GETADDR,	2
	.equ	NET_SETFILTER,	3
	.equ	NET_LINK,	4
	.equ	NETF_PROMISC,	1
	.equ	NETF_MULTICAST,	2

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
	.equ	G_NPROT,	24		| word
	.equ	G_INRX,		26		| word: receive poll in progress (reentrancy guard)
	.equ	G_VBL,		28		| VBLTask: qLink 4, qType 2, vblAddr 4, vblCount 2, vblPhase 2 = 14 bytes
	.equ	G_VBLSELF,	42		| long: globals pointer, right after the VBL task record
	.equ	G_CURPTR,	46		| long: frame data pointer for ReadPacket/ReadRest
	.equ	G_CURLEFT,	50		| word: bytes left
	.equ	G_NMULTI,	52		| word
	.equ	G_MULTI,	54		| 8 x 6 bytes
	.equ	G_PROT,		104		| 16 x PROT_SIZE
	.equ	PROT_TYPE,	0		| word (0 = 802.3; 0xFFFF = free)
	.equ	PROT_HANDLER,	2		| long (0 = default handler: ERead)
	.equ	PROT_RDQ,	6		| long: first pending ERead PB (linked by qLink)
	.equ	PROT_SIZE,	10
	.equ	NPROT,		16
	.equ	G_TXBUF,	G_PROT+NPROT*PROT_SIZE	| 1536 bytes, gathered frame
	.equ	G_SIZE,		G_TXBUF+1536

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

| ---- helpers ----
window:					| a0 = window base
	lea	WIN32,%a0
	tst.b	MMU32Bit
	bne.s	1f
	lea	WIN24,%a0
1:	rts

| netcmd: d1 = cmd, d2 = arg0, a2 = buf (logical), d3 = len -> d0 = status, d4 = result, d5 = xfer
netcmd:
	movem.l	%a0/%d6,-(%sp)
	bsr.s	window
	lea	CMD_NET(%a0),%a0
	move.w	%d1,8(%a0)
	move.w	#0,10(%a0)
	move.l	%d2,12(%a0)
	move.l	%a2,28(%a0)
	move.l	%d3,32(%a0)
	move.l	(%a0),%d6
	addq.l	#1,%d6
	move.l	%d6,(%a0)
	move.l	#2000000,%d0
1:	cmp.l	4(%a0),%d6
	beq.s	2f
	subq.l	#1,%d0
	bne.s	1b
	moveq	#ioErr,%d0
	bra.s	3f
2:	move.l	36(%a0),%d0
	move.l	40(%a0),%d4
	move.l	44(%a0),%d5
3:	movem.l	(%sp)+,%a0/%d6
	rts

| strip24: d0 = logical address made safe for 24-bit mode
strip24:
	tst.b	MMU32Bit
	bne.s	1f
	and.l	#0x00FFFFFF,%d0
1:	rts

| ---- Open ----
d_open:
	movem.l	%d1-%d7/%a0-%a4,-(%sp)
	move.l	%a1,%a4			| DCE
	tst.l	dCtlStorage(%a4)
	bne	open_ok
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
	cmp.w	#3,OFF_SERVICES+NET_SVC*32(%a0)	| service 2 must be the net service
	bne	open_fail
	| protocol table empty
	lea	G_PROT(%a3),%a1
	moveq	#NPROT-1,%d0
1:	move.w	#0xFFFF,PROT_TYPE(%a1)
	clr.l	PROT_HANDLER(%a1)
	clr.l	PROT_RDQ(%a1)
	lea	PROT_SIZE(%a1),%a1
	dbf	%d0,1b
	| our address
	moveq	#NET_GETADDR,%d1
	moveq	#0,%d2
	suba.l	%a2,%a2
	moveq	#0,%d3
	bsr	netcmd
	tst.l	%d0
	bne	open_fail
	move.l	%d4,G_ADDR(%a3)
	move.w	%d5,G_ADDR+4(%a3)
	| start taking frames where the Pi is now
	bsr	window
	lea	OFF_NETRX(%a0),%a0
	move.l	(%a0),4(%a0)		| tail = head
	| VBL task: every tick
	lea	G_VBL(%a3),%a0
	clr.l	(%a0)
	move.w	#1,4(%a0)		| vType
	lea	rx_vbl(%pc),%a1
	move.l	%a1,6(%a0)
	move.w	#1,10(%a0)
	move.w	#0,12(%a0)
	.short	_VInstall
	tst.w	%d0
	bne	open_fail
open_ok:
	moveq	#noErr,%d0
	bra.s	open_ret
open_mem:
	moveq	#memFullErr,%d0
	bra.s	open_ret
open_fail:
	moveq	#ioErr,%d0
open_ret:
	movem.l	(%sp)+,%d1-%d7/%a0-%a4
	rts

| ---- Close ----
d_close:
	movem.l	%a0/%a3,-(%sp)
	move.l	dCtlStorage(%a1),%d0
	beq.s	1f
	move.l	%d0,%a3
	lea	G_VBL(%a3),%a0
	.short	_VRemove
	move.l	%a3,%a0
	.short	_DisposePtr
	clr.l	dCtlStorage(%a1)
1:	movem.l	(%sp)+,%a0/%a3
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
	movem.l	(%sp)+,%d1-%d7/%a2-%a6
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
	move.w	eProtType(%a2),%d1
	bsr	find_prot
	bne.s	1f
	moveq	#LAPProtErr,%d0		| already attached
	bra.s	ctl_done
1:	move.w	#0xFFFF,%d1
	bsr	find_prot
	bne.s	2f
	moveq	#LAPProtErr,%d0		| table full
	bra.s	ctl_done
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

| EWrite: gather the WDS into our buffer, insert the source address, send
c_write:
	move.l	ePointer(%a2),%d0
	bsr	strip24
	move.l	%d0,%a0			| WDS
	moveq	#0,%d2			| total
	lea	G_TXBUF(%a3),%a1
	move.w	(%a0),%d1
	cmp.w	#14,%d1
	blt	w_len
1:	move.w	(%a0)+,%d1
	beq.s	3f
	move.l	(%a0)+,%d0
	bsr	strip24
	move.l	%d0,%a5
	ext.l	%d1
	add.l	%d1,%d2
	cmp.l	#1514,%d2
	bgt	w_len
	bra.s	2f
4:	move.b	(%a5)+,(%a1)+
2:	dbf	%d1,4b
	bra.s	1b
3:	lea	G_TXBUF(%a3),%a1
	move.l	G_ADDR(%a3),6(%a1)	| source address
	move.w	G_ADDR+4(%a3),10(%a1)
	cmp.l	#60,%d2
	bge.s	5f
	moveq	#60,%d2			| pad short frames (the Pi sends what we give it)
5:	moveq	#NET_SEND,%d1
	move.l	%a1,%a2
	move.l	%d2,%d3
	moveq	#0,%d2
	bsr	netcmd
	tst.l	%d0
	beq	ctl_noerr
	moveq	#excessCollsns,%d0
	bra	ctl_done
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
set_filter:
	movem.l	%d1-%d5/%a2,-(%sp)
	moveq	#NET_SETFILTER,%d1
	moveq	#0,%d2
	tst.w	G_NMULTI(%a3)
	beq.s	1f
	moveq	#NETF_MULTICAST,%d2
1:	suba.l	%a2,%a2
	moveq	#0,%d3
	bsr	netcmd
	movem.l	(%sp)+,%d1-%d5/%a2
	rts

| find_prot: d1 = type -> Z set and a1 -> entry if found
find_prot:
	move.w	%d1,-(%sp)
	lea	G_PROT(%a3),%a1
	moveq	#NPROT-1,%d0
1:	cmp.w	PROT_TYPE(%a1),%d1
	beq.s	2f
	lea	PROT_SIZE(%a1),%a1
	dbf	%d0,1b
	moveq	#1,%d0
	tst.w	%d0
2:	move.w	(%sp)+,%d1
	rts

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
	move.l	G_DCE(%a3),%d2		| A1 = completion routine, A0 = PB, D0 = result (word)
	ext.l	%d0
	jsr	(%a1)
1:	movem.l	(%sp)+,%d1-%d2/%a0-%a1
	rts

| ---- receive: VBL task (A0 = VBLTask record) ----
rx_vbl:
	movem.l	%d0-%d7/%a0-%a6,-(%sp)
	move.l	14(%a0),%a3		| globals (G_VBLSELF follows the task record)
	move.w	#1,G_VBL+10(%a3)	| rearm: vblCount = 1
	tst.w	G_INRX(%a3)
	bne	rx_out
	move.w	#1,G_INRX(%a3)
	move.l	%a3,%a6
	bsr	window
	lea	OFF_NETRX(%a0),%a5	| ring
	moveq	#16,%d7			| frames per tick at most
rx_loop:
	move.l	(%a5),%d0		| head (Pi)
	move.l	4(%a5),%d1		| tail (ours)
	cmp.l	%d0,%d1
	beq	rx_end
	move.l	%d1,%d2
	mulu	#RX_SLOT_SIZE,%d2
	lea	16(%a5,%d2.l),%a2	| slot: len, reserved, data
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
	addq.l	#1,4(%a5)		| tail++
	move.l	4(%a5),%d0
	cmp.l	#RX_SLOTS,%d0
	blt.s	5f
	clr.l	4(%a5)
5:	subq.w	#1,%d7
	bne	rx_loop
rx_end:
	clr.w	G_INRX(%a6)
rx_out:
	movem.l	(%sp)+,%d0-%d7/%a0-%a6
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

| ---- common exit ----
iodone:
	btst	#noQueueBit,ioTrap(%a0)
	beq.s	1f
	rts
1:	move.l	JIODone,-(%sp)
	rts
	.even
drvr_end:
