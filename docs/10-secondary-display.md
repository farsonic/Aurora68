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
