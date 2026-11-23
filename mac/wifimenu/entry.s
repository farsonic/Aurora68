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

| Pascal callbacks of the password dialog (wifimenu.c ask_password)
| pascal void (DialogPtr dlg, short item): a user item's drawing procedure
	.globl	pas_useritem
pas_useritem:
	move.w	4(%sp),%d0
	ext.l	%d0
	move.l	%d0,-(%sp)
	move.l	10(%sp),-(%sp)
	jsr	c_useritem		| void c_useritem(void *dlg, int item)
	addq.l	#8,%sp
	move.l	(%sp)+,%a0
	addq.l	#6,%sp
	jmp	(%a0)
| pascal Boolean (DialogPtr dlg, EventRecord *ev, short *item): ModalDialog's filter
	.globl	pas_dlgfilter
pas_dlgfilter:
	move.l	4(%sp),-(%sp)
	move.l	12(%sp),-(%sp)
	move.l	20(%sp),-(%sp)
	jsr	c_dlgfilter		| int c_dlgfilter(void *dlg, EventRecord *ev, short *item)
	lea	12(%sp),%sp
	move.l	(%sp)+,%a0
	lea	12(%sp),%sp
	tst.l	%d0
	sne	%d0
	and.b	#1,%d0
	move.b	%d0,(%sp)		| the BOOLEAN result: the high byte of the word
	clr.b	1(%sp)
	jmp	(%a0)

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

| ---- for the clock (clock.c, 23 November 2026) ----
| The File and Device Managers: a parameter block in A0, synchronous, the result in D0. OS traps keep
| D1-D2/A1, so GCC's registers survive.
	.macro	pbcall name, trap
	.globl	\name
\name:	move.l	4(%sp),%a0
	.short	\trap
	ext.l	%d0
	rts
	.endm
	pbcall	pb_close, 0xA001
	pbcall	pb_read, 0xA002
	pbcall	pb_write, 0xA003
	pbcall	pb_control, 0xA004
	pbcall	pb_seteof, 0xA012
	pbcall	pb_flushvol, 0xA013
	pbcall	pb_hopen, 0xA200		| the data fork (HFS: a directory ID at 48)
	pbcall	pb_hcreate, 0xA208
	pbcall	pb_hdelete, 0xA209
	pbcall	pb_hgetfinfo, 0xA20C
	pbcall	pb_hsetfinfo, 0xA20D
| _HFSDispatch: the selector in D0
	.macro	hfscall name, sel
	.globl	\name
\name:	move.l	4(%sp),%a0
	moveq	#\sel,%d0
	.short	0xA260
	ext.l	%d0
	rts
	.endm
	hfscall	pb_catmove, 5
	hfscall	pb_dircreate, 6
	hfscall	pb_getfcbinfo, 8
	hfscall	pb_getcatinfo, 9

| void mac_secs2date(unsigned long secs, void *rec): _Secs2Date, D0 the seconds, A0 the DateTimeRec
	.globl	mac_secs2date
mac_secs2date:
	move.l	%d2,-(%sp)
	move.l	8(%sp),%d0
	move.l	12(%sp),%a0
	.short	0xA9C6
	move.l	(%sp)+,%d2
	rts

| ---- the Process Manager (_OSDispatch, an OSErr result, the selector pushed last) ----
	.macro	osd1 name, sel			| short name(void *a)
	.globl	\name
\name:	move.l	%d2,-(%sp)		| a Pascal trap may change D2, which GCC keeps
	clr.w	-(%sp)
	move.l	10(%sp),-(%sp)
	move.w	#\sel,-(%sp)
	.short	0xA88F
	move.w	(%sp)+,%d0
	ext.l	%d0
	move.l	(%sp)+,%d2
	rts
	.endm
	osd1	pm_current, 0x37		| GetCurrentProcess(VAR psn)
	osd1	pm_next, 0x38			| GetNextProcess(VAR psn)
	osd1	pm_setfront, 0x3B		| SetFrontProcess(psn)
	.globl	pm_info				| short pm_info(const psn *, ProcessInfoRec *): GetProcessInformation
pm_info:
	move.l	%d2,-(%sp)
	clr.w	-(%sp)
	move.l	10(%sp),-(%sp)
	move.l	18(%sp),-(%sp)
	move.w	#0x3A,-(%sp)
	.short	0xA88F
	move.w	(%sp)+,%d0
	ext.l	%d0
	move.l	(%sp)+,%d2
	rts

| ---- the Apple Event Manager (_Pack8, the selector in D0) and the Alias Manager (_AliasDispatch), as in
| mac/piagent/entry.s ----
	.macro	pas_l name, trap, sel, n	| short name(long a1 .. an): every argument a long
	.globl	\name
\name:	link	%a6,#0
	move.l	%d2,-(%sp)
	clr.w	-(%sp)
	.irp	k, 0, 1, 2, 3
	.if	\k < \n
	move.l	(8+4*\k)(%a6),-(%sp)
	.endif
	.endr
	move.l	#\sel,%d0		| (the whole of D0: the Alias Manager reads a long)
	.short	\trap
	move.w	(%sp)+,%d0
	ext.l	%d0
	move.l	-4(%a6),%d2
	unlk	%a6
	rts
	.endm
	pas_l	ae_create_desc, 0xA816, 0x0825, 4	| AECreateDesc(type, dataPtr, size, VAR result)
	pas_l	ae_put_desc, 0xA816, 0x0609, 3		| AEPutDesc(VAR list, index, desc)
	pas_l	ae_put_param_desc, 0xA816, 0x0610, 3	| AEPutParamDesc(VAR event, keyword, desc)
	pas_l	ae_dispose_desc, 0xA816, 0x0204, 1	| AEDisposeDesc(VAR desc)
	pas_l	new_alias, 0xA823, 2, 3			| NewAlias(fromFile, target, VAR alias)
	.globl	ae_create_list			| short ae_create_list(VAR list): AECreateList(nil, 0, false, list)
ae_create_list:
	move.l	%d2,-(%sp)
	clr.w	-(%sp)
	clr.l	-(%sp)
	clr.l	-(%sp)
	clr.w	-(%sp)			| isRecord: false
	move.l	20(%sp),-(%sp)
	move.w	#0x0706,%d0
	.short	0xA816
	move.w	(%sp)+,%d0
	ext.l	%d0
	move.l	(%sp)+,%d2
	rts
	.globl	ae_create_event			| short ae_create_event(class, id, target, VAR event): returnID -1 (made up), transaction 0
ae_create_event:
	link	%a6,#0
	move.l	%d2,-(%sp)
	clr.w	-(%sp)
	move.l	8(%a6),-(%sp)
	move.l	12(%a6),-(%sp)
	move.l	16(%a6),-(%sp)
	move.w	#-1,-(%sp)		| kAutoGenerateReturnID
	clr.l	-(%sp)			| kAnyTransactionID
	move.l	20(%a6),-(%sp)
	move.w	#0x0B14,%d0
	.short	0xA816
	move.w	(%sp)+,%d0
	ext.l	%d0
	move.l	-4(%a6),%d2
	unlk	%a6
	rts
	.globl	ae_send				| short ae_send(event, VAR reply, mode): normal priority, the default timeout, no procedures
ae_send:
	link	%a6,#0
	move.l	%d2,-(%sp)
	clr.w	-(%sp)
	move.l	8(%a6),-(%sp)
	move.l	12(%a6),-(%sp)
	move.l	16(%a6),-(%sp)
	clr.w	-(%sp)			| kAENormalPriority
	move.l	#-1,-(%sp)		| kAEDefaultTimeout
	clr.l	-(%sp)			| no idle procedure
	clr.l	-(%sp)			| no filter
	move.w	#0x0D17,%d0
	.short	0xA816
	move.w	(%sp)+,%d0
	ext.l	%d0
	move.l	-4(%a6),%d2
	unlk	%a6
	rts
| the Memory Manager, in registers
	.globl	mac_hlock			| void mac_hlock(void *h)
mac_hlock:
	move.l	4(%sp),%a0
	.short	0xA029
	rts
	.globl	mac_dispose_handle		| void mac_dispose_handle(void *h)
mac_dispose_handle:
	move.l	4(%sp),%a0
	.short	0xA023
	rts
	.globl	mac_setzone			| void mac_setzone(void *zone)
mac_setzone:
	move.l	4(%sp),%a0
	.short	0xA01B
	rts
