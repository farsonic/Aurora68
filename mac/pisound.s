| pisound.s — "Sound PiStorm": the sound card as the Sound Manager's output. MPL-2.0. GNU as MIT syntax, 68020.
|
| The kernel plays a ring of 16-bit stereo at 44.1 kHz in slot 9's window through the Pi's HDMI or
| its headphone jack (emu68/src/psmac/psmac_audio.c; the block is struct psmb_sound in psmb.h). This
| extension makes the Sound Manager play into it. Sound Manager 3.0 (1993, an extension for System
| 7.1, built into 7.5) has output devices as components of type 'sdev': it mixes every channel with
| its Apple Mixer and the device takes the mixed samples, at its own pace, with
| SoundComponentGetSourceData. Apple's own device for the ASC is one of them (the "Built-in" 'sdev'
| in the Sound Manager file), and its calls were the model for these: the dispatcher's selectors,
| the mixer opened with OpenMixerSoundComponent in InitOutputDevice, every source call passed on to
| the mixer with DelegateComponentCall, GetInfo answered for the hardware's own selectors, the
| output device chosen by the Sound Manager's 'odev' preference ("Output Device": the Sound
| Manager's version, a ComponentDescription and the name). Sound Manager 3.0's documentation
| ("Sound Components") gives the same shape; nothing of Apple's code is in here.
|
| What it does at startup (an INIT, after the Sound Manager's own: the file's name sorts after
| "Sound Manager", which is how System 7 orders extensions):
|   - no Aurora68 window, or the kernel's block says nothing plays the card (mac.audio=off, or
|     neither HDMI nor the jack would take sound): it says so in the Pi's log and leaves the Sound
|     Manager on the built-in device (the ASC, which the kernel plays to the SE's speaker and, if it
|     can, to HDMI);
|   - a Sound Manager older than 3.0 (System 7.1 has 2.0 without the extension) or no Component
|     Manager: it says so and leaves everything alone;
|   - otherwise it copies the component into the system heap, registers it, and names it in the
|     'odev' preference, so that every sound goes to the card.
| The component feeds the ring from a Time Manager task every 10 ms while the Sound Manager plays,
| up to 64 ms ahead (2816 frames): enough for a 16 ms system tick, little enough to feel immediate.
| With mac.audiotest=card it plays three seconds of a stereo test at startup, 441 Hz on the left,
| 882 Hz on the right, square waves written straight into the ring (tools/audiocheck.py).
| Written 20 November 2026 (docs/08 item 74). Build: make -C mac (Sound PiStorm.bin).

	.text
	.globl	init_start
	.equ	_NewPtrSysClear, 0xA71E
	.equ	_NewHandle,	0xA122
	.equ	_NewHandleSys,	0xA522
	.equ	_DisposeHandle,	0xA023
	.equ	_Gestalt,	0xA1AD
	.equ	_InsXTime,	0xA458
	.equ	_PrimeTime,	0xA05A
	.equ	_RmvTime,	0xA059
	.equ	_ComponentDispatch, 0xA82A
	.equ	_SoundDispatch,	0xA800
	.equ	_GetToolTrapAddress, 0xA746	| NGetTrapAddress, toolbox form
	.equ	MMU32Bit,	0x0CB2
	.equ	SdVolume,	0x0208
	| the window (psmb.h): the INFO service's command block, the sound card's block and its ring
	.equ	OFF_INFO,	0x1000
	.equ	OFF_SND,	0x11000
	.equ	OFF_RING,	0x12000
	.equ	RING_FRAMES,	8192
	| struct psmb_sound
	.equ	S_MAGIC, 0
	.equ	S_FRAMES, 12
	.equ	S_OUTKIND, 20
	.equ	S_TAIL, 24
	.equ	S_TEST, 32
	.equ	S_HEAD, 48
	.equ	S_STATE, 52
	.equ	S_VOLUME, 56
	.equ	S_SMVERS, 60
	.equ	S_OPENS, 64
	.equ	S_STARTS, 68
	.equ	S_FEEDS, 72
	| SoundComponentData
	.equ	D_CHANNELS, 8
	.equ	D_SIZE, 10
	.equ	D_COUNT, 16
	.equ	D_BUFFER, 20
	.equ	TARGET,	2816			| frames kept queued while playing: 64 ms at 44.1 kHz
	.equ	TEST_FRAMES, 132300		| mac.audiotest=card: three seconds
	.equ	badSelector, 0x80008002		| badComponentSelector

init_start:
	movem.l	%d3-%d7/%a2-%a6,-(%sp)
	bsr	window
	cmp.l	#0x50534D42,(%a0)		| 'PSMB': an Aurora68 Mac?
	bne	init_done
	lea	OFF_SND(%a0),%a2
	cmp.l	#0x41363853,S_MAGIC(%a2)		| 'A68S': a kernel with the sound card
	bne	init_old_kernel
	| the Sound Manager's version, if it has the call (System 6.0.7 and later)
	move.w	#0xA800,%d0
	.short	_GetToolTrapAddress
	move.l	%a0,%d3
	move.w	#0xA89F,%d0			| _Unimplemented
	.short	_GetToolTrapAddress
	moveq	#0,%d4
	cmp.l	%a0,%d3
	beq.s	1f
	subq.l	#4,%sp
	move.l	#0x000C0008,%d0			| SndSoundManagerVersion
	.short	_SoundDispatch
	move.l	(%sp)+,%d4
1:	move.l	%d4,S_SMVERS(%a2)
	lea	t_vers(%pc),%a0		| "N.N" into the message
	move.l	%d4,%d0
	rol.l	#8,%d0
	and.b	#15,%d0
	add.b	#'0',%d0
	move.b	%d0,(%a0)
	move.l	%d4,%d0
	swap	%d0
	lsr.w	#4,%d0
	and.b	#15,%d0
	add.b	#'0',%d0
	move.b	%d0,2(%a0)
	cmp.l	#0x03000000,%d4
	blo	init_too_old
	move.l	#0x63706E74,%d0			| Gestalt 'cpnt': the Component Manager
	.short	_Gestalt
	tst.w	%d0
	bne	init_no_cm
	tst.l	S_OUTKIND(%a2)
	beq	init_no_output
	| the component into the system heap: it must outlive this file
	move.l	#comp_end-comp_start,%d0
	.short	_NewPtrSysClear
	move.l	%a0,%d0
	beq	init_done
	move.l	%a0,%a3				| the resident block
	lea	comp_start(%pc),%a1
	move.l	#comp_end-comp_start,%d1
2:	move.b	(%a1)+,(%a0)+
	subq.l	#1,%d1
	bne.s	2b
	| its name, in a handle of the system heap, as RegisterComponent keeps it
	moveq	#20,%d0
	.short	_NewHandleSys
	move.l	%a0,%d0
	beq	init_done
	move.l	%a0,%a4
	move.l	(%a0),%a1
	lea	comp_name(%pc),%a0
	moveq	#19,%d1
3:	move.b	(%a0)+,(%a1)+
	dbf	%d1,3b
	| RegisterComponent(cd, entry, global = 1, name, nil, nil)
	subq.l	#4,%sp
	pea	cd-comp_start(%a3)
	pea	comp_entry-comp_start(%a3)
	move.w	#1,-(%sp)
	move.l	%a4,-(%sp)
	clr.l	-(%sp)
	clr.l	-(%sp)
	moveq	#1,%d0
	.short	_ComponentDispatch
	move.l	(%sp)+,%d0
	beq	init_reg_failed
	| the Sound Manager's output device: this one
	lea	cd-comp_start(%a3),%a0
	lea	comp_name(%pc),%a1
	bsr	set_odev
	lea	t_ok(%pc),%a1
	bsr	do_log
	tst.l	S_TEST(%a2)			| mac.audiotest=card
	beq.s	init_done
	lea	g_test-comp_start(%a3),%a0
	move.l	#TEST_FRAMES,(%a0)
	lea	t_test(%pc),%a1
	bsr	do_log
	jsr	feed_start-comp_start(%a3)
init_done:
	movem.l	(%sp)+,%d3-%d7/%a2-%a6
	rts
init_old_kernel:
	lea	t_oldk(%pc),%a1
	bra.s	init_say
init_too_old:
	lea	t_old(%pc),%a1
	bra.s	init_say
init_no_cm:
	lea	t_nocm(%pc),%a1
	bra.s	init_say
init_reg_failed:
	lea	t_regf(%pc),%a1
	bra.s	init_say
init_no_output:
	| the output preference back to the Sound Manager's built-in device, in case an earlier start
	| named this one: with the card silent the ASC must play
	lea	cd_builtin(%pc),%a0
	lea	name_builtin(%pc),%a1
	bsr	set_odev
	lea	t_noout(%pc),%a1
init_say:
	bsr	do_log
	bra.s	init_done

| SetSoundPreference('odev', "Output Device", h) with h = { the Sound Manager's version, the
| device's ComponentDescription (20 bytes), its name }: a0 = the description, a1 = the name
set_odev:
	movem.l	%a2-%a4,-(%sp)
	move.l	%a0,%a2
	move.l	%a1,%a3
	move.l	#280,%d0
	.short	_NewHandle
	move.l	%a0,%d0
	beq.s	9f
	move.l	%a0,%a4
	subq.l	#4,%sp
	move.l	#0x000C0008,%d0			| SndSoundManagerVersion
	.short	_SoundDispatch
	move.l	(%a4),%a0
	move.l	(%sp)+,(%a0)+
	moveq	#4,%d1
1:	move.l	(%a2)+,(%a0)+
	dbf	%d1,1b
	moveq	#0,%d1
	move.b	(%a3),%d1
2:	move.b	(%a3)+,(%a0)+
	dbf	%d1,2b
	subq.l	#2,%sp
	move.l	#0x6F646576,-(%sp)		| 'odev'
	pea	odev_name(%pc)
	move.l	%a4,-(%sp)
	move.l	#0x06340018,%d0			| SetSoundPreference
	.short	_SoundDispatch
	addq.l	#2,%sp
	move.l	%a4,%a0
	.short	_DisposeHandle
9:	movem.l	(%sp)+,%a2-%a4
	rts

| the slot-9 window: 32-bit or 24-bit address, as the Mac runs now
window:
	lea	0xF9000000,%a0
	tst.b	MMU32Bit
	bne.s	1f
	lea	0x00900000,%a0
1:	rts

| the Pi's log through the INFO service (as pimach.s): a1 = NUL-terminated text
do_log:
	bsr.s	window
	lea	OFF_INFO(%a0),%a0
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

t_ok:	.ascii	"PiSound: Sound Manager "
t_vers:	.ascii	"?.?; the sound card (16-bit stereo, 44.1 kHz, played by the Pi) registered and chosen as the output device"
	.byte	0
t_test:	.ascii	"PiSound: mac.audiotest=card: three seconds of 441 Hz left, 882 Hz right, into the card's ring"
	.byte	0
t_old:	.ascii	"PiSound: the Sound Manager is too old for the sound card (3.0 or later is needed: the Sound Manager extension on System 7.1); sound stays on the built-in ASC"
	.byte	0
t_nocm:	.ascii	"PiSound: no Component Manager: the sound card is not used"
	.byte	0
t_regf:	.ascii	"PiSound: RegisterComponent failed: the sound card is not used"
	.byte	0
t_noout: .ascii	"PiSound: the Pi plays no sound card (mac.audio=off, mac.sndcard=off, or no HDMI audio and no jack): the Sound Manager stays on the built-in ASC"
	.byte	0
t_oldk:	.ascii	"PiSound: this kernel has no sound card: nothing to do"
	.byte	0
odev_name: .byte 13
	.ascii	"Output Device"
cd_builtin: .long 0x73646576, 0x61736320, 0x6170706C, 0x80000009, 0	| 'sdev' 'asc ' 'appl', as the Sound Manager's 'thng' says
name_builtin: .byte 8
	.ascii	"Built-in"
	.even

| ---------------------------------------------------------------------------------------------
| The resident part, copied into the system heap. Everything in it is reached PC-relative, so it
| runs wherever it lands; its state is in the block itself (one card, one instance of interest).
comp_start:
cd:	.long	0x73646576			| 'sdev'
	.long	0x70697374			| 'pist'
	.long	0x50534D43			| 'PSMC'
	.long	0x0000000C			| k16BitIn | kStereoIn: what it takes from the mixer
	.long	0
comp_name: .byte 19
	.ascii	"Aurora68 Sound Card"
	.even
g_tm:	.long	0				| the Time Manager task: qLink
	.short	0				| qType
	.long	tm_task-comp_start		| tmAddr (made absolute when installed)
	.long	0, 0, 0				| tmCount, tmWakeUp, tmReserved
g_mixer: .long	0				| the Apple Mixer this device takes its samples from
g_src:	.long	0				| the mixer's current SoundComponentData, part used
g_running: .short 0				| the Sound Manager is playing: keep feeding
g_primed: .short 0				| the task is queued
g_inst:	.short	0				| the task is installed
g_test:	.long	0				| test frames still to write (mac.audiotest=card)
g_tph:	.long	0				| the test's frame count

| the component's entry: FUNCTION (params: ComponentParameters; storage: Handle): ComponentResult
| 4(sp) storage, 8(sp) params, 12(sp) result; params: +2 what, +4 the call's last parameter ...
comp_entry:
	movem.l	%d3-%d7/%a2-%a6,-(%sp)
	move.l	48(%sp),%a3
	bsr	card				| a2 = the sound card's block
	move.w	2(%a3),%d0
	cmp.w	#-1,%d0
	beq	c_open
	cmp.w	#-2,%d0
	beq	c_close
	cmp.w	#-3,%d0
	beq	c_cando
	cmp.w	#-4,%d0
	beq	c_version
	cmp.w	#1,%d0
	beq	c_init
	cmp.w	#3,%d0
	beq	c_getsource
	cmp.w	#0x103,%d0
	beq	c_getinfo
	cmp.w	#0x104,%d0
	beq	c_setinfo
	cmp.w	#0x105,%d0
	beq	c_start
	cmp.w	#0x108,%d0
	beq	c_playbuf
	cmp.w	#0x101,%d0
	beq	c_delegate			| AddSource, RemoveSource
	cmp.w	#0x102,%d0
	beq	c_delegate
	cmp.w	#0x106,%d0
	beq	c_delegate			| StopSource, PauseSource
	cmp.w	#0x107,%d0
	beq	c_delegate
c_bad:	move.l	#badSelector,%d0
c_ret:	move.l	%d0,52(%sp)
	movem.l	(%sp)+,%d3-%d7/%a2-%a6
	rtd	#8

c_open:	| Open(self): the storage is the resident block itself
	move.l	4(%a3),-(%sp)
	pea	comp_start(%pc)
	moveq	#13,%d0				| SetComponentInstanceStorage
	.short	_ComponentDispatch
	addq.l	#1,S_OPENS(%a2)
	moveq	#0,%d0				| the volume as the PRAM has it (the built-in device's rule)
	move.b	SdVolume,%d0
	and.w	#7,%d0
	lsl.w	#8,%d0
	addq.w	#6,%d0
	divu.w	#7,%d0
	move.w	%d0,%d1
	swap	%d0
	move.w	%d1,%d0
	move.l	%d0,S_VOLUME(%a2)
	moveq	#0,%d0
	bra.s	c_ret
c_close:
	bsr	feed_remove
	lea	g_mixer(%pc),%a0
	move.l	(%a0),%d0
	beq.s	1f
	clr.l	(%a0)
	lea	g_src(%pc),%a0
	clr.l	(%a0)
	subq.l	#2,%sp
	move.l	%d0,-(%sp)
	move.l	#0x02180018,%d0			| CloseMixerSoundComponent
	.short	_SoundDispatch
	addq.l	#2,%sp
1:	moveq	#0,%d0
	bra.s	c_ret
c_cando:
	move.w	4(%a3),%d1
	lea	can_list(%pc),%a0
1:	move.w	(%a0)+,%d0
	beq.s	2f
	cmp.w	%d0,%d1
	bne.s	1b
	moveq	#1,%d0
	bra.s	c_ret
2:	moveq	#0,%d0
	bra.s	c_ret
c_version:
	move.l	#0x00010000,%d0
	bra	c_ret
c_init:	| InitOutputDevice(self, actions): the Apple Mixer, asked for the card's own format
	lea	-28(%sp),%sp
	move.l	%sp,%a0
	clr.l	(%a0)+				| flags
	move.l	#0x74776F73,(%a0)+		| 'twos': signed, big-endian
	move.w	#2,(%a0)+			| stereo
	move.w	#16,(%a0)+			| 16 bits
	move.l	#0xAC440000,(%a0)+		| 44100.0
	move.l	#1024,(%a0)+			| the mixer's buffer, in frames
	clr.l	(%a0)+
	clr.l	(%a0)+
	move.l	%sp,%a0
	subq.l	#2,%sp
	move.l	%a0,-(%sp)
	clr.l	-(%sp)				| outputFlags
	pea	g_mixer(%pc)
	move.l	#0x06140018,%d0			| OpenMixerSoundComponent
	.short	_SoundDispatch
	move.w	(%sp)+,%d0
	lea	28(%sp),%sp
	ext.l	%d0
	bne	c_ret
	bsr	feed_install
	moveq	#0,%d0
	bra	c_ret
c_getsource:	| GetSource(self, sourceID, VAR source)
	move.l	4(%a3),%a0
	move.l	g_mixer(%pc),(%a0)
	moveq	#0,%d0
	bra	c_ret
c_getinfo:	| GetInfo(self, sourceID, selector, infoPtr): the hardware's own answers, the rest the mixer's
	move.l	4(%a3),%a4			| infoPtr
	move.l	8(%a3),%d1			| selector
	cmp.l	#0x6368616E,%d1			| 'chan'
	bne.s	1f
	move.w	#2,(%a4)
	bra.s	gi_ok
1:	cmp.l	#0x73726174,%d1			| 'srat'
	bne.s	1f
	move.l	#0xAC440000,(%a4)
	bra.s	gi_ok
1:	cmp.l	#0x7373697A,%d1			| 'ssiz'
	bne.s	1f
	move.w	#16,(%a4)
	bra.s	gi_ok
1:	cmp.l	#0x68766F6C,%d1			| 'hvol'
	bne.s	1f
	move.l	S_VOLUME(%a2),(%a4)
	bra.s	gi_ok
1:	cmp.l	#0x63686176,%d1			| 'chav': a SoundInfoList of one, 2
	bne.s	1f
	moveq	#2,%d3
	moveq	#2,%d4
	bra.s	gi_list
1:	cmp.l	#0x73726176,%d1			| 'srav': one rate
	bne.s	1f
	moveq	#4,%d3
	move.l	#0xAC440000,%d4
	bra.s	gi_list
1:	cmp.l	#0x73736176,%d1			| 'ssav': one size
	bne	c_delegate
	moveq	#2,%d3
	moveq	#16,%d4
gi_list:
	move.l	%d3,%d0
	.short	_NewHandle
	move.l	%a0,%d0
	beq.s	gi_mem
	move.w	#1,(%a4)
	move.l	%a0,2(%a4)
	move.l	(%a0),%a0
	cmp.w	#4,%d3
	beq.s	1f
	move.w	%d4,(%a0)
	bra.s	gi_ok
1:	move.l	%d4,(%a0)
gi_ok:	moveq	#0,%d0
	bra	c_ret
gi_mem:	moveq	#-108,%d0			| memFullErr
	bra	c_ret
c_setinfo:	| SetInfo(self, sourceID, selector, infoPtr): the value itself in infoPtr for these
	move.l	4(%a3),%d2
	move.l	8(%a3),%d1
	cmp.l	#0x68766F6C,%d1			| 'hvol': left << 16 | right, $0100 full
	bne.s	1f
	move.l	%d2,S_VOLUME(%a2)
	moveq	#0,%d0
	bra	c_ret
1:	cmp.l	#0x73726174,%d1			| 'srat': only the card's
	bne.s	1f
	cmp.l	#0xAC440000,%d2
	beq.s	si_ok
	move.l	#-225,%d0			| siInvalidSampleRate
	bra	c_ret
1:	cmp.l	#0x7373697A,%d1			| 'ssiz'
	bne.s	1f
	cmp.l	#16,%d2
	beq.s	si_ok
	move.l	#-226,%d0			| siInvalidSampleSize
	bra	c_ret
1:	cmp.l	#0x6368616E,%d1			| 'chan'
	bne.s	c_delegate
	cmp.l	#2,%d2
	beq.s	si_ok
	move.l	#-201,%d0			| notEnoughHardwareErr
	bra	c_ret
si_ok:	moveq	#0,%d0
	bra	c_ret
c_start:	| StartSource: the mixer starts its sources; the card starts taking
	bsr.s	delegate
	move.l	%d0,%d3
	bsr	feed_start
	move.l	%d3,%d0
	bra	c_ret
c_playbuf:	| PlaySourceBuffer(self, sourceID, pb, actions): unless it starts paused
	bsr.s	delegate
	move.l	%d0,%d3
	bne.s	1f
	btst	#0,7(%a3)			| actions bit 0: kSourcePaused
	bne.s	1f
	bsr	feed_start
1:	move.l	%d3,%d0
	bra	c_ret
c_delegate:
	bsr.s	delegate
	bra	c_ret
| DelegateComponentCall(params, mixer); badComponentSelector without a mixer
delegate:
	move.l	g_mixer(%pc),%d0
	bne.s	1f
	move.l	#badSelector,%d0
	rts
1:	subq.l	#4,%sp
	move.l	%a3,-(%sp)
	move.l	%d0,-(%sp)
	moveq	#0x24,%d0
	.short	_ComponentDispatch
	move.l	(%sp)+,%d0
	rts
can_list: .short -1, -2, -3, -4, 1, 3, 0x101, 0x102, 0x103, 0x104, 0x105, 0x106, 0x107, 0x108, 0

| a2 = the sound card's block, a5 untouched
card:	lea	0xF9000000+OFF_SND,%a2
	tst.b	MMU32Bit
	bne.s	1f
	lea	0x00900000+OFF_SND,%a2
1:	rts

| the Time Manager task, once: InsXTime
feed_install:
	lea	g_inst(%pc),%a0
	tst.w	(%a0)
	bne.s	1f
	move.w	#1,(%a0)
	lea	g_tm(%pc),%a0
	clr.l	(%a0)
	clr.w	4(%a0)
	lea	tm_task(%pc),%a1
	move.l	%a1,6(%a0)
	clr.l	10(%a0)
	clr.l	14(%a0)
	clr.l	18(%a0)
	.short	_InsXTime
1:	rts
feed_remove:
	lea	g_inst(%pc),%a0
	tst.w	(%a0)
	beq.s	1f
	clr.w	(%a0)
	lea	g_primed(%pc),%a0
	clr.w	(%a0)
	lea	g_running(%pc),%a0
	clr.w	(%a0)
	lea	g_tm(%pc),%a0
	.short	_RmvTime
	bsr	card
	clr.l	S_STATE(%a2)
1:	rts
| playing begins: the task queued for a millisecond from now (the card's state goes on with the first frames)
feed_start:
	move.l	%a2,-(%sp)
	bsr.s	feed_install
	bsr	card
	move.w	%sr,-(%sp)
	ori.w	#0x0700,%sr
	lea	g_running(%pc),%a0
	tst.w	(%a0)
	bne.s	1f
	move.w	#1,(%a0)
	addq.l	#1,S_STARTS(%a2)
1:	lea	g_primed(%pc),%a0
	tst.w	(%a0)
	bne.s	2f
	move.w	#1,(%a0)
	lea	g_tm(%pc),%a0
	moveq	#1,%d0
	.short	_PrimeTime
2:	move.w	(%sp)+,%sr
	move.l	(%sp)+,%a2
	rts

| the task, at interrupt time (A1 = the task; D0-D3/A0-A3 are ours, the rest is kept)
tm_task:
	movem.l	%d4-%d7/%a4-%a6,-(%sp)
	lea	g_primed(%pc),%a0
	clr.w	(%a0)
	bsr	card
	lea	OFF_RING-OFF_SND(%a2),%a3	| the ring
	addq.l	#1,S_FEEDS(%a2)
	bsr.s	feed
	move.w	g_running(%pc),%d0
	beq.s	1f
	lea	g_primed(%pc),%a0
	move.w	#1,(%a0)
	lea	g_tm(%pc),%a0
	moveq	#10,%d0
	.short	_PrimeTime
1:	movem.l	(%sp)+,%d4-%d7/%a4-%a6
	rts

| fill the ring up to TARGET frames ahead of the Pi: from the test, or from the mixer
feed:
1:	move.l	S_HEAD(%a2),%d4
	move.l	%d4,%d5
	sub.l	S_TAIL(%a2),%d5			| queued
	move.l	#TARGET,%d6
	sub.l	%d5,%d6				| frames wanted
	ble	9f
	move.l	%d4,%d7				| frames to the ring's end
	and.l	#RING_FRAMES-1,%d7
	move.l	%d7,%a5
	neg.l	%d7
	add.l	#RING_FRAMES,%d7
	cmp.l	%d7,%d6
	bls.s	2f
	move.l	%d7,%d6
2:	move.l	%a5,%d0
	lsl.l	#2,%d0
	lea	(%a3,%d0.l),%a4			| where they go
	move.l	g_test(%pc),%d0
	bne	feed_test
	| the mixer's samples: the part left of its last buffer, or a new one
	move.l	g_src(%pc),%d0
	beq.s	3f
	move.l	%d0,%a6
	tst.l	D_COUNT(%a6)
	bne.s	4f
3:	move.l	g_mixer(%pc),%d0
	beq	feed_stop
	subq.l	#4,%sp
	move.l	%d0,-(%sp)
	pea	g_src(%pc)
	move.l	#0x00040004,-(%sp)		| SoundComponentGetSourceData
	moveq	#0,%d0
	.short	_ComponentDispatch
	move.l	(%sp)+,%d0
	bne	feed_stop
	move.l	g_src(%pc),%d0
	beq	feed_stop
	move.l	%d0,%a6
	tst.l	D_COUNT(%a6)
	beq	feed_stop
4:	move.l	D_COUNT(%a6),%d0
	cmp.l	%d0,%d6
	bls.s	5f
	move.l	%d0,%d6				| d6 = frames this pass
5:	move.l	D_BUFFER(%a6),%a0
	move.l	%d6,%d1
	cmp.w	#16,D_SIZE(%a6)
	bne.s	7f
	cmp.w	#2,D_CHANNELS(%a6)
	bne.s	6f
	| 16-bit stereo, the format asked for: frames as longs
	subq.l	#1,%d1
51:	move.l	(%a0)+,(%a4)+
	dbf	%d1,51b
	bra.s	8f
6:	| 16-bit mono: each sample to both sides
	subq.l	#1,%d1
61:	move.w	(%a0)+,%d0
	move.w	%d0,(%a4)+
	move.w	%d0,(%a4)+
	dbf	%d1,61b
	bra.s	8f
7:	| 8-bit offset binary, mono or stereo
	subq.l	#1,%d1
71:	move.b	(%a0)+,%d0
	eor.b	#0x80,%d0
	lsl.w	#8,%d0
	move.w	%d0,(%a4)+
	cmp.w	#2,D_CHANNELS(%a6)
	bne.s	72f
	move.b	(%a0)+,%d0
	eor.b	#0x80,%d0
	lsl.w	#8,%d0
72:	move.w	%d0,(%a4)+
	dbf	%d1,71b
8:	move.l	%a0,D_BUFFER(%a6)
	sub.l	%d6,D_COUNT(%a6)
	add.l	%d6,%d4
	move.l	%d4,S_HEAD(%a2)
	move.l	#1,S_STATE(%a2)			| playing, from the first frames on (an empty ring is an underrun now)
	bra	1b
9:	rts
feed_stop:	| the mixer has nothing: the Sound Manager has stopped; the Pi plays out what is queued
	lea	g_running(%pc),%a0
	clr.w	(%a0)
	clr.l	S_STATE(%a2)
	rts
feed_test:	| d0 frames of test left: 441 Hz (100 frames) left, 882 Hz (50 frames) right, quarter scale
	cmp.l	%d0,%d6
	bls.s	1f
	move.l	%d0,%d6
1:	lea	g_test(%pc),%a0
	sub.l	%d6,(%a0)
	bne.s	2f
	lea	g_running(%pc),%a0		| the last of it: the task stops after this pass
	clr.w	(%a0)
	clr.l	S_STATE(%a2)
2:	lea	g_tph(%pc),%a0
	move.l	(%a0),%d2
	move.l	%d6,%d1
	subq.l	#1,%d1
3:	move.l	%d2,%d0
	divul.l	#100,%d3:%d0			| d3 = the frame's place in the left side's period
	move.w	#8192,%d0
	cmp.l	#50,%d3
	blo.s	4f
	neg.w	%d0
4:	move.w	%d0,(%a4)+
	cmp.l	#50,%d3				| the right side's period is half as long
	blo.s	41f
	sub.l	#50,%d3
41:	move.w	#8192,%d0
	cmp.l	#25,%d3
	blo.s	5f
	neg.w	%d0
5:	move.w	%d0,(%a4)+
	addq.l	#1,%d2
	dbf	%d1,3b
	move.l	%d2,(%a0)
	add.l	%d6,%d4
	move.l	%d4,S_HEAD(%a2)
	move.w	g_running(%pc),%d0
	bne	feed
	rts
	.even
comp_end:
