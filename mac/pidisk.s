| pidisk.s — ".PiDisk" block device driver for the Aurora68 mailbox (68k, GNU as MIT syntax). MPL-2.0.
|
| One DRVR serves every block device the Pi publishes in the slot-$9 window (psmb.h): Open adds a
| drive queue entry per device, Prime reads/writes 512-byte blocks through the BLOCK service's
| command slot (synchronous: post, spin until done_seq == req_seq), Control/Status answer the
| calls the File Manager and Finder make for a fixed hard disk.
|
| Removable units (20 November 2026): a USB flash drive plugged into the Pi is a PiDisk unit too
| (emu68/src/psmac/psmac_usbdisk.c), one that comes and goes while the Mac runs. The Pi marks such
| a unit PSMB_BD_REMOVABLE, clears PSMB_BD_PRESENT while no disk is in it and moves the unit's
| media count for every disk put in. A VBL task, every two seconds, does what the Sony driver does
| for a floppy: a new disk gets its drive (added to the drive queue the first time) and a
| disk-inserted event, which has the Event Manager mount it; a disk taken out leaves its drive
| empty (disk-in-place 0), and Prime then answers offLinErr, so the File Manager puts the volume
| offline and asks for the disk back, as for a floppy pulled out with a paper clip. Its drive says
| "ejectable disk" (disk-in-place 1), so the Finder ejects it: Control's eject sends the Pi
| PSMB_BLK_EJECT, which flushes the stick and releases it. A USB disk's command can take longer
| than a card image's (a slow stick, a bus reset): the Pi moves the command block's progress word
| while it works, and the spin goes on for as long as that moves.
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
	.equ	C_PROG,		48		| the Pi's progress word while a command takes long
	.equ	BLK_READ,	1
	.equ	BLK_WRITE,	2
	.equ	BLK_FLUSH,	3
	.equ	BLK_INFO,	4
	.equ	BLK_EJECT,	5
	.equ	BD_BLOCKS,	0
	.equ	BD_FLAGS,	8
	.equ	BD_MEDIA,	12
	.equ	BD_PRESENT,	0		| bit numbers in the flags' low byte
	.equ	BD_WRITABLE,	1
	.equ	BD_REMOVABLE,	2
	.equ	E_RO,		-6		| PSMB_E_RO: a write to a write-protected disk
	.equ	E_NODATA,	-7		| PSMB_E_NODATA: no disk in the unit
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
	.equ	wPrErr,		-44
	.equ	paramErr,	-50
	.equ	nsDrvErr,	-56
	.equ	offLinErr,	-65
	.equ	_NewPtrSysClear,	0xA71E
	.equ	_AddDrive,	0xA04E

| One record per device in the globals. The four flag bytes come first because the File Manager and
| the Finder read them just before a drive queue element: byte 0 bit 7 locked, byte 1 the disk in
| place (0 none, 1 an ejectable disk, 8 a fixed one).
	.equ	D_FLAGS,	0
	.equ	D_DQE,		4		| DrvQEl, 18 bytes: qLink, qType +4, dQDrive +6, dQRefNum +8, dQFSID +10, dQDrvSz +12, dQDrvSz2 +14
	.equ	D_ADDED,	22		| word: the drive is in the drive queue
	.equ	D_MEDIA,	24		| long: the Pi's media count for the unit, when last looked at
	.equ	D_POSTS,	28		| word: disk-inserted events still to post
	.equ	D_READ,		30		| word: read or written since the disk went in
	.equ	D_REMOV,	32		| word: an ejectable unit (a USB stick)
	.equ	DEV_SIZE,	36
| driver globals (dCtlStorage -> pointer, allocated with NewPtrSysClear)
	.equ	G_WIN,		0		| window base at Open (long)
	.equ	G_REFNUM,	4		| the driver's reference number (word), for drives added later
	.equ	G_DEV,		8		| MAXDEV device records
	.equ	G_VBL,		G_DEV+MAXDEV*DEV_SIZE	| VBLTask (14 bytes) + globals pointer (4)
	.equ	G_VBLSELF,	G_VBL+14
	.equ	G_LOGS,		G_VBLSELF+4	| word: Prime, Control and Status lines still to log after Open
	.equ	G_ERRS,		G_LOGS+2	| word: Prime error lines still to log
	.equ	G_TEXT,		G_ERRS+2	| 32 bytes: the text of an error line
	.equ	G_SIZE,		G_TEXT+32
	.equ	FIRST_LOGS,	6		| the first requests after Open, to show the driver at work
	.equ	ERR_LOGS,	16		| a disk that fails fails many times: the first sixteen say how
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


| ---- log through the Pi's INFO service: `log "text"` (trashes nothing; spins briefly).
|      The macro uses local labels 8 and 9: never branch to 8f/9f across a log. ----
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

| `log_first "text"`: the line only for the first FIRST_LOGS requests after Open (a1 = the DCE).
| Until 21 November 2026 Prime logged every read and write: 1,300 of the 1,790 lines in a user's
| 64 KB log, which pushed everything else out, and an INFO command (a round trip to the Pi's
| console) for every disk I/O (docs/08 item 78). Uses label 7 besides the log macro's 8 and 9.
	.macro	log_first text
	bsr	log_left
	beq.s	7f
	log	"\text"
7:
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

| log_left: Z clear (and one line fewer to come) while the first requests after Open are logged,
| Z set after that or before Open made the globals; a1 = the DCE, every register kept
log_left:
	movem.l	%d0/%a0,-(%sp)
	move.l	dCtlStorage(%a1),%d0
	beq.s	1f			| no globals: Z set
	move.l	%d0,%a0
	tst.w	G_LOGS(%a0)
	beq.s	1f			| none left: Z set
	subq.w	#1,G_LOGS(%a0)
	moveq	#1,%d0			| Z clear
1:	movem.l	(%sp)+,%d0/%a0		| (movem to registers leaves the condition codes alone)
	rts

| log_prime_err: "PiDisk: prime error -NNN" for d0.w = the result, the first ERR_LOGS times; a1 = the
| DCE, every register kept. The text is put together in the globals (G_TEXT).
log_prime_err:
	movem.l	%d0-%d1/%a0-%a2,-(%sp)
	move.l	dCtlStorage(%a1),%d1
	beq.s	9f
	move.l	%d1,%a2
	tst.w	G_ERRS(%a2)
	beq.s	9f
	subq.w	#1,G_ERRS(%a2)
	lea	G_TEXT(%a2),%a0
	lea	prime_err_text(%pc),%a1
1:	move.b	(%a1)+,(%a0)+
	bne.s	1b
	subq.l	#1,%a0			| onto the NUL
	moveq	#0,%d1
	move.w	%d0,%d1
	neg.w	%d1			| a Device Manager error, -1 .. -999
	divu	#100,%d1		| hundreds in the low word, the rest in the high
	add.b	#'0',%d1
	move.b	%d1,(%a0)+
	clr.w	%d1
	swap	%d1
	divu	#10,%d1
	add.b	#'0',%d1
	move.b	%d1,(%a0)+
	swap	%d1
	add.b	#'0',%d1
	move.b	%d1,(%a0)+
	clr.b	(%a0)
	lea	G_TEXT(%a2),%a1
	bsr	do_log
9:	movem.l	(%sp)+,%d0-%d1/%a0-%a2
	rts
prime_err_text:
	.asciz	"PiDisk: prime error -"
	.even

| window(): a0 = window base according to the MMU mode
window:
	lea	WIN32,%a0
	tst.b	MMU32Bit
	bne.s	1f
	lea	WIN24,%a0
1:	rts

| blkcmd: d1 = cmd, d2 = device, d3 = first block, d4 = block count, a2 = buffer
|   -> d0 = 0 done, the Pi's status (PSMB_E_*, -1..-8) when it refused, ioErr when it did not answer
blkcmd:
	movem.l	%a0/%d5-%d7,-(%sp)
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
	move.l	C_PROG(%a0),%d6		| the Pi's progress word as we post
	move.l	C_REQ(%a0),%d5
	addq.l	#1,%d5
	move.l	%d5,C_REQ(%a0)		| post
1:	move.l	#100000000,%d0		| spin limit: core 3 answers in microseconds, but a block of an image
					| still coming in from the card, or the WiFi's turn, can take tens of
					| milliseconds; 2,000,000 turns (2-5 ms at JIT speed) gave I/O errors
					| and "This disk is unreadable" (docs/08 item 52)
2:	cmp.l	C_DONE(%a0),%d5
	beq.s	3f
	subq.l	#1,%d0
	bne.s	2b
	| the spin is over: wait on while the Pi's progress word moves (a USB disk's command, a stick that
	| is slow or a bus being enumerated again); the Pi bounds every command, so this ends
	move.l	C_PROG(%a0),%d7
	cmp.l	%d6,%d7
	beq.s	5f
	move.l	%d7,%d6
	bra.s	1b
5:	moveq	#ioErr,%d0
	bra.s	4f
3:	move.l	C_STATUS(%a0),%d0
4:	movem.l	(%sp)+,%a0/%d5-%d7
	rts

| dev_ptrs: d5.w = device index, a2 = globals -> a0 = its descriptor in the window, a3 = its record
dev_ptrs:
	bsr.s	window
	moveq	#0,%d0
	move.w	%d5,%d0
	lsl.l	#6,%d0
	add.l	%d0,%a0
	lea	OFF_BLKDEV(%a0),%a0
	move.w	%d5,%d0
	mulu	#DEV_SIZE,%d0
	lea	G_DEV(%a2,%d0.l),%a3
	rts

| find_dev: d0.w = drive number, a2 = globals -> d2.l = device index and a3 = its record, d2 = -1 if
| the drive is not ours
find_dev:
	moveq	#0,%d2
	lea	G_DEV(%a2),%a3
1:	tst.w	D_ADDED(%a3)
	beq.s	2f
	cmp.w	D_DQE+6(%a3),%d0	| dQDrive
	beq.s	3f
2:	lea	DEV_SIZE(%a3),%a3
	addq.l	#1,%d2
	cmp.l	#MAXDEV,%d2
	blt.s	1b
	moveq	#-1,%d2
3:	rts

| insert: a disk is in the unit. a0 = its descriptor, a3 = its record, a2 = globals. The record's
| flags and size from the descriptor; the drive into the drive queue the first time (_AddDrive only
| links the element into the queue, so the VBL task may call it too).
insert:
	movem.l	%d0-%d2/%d7/%a0-%a1,-(%sp)
	move.b	#0,D_FLAGS(%a3)
	btst	#BD_WRITABLE,BD_FLAGS+3(%a0)
	bne.s	1f
	move.b	#0x80,D_FLAGS(%a3)	| read-only: locked
1:	move.b	#8,D_FLAGS+1(%a3)	| a fixed disk in place
	tst.w	D_REMOV(%a3)
	beq.s	2f
	move.b	#1,D_FLAGS+1(%a3)	| an ejectable disk in place
2:	clr.w	D_FLAGS+2(%a3)
	move.l	BD_BLOCKS(%a0),%d0
	move.w	%d0,D_DQE+12(%a3)	| dQDrvSz (low)
	swap	%d0
	move.w	%d0,D_DQE+14(%a3)	| dQDrvSz2 (high)
	move.w	#1,D_DQE+4(%a3)		| qType 1: dQDrvSz2 holds the high word
	clr.w	D_DQE+10(%a3)		| dQFSID
	clr.w	D_READ(%a3)
	tst.w	D_ADDED(%a3)
	bne.s	3f
	moveq	#5,%d7			| first drive number candidate (1..4: floppies and the ROM's own)
	bsr	free_drive_number
	clr.l	D_DQE(%a3)		| qLink
	move.w	%d7,D_DQE+6(%a3)	| dQDrive
	move.w	G_REFNUM(%a2),D_DQE+8(%a3)	| dQRefNum
	| _AddDrive: a0 = DrvQEl, d0 = drive<<16 | refNum (it fills dQDrive/dQRefNum from d0)
	lea	D_DQE(%a3),%a0
	move.w	%d7,%d0
	swap	%d0
	move.w	G_REFNUM(%a2),%d0
	.short	_AddDrive
	move.w	#1,D_ADDED(%a3)
3:	movem.l	(%sp)+,%d0-%d2/%d7/%a0-%a1
	rts

| removable: the unit's ejectable flag from its descriptor (a0) into its record (a3)
removable:
	clr.w	D_REMOV(%a3)
	btst	#BD_REMOVABLE,BD_FLAGS+3(%a0)
	beq.s	1f
	move.w	#1,D_REMOV(%a3)
1:	rts

| ---- Open: allocate globals, publish drives ----
d_open:
	movem.l	%d1-%d7/%a0-%a4,-(%sp)
	move.l	%a1,%a4			| DCE
	log	"PiDisk: open"
	tst.l	dCtlStorage(%a4)
	bne	open_ok			| already open
	move.l	#G_SIZE,%d0
	.short	_NewPtrSysClear
	move.l	%a0,%d0
	beq	open_fail
	move.l	%a0,dCtlStorage(%a4)
	move.l	%a0,%a2			| globals
	move.w	dCtlRefNum(%a4),G_REFNUM(%a2)
	move.w	#FIRST_LOGS,G_LOGS(%a2)
	move.w	#ERR_LOGS,G_ERRS(%a2)
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
	movem.l	%a2,-(%sp)
	suba.l	%a2,%a2
	bsr	blkcmd
	movem.l	(%sp)+,%a2
	| a drive for every unit with a disk in it now; the VBL task adds the others when theirs comes
	moveq	#0,%d5			| device index
open_loop:
	bsr	dev_ptrs
	move.l	BD_MEDIA(%a0),D_MEDIA(%a3)
	bsr	removable
	btst	#BD_PRESENT,BD_FLAGS+3(%a0)
	beq.s	open_next
	tst.l	BD_BLOCKS(%a0)
	beq.s	open_next
	bsr	insert
	move.w	#200,D_POSTS(%a3)	| posted every 2 s until the File Manager reads the drive, at most 200 times
	log	"PiDisk: drive added" 
open_next:
	addq.w	#1,%d5
	cmp.w	#MAXDEV,%d5
	blt.s	open_loop
	| The volumes get mounted when the Event Manager takes a disk-inserted event: post them from a
	| VBL task a few seconds from now (Open runs before the Event Manager exists), and keep the task
	| for removable disks put in and taken out later
	move.l	%a2,G_VBLSELF(%a2)
	lea	G_VBL(%a2),%a0
	clr.l	(%a0)
	move.w	#1,4(%a0)		| vType
	lea	disk_vbl(%pc),%a1
	move.l	%a1,6(%a0)
	move.w	#300,10(%a0)		| first look 5 s after Open, then every 2 s: an event posted before the
					| Finder runs is flushed and posted again, and a fast boot (docs/08 item
					| 50) no longer waits a minute for the volume
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
	log_first "PiDisk: prime (logged for the first requests after open only)"
	movem.l	%d1-%d7/%a0-%a4,-(%sp)
	move.l	%a0,%a4			| PB (a1: the DCE, kept)
	move.l	dCtlStorage(%a1),%a2
	move.l	%a2,%d0
	beq	prime_err
	move.w	ioVRefNum(%a4),%d0
	bsr	find_dev
	tst.l	%d2
	blt	prime_nodrv
	tst.b	D_FLAGS+1(%a3)
	beq	prime_offline		| no disk in the drive
	| block = dCtlPosition / 512, count = ioReqCount / 512
	move.l	dCtlPosition(%a1),%d3
	move.l	%d3,%d0
	and.l	#0x1FF,%d0
	bne	prime_param
	lsr.l	#8,%d3
	lsr.l	#1,%d3
	move.l	ioReqCount(%a4),%d4
	move.l	%d4,%d0
	and.l	#0x1FF,%d0
	bne	prime_param
	lsr.l	#8,%d4
	lsr.l	#1,%d4
	move.l	ioBuffer(%a4),%d0
	tst.b	MMU32Bit
	bne.s	0f
	and.l	#0x00FFFFFF,%d0		| 24-bit mode: drop the flag bits in the high byte
0:	move.l	%d0,%a2
	moveq	#BLK_READ,%d1
	move.w	ioTrap(%a4),%d0
	and.w	#0xFF,%d0
	cmp.w	#3,%d0			| aWrCmd
	bne.s	1f
	moveq	#BLK_WRITE,%d1
1:	bsr	blkcmd
	tst.l	%d0
	bne.s	prime_status
	move.w	#1,D_READ(%a3)
	move.l	ioReqCount(%a4),%d0
	move.l	%d0,ioActCount(%a4)
	add.l	%d0,dCtlPosition(%a1)
	moveq	#noErr,%d0
	bra.s	prime_done
prime_status:
	cmp.l	#E_NODATA,%d0
	beq.s	prime_offline
	cmp.l	#E_RO,%d0
	beq.s	prime_wprot
prime_io:
	moveq	#ioErr,%d0
	bra.s	prime_done
prime_offline:
	moveq	#offLinErr,%d0		| as for a floppy taken out: the File Manager puts the volume offline
	bra.s	prime_done
prime_wprot:
	moveq	#wPrErr,%d0
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
	tst.w	%d0
	beq.s	1f
	bsr	log_prime_err
1:	movem.l	(%sp)+,%d1-%d7/%a0-%a4
	bra	iodone

| ---- Control ----
d_control:
	log_first "PiDisk: control"
	move.w	csCode(%a0),%d0
	cmp.w	#1,%d0			| KillIO
	beq.s	ctl_ok
	cmp.w	#5,%d0			| verify
	beq.s	ctl_ok
	cmp.w	#6,%d0			| format
	beq.s	ctl_ok
	cmp.w	#7,%d0			| eject
	beq.s	ctl_eject
	cmp.w	#21,%d0			| physical drive icon
	beq.s	ctl_icon
	cmp.w	#22,%d0			| media icon
	beq.s	ctl_icon
	cmp.w	#23,%d0			| drive info: 0x00000601 = fixed, SCSI-like, primary? use external fixed
	beq.s	ctl_info
	moveq	#controlErr,%d0
	bra	iodone
| the drive's icon (24 November 2026): an SD card for the card's images and the Mac Exchange folder, a USB stick
| for a removable unit; before, one box with "Pi" for every unit
ctl_icon:
	movem.l	%d2/%a2-%a3,-(%sp)	| a1 (the DCE) must survive for IODone
	move.l	dCtlStorage(%a1),%a2
	move.l	%a2,%d2
	beq.s	2f
	move.w	ioVRefNum(%a0),%d0	| the drive number
	bsr	find_dev
	tst.l	%d2
	blt.s	2f
	tst.w	D_REMOV(%a3)
	beq.s	2f
	lea	icon_usb(%pc),%a2
	bra.s	3f
2:	lea	icon_sd(%pc),%a2
3:	move.l	%a2,csParam(%a0)
	movem.l	(%sp)+,%d2/%a2-%a3
	bra.s	ctl_ok
ctl_info:
	move.l	#0x00000401,csParam(%a0)	| drive type 1 (fixed? ) bit 10 external? keep simple
ctl_ok:
	moveq	#noErr,%d0
	bra	iodone
| eject: a fixed disk (a disk image) stays where it is; an ejectable one (a USB stick) is flushed and
| released by the Pi, and its drive is empty from now on (the File Manager has flushed the volume and
| put it offline, or unmounted it, before it asks)
ctl_eject:
	movem.l	%d1-%d7/%a0-%a4,-(%sp)
	move.l	dCtlStorage(%a1),%a2
	move.l	%a2,%d0
	beq.s	1f
	move.w	ioVRefNum(%a0),%d0
	bsr	find_dev
	tst.l	%d2
	blt.s	1f
	tst.w	D_REMOV(%a3)
	beq.s	1f
	log	"PiDisk: eject"
	clr.b	D_FLAGS+1(%a3)		| no disk in the drive
	clr.w	D_POSTS(%a3)
	moveq	#BLK_EJECT,%d1
	moveq	#0,%d3
	moveq	#0,%d4
	suba.l	%a2,%a2
	bsr	blkcmd
1:	movem.l	(%sp)+,%d1-%d7/%a0-%a4
	bra.s	ctl_ok

| ---- Status ----
d_status:
	log_first "PiDisk: status"
	move.w	csCode(%a0),%d0
	cmp.w	#8,%d0			| drive status
	bne.s	st_err
	| the 22-byte DrvSts block: track, writeProt, diskInPlace, installed, sides, the DrvQEl, ...
	movem.l	%a2/%a3/%d1/%d2,-(%sp)	| a1 (the DCE) must survive for IODone
	move.l	dCtlStorage(%a1),%a2
	move.w	ioVRefNum(%a0),%d0
	bsr	find_dev
	tst.l	%d2
	blt.s	st_nodrv
	lea	csParam(%a0),%a2
	move.w	#0,(%a2)+		| track
	move.b	D_FLAGS(%a3),(%a2)+	| writeProt (0x80 = locked)
	move.b	D_FLAGS+1(%a3),(%a2)+	| diskInPlace: 0 none, 1 ejectable, 8 fixed
	move.b	#1,(%a2)+		| installed
	move.b	#0,(%a2)+		| sides
	move.l	D_DQE(%a3),(%a2)+	| qLink
	move.w	D_DQE+4(%a3),(%a2)+	| qType
	move.w	D_DQE+6(%a3),(%a2)+	| dQDrive
	move.w	D_DQE+8(%a3),(%a2)+	| dQRefNum
	move.w	D_DQE+10(%a3),(%a2)+	| dQFSID
	move.b	#0,(%a2)+		| twoSideFmt
	move.b	#0,(%a2)+		| needsFlush
	move.w	#0,(%a2)+		| diskErrs
	movem.l	(%sp)+,%a2/%a3/%d1/%d2
	moveq	#noErr,%d0
	bra	iodone
st_nodrv:
	movem.l	(%sp)+,%a2/%a3/%d1/%d2
	moveq	#nsDrvErr,%d0
	bra	iodone
st_err:
	moveq	#statusErr,%d0
	bra	iodone

| ---- Close ----
d_close:
	moveq	#noErr,%d0
	rts

| ---- VBL task (every 2 s, the first 5 s after Open), for each unit: a new disk (a unit that has one
|      and whose drive is not in the queue yet, or whose media count moved) gets its drive and
|      disk-inserted events; a drive whose disk went is marked empty; and the events are posted, one
|      per drive each time, until the File Manager has read the drive (events posted before the
|      Finder runs are flushed at startup) ----
disk_vbl:
	movem.l	%d0-%d7/%a0-%a4,-(%sp)
	move.l	14(%a0),%a2		| globals (G_VBLSELF, after the task)
	moveq	#0,%d5
vbl_unit:
	bsr	dev_ptrs
	btst	#BD_PRESENT,BD_FLAGS+3(%a0)
	beq	vbl_none
	tst.l	BD_BLOCKS(%a0)
	beq	vbl_none
	tst.w	D_ADDED(%a3)
	beq.s	1f
	move.l	BD_MEDIA(%a0),%d0
	cmp.l	D_MEDIA(%a3),%d0
	beq	vbl_post
1:	move.l	BD_MEDIA(%a0),D_MEDIA(%a3)
	bsr	removable
	bsr	insert
	move.w	#15,D_POSTS(%a3)	| every 2 s for half a minute at most, until the drive is read
	log	"PiDisk: a disk was put in: disk-inserted event"
	bra	vbl_post
vbl_none:
	tst.w	D_ADDED(%a3)
	beq	vbl_next
	tst.b	D_FLAGS+1(%a3)
	beq	vbl_next
	clr.b	D_FLAGS+1(%a3)		| no disk in the drive
	clr.w	D_POSTS(%a3)
	log	"PiDisk: a disk was taken out: its drive is empty"
	bra	vbl_next
vbl_post:
	tst.w	D_POSTS(%a3)
	beq	vbl_next
	tst.w	D_READ(%a3)
	bne.s	2f
	tst.b	D_FLAGS+1(%a3)
	beq.s	2f
	subq.w	#1,D_POSTS(%a3)
	moveq	#7,%d0
	move.l	%d0,%a0			| diskEvt
	moveq	#0,%d0
	move.w	D_DQE+6(%a3),%d0	| the message: the drive number
	.short	0xA02F			| _PostEvent
	bra	vbl_next
2:	clr.w	D_POSTS(%a3)		| read already (or empty): no more events
vbl_next:
	addq.w	#1,%d5
	cmp.w	#MAXDEV,%d5
	blt	vbl_unit
	move.w	#120,G_VBL+10(%a2)	| again in 2 s
	movem.l	(%sp)+,%d0-%d7/%a0-%a4
	rts

| ---- common exit: immediate calls return, queued calls go through IODone ----
iodone:
	btst	#noQueueBit,ioTrap(%a0)
	beq.s	1f
	rts
1:	move.l	JIODone,-(%sp)
	rts

| ---- the drives' icons, 32x32 1-bit icon + mask (ICN#) each, then the Pascal string Get Info shows as where
|      the disk is (Control 21 and 22). Drawn 24 November 2026 for the owner ("how about an SD card"): an SD card,
|      its corner cut and its contacts along the top, the lock switch on the left and "SD" on it; a USB stick with
|      its plug, a lanyard hole and "USB" ----
	.even
icon_sd:
	.long	0x00000000,0x03FFFE00,0x02000300,0x02AAA180,0x02AAA0C0,0x02AAA060,0x02AAA020,0x02000020
	.long	0x02000020,0x02000020,0x06000020,0x04000020,0x05000020,0x05000020,0x06000020,0x02000020
	.long	0x023C3C20,0x02663620,0x02603320,0x023C3320,0x02063320,0x02663620,0x023C3C20,0x02000020
	.long	0x02000020,0x02000020,0x02000020,0x02000020,0x03FFFFE0,0x00000000,0x00000000,0x00000000
	.long	0x00000000,0x03FFFE00,0x03FFFF00,0x03FFFF80,0x03FFFFC0,0x03FFFFE0,0x03FFFFE0,0x03FFFFE0
	.long	0x03FFFFE0,0x03FFFFE0,0x07FFFFE0,0x07FFFFE0,0x07FFFFE0,0x07FFFFE0,0x07FFFFE0,0x03FFFFE0
	.long	0x03FFFFE0,0x03FFFFE0,0x03FFFFE0,0x03FFFFE0,0x03FFFFE0,0x03FFFFE0,0x03FFFFE0,0x03FFFFE0
	.long	0x03FFFFE0,0x03FFFFE0,0x03FFFFE0,0x03FFFFE0,0x03FFFFE0,0x00000000,0x00000000,0x00000000
	.byte	7
	.ascii	"SD card"
	.even
icon_usb:
	.long	0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000
	.long	0x1FFFFE00,0x100003F8,0x10000208,0x100002C8,0x102BB2C8,0x172A2A08,0x152BB208,0x1728AA08
	.long	0x103BB2C8,0x100002C8,0x10000208,0x100003F8,0x1FFFFE00,0x00000000,0x00000000,0x00000000
	.long	0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000
	.long	0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000
	.long	0x1FFFFE00,0x1FFFFFF8,0x1FFFFFF8,0x1FFFFFF8,0x1FFFFFF8,0x1FFFFFF8,0x1FFFFFF8,0x1FFFFFF8
	.long	0x1FFFFFF8,0x1FFFFFF8,0x1FFFFFF8,0x1FFFFFF8,0x1FFFFE00,0x00000000,0x00000000,0x00000000
	.long	0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000
	.byte	9
	.ascii	"USB drive"
	.even
drvr_end:
