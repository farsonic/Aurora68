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
  `/etc/startup`, the script autoconfig wrote, which calls each linked driver's `/etc/startup.d` script. That
  start-up call does not link a driver for a card that is new: in run 3 (section 5), with the card in the slot
  and a kernel without ae6, it came back with status 0 in half a minute and A/UX went on as before. (Its code can
  write `/nextunix`, put up "The kernel was inconsistent with the hardware configuration. The system will be
  rebooted to use a reconfigured kernel." and restart; when it does that was not found out.)
- **`/etc/newconfig [-v] [modules]`** is what a person runs: it saves `/etc/inittab`, `passwd` and `group`,
  calls `newunix` with the module names (each runs its `/etc/install.d` script; none is needed for ae6, whose
  files are already in `/etc/boot.d`, `master.d` and `startup.d`), then `autoconfig -u -v -I` into temporary
  files (it links the base kernel `/etc/config.d/newunix` with the chosen modules of `/etc/boot.d`, then runs
  each module's `/etc/init.d` script), `kconfig` to keep the kernel's tuned parameters, the new startup script with `-i` (the drivers'
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

`mac.auxnet=on` (new; off by default) puts it in the first free slot of $D, $A, $C and $E; a letter (`a`, `c`,
`d`, `e`) names the slot. $D is free unless the kernel was built with the PC card and `pc=on` is given: then the
PC card keeps $D, where its ROM and driver expect it, and the network card takes $A, which on the SE is free
unless `mac.hdmi=screen` puts the HDMI screen's card there; $C is the video decoder's unless `mac.video=off`, $E
the Apple II card's with `a2=on`, and $E comes last because autoconfig's own loop does not ask about it (section
2). So both cards can be had at once, and a default start keeps the slot the owner's A/UX kernel was linked for.
A/UX's kernel records the card's slot: after a move, `newconfig` once more. It is for the SE, Plus and solo boards, whose RBV this kernel answers
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

### 4.1 Where the pieces are, and the log's lines

| Piece | Where |
|---|---|
| the card | `emu68/src/psmac/psmac_etalk.c`; its ROM `psmac_etalk_declrom.h`, made by `python3 tools/mac/mkauxnetdeclrom.py -o emu68/src/psmac/psmac_etalk_declrom.h` (`tools/mac/declrom.py` gained `below()`, an entry that points below the image) |
| memory map | `psmac_mmu.c`: `pi_phys` asks `psmac_etalk_phys`, `ro_phys` asks `psmac_etalk_ro` |
| register accesses, set-up | `psmac_bus.c`: `psmac_io_read` and `psmac_io_write` ask `psmac_etalk_access` first; `psmac_etalk_init` beside the video card's |
| interrupt | `psmac_board_se.c`: `rbv_recalc` and the slot register's read use `psmac_etalk_slot_irq` |
| service loop, resets | `alice_psmac.c`: `psmac_etalk_poll` after `alice_net_poll`; `psmac_etalk_reset` in `psmac_card_reset` and `psmac_card_handoff` |
| options | `mac.auxnet=on|off|a|c|d|e`, `mac.auxnettrace=N` (the first N register accesses by name); both in `psmac_diag.c`'s table and README's |
| host test | `tools/aux/etalk_test.sh`: the chip model driven as ae6 drives it, against a stand-in for the board (44,539 checks) |

The kernel log, all with `[AUXNET]`: at start `the A/UX network card in slot $D: board ID 8, a DP8390 at $fd0e0000,
64 KiB of buffer at $fd0d0000, ROM 204 bytes at the slot's top`; when the board has an address `Ethernet address
..., the network board's WiFi (or wired port), in the card's ROM`; when ae6 runs `the driver started the chip:
address ..., ring pages 06 to fe, receive configuration 0c; frames through the WiFi`, `the network board's WiFi
gives its frames to A/UX's driver now`, `the first frame from A/UX's driver went to the network board`, `the
first frame for A/UX's driver is in the card's ring`, and every 30 s while the counts move `N sent (N lost), N
received (N not for the driver, N waits for room); ISR .. IMR .., ring at .., boundary ..`. `the driver stopped
the chip` and `the chip is reset` say what they say. A line that begins `the driver's address ... is not one of
the network board's` means the ROM's cell and the driver disagree.

## 5. The emulator runs, and a fault that is not the card's

**Run 1** (27 November, the MAME SE, `mac.mmu=full mac.ram=64 mac.auxnet=on mac.auxnettrace=300`, a copy of the
image with the testbed's `/etc/sysinitrc` of section 7). The card's two start lines came (slot $D, and the
testbed bridge's address in the ROM). The Mac OS on A/UX's disk reached its Finder with the card in the slot,
A/UX Startup launched, the kernel logged "A/UX's tables on", A/UX's kernel booted and init started the Macintosh
environment (the menu bar and the watch cursor on the screen). At 13 minutes everything stood still, the log
ending in the middle of nothing. Nothing had touched the chip: no register trace line.

The log's last lines had not been sent. Read out of the kernel's console ring through QEMU's monitor, they were

```
[CRASH] System error 1 (bus error) called at PC 002f99fa in "A/UX Startup", A5 03f90cf4 (crash record 1 of at most 8)
[JIT:SYS] Unhandled page fault: opcode b8626800, read from 0xffffff9100ffffff
[JIT:SYS] Exception with vector 0200 on CPU2. ELR=..., ESR=0x0000000096000010, FAR=0xffffff9100ffffff
[PSMAC] bridge stall: waiting on op 1 size 1 addr 00effbfe ...
```

and a run of the evening before (26 November, test kernel 20, no card, `mac.usertrace` on) ends with the same
two first lines: the same PC, the same A5, the same address. Two things, neither the card's:

1. **A system error in A/UX's Macintosh environment.** The crash record (read from the kernel's memory) holds the
   bus error's frame on the Mac environment's stack: format `$B`, PC `$002FCE8E`, SSW `$0141` (a data read in
   user mode), fault address `$08F90007`, A0 the same. The code is the System file's (the copy in
   `/mac/sys/Startup System Folder/System`, found by searching the image for the handler's bytes): a Process
   Manager routine, the one after `ChangeIconicTitle` by its debugger names, which takes a handle from offset 102
   of a process's record and reads through it to set a menu item's text (`_SetItem`). The handle is `$08F90007`:
   not an address the process has. Why that record holds it is not known. *Inference:* a race in the start of the
   Macintosh environment that the emulator loses when the host is slow (both runs were made with another
   emulator-heavy job on the machine, and runs 52, 53, 60 and the others before and after reached the Finder with
   the same disk); it was not looked into further here.
2. **The kernel's own fault on top of it.** The crash recorder prints its block from the bus core, and reads the
   68k's memory for it through `mac_rd32` (`psmac_bus.c`), whose bound check was `ph + 4 > ram_size`: for a
   physical address in the last four bytes of the 4 GiB the sum wraps, the check passes, and the read is far
   outside the Pi's memory. The core that serves the Mac's bus cycles and sends the log died there, with its last
   lines unsent, and the 68k waited for it for ever. Fixed: `ph >= ram_size` is tested first. With that a system
   error in the Macintosh environment gets its block in the log and the machine goes on.

**Run 2** was withdrawn half a minute after its start (the coordinator's word from the owner's SE had arrived,
section 9, and the one emulator was wanted for the merged kernel).

**Run 3** (the merged kernel of section 10, full configuration, the same options and image as run 1). The same
system error came at the same place, eight times over (the Macintosh environment is started again and fails
again), each with its block in the log now: registers, the code at the PC, the bus error's frame on the stack.
The kernel went on, and so did A/UX's Unix side, with its text console on the screen where the start-up dialog
would have been. So the error is not chance: every run of this branch has it and the keyboard work's runs of the
same night do not. What this branch's runs have that those lack is the testbed's network (`mac.net=` and
`tools/pinet.py`, which makes the service core's turn longer, 10 to 200 ms in the heartbeat's "network" figure),
the card, and the changed `/etc/sysinitrc`; the earlier run that had it had none of the three but
`mac.usertrace`, which also slows the 68k. *Inference:* timing, not the card; the owner's SE, with the card,
starts its Finder. It should be run down before the emulator is trusted for A/UX's Macintosh side again.

What the run showed of the network, from the files A/UX wrote (section 7):

- `/auxnet.boot`: `boot Thu Nov 26 18:35:45 PST 2026`, `autoconfig 0`: the start-up autoconfig, as said in
  section 2.
- `/auxnet.newconfig`, newconfig's own words: "Opening kernel /etc/config.d/newunix", the nineteen master
  files, then "Searching for hardware devices", "Found board ID 20563 in slot 9" and "Found board ID 16726 in
  slot 12", each with "Warning cannot find driver(s)" (the Pi's main card, `$5053`, and the video decoder,
  `$4156`), and **"Found board ID 8 in slot 13 Version 0.0"** with no warning; the link, `ld ... /etc/boot.d/at_atp
  ddp elap llap adsp ae6 at_sig bnet devscsi line scn sin snd tc toolbox ufs`; "Setting up network devices",
  "Setting up slot vectors", "Closing the kernel and putting it in place", and the modules' `/etc/init.d`
  scripts one by one.

There the run's 55 minutes ended: 17 minutes to A/UX's system initialisation on that night's host, then half an
hour of newconfig, with the driver's questions, the restart and the test still to come. In the emulator,
therefore, the card is found by autoconfig and ae6 is linked against it; ae6 itself has not run there.

## 6. What a person does, once

With the kernel and `mac.auxnet=on` on the command line (and a WiFi network joined as before, or the cable in),
in A/UX's CommandShell as root. The questions are quoted from `/etc/newconfig`, `/etc/startup.d/ae6` and
`/etc/sethost`; section 8 says how far a run has shown them.

1. `/etc/etheraddr` prints the card's Ethernet address, the Pi's. Nothing printed: the Slot Manager does not
   show the card, and nothing below will work.
2. `cp /unix /unix.old`, then `newconfig -v`. It prints "Making backup copies...", "Building a new kernel (this
   may take several minutes)", autoconfig's lines (with `-v`: "Found board ID 8 in slot 13"), "Restoring kernel
   parameters", "Checking for any installation information", then asks:
   - ` 1 ae Ethernet card(s) installed`, `ae0: Would you like to configure this interface for IP? [y/n, default=y]`
   - `Please enter a host name (it must be unique):` (lower case letters, digits and dashes), then
     `Please enter a NIS domain name [none]:` (Return)
   - `ae0: Please enter an Internet address:` a free fixed address of the network, outside what its DHCP server
     hands out
   - `ae0: Please enter a netmask [none]:` for example 255.255.255.0
   - it shows the address, the netmask and the broadcast address it worked out, and asks `OK? [y/n, default=y]`
   and ends with "To avoid kernel/device inconsistancies, reboot immediately."
3. Restart A/UX. From then on `/etc/startup` runs ae6's script at every start, which sets `ae0` from
   `/etc/NETADDRS`. `ifconfig ae0`, `netstat -in`, `/usr/etc/ping HOST 56 3`, `telnet HOST`.

The address is kept in `/etc/NETADDRS` (one line: `ae0`, a tab, the address, the broadcast address, the
netmask), the name in `/etc/HOSTNAME` and both in `/etc/hosts`; edit them and restart to change it, or use
`ifconfig ae0 ADDRESS netmask MASK` for the session. Nothing sets a default route or a name server: `/usr/etc/route
add default ROUTER 1` (by hand, or as a line at the end of `/etc/sysinitrc`, below "Add local code below this
line"), and `/etc/resolv.conf`. `newconfig bnet` instead of plain `newconfig` also switches on inetd (telnet and
ftp into the SE), routed, sendmail, portmap and syslogd, and asks about NIS.

## 7. The testbed, from nothing

The A/UX image is the owner's and is not in the repository: ask for it again (`AUX.zip`, holding
`AUX_3_1_1GB.dsk`, 1,048,576,000 bytes; sector 96 starts the root file system). Keep one copy as it came, for
reading, and give A/UX another to write to; `conv=sparse` and `cp --sparse=always` keep each near 200 MB on the
host. What was read from it, all with `ufscat.py IMAGE 96 PATH`: `/etc/master.d/*`, `/etc/master`, `/etc/startup`,
`/etc/startup.d/ae6`, `/etc/sysinitrc`, `/etc/inittab`, `/etc/newconfig`, `/etc/newunix`, `/etc/install.d/bnet` and
`install`, `/etc/sethost` (scripts, read as they are); `/etc/boot.d/ae6` (taken apart with `tools/aux/coffdis.py`),
`/unix` (`coffsyms.py`, `unixdis.sh`: `slotmanager`, `slotintr`, `via2intr`, the slot tables in its data),
`/etc/autoconfig` and `/etc/etheraddr` (no symbols: objdump on their text, the strings' addresses as landmarks),
and the Mac environment's System file for section 5.

A/UX in the testbed has no keyboard, so what a person would type is put where A/UX runs it anyway.
`tools/aux/mknetimage.py` makes a `/etc/sysinitrc` that, at its end, runs `newconfig -v` once with the answers of
section 6 on its standard input and restarts A/UX, and on every later start tests the network in the background;
`tools/aux/ufspatch.py` writes it over the image's own. The tool allocates nothing: it rewrites the fragments the
file has and its length, which is why the new script drops the old one's comments (2,924 bytes become about
2,300, in room for 3,072). A/UX itself then creates the result files, which are read off the image afterwards:
`/auxnet.boot` (a line per start and autoconfig's results), `/auxnet.newconfig` (the transcript) and
`/auxnet.log` (etheraddr, ifconfig, netstat, ping, arp, telnet). A run that was stopped leaves the file system
unclean, and A/UX then wants to check it at the next start: begin each run from a fresh copy, or from a copy a
finished run left.

`tools/aux/run-aux-net.sh SECS LOGDIR KERNEL DISK mac.auxnet=on` is the run: the MAME SE (`tools/run-fullstack.sh`,
`MACHINE=macse_ps`), the image as a virtual SCSI disk at ID 0, `mac.mmu=full`, 64 MiB, `mac.net=/tmp/psmac-net`
and `tools/pinet.py /tmp/psmac-net peer` as the other end of the wire (192.168.68.1: it answers ARP, ping, TCP
echo on port 7 and HTTP, and logs every frame). A/UX needs 12 to 20 minutes to its system initialisation
there, newconfig more than half an hour, and the run with newconfig is two starts: give it two hours
(`SECS` 7200; run 3's 3,300 were not enough), or stop it when A/UX has restarted after newconfig (the log's
second "68k RESET" line; the file system is clean then) and start a second run from the image it left. When a run stands still, QEMU's monitor still answers (`tools/qemu-mon.py "info
registers -a"` for where each core is; `memsave` of `con_ring` for log lines that were never sent).

Without A/UX: `sh tools/aux/etalk_test.sh` (a second), and `tools/diag_test.sh` for the options table.

## 8. What is proven, and what is not (27 November 2026, after run 3 and the owner's test)

Proven:
- by reading the code: sections 2 and 3;
- by `tools/aux/etalk_test.sh`: the chip model against the driver's sequence as read, the ring's arithmetic, the
  interrupt's bit, the board's command box;
- in the emulator (runs 1 and 3): the Mac ROM's Slot Manager and the Mac OS on A/UX's disk start with the card
  in slot $D, A/UX Startup launches, A/UX's kernel boots; the address reaches the ROM's cell; autoconfig finds
  board ID 8 in slot 13 and links ae6; System 7.1 from the SE's disk starts with the card in the slot and
  without it (section 10);
- on the owner's SE (section 9): the address read by `/etc/etheraddr`, newconfig's kernel, `ae0` up over the
  WiFi, ARP and ping answered both ways, the slot interrupt.

Not proven yet:
- ae6 running in the emulator (run 3 ended before the restart), so the testbed has no regression for it but
  the host test;
- newconfig's questions word for word as section 6 lists them (they are the scripts' text; the owner answered
  them, no transcript came back);
- on the SE: the log's `[AUXNET]` lines, TCP, a long transfer, the wired port, the merged kernel;
- AppleTalk over the card; the card in a slot other than $D (with `pc=on`); the IIci board.

## 9. The owner's SE: ae0 over the WiFi (27 November 2026)

The kernel of section 4 as it stood after run 1 (test kernel 22: the card, the `mac_rd32` fix, none of the
keyboard work yet) on the owner's Macintosh SE, `mac.auxnet=on` added to the command line, the WiFi joined as
before, no Ethernet cable. The owner had a keyboard in CommandShell as root and did section 6 by hand; reported
through the coordinator, without the kernel's log so far:

- `/etc/etheraddr` printed the Pi's WiFi Ethernet address: the Mac ROM's Slot Manager shows the card, A/UX's
  system call reaches it, the network sResource and its entry `$80` are found and the six bytes are read from
  the cell below the ROM image, where the kernel had put the WiFi chip's address after the Mac had started.
- `cp /unix /unix.old`, `newconfig -v`: autoconfig built a kernel with ae6, the questions were answered, A/UX
  was restarted.
- After the restart `ae0` is up, and another machine on the same WiFi network pings A/UX: every echo answered,
  ttl 255, the first after about a second (the ARP exchange), then 30 ms falling to 4 ms.

So on hardware the whole chain holds, in both directions and with the interrupt: autoconfig's slot scan and its
`id 8` match, ae6's probe and start on this chip model, a broadcast ARP request received through the WiFi, the
reply and the echo replies sent, each received frame announced through slot $D's bit of the RBV and
acknowledged. Not looked at there yet: the `[AUXNET]` lines of the log, TCP (telnet, ftp), a long transfer, the
cable instead of the WiFi, a start with the keyboard kernel, and A/UX's address beside the kernel's own
(`remote=on`).

## 10. The merge with the keyboard work, and what comes next (27 November 2026)

The branch was merged with `mmu-experimental` as it stood with the virtual ADB keyboard and mouse (docs/08 item
151); the two meet in single lines of five files and nothing had to be chosen. The owner's kernels are the full
configuration (`-DPSMAC_PC=ON -DPSMAC_A2=ON` with their libraries), and there the PC card can ask for slot $D:
hence the slot order of section 4. On the merged tree: the kernel builds without a warning in both
configurations, `make -C mac check` passes with the chip model's test in it (44,539 checks), `tools/diag_test.sh`
7,689 checks, `tools/adb_test.sh` 86.

System 7.1 from the SE's SCSI disk in the MAME SE, the merged kernel in its full configuration, 520 s each: the
Finder's desktop and the image's startup program, once without the option and once with `mac.auxnet=on` (the
card's line in the log, slot $D), the last screens the same to the byte, no system error, no unserviceable slot
interrupt. (Two boots of 330 s before them had only got to "Welcome to Macintosh" and .PiInput's first
interrupt: that night's host ran the testbed at about 0.6 of its usual speed, docs/08 item 151.) The A/UX boot
with the merged kernel is run 3 of section 5.

Next, in order:

1. The system error of section 5 in the emulator's Macintosh environment: which of `mac.net=`, the card and the
   changed `/etc/sysinitrc` brings it (three short runs), then why.
2. A full emulator run of section 7's script (two hours, or in two parts), so that ae6 against the card is a
   test anyone can repeat: the three files and `pinet.log`.
3. From the owner's SE: the kernel's log with the `[AUXNET]` lines, telnet or ftp, a default route, and the
   merged kernel with the Pi's keyboard.
4. Then: the IIci board's slot register, AppleTalk over the card (`elap` is in the kernel), a default route and
   a resolver set by the same hand that sets the address.
