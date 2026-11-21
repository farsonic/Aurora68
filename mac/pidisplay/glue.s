| mac/pidisplay/glue.s - PiDisplay's register-based and selector traps, for C. MPL-2.0.
| GCC keeps D2-D7/A2-A6 and the traps clobber D0-D2/A0-A1, so D2 is saved where a trap may touch it.

	.text
| unsigned short get_sr(void), void set_sr(unsigned short): the INFO block is shared with the drivers'
| logs, so a command is posted with interrupts masked (pdcommon.c)
	.globl	get_sr
get_sr:	moveq	#0,%d0
	move.w	%sr,%d0
	rts
	.globl	set_sr
set_sr:	move.w	6(%sp),%sr
	rts

| long mac_gestalt(long selector, long *answer): _Gestalt, the OSErr in D0
	.globl	mac_gestalt
mac_gestalt:
	move.l	4(%sp),%d0
	.short	0xA1AD
	move.l	8(%sp),%a1
	move.l	%a0,(%a1)
	ext.l	%d0
	rts

| short mac_findfolder(short vRefNum, long type, int create, short *vref, long *dirid):
| FindFolder (moveq #0,d0; _AliasDispatch $A823), System 7's Folder Manager
	.globl	mac_findfolder
mac_findfolder:
	move.l	%d2,-(%sp)
	clr.w	-(%sp)			| the result
	move.w	12(%sp),-(%sp)		| vRefNum: the low word of its slot (4 + 2, past the saved D2 and the result)
	move.l	16(%sp),-(%sp)		| folderType
	move.b	27(%sp),-(%sp)		| createFolder: the low byte of its slot, as a Pascal BOOLEAN's high byte
	move.l	30(%sp),-(%sp)		| &foundVRefNum
	move.l	38(%sp),-(%sp)		| &foundDirID
	moveq	#0,%d0
	.short	0xA823
	move.w	(%sp)+,%d0
	ext.l	%d0
	move.l	(%sp)+,%d2
	rts

| short mac_sysenvirons(void *rec): _SysEnvirons version 1 (sysVRefNum: the System Folder's working directory)
	.globl	mac_sysenvirons
mac_sysenvirons:
	move.l	4(%sp),%a0
	moveq	#1,%d0
	.short	0xA090
	ext.l	%d0
	rts

| void mac_restart(void): the Shutdown Manager's ShutDwnStart (selector 2): every program is asked to quit,
| the volumes are put away, the ROM's restart (the 68k's RESET instruction) follows
	.globl	mac_restart
mac_restart:
	move.l	%d2,-(%sp)
	move.w	#2,-(%sp)
	.short	0xA895
	move.l	(%sp)+,%d2
	rts

| the HFS calls PiDisplay needs beyond the kit's (mac/c/crt0.s): a parameter block in A0, the result in D0
	.macro	pbcall name, trap
	.globl	\name
\name:	move.l	4(%sp),%a0
	.short	\trap
	ext.l	%d0
	rts
	.endm
	pbcall	mac_PBRead, 0xA002
	pbcall	mac_PBHOpen, 0xA200
	pbcall	mac_PBHCreate, 0xA208
	pbcall	mac_PBHGetFInfo, 0xA20C
	pbcall	mac_PBHSetFInfo, 0xA20D

| void mac_disposeptr(void *p): _DisposPtr
	.globl	mac_disposeptr
mac_disposeptr:
	move.l	4(%sp),%a0
	.short	0xA01F
	rts
