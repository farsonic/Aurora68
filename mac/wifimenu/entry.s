| mac/wifimenu/entry.s - the WiFi menu's way in: the INIT entry and its two hooks. MPL-2.0.
|
| The Start Manager runs 'INIT' 128 from a resource it will release afterwards. So the entry copies
| the whole image (tools/mac/elf2init.py: the code and data, then the relocation table) into a
| pointer in the system heap, adds the copy's address to every 32-bit field the table names,
| flushes the instruction cache and calls init_main() in the copy. init_main installs:
|   patch_drawmenubar  _DrawMenuBar: the system's, then the WiFi icon at the right of the bar
|   gne_filter         jGNEFilter: a press on the icon pops up the WiFi menu (wifimenu.c), and
|                      between events a look at the WiFi state, to redraw the icon and to save a
|                      joined network (c_tick)
| The C code keeps D2-D7/A2-A6 (GCC) and is compiled -ffixed-a5: the application's A5, which the
| Toolbox needs, is never touched.

	.text
	.globl	_start
	.globl	_reloc_off
_start:
	bra.s	1f
_reloc_off:
	.long	0			| filled in by elf2init.py: the image's size, where the table starts
1:	movem.l	%d0-%d7/%a0-%a6,-(%sp)
	lea	_start(%pc),%a3
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
	lea	(%a3,%d2.l),%a4		| the table (in the resource)
	move.l	(%a4)+,%d3
	move.l	%a2,%d4
	bra.s	4f
3:	move.l	(%a4)+,%d1
	add.l	%d4,(%a2,%d1.l)
4:	subq.l	#1,%d3
	bcc.s	3b
	moveq	#1,%d0
	.short	0xA198			| _HWPriv 1: FlushInstructionCache
	move.l	#init_main,%a0		| an offset here: this copy is not relocated
	add.l	%a2,%a0
	jsr	(%a0)
9:	movem.l	(%sp)+,%d0-%d7/%a0-%a6
	rts

| pascal void DrawMenuBar(void)
	.globl	patch_drawmenubar
patch_drawmenubar:
	move.l	old_drawmenubar,%a0
	jsr	(%a0)
	movem.l	%d0-%d2/%a0-%a1,-(%sp)
	jsr	c_draw_icon
	movem.l	(%sp)+,%d0-%d2/%a0-%a1
	rts

| jGNEFilter: A1 the event, the result word at 4(sp); every register kept, the next filter after.
| A press on the WiFi icon is taken here (c_filter answers 1): the event becomes a null event and
| the result false, so the program never sees it. (Patching _MenuSelect instead, the first
| version, skipped the real MenuSelect under the Process Manager's own patch of it, which then
| worked on menu state nobody had set up and wrote over low memory: the _PPC trap's address, a
| crash of the Finder later.)
	.globl	gne_filter
gne_filter:
	movem.l	%d0-%d2/%a0-%a1,-(%sp)
	move.l	%a1,-(%sp)
	jsr	c_filter		| int c_filter(EventRecord *ev)
	addq.l	#4,%sp
	tst.l	%d0
	movem.l	(%sp)+,%d0-%d2/%a0-%a1	| (the condition codes stay)
	beq.s	2f
	clr.w	4(%sp)
	moveq	#0,%d0
2:	move.l	old_gne,-(%sp)
	beq.s	1f
	rts				| into the filter that was there before
1:	addq.l	#4,%sp
	rts

| register-based traps, for C
	.globl	get_tool_trap
get_tool_trap:				| void *get_tool_trap(int trap)
	move.w	6(%sp),%d0
	.short	0xA746			| _GetToolTrapAddress
	move.l	%a0,%d0
	rts
	.globl	set_tool_trap
set_tool_trap:				| void set_tool_trap(void *addr, int trap)
	move.l	4(%sp),%a0
	move.w	10(%sp),%d0
	.short	0xA647			| _SetToolTrapAddress
	rts
	.globl	mac_newhandle
mac_newhandle:				| void **mac_newhandle(long size): the current heap
	move.l	4(%sp),%d0
	.short	0xA122			| _NewHandle
	move.l	%a0,%d0
	rts
	.globl	get_sr
get_sr:	moveq	#0,%d0
	move.w	%sr,%d0
	rts
	.globl	set_sr
set_sr:	move.w	6(%sp),%sr
	rts
