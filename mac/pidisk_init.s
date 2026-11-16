| pidisk_init.s — INIT that installs the ".PiDisk" driver from its own resource file and mounts
| the Pi's volumes (System 6/7 Extensions). GNU as MIT syntax, 68020. MPL-2.0.
|
| Sequence: _Open ".PiDisk" (done when the declaration-ROM copy is there) -> GetResource('DRVR',128) -> DetachResource -> find a free unit number in the unit
| table -> _DrvrInstall -> _Open ".PiDisk" (Open adds the drive queue entries) -> for every drive
| the driver added, _MountVol so the Finder sees the volume at startup.

	.text
	.globl	init_start
	.equ	UTableBase,	0x011C
	.equ	UnitNtryCnt,	0x01D2
	.equ	DrvQHdr,	0x0308
	.equ	ioCompletion,	12
	.equ	ioResult,	16
	.equ	ioNamePtr,	18
	.equ	ioVRefNum,	22
	.equ	ioRefNum,	24
	.equ	ioPermssn,	27
	.equ	_GetResource,	0xA9A0
	.equ	_DetachResource,0xA992
	.equ	_HLock,		0xA029
	.equ	_DrvrInstall,	0xA03D
	.equ	_Open,		0xA000
	.equ	_MountVol,	0xA00F
	.equ	_SysBeep,	0xA9C8

| ---- log through the Pi's INFO service: `log "text"` (trashes nothing; spins briefly) ----
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


init_start:
	movem.l	%d1-%d7/%a0-%a5,-(%sp)
	link	%a6,#-64		| ParamBlock on the stack
	log	"PiDisk INIT: start" 
	| Since the slot-$9 declaration ROM carries the same driver and the Start Manager opens it
	| before any extension runs, this file only matters on a Mac booted without the card's ROM
	| (a kernel started without mac.declrom=). If _Open ".PiDisk" by name already succeeds, the
	| ROM's copy is in the unit table with its drives and there is nothing left to do; a second
	| copy would add the same volumes twice.
	lea	-64(%a6),%a0
	move.l	%a0,%a2
	moveq	#15,%d0
0:	clr.l	(%a0)+
	dbf	%d0,0b
	lea	name(%pc),%a1
	move.l	%a1,ioNamePtr(%a2)
	move.b	#0,ioPermssn(%a2)
	move.l	%a2,%a0
	.short	_Open
	tst.w	%d0
	bne.s	install
	log	"PiDisk INIT: the ROM's driver is already open; mounting its drives"
	bra	mount			| System 7's Finder does not mount a drive from the card's own disk-inserted event
install:
	| our DRVR resource
	subq.l	#4,%sp
	move.l	#0x44525652,-(%sp)	| 'DRVR'
	move.w	#128,-(%sp)
	.short	_GetResource
	move.l	(%sp)+,%d7
	beq	done
	move.l	%d7,%a0
	.short	_HLock
	move.l	%d7,-(%sp)
	.short	_DetachResource
	| free unit: scan the unit table from entry 48 upwards (avoid the ROM/slot range < 48)
	move.l	UTableBase,%a1
	moveq	#48,%d6
1:	cmp.w	UnitNtryCnt,%d6
	bge	done			| table full
	tst.l	(%a1,%d6.w*4)
	beq.s	2f
	addq.w	#1,%d6
	bra.s	1b
2:	| refNum = -(unit+1)
	move.w	%d6,%d0
	addq.w	#1,%d0
	neg.w	%d0
	move.w	%d0,%d5			| refNum
	move.l	%d7,%a0
	move.l	(%a0),%a0		| DRVR pointer (handle dereferenced; locked)
	.short	_DrvrInstall
	tst.w	%d0
	bne	done
	log	"PiDisk INIT: driver installed" 
	| open it: PB.ioNamePtr = ".PiDisk"
	lea	-64(%a6),%a0
	move.l	%a0,%a2
	moveq	#15,%d0
3:	clr.l	(%a0)+
	dbf	%d0,3b
	lea	name(%pc),%a1
	move.l	%a1,ioNamePtr(%a2)
	move.b	#0,ioPermssn(%a2)
	move.l	%a2,%a0
	.short	_Open
	tst.w	%d0
	bne	done
	log	"PiDisk INIT: open ok" 
	| mount every drive that belongs to this refNum
mount:
	move.w	ioRefNum(%a2),%d5
	move.l	DrvQHdr+2,%a3		| qHead
4:	move.l	%a3,%d0
	beq	done
	cmp.w	8(%a3),%d5		| dQRefNum
	bne.s	5f
	lea	-64(%a6),%a0
	moveq	#15,%d0
6:	clr.l	(%a0)+
	dbf	%d0,6b
	move.w	6(%a3),ioVRefNum(%a2)	| dQDrive
	move.l	%a2,%a0
	.short	_MountVol
	log	"PiDisk INIT: mountvol done"
5:	move.l	(%a3),%a3
	bra.s	4b
done:
	log	"PiDisk INIT: end"
	unlk	%a6
	movem.l	(%sp)+,%d1-%d7/%a0-%a5
	rts

| do_log: a1 = C string -> INFO command (block 0): cmd 2, buf, len
do_log:
	lea	0xF9000000,%a0
	tst.b	0x0CB2
	bne.s	1f
	lea	0x00900000,%a0
1:	lea	0x1000(%a0),%a0		| INFO command block
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
name:	.byte	7
	.ascii	".PiDisk"
	.even
