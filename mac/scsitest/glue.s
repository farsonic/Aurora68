| SPDX-License-Identifier: MPL-2.0
| mac/scsitest/glue.s - the SCSI Manager calls of "Aurora68 SCSI Test" (mac/scsitest/scsitest.c), and the status
| register for the log (as mac/iotest/glue.s). _SCSIDispatch ($A815) is a Pascal call: room for the OSErr, the
| arguments left to right, the routine selector last (Inside Macintosh IV, The SCSI Manager). D2 is kept, as GCC
| expects; the trap may change it. C passes every argument as a long on the stack.
	.text
	.globl	sc_get
sc_get:	move.l	%d2,-(%sp)
	clr.w	-(%sp)
	move.w	#1,-(%sp)		| SCSIGet: arbitration
	.short	0xA815
	move.w	(%sp)+,%d0
	ext.l	%d0
	move.l	(%sp)+,%d2
	rts
| short sc_select(int id)
	.globl	sc_select
sc_select:
	move.l	%d2,-(%sp)
	clr.w	-(%sp)
	move.w	12(%sp),-(%sp)		| the ID (the low word of its slot)
	move.w	#2,-(%sp)		| SCSISelect
	.short	0xA815
	move.w	(%sp)+,%d0
	ext.l	%d0
	move.l	(%sp)+,%d2
	rts
| short sc_cmd(void *cdb, int count)
	.globl	sc_cmd
sc_cmd:	move.l	%d2,-(%sp)
	clr.w	-(%sp)
	move.l	10(%sp),-(%sp)		| the command's bytes
	move.w	20(%sp),-(%sp)		| their count
	move.w	#3,-(%sp)		| SCSICmd
	.short	0xA815
	move.w	(%sp)+,%d0
	ext.l	%d0
	move.l	(%sp)+,%d2
	rts
| short sc_complete(short *stat, short *msg, long wait)
	.globl	sc_complete
sc_complete:
	move.l	%d2,-(%sp)
	clr.w	-(%sp)
	move.l	10(%sp),-(%sp)		| VAR stat
	move.l	18(%sp),-(%sp)		| VAR message
	move.l	26(%sp),-(%sp)		| wait, in ticks
	move.w	#4,-(%sp)		| SCSIComplete
	.short	0xA815
	move.w	(%sp)+,%d0
	ext.l	%d0
	move.l	(%sp)+,%d2
	rts
| short sc_xfer(void *tib, int selector): SCSIRead 5, SCSIWrite 6, SCSIRBlind 8, SCSIWBlind 9
	.globl	sc_xfer
sc_xfer:
	move.l	%d2,-(%sp)
	clr.w	-(%sp)
	move.l	10(%sp),-(%sp)		| the transfer instruction block
	move.w	20(%sp),-(%sp)		| the selector
	.short	0xA815
	move.w	(%sp)+,%d0
	ext.l	%d0
	move.l	(%sp)+,%d2
	rts

| the INFO block is shared with the drivers' logs: a line is written with interrupts masked
	.globl	io_get_sr
io_get_sr:
	moveq	#0,%d0
	move.w	%sr,%d0
	rts
	.globl	io_set_sr
io_set_sr:
	move.w	6(%sp),%sr
	rts
