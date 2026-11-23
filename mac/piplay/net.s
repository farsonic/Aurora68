| SPDX-License-Identifier: MPL-2.0
| mac/piplay/net.s - the calls PiPlay's radio (radio.c) makes that the C kit's trap list cannot express:
| the Device Manager's Control call to MacTCP's .IPP driver, synchronous and asynchronous; the
| completion routine of the receive call; the resolver's code resource called the way Apple's DNR glue
| calls it, and its result procedure; HLock; Gestalt, which asks for Riptide's .IPP6 (the Control calls
| serve .IPP6 as they serve .IPP). Written on 21 November 2026 (docs/08 items 83 and 88).
|
| Register conventions met here, from Inside Macintosh and the MacTCP Programmer's Guide:
| - _Control takes the parameter block in A0 and answers in D0 (asynchronous: at once, with ioResult 1
|   until the call completes).
| - A completion routine of a TCP call is called by MacTCP as a C function of the parameter block
|   (MacTCP.h: TCPIOCompletionProc), at interrupt or deferred-task time; Riptide also passes the block in
|   A0. PiPlay has one receive outstanding at a time and needs neither: the stub saves every register,
|   because the caller may be interrupt code that expects them kept, and calls C.
| - The 'dnrp' resource is MPW C code called as (*dnr)(selector, ...): the arguments as longs on the
|   stack, the caller pops them, the result in D0. MPW C treats D0-D2/A0-A1 as scratch and GCC expects
|   D2 kept, so the glue keeps D2-D7/A2-A6 itself.
| - The result procedure the resolver calls is Pascal: (hostInfoPtr, userDataPtr) pushed left to right,
|   popped by the procedure. PiPlay's writes 1 into the long at userDataPtr and returns.
	.text

| short mac_PBControl(void *pb), short mac_PBControlAsync(void *pb)
	.globl	mac_PBControl
mac_PBControl:
	move.l	4(%sp),%a0
	.short	0xA004			| _Control
	ext.l	%d0
	rts
	.globl	mac_PBControlAsync
mac_PBControlAsync:
	move.l	4(%sp),%a0
	.short	0xA404			| _Control,async
	ext.l	%d0
	rts
| short mac_PBStatus(void *pb): the video decoder card's driver's Status 100 (vcard.c, 23 November 2026)
	.globl	mac_PBStatus
mac_PBStatus:
	move.l	4(%sp),%a0
	.short	0xA005			| _Status
	ext.l	%d0
	rts

| the receive call's completion routine: radio.c's c_rcv_done(), every register kept
	.globl	mac_rcv_completion
mac_rcv_completion:
	movem.l	%d0-%d7/%a0-%a6,-(%sp)
	bsr.l	c_rcv_done
	movem.l	(%sp)+,%d0-%d7/%a0-%a6
	rts

| long mac_dnr_call(void *code, long sel, long a, long b, long c, long d): the resolver's entry point
| with five longs (calls that take fewer ignore the rest)
	.globl	mac_dnr_call
mac_dnr_call:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)	| 44 bytes; code at 48(sp), sel at 52 ... d at 68
	move.l	68(%sp),-(%sp)		| d: each push moves the next argument to the same offset
	move.l	68(%sp),-(%sp)		| c
	move.l	68(%sp),-(%sp)		| b
	move.l	68(%sp),-(%sp)		| a
	move.l	68(%sp),-(%sp)		| sel
	move.l	68(%sp),%a0		| code
	jsr	(%a0)
	lea	20(%sp),%sp
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts

| pascal void mac_dnr_result(struct hostInfo *hi, char *userData): *(long *)userData = 1
	.globl	mac_dnr_result
mac_dnr_result:
	move.l	4(%sp),%a0		| userDataPtr, pushed last
	move.l	#1,(%a0)
	move.l	(%sp)+,%a0
	addq.l	#8,%sp
	jmp	(%a0)

| void mac_HLock(void *h), mac_HUnlock(void *h), mac_DisposPtr(void *p), mac_DisposHandle(void *h): register-based traps
	.globl	mac_HLock
mac_HLock:
	move.l	4(%sp),%a0
	.short	0xA029			| _HLock
	rts
	.globl	mac_HUnlock
mac_HUnlock:
	move.l	4(%sp),%a0
	.short	0xA02A			| _HUnlock
	rts
	.globl	mac_DisposPtr
mac_DisposPtr:
	move.l	4(%sp),%a0
	.short	0xA01F			| _DisposPtr
	rts
	.globl	mac_DisposHandle
mac_DisposHandle:
	move.l	4(%sp),%a0
	.short	0xA023			| _DisposHandle
	rts

| long mac_Gestalt(long selector, long *response): _Gestalt is register-based (the selector in D0, the
| answer in A0, the result in D0); System 6.0.4 and later have it (radio.c checks SysVersion first)
	.globl	mac_Gestalt
mac_Gestalt:
	move.l	4(%sp),%d0
	.short	0xA1AD			| _Gestalt
	move.l	8(%sp),%a1
	move.l	%a0,(%a1)
	ext.l	%d0
	rts
