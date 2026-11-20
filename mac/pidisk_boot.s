| pidisk_boot.s — the boot record (sBootRecord, ID 6) of PiDisk's sResource in the slot-$9
| declaration ROM: what lets a disk image on the Pi's card be the Macintosh's startup disk.
| GNU as MIT syntax, 68020. Written 20 November 2026 (docs/08 item 67).
| SPDX-License-Identifier: MPL-2.0
|
| How the IIci ROM starts from a NuBus card. The Start Manager asks _GetDefaultStartup for the
| startup device in parameter RAM ($78-$7B). When that names a slot and an sResource rather than a
| driver, it runs the sResource's boot record through the Slot Manager's sExec (0x40801540) with
| seBootState 0, before it loads the SCSI drivers, and takes the driver reference number the record
| leaves in seRefNum: its first pass over the drive queue then tries that driver's drives (and a
| floppy), the passes after it every drive (0x408014F0, 0x40801590, the filter at 0x408016F0). Later,
| once the System file is open, the ROM opens the slot drivers that are to be opened at start
| (0x40802240): for an sResource with a boot record it runs the record once more, with seBootState 1
| (0x4080222A), and leaves the driver to it. sExec copies the record into a block of the heap,
| calls it with A0 = the SEBlock and every register saved, and disposes of the block afterwards
| (0x40805C3A), so nothing may be left pointing into this code.
|
| So, both times: open .PiDisk from the declaration ROM with OpenSlot, exactly as the ROM itself
| opens a slot driver (0x40800CA0: ioFlags fMulti, an SEBlock naming the slot and the sResource).
| The driver's Open puts a drive in the drive queue for every image the Pi publishes. At state 0,
| ask the Pi which disk is to be tried first (the BLOCK service's PSMB_BLK_BOOT): the image, or a
| SCSI disk the parameter RAM names, which the kernel showed the Mac as this card until now
| (mac.boot=, psmac_mbox.c), and put that driver's reference number in seRefNum. A drive the Start Manager does not take in
| its first pass stays in the queue, ahead of the SCSI drives the ROM loads after this, so the
| image is tried before the other SCSI disks. With no image published, seStatus says so and the
| ROM carries on as if the record had not been there.
|
| SEBlock (Designing Cards and Drivers): seSlot 0, sesRsrcId 1, seStatus 2 (word), seFlags 4,
| seResult 8, seIOFileName 12, seDevice 16, sePartition 17, seOSType 18, seReserved 19,
| seRefNum 20 (word), seBootState 22.

	.text
	.globl	boot_start

	.equ	MMU32Bit,	0x0CB2
	.equ	Ticks,		0x016A
	.equ	DrvQHdr,	0x0308
	.equ	_OpenSlot,	0xA200		| _Open with the slot bit
	| SEBlock
	.equ	seSlot,		0
	.equ	sesRsrcId,	1
	.equ	seStatus,	2
	.equ	seIOFileName,	12
	.equ	seDevice,	16
	.equ	seRefNum,	20
	.equ	seBootState,	22
	| ParamBlock (SlotDevParam / MultiDevParam)
	.equ	ioNamePtr,	18
	.equ	ioRefNum,	24
	.equ	ioPermssn,	27
	.equ	ioMix,		28
	.equ	ioFlags,	32
	.equ	ioSEBlkPtr,	34
	.equ	fMulti,		0
	| the mailbox (psmb.h)
	.equ	CMD_BLOCK,	0x1000+64
	.equ	C_REQ,		0
	.equ	C_DONE,		4
	.equ	C_CMD,		8
	.equ	C_STATUS,	36
	.equ	C_RESULT,	40
	.equ	BLK_BOOT,	6
	.equ	E_BUSY,		-8
	| the frame
	.equ	F_PB,		-80		| 50-byte parameter block
	.equ	F_SE,		-24		| 24-byte SEBlock for OpenSlot
	.equ	F_SIZE,		-80

| ---- log through the Pi's INFO service, as the driver does: `log "text"` ----
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

boot_start:
	link	%a6,#F_SIZE
	move.l	%a0,%a4			| the SEBlock the Slot Manager hands us
	move.w	#-1,seStatus(%a4)	| until a drive is there
	lea	F_PB(%a6),%a0
	moveq	#80/4-1,%d0
1:	clr.l	(%a0)+
	dbf	%d0,1b
	| OpenSlot ".PiDisk" from this slot and sResource
	lea	F_SE(%a6),%a1
	move.b	seSlot(%a4),seSlot(%a1)
	move.b	sesRsrcId(%a4),sesRsrcId(%a1)
	lea	name(%pc),%a0
	move.l	%a0,seIOFileName(%a1)
	lea	F_PB(%a6),%a0
	move.l	%a1,ioSEBlkPtr(%a0)
	move.w	#1<<fMulti,ioFlags(%a0)
	lea	name(%pc),%a1
	move.l	%a1,ioNamePtr(%a0)
	.short	_OpenSlot
	tst.w	%d0
	bne	open_failed
	move.w	F_PB+ioRefNum(%a6),%d5	| the driver's reference number
	tst.b	seBootState(%a4)
	bne	done_ok			| state 1: the driver is open, which is all the ROM asks now
	| state 0: which disk goes first (asked whatever comes next: the question also tells the
	| kernel that the Start Manager has read the startup device), and is there a drive?
	bsr	ask_pi			| d0 = 0: the image first; else the refNum of a SCSI disk
	move.w	%d0,%d4
	bsr	count_drives
	tst.w	%d0
	beq	no_drive
	move.w	%d4,%d0
	bne.s	2f
	move.w	%d5,%d0
	log	"PiDisk boot: the card's image is the startup disk"
	bra	3f
2:	log	"PiDisk boot: the SCSI disk the parameter RAM names first, the card's image after it"
3:	move.w	%d0,seRefNum(%a4)
done_ok:
	clr.w	seStatus(%a4)
	bra	out
open_failed:
	log	"PiDisk boot: OpenSlot .PiDisk failed"
	bra	out
no_drive:
	log	"PiDisk boot: no image on the card, nothing to start from"
out:
	unlk	%a6
	rts

| count_drives: d0 = drives in the queue whose dQRefNum is d5
count_drives:
	moveq	#0,%d0
	move.l	DrvQHdr+2,%a0		| qHead
1:	move.l	%a0,%d1
	beq.s	2f
	cmp.w	8(%a0),%d5		| dQRefNum
	bne.s	3f
	addq.w	#1,%d0
3:	move.l	(%a0),%a0
	bra.s	1b
2:	rts

| window: a0 = the mailbox window for the MMU's mode
window:
	lea	0xF9000000,%a0
	tst.b	MMU32Bit
	bne.s	1f
	lea	0x00900000,%a0
1:	rts

| ask_pi: PSMB_BLK_BOOT -> d0.w = the result (0, or a driver reference number). The kernel answers
| PSMB_E_BUSY while it has not yet looked at the card for images (a few tens of milliseconds after
| the 68k starts, long before this runs, but a slow card is possible): asked again for up to three
| seconds of Ticks, then the image first.
ask_pi:
	movem.l	%d1-%d2/%a0,-(%sp)
	move.l	Ticks,%d2
	add.l	#180,%d2
1:	bsr	window
	lea	CMD_BLOCK(%a0),%a0
	move.w	#BLK_BOOT,C_CMD(%a0)
	move.l	C_REQ(%a0),%d1
	addq.l	#1,%d1
	move.l	%d1,C_REQ(%a0)
	move.l	#100000000,%d0
2:	cmp.l	C_DONE(%a0),%d1
	beq.s	3f
	subq.l	#1,%d0
	bne.s	2b
	moveq	#0,%d0			| no answer: the image first
	bra.s	9f
3:	moveq	#E_BUSY,%d0
	cmp.l	C_STATUS(%a0),%d0
	bne.s	4f
	cmp.l	Ticks,%d2
	bhi.s	1b
	moveq	#0,%d0
	bra.s	9f
4:	tst.l	C_STATUS(%a0)
	beq.s	5f
	moveq	#0,%d0			| an older kernel without the command: the image first
	bra.s	9f
5:	move.l	C_RESULT(%a0),%d0
9:	movem.l	(%sp)+,%d1-%d2/%a0
	rts

| do_log: a1 = C string -> INFO command (block 0): cmd 2, buf, len
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

name:	.byte	7
	.ascii	".PiDisk"
	.even
