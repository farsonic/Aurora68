| SPDX-License-Identifier: MPL-2.0
| mac/vdec/entry.s - the way into the Aurora68 Video Decoder's QuickTime components, and the calls the C
| side makes that the kit's trap list cannot express. Written on 22 November 2026 (docs/08 item 110).
|
| The component's code is one resource ('cdec' 128, tools/mac/elf2thng.py) named by four 'thng'
| resources, one per format. The Component Manager loads it, and may move it or load it again between
| calls; it calls it as
|     pascal ComponentResult main(ComponentParameters *params, Handle storage)
| (Inside Macintosh: More Macintosh Toolbox, "Component Manager"): the result's room, params, storage
| pushed in that order, the routine pops its eight bytes of arguments. The image is linked at 0 with every
| 32-bit absolute field listed after it (tools/mac/macelf.py); on entry it adds the distance from where it
| was last fixed up (_reloc_base: 0 as it comes from the file) to where it is now, so a fresh copy or a
| moved one is right before any C runs, then flushes the instruction cache. Then c_dispatch(params,
| storage) in C.
|
| The calls below are Pascal (the Component Manager's, through _ComponentDispatch with the selector in
| D0, Inside Macintosh's numbers) or register-based (the Device and Memory Managers). Each keeps D2,
| which GCC expects kept and the Toolbox may change.

	.text
	.globl	_start
	.globl	_reloc_off
_start:
	bra.s	1f
_reloc_off:
	.long	0			| filled in by macelf.py: the relocation table's offset
_reloc_base:
	.long	0			| the address the fields were last fixed up for
1:	movem.l	%d2-%d7/%a2-%a6,-(%sp)	| 44 bytes: storage at 48(sp), params at 52, the result at 56
	lea	_start(%pc),%a3
	move.l	%a3,%d4
	sub.l	_reloc_base(%pc),%d4	| the distance moved
	beq.s	3f
	move.l	_reloc_off(%pc),%d2
	lea	(%a3,%d2.l),%a4
	move.l	(%a4)+,%d3
	bra.s	5f
4:	move.l	(%a4)+,%d2
	add.l	%d4,(%a3,%d2.l)
5:	subq.l	#1,%d3
	bcc.s	4b
	lea	_reloc_base(%pc),%a0
	move.l	%a3,(%a0)
	moveq	#1,%d0
	.short	0xA198			| _HWPriv 1: FlushInstructionCache
3:	move.l	48(%sp),-(%sp)		| storage
	move.l	56(%sp),-(%sp)		| params (52 + 4)
	jsr	c_dispatch
	addq.l	#8,%sp
	move.l	%d0,56(%sp)		| the ComponentResult
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	move.l	(%sp)+,%a0
	addq.l	#8,%sp
	jmp	(%a0)

| ---- the Component Manager (Components.h numbers: _ComponentDispatch $A82A, the selector in D0) -------
| void cm_set_storage(long ci, long storage)
	.globl	cm_set_storage
cm_set_storage:
	move.l	%d2,-(%sp)
	move.l	8(%sp),-(%sp)		| aComponentInstance
	move.l	16(%sp),-(%sp)		| theStorage (12 + 4)
	moveq	#0x0D,%d0		| SetComponentInstanceStorage
	.short	0xA82A
	move.l	(%sp)+,%d2
	rts
| long cm_find_next(long comp, void *desc): FindNextComponent
	.globl	cm_find_next
cm_find_next:
	move.l	%d2,-(%sp)
	clr.l	-(%sp)			| the result's room
	move.l	12(%sp),-(%sp)		| aComponent (8 + 4)
	move.l	20(%sp),-(%sp)		| looking (12 + 8)
	moveq	#0x04,%d0
	.short	0xA82A
	move.l	(%sp)+,%d0
	move.l	(%sp)+,%d2
	rts
| long cm_open(long comp): OpenComponent
	.globl	cm_open
cm_open:
	move.l	%d2,-(%sp)
	clr.l	-(%sp)
	move.l	12(%sp),-(%sp)
	moveq	#0x07,%d0
	.short	0xA82A
	move.l	(%sp)+,%d0
	move.l	(%sp)+,%d2
	rts
| short cm_get_info(long comp, void *desc): GetComponentInfo, without the name, info and icon handles (an
| instance does for its component)
	.globl	cm_get_info
cm_get_info:
	move.l	%d2,-(%sp)
	clr.w	-(%sp)
	move.l	10(%sp),-(%sp)		| aComponent (8 + 2)
	move.l	18(%sp),-(%sp)		| cd (12 + 6)
	clr.l	-(%sp)			| componentName
	clr.l	-(%sp)			| componentInfo
	clr.l	-(%sp)			| componentIcon
	moveq	#0x05,%d0		| GetComponentInfo (Components.h: $7005; $7006 is GetComponentListModSeed)
	.short	0xA82A
	move.w	(%sp)+,%d0
	ext.l	%d0
	move.l	(%sp)+,%d2
	rts
| void cm_close(long ci): CloseComponent (an OSErr, not looked at)
	.globl	cm_close
cm_close:
	move.l	%d2,-(%sp)
	clr.w	-(%sp)
	move.l	10(%sp),-(%sp)		| ci (8 + 2)
	moveq	#0x08,%d0
	.short	0xA82A
	addq.l	#2,%sp
	move.l	(%sp)+,%d2
	rts
| long cm_delegate(void *params, long ci): DelegateComponentCall
	.globl	cm_delegate
cm_delegate:
	move.l	%d2,-(%sp)
	clr.l	-(%sp)
	move.l	12(%sp),-(%sp)		| originalParams
	move.l	20(%sp),-(%sp)		| ci
	moveq	#0x24,%d0
	.short	0xA82A
	move.l	(%sp)+,%d0
	move.l	(%sp)+,%d2
	rts
| void call_completion(void *proc, int err, int flags, long refcon): pascal void (OSErr, short, long)
	.globl	call_completion
call_completion:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)	| a routine of QuickTime's: keep everything
	move.l	48(%sp),%a0		| proc
	move.w	54(%sp),-(%sp)		| result (the low word of the int at 52)
	move.w	60(%sp),-(%sp)		| flags (56 + 2 + 2)
	move.l	64(%sp),-(%sp)		| refcon (60 + 4)
	jsr	(%a0)
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts

| ---- the Device Manager and the Memory Manager (register-based) ---------------------------------------
| short dm_open(void *pb), dm_control(void *pb), dm_status(void *pb): synchronous, the result in D0
	.globl	dm_open
dm_open:
	move.l	4(%sp),%a0
	.short	0xA000			| _Open
	ext.l	%d0
	rts
	.globl	dm_control
dm_control:
	move.l	4(%sp),%a0
	.short	0xA004			| _Control
	ext.l	%d0
	rts
	.globl	dm_status
dm_status:
	move.l	4(%sp),%a0
	.short	0xA005			| _Status
	ext.l	%d0
	rts
| void *mm_newptr_clear(long size): NewPtrClear in the current heap (the program's that opened the codec);
| the pointer in A0 (where GCC reads it) and D0
	.globl	mm_newptr_clear
mm_newptr_clear:
	move.l	4(%sp),%d0
	.short	0xA31E			| _NewPtr,clear
	move.l	%a0,%d0
	rts
| void mm_disposeptr(void *p)
	.globl	mm_disposeptr
mm_disposeptr:
	move.l	4(%sp),%a0
	.short	0xA01F			| _DisposPtr
	rts
| long mm_handlesize(void *h)
	.globl	mm_handlesize
mm_handlesize:
	move.l	4(%sp),%a0
	.short	0xA025			| _GetHandleSize
	rts
| unsigned long tm_micro(void): the low half of _Microseconds (System 7), for the time a picture takes
	.globl	tm_micro
tm_micro:
	.short	0xA193			| _Microseconds: the high half in A0, the low in D0
	rts
| void *mm_strip(void *p): StripAddress (a 24-bit Mac's master pointers carry flags in their top byte)
	.globl	mm_strip
mm_strip:
	move.l	4(%sp),%d0
	.short	0xA055			| _StripAddress
	move.l	%d0,%a0			| GCC reads a pointer result from A0 (mac/c/README.md)
	rts
