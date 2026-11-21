| mac/piscrn/entry.s - PiScrn's way in: the INIT entry, and its filter on GetNextEvent. MPL-2.0.
|
| The Start Manager runs 'INIT' 128 once, from a locked resource in the system heap, and releases it
| afterwards. Most starts PiScrn does all its work in that one call and leaves nothing behind. But
| when a new HDMI mode waits to be kept (PiDisplay, 21 November 2026) it must stay to ask once the
| Finder is up, through a filter on GetNextEvent. So the image is copied into a pointer in the system
| heap first, as the WiFi menu's is (mac/wifimenu/entry.s), every 32-bit field the table names
| (tools/mac/elf2init.py, macelf.py) gets the copy's address added, the instruction cache is
| flushed, and init_main() in piscrn.c runs in the copy. It answers 1 when it has put the filter in
| (the copy stays), else 0, and the copy is given back. The C code keeps D2-D7/A2-A6 (GCC) and is
| compiled -ffixed-a5.

	.text
	.globl	_start
	.globl	_reloc_off
_start:
	bra.s	1f
_reloc_off:
	.long	0			| filled in by elf2init.py: the image's size, where the table starts
1:	movem.l	%d0-%d7/%a0-%a6,-(%sp)
	lea	_start(%pc),%a3		| the image, in the resource
	move.l	_reloc_off(%pc),%d2
	move.l	%d2,%d0
	.short	0xA51E			| _NewPtr,Sys
	move.l	%a0,%d0
	beq.s	9f
	move.l	%a0,%a2			| the copy
	move.l	%a3,%a1
	move.l	%d2,%d1
2:	move.b	(%a1)+,(%a0)+
	subq.l	#1,%d1
	bne.s	2b
	lea	(%a3,%d2.l),%a4		| the table (in the resource): a count, then the offsets of the fields
	move.l	(%a4)+,%d3
	move.l	%a2,%d4
	bra.s	4f
3:	move.l	(%a4)+,%d1
	add.l	%d4,(%a2,%d1.l)
4:	subq.l	#1,%d3
	bcc.s	3b
	moveq	#1,%d0
	.short	0xA198			| _HWPriv 1: FlushInstructionCache
	move.l	#init_main,%a0		| an offset here: this code is not relocated
	add.l	%a2,%a0
	jsr	(%a0)
	tst.l	%d0
	bne.s	9f			| the filter is in: the copy stays
	move.l	%a2,%a0
	.short	0xA01F			| _DisposPtr
9:	movem.l	(%sp)+,%d0-%d7/%a0-%a6
	rts

| jGNEFilter: A1 the event, the result word at 4(sp); every register kept, the next filter after
| (mac/wifimenu/entry.s has the same, and why a press is taken there; PiScrn takes nothing)
	.globl	gne_filter
gne_filter:
	movem.l	%d0-%d2/%a0-%a1,-(%sp)
	move.l	%a1,-(%sp)
	jsr	c_filter		| int c_filter(EventRecord *ev): always 0
	addq.l	#4,%sp
	movem.l	(%sp)+,%d0-%d2/%a0-%a1
	move.l	old_gne,-(%sp)
	beq.s	1f
	rts				| into the filter that was there before
1:	addq.l	#4,%sp
	rts

| short slot_next_video(void *spblock): _SlotManager sNextTypeSRsrc (21), the result in D0
	.globl	slot_next_video
slot_next_video:
	move.l	4(%sp),%a0
	moveq	#21,%d0
	.short	0xA06E
	ext.l	%d0
	rts

| void *new_handle_sys(long size): _NewHandle,Sys (A0 the handle, 0 when there is no room)
	.globl	new_handle_sys
new_handle_sys:
	move.l	4(%sp),%d0
	.short	0xA522
	move.l	%a0,%d0
	rts

| void restart_mac(void): the Shutdown Manager's ShutDwnStart (selector 2 on the stack)
	.globl	restart_mac
restart_mac:
	move.l	%d2,-(%sp)		| a Toolbox trap may change D2, which GCC keeps
	move.w	#2,-(%sp)
	.short	0xA895
	move.l	(%sp)+,%d2
	rts

| unsigned short get_sr(void), void set_sr(unsigned short): the INFO block is shared with the drivers'
| logs, so a log line is written with interrupts masked, as the drivers do it
	.globl	get_sr
get_sr:
	moveq	#0,%d0
	move.w	%sr,%d0
	rts
	.globl	set_sr
set_sr:
	move.w	6(%sp),%sr
	rts

| void dispose_handle(void *h): _DisposHandle
	.globl	dispose_handle
dispose_handle:
	move.l	4(%sp),%a0
	.short	0xA023
	rts
