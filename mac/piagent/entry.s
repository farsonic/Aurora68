| SPDX-License-Identifier: MPL-2.0
| mac/piagent/entry.s - the Aurora68 Agent's way in, its two hooks, and the Toolbox calls it makes.
|
| The Start Manager runs 'INIT' 128 once, from a resource it releases afterwards. So the entry copies
| the whole image (tools/mac/elf2init.py: code, data, then the relocation table) into a pointer in the
| system heap, adds the copy's address to every 32-bit field the table names, flushes the instruction
| cache and calls init_main() in the copy, as the WiFi menu and Screen Setup do (mac/wifimenu/entry.s,
| mac/piscrn/entry.s). init_main answers 1 when the hooks are in (the copy stays), else 0, and the copy
| is given back. Then two ways into the C code (agent.c):
|   gne_filter  jGNEFilter: between two events of whatever program calls GetNextEvent or WaitNextEvent,
|               application time, where the File Manager's synchronous calls are allowed. The requests
|               of the remote control are carried out here.
|   vbl_task    a VBL task, 60 times a second at interrupt time: a heartbeat for the kernel, and under
|               System 7 WakeUpProcess for the program in front when a request waits.
| The C code keeps D2-D7/A2-A6 (GCC) and is compiled -ffixed-a5: the current program's A5, which the
| Toolbox needs, is never touched. The rest of this file is glue for the calls the Toolbox takes in
| registers or with a selector, which tools/mac/mktraps.py's Pascal glue does not cover.

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
	bne.s	9f			| the hooks are in: the copy stays
	move.l	%a2,%a0
	.short	0xA01F			| _DisposPtr
9:	movem.l	(%sp)+,%d0-%d7/%a0-%a6
	rts

| jGNEFilter: A1 the event, the result word at 4(sp); every register kept, the next filter after.
| c_filter does the waiting request; it answers 1 when it has taken the event (an application-died
| event of the agent's own launch, agent.c take_obit), and GetNextEvent's result is made false.
	.globl	gne_filter
gne_filter:
	movem.l	%d0-%d2/%a0-%a1,-(%sp)
	move.l	%a1,-(%sp)
	jsr	c_filter		| int c_filter(EventRecord *event)
	addq.l	#4,%sp
	tst.l	%d0
	beq.s	2f
	clr.w	24(%sp)			| the result word, above the five registers and the return address
2:	movem.l	(%sp)+,%d0-%d2/%a0-%a1
	move.l	old_gne,-(%sp)
	beq.s	1f
	rts				| into the filter that was there before
1:	addq.l	#4,%sp
	rts

| A VBL task: A0 the task record. A task may change D0-D3/A0-A3; GCC keeps the rest.
	.globl	vbl_task
vbl_task:
	move.l	%a0,-(%sp)
	jsr	c_vbl			| void c_vbl(void *task)
	addq.l	#4,%sp
	rts

| ---- the File Manager: a parameter block in A0, the result in D0 (and ioResult), synchronous ----
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
	pbcall	pb_geteof, 0xA011
	pbcall	pb_seteof, 0xA012
	pbcall	pb_flushvol, 0xA013
	pbcall	pb_setvol, 0xA015
	pbcall	pb_hopen, 0xA200		| the data fork (HFS: a directory ID at 48)
	pbcall	pb_hgetvinfo, 0xA207
	pbcall	pb_hcreate, 0xA208
	pbcall	pb_hdelete, 0xA209
	pbcall	pb_hopenrf, 0xA20A
	pbcall	pb_hrename, 0xA20B
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
	hfscall	pb_openwd, 1
	hfscall	pb_catmove, 5
	hfscall	pb_dircreate, 6
	hfscall	pb_getcatinfo, 9
	hfscall	pb_setcatinfo, 10
	hfscall	pb_catsearch, 0x18		| System 7: the catalog searched (an application by its creator)

| ---- the Process Manager (System 7): _OSDispatch, an OSErr result, the selector pushed last ----
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
	osd1	pm_wakeup, 0x3C			| WakeUpProcess(psn): allowed at interrupt time (the VBL task's)
| GetFrontProcess(VAR psn) takes one more long, -1, pushed after its argument (its inline in Apple's
| interface is moveq #-1,D0; move.l D0,-(SP) before the selector): without it the trap popped four bytes
| that were never pushed, and the return address after it was garbage (the first tests crashed in it)
	.globl	pm_front
pm_front:
	move.l	%d2,-(%sp)
	clr.w	-(%sp)
	move.l	10(%sp),-(%sp)
	moveq	#-1,%d0
	move.l	%d0,-(%sp)
	move.w	#0x39,-(%sp)
	.short	0xA88F
	move.w	(%sp)+,%d0
	ext.l	%d0
	move.l	(%sp)+,%d2
	rts
| AcceptHighLevelEvent(VAR sender, VAR refcon, buffer, VAR size): the high-level event in front taken
	.globl	hle_accept			| short hle_accept(void *sender, long *refcon, void *buffer, long *size)
hle_accept:
	link	%a6,#0
	move.l	%d2,-(%sp)
	clr.w	-(%sp)
	move.l	8(%a6),-(%sp)
	move.l	12(%a6),-(%sp)
	move.l	16(%a6),-(%sp)
	move.l	20(%a6),-(%sp)
	move.w	#0x33,-(%sp)
	.short	0xA88F
	move.w	(%sp)+,%d0
	ext.l	%d0
	move.l	-4(%a6),%d2
	unlk	%a6
	rts
| PostHighLevelEvent(theEvent, receiverID, msgRefcon, msgBuff, msgLen, postingOptions): an event given back
	.globl	hle_post			| short hle_post(ev, to, refcon, buffer, size, options)
hle_post:
	link	%a6,#0
	move.l	%d2,-(%sp)
	clr.w	-(%sp)
	.irp	k, 0, 1, 2, 3, 4, 5
	move.l	(8+4*\k)(%a6),-(%sp)
	.endr
	move.w	#0x34,-(%sp)
	.short	0xA88F
	move.w	(%sp)+,%d0
	ext.l	%d0
	move.l	-4(%a6),%d2
	unlk	%a6
	rts
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

| short mac_launch(void *block): _Launch with the block in A0. System 7's LaunchApplication answers in D0;
| System 6's Launch without MultiFinder does not come back.
	.globl	mac_launch
mac_launch:
	move.l	4(%sp),%a0
	.short	0xA9F2
	ext.l	%d0
	rts

| ---- the Apple Event Manager (_Pack8, the selector in D0) and the Alias Manager (_AliasDispatch) ----
| Each takes longs (pointers, OSTypes) on the GCC side; the few INTEGER and BOOLEAN arguments are
| pushed as the Pascal side wants them, by hand.
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
	pas_l	ae_coerce_desc, 0xA816, 0x0603, 3	| AECoerceDesc(desc, toType, VAR result)
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

| ---- the Memory Manager and the rest, in registers ----
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
	.globl	mac_vinstall			| short mac_vinstall(void *task)
mac_vinstall:
	move.l	4(%sp),%a0
	.short	0xA033
	ext.l	%d0
	rts
	.globl	get_tool_trap			| void *get_tool_trap(int trap)
get_tool_trap:
	move.w	6(%sp),%d0
	.short	0xA746			| _GetToolTrapAddress
	move.l	%a0,%d0
	rts
	.globl	get_sr				| the INFO block is shared with the drivers' logs: a line is
get_sr:					| written with interrupts masked, as the drivers do it
	moveq	#0,%d0
	move.w	%sr,%d0
	rts
	.globl	set_sr
set_sr:
	move.w	6(%sp),%sr
	rts
