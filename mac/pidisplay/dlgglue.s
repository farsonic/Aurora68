| mac/pidisplay/dlgglue.s - the dialogs' Pascal callbacks for C, shared by PiDisplay and PiScrn
| (pdcommon.c). MPL-2.0. The same stubs as the WiFi menu's (mac/wifimenu/entry.s).

	.text
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
| void **mac_newhandle(long size): _NewHandle in the current heap, 0 when there is no room
	.globl	mac_newhandle
mac_newhandle:
	move.l	4(%sp),%d0
	.short	0xA122
	move.l	%a0,%d0
	rts
