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
the edge, half on each monitor). The values the 'scrn' needs come from PiScreens: the IIci's
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
PiScreens showed the device active at 32 bits but based at $F9980000, the window, while the Pi
converted the big region. InitGDevice never asks the driver where a mode's pixels are (no
GetPageBase in the trace); it adds the mode's `vpBaseOffset` from the sResource to dCtlDevBase.
mkdeclrom.py now writes that offset by the kernel's placement rule, and `docs/img/
system71-pi-1024x768-millions.png` is the result: the Pi's screen at 1024 by 768 in millions of
colours (`mac.screen=1024x768`, `psmac-1024x768.declrom`, a 'scrn' with mode 133 and the
rectangle 0,640,768,1664), PiColor drawn by Color QuickDraw, the Finder's dither pattern behind
it, 3 MiB of frame buffer that cost the Macintosh nothing. Thousands of colours at 640 by 480
(mode 132, fs165) draws the same window with the 16-bit pixels widened.

## Colour (17 November, 01:30)
PiColor (`mac/picolor.s`) opens a colour window on the Pi's screen and paints twelve hues at full
and half strength, the primaries and a sixteen-step grey ramp with RGBForeColor and PaintRect.
Color QuickDraw matches each colour to the card's 8-bit table, the driver hands the table to the
Pi, the service core shows it: `docs/img/system71-two-screens.png` is the SE's screen and the Pi's
side by side, the Finder on the left and PiColor's window on the right, 256 colours on a machine
that was built with 512 by 342 black-and-white pixels. (The first run came out transposed:
SetRect takes left, top, right, bottom, not a Rect's top, left, bottom, right.)

## Where it stood an hour earlier (17 November, 00:30)
PiScreens (`mac/piscreens.s`, `docs/img/system71-piscreens.png`) lists QuickDraw's GDevices: the
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
