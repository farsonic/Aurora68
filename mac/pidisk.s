| pidisk.s — ".PiDisk" block device driver for the PiStorm-SE mailbox (68k, GNU as MIT syntax). MPL-2.0.
|
| One DRVR serves every block device the Pi publishes in the slot-$9 window (psmb.h): Open adds a
| drive queue entry per device, Prime reads/writes 512-byte blocks through the BLOCK service's
| command slot (synchronous: post, spin until done_seq == req_seq), Control/Status answer the
| calls the File Manager and Finder make for a fixed hard disk.
|
| Window address: 0x900000 while the MMU is in 24-bit mode (lowmem MMU32Bit $CB2 == 0), else
| 0xF9000000. Buffers are handed to the Pi as 68k logical addresses (the Pi translates).
|
| Assemble: m68k-linux-gnu-as -m68020 pidisk.s -o pidisk.o && objcopy -O binary -j .text pidisk.o pidisk.bin

	.text
	.globl	drvr_start

| ---- psmb.h constants ----
	.equ	WIN24,		0x00900000
	.equ	WIN32,		0xF9000000
	.equ	PSMB_MAGIC,	0x50534D42
	.equ	OFF_CMD,	0x1000		| service command blocks, 64 bytes each; block service = index 1
	.equ	OFF_BLKDEV,	0x2000		| 4 x 64-byte device descriptors
	.equ	CMD_BLOCK,	OFF_CMD+64
	.equ	C_REQ,		0
	.equ	C_DONE,		4
	.equ	C_CMD,		8
	.equ	C_FLAGS,	10
	.equ	C_ARG0,		12
	.equ	C_ARG1,		16
	.equ	C_ARG2,		20
	.equ	C_BUF,		28
	.equ	C_LEN,		32
	.equ	C_STATUS,	36
	.equ	C_RESULT,	40
	.equ	C_XFER,		44
	.equ	BLK_READ,	1
	.equ	BLK_WRITE,	2
	.equ	BLK_FLUSH,	3
	.equ	BLK_INFO,	4
	.equ	BD_BLOCKS,	0
	.equ	BD_FLAGS,	8
	.equ	BD_WRITABLE,	2
	.equ	MAXDEV,		4

| ---- Mac OS ----
	.equ	MMU32Bit,	0x0CB2
	.equ	JIODone,	0x08FC
	.equ	ioTrap,		6
	.equ	ioResult,	16
	.equ	ioVRefNum,	22
	.equ	csCode,		26
	.equ	csParam,	28
	.equ	ioBuffer,	32
	.equ	ioReqCount,	36
	.equ	ioActCount,	40
	.equ	ioPosMode,	44
	.equ	ioPosOffset,	46
	.equ	dCtlStorage,	20
	.equ	dCtlPosition,	16
	.equ	dCtlRefNum,	24
	.equ	noQueueBit,	9
	.equ	noErr,		0
	.equ	controlErr,	-17
	.equ	statusErr,	-18
	.equ	ioErr,		-36
	.equ	paramErr,	-50
	.equ	nsDrvErr,	-56
	.equ	wPrErr,		-44
	.equ	_NewPtrSysClear,	0xA71E
	.equ	_AddDrive,	0xA04E

| driver globals (dCtlStorage -> pointer, allocated with NewPtrSysClear)
	.equ	G_WIN,		0		| window base currently used (long)
	.equ	G_NDEV,		4		| devices published (word)
	.equ	G_FIRSTDRV,	6		| first drive number (word)
	.equ	G_DQE,		8		| 4 x (4 flag bytes + 18-byte DrvQEl) = 4 x 22 -> use 24 each
	.equ	DQE_SIZE,	24
	.equ	G_VBL,		G_DQE+MAXDEV*DQE_SIZE	| VBLTask (14 bytes) + globals pointer (4)
	.equ	G_VBLSELF,	G_VBL+14
	.equ	G_POSTS,	G_VBLSELF+4	| word: disk events posted so far
	.equ	G_PRIMED,	G_VBLSELF+6	| word: nonzero once the File Manager has read from us
	.equ	G_SIZE,		G_VBLSELF+8
	.equ	_VInstall,	0xA033
	.equ	_VRemove,	0xA034

| ---- DRVR header ----
drvr_start:
	.short	0x4F00			| dNeedLock|dStatEnable|dCtlEnable|dWritEnable|dReadEnable
	.short	0			| drvrDelay
	.short	0			| drvrEMask
	.short	0			| drvrMenu
	.short	d_open-drvr_start
	.short	d_prime-drvr_start
	.short	d_control-drvr_start
	.short	d_status-drvr_start
	.short	d_close-drvr_start
	.byte	7
	.ascii	".PiDisk"
	.even


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

| ---- helpers ----
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

| window(): a0 = window base according to the MMU mode
window:
	lea	WIN32,%a0
	tst.b	MMU32Bit
	bne.s	1f
	lea	WIN24,%a0
1:	rts

| blkcmd: d1 = cmd, d2 = device, d3 = first block, d4 = block count, a2 = buffer -> d0 = status (0 ok)
blkcmd:
	movem.l	%a0/%d5,-(%sp)
	bsr.s	window
	lea	CMD_BLOCK(%a0),%a0
	move.w	%d1,C_CMD(%a0)
	move.w	#0,C_FLAGS(%a0)
	move.l	%d2,C_ARG0(%a0)
	move.l	%d3,C_ARG1(%a0)
	move.l	%d4,C_ARG2(%a0)
	move.l	%a2,C_BUF(%a0)
	move.l	%d4,%d5
	lsl.l	#8,%d5
	add.l	%d5,%d5			| count * 512
	move.l	%d5,C_LEN(%a0)
	move.l	C_REQ(%a0),%d5
	addq.l	#1,%d5
	move.l	%d5,C_REQ(%a0)		| post
	move.l	#2000000,%d0		| spin limit (service core answers in microseconds)
2:	cmp.l	C_DONE(%a0),%d5
	beq.s	3f
	subq.l	#1,%d0
	bne.s	2b
	moveq	#ioErr,%d0
	bra.s	4f
3:	move.l	C_STATUS(%a0),%d0
	beq.s	4f
	moveq	#ioErr,%d0
4:	movem.l	(%sp)+,%a0/%d5
	rts

| ---- Open: allocate globals, publish drives ----
d_open:
	movem.l	%d1-%d7/%a0-%a4,-(%sp)
	move.l	%a1,%a4			| DCE
	move.l	%a0,%a3			| PB
	log	"PiDisk: open"
	tst.l	dCtlStorage(%a4)
	bne	open_ok			| already open
	move.l	#G_SIZE,%d0
	.short	_NewPtrSysClear
	move.l	%a0,%d0
	beq	open_fail
	move.l	%a0,dCtlStorage(%a4)
	move.l	%a0,%a2			| globals
	bsr	window
	move.l	%a0,G_WIN(%a2)
	cmp.l	#PSMB_MAGIC,(%a0)
	bne	open_fail		| no Pi
	log	"PiDisk: window ok" 
	| refresh the device table: INFO command (ignores errors)
	moveq	#BLK_INFO,%d1
	moveq	#0,%d2
	moveq	#0,%d3
	moveq	#0,%d4
	suba.l	%a2,%a2
	move.l	dCtlStorage(%a4),%a2
	movem.l	%a2,-(%sp)
	suba.l	%a2,%a2
	bsr	blkcmd
	movem.l	(%sp)+,%a2
	| walk the descriptors
	bsr	window
	lea	OFF_BLKDEV(%a0),%a0
	moveq	#0,%d5			| device index
	moveq	#0,%d6			| drives added
	move.w	#5,%d7			| first drive number candidate (1..4 are floppies/SCSI ids are fine? use 5+)
	bsr	free_drive_number
open_loop:
	move.l	BD_BLOCKS(%a0),%d0
	beq	open_next
	| build flags + DrvQEl in globals
	move.l	%d5,%d1
	mulu	#DQE_SIZE,%d1
	lea	G_DQE(%a2,%d1.l),%a1
	move.b	#8,(%a1)		| disk in place, nonejectable (-8? 8 = nonejectable)
	move.b	#0,1(%a1)		| don't verify
	move.b	#0,2(%a1)
	move.b	#0,3(%a1)
	tst.b	BD_FLAGS+3(%a0)		| writable?
	btst	#1,BD_FLAGS+3(%a0)
	bne.s	1f
	move.b	#0x80,3(%a1)		| locked? (bit 7 of byte 3 is not standard; locking handled via wPrErr)
1:	lea	4(%a1),%a1		| DrvQEl
	clr.l	0(%a1)			| qLink
	move.w	#1,4(%a1)		| qType 1: dQDrvSz2 holds the high word
	move.w	%d7,6(%a1)		| dQDrive
	move.w	dCtlRefNum(%a4),8(%a1)	| dQRefNum
	move.w	#0,10(%a1)		| dQFSID
	move.w	%d0,12(%a1)		| dQDrvSz (low)
	swap	%d0
	move.w	%d0,14(%a1)		| dQDrvSz2 (high)
	| _AddDrive: a0 = DrvQEl, d0 = refNum<<16 | drive
	movem.l	%a0,-(%sp)
	move.l	%a1,%a0
	move.w	dCtlRefNum(%a4),%d0
	swap	%d0
	move.w	%d7,%d0
	.short	_AddDrive
	movem.l	(%sp)+,%a0
	log	"PiDisk: drive added" 
	tst.w	%d6
	bne.s	2f
	move.w	%d7,G_FIRSTDRV(%a2)
2:	addq.w	#1,%d6
	addq.w	#1,%d7
	bsr	free_drive_number
open_next:
	lea	64(%a0),%a0
	addq.w	#1,%d5
	cmp.w	#MAXDEV,%d5
	blt	open_loop
	move.w	%d6,G_NDEV(%a2)
	tst.w	%d6
	beq.s	open_ok
	| the volumes get mounted by the Finder when it sees a disk-inserted event: post them from a
	| VBL task a few seconds from now (Open runs before the Event Manager exists)
	move.l	%a2,G_VBLSELF(%a2)
	lea	G_VBL(%a2),%a0
	clr.l	(%a0)
	move.w	#1,4(%a0)		| vType
	lea	mount_vbl(%pc),%a1
	move.l	%a1,6(%a0)
	move.w	#300,10(%a0)		| 5 s
	move.w	#0,12(%a0)
	.short	_VInstall
open_ok:
	moveq	#noErr,%d0
	bra.s	open_ret
open_fail:
	moveq	#ioErr,%d0
open_ret:
	movem.l	(%sp)+,%d1-%d7/%a0-%a4
	rts

| free_drive_number: advance d7 past drive numbers already in the drive queue (DrvQHdr $308)
free_drive_number:
	movem.l	%d0/%a1,-(%sp)
3:	move.l	0x308+2,%a1		| DrvQHdr.qHead
4:	move.l	%a1,%d0
	beq.s	5f			| end of queue: d7 is free
	cmp.w	6(%a1),%d7		| dQDrive
	bne.s	6f
	addq.w	#1,%d7
	bra.s	3b
6:	move.l	(%a1),%a1
	bra.s	4b
5:	movem.l	(%sp)+,%d0/%a1
	rts

| ---- Prime: read / write ----
d_prime:
	movem.l	%d1-%d5/%a2-%a4,-(%sp)
	move.l	%a0,%a3			| PB
	move.l	%a1,%a4			| DCE
	move.l	dCtlStorage(%a4),%a2
	move.l	%a2,%d0
	beq	prime_err
	| device index from drive number
	move.w	ioVRefNum(%a3),%d2
	sub.w	G_FIRSTDRV(%a2),%d2
	blt	prime_nodrv
	cmp.w	G_NDEV(%a2),%d2
	bge	prime_nodrv
	ext.l	%d2
	| block = dCtlPosition / 512, count = ioReqCount / 512
	move.l	dCtlPosition(%a4),%d3
	move.l	%d3,%d0
	and.l	#0x1FF,%d0
	bne	prime_param
	lsr.l	#8,%d3
	lsr.l	#1,%d3
	move.l	ioReqCount(%a3),%d4
	move.l	%d4,%d0
	and.l	#0x1FF,%d0
	bne	prime_param
	lsr.l	#8,%d4
	lsr.l	#1,%d4
	move.l	ioBuffer(%a3),%d0
	tst.b	MMU32Bit
	bne.s	0f
	and.l	#0x00FFFFFF,%d0		| 24-bit mode: drop the flag bits in the high byte
0:	move.l	%d0,%a2
	moveq	#BLK_READ,%d1
	move.w	ioTrap(%a3),%d0
	and.w	#0xFF,%d0
	cmp.w	#3,%d0			| aWrCmd
	bne.s	1f
	moveq	#BLK_WRITE,%d1
1:	bsr	blkcmd
	tst.l	%d0
	bne.s	prime_io
	move.l	ioReqCount(%a3),%d0
	move.l	%d0,ioActCount(%a3)
	add.l	%d0,dCtlPosition(%a4)
	move.l	dCtlStorage(%a4),%a2
	move.w	#1,G_PRIMED(%a2)
	moveq	#noErr,%d0
	bra.s	prime_done
prime_io:
	moveq	#ioErr,%d0
	bra.s	prime_done
prime_param:
	moveq	#paramErr,%d0
	bra.s	prime_done
prime_nodrv:
	moveq	#nsDrvErr,%d0
	bra.s	prime_done
prime_err:
	moveq	#ioErr,%d0
prime_done:
	movem.l	(%sp)+,%d1-%d5/%a2-%a4
	bra	iodone

| ---- Control ----
d_control:
	move.w	csCode(%a0),%d0
	cmp.w	#1,%d0			| KillIO
	beq.s	ctl_ok
	cmp.w	#5,%d0			| verify
	beq.s	ctl_ok
	cmp.w	#6,%d0			| format
	beq.s	ctl_ok
	cmp.w	#7,%d0			| eject: nothing to do, keep the drive
	beq.s	ctl_ok
	cmp.w	#21,%d0			| physical drive icon
	beq.s	ctl_icon
	cmp.w	#22,%d0			| media icon
	beq.s	ctl_icon
	cmp.w	#23,%d0			| drive info: 0x00000601 = fixed, SCSI-like, primary? use external fixed
	beq.s	ctl_info
	moveq	#controlErr,%d0
	bra	iodone
ctl_icon:
	lea	icon(%pc),%a1
	move.l	%a1,csParam(%a0)
	bra.s	ctl_ok
ctl_info:
	move.l	#0x00000401,csParam(%a0)	| drive type 1 (fixed? ) bit 10 external? keep simple
ctl_ok:
	moveq	#noErr,%d0
	bra	iodone

| ---- Status ----
d_status:
	move.w	csCode(%a0),%d0
	cmp.w	#8,%d0			| drive status
	bne.s	st_err
	| return the 22-byte "DrvSts" block: track, writeProt, diskInPlace, installed, sides, qLink..., we copy flags+DrvQEl
	movem.l	%a2/%d1,-(%sp)
	move.l	dCtlStorage(%a1),%a2
	move.w	ioVRefNum(%a0),%d1
	sub.w	G_FIRSTDRV(%a2),%d1
	mulu	#DQE_SIZE,%d1
	lea	G_DQE(%a2,%d1.l),%a2
	lea	csParam(%a0),%a1
	move.w	#0,(%a1)+		| track
	move.b	#0,(%a1)+		| writeProt (0x80 = locked)
	move.b	#8,(%a1)+		| diskInPlace: 8 = nonejectable
	move.b	#1,(%a1)+		| installed
	move.b	#0,(%a1)+		| sides
	move.l	4(%a2),(%a1)+		| qLink
	move.w	8(%a2),(%a1)+		| qType
	move.w	10(%a2),(%a1)+		| dQDrive
	move.w	12(%a2),(%a1)+		| dQRefNum
	move.w	14(%a2),(%a1)+		| dQFSID
	move.b	#0,(%a1)+		| twoSideFmt
	move.b	#0,(%a1)+		| needsFlush
	move.w	#0,(%a1)+		| diskErrs
	movem.l	(%sp)+,%a2/%d1
	moveq	#noErr,%d0
	bra.s	iodone
st_err:
	moveq	#statusErr,%d0
	bra.s	iodone

| ---- Close ----
d_close:
	moveq	#noErr,%d0
	rts

| ---- VBL task (every 5 s): post one diskEvt per drive until the File Manager has read from us
|      (events posted before the Finder runs are flushed at startup), at most 24 times ----
mount_vbl:
	movem.l	%d0-%d2/%a0-%a2,-(%sp)
	move.l	14(%a0),%a2		| globals
	tst.w	G_PRIMED(%a2)
	bne.s	3f
	addq.w	#1,G_POSTS(%a2)
	cmp.w	#24,G_POSTS(%a2)
	bgt.s	3f
	move.w	G_NDEV(%a2),%d2
	beq.s	2f
	move.w	G_FIRSTDRV(%a2),%d1
	subq.w	#1,%d2
1:	moveq	#7,%d0
	move.l	%d0,%a0			| diskEvt
	moveq	#0,%d0
	move.w	%d1,%d0
	.short	0xA02F			| _PostEvent
	addq.w	#1,%d1
	dbf	%d2,1b
2:	move.w	#300,G_VBL+10(%a2)	| rearm
	bra.s	4f
3:	lea	G_VBL(%a2),%a0
	.short	_VRemove
4:	movem.l	(%sp)+,%d0-%d2/%a0-%a2
	rts

| ---- common exit: immediate calls return, queued calls go through IODone ----
iodone:
	btst	#noQueueBit,ioTrap(%a0)
	beq.s	1f
	rts
1:	move.l	JIODone,-(%sp)
	rts

| ---- a 32x32 1-bit icon + mask (ICN#) for the Finder: a small box with "Pi" ----
	.even
icon:
	.long	0x00000000,0x7FFFFFFE,0x40000002,0x5FFFFFFA,0x50000012,0x53C78012,0x52248012,0x52248012
	.long	0x53C68012,0x52048012,0x52040012,0x52078012,0x50000012,0x5FFFFFFA,0x40000002,0x40000002
	.long	0x4FFFFFF2,0x48000012,0x48000012,0x48000012,0x48000012,0x4FFFFFF2,0x40000002,0x40000002
	.long	0x40000002,0x40000002,0x7FFFFFFE,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000
	.long	0x00000000,0x7FFFFFFE,0x7FFFFFFE,0x7FFFFFFE,0x7FFFFFFE,0x7FFFFFFE,0x7FFFFFFE,0x7FFFFFFE
	.long	0x7FFFFFFE,0x7FFFFFFE,0x7FFFFFFE,0x7FFFFFFE,0x7FFFFFFE,0x7FFFFFFE,0x7FFFFFFE,0x7FFFFFFE
	.long	0x7FFFFFFE,0x7FFFFFFE,0x7FFFFFFE,0x7FFFFFFE,0x7FFFFFFE,0x7FFFFFFE,0x7FFFFFFE,0x7FFFFFFE
	.long	0x7FFFFFFE,0x7FFFFFFE,0x7FFFFFFE,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000
	.byte	6
	.ascii	"PiDisk"
	.even
drvr_end:
