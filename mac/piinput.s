| piinput.s — ".PiInput" driver: keyboards and mice attached to the Pi become Macintosh input.
| MPL-2.0. 68k, GNU as MIT syntax, assembled with -m68020.
|
| Why this exists. The Macintosh SE talks to its keyboard and mouse through ADB (the VIA and the
| IIci ROM's ADB Manager do that work, unchanged by this project). The Pi, on the other side of the
| PiStorm, can also have keyboards and mice: USB ones on a Pi 4 through Alice68's xHCI driver, and
| Bluetooth ones through Alice68's HID-over-HCI code. Those drivers deliver their reports to the
| service core (ARM core 3), which writes one 16-byte record per key change or mouse movement into
| the input ring at slot-9 window + 0x51000 (emu68/src/alice/alice_psmac.h). This driver drains
| that ring from a VBL task, sixty times a second, and feeds the Event Manager and the cursor
| exactly as the ADB Manager would, so every application sees ordinary key and mouse events and
| cannot tell which keyboard they came from.
|
| How a key becomes an event. The ring carries USB HID usages (the "boot protocol" codes every
| keyboard speaks: 0x04 = A, 0x28 = Return ...). The Mac wants two things in a keyDown message:
| the virtual key code in bits 8-15 (the Apple layout-independent code, 0x00 = A, 0x24 = Return)
| and the character in bits 0-7. The virtual code comes from the usb_to_mac table below (Inside
| Macintosh: Text, "Virtual Key Codes", against the USB HID Usage Tables 1.12 keyboard page). The
| character comes from a KCHR resource read directly (Inside Macintosh: Text, "KCHR resource":
| the modifier byte picks a table, the virtual code picks the character; dead keys not yet), for
| now the ROM's U.S. layout, later the one chosen in the Keyboard control panel. Modifier
| state (Shift, Option, Command, Control, Caps Lock) is kept in the driver from the HID modifier
| byte and OR'd into the event's modifiers word the way the ADB Manager does.
|
| How a mouse moves. The Event Manager does not have a "mouse moved" event; the cursor follows the
| low-memory point RawMouse/MTemp and the cursor task redraws it when CrsrNew is set (Inside
| Macintosh I, "The Mouse"; Macintosh Technical Note "Mouse Movement" explains the coupling). The
| driver adds the movement to RawMouse and MTemp (clipped to the rectangle around every active
| screen), sets CrsrNew, and posts mouseDown/mouseUp when the button bits change, mirroring
| MBState so that _Button and _StillDown answer correctly. A second or third button is reported as Option-click
| and Command-click, which is what Apple's own multi-button drivers of the day did.
|
| What is not done here. Scroll wheels (ALICE_IN_WHEEL) are dropped: System 6 and 7 have no notion
| of them. Key repeat is left to the Mac: the Event Manager generates autoKey events from the
| keyboard's own repeat in the ADB world, so the Pi side repeats keys (the service core does that
| from its timer), and this driver turns a repeated "down" into an autoKey event.
|
| Loaded from the slot-9 declaration ROM (sResource 0x82, fOpenAtStart), so it exists from the boot
| screen onwards, like a third-party ADB-equivalent card driver of 1990 would have.

	.text
	.globl	drvr_start

| ---- the input ring (alice_psmac.h) ----
	.equ	WIN32,		0xF9000000
	.equ	WIN24,		0x00900000
	.equ	PSMB_MAGIC,	0x50534D42
	.equ	OFF_INPUT,	0x51000
	.equ	IN_MAGIC,	0x41364949	| 'A6II'
	.equ	IN_SLOTS,	64
	.equ	IN_ATTACHED,	12
	.equ	IN_HEAD,	16
	.equ	IN_TAIL,	20
	.equ	IN_EV,		32
	.equ	EV_TYPE,	0
	.equ	EV_A,		4
	.equ	EV_B,		8
	.equ	EV_C,		12
	.equ	EV_SIZE,	16
	.equ	IN_KEY,		1
	.equ	IN_MOUSE,	2
	.equ	IN_WHEEL,	3
	.equ	IN_MOUSEABS,	4		| the remote control (docs/13): a = x, b = y, c = buttons or -1
	.equ	IN_MACKEY,	5		| the remote control: a = Mac virtual key code, b = down/up, c = HID modifiers

| ---- Mac OS low memory and traps (Inside Macintosh, MPW SysEqu.a) ----
	.equ	MMU32Bit,	0x0CB2
	.equ	MBState,	0x0172		| byte: $80 = button up
	.equ	MTemp,		0x0828		| Point: unprocessed mouse position
	.equ	RawMouse,	0x082C		| Point: unprocessed mouse position
	.equ	CrsrNew,	0x08CE		| byte: nonzero when the cursor must be redrawn
	.equ	CrsrCouple,	0x08CF		| byte: cursor coupled to the mouse
	.equ	CrsrPin,	0x0834		| Rect: cursor limits
	.equ	DeviceList,	0x08A8		| Handle: the first GDevice (Color QuickDraw's screens)
	.equ	KeyMap,		0x0174		| 16 bytes: keys down, one bit per virtual code
	.equ	KeyLast,	0x0184		| word: last key code
	.equ	KeyTime,	0x0186		| long: when it went down
	.equ	KeyRepTime,	0x018A		| long: last repeat
	.equ	Ticks,		0x016A
	.equ	_GetResource,	0xA9A0
	.equ	_GetHandleSize,	0xA025
	.equ	_NewPtrSys,	0xA51E
	.equ	_StripAddress,	0xA055
	.equ	dCtlStorage,	20
	.equ	noErr,		0
	.equ	ioErr,		-36
	.equ	controlErr,	-17
	.equ	statusErr,	-18
	.equ	_NewPtrSysClear,	0xA71E
	.equ	_VInstall,	0xA033
	.equ	_PostEvent,	0xA02F
	.equ	mouseDown,	1
	.equ	mouseUp,	2
	.equ	keyDown,	3
	.equ	keyUp,		4
	.equ	autoKey,	5
	| event modifier bits (Inside Macintosh I "Event Record")
	.equ	btnState,	0x0080
	.equ	cmdKey,		0x0100
	.equ	shiftKey,	0x0200
	.equ	alphaLock,	0x0400
	.equ	optionKey,	0x0800
	.equ	controlKey,	0x1000

| ---- driver globals (dCtlStorage) ----
	.equ	G_WIN,		0		| long: window base
	.equ	G_RING,		4		| long: ring base
	.equ	G_VBL,		8		| VBLTask: qLink 4, qType 2, vblAddr 4, vblCount 2, vblPhase 2
	.equ	G_SELF,		G_VBL+14	| long: globals pointer, read by the VBL task through a0
	.equ	G_MODS,		G_SELF+4	| word: current modifier bits in event-record form
	.equ	G_BUTTONS,	G_MODS+2	| word: last mouse buttons from the ring
	.equ	G_DEAD,		G_BUTTONS+2	| long: dead-key state (unused until dead keys are composed)
	.equ	G_CAPS,		G_DEAD+4	| word: caps lock toggled on
	.equ	G_SIQ,		G_CAPS+2	| SlotIntQElement: sqLink 4, sqType 2, sqPrio 2, sqAddr 4, sqParm 4
	.equ	G_INTS,		G_SIQ+16	| long: slot interrupts taken (for the log)
	.equ	G_SIQ2,		G_INTS+4	| a second queue element: the catch-all handler (below)
	.equ	G_KCHR,		G_SIQ2+16	| long: the keyboard layout (KCHR data), 0 = built-in table
	.equ	G_SIZE,		G_KCHR+4
	.equ	_SIntInstall,	0xA075		| its own OS trap (Inside Macintosh V): A0 = SlotIntQElement, D0.w = slot
	.equ	sIQType,	6
	.equ	PI_SLOT,	9

| ---- DRVR header ----
drvr_start:
	.short	0x4C00			| dNeedLock|dStatEnable|dCtlEnable
	.short	0
	.short	0
	.short	0
	.short	d_open-drvr_start
	.short	d_prime-drvr_start
	.short	d_control-drvr_start
	.short	d_status-drvr_start
	.short	d_close-drvr_start
	.byte	8
	.ascii	".PiInput"
	.even

| ---- log through the Pi's INFO service (same macro as pidisk.s; labels 8 and 9 are its own) ----
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
	move.l	#200000,%d1
4:	cmp.l	4(%a0),%d0
	beq.s	5f
	subq.l	#1,%d1
	bne.s	4b
5:	rts

| log_err: d0.w = OSErr -> the log line "PiInput: SIntInstall error -NNN" (digits written into the
| text; the driver runs from a RAM copy the Slot Manager made, so the text is writable)
log_err:
	movem.l	%d0-%d2/%a0-%a1,-(%sp)
	lea	err_text(%pc),%a1
	lea	err_digits(%pc),%a0
	move.w	%d0,%d1
	ext.l	%d1
	move.b	#' ',(%a0)
	tst.l	%d1
	bpl.s	1f
	neg.l	%d1
	move.b	#'-',(%a0)
1:	moveq	#2,%d2			| three digits, most significant first
	move.l	%d1,%d0
	divu	#100,%d0
	add.b	#'0',%d0
	move.b	%d0,1(%a0)
	clr.w	%d0
	swap	%d0
	divu	#10,%d0
	add.b	#'0',%d0
	move.b	%d0,2(%a0)
	swap	%d0
	add.b	#'0',%d0
	move.b	%d0,3(%a0)
	bsr	do_log
	movem.l	(%sp)+,%d0-%d2/%a0-%a1
	rts
err_text:
	.ascii	"PiInput: SIntInstall error"
err_digits:
	.ascii	" 000"
	.byte	0
	.even

| window: a0 = the slot-9 window for the current addressing mode
window:
	lea	WIN32,%a0
	tst.b	MMU32Bit
	bne.s	1f
	lea	WIN24,%a0
1:	rts

| ---- Open: find the ring, learn the keyboard layout, start the VBL task ----
d_open:
	movem.l	%d1-%d2/%a0-%a3,-(%sp)
	move.l	%a1,%a3			| DCE
	tst.l	dCtlStorage(%a3)
	bne	open_ok
	move.l	#G_SIZE,%d0
	.short	_NewPtrSysClear
	move.l	%a0,%d0
	beq	open_fail
	move.l	%a0,dCtlStorage(%a3)
	move.l	%a0,%a2
	bsr	window
	move.l	%a0,G_WIN(%a2)
	cmp.l	#PSMB_MAGIC,(%a0)
	bne	open_fail
	lea	OFF_INPUT(%a0),%a0
	move.l	%a0,G_RING(%a2)
	cmp.l	#IN_MAGIC,(%a0)
	bne	open_fail		| the service core has not published a ring
	move.l	IN_HEAD(%a0),IN_TAIL(%a0)	| start from now: drop what happened before boot
	move.w	#0x8000,G_BUTTONS(%a2)	| "unknown" so the first report sets MBState
	log	"PiInput: ring found"
	| the keyboard layout: 'KCHR' 0 (U.S.) from the resource chain (the System file's or the
	| ROM's), copied into a block of our own so its address can be used from interrupt level.
	| (The first version read the low-memory long at $1B40, documented as KCHRPtr: under 7.1 on
	| this ROM it holds the address of a code patch, and every key came out as a random letter.)
	clr.l	-(%sp)
	move.l	#0x4B434852,-(%sp)	| 'KCHR'
	clr.w	-(%sp)			| ID 0
	.short	_GetResource
	move.l	(%sp)+,%d0
	beq.s	1f
	move.l	%d0,%a0
	.short	_GetHandleSize		| a0 = handle -> d0 = size
	move.l	%d0,%d1
	ble.s	1f
	move.l	%a0,%a1
	.short	_NewPtrSys		| our own copy: the resource's handle is the Script Manager's to
	move.l	%a0,%d0			| release, move or purge, and a key arrives long after Open
	beq.s	1f
	move.l	%d0,G_KCHR(%a2)
	move.l	(%a1),%d0
	.short	_StripAddress		| Open runs in 24-bit mode: the master pointer carries flag bits in
	move.l	%d0,%a1			| its top byte (E0...); the copy is addressed in 32-bit mode later
2:	move.b	(%a1)+,(%a0)+
	subq.l	#1,%d1
	bne.s	2b
	| "PiInput: KCHR 0 at AAAAAAAA: VVVVVVVV TTTTTTTT" (the address, its first long, the first long
	| of table 0): which layout the driver got, for the log
	move.l	G_KCHR(%a2),%d0
	lea	kchr_text+19(%pc),%a0
	moveq	#7,%d1
	bsr	put_hex
	move.l	G_KCHR(%a2),%a1
	move.l	(%a1),%d0
	lea	kchr_text+29(%pc),%a0
	bsr	put_hex
	move.l	260(%a1),%d0
	lea	kchr_text+38(%pc),%a0
	bsr	put_hex
	lea	kchr_text(%pc),%a1
	bsr	do_log
1:
	| The card's interrupt: the kernel raises the slot-9 line while the ring holds events
	| (docs/07), the ROM's level-2 handler runs every routine queued for the slot with the Slot
	| Manager's SIntInstall, and ours drains the ring. _SIntInstall is an OS trap of its own
	| ($A075, not a _SlotManager selector: that was found the hard way), slot in D0.w, element in A0.
	lea	G_SIQ(%a2),%a0
	clr.l	(%a0)
	move.w	#sIQType,4(%a0)
	move.w	#100,6(%a0)		| sqPrio: ordinary
	lea	slot_int(%pc),%a1
	move.l	%a1,8(%a0)
	move.l	%a2,12(%a0)		| sqParm: our globals, handed back in A1
	move.w	#PI_SLOT,%d0
	.short	_SIntInstall
	tst.w	%d0
	beq.s	1f
	bsr	log_err			| "PiInput: SIntInstall error -NNN"
	bra.s	2f
1:	log	"PiInput: slot interrupt handler installed"
	| The catch-all. The ROM runs the slot's handlers from the highest sqPrio down and bombs
	| with "unserviceable slot interrupt" when every one of them answers 0. Each driver here
	| claims only what its own board flagged, and a flag can be consumed by a driver's VBL poll
	| between the line going up and the handlers running, so one evening the ROM found nobody
	| at home. A real card's driver knows its card raised the line; this one, at priority 0 and
	| therefore last, simply says so.
	lea	G_SIQ2(%a2),%a0
	clr.l	(%a0)
	move.w	#sIQType,4(%a0)
	move.w	#0,6(%a0)		| sqPrio 0: called after everyone else
	lea	slot_catchall(%pc),%a1
	move.l	%a1,8(%a0)
	move.l	%a2,12(%a0)
	move.w	#PI_SLOT,%d0
	.short	_SIntInstall
	move.l	G_RING(%a2),%a0
	move.l	#1,IN_ATTACHED(%a0)	| only now: from this write on, ring events raise the card's interrupt
2:	| The VBL task as well: every tick until the first slot interrupt arrives, then every 15
	| ticks as a safety net (a System 6 install without working slot interrupts still works,
	| at keyboard-and-mouse speed).
	move.l	%a2,G_SELF(%a2)
	lea	G_VBL(%a2),%a0
	clr.l	(%a0)
	move.w	#1,4(%a0)		| vType
	lea	input_vbl(%pc),%a1
	move.l	%a1,6(%a0)
	move.w	#60,10(%a0)		| first run a second after Open
	clr.w	12(%a0)
	.short	_VInstall
open_ok:
	moveq	#noErr,%d0
	bra.s	open_ret
open_fail:
	moveq	#ioErr,%d0
open_ret:
	movem.l	(%sp)+,%d1-%d2/%a0-%a3
	rts

d_prime:
	moveq	#ioErr,%d0
	rts
d_control:
	moveq	#controlErr,%d0
	bra	iodone
d_status:
	moveq	#statusErr,%d0
	bra	iodone
d_close:
	moveq	#noErr,%d0
	rts

| ---- the slot interrupt handler ----
| The Slot Manager calls it with A1 = sqParm (our globals) at interrupt level 2 and expects D0
| nonzero if the interrupt was ours; registers other than D0 are preserved here although the
| Slot Manager only asks for A4-A7 and D4-D7. PostEvent is interrupt-safe (the ADB Manager calls
| it from the VIA interrupt), so draining the ring here is no different from doing it in a VBL.
slot_int:
	movem.l	%d1-%d7/%a0-%a4,-(%sp)
	move.l	%a1,%a2
	addq.l	#1,G_INTS(%a2)
	cmp.l	#1,G_INTS(%a2)
	bne.s	1f
	log	"PiInput: first slot interrupt"
1:	bsr	drain			| d0 = events taken
	movem.l	(%sp)+,%d1-%d7/%a0-%a4
	rts

| the catch-all: "it was the card": D0 = 1 and nothing else
slot_catchall:
	moveq	#1,%d0
	rts

| ---- the VBL task ----
| On entry a0 -> our VBLTask record; the globals pointer sits 14 bytes after it. VBL tasks run at
| interrupt level 1 with the Event Manager's PostEvent allowed.
input_vbl:
	movem.l	%d0-%d7/%a0-%a4,-(%sp)
	move.l	14(%a0),%a2		| globals
	move.w	#15,%d0
	tst.l	G_INTS(%a2)
	bne.s	1f			| interrupts work: the line is level-triggered and stays up while the
	bsr	drain			| ring holds events, so nothing is lost by leaving them to the handler.
	move.w	#1,%d0			| (Draining from both: the VBL task read an event, the slot interrupt
1:	move.w	%d0,G_VBL+10(%a2)	| came in before it advanced the tail and posted the same key again.)
	movem.l	(%sp)+,%d0-%d7/%a0-%a4
	rts

| clip_desk: d2 = h, d3 = v clipped to the rectangle around every active screen: QuickDraw's
| GDevices from DeviceList ($8A8), gdFlags at 20 (screenActive is its bit 15), gdNextGD at 30, gdRect
| at 34. The ROM's cursor task moves the cursor onto whichever screen holds the point and pins it to
| the one it is on, as it does for an ADB mouse. Until 20 November this was CrsrPin, which is the
| rectangle of the screen the cursor is on, so a USB or Bluetooth mouse and the remote control never
| left the first screen of two (the SE board's HDMI screen, docs/10). CrsrPin is where the union
| starts, so a Mac without the list (or before its first device) clips as before.
clip_desk:
	movem.l	%d0-%d1/%d4-%d7/%a0,-(%sp)
	move.w	CrsrPin,%d4		| top
	move.w	CrsrPin+2,%d5		| left
	move.w	CrsrPin+4,%d6		| bottom
	move.w	CrsrPin+6,%d7		| right
	move.l	DeviceList,%d0
	moveq	#7,%d1			| eight devices at most
1:	tst.l	%d0
	beq.s	6f
	move.l	%d0,%a0
	move.l	(%a0),%a0		| the GDevice
	btst	#7,20(%a0)		| screenActive (the high byte of gdFlags)
	beq.s	5f
	cmp.w	34(%a0),%d4
	ble.s	2f
	move.w	34(%a0),%d4		| a higher top
2:	cmp.w	36(%a0),%d5
	ble.s	3f
	move.w	36(%a0),%d5		| further left
3:	cmp.w	38(%a0),%d6
	bge.s	4f
	move.w	38(%a0),%d6		| a lower bottom
4:	cmp.w	40(%a0),%d7
	bge.s	5f
	move.w	40(%a0),%d7		| further right
5:	move.l	30(%a0),%d0		| gdNextGD
	dbf	%d1,1b
6:	cmp.w	%d5,%d2
	bge.s	7f
	move.w	%d5,%d2
7:	cmp.w	%d4,%d3
	bge.s	8f
	move.w	%d4,%d3
8:	subq.w	#1,%d7
	cmp.w	%d7,%d2
	ble.s	9f
	move.w	%d7,%d2
9:	subq.w	#1,%d6
	cmp.w	%d6,%d3
	ble.s	10f
	move.w	%d6,%d3
10:	movem.l	(%sp)+,%d0-%d1/%d4-%d7/%a0
	rts

| ---- drain: a2 = globals -> d0 = events taken (at most 32 per call: leave time for others) ----
drain:
	movem.l	%d1-%d7/%a0-%a1/%a3-%a4,-(%sp)
	move.l	G_RING(%a2),%a3
	moveq	#0,%d0
	move.l	%d0,-(%sp)		| events taken, on the stack (d0..d5 are used by the event code)
	moveq	#31,%d7
vbl_loop:
	move.l	IN_TAIL(%a3),%d6
	cmp.l	IN_HEAD(%a3),%d6
	beq	vbl_done
	move.l	%d6,%d0
	and.l	#IN_SLOTS-1,%d0
	lsl.l	#4,%d0
	lea	IN_EV(%a3,%d0.l),%a4	| the event record
	move.l	EV_TYPE(%a4),%d0
	cmp.l	#IN_KEY,%d0
	beq	ev_key
	cmp.l	#IN_MOUSE,%d0
	beq	ev_mouse
	cmp.l	#IN_MOUSEABS,%d0
	beq	ev_mouseabs
	cmp.l	#IN_MACKEY,%d0
	beq	ev_mackey
	bra	ev_next			| wheel and unknown: dropped

| --- the remote control's key: a = Mac virtual key code already, b = down/up, c = HID modifiers ---
ev_mackey:
	move.l	EV_C(%a4),%d0
	bsr	hid_modifiers
	move.l	EV_A(%a4),%d1
	cmp.l	#0x7F,%d1
	bhi	ev_next
	and.w	#0x007F,%d1
	bra	key_code		| from here on as a USB key would go

| --- the remote control's mouse: a = x, b = y (global), c = buttons ---
ev_mouseabs:
	move.l	EV_A(%a4),%d2
	move.l	EV_B(%a4),%d3
	bra	mouse_clip

| --- a key: a = HID usage, b = 1 down / 0 up, c = HID modifier byte ---
ev_key:
	move.l	EV_C(%a4),%d0
	bsr	hid_modifiers		| d0 = event modifiers from the HID byte -> G_MODS
	move.l	EV_A(%a4),%d0
	beq	ev_next			| modifiers-only report
	cmp.l	#0xE8,%d0
	bhs	ev_next
	lea	usb_to_mac(%pc),%a0
	move.b	(%a0,%d0.w),%d1		| Mac virtual key code, 0xFF = none
	cmp.b	#0xFF,%d1
	beq	ev_next
	and.w	#0x00FF,%d1
key_code:				| d1 = the Mac virtual key code
	cmp.b	#0x39,%d1		| caps lock toggles rather than holds
	bne.s	1f
	tst.l	EV_B(%a4)
	beq	ev_next
	eor.w	#1,G_CAPS(%a2)
	bra	ev_next
1:	| KeyMap: code k is byte k >> 3, bit k & 7 (keymap_modifiers), which GetKeys hands to programs
	move.w	%d1,%d2
	lsr.w	#3,%d2			| byte index
	move.w	%d1,%d3
	and.w	#7,%d3
	lea	KeyMap,%a0
	tst.l	EV_B(%a4)
	beq.s	2f
	bset	%d3,(%a0,%d2.w)
	bra.s	3f
2:	bclr	%d3,(%a0,%d2.w)
3:	| message = virtual code << 8 | character
	move.w	%d1,%d4
	lsl.w	#8,%d4			| d4 = code << 8
	move.w	G_MODS(%a2),%d0
	tst.w	G_CAPS(%a2)
	beq.s	4f
	or.w	#alphaLock,%d0
4:	move.w	%d0,%d5			| d5 = event modifiers
	bsr	key_char		| d1 = code, d5 = modifiers -> d0.b = character (0 if none)
	or.b	%d0,%d4
	move.l	#0x00020000,%d0		| bits 16-23: ADB address 2, where Apple keyboards live
	move.w	%d4,%d0			| d0 = message = ADB address << 16 | virtual code << 8 | character
	move.l	%d0,%a0
	moveq	#keyDown,%d0
	tst.l	EV_B(%a4)
	bne.s	5f
	moveq	#keyUp,%d0
	bra.s	6f
5:	| a down for a key already in the map is a repeat from the Pi side -> autoKey
	cmp.l	#2,EV_B(%a4)
	bne.s	6f
	moveq	#autoKey,%d0
6:	bsr	post_event		| d0 = what, a0 = message, d5 = modifiers
	bsr	key_trace		| the first few keys go to the Pi's log: "PiInput: key WHAT MESSAGE post RESULT"
	bra	ev_next

| --- the mouse: a = dx, b = dy, c = buttons (bit 0 left, 1 right, 2 middle) ---
ev_mouse:
	move.l	EV_A(%a4),%d0
	move.l	EV_B(%a4),%d1
	beq.s	1f
	bra.s	2f
1:	tst.l	%d0
	beq.s	mouse_buttons
2:	| RawMouse.h += dx, RawMouse.v += dy, clipped to the desktop; MTemp follows; CrsrNew set
	move.w	RawMouse+2,%d2		| h
	add.w	%d0,%d2
	move.w	RawMouse,%d3		| v
	add.w	%d1,%d3
mouse_clip:				| d2 = h, d3 = v: clipped to the desktop, into RawMouse and MTemp
	bsr	clip_desk
	move.w	%d3,RawMouse
	move.w	%d2,RawMouse+2
	move.w	%d3,MTemp
	move.w	%d2,MTemp+2
	st	CrsrNew
mouse_buttons:
	move.l	EV_C(%a4),%d0
	cmp.l	#-1,%d0			| "no button information" marker from the service core
	beq	ev_next
	move.w	%d0,%d1
	cmp.w	G_BUTTONS(%a2),%d1
	beq	ev_next
	move.w	%d1,G_BUTTONS(%a2)
	| any button down -> MBState $00 and a mouseDown; all up -> $80 and a mouseUp.
	| Right button adds Command, middle adds Option (Kensington-style), so two-button mice do
	| something useful under a one-button system.
	move.w	G_MODS(%a2),%d5
	btst	#1,%d1
	beq.s	1f
	or.w	#cmdKey,%d5
1:	btst	#2,%d1
	beq.s	2f
	or.w	#optionKey,%d5
2:	moveq	#0,%d0
	move.l	%d0,%a0			| message 0 for mouse events
	and.w	#7,%d1
	beq.s	3f
	move.b	#0x00,MBState
	moveq	#mouseDown,%d0
	bsr	post_event
	bra	ev_next
3:	move.b	#0x80,MBState
	or.w	#btnState,%d5
	moveq	#mouseUp,%d0
	bsr	post_event
ev_next:
	addq.l	#1,%d6
	move.l	%d6,IN_TAIL(%a3)
	addq.l	#1,(%sp)
	dbf	%d7,vbl_loop
vbl_done:
	move.l	(%sp)+,%d0
	movem.l	(%sp)+,%d1-%d7/%a0-%a1/%a3-%a4
	rts

| post_event: d0 = what, a0 = message, d5 = modifiers. _PostEvent builds the event record itself
| and fills its modifiers word from the modifier-key bits in KeyMap (and the button from MBState),
| the same way it does for the ADB Manager, so the modifiers are made visible by setting those
| KeyMap bits first; the d5 word is the driver's own idea of them.
post_event:
	movem.l	%d1-%d2/%a1,-(%sp)
	bsr	keymap_modifiers
	move.l	%a0,%d1			| _PostEvent wants the event code in A0 and the message in D0
	move.l	%d0,%a0			| (the first version had them the other way round: the cursor
	move.l	%d1,%d0			| moved, no click or key ever arrived)
	.short	_PostEvent
	movem.l	(%sp)+,%d1-%d2/%a1
	rts

| keymap_modifiers: set or clear the KeyMap bits of the five modifier keys from d5. Virtual codes:
| Command 0x37, Shift 0x38, Caps Lock 0x39, Option 0x3A, Control 0x3B (Inside Macintosh: Text).
| Code k lives in byte k >> 3, bit k & 7 of KeyMap: the ROM's _PostEvent (IIci ROM 0xB406) takes the
| event's modifier byte from the word at KeyMap+6 rotated left by one (Command, code 0x37, is bit 7
| of byte 6; Shift to Control, 0x38-0x3B, bits 0-3 of byte 7). An earlier version used byte
| (k >> 3) ^ 3, as if the map were little-endian longwords: no modifier reached an event.
keymap_modifiers:
	movem.l	%d0-%d4/%a0-%a1,-(%sp)
	lea	KeyMap,%a0
	lea	mod_table(%pc),%a1
	moveq	#4,%d4
1:	move.w	(%a1)+,%d0		| virtual code
	move.w	(%a1)+,%d1		| event modifier bit
	move.w	%d0,%d2
	lsr.w	#3,%d2
	move.w	%d0,%d3
	and.w	#7,%d3
	and.w	%d5,%d1
	beq.s	2f
	bset	%d3,(%a0,%d2.w)
	bra.s	3f
2:	bclr	%d3,(%a0,%d2.w)
3:	dbf	%d4,1b
	movem.l	(%sp)+,%d0-%d4/%a0-%a1
	rts
mod_table:
	.short	0x37,cmdKey, 0x38,shiftKey, 0x39,alphaLock, 0x3A,optionKey, 0x3B,controlKey

| hid_modifiers: d0.b = HID modifier byte (bit 0 LCtrl, 1 LShift, 2 LAlt, 3 LGUI, 4-7 right side)
| -> G_MODS = event-record modifier bits. On a Mac keyboard Alt is Option and GUI (Windows) is Command.
hid_modifiers:
	moveq	#0,%d1
	btst	#0,%d0
	bne.s	1f
	btst	#4,%d0
	beq.s	2f
1:	or.w	#controlKey,%d1
2:	btst	#1,%d0
	bne.s	3f
	btst	#5,%d0
	beq.s	4f
3:	or.w	#shiftKey,%d1
4:	btst	#2,%d0
	bne.s	5f
	btst	#6,%d0
	beq.s	6f
5:	or.w	#optionKey,%d1
6:	btst	#3,%d0
	bne.s	7f
	btst	#7,%d0
	beq.s	8f
7:	or.w	#cmdKey,%d1
8:	move.w	%d1,G_MODS(%a2)
	rts

| key_char: d1 = virtual code, d5 = modifiers -> d0.b = character for the current layout.
| The layout is a 'KCHR' resource (G_KCHR, fetched and locked at Open; at boot the ROM's U.S.
| one), read here directly: version word, then 256 bytes mapping the modifier byte (the high
| byte of the event modifiers: Command, Shift, Caps Lock, Option, Control in bits 0-4) to a table
| number, a table count, then the tables of 128 characters indexed by virtual key code (Inside
| Macintosh: Text, "KCHR resource"). A character of 0 is a dead key or nothing; dead keys are not
| composed yet (the ADB Manager would post the accent and then the letter). The first version
| called _KeyTranslate with the pointer from $1B40 and then with this one; the trap answered random
| letters with the former and zero with the latter, and a hand-written lookup of a documented
| table was the end of guessing what the trap wanted. Without a KCHR a built-in US table serves.
key_char:
	movem.l	%d1-%d2/%a0-%a1,-(%sp)
	move.l	G_KCHR(%a2),%d0
	beq.s	1f
	move.l	%d0,%a0
	moveq	#0,%d2
	move.w	%d5,%d2
	lsr.w	#8,%d2			| the modifier byte
	move.b	2(%a0,%d2.w),%d2	| -> table number
	and.w	#0xFF,%d2
	cmp.w	258(%a0),%d2		| past the tables the KCHR has? (malformed) -> the first
	blo.s	2f
	moveq	#0,%d2
2:	lsl.w	#7,%d2			| 128 bytes a table
	add.w	%d1,%d2
	moveq	#0,%d0
	move.b	260(%a0,%d2.w),%d0	| the character
	bra.s	9f
1:	lea	us_chars(%pc),%a0
	moveq	#0,%d0
	cmp.w	#0x80,%d1
	bhs.s	9f
	move.b	(%a0,%d1.w),%d0
	tst.b	%d0
	beq.s	9f
	btst	#9,%d5			| Shift: uppercase letters only (good enough before boot ends)
	beq.s	9f
	cmp.b	#'a',%d0
	blo.s	9f
	cmp.b	#'z',%d0
	bhi.s	9f
	sub.b	#0x20,%d0
9:	movem.l	(%sp)+,%d1-%d2/%a0-%a1
	rts

| key_trace: the first key events (trace_left of them), with PostEvent's answer, into the Pi's log, so a keyboard
| that types nothing can be told apart from one whose characters come out wrong (the first test
| of typing into a program, PiWiFi, showed a window that ignored every key). a0 = what, d0 = the
| PostEvent result, d4.w = virtual code << 8 | character; the text is written in place (RAM copy).
key_trace:
	movem.l	%d0-%d3/%a0-%a1,-(%sp)
	move.w	trace_left(%pc),%d3
	beq.s	9f
	subq.w	#1,%d3
	lea	trace_left(%pc),%a1
	move.w	%d3,(%a1)
	move.l	%d0,%d2			| the result
	move.l	%a0,%d0			| what
	lea	trace_text+13(%pc),%a0
	moveq	#1,%d1
	bsr	put_hex			| what (2 digits)
	moveq	#0,%d0
	move.w	%d4,%d0
	lea	trace_text+16(%pc),%a0
	moveq	#3,%d1
	bsr	put_hex			| message (4 digits)
	move.l	%d2,%d0
	lea	trace_text+26(%pc),%a0
	moveq	#3,%d1
	bsr	put_hex			| result (4 digits)
	lea	trace_text(%pc),%a1
	bsr	do_log
9:	movem.l	(%sp)+,%d0-%d3/%a0-%a1
	rts
| put_hex: d0 = value, d1 = digits - 1, a0 = where (the digits land most significant first)
put_hex:
	movem.l	%d0-%d2/%a0,-(%sp)
	lea	0(%a0,%d1.w),%a0
	addq.l	#1,%a0			| one past the last digit: written backwards
1:	move.b	%d0,%d2
	and.b	#15,%d2
	add.b	#'0',%d2
	cmp.b	#'9',%d2
	bls.s	2f
	addq.b	#7,%d2
2:	move.b	%d2,-(%a0)
	lsr.l	#4,%d0
	dbf	%d1,1b
	movem.l	(%sp)+,%d0-%d2/%a0
	rts
trace_left:	.word	32
trace_text:	.ascii	"PiInput: key XX MMMM post RRRR"
	.byte	0
kchr_text:	.ascii	"PiInput: KCHR 0 at AAAAAAAA: VVVVVVVV TTTTTTTT"
	.byte	0
	.even

| ---- common exit: immediate calls return, queued calls go through IODone ----
iodone:
	btst	#9,6(%a0)
	beq.s	1f
	rts
1:	move.l	0x08FC,-(%sp)
	rts

| ---- USB HID keyboard usage (0x00..0xE7) -> Macintosh virtual key code (0xFF = no equivalent) ----
| Sources: USB HID Usage Tables 1.12 section 10 (Keyboard/Keypad page); Inside Macintosh: Text,
| figure C-2 (Apple Extended Keyboard II virtual key codes). The 0xE0..0xE7 modifier usages are
| handled from the modifier byte and are 0xFF here.
	.even
usb_to_mac:
	.byte	0xFF,0xFF,0xFF,0xFF, 0x00,0x0B,0x08,0x02, 0x0E,0x03,0x05,0x04, 0x22,0x26,0x28,0x25	| 00: -,-,-,-, A B C D, E F G H, I J K L
	.byte	0x2E,0x2D,0x1F,0x23, 0x0C,0x0F,0x01,0x11, 0x20,0x09,0x0D,0x07, 0x10,0x06,0x12,0x13	| 10: M N O P, Q R S T, U V W X, Y Z 1 2
	.byte	0x14,0x15,0x17,0x16, 0x1A,0x1C,0x19,0x1D, 0x24,0x35,0x33,0x30, 0x31,0x1B,0x18,0x21	| 20: 3 4 5 6, 7 8 9 0, Ret Esc BS Tab, Spc - = [
	.byte	0x1E,0x2A,0x2A,0x29, 0x27,0x32,0x2B,0x2F, 0x2C,0x39,0x7A,0x78, 0x63,0x76,0x60,0x61	| 30: ] \ #(non-US) ; ' ` , . / Caps F1 F2 F3 F4 F5 F6
	.byte	0x62,0x64,0x65,0x6D, 0x67,0x6F,0x69,0x6B, 0x71,0x72,0x73,0x74, 0x75,0x77,0x79,0x7C	| 40: F7 F8 F9 F10 F11 F12 PrtSc ScrLk Pause Ins Home PgUp Del End PgDn Right
	.byte	0x7B,0x7D,0x7E,0x47, 0x4B,0x43,0x4E,0x45, 0x4C,0x53,0x54,0x55, 0x56,0x57,0x58,0x59	| 50: Left Down Up NumLk, KP/ KP* KP- KP+, KPEnt KP1 KP2 KP3, KP4 KP5 KP6 KP7
	.byte	0x5B,0x5C,0x52,0x41, 0x32,0x6E,0x3F,0x51, 0x69,0x6B,0x71,0xFF, 0xFF,0xFF,0xFF,0xFF	| 60: KP8 KP9 KP0 KP., non-US\ App Power KP=, F13 F14 F15 -,-,-,-,-
	.byte	0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF	| 70
	.byte	0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF	| 80
	.byte	0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF	| 90
	.byte	0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF	| A0
	.byte	0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF	| B0
	.byte	0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF	| C0
	.byte	0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF	| D0
	.byte	0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF					| E0: modifiers

| ---- characters for the US layout by virtual code, unshifted (0 = none); used before the KCHR ----
us_chars:
	.ascii	"asdfhgzxcv"		| 00-09
	.byte	0			| 0A (section sign on ISO keyboards)
	.ascii	"bqweryt1234"		| 0B-15
	.ascii	"65=97-80]o"		| 16-1F
	.ascii	"u[ip"			| 20-23
	.byte	0x0D			| 24 return
	.ascii	"lj'k;\\,/nm."		| 25-2F
	.byte	0x09			| 30 tab
	.ascii	" `"			| 31-32
	.byte	0x08			| 33 delete (backspace)
	.byte	0			| 34
	.byte	0x1B			| 35 escape
	.byte	0,0,0,0,0,0,0,0,0,0	| 36-3F modifiers, unused
	.byte	0,0x2E,0,0x2A,0,0x2B,0,0x1B,0,0,0,0x2F,0x03,0,0,0x2D	| 40-4F: KP. KP* KP+ clear KP/ KPenter KP-
	.byte	0,0x3D,'0','1','2','3','4','5','6','7',0,'8','9',0,0,0	| 50-5F: KP= KP0-9
	.byte	0x10,0x10,0x10,0x10,0x10,0x10,0,0x10,0,0x10,0,0x10,0,0x10,0x10,0x10	| 60-6F function keys -> 0x10
	.byte	0,0x05,0x1F,0x01,0x0B,0x10,0x7F,0x10,0x7F,0x10,0x04,0x0C,0x1D,0x1C,0x1F,0x1E	| 70-7F: help home pgup fwddel end pgdn arrows
	.even
drvr_end:
