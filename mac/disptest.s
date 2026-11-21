| disptest.s — "DisposeTest": a pointer and a handle disposed of once too often, in a Mac with more
| than 16 MB in 32-bit mode, with the 24-bit image of them filled as the user's SE had it. MPL-2.0.
| GNU as MIT syntax, 68020, packaged by tools/mac/mkapp.py (make -C mac DisposeTest; not part of
| `all`: it is a test, and it writes for a moment into memory that is not its own).
|
| What it reproduces (docs/08 item 78). Speedometer 3.23 disposed of a pointer twice. The ROM's
| Memory Manager ($4080DC2E) checks the block in the 32-bit layout, finds it free (-111) and checks
| it again in the 24-bit layout at the address masked to 24 bits. With 128 MB the pointer was
| $07FD7A4E and the masked address $00FD7A4E another piece of RAM, and that RAM, like the zone its
| header pointed to ($00555555), held nothing but $55 bytes: the 24-bit check passed and the
| dispatcher jumped through a $55555599 table entry to $FFFFFFFF ("bad F-Line instruction").
|
| What it does, logged through the Pi's INFO service ([MAC] DisposeTest: ...):
| 1. the ordinary calls: NewPtr, GetPtrSize, DisposPtr, NewHandle, DisposHandle (all must say 0);
| 2. DisposPtr of the freed pointer again, memory as it is (-111), in either mode;
| 3. the same with (pointer & $00FFFFFF) - 16 .. + 64 and $00555555 - 16 .. + 96 filled with $55,
|    interrupts off, the bytes put back afterwards. The unpatched ROM jumps to $FFFFFFFF here; the
|    patched one returns -111;
| 4. DisposHandle of a bad handle above 16 MB (one whose master pointer points at a free block
|    header) with its 24-bit image filled the same way: the handle dispatcher's retry ($4080DC6E).
| Steps 3 and 4 run only in 32-bit mode with the pointer above 16 MB; otherwise the program says so.
| It then quits (_ExitToShell): put it in Startup Items and read the kernel's log.

	.text
	.globl	app_start

	.equ	_NewPtr,	0xA11E
	.equ	_NewPtrClear,	0xA31E
	.equ	_DisposPtr,	0xA01F
	.equ	_GetPtrSize,	0xA021
	.equ	_NewHandle,	0xA122
	.equ	_DisposHandle,	0xA023
	.equ	_ExitToShell,	0xA9F4
	.equ	MMU32Bit,	0x0CB2
	.equ	MemErr,		0x0220
	.equ	FILLZONE,	0x00555555	| the zone pointer a $55 fill makes, masked to 24 bits
	.equ	SAVE1,		80		| bytes saved and filled around the masked block: -16 .. +64
	.equ	SAVE2,		112		| and around the fill's zone: -16 .. +96 (its fields at +32, +56)

| ---- log through the Pi's INFO service: `log "text"` ----
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
| `logv "text"`: the text, a space and d0 in hex
	.macro	logv text
	movem.l	%d0-%d1/%a0-%a1,-(%sp)
	lea	9f(%pc),%a1
	bsr	do_logv
	bra.s	8f
9:	.ascii	"\text"
	.byte	0
	.even
8:	movem.l	(%sp)+,%d0-%d1/%a0-%a1
	.endm

| registers: d6 = the pointer, d7 = the bad handle's block, a2 = the save area (SAVE1 + SAVE2 bytes)
app_start:
	log	"DisposeTest: start"
	| the save area and the bad handle's block first: nothing may be allocated once the pointer is
	| freed, or the Memory Manager hands its block out again and the second DisposPtr is legitimate
	move.l	#SAVE1+SAVE2,%d0
	.short	_NewPtrClear
	move.l	%a0,%d0
	beq	no_memory
	move.l	%a0,%a2
	move.l	#256,%d0
	.short	_NewPtrClear
	move.l	%a0,%d0
	beq	no_memory
	move.l	%a0,%d7
	| 1. the ordinary calls (the handle first: the freed pointer's block must stay free)
	move.l	#64,%d0
	.short	_NewHandle
	logv	"DisposeTest: NewHandle(64)"
	.short	_DisposHandle
	logv	"DisposeTest: DisposHandle, 0 expected:"
	move.l	#64,%d0
	.short	_NewPtr
	logv	"DisposeTest: NewPtr(64)"
	move.l	%a0,%d6
	move.l	%d6,%d0
	logv	"DisposeTest: the pointer"
	move.l	%d6,%a0
	.short	_GetPtrSize
	logv	"DisposeTest: GetPtrSize, 40 expected:"
	move.l	%d6,%a0
	.short	_DisposPtr
	logv	"DisposeTest: DisposPtr, 0 expected:"
	moveq	#0,%d0
	move.b	MMU32Bit,%d0
	logv	"DisposeTest: MMU32Bit"
	| 2. the freed pointer once more, memory untouched (in either mode: the ROM's own path)
	move.l	%d6,%a0
	.short	_DisposPtr
	logv	"DisposeTest: DisposPtr again, -111 expected:"
	tst.b	MMU32Bit
	beq	not_here
	cmp.l	#0x00ffffff,%d6
	bls	not_here
	| 3. the same with the pointer's 24-bit image and the fill's zone made of $55
	move.l	%d6,%d0
	and.l	#0x00ffffff,%d0
	logv	"DisposeTest: 24-bit image of the pointer"
	move.l	0x55555599,%d0		| what the fill's table entry reads as: where the unpatched ROM jumps
	logv	"DisposeTest: the long at $55555599"
	move.l	%d6,%d0
	and.l	#0x00ffffff,%d0
	sub.l	#16,%d0
	move.l	%d0,%a3			| a3 = the first region
	lea	FILLZONE-16,%a4		| a4 = the second
	move.w	%sr,-(%sp)
	ori.w	#0x0700,%sr		| nothing else may run while foreign memory holds the fill
	bsr	save_fill
	move.l	%d6,%a0
	.short	_DisposPtr
	move.l	%d0,%d5
	bsr	restore
	move.w	(%sp)+,%sr
	move.l	%d5,%d0
	logv	"DisposeTest: DisposPtr with the $55 fill, -111 expected:"
	move.w	MemErr,%d0
	ext.l	%d0
	logv	"DisposeTest: MemErr"
	| 4. a bad handle above 16 MB: a long in our own cleared block (the handle) points at another
	| place in it (the master pointer's block) whose header, 12 bytes below, is zeros: a free block
	move.l	%d7,%a0
	lea	128(%a0),%a1
	move.l	%a1,64(%a0)		| the master pointer, at block + 64, points at block + 128
	lea	64(%a0),%a0
	move.l	%a0,%d4			| d4 = the handle
	move.l	%d4,%d0
	and.l	#0x00ffffff,%d0
	sub.l	#16,%d0
	move.l	%d0,%a3
	lea	FILLZONE-16,%a4
	move.w	%sr,-(%sp)
	ori.w	#0x0700,%sr
	bsr	save_fill
	move.l	%d4,%a0
	.short	_DisposHandle
	move.l	%d0,%d5
	bsr	restore
	move.w	(%sp)+,%sr
	move.l	%d5,%d0
	logv	"DisposeTest: DisposHandle of a bad handle, -111 expected:"
	move.l	%d7,%a0
	.short	_DisposPtr
	move.l	%a2,%a0
	.short	_DisposPtr
	log	"DisposeTest: done"
	.short	_ExitToShell
not_here:
	log	"DisposeTest: not in 32-bit mode with the pointer above 16 MB: steps 3 and 4 left out"
	.short	_ExitToShell
no_memory:
	log	"DisposeTest: no memory for the save area"
	.short	_ExitToShell

| save_fill: SAVE1 bytes at a3 and SAVE2 at a4 into the save area (a2), then both filled with $55
save_fill:
	move.l	%a2,%a0
	move.l	%a3,%a1
	move.w	#SAVE1-1,%d0
1:	move.b	(%a1),(%a0)+
	move.b	#0x55,(%a1)+
	dbf	%d0,1b
	move.l	%a4,%a1
	move.w	#SAVE2-1,%d0
2:	move.b	(%a1),(%a0)+
	move.b	#0x55,(%a1)+
	dbf	%d0,2b
	rts
| restore: the bytes back where save_fill took them
restore:
	move.l	%a2,%a0
	move.l	%a3,%a1
	move.w	#SAVE1-1,%d0
1:	move.b	(%a0)+,(%a1)+
	dbf	%d0,1b
	move.l	%a4,%a1
	move.w	#SAVE2-1,%d0
2:	move.b	(%a0)+,(%a1)+
	dbf	%d0,2b
	rts

| window(): a0 = the slot-9 window for the addressing mode the Mac is in now
window:
	lea	0xF9000000,%a0
	tst.b	MMU32Bit
	bne.s	1f
	lea	0x00900000,%a0
1:	rts
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
	move.l	#50000000,%d1		| the Pi reads the text when it gets to the command: wait for it
4:	cmp.l	4(%a0),%d0
	beq.s	5f
	subq.l	#1,%d1
	bne.s	4b
5:	rts
| do_logv: a1 = C string, d0 = value -> "text XXXXXXXX", put together in a buffer on the stack
do_logv:
	movem.l	%d2/%a2,-(%sp)
	lea	-160(%sp),%sp
	move.l	%sp,%a2
1:	move.b	(%a1)+,(%a2)+
	bne.s	1b
	move.b	#' ',-1(%a2)
	moveq	#7,%d2
2:	rol.l	#4,%d0
	move.b	%d0,%d1
	and.b	#15,%d1
	add.b	#'0',%d1
	cmp.b	#'9',%d1
	bls.s	3f
	addq.b	#7,%d1
3:	move.b	%d1,(%a2)+
	dbf	%d2,2b
	clr.b	(%a2)
	move.l	%sp,%a1
	bsr	do_log
	lea	160(%sp),%sp
	movem.l	(%sp)+,%d2/%a2
	rts
