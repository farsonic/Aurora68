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
| and Command-click, which is what Apple's own multi-button drivers of the day did. Since 23 November
| 2026 the driver also stands in front of the ROM's cursor task, so that a pointer pushed against the
| side of a screen where another screen lies goes onto it, from any mouse ("the edge jump", below).
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
	.equ	IN_OPTIONS,	28		| long: the kernel's input options (alice_psmac.h ALICE_IN_OPT_*)
	.equ	IN_OPT_NOEDGE,	0		| bit: mac.edgejump=off, the cursor does not jump between screens' edges

| ---- Mac OS low memory and traps (Inside Macintosh, MPW SysEqu.a) ----
	.equ	MMU32Bit,	0x0CB2
	.equ	MBState,	0x0172		| byte: $80 = button up
	.equ	MTemp,		0x0828		| Point: unprocessed mouse position
	.equ	RawMouse,	0x082C		| Point: unprocessed mouse position
	.equ	CrsrNew,	0x08CE		| byte: nonzero when the cursor must be redrawn
	.equ	CrsrCouple,	0x08CF		| byte: cursor coupled to the mouse
	.equ	CrsrPin,	0x0834		| Rect: cursor limits
	.equ	DeviceList,	0x08A8		| Handle: the first GDevice (Color QuickDraw's screens)
	.equ	CrsrDevice,	0x089C		| Handle: the GDevice the cursor is on
	.equ	CrsrBusy,	0x08CD		| byte: the cursor is being changed (the ROM's cursor task leaves it alone)
	.equ	Mouse,		0x0830		| Point: the processed mouse position
	.equ	JCrsrTask,	0x08EE		| the cursor task, called at the cursor's screen's vertical blank
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
	| the edge jump (crsr_hook, below)
	.equ	G_OLDCRSR,	G_KCHR+4	| long: the cursor task ours calls (the ROM's)
	.equ	G_PREVCRSR,	G_OLDCRSR+4	| long: the one before that, should someone have replaced ours (crsr_watch)
	.equ	G_HOOKN,	G_PREVCRSR+4	| long: times ours ran
	.equ	G_HOOKSEEN,	G_HOOKN+4	| long: G_HOOKN at the VBL task's last look
	.equ	G_UNTIL,	G_HOOKSEEN+4	| long: Ticks until which no further jump is made (the dead time)
	.equ	G_DEVBEFORE,	G_UNTIL+4	| long: CrsrDevice before the ROM's task ran
	.equ	G_JUMPS,	G_DEVBEFORE+4	| long: jumps made, for the log
	.equ	G_ABS,		G_JUMPS+4	| byte: the last movement was the remote control's absolute one
	.equ	G_INHOOK,	G_ABS+1		| byte: inside crsr_hook
	.equ	G_DO,		G_INHOOK+1	| byte: look for a crossing the ROM made, after its task
	.equ	G_PAD,		G_DO+1
	.equ	G_SIZE,		G_PAD+1
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
2:	lea	hook_globals(%pc),%a0	| for the cursor task's edge jump, which crsr_watch puts in
	move.l	%a2,(%a0)
	| The VBL task as well: every tick until the first slot interrupt arrives, then every 15
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
	bsr	crsr_watch		| the edge jump's hook on the cursor task: in, and still in
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

| ==== the edge jump (23 November 2026) ====
| The owner's SE has its own 512 by 342 screen to the left of a 2560 by 1440 HDMI one, tops aligned in
| Monitors. The ROM moves the cursor from one screen to another only where they touch: across the top
| 342 rows of the HDMI screen's left edge; below them that edge is a wall, and coming back the cursor
| lands at the same height, so the small screen is reached only by aiming at a strip a quarter of the
| big one's height. The owner asked that "moving to the side where the screen is moves the mouse
| regardless". So the whole edge of a screen that has a neighbour on that side (in the Monitors
| arrangement, whether or not the two touch at that height) leads onto the neighbour, at the same
| proportion along the edge: HDMI's left edge at y 1000 of 1440 is the SE screen's right edge at 238
| of 342, and the SE's right edge at 100 is HDMI's left edge at 421. Where the screens touch the ROM
| still makes the crossing, and the point is then moved to the same proportion, so the mapping is the
| same everywhere along the edge and both ways round. The point lands EDGE_INSET pixels inside the
| neighbour's facing edge, and after a jump none through a wall follows for EDGE_DEAD ticks, so a
| hand that overshoots or a mouse that jitters does not throw the cursor straight back.
|
| Where. The one routine every pointer movement passes through is the ROM's cursor task (the IIci ROM
| at $4082DF94, reached through the low-memory vector JCrsrTask, $8EE, from the vertical-blank handler
| of the screen the cursor is on, $4080A33E). The ADB Manager's mouse handler (the SE's own mouse) adds
| its movement to MTemp and sets CrsrNew = CrsrCouple ($4080AB26); this driver sets MTemp and RawMouse
| for USB and Bluetooth mice and the remote control. The task then moves RawMouse by the (accelerated)
| difference, pins it to CrsrPin (the cursor's screen's rectangle) and, for a point outside it, looks
| for another active screen that holds the point ($4082E14A): found, the cursor moves there (CrsrDevice,
| CrsrPin and the cursor's depth change with it); not found, the point is pinned at the edge pixel.
| So crsr_hook goes in front of it and looks twice:
| - before (edge_wall): MTemp beyond the cursor's screen's edge, on no screen at all, means the cursor
|   is being pushed against a wall. If a screen lies on that side, the point becomes the proportional
|   one on it, in MTemp and RawMouse, with CrsrNew = CrsrCouple: the documented way of setting the
|   mouse position (Technical Note "Mouse Movement"), and the ROM's task, which runs next, finds the
|   point on that screen and moves the cursor there as for any crossing;
| - after (edge_crossed): the ROM's task moved the cursor onto another screen by itself, near the edge
|   it came over: the point is moved along that edge to the proportional place and the task run once
|   more, so the cursor is drawn there in the same tick.
| A point the remote control chose (IN_MOUSEABS, "display": N) is left where it was put, and so is a
| point another program put far from the edge it would have crossed (EDGE_NEAR). Nothing happens while
| CrsrBusy is set or the cursor is uncoupled (CrsrCouple 0), when CrsrPin is not the cursor's screen
| (a program has pinned the cursor to a rectangle of its own), with one screen, or with mac.edgejump=off
| (the input ring's option bit, read every time). The screens are read from QuickDraw's device list
| each time, so a new arrangement in Monitors or another HDMI size applies at once.
|
| Staying in. The hook goes in from the VBL task (crsr_watch), not from Open, and the VBL task looks
| again every quarter second: should something replace the vector later (an extension, a system
| patch) without calling ours, ours goes in front again. If that something saved ours and calls it,
| ours keeps running and nothing changes; and if ours goes in front of one that calls ours, the inner
| call (G_INHOOK) goes on to the task ours called before, so nothing runs twice or for ever.
	.equ	EDGE_INSET,	4		| pixels inside the neighbour's facing edge where a jump lands
	.equ	EDGE_DEAD,	10		| ticks after a jump through a wall before the next
	.equ	EDGE_NEAR,	64		| a crossing the ROM made is remapped only this close to the edge

hook_globals:	.long	0			| our globals, for the cursor task (written once, at Open)

| crsr_watch: a2 = globals, from the VBL task. JCrsrTask not ours, and ours not called since the last
| look: ours goes in front of whatever is there.
crsr_watch:
	movem.l	%d0-%d1/%a0-%a1,-(%sp)
	move.l	JCrsrTask,%d0
	beq.s	9f			| no cursor task (yet)
	lea	crsr_hook(%pc),%a0
	cmp.l	%a0,%d0
	beq.s	8f			| ours is in front
	move.l	G_HOOKN(%a2),%d1
	cmp.l	G_HOOKSEEN(%a2),%d1
	bne.s	8f			| ours still runs: one in front of ours calls it
	move.l	G_OLDCRSR(%a2),G_PREVCRSR(%a2)
	move.l	%d0,G_OLDCRSR(%a2)
	move.l	%a0,JCrsrTask
	lea	ht_addr(%pc),%a0
	moveq	#7,%d1
	bsr	put_hex
	lea	hook_text(%pc),%a1
	bsr	do_log			| "PiInput: edge jump in front of the cursor task at AAAAAAAA"
8:	move.l	G_HOOKN(%a2),G_HOOKSEEN(%a2)
9:	movem.l	(%sp)+,%d0-%d1/%a0-%a1
	rts

| crsr_hook: the cursor task, in front of the ROM's. Every register is kept (the ROM's task may
| change D0-D3/A0/A1/A3, and its caller allows for that). The long under the registers is room for
| a return address: the way on when ours is called from inside itself (crsr_watch).
crsr_hook:
	subq.l	#4,%sp
	movem.l	%d0-%d7/%a0-%a4,-(%sp)	| 52 bytes
	move.l	hook_globals(%pc),%a2
	tst.b	G_INHOOK(%a2)
	bne	crsr_reentered
	st	G_INHOOK(%a2)
	addq.l	#1,G_HOOKN(%a2)
	sf	G_DO(%a2)
	move.b	G_ABS(%a2),%d7
	tst.b	CrsrNew
	beq.s	2f			| nothing moved
	sf	G_ABS(%a2)
	tst.b	%d7
	bne.s	2f			| the remote control's point: where it was put
	move.l	G_RING(%a2),%a0
	btst	#IN_OPT_NOEDGE,IN_OPTIONS+3(%a0)
	bne.s	2f			| mac.edgejump=off
	tst.b	CrsrBusy
	bne.s	2f
	tst.b	CrsrCouple
	beq.s	2f
	move.l	CrsrDevice,G_DEVBEFORE(%a2)
	beq.s	2f
	st	G_DO(%a2)
	bsr	edge_wall
2:	move.l	G_OLDCRSR(%a2),%d0
	beq.s	3f
	move.l	%d0,%a0
	jsr	(%a0)			| the ROM's cursor task: pin, cross where screens touch, draw
	move.l	hook_globals(%pc),%a2
	tst.b	G_DO(%a2)
	beq.s	3f
	bsr	edge_crossed
3:	sf	G_INHOOK(%a2)
	movem.l	(%sp)+,%d0-%d7/%a0-%a4
	addq.l	#4,%sp
	rts
crsr_reentered:
	move.l	G_PREVCRSR(%a2),52(%sp)
	beq.s	1f
	movem.l	(%sp)+,%d0-%d7/%a0-%a4
	rts				| into the task that was there before the one in front of ours
1:	movem.l	(%sp)+,%d0-%d7/%a0-%a4
	addq.l	#4,%sp
	rts

| edge_wall: a2 = globals. MTemp beyond an edge of the cursor's screen and on no screen: onto the
| neighbour on that side, if there is one, at the same proportion along the edge.
edge_wall:
	move.l	Ticks,%d0
	sub.l	G_UNTIL(%a2),%d0
	bmi	9f			| within the dead time of the last jump
	bsr	cur_rect		| a3 = the cursor's screen's rectangle
	beq	9f
	move.w	MTemp,%d3		| v
	move.w	MTemp+2,%d2		| h
	move.l	%a3,%a0
	bsr	pt_in
	bne	9f			| still on its screen
	bsr	pt_on_screen
	bne	9f			| on another screen: the ROM crosses there (edge_crossed follows)
	moveq	#0,%d4			| the sides in turn: 0 left, 1 right, 2 top, 3 bottom
1:	bsr	beyond
	beq.s	2f
	bsr	neighbour		| a1 = the neighbour's rectangle
	bne.s	3f
2:	addq.w	#1,%d4
	cmp.w	#4,%d4
	blo.s	1b
	bra.s	9f
3:	move.w	%d2,%d6			| from, for the log
	move.w	%d3,%d7
	bsr	edge_target		| d2, d3 = the point on the neighbour
	move.w	%d3,MTemp
	move.w	%d2,MTemp+2
	move.w	%d3,RawMouse
	move.w	%d2,RawMouse+2
	move.b	CrsrCouple,CrsrNew
	bsr	edge_dead
	sf	G_DO(%a2)		| the ROM's task moves it there: nothing to look at after
	bsr	jump_log
9:	rts

| edge_crossed: a2 = globals, after the ROM's task. It moved the cursor onto another screen: if that
| screen lies beside the old one and the point is near the edge it came over, the same proportion along it.
edge_crossed:
	move.l	CrsrDevice,%d0
	beq	9f
	cmp.l	G_DEVBEFORE(%a2),%d0
	beq	9f			| the cursor stayed on its screen
	move.l	%d0,%a1
	move.l	(%a1),%a1
	lea	34(%a1),%a1		| N: the screen it is on now
	move.l	G_DEVBEFORE(%a2),%a3
	move.l	(%a3),%a3
	lea	34(%a3),%a3		| C: the one it was on
	move.w	RawMouse,%d3
	move.w	RawMouse+2,%d2
	moveq	#0,%d4			| left: N.right <= C.left, the two side by side
	move.w	6(%a1),%d0
	cmp.w	2(%a3),%d0
	bgt.s	1f
	bsr	overlap_v
	beq.s	1f
	move.w	6(%a1),%d0
	sub.w	%d2,%d0			| how far in from N's right edge
	bra.s	5f
1:	moveq	#1,%d4			| right: N.left >= C.right
	move.w	2(%a1),%d0
	cmp.w	6(%a3),%d0
	blt.s	2f
	bsr	overlap_v
	beq.s	2f
	move.w	%d2,%d0
	sub.w	2(%a1),%d0
	addq.w	#1,%d0
	bra.s	5f
2:	moveq	#2,%d4			| top: N.bottom <= C.top, one above the other
	move.w	4(%a1),%d0
	cmp.w	(%a3),%d0
	bgt.s	3f
	bsr	overlap_h
	beq.s	3f
	move.w	4(%a1),%d0
	sub.w	%d3,%d0
	bra.s	5f
3:	moveq	#3,%d4			| bottom: N.top >= C.bottom
	move.w	(%a1),%d0
	cmp.w	4(%a3),%d0
	blt	9f
	bsr	overlap_h
	beq	9f
	move.w	%d3,%d0
	sub.w	(%a1),%d0
	addq.w	#1,%d0
5:	cmp.w	#EDGE_NEAR,%d0
	bhi	9f			| far from the edge (or not in): a point someone chose, not a crossing
	move.w	%d2,%d6
	move.w	%d3,%d7
	bsr	edge_target
	| across the edge, keep it as far in as the ROM put it when that is further than the inset
	cmp.w	#1,%d4
	beq.s	11f
	bhi.s	12f
	cmp.w	%d2,%d6			| left: the smaller h
	bge.s	20f
	move.w	%d6,%d2
	bra.s	20f
11:	cmp.w	%d2,%d6			| right: the larger h
	ble.s	20f
	move.w	%d6,%d2
	bra.s	20f
12:	cmp.w	#3,%d4
	beq.s	13f
	cmp.w	%d3,%d7			| top: the smaller v
	bge.s	20f
	move.w	%d7,%d3
	bra.s	20f
13:	cmp.w	%d3,%d7			| bottom: the larger v
	ble.s	20f
	move.w	%d7,%d3
20:	cmp.w	%d2,%d6
	bne.s	21f
	cmp.w	%d3,%d7
	beq.s	9f			| already there
21:	move.w	%d3,MTemp
	move.w	%d2,MTemp+2
	move.w	%d3,RawMouse
	move.w	%d2,RawMouse+2
	move.b	CrsrCouple,CrsrNew
	bsr	edge_dead
	bsr	jump_log
	move.l	G_OLDCRSR(%a2),%a0
	jsr	(%a0)			| moved and drawn there now, not a tick later
	move.l	hook_globals(%pc),%a2
9:	rts

| edge_dead: no jump through a wall for the next EDGE_DEAD ticks
edge_dead:
	move.l	Ticks,%d0
	add.l	#EDGE_DEAD,%d0
	move.l	%d0,G_UNTIL(%a2)
	rts

| cur_rect: -> a3 = the cursor's screen's gdRect (top, left, bottom, right), Z clear; Z set when
| there is no cursor screen or CrsrPin is not its rectangle (a program has the cursor pinned)
cur_rect:
	move.l	CrsrDevice,%d0
	beq.s	9f
	move.l	%d0,%a3
	move.l	(%a3),%a3
	lea	34(%a3),%a3
	move.l	CrsrPin,%d0
	cmp.l	(%a3),%d0
	bne.s	8f
	move.l	CrsrPin+4,%d0
	cmp.l	4(%a3),%d0
	bne.s	8f
	moveq	#1,%d0
	rts
8:	moveq	#0,%d0
9:	rts

| pt_in: d2 = h, d3 = v, a0 = a rectangle -> Z clear (d0 1) if the point is in it
pt_in:
	cmp.w	(%a0),%d3
	blt.s	8f
	cmp.w	4(%a0),%d3
	bge.s	8f
	cmp.w	2(%a0),%d2
	blt.s	8f
	cmp.w	6(%a0),%d2
	bge.s	8f
	moveq	#1,%d0
	rts
8:	moveq	#0,%d0
	rts

| pt_on_screen: d2 = h, d3 = v -> Z clear if an active screen holds the point
pt_on_screen:
	movem.l	%d1/%d5/%a0/%a4,-(%sp)
	moveq	#0,%d0
	move.l	DeviceList,%d1
	moveq	#7,%d5			| eight screens at most
1:	tst.l	%d1
	beq.s	9f
	move.l	%d1,%a4
	move.l	(%a4),%a4
	btst	#7,20(%a4)		| screenActive
	beq.s	2f
	lea	34(%a4),%a0
	bsr	pt_in
	bne.s	9f
2:	move.l	30(%a4),%d1		| gdNextGD
	dbf	%d5,1b
	moveq	#0,%d0
9:	movem.l	(%sp)+,%d1/%d5/%a0/%a4
	tst.l	%d0
	rts

| beyond: d4 = side, a3 = the cursor's screen, d2 = h, d3 = v -> Z clear if the point is past that edge
beyond:
	moveq	#0,%d0
	cmp.w	#1,%d4
	beq.s	1f
	bhi.s	2f
	cmp.w	2(%a3),%d2		| left of the left edge
	bge.s	9f
	bra.s	8f
1:	cmp.w	6(%a3),%d2		| right of the right edge
	blt.s	9f
	bra.s	8f
2:	cmp.w	#3,%d4
	beq.s	3f
	cmp.w	(%a3),%d3		| above the top
	bge.s	9f
	bra.s	8f
3:	cmp.w	4(%a3),%d3		| below the bottom
	blt.s	9f
8:	moveq	#1,%d0
9:	tst.l	%d0
	rts

| neighbour: d4 = side, a3 = the cursor's screen -> a1 = the rectangle of the active screen on that
| side (all of it past that edge), Z clear; 0 and Z set if none. Of several, the nearest: the gap
| between the two edges plus the gap across (0 where they overlap along the edge).
neighbour:
	movem.l	%d1-%d7/%a0/%a4,-(%sp)
	suba.l	%a1,%a1
	move.l	#0x7FFFFFFF,%d7		| the best so far
	move.l	DeviceList,%d6
	moveq	#7,%d5
1:	tst.l	%d6
	beq	8f
	move.l	%d6,%a4
	move.l	(%a4),%a4
	btst	#7,20(%a4)
	beq	7f
	lea	34(%a4),%a0
	cmp.l	%a0,%a3
	beq	7f			| the cursor's own screen
	cmp.w	#1,%d4
	beq.s	11f
	bhi.s	12f
	move.w	2(%a3),%d0		| left: C.left - N.right
	sub.w	6(%a0),%d0
	bra.s	20f
11:	move.w	2(%a0),%d0		| right: N.left - C.right
	sub.w	6(%a3),%d0
20:	move.w	(%a0),%d1		| across: N.top - C.bottom, C.top - N.bottom
	sub.w	4(%a3),%d1
	move.w	(%a3),%d2
	sub.w	4(%a0),%d2
	bra.s	30f
12:	cmp.w	#3,%d4
	beq.s	13f
	move.w	(%a3),%d0		| top: C.top - N.bottom
	sub.w	4(%a0),%d0
	bra.s	21f
13:	move.w	(%a0),%d0		| bottom: N.top - C.bottom
	sub.w	4(%a3),%d0
21:	move.w	2(%a0),%d1		| across: N.left - C.right, C.left - N.right
	sub.w	6(%a3),%d1
	move.w	2(%a3),%d2
	sub.w	6(%a0),%d2
30:	tst.w	%d0
	bmi.s	7f			| not wholly on that side
	cmp.w	%d2,%d1
	bge.s	31f
	move.w	%d2,%d1			| the larger of the two
31:	tst.w	%d1
	bpl.s	32f
	moveq	#0,%d1			| they overlap across
32:	ext.l	%d0
	ext.l	%d1
	add.l	%d1,%d0
	cmp.l	%d7,%d0
	bge.s	7f
	move.l	%d0,%d7
	move.l	%a0,%a1
7:	move.l	30(%a4),%d6
	dbf	%d5,1b
8:	movem.l	(%sp)+,%d1-%d7/%a0/%a4
	move.l	%a1,%d0
	rts

| overlap_v / overlap_h: a1, a3 = two rectangles -> Z clear if they share rows / columns
overlap_v:
	move.w	(%a1),%d0
	cmp.w	4(%a3),%d0
	bge.s	8f
	move.w	4(%a1),%d0
	cmp.w	(%a3),%d0
	ble.s	8f
	moveq	#1,%d0
	rts
overlap_h:
	move.w	2(%a1),%d0
	cmp.w	6(%a3),%d0
	bge.s	8f
	move.w	6(%a1),%d0
	cmp.w	2(%a3),%d0
	ble.s	8f
	moveq	#1,%d0
	rts
8:	moveq	#0,%d0
	rts

| edge_target: d4 = side, a3 = the screen left, a1 = the one arrived at, d2 = h, d3 = v -> d2, d3:
| along the edge at the same proportion, across it EDGE_INSET pixels inside the facing edge
edge_target:
	movem.l	%d0-%d1/%d5-%d7,-(%sp)
	cmp.w	#2,%d4
	bhs.s	5f
	move.w	%d3,%d0			| left or right: v in proportion
	move.w	(%a3),%d1
	move.w	4(%a3),%d5
	move.w	(%a1),%d6
	move.w	4(%a1),%d7
	bsr	proportion
	move.w	%d0,%d3
	tst.w	%d4
	bne.s	1f
	move.w	6(%a1),%d2		| left: h inside the right edge
	subq.w	#1+EDGE_INSET,%d2
	cmp.w	2(%a1),%d2
	bge.s	9f
	move.w	2(%a1),%d2
	bra.s	9f
1:	move.w	2(%a1),%d2		| right: inside the left edge
	addq.w	#EDGE_INSET,%d2
	cmp.w	6(%a1),%d2
	blt.s	9f
	move.w	6(%a1),%d2
	subq.w	#1,%d2
	bra.s	9f
5:	move.w	%d2,%d0			| top or bottom: h in proportion
	move.w	2(%a3),%d1
	move.w	6(%a3),%d5
	move.w	2(%a1),%d6
	move.w	6(%a1),%d7
	bsr	proportion
	move.w	%d0,%d2
	cmp.w	#2,%d4
	bne.s	6f
	move.w	4(%a1),%d3		| top: v inside the bottom edge
	subq.w	#1+EDGE_INSET,%d3
	cmp.w	(%a1),%d3
	bge.s	9f
	move.w	(%a1),%d3
	bra.s	9f
6:	move.w	(%a1),%d3		| bottom: inside the top edge
	addq.w	#EDGE_INSET,%d3
	cmp.w	4(%a1),%d3
	blt.s	9f
	move.w	4(%a1),%d3
	subq.w	#1,%d3
9:	movem.l	(%sp)+,%d0-%d1/%d5-%d7
	rts

| proportion: d0.w = a coordinate along [d1, d5) -> d0.w at the same fraction of [d6, d7), rounded
proportion:
	movem.l	%d1-%d2/%d5,-(%sp)
	cmp.w	%d1,%d0
	bge.s	1f
	move.w	%d1,%d0
1:	cmp.w	%d5,%d0
	blt.s	2f
	move.w	%d5,%d0
	subq.w	#1,%d0
2:	sub.w	%d1,%d0			| the offset along the first
	sub.w	%d1,%d5			| its length
	ble.s	8f
	move.w	%d7,%d2
	sub.w	%d6,%d2			| the second's length
	muls.w	%d2,%d0
	move.w	%d5,%d2
	lsr.w	#1,%d2
	ext.l	%d2
	add.l	%d2,%d0
	divs.w	%d5,%d0
	add.w	%d6,%d0
	cmp.w	%d7,%d0
	blt.s	9f
	move.w	%d7,%d0
	subq.w	#1,%d0
	bra.s	9f
8:	move.w	%d6,%d0
9:	movem.l	(%sp)+,%d1-%d2/%d5
	rts

| jump_log: d6 = h, d7 = v before, d2 = h, d3 = v after: the first jumps into the Pi's log
jump_log:
	movem.l	%d0-%d1/%a0-%a1,-(%sp)
	addq.l	#1,G_JUMPS(%a2)
	cmp.l	#24,G_JUMPS(%a2)
	bhi.s	9f
	move.w	%d6,%d0
	lea	jt_fh(%pc),%a0
	bsr	put_dec
	move.w	%d7,%d0
	lea	jt_fv(%pc),%a0
	bsr	put_dec
	move.w	%d2,%d0
	lea	jt_th(%pc),%a0
	bsr	put_dec
	move.w	%d3,%d0
	lea	jt_tv(%pc),%a0
	bsr	put_dec
	lea	jump_text(%pc),%a1
	bsr	do_log			| "PiInput: edge jump -0003,+1000 -> -0005,+0238"
9:	movem.l	(%sp)+,%d0-%d1/%a0-%a1
	rts

| put_dec: d0.w signed -> five characters at a0: the sign, then four digits
put_dec:
	movem.l	%d0-%d1/%a0,-(%sp)
	move.b	#'+',(%a0)
	tst.w	%d0
	bpl.s	1f
	neg.w	%d0
	move.b	#'-',(%a0)
1:	and.l	#0xFFFF,%d0
	lea	5(%a0),%a0
	moveq	#3,%d1
2:	divu.w	#10,%d0
	swap	%d0
	add.b	#'0',%d0
	move.b	%d0,-(%a0)
	clr.w	%d0
	swap	%d0
	dbf	%d1,2b
	movem.l	(%sp)+,%d0-%d1/%a0
	rts

hook_text:	.ascii	"PiInput: edge jump in front of the cursor task at "
ht_addr:	.ascii	"AAAAAAAA"
	.byte	0
jump_text:	.ascii	"PiInput: edge jump "
jt_fh:		.ascii	"+0000,"
jt_fv:		.ascii	"+0000 -> "
jt_th:		.ascii	"+0000,"
jt_tv:		.ascii	"+0000"
	.byte	0
	.even

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
	st	G_ABS(%a2)		| a point the remote chose: the cursor task's edge jump leaves it alone
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
	sf	G_ABS(%a2)		| a movement: the edge jump may take it
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
