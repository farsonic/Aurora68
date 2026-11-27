<!-- SPDX-License-Identifier: MPL-2.0 -->
# 24. A network card for A/UX: a DP8390 that exists only in the Pi

*27 November 2026.* A/UX 3.1.1 reaches its Finder on the owner's SE (docs/23, sections 21 and 22). The owner's
next wish after the keyboard: the network, over the Pi's WiFi. This is the record of how A/UX finds a network
card, what its driver was seen to need, the card that was built for it, and what the testbed showed. Where
something is our reading and not an observation it says *inference*.

## 1. Why the Pi's own cards are no use to A/UX

Under Mac OS the Pi's network is two `.ENET` cards in slot 9 (docs/07, docs/09 "Two network cards on one NuBus
card"): their drivers are 68k code in the declaration ROM (`mac/enet.s`), which the Slot Manager installs and
MacTCP or AppleTalk opens, and the frames cross a ring in the card's window (`alice_net.h`). A/UX opens no
Mac OS slot driver. Its kernel is Unix with its own drivers, linked in when the machine has the hardware, and at
the switch to A/UX's page tables this kernel even drops the network board (`psmac_card_handoff`, docs/23
section 16), because nothing in A/UX would ever clear its interrupt.

What A/UX has for Ethernet is on its disk (the owner's image, the Apple Workgroup Server 95 edition):

| File | What it says |
|---|---|
| `/etc/master.d/ae6` | `id 8`, then `anvs 1 ae6 - 1 1`: a driver named ae6 for boards whose ID is 8 |
| `/etc/master.d/as`, `ao` | `id 0x26`; `id 43`, `51`, `54`, `59`: two more Ethernet drivers, for other boards |
| `/etc/boot.d/ae6` | the driver's object module (COFF, 12 KB, with its symbols) |
| `/etc/startup.d/ae6` | a script, the same file as `as` and `ao` by hard link: it sets the interface's address from `/etc/NETADDRS` at each start, and with `-i` asks for it |
| `/etc/master`, `/etc/startup` | what the running kernel was linked with: `ao`, no `ae6`; `/etc/startup` runs `bnet` and `elap` only |

Board ID 8 is Apple's first NuBus Ethernet card of 1987, built around National Semiconductor's DP8390 with
buffer memory the Mac reads and writes directly. So the plan was a card of that shape that exists only in the
Pi: our own declaration ROM with board ID 8, the chip's registers and the buffer where ae6 looks for them, a slot
interrupt, and the Pi's network board behind it.

## 2. How A/UX decides what goes into its kernel

Read from the scripts and, where they call programs, from the programs (disassembled in the scratch area; none
of it is in the repository).

- **`/etc/autoconfig`** builds a kernel from the modules in `/etc/boot.d` whose `/etc/master.d` file either
  says `include` or names a board ID that is present. To learn the boards it reads a table from the running
  kernel through `/dev/kmem` and then, for slots 9 to 13 ($9 to $D), asks itself: the system call that hands a
  request to the Slot Manager (the Mac ROM's own, which the A/UX kernel calls: `slotmanager`,
  `callRealSlotManager` in `/unix`), `SNextTypeSRsrc` for the slot's board sResource (category 1, type 0) and
  `SReadWord` of its entry 32, the board ID. For each ID it then goes through every module's `id` lines; a
  match marks the module as having a board, with the slot, and the module is linked in. An ID nobody claims is
  a warning. Slot $E is in the table but not in that loop (the loop ends before 14), so whether a card there is
  seen depends on what the kernel's table says; the card below therefore prefers slot $D.
- **`/etc/sysinitrc`** runs `autoconfig -t0 -a -o /unix -S /etc/startup -M /etc/master` at every start, then
  `/etc/startup`, the script autoconfig wrote, which calls each linked driver's `/etc/startup.d` script.
- **`/etc/newconfig [-v] [modules]`** is what a person runs: it saves `/etc/inittab`, `passwd` and `group`,
  calls `newunix` with the module names (each runs its `/etc/install.d` script; none is needed for ae6, whose
  files are already in `/etc/boot.d`, `master.d` and `startup.d`), then `autoconfig -u -v -I` into temporary
  files, `kconfig` to keep the kernel's tuned parameters, the new startup script with `-i` (the drivers'
  questions), and only then moves the kernel to `/unix` and the scripts to `/etc/startup` and `/etc/master`.
  `newconfig bnet` would in addition switch on portmap, routed, sendmail, inetd and syslogd in `/etc/inittab`
  and ask about NIS; plain `newconfig` leaves `/etc/inittab` alone.

## 3. What ae6 expects of the card

From the module's code (its symbols name the functions: `ae6_probe`, `ae6_map`, `ae6_init`, `size_ram`,
`slot_ether_addr`, `ae6int`, `ae6_rpkt`, `ae6_put`).

- **Where.** With the slot number s, the chip is at `$Fs0E0000`, the buffer memory at `$Fs0D0000` and the ROM at
  `$Fs0F0000` (kept in the driver's record, not read by it). The DP8390's register n is one byte at
  `$Fs0E0000 + $3C - 4n`: the registers run backwards, four bytes apart, on the high byte lane. The probe is one
  guarded read of the command register's address.
- **The address.** `slot_ether_addr` asks the Slot Manager: `SNextTypeSRsrc` for category 4 (network), type 1
  (Ethernet) with the driver fields masked out, then `SFindStruct` for entry `$80` of that sResource and
  `SReadStruct` of six bytes. `/etc/etheraddr`, which the startup script uses to see whether a card is there,
  takes the same road for a board ID 8.
- **The memory's size.** `size_ram` writes `$1234` at the buffer's start and looks for it 16 KiB and 32 KiB
  further on: found, the buffer is 16 KiB and the receive ring ends at page `$3F`; not found, the ring ends at
  page `$FE`, a buffer of 64 KiB. The transmit buffer is page 0, the ring starts at page 6.
- **The chip.** Stop (`$21`), wait for the reset flag in the interrupt status register, data configuration
  `$49`, receive configuration `$0C` (broadcasts and the multicast filter), loopback while it sets PSTART 6,
  PSTOP, BNRY, CURR 6, the six address bytes and the eight filter bytes on page 1, interrupt mask `$1F`, start
  (`$22`), loopback off. It selects a register page by writing the command register with neither the start nor
  the stop bit (`$40`, `$00`): the chip must stay as it was.
- **A frame out.** Copied to page 0 in words, the length into TBCR, TPSR 0, command `$26`; the transmit flag
  of the status register ends its watchdog.
- **A frame in.** The driver reads the ring through the buffer memory: at each packet's first page the status
  byte, the next page, the length low and high (as the 68k reads the bytes, in that order), then the frame;
  it wraps from the ring's end to page 6, and keeps the boundary register one page behind what it has read.
- **The interrupt.** A/UX's level-2 handler reads the RBV's slot register (`$1E02`), calls the handler of each
  slot whose bit is low, and reads again until none is; ae6's handler reads the status register once, writes
  back the flags it serves and returns. A slot that asks with no flag would be logged as a stray interrupt, and
  one whose flags stay up would be called for ever.

## 4. The card

`mac.auxnet=on` (new; off by default) puts it in slot $D, or $E when another of the Pi's cards has $D; a letter
(`a`, `c`, `d`, `e`) names the slot. It is for the SE, Plus and solo boards, whose RBV this kernel answers
itself; on the testbed's IIci board the slot register is the motherboard's and the card is left out.

- **The ROM** (`tools/mac/mkauxnetdeclrom.py`, compiled in as `psmac_etalk_declrom.h`, 204 bytes): a board
  sResource "Aurora68 Ethernet for A/UX" with board ID 8, and a network sResource, Ethernet, whose entry `$80`
  leads to the address. It carries no driver. The address is not inside the image: the entry points eight bytes
  below the image's first byte, outside the length the format block declares and so outside its checksum. The
  kernel writes the address there when the network board has one, which on a Pi is seconds after the Mac
  started (the WiFi chip gives its address when its firmware runs), and the checksum the Slot Manager verified
  stays true. The 1987 card had the same arrangement, by the account of the Shoebill emulator's author, whose
  own ROM for A/UX does the same; nothing of either ROM is used here.
- **The memory.** 64 KiB at `$Fs0D0000` and the ROM's page at the slot's top are Pi memory mapped for the 68k
  (`psmac_mmu.c` asks `psmac_etalk_phys`), so the driver's copies run without a trap. There is no alias inside
  the 64 KiB, so ae6 takes the large ring: 249 pages, room for some forty full frames.
- **The chip** (`psmac_etalk.c`): the registers of pages 0, 1 and 2 as ae6 uses them, answered on the 68k's
  core from the access fault path (`psmac_io_read`, `psmac_io_write`). No remote DMA, no collisions, no
  counters; a frame is "sent" as soon as it is in the network board's ring.
- **The interrupt.** The card's bit in the RBV's slot register is low exactly while a flag of the status
  register is set that the mask register enables, and the RBV's slot flag follows it (`psmac_board_se.c`,
  `psmac_etalk_slot_irq`). The 68k's RESET and the hand-over to A/UX's tables reset the chip, as a card's /RESET
  would: stopped, mask clear, no interrupt until a driver starts it.
- **The network.** The card uses a unit of the network board exactly as `mac/enet.s` does, from inside the
  kernel: START, RXON and the filter through the unit's command box, frames taken from its receive ring and put
  into its transmit ring. Nothing in `alice_net.c` changed, so the WiFi, the USB Ethernet and the testbed's file
  bridge all serve it. The unit is the wired port when its link is up, else the WiFi; its address is the one in
  the ROM's cell, and a driver that starts the chip is bound to the unit whose address it programmed. Frames
  that do not fit the ring's free pages wait in the board's ring; the chip never reports an overflow.
  The board's header is left without a Mac driver's version, so slot 9's line stays down under A/UX.

Not done: the IIci board; AppleTalk over the card was not tried (the multicast filter is implemented, the
driver's multicast calls were not exercised); the tally counters read 0.
