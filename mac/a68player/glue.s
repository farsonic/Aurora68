| SPDX-License-Identifier: MPL-2.0
| mac/a68player/glue.s - Aurora68 Video's register-based calls beyond the kit's (crt0.s) and Aurora68 Play's
| fs.s: the Device Manager's Open, Control and Status for the card's driver .A68Video, and SetFPos.
| Written on 23 November 2026 (docs/08 item 112). Each takes a parameter block, answers in D0.
	.text
	.macro	pbcall name, trap
	.globl	\name
\name:	move.l	4(%sp),%a0
	.short	\trap
	ext.l	%d0
	rts
	.endm
	pbcall	drv_open, 0xA000		| _Open (a driver's name)
	pbcall	drv_control, 0xA004		| _Control
	pbcall	drv_status, 0xA005		| _Status
	pbcall	mac_SetFPos, 0xA044		| _SetFPos
