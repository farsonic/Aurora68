# Aurora68 / PiStorm-SE: an Emu68 kernel for the Macintosh SE

*This document is the project's front page and its history. The later numbered documents are the
working notes of each part, written as the work was done; this one is kept up to date and says
where everything came from and why it is the way it is.*

## The idea (16 November 2026)
Take a PiStorm (a Raspberry Pi in the 68000 socket of an old computer, with a small CPLD turning
the Pi's GPIO into a 68000 bus) and Emu68 (Michal Schulz's bare-metal AArch64 JIT that makes the Pi
behave as a fast 68k), both built for the Amiga, and put them into a Macintosh SE. The SE keeps
its motherboard, screen, keyboard, mouse, floppy and real NCR 5380 SCSI chip; the Pi replaces the
8 MHz 68000 with a 68040 running at Pi speed, adds memory, bootable disk images from the SD card,
networking (Ethernet, WiFi, Bluetooth) and, later, a second display from the Pi's own video output.

The user already runs a sibling of this on the Amiga: **Alice68**, a Pi 4 Amiga kernel built on
Emu68 with a virtual AGA chipset, whose spare ARM core drives the Pi's WiFi, Gigabit Ethernet,
Bluetooth and USB and hands them to AmigaOS through shared-memory "boards". Much of the Mac work
reuses it; where this project differs, the documents say so.

## The name
The Amiga kernel is named after Alice, the AGA chip. The Macintosh variant is **Aurora68**: Aurora
was Apple's code name for the Macintosh IIci, whose ROM this machine runs (see below), and the
name keeps the Alice68 pattern. The kernel binary is still called `kernel8.img` (the Pi firmware
insists); the Emu68 build variant and source directory are `psmac`, the working name from the first
day, and the project repository is `pistorm-se`. All three names refer to the same thing.

## Why the Macintosh IIci ROM runs on a Macintosh SE
The SE's own ROM is "24-bit dirty": it keeps flags in the top byte of pointers and can address at
most 4 MB, and it knows nothing of a Slot Manager, colour video or a 68030/68040's MMU. Making it
32-bit clean is a project the size of Connectix's MODE32. The first analysis (docs/01) showed the SE
ROM would boot a 68040 but could never give the machine more than 4 MB without a rewrite.

The IIci ROM (version 368CADFE, 512 KB) is 32-bit clean, drives the same NCR 5380 SCSI chip and the
same VIA/ADB keyboard interface as the SE, and carries a Slot Manager, a Start Manager that loads
drivers from NuBus cards, and a built-in video driver; Apple used it for the IIci, IIsi and later
machines. With it the Macintosh side of this project became a well-trodden path: every device the
Pi adds is presented as a NuBus card in a slot the SE does not have, with a declaration ROM the
Slot Manager finds at boot, exactly as a 1989 card would have been (docs/03, docs/09). The price
is that the ROM expects IIci hardware around it: RBV video, ASC sound, SWIM floppy, a 68030 MMU.
Each of those is either the real SE part behind a small translation on the Pi, or emulated by the
kernel (the MMU, docs/08). The decision was taken on the first day after the IIci ROM booted
System 6 and 7.1 in the emulated testbed.

## How it was built without the hardware: the testbed
No PiStorm was plugged into an SE while this was written. Instead the whole stack runs on a
development machine: Emu68 (the psmac build) runs under QEMU's Raspberry Pi 3 model, and the Mac
motherboard is MAME's IIci with its 68030 replaced by a *bridge* CPU device that serves bus cycles
over a FIFO to Emu68 (docs/04, docs/05, docs/06-mame-bridge). Every byte the Mac reads or writes
to VIA, SCC, SCSI or video goes through that bridge, so the JIT, the MMU shim, the ROM patches and
the Mac-side drivers were all exercised against a faithful motherboard before any PCB existed.
Boot takes about four minutes in the testbed; a Pi 4 will do it in seconds.

## Where the work stands (see docs/08 for the day-by-day account)
- The IIci ROM runs on the Emu68 JIT with the MAME motherboard; System 6 and System 7.1 boot to the
  Finder from the user's SCSI disk images.
- The Pi is a NuBus card in slot 9: a shared mailbox window and a declaration ROM served from Pi
  RAM. The Slot Manager accepts it, the Start Manager loads the Pi's block driver at boot, and a
  disk image on the Pi mounts on the desktop (docs/07, docs/09).
- The Alice68 service modules (network board, Bluetooth, status page) run on ARM core 3 behind
  that window; in the testbed the network board is fed from host files, on hardware from the
  Pi's chips. The card has a real interrupt: the kernel raises the IIci's slot-9 line and the
  drivers install handlers with the Slot Manager (docs/07).
- Under System 6 and System 7.1 the whole stack works end to end: the Pi disk mounts, the Apple-interface
  Ethernet driver (`.ENET`) sends an ARP request through the Pi and receives the reply through
  its slot interrupt, and keyboards and mice on the Pi move the Mac's cursor and post its events
  (docs/09, `docs/img/system6-pinet-arp.png`, `docs/img/system71-pinet-arp.png`). PiWiFi, PiBlue and PiStatus give the WiFi,
  Bluetooth and the Pi's status page a Macintosh face.
- MacTCP, Apple's TCP/IP, runs over the Pi's Ethernet under System 7.1: configured by a tool
  that writes its undocumented configuration resource, it pings the testbed's peer through our
  `.ENET` driver (`docs/img/system71-mactcp-ping.png`). The WiFi unit is the same driver as a
  second card.
- The Mac calls itself a Macintosh SE: a small extension replaces the Gestalt Manager's machine
  answer, while the ROM keeps its IIci view of the hardware (docs/08, item 19).
- The WiFi path is tested in the testbed against a simulated radio on the service core: PiWiFi
  scans and joins it, and MacTCP pings through the WiFi driver (docs/09). The chip itself waits
  for hardware.
- A second display through the VideoCore is designed (docs/10).

## The documents
| | |
|---|---|
| docs/01 | The SE ROM, read with a disassembler: why it cannot be the 32-bit ROM |
| docs/02 | Prior art: PiStorm, Emu68, Frimples' SE socket adapter, MODE32, ROMinator |
| docs/03 | The IIci ROM as donor: its memory map, MMU use, slot probing, the timing calibration |
| docs/04 | The emulation testbed: QEMU and MAME, how the two are joined |
| docs/05 | The bridge protocol between Emu68 and the MAME motherboard |
| docs/06-mame-bridge | The MAME side: the bridge CPU device and the IIci machine built on it |
| docs/06-rom-patches | ROM patches: checksum, timing calibration, why saturation is not enough |
| docs/07 | The service core and the mailbox window; the Alice68 port plan |
| docs/08 | Emu68 port status: every bug found and fixed, in order |
| docs/09 | The Mac side: drivers, declaration ROM, tools, test application |
| docs/10 | The second display from the VideoCore |

## Hardware plan (unchanged from day one, decision pending)
The classic PiStorm board with a Pi Zero 2 W or 3A+ fits the SE's socket (Frimples'
`MacSE-PiStorm-DirectSocket` CPLD design adds the bus arbitration the Mac needs); it offers WiFi and
Bluetooth only. Gigabit Ethernet and the Pi 4-class drivers in Alice68 need a CM4 carrier in the
PDS slot, which also puts an RJ45 at the SE's rear cut-out. The software is written for both; the
network and USB drivers taken from Alice68 are Pi 4 code and the differences for a Pi 3-class board
are listed in docs/07.
