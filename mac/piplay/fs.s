| SPDX-License-Identifier: MPL-2.0
| mac/piplay/fs.s - the File Manager calls PiPlay needs beyond mac/c/crt0.s's: PBRead and PBGetEOF,
| synchronous, a parameter block in A0, the result in D0 (and the block's ioResult). Here rather
| than in crt0.s so that the other C programs' binaries stay as they are.
	.text
	.globl	mac_PBRead
mac_PBRead:
	move.l	4(%sp),%a0
	.short	0xA002			| _Read
	ext.l	%d0
	rts
	.globl	mac_PBGetEOF
mac_PBGetEOF:
	move.l	4(%sp),%a0
	.short	0xA011			| _GetEOF: the length in ioMisc
	ext.l	%d0
	rts

| The HFS forms of Open, Create, GetFileInfo and SetFileInfo (a directory ID in ioDirID, offset 48),
| for the preferences file
	.macro	pbcall name, trap
	.globl	\name
\name:	move.l	4(%sp),%a0
	.short	\trap
	ext.l	%d0
	rts
	.endm
	pbcall	mac_PBHOpen, 0xA200
	pbcall	mac_PBHCreate, 0xA208
	pbcall	mac_PBHGetFInfo, 0xA20C
	pbcall	mac_PBHSetFInfo, 0xA20D

| short mac_FindFolder(short vRefNum, long type, int create, short *foundVRefNum, long *foundDirID):
| the Folder Manager (System 7), a Pascal function whose selector goes in D0 (moveq #0, _AliasDispatch)
	.globl	mac_FindFolder
mac_FindFolder:
	movem.l	%d2/%a2,-(%sp)		| a Pascal trap may change D2, which GCC expects kept
	lea	12(%sp),%a2		| the first argument's slot
	clr.w	-(%sp)			| room for the OSErr
	move.w	2(%a2),-(%sp)		| vRefNum
	move.l	4(%a2),-(%sp)		| folderType
	moveq	#0,%d0
	move.b	11(%a2),%d0
	lsl.w	#8,%d0
	move.w	%d0,-(%sp)		| createFolder: a BOOLEAN in the high byte
	move.l	12(%a2),-(%sp)		| VAR foundVRefNum
	move.l	16(%a2),-(%sp)		| VAR foundDirID
	moveq	#0,%d0
	.short	0xA823			| _FindFolder
	move.w	(%sp)+,%d0
	ext.l	%d0
	movem.l	(%sp)+,%d2/%a2
	rts
