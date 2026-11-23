# A second display from the Pi's VideoCore

## Why this is possible at all
The Macintosh SE has one screen: a 512 by 342 one-bit display driven by the motherboard's video
circuit out of the first 21.9 KB of RAM (docs/01). Every IIci-ROM machine, on the other hand, was
built for colour cards in NuBus slots: the ROM carries a Slot Manager, the system a video driver
interface (Inside Macintosh: Devices, "Video Driver"), and the Monitors control panel, QuickDraw and
the Finder cope with any number of screens of any depth. Since this project runs the IIci ROM on the
SE (docs/00), the software side of a second monitor is already paid for.

The hardware side is on the Pi: the VideoCore scans out frame buffers from RAM to HDMI (or to the
RGB header on AGA-PiStorm hardware). Alice68 already drives it bare-metal for the virtual Amiga
(`arm/aga_hvs.c` in the Alice68 tree: HVS plane configuration, mode setting through the firmware
mailbox, double buffering), and AGA-PiStorm before it proved the path on a Pi 4. The 68k never
touches VideoCore registers; it draws into memory and the Pi displays it.

## The card the Mac will see
A third functional sResource in the slot-$9 declaration ROM (docs/09): category catDisplay (3),
type typVideo (1), name `Display_Video_PiStorm`, with a video DRVR in its sDriverDir and the mode
list the Slot Manager expects (sVidParams per mode: VPBlock with rowBytes, bounds, depth, the pixel
type, plus sVidAttributes). The IIci ROM's own built-in video is described exactly that way (the
`Display_Video_Apple_RBV1` sResources that `tools/declrom.py` lists from the ROM), so the format is
known and tested.

The frame buffer is Pi RAM carved next to the slot window: for a 1024 by 768 screen at 8 bits that
is 768 KB; at 32 bits 3 MB. It is mapped into the 68k's view at the slot's super slot space (the
IIci ROM maps $F9000000..$F9FFFFFF for slot 9; our window uses the bottom 1 MB and the declaration
ROM the top 64 KB, leaving 14 MB in between for the buffer) as ordinary cached memory. QuickDraw
draws straight into it. The VideoCore reads it: Pi RAM is coherent with the VideoCore when the ARM
side flushes its caches, so the driver's VBL task or the service core issues a cache clean of the
buffer at each frame (or the buffer is mapped uncached on the 68k side, as the frame buffer is on a
real card).

## The driver
Apple's video driver API is small: Open/Close, Control calls cscReset, cscKillIO, cscSetMode
(depth), cscSetEntries (colour table), cscSetGamma, cscGrayScreen, cscSetGray, cscSetInterrupt,
cscDirectSetEntries, and Status calls cscGetMode, cscGetEntries, cscGetPageCnt, cscGetPageBase,
cscGetGray, cscGetInterrupt. Mode changes and the colour table are commands to the Pi over the
mailbox (a `display` service: set mode, set palette, vertical blank count); everything else is
bookkeeping in the driver. The display service on the Pi programs the HVS and the palette and
reports the real timing. The Slot Manager opens the video driver at boot like any other
fOpenAtStart sResource (that is how the ROM found the Mac's own video), so the second screen exists
from the boot screen onwards and the Monitors control panel can arrange it.

## Interrupts
QuickDraw's cursor and the VBL tasks of a screen need the card's vertical blank interrupt (the
driver's cscSetInterrupt). Until the virtual slot interrupt exists (docs/07), the service core
counts frames into the mailbox and the Mac's Time Manager/VBL task approximates the 60 Hz tick; the
proper version raises the slot-9 interrupt at every VideoCore vertical sync.

## What exists (17 November)
The first version is built and lit: `emu68/src/psmac/psmac_display.c` (the display block and the
conversion into the VideoCore frame buffer the boot splash set up, sixty ticks a second, the
slot interrupt as the VBL), `mac/pivideo.s` (the driver), the video sResource in
`tools/mac/mkdeclrom.py --video` with the four indexed modes at 640 by 480, and
`tools/vcshot.py`, which photographs QEMU's emulated VideoCore frame buffer through the monitor
socket. The Start Manager opens the driver from the declaration ROM at boot, the system sets its
colour table and luminance mode, and the desktop pattern appears on the Pi's screen beside the
SE's (`docs/img/system71-second-display-desktop.png`). Two things changed from the design
above as it met the machine: the frame buffer is in the window's top 512 KiB rather than out in
the slot's 16 MiB, so that 24-bit System 6 can reach it (MinorBaseOS 0x80000: $980000 in 24-bit
mode, $F9080000 in 32-bit, and the driver asks MMU32Bit which), and the Pi converts the indexed
pixels instead of mapping the VideoCore buffer into the Mac's space, because the Mac's modes are
1 to 8 bits with a colour table and the VideoCore's are direct; a 300 KB copy sixty times a
second costs the service core nothing. Not yet: Monitors tried in the testbed (it needs a
scripted trip through the control panel), 8-bit colour seen on the Pi's screen (the machinery
is there: SetEntries fills the table, the conversion applies it), gamma, 16/32-bit direct modes,
other resolutions, the HVS on real hardware.

## It works (17 November, 01:15)
With 32-bit addressing on (`tools/pram32.py`) and a 'scrn' resource in the System file naming
both screens (`tools/mac/mkscrn.py`, installed with `tools/mac/rsrcadd.py`), System 7.1 boots
with two screens: the SE's at (0,0)-(480,640), main, one bit; the Pi's at (0,640)-(480,1280),
active, 256 colours (`docs/img/system71-piscreens-dual.png`). The Finder's desktop covers both,
and an icon dragged off the right edge of the SE's screen appears on the Pi's
(`docs/img/system71-second-display-icon-se.png`, `-pi.png`: the "Mac Volume" icon cut in two by
the edge, half on each monitor). The values the 'scrn' needs come from Aurora68 Screens: the IIci's
built-in video is sResource 134 of "slot 0" with hardware id $18 and device base $FBB08000,
ours is sResource 132 of slot 9, hardware id $5056, device base $F9980000. The 7.1 image
(`images/sys710pi.image`) carries the resource now; on a machine without the IIci's built-in
video the count will not match and System 7 falls back to one screen, which is harmless.
The record and the loader's rules are in mkscrn.py's notes and docs/08 item 26, including the
mask bit that cost an hour: clear allInit in the mask and every program's InitGraf runs the
loader again, whose width test only works for a screen at the origin, and the second screen
walks off to the right by its own width each time.

## Direct colour and other sizes (17 November, 02:00)
Version 2 of the display block (psmb.h) adds per-mode tables the Pi fills in: for each of the six
modes (128 to 133: 1, 2, 4, 8, 16, 32 bits) the 68k address of its frame buffer and its row pitch.
Two regions exist: the window's top 512 KiB at $F9980000, which a 24-bit System 6 can reach and
which holds 640 by 480 at up to 8 bits, and a big region of 8 MiB of Pi RAM mapped at $F9100000
in the slot's 16 MiB space (enough for 1920 by 1080 at 32 bits), for direct colour and for larger
screens; `psmac_mmu.c` maps it, `psmac_bus.c` carves it. The driver reads the tables and never
decides placement. The VideoCore buffer is remade at start-up at the screen's size and 32 bits
(`psmac_vc_setup` in support_rpi.c; the splash's was 16), and the conversion writes XRGB words:
indexed pixels through the colour table, 16-bit 0RRRRRGGGGGBBBBB words widened, 32-bit 00RRGGBB
longs byte-swapped. The screen's size is `mac.screen=WxH` (512..1920 by 342..1200) or the
VideoCore's own, and the declaration ROM must agree: `make -C mac` now builds one per size
(`psmac.declrom` for 640 by 480, `psmac-800x600.declrom`, `-1024x768`, `-1280x720`, `-1280x1024`,
`-1920x1080`), and the kernel takes from the container the one whose name ends in the size in use,
else the plain one. In the testbed `mac.declrom=` names the file and `mac.screen=` the size.
Monitors' 'scrn' sets the depth with `mode=` (133 is millions of colours) and the rectangle must
be the screen's size.

The first 32-bit picture (fs153, fs154) was thin dark red stripes over the whole screen: pixels
(128,16,0) and (0,16,0) in turn, which is what a 16-bit scan-out makes of 0x00808080 written
four bytes wide. QEMU had granted 32 bits and the pitch said so; the depth was lost afterwards.
The 68000 RESET instruction, which the ROM issues at the start of every boot, takes the JIT to
Emu68's `do_reset`, and that remade the VideoCore buffer at 16 bits for the Amiga's splash screen.
Under PSMAC it now calls `psmac_display_reinit`, which asks for the same buffer again at the Mac's
depth. Byte order is the other trap: QEMU's model reads the first byte of a 32-bit pixel as red
(its "pixel order 1, RGB"), a Pi's firmware answers the same tag for its own scan-out, so the
kernel asks (tag 0x40006) and writes R,G,B,x or B,G,R,x accordingly; `mac.vcswap=0|1` overrides
the answer if a board disagrees. The firmware's answer is also kept in the display block
(`vc_depth`) for the Mac side to read.

The third trap was the ROM's: with the scan-out right the 32-bit screen was a plain grey, and
Aurora68 Screens showed the device active at 32 bits but based at $F9980000, the window, while the Pi
converted the big region. InitGDevice never asks the driver where a mode's pixels are (no
GetPageBase in the trace); it adds the mode's `vpBaseOffset` from the sResource to dCtlDevBase.
mkdeclrom.py now writes that offset by the kernel's placement rule, and `docs/img/
system71-pi-1024x768-millions.png` is the result: the Pi's screen at 1024 by 768 in millions of
colours (`mac.screen=1024x768`, `psmac-1024x768.declrom`, a 'scrn' with mode 133 and the
rectangle 0,640,768,1664), Aurora68 Colour drawn by Color QuickDraw, the Finder's dither pattern behind
it, 3 MiB of frame buffer that cost the Macintosh nothing. Thousands of colours at 640 by 480
(mode 132, fs165) draws the same window with the 16-bit pixels widened.

## 1920 by 1200 (18-19 November)
The user asked for a 1920 by 1200 screen in millions of colours. Two things stood in the way. No
declaration ROM had that size (`RESES` in mac/Makefile stopped at 1920x1080), and the big region
was too small: 1920 x 1200 x 4 bytes is 9,216,000, the region 8 MiB (8,388,608); 1080p, at
8,294,400, had only just fitted. The region could not grow where it was. The slot's 16 MiB is
spoken for: the window at $F9000000, the big region from $F9100000, the window again at
$F9900000 (the ROM addresses the frame buffer as $F9980000) and the declaration ROM at
$F9FF0000. What is left, $F9A00000-$F9FEFFFF, is under 6 MiB.

But a NuBus slot owns 256 MiB more in 32-bit space, its super slot space: $90000000-$9FFFFFFF
for slot 9. The big region now lives at the bottom of it, 9 MiB at $90000000 (psmb.h), which
holds every size `mac.screen` accepts, up to 1920 by 1200, at 32 bits. Only 32-bit systems
could reach the old place either, so nothing is lost: indexed modes that fit the window's top
512 KiB stay there for 24-bit System 6, as before. The IIci ROM's 32-bit tables map
$40000000-$FBAFFFFF one to one, so the MMU shim maps logical $90000000 to the Pi RAM behind it
just as it did $F9100000, and the carve-out next to the window grows by 1 MiB. The kernel's
placement rule is unchanged, so is the display block's layout (it stays at version 2), and
mkdeclrom.py writes the same rule into each mode's vpBaseOffset: $90000000 - $F9980000, a sum
that wraps round to the right address in 32 bits, and an error for a mode the region could not
hold. One safety catch went into the driver: its grey fill writes to the mode's address, and in
24-bit mode $90000000 comes out as $000000, the exception vectors, so the fill leaves a mode
outside the slot's 24-bit megabyte alone until the Mac is in 32-bit mode.

On the solo board (`tools/run-solo.sh`, `mac.screen=1920x1200`, `psmac-1920x1200.declrom`, a
System 7.1 image with "Aurora68 Depth 32" and Aurora68 Colour in Startup Items) QEMU granted the VideoCore
buffer at 1920 x 1200 x 32 with a pitch of 7680, the driver's SetMode went to mode 133, and the
heartbeat read `depth 32 rowbytes 7680`. `docs/img/system71-pi-1920x1200-millions.png` is the
result: Aurora68 Colour's hues and its sixteen-step grey ramp in millions of colours on a 1920 by 1200
desktop. The VideoCore buffer at that size is 8.8 MiB; the firmware allocates it out of the
`gpu_mem=64` split, which leaves room for it (a Pi 3 drives 1920 by 1200 at 60 Hz only with
reduced blanking, `hdmi_group=2`, `hdmi_mode=69`). Not seen yet on a real Pi and monitor.
The same runs checked what was there before: 1920 by 1080 in millions (heartbeat `depth 32
rowbytes 7680`, Aurora68 Ping's window over the Finder, `docs/img/solo-1920x1080-wifi-piping.png`,
which is also the WiFi test of docs/09), 640 by 480 at 256 colours (the window region, `depth 8
rowbytes 640`, the Finder in colour), and a 1024 by 768 run at 256 colours, whose 768 KiB frame
buffer is in the big region too, drawing for ten minutes in another session's testbed. Aurora68 Colour's
window is placed at x = 660 for the two-screen layout, so at 640 by 480 on the solo board it is
off the screen; that is Aurora68 Colour's, not the display's.

## Two screens on the SE (20 November 2026)
The user's SE bring-up begins with a Pi 3B on the classic PiStorm, and they asked for what the IIci
testbed had shown on 17 November ("It works") on the SE as well: the SE's own 512 by 342 one-bit
screen and the Pi's HDMI as a second, larger screen, at any size up to 1920 by 1200 and any depth,
both in use at once, with the SE's as the main screen. Until today `mac.board=se` had one screen:
the slot-9 card's single video sResource *was* the SE's (its 512 by 342 one-bit frame copied row
by row into the SE's video RAM by `psmac_se_mark`), and HDMI showed the same picture or the log.

**The choice.** Two shapes fit the Slot Manager. One card with two video sResources: one
declaration ROM, one window, one interrupt line; the Start Manager installs a driver per sResource
(a different driver name each, so the Device Manager gives each its own unit), and the ROM's
screen set-up makes a GDevice for each. Or two cards: slot 9 as before and a second virtual card
in another slot, each with its own declaration ROM. The SE has no NuBus, so the slot space is
ours: on `mac.board=se` the kernel's board layer answers every slot address outside the I/O block
with $FF (an empty slot), and the RBV it emulates reports slots $A-$E idle. The ROM size budget
is no constraint either way (a two-screen card is 14 KB of the 64 KB). The first version was the
single card, `$84` the SE's screen and `$85` the HDMI one; it booted with two screens at once, and
then Monitors gave it away: for the HDMI screen it listed one depth, "Black & White", and drew
both monitors the same size. Monitors 7.0 takes a screen's depths (and its proportions) from the
first video sResource of the screen's slot, here the SE's one-bit `$84`; the driver's DCE, the
GDevice and InitGDevice all had the right sResource (Aurora68 Screens: `dctl 9:133`). The same card on
the solo board, alone in its slot, lists all six depths. So the HDMI screen moved to a card of its
own, and the design is **two cards**:

- *Slot 9*, the card it always was (disk, input, the two network cards, the status page, the
  log), with its video sResource `$84` now the SE's own screen, 512 by 342, mode 128 (one bit)
  only: the SE's hardware has no other.
- *Slot $A*, the HDMI screen's card: a board sResource ("Aurora68 HDMI", BoardId 'PH') and one
  video sResource `$80`, drHw `$5048`, every mode from one bit to millions at `mac.screen`'s size,
  and a driver, `.Display_Video_PiHDMI`, which is `mac/pivideo.s` assembled with `SCREEN2`. The
  card is a declaration ROM and nothing more. Its driver reaches the slot-9 window at its own
  addresses (its display block is the second one, window + $59000, psmb.h), installs its slot
  interrupt handler on slot 9, whose line the Pi raises, and runs slot $A's VBL queue at its
  ticks; its modes' vpBaseOffsets reach from its dCtlDevBase ($FAA80000) into slot 9's space,
  wrapping round in 32 bits as the big region's always did. The kernel serves the ROM from the
  window's top 16 KiB, mapped at the top of slot $A's space ($FAFFC000), on the SE board only.
- *The window's top 512 KiB* is split so that both screens keep what a 24-bit system reaches: the
  SE's frame in its first 32 KiB ($F9980000, 21,888 bytes used), the HDMI screen's indexed modes
  in the other 480 KiB ($F9988000) where they fit (640 by 480 and 800 by 600 up to 256 colours,
  1024 by 768 up to 16), everything else in the big region at $90000000 as before. So System 6
  and a 24-bit System 7 keep the SE's screen, and the HDMI screen where it fits; 32-bit addressing
  gives millions at every size. The driver now refuses (paramErr) a mode whose pixels a 24-bit
  system cannot reach, which Monitors would otherwise have offered.
- *Interrupts*: one line, slot 9's, raised while either display block (or the network board, or
  the input ring) has something pending. Each video driver's handler claims only its own block's
  tick (`D0 = 1` only when its `vbl_pending` was set), the input driver's catch-all comes last, as
  it always has. Each driver runs its own slot's VBL queue, so each queue runs sixty times a
  second.
- *One file*: the SE's declaration ROMs are `psmac-se-WxH.declrom` (`aurora68-se-WxH.declrom` in
  the release), slot $A's ROM followed by slot 9's, so the top of the file is still slot 9's
  format block; the kernel finds slot $A's by slot 9's length and checks its test pattern
  (`declrom_below`, psmac_bus.c). `make -C mac` builds them for 640x480, 800x600, 1024x768,
  1280x720, 1280x1024, 1600x1200, 1920x1080 and 1920x1200 (`SE_RESES`).

**HDMI's three jobs, `mac.hdmi=`.** `screen` (the default on the SE board): the second screen,
with an `-se-` ROM in the container of the HDMI size (`mac.screen=WxH`, else the size the
firmware's HDMI mode gave the VideoCore). `mirror`: the SE's picture, as until today (the plain
`psmac-512x342` ROM, or an `-se-` one whose slot $A card the kernel then leaves out). `log`: the
kernel's log for the whole run, at the HDMI size rather than 512 by 342 (`mac.console=hdmi` means
the same). Without an `-se-` ROM the kernel says so and mirrors. On the solo board and the IIci
testbed nothing changed: an `-se-` file is the last choice there and its slot $A card is never
served.

**The second screen active without Monitors: Aurora68 Screen Setup.** The ROM's 'scrn' loader (docs/08 item 26)
activates a second screen only when the System file's 'scrn' names every screen and matches the
hardware; without one the HDMI screen stays the driver's grey. Monitors writes one when a screen
is dragged and OK clicked, but on a 9-inch screen that is a lot to ask. Aurora68 Screen Setup
(`mac/piscrn/piscrn.c`, an INIT built with the C kit) does it at startup: it lists the video
sResources as the loader does, checks the 'scrn' by the loader's rules (count; each record's
hardware id, slot, dCtlDevBase; a rectangle of the screen's size), and keeps it if it matches,
which is also how a Monitors arrangement survives. If not (none, or the HDMI size changed), it
writes one, the SE's screen main at the origin and the HDMI screen to its right, top-aligned, in
millions with 32-bit addressing (else the deepest mode the 24-bit window holds), and restarts the
Mac once, since the loader runs at InitGraf, before any extension. If the same bytes are already
there and still do not match, it says so in the log rather than restart again. It does nothing on
a Mac with one screen or without the Pi's window.

**Two bugs the restart found.** The first two-card run hung right after Aurora68 Screen Setup's restart, the 68k
in its level-2 interrupt with no bus traffic: the display block's "enabled" flag had outlived the
68k's RESET, and the HDMI driver's Open turns VBLs on before it installs its handler, with a grey
fill of 288 KB (1920 by 1200 at one bit) in between, so the first tick raised slot 9's line with
nobody to take it but the input driver's catch-all, which cannot lower it. Now the kernel clears
the flag at RESET and the driver enables VBLs only once its handler is in. And the mouse: the
input driver clipped every movement (USB, Bluetooth, the remote's absolute moves) to CrsrPin,
the rectangle of the screen the cursor is on, so nothing but the SE's own ADB mouse could reach
the second screen; the first click on the HDMI screen landed on the SE's right edge. It now clips
to the rectangle around every active screen and leaves the rest to the ROM's cursor task, as
for an ADB mouse.

**The rest.** The kernel's display code keeps a block per screen (`struct screen`,
psmac_display.c): screen 0 feeds the SE's video RAM (now every other tick whatever the
VideoCore shows, which also mends `mac.console=hdmi` on the SE board: the SE's screen stopped
changing then, because its rows were sent only at the end of an HDMI conversion), screen 1 is
converted for the VideoCore. The remote control (docs/13) photographs either screen
(`/screen.png?display=0|1`, `aurora68.py --display 1 screenshot`), takes `"display": N` on input
events to mean a point of that screen, wherever Monitors put it, and lists both screens with
their place on the desktop in `/state` ("screens", from QuickDraw's GDevices). Aurora68 Colour opens its
window on the first active screen that is not the main one.

**What the testbed showed** (MAME `macse_ps`, System 7.1 on the SE's SCSI disk with Aurora68 Screen Setup in the
Extensions folder and Aurora68 Colour in Startup Items, 16 MB, 32-bit addressing on in MAME's PRAM):
- 1024 by 768, the first (one-card) version: the first boot wrote the 'scrn' and restarted; the
  second came up with the Finder on the SE's screen and Aurora68 Colour's twelve hues in millions of
  colours on the HDMI screen at (512,0)-(1536,768); `/state` had both screens active, the SE's main.
- 1920 by 1200, the same: Aurora68 Screen Setup saw the HDMI size change, wrote a new 'scrn' and restarted;
  Aurora68 Colour in millions on 1920 by 1200, the disk window dragged by its title bar from the SE's
  screen onto the HDMI one (with the input driver's clipping mended), Aurora68 Screens listing both
  GDevices and both sResources with the values above, and Monitors' one-depth list that led to
  the second card.
- 1920 by 1200, two cards: Aurora68 Screen Setup saw the HDMI screen's slot change from 9 to $A and rewrote the
  'scrn' (that restart is the one that hung, above); with the fixes the next boot reached the
  Finder in 146 s with both screens active and Aurora68 Colour in millions on the HDMI screen
  (`docs/img/se-hdmi-1920x1200-millions-picolor.png`). Monitors now shows monitor 2 in its true
  proportion beside the SE's with the depths 4, 16, 256, thousands and millions, millions selected
  (`docs/img/se-monitors-two-screens.png`, MAME's picture of the SE's own screen); choosing 256
  switched the HDMI screen to 8 bits at once, and the Control Panels window, dragged by the remote
  control from the SE's screen, came up on the HDMI one in colour
  (`docs/img/se-hdmi-1920x1200-256-window-moved.png`), leaving the SE's Finder desktop behind
  (`docs/img/se-two-screens-se-finder.png`, MAME).
- 1024 by 768, two cards: Aurora68 Screen Setup's restart again (the size changed), this time through to the
  Finder; Aurora68 Colour in millions on the VideoCore's own picture (`tools/vcshot.py`,
  `docs/img/se-hdmi-1024x768-millions-picolor.png`).

## On the hardware: the firmware's buffer, and sound (20 November 2026)
Everything in "Two screens on the SE" ran on QEMU's model of the VideoCore, which shows any buffer the
kernel is given. The real firmware had already shown one difference (docs/08 item 65): a 32-bit buffer
asked for at the HDMI mode's own size stayed black on the user's monitor, while the boot buffer, where the
splash and the first log lines are drawn, is on every Pi's screen. The HDMI screen of the two screens now
takes that boot buffer when it already has the screen's size, which it has whenever `mac.screen=` matches
the `hdmi_mode`, and converts into it at 16 bits: QuickDraw still draws in millions in the big region, the
monitor shows RGB565. `mac.vcdepth=32` asks for a 32-bit buffer instead, by a sequence that now follows
Circle's (physical and virtual size, depth, virtual offset, "alpha ignored", allocate, pitch) with the
request handed to the firmware through the VideoCore's uncached alias, as Circle and Linux do; a message
that only reads then puts the firmware's own view of the buffer into the log ("VideoCore read back"), so
a black monitor can be told from a wrong buffer. docs/08 item 73.

The monitor's loudspeakers play the Mac's sound now (docs/08 item 74): the ASC's two channels and a sound
card of 16-bit stereo at 44.1 kHz, mixed on the Pi's core 1 and sent to the HDMI transmitter's audio
FIFO by DMA. The card is a ring in slot 9's window that the Aurora68 Sound extension makes the Sound
Manager's output device. The user's steps for both are in docs/09, "Two screens and sound through HDMI
on the real SE".

## 2560 by 1440 (20 November 2026, evening)
The user's SE has run two screens on hardware since the se9 kernel (5799de9), at 1024 by 768 and
1920 by 1200. They now have a Dell UP2716D, 2560 by 1440, and asked for the HDMI screen at its
native size.

**The monitor's side: 30 Hz.** 2560 by 1440 at 60 Hz needs a pixel clock of about 241 MHz even with
CVT's reduced blanking; the Pi 3's HDMI cannot send that (1920 by 1200 at 60 Hz already needed
reduced blanking, 154 MHz, `hdmi_mode=69`). At 30 Hz with reduced blanking it is about 121 MHz, which
it can. The firmware makes such a mode from a CVT description: in config.txt

    hdmi_group=2
    hdmi_mode=87
    hdmi_cvt=2560 1440 30 3 0 0 1
    max_framebuffer_width=2560
    max_framebuffer_height=1440

(`hdmi_mode=87` is "the custom mode", `hdmi_cvt` is width, height, refresh, aspect 3 = 16:9, no margins,
not interlaced, reduced blanking; the last two lines let the firmware make its frame buffer that
large). Whether the monitor takes 30 Hz is the monitor's affair; the user is finding out.

**The Mac's side: every limit that stopped at 1920 by 1200.**
- *mac.screen=* took 512..1920 by 342..1200 (psmac_bus.c). Now 512..4096 by 342..2560 (psmb.h,
  `PSMB_SCREEN_*`), and with no `mac.screen=` the size the firmware's mode gave its frame buffer, as
  before.
- *The big region*, the Pi RAM behind $90000000 in slot 9's super slot space where every mode that does
  not fit the window lives, was 9 MiB: 1920 x 1200 x 4 bytes is 8.8 MiB. 2560 x 1440 in millions is
  2560 x 4 x 1440 = 14,745,600 bytes. The address range is no limit: the super slot space has 256 MiB,
  and the slot $A card's modes reach into it from $FAA80000 by the same wrapping vpBaseOffset as the
  1920 by 1200 ones; the IIci ROM's 32-bit tables map it one to one. What it costs is Pi RAM, carved at
  start-up and taken from the Mac's memory and the disk images' room, so it grows only when the screen
  needs it: 9 MiB up to 1920 by 1200, exactly as before, and the 32-bit frame rounded up to a MiB above
  that (15 MiB at 2560 by 1440; at most 32 MiB, which holds 3840 by 2160). The MMU shim maps the size in
  use. So millions of colours fit, and nothing had to be capped at thousands. QuickDraw's limit on a
  pixel map's rowBytes, $3FFE, is the other bound: 2560 at 32 bits is 10,240; a depth whose rows would be
  wider (4096 at 32 bits) is left out.
- *The declaration ROMs* (`RESES`, `SE_RESES`, the WiFi ones) stopped at 1920x1200; 2560x1440 is built
  too. Since the next section, every two-screen file serves every size anyway.
- *The VideoCore*. With `mac.screen=` equal to the firmware's mode the HDMI screen keeps the firmware's
  own 16-bit buffer (docs/08 item 73): 2560 x 1440 x 2 = 7.4 MB, out of the `gpu_mem=64` split, where the
  1920 by 1200 one was 4.6 MB; the firmware's own needs at this setting are a few MB. `mac.vcdepth=32`
  asks for a second, 32-bit buffer of 14.7 MB beside it, which should still fit in 64 MB but is the
  first thing to suspect if the read-back line says `NOT what was asked` (then `gpu_mem=128`). QEMU's
  model has 64 MB of VideoCore memory and takes either.
- *The copy.* The service core converts the Mac's frame into the VideoCore's a band at a time (docs/08
  item 49). A 2560 by 1440 frame in millions is 14.7 MB read and 7.4 MB written and flushed, 1.6 times
  1920 by 1200: on a Pi 3, scaled from the 1920 by 1200 figure (20 ms for 9 + 9 + 9 MB), some 22 ms of
  core 3 a frame into the 16-bit buffer and 33 ms into a 32-bit one. At 30 frames a second that is core 3
  two-thirds to wholly full, and its disk, network and remote work
  would get what is left between bands. Core 1 (USB, sound) and core 2 (the SE's bus, its screen and
  speaker) do not run the conversion, but they share the memory bus with it. So the conversion now keeps
  to half of core 3: the bands' own time is measured, and the next frame starts no sooner than that long
  after the last one ended (`FRAME_SHARE`, psmac_display.c). A frame that takes less than 1/60 s
  changes nothing (30 a second, as before: 1080p and below); 1920 by 1200 in millions, about 1/60 s,
  may come out at 25 to 30; at 2560 by 1440 the picture is redrawn as often as half the core
  allows, by the same scaling about 22 times a second into the 16-bit buffer and 15 into a 32-bit one,
  on a monitor that shows 30. The loops for the common pairs
  (millions into the 16-bit buffer, or into a 32-bit one; thousands into 16 bits) lost their per-pixel
  choices, which is worth more than the pacing. The heartbeat's display line now says how many frames
  were drawn in five seconds and how long the longest took on core 3: the number the testbed cannot
  give, and the one to send back from the real Pi.
- *The remote's screenshot* was cropped at 2048 pixels wide; now 2560.
- *Aurora68 Screen Setup and the driver* had no size of their own: Aurora68 Screen Setup takes each screen's rectangle from its
  GDevice, the driver its rows and pitch from the display block. 512 + 2560 = 3072 fits QuickDraw's
  coordinates easily.
- *Aurora68 Depth* set only the main screen's depth, which on the SE is the one-bit screen. "Aurora68 Depth HDMI 16"
  (any depth at the end) now sets the first active screen that is not the main one, so the HDMI screen's
  depth can be chosen without Monitors on a 9-inch screen.

## Any size, and declaration ROMs from the card (20 November 2026, evening)
The user then asked for a card that follows config.txt: no container rebuilt for a new monitor, and a
new Mac-side driver by copying a file.

**One two-screen file for every size.** Until now the screen's size was built into the declaration
ROM: each mode's VPBlock (bounds, rowBytes, the vpBaseOffset of its place) is read by the Slot Manager
and the ROM's screen set-up, which never ask the driver (the third trap above). So there was one file
per size, and a container to rebuild for each monitor. Now the kernel rewrites the video sResource in
Pi RAM before the 68k starts (`declrom_resize`, psmac_bus.c): the HDMI card's ($80 of slot $A) for the
HDMI size, a one-screen card's ($84 of slot 9) for its screen. Each mode gets the screen's bounds, its
row pitch and the vpBaseOffset of the place the display block gives it (`psmac_display_mode_base`, the
one function both use); a depth that fits nowhere at that size (a frame larger than the big region can
be, or rows wider than $3FFE) leaves the mode list, the later entries moving up as when the wired
Ethernet card is left out; then the ROM's CRC again. Rewritten from a 1024 by 768 file, the slot $A ROMs
for 640x480, 1366x768, 1920x1200, 2048x1152 and 2560x1440 come out byte for byte the same as the files
mkdeclrom.py builds for those sizes (a host harness compiling the kernel's functions, docs/08 item 77). The size is `mac.screen=` if given, else the firmware's: config.txt's `hdmi_mode` or
`hdmi_cvt` alone chooses it. `psmac-se.declrom` (`aurora68-se.declrom` in the release) is the one file
a container needs; the sized ones stay, and an exact size is still preferred, so a container made for
one size does what it did. Rows are now padded to a long (`psmb_rowbytes`): QuickDraw needs an even
rowBytes and 1366 at one bit is 171 bytes; every width that is a multiple of 32, which every size before
today was, comes out as before, and the existing files did not change by a byte. The conversion's 1-,
2- and 4-bit loops stop at the screen's last pixel instead of the byte's, which a width like 1366 needs.

**Declaration ROMs from the card's root.** `*.declrom` files in the card's FAT root are offered beside
the container's, by the same rules, and win a tie. The Slot Manager reads slot 9 within the first second,
long before core 3 takes over the card, so this one look is made on core 0 in psmac_setup_memory,
before psmac_alice_init starts core 3: the card is initialised and mounted there, the root listed, and
only the chosen file read; card_start later takes over the mounted card rather than resetting it again.
Only when the Pi booted from a container (on hardware always; a testbed run from a bare ROM would wait
1.5 s for a card that is not there); `mac.cardrom=off` leaves the card's files out. The log names the file
and where it came from: `[PSMAC] declaration ROM: aurora68-se.declrom from the card's root taken (the
HDMI screen 2560x1440: the file's screen is rewritten for it)`. A release's card root holds every
declaration ROM of the release, among them `aurora68-wifionly.declrom`, which has no screen; a chosen
file is looked into, and one without a video sResource is taken only if nothing else is left.

**What the testbed showed** (21 November, after midnight). QEMU's VideoCore model starts its frame buffer at
any size (`-global bcm2835-fb.xres=W -global bcm2835-fb.yres=H`, now passed by `QEMU_EXTRA` in
tools/run-psmac-qemu.sh), which is what config.txt's HDMI mode does on a Pi, so these runs had no
`mac.screen=` at all. MAME's `macse_ps`, System 7.1 on the SE's SCSI disk with Aurora68 Screen Setup, Aurora68 Colour in Startup
Items and copies of Aurora68 Depth on the desktop; a private copy of MAME's parameter RAM.
- *2560 by 1440, the ROM from the card.* The container held the IIci ROM and `psmac-se-1024x768.declrom`;
  the card image's root `aurora68-se.declrom`. The log: `screen: 2560x1440, the firmware's HDMI mode`,
  `the big frame buffer region is 15 MiB`, `[CARD] looked for declaration ROMs in the card's root (68 ms)`,
  `declaration ROM: aurora68-se.declrom from the card's root taken (the HDMI screen 2560x1440: the file's
  screen is rewritten for it)`, `slot $A's screen rewritten for 2560x1440, 6 depths`, `VideoCore read back:
  physical 2560x1440, ..., depth 16, pitch 5120, ...: as asked`, `two screens: ... (15 MiB); VideoCore
  2560x1440 x16 pitch 5120 ...: the firmware's own buffer, kept`. Aurora68 Screen Setup saw the new size, rewrote the
  arrangement and restarted; the second start reached the Finder with both screens active, the HDMI one
  at (512,0)-(3072,1440) in millions (`/state`), the Finder's desktop and Aurora68 Colour on it
  (`docs/img/se-hdmi-2560x1440-millions-picolor.png`). "Aurora68 Depth HDMI N", double-clicked on the SE's
  screen through the remote control, took the HDMI screen to 8, 4, 2, 1, 16 and 32 bits in turn, each
  within six seconds; 256 colours with Aurora68 Colour in front is `docs/img/se-hdmi-2560x1440-256-picolor.png`.
  QEMU's frame took 44 to 50 ms of core 3 (TCG on the host, not a Pi), and the pacing held it to 63-72
  frames in five seconds.
- *1366 by 768*, the same container and card: rewritten for 1366x768, rowBytes 172, 344, 684, 1368, 2732,
  5464 for the six depths (the heartbeat), 1 and 2 bits in the window at $F9988000 (24-bit reachable), the
  rest at $90000000; every depth reached; the Mac's own frame (`docs/img/se-hdmi-1366x768-thousands-picolor.png`,
  thousands) and the VideoCore buffer as the kernel wrote it (the remote's `--hdmi` picture) both right.
  QEMU's own screendump of that buffer comes out sheared, because its display takes a different stride
  for a 2732-byte row than the 2732 its mailbox reports; a Pi's firmware reports the pitch its scan-out
  uses, and the kernel writes by the reported pitch.
- *2048 by 1152*: rewritten, the Finder and Aurora68 Colour in millions (`docs/img/se-hdmi-2048x1152-millions-picolor.png`).
- *1024 by 768 with its own file and no card* (the regression): `psmac-se-1024x768.declrom from the
  container taken (the HDMI screen 1024x768, the file's own size)`, `kept at 1024x768`, the card's absence
  costing 1.55 s of the boot (`the card does not answer`); Aurora68 Colour in millions, 144 frames in five
  seconds, as before.
- *The solo board at 2560 by 1440* from the plain 640 by 480 `psmac.declrom` through semihosting:
  `slot $9's screen rewritten for 2560x1440 ... (the file described 640x480)`, a new 32-bit VideoCore
  buffer (`depth 32, pitch 10240: as asked`), plain "Aurora68 Depth N" through 1, 2, 4, 8, 16 and 32 bits, the
  Finder and Aurora68 Colour in millions (`docs/img/solo-2560x1440-millions.png`).
- *A release's card*: with `aurora68-wifionly.declrom` (no screen) listed before `aurora68-wifi.declrom`
  in the card's root and no declaration ROM in the container, the kernel took `aurora68-wifi.declrom`.

Not seen on hardware: the monitor's 30 Hz mode itself, the firmware's frame buffer at 2560 by 1440 within
`gpu_mem=64`, the conversion's real cost on a Pi 3 (the heartbeat's `VideoCore N frames in 5 s, longest U
us` line will say), and the early card look on the real sdhost controller (it is the code card_start
always ran, run earlier and on core 0).

## Aurora68 Display: the monitor, its resolution and rotation (21 November 2026)
The user's SE has had two screens since 20 November, its own and a Dell UP2716D on HDMI, and choosing
the HDMI mode meant taking the card out and typing `hdmi_` lines into config.txt on another computer,
after finding out what a Pi 3 can send. They asked for a Macintosh program: choose the resolution and
the rotation, read what the monitor says about itself, see which resolutions suit it. The design was
agreed with them before a line was written; this is what was built.

**The monitor's own description.** Every HDMI monitor carries an EDID: VESA E-EDID 1.3 or 1.4 in
128-byte blocks, the first with the maker's PNP ID, the product code, the serial number, the week and
year of manufacture, the picture's size in centimetres, the established and standard timings, four
18-byte descriptors (detailed timings, the first of them the preferred mode; the name, the serial
number as text, the range limits with the monitor's maximum pixel clock), and usually a CEA-861
extension block with the TV modes (short video descriptors, VICs), the sound (short audio descriptors,
the speaker allocation) and the HDMI vendor block (OUI 00-0C-03, without which the link is DVI and
silent). The Pi's firmware reads it over DDC at boot and gives it out through the mailbox's property
tag 0x00030020, "get EDID block": a block number in, the number, a status and 128 bytes out, the
status 0 while the block exists (`psmac_vc_edid_block`, support_rpi.c). The tag is in the firmware's
property interface since 2013, older than the Pi 3's legacy firmware; whether that firmware answers it
on the user's Pi is the first thing the hardware has to say (`[PSMAC:EDID] the firmware's EDID: N
blocks`). `psmac_edid.c` parses it, pure C that `tools/edid_test.c` compiles on the host (`make -C mac
check`): every mode once (a detailed timing and its VIC are one entry), each judged:
- *usable* when its pixel clock is within the Pi 3's. The figure is 162 MHz: Raspberry Pi's own
  documentation (the legacy config.txt options, under `hdmi_mode`) names 1920 by 1200 at 60 Hz with
  reduced blanking, 154 MHz, as the highest mode of every model before the Pi 4, and the firmware's own
  group 2 list has 1600 by 1200 at 60 Hz (DMT mode 51, 162 MHz), which these Pis are generally reported
  to send. `mac.hdmiclock=MHZ` moves the line. The monitor's own maximum (range limits) applies too.
- *too fast*, and then the same size with CVT reduced blanking at the mode's own refresh, else 50, else
  30 Hz, as long as the monitor's range limits go that low: the first that fits is offered. VESA CVT
  1.1's formulas, in integers (`edid_cvt_khz`), checked against VESA's own figures: 1920x1080 at 60 Hz
  173.00 MHz, 2560x1440 at 60 Hz reduced 241.50 MHz, 1920x1200 at 60 Hz reduced 154.00 MHz (DMT mode 69
  exactly). 2560 by 1440 at 30 Hz reduced is 119.00 MHz by the formula ("about 121 MHz" in "2560 by 1440" above was a
  rounder estimate).
- *the wrong shape* when its picture's aspect differs from the panel's (its size in millimetres, else its
  preferred mode) by more than 4%: a 4:3 or 5:4 mode on a 16:9 panel is stretched by the monitor.
- *not for the Mac*: interlaced, or outside the 512..4096 by 342..2560 the kernel takes.
The suggestion is the panel's own size if the Pi can send it at all (its highest usable refresh, else its
reduced-blanking alternative), else the largest usable mode of the panel's shape, with a one-line
reason. The kernel logs it all at boot, a line per mode (`[PSMAC:EDID]   2560x1440 at 59.95 Hz, 241.50
MHz (detailed timing): too fast, 30 Hz with reduced blanking fits (119.00 MHz), preferred`), and serves it
to the Mac as `struct psmb_dispinfo` (psmb.h) through the INFO service, `PSMB_INFO_DISPINFO`: 1,956
bytes copied into the Mac's buffer, laid out so that m68k GCC and the kernel agree (every long at a
multiple of four; asserted on both sides).

**Applying a resolution without touching config.txt.** config.txt is the user's and is never written.
They add one line to it, once, at its end: `include aurora68-display.txt`. The kernel writes only that
small file in the card's root (psmac_card.c, `psmac_card_put_file`): `hdmi_group` and `hdmi_mode` (a VIC
the firmware knows, group 1; a DMT number, group 2; else the custom mode 87 with `hdmi_timings` copied
from the monitor's own detailed timing, or `hdmi_cvt` for a reduced-blanking alternative),
`max_framebuffer_width` and `_height`, `hdmi_drive` (2 for an HDMI monitor: sound), and two comment lines
of its own that the firmware skips: `#aurora68 rotate=N` and `#aurora68 confirm=WxH@R`. The file before
goes to `aurora68-display.bak`. Both names are longer than 8.3 and the firmware's include looks the file up
by its long name, so a file that is not there yet is made with VFAT long-name entries (the name in 13-
character pieces, last first, each with the short name's checksum, then the short entry `AURORA~1.TXT`
with the first number nobody has), in one directory sector written at once; the text lives in the file's
first cluster and is read back. The firmware takes the last value of a setting it reads, which is why the
include goes at the end: an `hdmi_mode=` after it would win, and the kernel, which reads config.txt before
the 68k starts (on core 0, with the card's early look of "Any size" above), tells Aurora68 Display so
(`PSMB_CF_OVERRIDE`), as it does when the line is missing.

**The restart.** The firmware reads config.txt only when the Pi starts, so a new mode needs the whole Pi
restarted. Aurora68 Display asks the kernel to write the file (`PSMB_INFO_DISPSET`), waits until it is on the
card, arms the Pi's restart (`PSMB_INFO_DISPRESTART`) and restarts the Mac through the Shutdown Manager,
so that every program quits and every volume is put away first. The ROM's restart ends in the 68k's
RESET instruction, which reaches the kernel (`psmac_hdmi_bus_reset`); core 3 then waits until the card
has nothing left to write (the disk image's changes included: `psmac_card_idle`) and pulls the Pi's
watchdog, with the boot partition set to 0 in PM_RSTS, exactly as upstream PiStorm's housekeeper restarts
a Pi when the Amiga's reset is held (ps_classic_protocol.c). Is that safe for the SE's bus? It is the
classic PiStorm's ordinary way of restarting, and the SE is reset anyway: the kernel's start brings the
CPLD up and pulses the motherboard's RESET (psmac_gpio.c), as at power-on; while the Pi is down the 68000
socket is the CPLD's, which drives nothing until asked. The remote control's `restart?how=pi` (docs/13) has
done the same since 20 November.

**Keep this resolution?** After that restart the confirm mark is in the file. Nobody knows yet whether the
monitor shows the new mode, so the question is asked on the SE's own screen, which always works, and it
answers itself: Keep (the default button, Return) or Revert (Escape, Command-period), with "If you do
nothing, the previous resolution comes back in 15 seconds" counting down. Revert, or the countdown, puts
`aurora68-display.bak` back (without its mark) and restarts the Pi again. Aurora68 Screen Setup asks it: it runs at every
start of such a Mac anyway, and now, when the kernel says a mode waits to be kept, it stays in the system
heap (its entry copies the image there, as the WiFi menu's does, and gives the copy back otherwise), puts
a filter on GetNextEvent and, at the first null event of the first program two seconds on, puts up the
dialog (mac/pidisplay/pdcommon.c, shared with Aurora68 Display, which asks itself if it is started while the
question is still open). The kernel keeps its own clock as well: once the question is on the screen
(`PSMB_DC_ASKING`) it reverts by itself 25 seconds later without an answer, and if nothing asks at all (no
Aurora68 Screen Setup) it reverts `mac.displaywait=` seconds after its start, 300 by default.

**Rotation.** 0, 90, 180 and 270 degrees, the picture turned clockwise on the monitor: at 90 the Mac's top
row is the monitor's right-hand column, so a monitor stood on its side anticlockwise shows the Mac upright.
The kernel turns it while it copies the Mac's frame into the VideoCore's (`convert_turned`,
psmac_display.c), and the Mac's HDMI screen is then H by W: the screen size decided at start (psmac_bus.c)
is swapped, the declaration ROM rewritten for it, the display block made for it. The setting is the
`#aurora68 rotate=` line, so it outlasts a power cut, and it needs only a restart of the Mac: the kernel
keeps the HDMI card's declaration ROM as it was loaded, and at the 68k's RESET, before the Slot Manager
reads the cards again, rewrites it for the turned size (`psmac_declrom_rescreen`) and makes the display
block again (`psmac_display_rotate`); Aurora68 Screen Setup then sees the new size and arranges the screens once more. A
turn of 90 or 270 writes the Mac's rows as the monitor's columns, which caches dislike: a 1440-pixel row
lands one pixel on each of 1440 lines. So a band is done in tiles of 32 of the Mac's columns, each written
as one short run along a VideoCore row and flushed as such; millions of colours into the 16-bit or 32-bit
buffer have loops of their own.

**Aurora68 Display** (mac/pidisplay/pidisplay.c, an application in C with the kit in mac/c; 68020, System 6.0.8 and
7). One document window, written to the 1992 Human Interface Guidelines as the user asked: it moves, grows,
zooms to the screen it is on (as large as its contents want, on the screen holding most of it) and scrolls
both ways; on a narrow screen its sections stand in one column, from 720 pixels wide in two. A strip at the
top says what is in force or what has been chosen and carries the two buttons, **Revert** (back to the
settings in force) and **Restart to Apply...** (dimmed until something is chosen, and while the card cannot be
written). The sections: *Monitor* (name, maker, model, serial number, date, size in centimetres and inches
with the diagonal, shape, the EDID's version), *Current* (the HDMI mode and its refresh, the config.txt
lines that set it, the Mac's screen and its rotation, the depth the Mac draws in and the one the Pi sends,
the copy's time on core 3, and a note when config.txt lacks the include line, overrides it, or the card is
read-only), *Resolutions* (the list with its own scroll bar: size, refresh, clock and the judgement, the
monitor's preferred mode and the one in force marked, the suggested one in bold with a bullet and its reason
under the list, the modes that cannot be chosen dimmed; arrow keys move the choice), *Rotation* (four radio
buttons), *Sound* (the formats, rates and sample sizes the monitor plays, its speakers, and where the Mac's
sound goes now), *Connection* (HDMI or DVI, the Pi's, the monitor's and the HDMI link's pixel clocks, the
range limits, the colour depths). Menus: Apple (About Aurora68 Display...), File (Close, Save Report As..., Quit),
Edit (Copy copies the report as text; Undo, Cut, Paste, Clear and Select All are there, dimmed, and work for
desk accessories), Display (Use Suggested Resolution, Revert, Restart to Apply...). Balloon Help for all four
menus on System 7 ('hmnu' resources made by tools/mac/mkhmnu.py). Dialogs are built in memory, centred a
third of the way down the main screen, their default button outlined, Escape and Command-period cancelling.
The window's place and size are kept in "Aurora68 Display Prefs" (the Preferences folder through FindFolder on
System 7, the System Folder through SysEnvirons on System 6) and used only if its title bar is on a screen
that is there now. It takes every event, runs in the background, and refreshes its answer every two
seconds. Its lines in the kernel's log start `[MAC] A68 Display:`.

**What the testbed showed** (21 November, MAME's `macse_ps` with System 7.1 on the SE's SCSI disk, Aurora68 Screen Setup in the
Extensions folder and Aurora68 Display in Startup Items, QEMU's VideoCore started at 1920 by 1080 as config.txt's
`hdmi_mode=82` would, a 64 MB FAT16 card image with that config.txt and its include line, `mac.edidtest=sample
mac.hdmisim=1`; a private copy of MAME's parameter RAM):
- The boot logs the sample's 17 modes, 14 usable, 3 too fast (2560x1440 at 59.95 Hz with 30 Hz fitting at
  119.00 MHz; 1920x1200 at 60 Hz with its 154.00 MHz reduced-blanking twin; 3840x2160 at 30 Hz with nothing
  that fits), 8 of the wrong shape, the suggestion 2560x1440 at 30 Hz and the mode in use, 1920x1080 at 60 Hz
  from config.txt. Aurora68 Display opens on the SE's screen (`docs/img/pidisplay-se.png`); dragged onto the HDMI
  screen and zoomed it fills it as two columns (`docs/img/pidisplay-hdmi-1080.png`).
- 2560x1440 chosen, Restart to Apply, the alert (`docs/img/pidisplay-restart-alert.png`), Restart: the kernel
  made `aurora68-display.bak` (92 bytes, the empty first state) and `aurora68-display.txt` (515 bytes), both new
  with long names (mtools lists them as `aurora68-display.bak` and `.txt`, short names `AURORA~1.BAK` and
  `.TXT`), the Mac quit its programs and restarted, and the 68k's RESET restarted QEMU's Pi through the watchdog.
  The new start read the file, `mac.hdmisim` made a 2560x1440 buffer, Aurora68 Screen Setup rearranged the screens and
  restarted, and at the next start put up "Keep this resolution?" over the Finder, counting down
  (`docs/img/pidisplay-keep.png`). Left alone, it answered Revert at 0: the .bak went back, the Pi restarted, and
  the Mac came up at 1920x1080 again. A second time, Keep: the mark left the file and 2560x1440 at 30 Hz stayed
  (the kernel: `in use: 2560x1440 at 30 Hz (aurora68-display.txt, hdmi_group=2 hdmi_mode=87 hdmi_cvt=2560
  1440 30)`), with the log saved into aurora68.log before the restart.
- Rotation 90, then 270, each with a Mac restart only: `declaration ROM: slot $A's screen rewritten for
  1080x1920`, `rotation 90 from this restart: the Mac's HDMI screen is 1080x1920, the VideoCore 1920x1080`,
  Aurora68 Screen Setup's rearrangement, and Aurora68 Display on the turned screen (`docs/img/pidisplay-hdmi-rot90.png`, the
  VideoCore's own picture, the Mac's top along the monitor's right; `-rot270.png` along its left). Then
  2560x1440 again with 270 kept in the file: the Mac's screen 1440x2560 (`docs/img/pidisplay-hdmi-1440-rot270.png`).
- The copy's time on core 3 under QEMU (TCG on the host, not a Pi): 1920x1080 in millions into the 16-bit
  buffer unturned 17-24 ms a frame; turned 90, 90-100 ms; turned 270, 36-45 ms; 2560x1440 turned 270,
  80 ms (42-45 frames in five seconds with the pacing). The turned copy is the expensive one, as expected;
  the bands of a turned frame were then made twice as tall, halving the separate flushes, and 1080x1920
  turned 90 came down to 47-56 ms (68-74 frames in five seconds; the last run, on the kernel rebased onto
  main). But the next run, with the rotation read from the file at the start rather than set at a restart,
  measured 380-520 ms a frame for the same copy (6 or 7 frames in five seconds; the loop's "display" step
  60-95 ms a band), on the same host and the same code. Nothing in the copy differs between the two paths;
  QEMU's software TLB, walked by a turned copy one 4 KiB page per source row (4,320-byte rows), is the likely
  reason the figure jumps about. A Pi 3's 512-entry TLB over the kernel's block mappings will not see that,
  so QEMU's figures say only that the copy works; what a Pi 3 makes of it is the heartbeat's `turned N`
  line on the real machine.
- Aurora68 Display on the SE's own screen at its first place (`docs/img/pidisplay-se.png`), scrolled to its
  end with the Rotation buttons (`-se-bottom.png`), and made smaller with the grow box, the document then
  scrolling sideways too (`-se-resized.png`).
- Found on the way: the remote control's testbed channel (`mac.remotefile=`) loses its answers after a
  restart of QEMU's Pi (its requests still arrive, so clicks work; screenshots were taken with
  `tools/vcshot.py` and MAME's own pictures instead); and Aurora68 Display's first build hid and showed its radio
  buttons at every redraw, which, with a redraw every two seconds as the copy's time changed, left them
  invisible in some pictures: controls now move only when their place changes, and the copy's time alone
  no longer redraws the window.

Not verified on hardware: the firmware's answer to the EDID tag on the Pi 3's firmware and the UP2716D's real
EDID (the user's monitor already runs 2560x1440 at 30 Hz from hand-written config.txt lines, the mode
Aurora68 Display would write); whether a missing included file stops the firmware (the release ships an empty one);
the Pi's watchdog restart with the SE attached; the turned copy's cost on a Pi 3.

**A note of 22 November 2026: the same choice without the Mac.** Aurora68 Display needs a Mac that starts and a
screen to read it on. A monitor that shows nothing after a new mode is put right by "Keep this resolution?" timing
out, but a Mac that does not start at all, or a new monitor on a Pi whose Mac is away, had only config.txt on
another computer. Setup and Recovery (docs/08 item 106) now has a Display tab that does what Aurora68 Display does
from a phone (docs/08 item 111): the same EDID, read at the recovery start exactly as at any other (the 68k never
runs, but `psmac_display_init` and so `psmac_hdmi_init` do, on core 0, before core 0 parks), the same judgement
and suggestion, the same file through the same writer (`edid_display_text`, `.bak` kept), and the same rule for
what can be chosen (`edid_mode_choosable`, Aurora68 Display's `selectable`). Three things are new in psmac_edid.c,
each tested by tools/edid_test.c: **Automatic** writes `hdmi_group=0`, which the firmware's documentation names
"auto-detect from EDID", in place of a mode, so that it wins over an `hdmi_group` config.txt sets before the include
line (`#aurora68 confirm=auto`, without a size, since what the firmware takes is known only afterwards); **the
include line** can be put at the end of config.txt by the page (`edid_config_include`: one already there elsewhere
is moved, `[all]` goes before it when the last section is another, every other byte stays, config.bak keeps the
old file), where Aurora68 Display can only ask the user to do it; and **with no EDID** the standard 60 Hz DMT modes
and 2560 x 1440 are offered (`edid_standard_modes`), judged by the clock alone, with no suggestion. On `mac.screen=`
nothing new was decided: "Any size" above says it is needed only to differ from the firmware's mode, so when
cmdline.txt has one it follows the new size (and is taken out for Automatic), and when it has none none is added.

## Before the Mac's picture: the splash (23 November 2026)
From 17 November the VideoCore buffer showed the kernel's log until the Mac's video driver switched the display on
(the HDMI console, `psmac_display.c`): Emu68's own logo first, then the log's lines from the bottom row, wrapping.
It was the bench's window into a Pi with no serial cable; once the machine worked the owner called it noise and asked
for "a subtle splash screen on the HDMI monitor that has the Aurora logo and version" (docs/08 item 128). Now, by
default (`mac.console=splash`), the same span shows the Aurora68 logo on #0E0F12, the name in Topaz at twice the
size of the rest, the kernel's version string, a status line a tenth of the height from the bottom and, above it,
an amber line for what must be seen (`docs/img/hdmi-splash.png`). The layout is `psmac_splash.c`'s: the logo a
fifth of the screen's height (96 pixels at 640 by 480, 216 at 1080p, at most 256), scaled from the 192 by 192
picture `tools/mklogo.py` draws from `docs/img/aurora68-logo.svg` (a C header, checked by `make -C mac check`);
text in whole multiples of Topaz's pixels, one per 540 lines of screen.

When it is drawn: by `display_logo` (start_rpi64.c) into the firmware's 16-bit buffer in place of Emu68's logo; by
`psmac_display_init` again after the buffer is remade at the screen's size and depth; and from then on by the service
core (`psmac_display_poll`) alone, whenever a status or alert line changes, until the Mac's display flag is set, when
the picture conversion takes the buffer as before. A Restart clears that flag (`psmac_display_bus_reset`), so the
splash comes back ("Restarting the Macintosh...") until the driver opens again; with no declaration ROM the flag never
comes and the splash stays all the run. Setup and Recovery's screens and the bus check's report keep the buffer to
themselves as they did with the log. The splash is drawn upright: a PiDisplay rotation turns the Mac's picture only.

What the log does is unchanged except where it is drawn: `mac.console=boot` gives the old behaviour (Emu68's logo,
then the log until the picture), `mac.console=hdmi` (and `mac.hdmi=log` on the SE board) the log for the whole run,
and F12 on a USB keyboard changes between the splash and the log while the splash is what HDMI shows.

## The edge jump: from one screen to the other anywhere along the side (23 November 2026)
**What the owner asked.** The owner's SE has its own 512 by 342 screen at (-512,0)-(0,342), left of the
HDMI screen at (0,0)-(2560,1440), tops aligned in Monitors. "I have the small Mac screen and the large HDMI.
When I move the mouse to the small screen I need to precisely match its position in Monitors. Can we just have
it so moving to the side where the screen is moves the mouse regardless." The ROM moves the cursor between
screens only where they touch: here the top 342 rows of the HDMI screen's left edge, under a quarter of its
height. Below them that edge is a wall, and the way back lands at the same height, so the SE's screen was
reached only by aiming at that strip.

**What it does now.** Pushing the pointer against a side of a screen where another screen lies in the
Monitors arrangement (left, right, above or below, whether or not the two touch at that height) takes it
onto that screen at the same proportion along the edge: HDMI's left edge at y 1000 of 1440 is the SE's right
edge at 238 of 342, and the SE's right edge at y 100 is HDMI's left edge at 421. The rule is the same where the
screens do touch (the ROM makes that crossing and the point is then moved along the edge), so the whole edge
maps one way and the same way back, and how fast the mouse moves does not change where it lands. It lands 4
pixels inside the other screen's facing edge, and after a jump through a wall none follows for a sixth of a
second, so an overshooting hand or a jittering mouse does not throw it straight back. With three or more
screens the nearest one on that side is taken. Points the remote control places (`"display": N`, docs/13) and
points a program puts on another screen away from the edge are left where they were put. `mac.edgejump=off`
(Setup and Recovery's Options, Display) gives the ROM's behaviour back.

**Where it lives, and why there.** Every movement of the pointer passes through one routine: the ROM's cursor
task (IIci ROM $4082DF94), which the vertical-blank handler of the cursor's screen calls through the low-memory
vector JCrsrTask ($8EE, the call at $4080A33E). The ADB Manager's mouse handler, which serves the SE's own mouse
on the SE board, only adds its movement to MTemp ($828) and sets CrsrNew ($8CE) to CrsrCouple ($8CF)
($4080AB26); .PiInput puts USB and Bluetooth mice and the remote control's moves into MTemp and RawMouse
($82C) the same way. The task then moves RawMouse by the difference (accelerated), pins it to CrsrPin ($834,
the cursor's screen's rectangle) and, for a point past an edge, looks through the device list ($8A8) for an
active screen that holds it ($4082E14A): found, it sets CrsrDevice ($89C), CrsrPin and the cursor's depth for
that screen and moves there; not found, it pins the point at the edge pixel. So the jump is a cursor task in
front of the ROM's, in .PiInput (`crsr_hook`, mac/piinput.s), which is on every board from the boot screen on,
under System 6 and 7, with or without the Agent or Screen Setup. It looks twice. Before the ROM's task: MTemp
past an edge of the cursor's screen and on no screen is a push against a wall; if a screen lies on that side,
MTemp and RawMouse become the proportional point on it with CrsrNew = CrsrCouple, the documented way of setting
the mouse position, and the ROM's task, running next, moves the cursor there as for any crossing. After it: if
the ROM's task moved the cursor onto another screen beside the old one, within 64 pixels of the edge it came
over, the point is moved along that edge to the same proportion and the task run once more, so the cursor is
drawn there in the same tick. Nothing is done while CrsrBusy ($8CD) is set or the cursor is uncoupled, when
CrsrPin is not the cursor's screen (a program pinned the cursor to a rectangle of its own), or with one screen;
the screens are read from the device list every time, so a new Monitors arrangement or HDMI size applies at once.
The hook goes in from .PiInput's VBL task a second after the driver opens, and that task looks again every
quarter second: a vector replaced later without a call to ours gets ours in front again, and an inner call of
ours (when ours ends up in front of one that calls it) goes on to the task ours called before. The kernel's
part is the option: bit 0 of the input ring's `options` word (offset 28, `ALICE_IN_OPT_NOEDGE`, alice_psmac.h,
the long that was padding), read by the hook every time.

**What the testbed showed** (MAME `macse_ps`, 32 MiB, System 7.1 on the SE's SCSI disk with Aurora68 Screen
Setup, HDMI 1024 by 768: the SE's screen at (0,0)-(512,342), main, and HDMI at (512,0)-(1536,768), tops aligned, as
Screen Setup arranges them: the owner's arrangement, with the SE's screen main rather than HDMI). `mac.inputedge=SECONDS` (alice_psmac.c) is a scripted
USB mouse that walks to (0,100) on the SE's screen, right across the shared edge, down the HDMI screen to y 725, left
against its edge below the SE's screen, and back right; `mac.peek=828,200` showed MTemp, RawMouse, Mouse and CrsrPin
at each heartbeat. Four runs:
- the first boot found `"PiInput: edge jump in front of the cursor task at 4082DF94"` (the ROM's task, so nothing
  had patched it before), then Aurora68 Screen Setup's restart; in the second boot, from the Startup screen on, the
  three crossings were `+0520,+0100 -> +0520,+0225` (across the touching part at y 100: the ROM's crossing, moved
  to 100 x 768 / 342 = 224.6), `+0500,+0725 -> +0507,+0323` (the wall: HDMI's left edge at 725, onto the SE's
  screen at x 507, four pixels inside its last column, and y 725 x 342 / 768 = 322.9) and `+0527,+0323 -> +0527,+0725` (back:
  the same rows again); Mouse and CrsrPin agreed at each step (the SE's (0,0,342,512) while the pointer was there,
  HDMI's (0,512,768,1536) after);
- the same once more from a cold start, then the remote control's absolute moves (`aurora68.py --semihost input
  '{"type":"move","x":505,"y":100,"display":0}'`, then HDMI's (5,100), then the SE's (500,300)): each landed
  exactly where it was put ((505,100), (517,100), (500,300)), none moved along the edge;
- with the Finder running (it came at 345 s; the script at 530 s) the same three jumps, the hook still the one in
  front (one "in front of" line in the whole run: nothing in System 7.1 took the vector away), the pointer on HDMI at (8,225) of
  its own after the first (`docs/img/se-edge-jump-hdmi-across.png`, tools/vcshot.py) and on the SE's screen at its
  bottom right after the wall, with Aurora68 Log showing the jump's line (`docs/img/se-edge-jump-se-screen.png`,
  MAME's snapshot);
- `mac.edgejump=off`: the pointer crossed at y 100 to (520,100) as the ROM has it, went down to (520,600), stopped at
  HDMI's left edge, (512,600), however long it was pushed, and went back right; no jump was logged.

Not seen yet: the SE's own ADB mouse (MAME's SE has no mouse to move in these runs, so every push here was
.PiInput's; the ADB handler's path through MTemp and the ROM's acceleration is read from the ROM, not watched),
the owner's 2560 by 1440 arrangement with the HDMI screen main, System 6, and how it feels in the hand: the
dead time and the inset are guesses until then.

## Colour (17 November, 01:30)
Aurora68 Colour (`mac/picolor.s`) opens a colour window on the Pi's screen and paints twelve hues at full
and half strength, the primaries and a sixteen-step grey ramp with RGBForeColor and PaintRect.
Color QuickDraw matches each colour to the card's 8-bit table, the driver hands the table to the
Pi, the service core shows it: `docs/img/system71-two-screens.png` is the SE's screen and the Pi's
side by side, the Finder on the left and Aurora68 Colour's window on the right, 256 colours on a machine
that was built with 512 by 342 black-and-white pixels. (The first run came out transposed:
SetRect takes left, top, right, bottom, not a Rect's top, left, bottom, right.)

## Where it stood an hour earlier (17 November, 00:30)
Aurora68 Screens (`mac/piscreens.s`, `docs/img/system71-piscreens.png`) lists QuickDraw's GDevices: the
SE's own screen, active and main, and ours: 640 by 480, 1 bit, base $F9980000, driver -52, flags
$2000, which is screenDevice without screenActive, and a rectangle equal to the main screen's.
So the ROM's screen set-up ran the whole InitGDevice sequence on the card (SetMode, GrayPage,
SetGray, SetEntries, SetGamma, SetEntries, all traced in the driver's log) and created the device,
but nobody activated or placed it; the desktop is painted only on active devices, which is why
the Pi's screen shows the driver's own grey and not QuickDraw's. The System file has no 'scrn'
resource (Monitors' saved arrangement), so the default rule applies, and the default rule of this
ROM and System 7.1 evidently is not "activate every card". The next step is to find what
activates a second screen at boot on this ROM: the 'scrn' resource's format and a hand-made one
naming both screens, or the ROM's own default-placement code (docs/03's boot flow, the
InitGDevice/InitScreens pass after the Start Manager). Two side findings worth keeping: the ROM
addresses a card's frame buffer in the 32-bit form of its 24-bit megabyte ($F9980000 for slot
9's $980000), so the kernel now maps the window at $F9900000 as well as $F9000000; and this
System 7.1 runs in 24-bit mode (MMU32Bit 0), which caps usable RAM at 8 MiB whatever is installed,
so "more than 8 MiB" also needs 32-bit addressing switched on in the Memory control panel (a
PRAM bit the testbed's saved PRAM does not have yet).

## Order of work
1. 1-bit and 8-bit modes at 640 by 480 (the IIci's own sizes), palette through the mailbox,
   the frame buffer mapped at the slot's super space. (Done as described in "What exists":
   QEMU does emulate the VideoCore frame buffer, so the testbed shows the second screen.)
2. 16/32-bit modes, more resolutions, gamma, the Monitors control panel.
3. Vertical blank through the slot interrupt.

## What it buys
A colour desktop beside the SE's own screen, with the SE's display still the "main" screen or
either chosen as main in Monitors. It is the same mechanism the Alice68 kernel uses for its HDMI
output, so the HVS and mode-setting code is shared, and it needs no change to the ROM beyond the
declaration ROM the project already serves.
