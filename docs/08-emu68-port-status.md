# Emu68 "psmac" port: status (2026-11-16, end of first session)

## What exists
Fork: `emu68/` (branch `psmac`, MPL-2.0), build dir `psmac-build/`, image `psmac-build/Emu68.img`
(to be renamed `kernel8.img` for SD cards; the `initramfs` next to it is the Mac ROM or, for
hardware, the cpio container from `tools/mkcontainer.py` with the ROM, declaration ROM and
firmware, docs/07). Build:
```
cmake -S emu68 -B psmac-build -DCMAKE_TOOLCHAIN_FILE=emu68/toolchains/aarch64-linux-gnu-13.cmake \
      -DTARGET=raspi64 -DVARIANT=psmac -DCMAKE_BUILD_TYPE=Release
cmake --build psmac-build -j3 && gunzip -kf psmac-build/Emu68.img.gz
```
Host prerequisites that had to be added: `gcc-aarch64-linux-gnu`, `gcc-powerpc-linux-gnu` (Emu68's
PPC ROM sub-build), `device-tree-compiler`, `xxd`, `qemu-system-arm`, and a stub
`/usr/aarch64-linux-gnu/include/gnu/stubs-lp64_be.h` (Emu68 is big-endian AArch64; the Linux
cross toolchain has no BE multilib headers; only headers are needed, the link is -nostdlib).

Full-stack testbed: `tools/run-fullstack.sh SECONDS` starts MAME `maciici_ps` (the IIci motherboard
with the bridge CPU, `reference/mame/mamepsmac`, see docs/06) on a FIFO pair and QEMU `raspi3b`
running the psmac image with the IIci ROM as initrd (`tools/run-psmac-qemu.sh`). Logs land in
`$LOGDIR` (default /tmp/psmac-logs), MAME screenshots in `mame/snapFS/maciici_ps/`.
A QEMU monitor is on `/tmp/qemu-mon.sock` (`tools/qemu-mon.py "info registers"`), symbols via
`aarch64-linux-gnu-nm -n psmac-build/Emu68.elf`.

## Architecture as implemented (`emu68/src/psmac/`)
- `psmac_bus.c`: Mac RAM (`mac.ram=MB`, default 8) and the 512 KiB ROM are carved from Pi RAM;
  RAM is mapped at 68k address 0 (plus its bank A aliases up to 64 MiB), ROM at 0x40800000 and
  0x40000000. Everything else the JIT touches faults into Emu68's abort handler, which calls
  `psmac_bus_read/write`: physical frame-buffer pages (0 to 0x50000) are write-through
  (local copy + posted write to the motherboard), all other non-local addresses go to the bridge.
  Transport: the Pi mini-UART (QEMU `-serial` #1) or, default in the testbed, **semihosting over
  two FIFOs** (`mac.bridge=semihost`, QEMU `-semihosting-config enable=on,target=native`): 11 µs
  round trip versus ~1 ms through the UART model. The service core (Pi core 2, Emu68's
  housekeeping core) owns the transport; core 0 hands it requests through a shared slot; it also
  polls the interrupt level at 1 kHz and prints a heartbeat every 5 s with per-device counters,
  68k PC/SR, registers and stack top.
- `psmac_mmu.c`: 68030 MMU shadowing. PMOVE to TC/CRP/SRP/TT0/TT1/MMUSR is trapped (F-line
  emitter in `M68k_LINEF.c` calls `psmac_pmove`); PFLUSH/PLOAD/PTEST are skipped **including
  their EA extension words**. On TC/CRP load the 68030 tables are walked once (short/long
  descriptors, early termination, limits, IS aliases) into a 4 KiB-page logical→physical table,
  the previous ARM mapping is removed and a new one applied (runs coalesced; ROM/WP/frame-buffer
  pages read-only). The abort handler translates logical faults with the same table.
- Options come from `/chosen` bootargs (this Emu68 branch does not parse the cmdline: its
  `parse_cmdline` is `#if 0`).
- Pi peripherals (0xF2000000..) are mapped EL1-only in this fork; Emu68's own EL0 helpers that
  fault there are served by the abort handler (origin decided by ELR in Emu68 text vs JIT code);
  Mac accesses to that window are forwarded to the bridge like any slot space.
- MAME side changes made this session: immediate replies (the one-slice deferral cost ~1 ms per
  access), wall-clock credit consumed on every iteration (otherwise emulated time froze under
  load: SCC transmits and VIA timers never fired), FIFO transport mode (`PSMAC_BRIDGE_FIFO=req:rep`).

## Where the boot gets to (updated, second session)
**System 6 boots to the Finder** on Emu68's JIT with MAME as motherboard, from
`images/chd/hd20.chd` over MAME's NCR 5380 (`mame/snapFS/maciici_ps/0031.png`: menu bar,
"BlueSCSI Mac Plus" volume, Trash). The whole ROM boot runs unmodified: decoder detection,
VIA1/RBV/VDAC/ASC init, boot chime, RAM test, both MMU table sets, PRAM, SCSI boot, happy Mac,
"Welcome to Macintosh", System file patches, Finder. About 4 minutes of wall time in the
QEMU+MAME testbed (the JIT runs under TCG; a Pi 4 is far faster). `HD=images/chd/sys710p.chd
tools/run-fullstack.sh 400` selects another image.

### Found and fixed this session
8. **CRP-then-TC ordering.** SwapMMUMode does `ploadr 8(a0); pmove (a0),crp; pmove 8(a0),tc; pflusha`
   with the record in RAM near the top of memory. Rebuilding the ARM map at the CRP load made the
   record unreachable, the TC read returned the bridge's 0xBAD0BAD0 pattern, and the walker then
   dereferenced garbage (Emu68 fatal exception, core 0 in `SYSHandler` wfe loop). The 68030 keeps
   the old translation in its ATC until the TC load; the shim now defers a CRP change until the next
   TC load or PFLUSH. TC values are sanity-checked (PS/IS/TI must sum to 32) and table reads are
   bounds-checked (`rd32p` had a wrap-around in its range check).
9. **Switch cost.** The ROM/OS calls SwapMMUMode thousands of times (≈5000 in a 4 minute boot).
   Tearing down and rebuilding the ARM mapping each time cost ~1 s per switch (24-bit map: 2816
   runs). Now each (TC, CRP) pair owns a cached ARM subtree (user L1 entries 0..3), new helpers
   `mmu_low4g_get/set/clone` in `mmu.c` swap it in; a switch is a table walk (hash of the 68030
   tables, to catch in-place edits) plus four L1 descriptor writes: 0 ms for the 24-bit set, ~10 ms
   for the 32-bit set in QEMU (the 32-bit walk fills 1M l2p entries; on the Pi this is ~1 ms).
   PFLUSH re-walks and remaps only if the hash changed, and the PFLUSHA that follows every switch is
   skipped (within 5 ms of the switch).
10. The Pi's local interrupt controller is mapped at VA 0x40000000, inside the Mac ROM-mirror
   range; Mac runs are now clipped around both Pi windows (0x3F000000 and 0x40000000..0x40200000).
11. `mac.ram` is clamped to **1024 MiB** (RAM space below the ROM at 0x40000000); bank-A aliasing
   applies only below 64 MiB. The IIci ROM sizes two 64 MiB banks, so >128 MiB needs the
   memory-sizing patch (docs/06).

12. **Stale instruction fetches (the post-Welcome runaway).** Emu68 fetches code for translation
   through its software 68k cache model for addresses below 16 MiB; only 68040 CINV/CPUSH or a
   CACR IE toggle invalidate it. The Mac ROM/OS flushes the 68030 caches with CACR CI/CD, so the
   System 6 'ptch' installer (code just loaded from disk at 0x13414) was translated from stale
   lines, returned a bogus patch table and `_SetTrapAddress` overwrote the OS trap table with
   code words; the next `_Read` jumped to 0x0A43341B. Found with the trap-table write watch
   (`[PSMAC:WATCH]`), the PC trail, a RAM dump compared against the disk image (`tools`-less
   scripts in the scratchpad) and a reference boot on MAME's own 68030 (`mame/trapwatch.lua`).
   Fix: `cache.c` bypasses the model under PSMAC (`CACHE_BYPASS`).
13. **MAME bridge and SCSI pseudo-DMA.** The earlier "reply immediately" optimisation let Emu68
   read the 5380 FIFO before the scheduler had applied the HALT hold that fills it. Replies for
   the SCSI register/pseudo-DMA windows are now held until the next slice (`access_needs_sync`),
   everything else still answers at once. Bus errors reported by the bridge are counted
   (`berr` in the heartbeat) but not yet delivered as 68k exceptions.

14. **Timing calibration from the Pi, and a disk that was not a disk.** With the JIT fast, the
   saturation patch (fix for the 7.1 zero divide) left TimeSCSIDB = 0xFFFF, and every SCSI wait
   loop in the ROM multiplies its millisecond argument by that word: a selection timeout became
   minutes. The ROM now takes the four results from a table the kernel writes into the slot-9
   window (+0x80, `psmb.h` PSMB_OFF_CALIB): TimeDBRA saturated at 0x7FFF (the ROM's own tests
   divide by it), the three bus loops at the bridge's measured latency (`psmac_bus.c` benchmark
   → `psmac_mbox_set_calib`), with saturation as the fallback when no window answers. The stub
   lives in the ROM's 7 KB of zero padding at 0x629E0 and is reached by `bra.l` from the first
   stub (`rom/patches/iici.json`; the ROM's loop table at 0x86E is `{offset.w, lowmem.w}` pairs
   and A4 points at the low-memory word when the stub runs). The heartbeat now prints the four
   globals as the ROM left them.
   The boot still showed the flashing "?" disk, and the new SCSI register trace
   (`mac.scsitrace=N`, folded polls, PC per access) showed why in one screen: arbitration,
   selection and the READ(6) command all worked, and the data phase returned `MComprHD` — the
   CHD file's own header. The testbed MAME could not open the CHD that `chdman createhd`
   had written with its default codecs and fell back to treating it as a raw image. Images for
   the testbed are created with `-c none` (docs/04 said so; docs/09 now says so too).

15. **The virtual slot-9 interrupt, and the storm it caused first.** With the input driver
   draining a ring from a VBL task, the latency of polling became visible, and the card got the
   interrupt a real one has (docs/07 "Interrupts"): the RBV's slot interrupt registers are
   snooped and merged in `psmac_bus.c`, the service core raises the line, the drivers queue
   handlers with `SIntInstall`. The first version raised the line whenever a board had something
   new; the IIci ROM enables every slot's interrupt early in the boot, the network board had set
   its irq flag at start, no driver was open to clear it, and the level-2 handler ran for ever
   (the heartbeat showed ipl 2 and the boot-chime loop). The line is now raised only for boards
   whose Mac driver has attached (the Ethernet driver's version in the board header, the input
   ring's `attached` word). The drivers' first `SIntInstall` came back -344: it had been issued as
   `_SlotManager` selector $0B, which in this ROM is SGetSRsrc. The ROM's dispatch table
   (61 word offsets at 0x6D98, copied to the pointer at low memory $DB8) and the routine at
   0x6E56 (slot in D0.w checked against 14, element inserted by priority with interrupts off)
   settled it: `_SIntInstall` is an OS trap of its own, $A075 (`_SIntRemove` $A076), with the
   element in A0 and the slot in D0.w, as Inside Macintosh V says.
   Found on the way: `mac/enet.s` had been written against an older Alice68 header and
   addressed the command box and rings 36 bytes off; `make -C mac check` now compiles the
   offsets from the header (`tools/mac/an_offsets.c`) and compares them with the source.

16. **PiNet crashed after "PiDisk 0:" on System 7.1.** Not the JIT: `divul #10,%d2:%d0` in the
   decimal printer assembles, in GNU as, to the 64-bit-dividend form (extension word bit 10),
   with D2 never cleared; the divide overflowed, left D0 as it was, and the digit loop wrote
   backwards through the application until it ran into its own code (the kernel's "Undefined
   Line0" and an illegal-instruction trail inside the application's data). The 32-bit form is
   GNU as calls the 32-bit form `divull` (extension word $0002) and encodes `divul`, whatever
   the operand punctuation, as the 64-bit dividend ($0402): the three applications now use
   `divull #10,%d2:%d0`.

17. **System 6, with everything on (16 Nov, evening).** The declaration-ROM drivers load, the
   PiDisk volume mounts, the slot-9 interrupt handler installs and fires for the first input
   event, the cursor follows the Pi's "mouse" and the click opens the Apple menu. The first key
   then bombed with "illegal instruction": `_KeyTranslate` is a Pascal function and the caller
   must push room for its LONGINT result before the three parameters; without it the stack was
   four bytes short on return and `key_char` returned to address 0. Fixed in `mac/piinput.s`.
   PiNet is now launched at boot under System 6 through the boot blocks' startup-application
   name (`tools/mac/bootblock.py --startup PiNet`: bbHelloName, not bbShellName, which was the
   earlier mistake). Two more register-convention mistakes fell out of the same evening:
   `_PostEvent` takes the event code in A0 and the message in D0 (the driver had them the other
   way round, so the Pi's cursor moved but no click or key ever reached a program), and PiNet's
   SpBlock had spCategory at 48 and spTBMask at 55 where Slots.h puts them at 40 and 48, so
   SGetTypeSRsrc answered -344 "no more sResources" (also: SGetTypeSRsrc is selector $0C, and
   $2B, used before, is SUpdateSRT). With those fixed, PiNet found the card (slot 9, sRsrcID
   129), opened `.ENET` through OpenSlot (refNum -52) and the Ethernet driver took its first
   slot interrupt; the EGetInfo control call then returned to address 0, because the driver's
   Control exit left EGetInfo's buffer pointers in A0 and A1 where `iodone` expects the
   parameter block and the DCE (the same mistake PiDisk had made a day earlier). Fixed. The
   next boot sat in a level-2 storm inside the Ethernet driver's Open: the driver announced
   itself to the kernel (which then raises the line for the board's irq flag) before its
   SIntInstall, and the flag was already set. Both drivers now announce themselves as the last
   thing in Open, after the handler is in place, and the Ethernet driver clears the flag first.
   With the log points in the driver the next run read like a script: EAttachPH 0, ERead -94,
   EWrite 0. The protocol lookup restored D1 from the stack as its last instruction, and that
   move set Z from D1 rather than from the search, so "found" and "not found" were decided by
   the protocol number: EAttachPH wrote its entry past the table (over the interrupt queue
   element) and ERead could not find it. The lookup now ends with the result in D0 and Z.
   Then EWrite went out (the host peer answered the ARP) and the 68k never came back from the
   call: the heartbeat's PC was the ROM's slot interrupt dispatcher (0x40806EAA: acknowledge
   IFR, read SIFR and SIER, dispatch). Two handlers share slot 9, the ROM stops at the first
   that returns nonzero, and the Ethernet handler always said "ours", so from the moment it was
   open the input driver's handler never ran, its ring never drained, and the line stayed up.
   A handler now claims the interrupt only when its own board flagged it. That was true but
   not the whole story: with the queue state logged at every completion, EWrite's exit showed
   "qhead 00000000 flags 0000", a DCE that was not one: EWrite had used A4, the register the
   dispatcher keeps the DCE in, for the board pointer, so IODone was handed the board and the
   Device Manager's synchronous wait never ended. The handler now walks its gather list through
   D7 and leaves A4 alone. EWrite then completed, the host peer answered the ARP, and the ROM
   bombed with "unserviceable slot interrupt": that is its verdict when every handler on the
   slot answers 0, and a driver's VBL poll can consume the board's flag between the line going
   up and the handlers running. A real card's driver knows its card raised the line; here the
   input driver installs a second handler at priority 0, called last, that always claims the
   interrupt (`slot_catchall`). With that, **System 6 runs the whole stack end to end**
   (`docs/img/system6-pinet-arp.png`): PiNet launched at boot, the mailbox header, the 20 MiB
   Pi disk, the card found by the Slot Manager (slot 9, sRsrcID 129), `.ENET` opened through
   OpenSlot (refNum -52), the Ethernet address 02:50:53:4D:41:43, an ARP request out through
   the Pi to the host peer and its reply back through ERead, 60 bytes from 02:00:00:00:68:01,
   received by the driver's slot interrupt handler.
   System 7.1 then boots the same stack to the Finder with the Pi disk mounted and the cursor
   and clicks from the Pi's input ring working, but its Finder no longer launched the Startup
   Items (PiNet, PiStatus). The one 7.1 boot where they did launch was the one where the PiDisk
   extension had done nothing; in between it had been taught to MountVol the ROM driver's drives
   at INIT time. The extension is back to doing nothing when the ROM driver is present: the
   driver's own disk-inserted events mount the volume under 7.1 as well. With that, **System
   7.1 runs the stack end to end too** (`docs/img/system71-pinet-arp.png`): PiNet from Startup
   Items, the card, `.ENET` (refNum -53), the ARP round trip. One difference from System 6 is
   visible in the log: the reply was taken by the driver's VBL poll before the slot interrupt
   got its turn (the catch-all claimed the interrupt), which is the race the catch-all exists
   for; the VBL fallback's first-phase period is the thing to lengthen when the interrupt is
   to be the usual path.

18. **MacTCP (16 November, night).** The user supplied MacTCP 2.0.6. Its configuration resource
   was decoded from its own code (`tools/mac/mactcpcfg.py`, docs/09) and the control panel set
   to the Ethernet link layer, first card, 192.168.68.2. MacTCP opened `.ENET`, attached its IP
   and ARP handlers and sent ARP requests for ever: the peer answered every one, MacTCP saw
   none. Its handler glue (in the 'iplp' 129 code resource) saves the driver's A0-A2 and D1 at
   entry and restores them before each ReadPacket/ReadRest call, which it makes from C with its
   own A6, so those registers are the driver's read state by convention, exactly as Inside
   Macintosh: Networking says ("A0, A1 reserved for the driver"); this driver had kept its
   position through A6 and read garbage. ReadPacket and ReadRest now carry the position in A0
   and the bytes left in D1 and nothing else. With that, **MacTCP works end to end on System
   7.1** (`docs/img/system71-mactcp-ping.png`): PiPing's first echo times out while ARP resolves,
   as on any Mac, the next two come back in 0 and 1 ticks through the host peer.

### Debug aids added
- `mac.dbg=LO,HI` bootarg: translator prints the blocks it creates in that 68k range.
- `[PSMAC:PC]` report when the translator is asked for code at an odd or unmapped address.
- `[PSMAC:WATCH]`: the low-memory page is mapped read-only; writes of implausible values into
  the OS trap table are reported with the 68k PC, A7 and stack.
- Service core samples the JIT's 68k PC and keeps the last 1024 distinct values; the trail is
  printed when bus traffic stops for a heartbeat or on the first unexpected 68k exception
  (`[PSMAC:EXC]` from `M68k_ExceptionEntry.c`).
- `dump_l2p` prints the walked logical→physical runs of every new map set.

### Note on the CPU model
Emu68 presents a **68040** (ISA, FPU, caches) to the Mac. The IIci ROM programs a 68030 MMU; the
PMOVE/PFLUSH/PLOAD/PTEST shim is the "030 MMU personality" of that 040 so the unmodified ROM's
table code works. Software only learns the CPU type from the ROM's box table (CPUFlag), so the
Gestalt processor type stays 68030 until the ROM patch set flips it; 040-only instructions
(MOVE16, CINV/CPUSH, 040 FPU) already execute natively in the JIT.

## Root causes found and fixed along the way (keep for the hardware bring-up)
1. Only writes reached the bridge: the read hook was in the inactive copy of `SYSReadValFromAddr`.
2. ~1 ms per access: MAME deferred replies by a scheduler slice; QEMU's mini-UART delivers one byte
   per main-loop iteration and one syscall per byte. Fixed by immediate replies + semihosting FIFOs.
3. Emulated time froze under load (bridge consumed credit only when idle).
4. The ROM entered its serial test monitor: illegal instruction after `PLOADR` because the MMU
   instruction skip ignored EA extension words.
5. Frame-buffer/low-memory confusion: the IIci reserves 0x50000 (not 0x80000) for video.
6. RAM sizing broke when bank A aliases were answered by MAME's mirrored RAM instead of ours.
7. Console deadlocks: the Mac probing 0xF2xxxxxx (slot 2) hit the Pi UART mapped there;
   then EL0 prints from JIT context deadlocked on the print lock when EL0 lost peripheral access.

**System 7.1 boots to the Finder as well** (`sys710p.chd`, `mame/snapFS/maciici_ps/0077.png`) once
the ROM carries the calibration-saturation patch (`ROM=rom/patched/MacIIci_psmac.rom`): with the
unpatched ROM the JIT leaves TimeDBRA ($D00) = 0 and System 7.1's speed probe (code at 0x9C178:
`move.w $D00,d1 ; mulu #4000,d1 ; divu d5,d1`) dies with a zero divide and the bomb dialog.
System 6 does not touch TimeDBRA that way and boots with either ROM.

19. **A Macintosh SE by name (16 November, late).** The user asked whether the ROM could be
   patched to identify as a Macintosh SE. The IIci ROM's identity lives in two places: the box
   table its Start Manager consults for the hardware it drives (video, sound, the VIA layout,
   the memory map), and the Gestalt Manager's answer to `'mach'`, which is what programs and
   About This Macintosh show. The first must stay "IIci": it is the reason the ROM runs on the SE
   at all (docs/00). The second is a documented, replaceable selector: `mac/pimach.s` is a
   66-byte INIT that installs a function returning gestaltMacSE (5) with `_ReplaceGestalt`,
   so the Finder says "Macintosh SE", with the compact Mac's picture, which the Finder also
   chooses by machine type (`docs/img/system71-about-se.png`), and the ROM goes on thinking
   it is a IIci. The processor still reads 68030, which is what the kernel presents. For two
   hours the About box refused to open with the extension installed, with 5 and with 11 as
   the answer alike, and the extension was suspected; the cause was the input driver's double
   drain (item 23), which turned the script's click on the Apple menu into two.
20. **The WiFi, tested without a WiFi chip (16 November, late).** The user asked whether the WiFi
   could be tested in this environment. The chip cannot be: QEMU's Pi has no SDIO function and
   Broadcom's firmware would have nothing to run on. Everything above it can, because the Mac
   only ever sees unit 0 of the network board. The service core now has a simulated radio
   (`mac.wifisim=`, docs/09): it scans, joins with a name and password check, carries frames
   through a second host file pair, and never starts the real driver. With it, PiWiFi's scan and
   join dialogue runs from a scripted keyboard (`mac.inputwifi=`), and MacTCP, told to use the
   WiFi card by building the declaration ROM without the Ethernet sResource, pings the host peer
   through `.Network_WiFi_PiStorm`, the same driver as the Ethernet's assembled for unit 0.

21. **The stall that was a race with the MMU (16 November, late).** Three runs in twenty-five
   had frozen with the kernel log cut mid-line, the Mac's screen blank and MAME's bridge
   waiting for requests that never came (fs83, fs108, fs110). Always just after the ROM's first
   MMU table switch, always inside a line the bus core was printing. The cause is in two
   designs meeting: upstream Emu68 maps the Pi's peripherals inside the first 4 GiB so a 68k
   driver can reach them, and the Macintosh MMU shim (psmac_mmu.c) installs a whole new first
   4 GiB each time the ROM switches translation tables, break-before-make: four level-1
   entries cleared, a TLB flush, four new ones written. For those microseconds nothing below
   4 GiB exists on any core. Core 0 is the one doing it and touches nothing there meanwhile;
   core 2 was writing its heartbeat to the PL011 at 0x3F201000, and core 3 reads the firmware
   mailbox at 0x3F00B880 for the status page. A core that falls into the gap takes a data
   abort; the exception handler's first act is to print, the console lock is held by the
   very print that was interrupted, and the machine stops: no console, no bus requests, no
   heartbeat. Intermittent because the gap is short and the switch rare. On hardware the bus
   core would hammer the GPIO block through the same window a hundred thousand times a
   second, so this would have been the first thing to go wrong at the first MMU switch there.
   The fix is a second mapping of the peripherals high in the kernel half of the address
   space (`PSMAC_PERIPH_HI` in psmac.h, mapped beside the low one in start_rpi64.c), which an
   MMU switch never touches; the bus core's console and UART, the Alice modules' mailbox,
   SDIO, Bluetooth UART and GENET registers now go through it, and the low window stays for
   the 68k's sake only.

22. **The keyboard typed random letters (16 November, late).** The first program to take typed
   input, PiWiFi, ignored every key. A trace in `.PiInput` (the first eight key events go to
   the Pi's log with PostEvent's answer) showed the keys arriving, posted, and carrying the
   wrong characters: S came out as control-B, J as N. The driver translated through
   `_KeyTranslate` with the pointer it found at $1B40, which MPW's SysEqu.a names `KCHRPtr`; a
   dump of that memory from the kernel (`mac.peek=`, added for this) showed 68k code, not a
   keyboard layout: under 7.1 on this ROM the long holds the address of a patch. With the
   `'KCHR'` 0 resource fetched at Open instead, the trap answered zero for everything, and so
   did a hand-written lookup of the documented table. The driver's log line for the resource
   gave it away: the KCHR was "at E009ACD4". The slot driver opens during the boot, in 24-bit
   mode, where a master pointer carries the handle's flag bits in its top byte; the pointer is
   used much later, in 32-bit mode, where E009ACD4 is nowhere. `_StripAddress` on the
   dereferenced pointer, the call Apple made for exactly this, and S is S again. The driver
   now reads the KCHR itself (the modifier byte picks a table, the virtual code the character,
   as Inside Macintosh: Text documents the resource); dead keys and the layout chosen in the
   Keyboard control panel are still to come. The trace stays in the driver; it is the first
   thing to read when a keyboard misbehaves on hardware.

23. **Every PiWiFi command "failed" after its timeout (16 November, late).** The first scan
   through the simulated radio filled the list, and the window said "Last command failed:
   -1" twenty seconds later. The command routine in PiWiFi (and PiBlue, the same code) kept
   the sequence number it was waiting for in D2 and called `_SystemTask` in the loop; Toolbox
   traps preserve only D3-D7 and A2-A6, and that one did not keep D2. The number lives in D4
   now and the board pointer in A2. The line reader had the same flaw (its length in D2 across
   GetNextEvent and DrawChar: Return then looked like a cancel), and so had the input driver's
   VBL task, which kept draining the ring after the slot interrupt took over and posted a key
   twice when the interrupt came in mid-drain. A lesson Inside Macintosh states in its first
   volume, relearned the slow way. The scripted session also typed ahead while the list was
   being redrawn through the bridge, and the system event queue, twenty events deep under
   System 7, would have dropped the rest; the script waits for the redraw now.

24. **Memory sizes (17 November, small hours).** The user asked for more than the testbed's 8 MiB
   and for Pis of every size. The testbed now runs with 16 MiB (`PSMAC_RAM`, default 16 in
   `tools/run-psmac-qemu.sh`), and without `mac.ram=` the kernel takes what the Pi can spare,
   up to `PSMAC_RAM_DEFAULT_MB`, keeping 192 MiB for itself. 64 MiB boots System 7.1 to the
   Finder (fs133). 128 MiB does not: the boot bombs after the extensions with the 68k fetching
   code from an odd address around 74 MiB (fs131, "translating at bad PC 046a003f"), which is
   bank B territory in the IIci's memory map (bank A is the first 64 MiB at 0, bank B the next
   at 0x04000000). The ROM sizes the two banks and builds its MMU tables for them; something
   in how the kernel presents the second bank is wrong, and until that is found the default
   stays at 64 MiB and `mac.ram=128` is a known crash. The RAM-size probe is the next thing to
   read in docs/03's boot flow.
25. **The second display, first light (17 November).** docs/10's design, built: a display block
   in the slot window (psmb.h), the frame buffer in the window's top 512 KiB so that 24-bit
   System 6 can reach it too, a video sResource with four modes (1, 2, 4, 8 bits at 640 by
   480) copied field for field from the IIci ROM's own `Display_Video_Apple_RBV1`, a video
   DRVR (`mac/pivideo.s`) answering Apple's Control and Status calls, and the service core
   converting the Mac's indexed pixels through the colour table into the VideoCore frame
   buffer that the boot splash set up, sixty ticks a second with the slot interrupt as the
   card's VBL (`emu68/src/psmac/psmac_display.c`). QEMU emulates that frame buffer, and
   `tools/vcshot.py` photographs it through the monitor socket: the testbed can show the Pi's
   screen as well as the SE's. The first boot opened the driver from the declaration ROM, as
   the Start Manager does for every video card, and the driver found no display block: it had
   addressed the card at $F9058000, and in 24-bit mode, which is how the ROM boots, the slot's
   megabyte is $900000-$9FFFFF (the slot number in bits 20-23), so $F9058000 was plain RAM.
   The driver asks MMU32Bit now, as the disk and input drivers do. With that the ROM ran its
   full screen set-up on the card and the VBL interrupt fires sixty times a second, but the
   device it created is not activated (PiScreens, docs/10), so QuickDraw does not paint it yet;
   that is where the work stops tonight. Along the way: the ROM wants a video card's frame
   buffer in the 32-bit form of its 24-bit megabyte ($F9980000), the kernel aliases the window
   there; and this 7.1 runs in 24-bit mode, which is its own 8 MiB ceiling (item 24).

26. **32-bit addressing, and what activates a second screen (17 November, small hours).** The
   user asked for both. The 24-bit finding of item 25 had a PRAM cause: the IIci ROM reads
   extended PRAM byte $8A at start-up (0x4080CC60: one byte with _ReadXPRam, then bits 0 and 2
   tested) and a fresh PRAM has both clear. Setting them (`tools/pram32.py` writes MAME's saved
   PRAM) boots System 7.1 in 32-bit mode: PiScreens reads MMU32Bit 1 and the video driver's
   trace says "mode 32" for every call. The screen question needed the ROM's own loader for
   the 'scrn' resource, which Apple never documented beyond its name; it is at 0x40827BA0 in
   this ROM, called from InitGraf's start-up pass, and reads as follows. GetResource('scrn', 0)
   from the System file; if none, nothing (the screens stay as the Start Manager left them:
   side by side, only the startup screen active). Otherwise a count word and one record per
   video sResource in sNextTypeSRsrc order, each compared against the hardware: spDrvrHW,
   spSlot, the driver's dCtlDevBase, and the rectangle's height and width against the device's
   own; one mismatch or a wrong count, and the resource is ignored. A matching record sets the
   mode (InitGDevice), gdFlags through a mask (screenActive is bit 15, mainScreen bit 11), the
   colour and gamma tables by resource id, the rectangle, and sends any listed Control calls to
   the driver. `tools/mac/mkscrn.py` builds the resource from those values and
   `tools/mac/rsrcadd.py` puts it into the System file; the values themselves come from
   PiScreens, which now lists the video sResources with exactly the fields the loader compares.
   With those in place the second screen is active and the Finder's desktop spans both
   monitors (docs/10, "It works"): the SE's one-bit screen on the left, the Pi's 256-colour
   screen on the right, an icon dragged across the edge showing on both.

27. **Direct colour, larger screens, and the road past 64 MiB (17 November, 02:00-03:00).** The
   user asked for the three in order: thousands and millions of colours, the HDMI mode's
   resolution, and the 128 MiB fault, with the memory policy right for Pis of every size.
   - *Direct colour and sizes* are in docs/10 ("Direct colour and other sizes"): display block
     v2 with per-mode frame buffer tables, the 8 MiB big region at $F9100000, `mac.screen=`,
     a declaration ROM per size. The first 32-bit picture was dark red stripes: QEMU had granted
     32 bits, then the ROM's RESET instruction took the JIT into Emu68's `do_reset`, which
     remakes the VideoCore buffer at 16 bits for the Amiga splash. PSMAC now reinstates its own
     buffer there (`psmac_display_reinit`). The byte order of a 32-bit pixel is asked of the
     firmware (tag 0x40006; QEMU says "RGB" and reads the first byte as red) and `mac.vcswap=`
     overrides it; PiColor's reds are red in QEMU with the answer honoured (fs158). The second
     32-bit picture was a plain grey: PiScreens (fs159) showed the device active at 32 bits but
     based at $F9980000, the window, while the Pi converted the big region. The ROM's
     InitGDevice never asks the driver where a mode's pixels are; it adds the mode's
     vpBaseOffset from the sResource to dCtlDevBase. mkdeclrom.py now writes the offset by the
     kernel's own placement rule (0 for an indexed mode that fits the window's 512 KiB,
     -$880000 to the big region for the rest), and the driver's tables agree.
   - *The JIT and the ROM's second home.* The block verifier exempted $800000-$8FFFFF from
     re-checking because the ROM appears there in 24-bit mode. In 32-bit mode with more than
     8 MiB that megabyte is RAM: code the Memory Manager loaded over an old block was never
     re-verified. The exemption now asks the MMU shim whether the block's logical address
     translates into the ROM (`psmac_mmu_l2p`), whatever the mode.
   - *The 128 MiB boot and the speed probe.* With the above, the 128 MiB boot (fs157) no longer
     dies at an odd PC in bank B; it dies two minutes in with a zero divide (vector offset $14,
     format 2, PC $9D1DE in the system heap, D5 = 0), and the code is System 7.1's speed probe
     again, found by its bytes in the System file: `_InsTime; _PrimeTime; four times (move.w
     TimeDBRA,d0; dbra d0,*); _RmvTime`, the smallest elapsed time over a few passes, less the
     same measurement without the loops, then `move.w TimeDBRA,d1; mulu #4000,d1; divu d5,d1`.
     The JIT runs a self-looping dbf in four ARM instructions per turn, so the four loops take
     microseconds and the Time Manager, reading the VIA through the bridge with its jitter,
     measures the same for both passes often enough: zero. `M68k_LINE5.c` now gives every
     self-looping dbf one device read per turn under PSMAC (the system timer's counter through
     the peripherals' high alias), about what a 68000 spent per turn, so the ROM's and the
     System's timed loops take real time; `mac.dbfslow=N` sets the reads per turn, 0 for none.
     The exception logger also prints the instruction words around a faulting PC now. With
     the slowed loops the 128 MiB boot ran its five minutes to the Finder's extensions and
     beyond without a fault (fs161).
   - *A bank that is not full.* 72 MiB (fs162) never left the ROM's memory test: the shim
     aliased RAM across bank A's 64 MiB window only, so a part-filled bank B had nothing past
     its end where the ROM looks for the wrap. `norm_phys`, the identity map and the page filler
     now alias bank B across its own window too. The default without `mac.ram=` is 128 MiB
     (`PSMAC_RAM_DEFAULT_MB`), still bounded by what the Pi has after the kernel's 192 MiB, so a
     512 MiB board and an 8 GiB board both give the Macintosh 128 MiB. More than that needs the
     ROM's memory sizing patched (docs/06, item 3), which is the next memory step. Verified
     alone on the testbed (two runs at once share the bridge socket and corrupt each other,
     which had muddied fs161-fs163): 128 MiB boots System 7.1 to the Finder with the stack at
     $07F7FF32 and no exception in 400 s (fs166), 72 MiB the same with the stack at $04786F5E
     (fs167); `docs/img/system71-about-128mb.png` is About This Macintosh on the 128 MiB boot
     (fs171): a Macintosh SE with 131,072K of memory.

28. **The IIci ROM meets a Macintosh SE (17 November, 04:00-).** With the hardware known
   (docs/12) the board layer was written the same night, and a Macintosh SE to test it against:
   MAME's `macse` with the bridge CPU on a 16-bit, 24-bit bus (`macse_ps`, docs/06). What the
   first boots found, in order:
   - The IIci ROM's first cycles translate as expected: VIA interrupt-enable tests at $EFFDFE,
     the decoder probes answered locally, the boot chime's wavetable going into the kernel's
     ASC while its timing loop reads the SE's VIA. The chime took 140 s against the SE where
     the IIci took 50: every VIA read is a bridge round trip and the SE machine's are slower.
   - Then the ROM sat for ever at $40842F6A reading SCC channel A's RR0 (fs172). Not the SCC's
     fault: the loop is guarded by bit 17 of D7, the ROM's hardware flags, which were $20011
     here and $40003 on the IIci. The flags come from the machine's identity, and the Mac II
     family reads that from VIA1 port A: bits 6, 2, 1, 0 are a box code, $C7 on a IIci. An
     SE's port A reads $81. The board layer now answers ORA reads with the IIci's identity bits
     and the SE's own bits 7, 5, 4, 3 (`via_read_fix`); port B's bit 6, an input tied high on
     the IIci and the video beam on the SE, is read as high too.
   - With the identity right the first 400 bus cycles match the IIci's to the byte, yet the
     boot still ended in the ROM's error reporter (fs174: the "ERROR:" strings at $408442C6,
     the SCC set up as a terminal, bit 17 of D7 set at $40842EA0 by that set-up and nothing
     else). The registers at the time pointed at the ASC, whose 2 KB of wave RAM the kernel's
     stand-in did not keep: writes went nowhere, reads came back zero, and the test that writes
     and reads it failed. The stand-in now has the RAM (fs175), which did no harm and did not
     help: the trail into the error reporter (`mac.pcwatch=`, a new option that dumps the PC
     ring the first time the sampled PC enters a page) showed the chime itself was the way in.
     The order is ROM checksum (test 1, $40844690), the startup chime, then the RAM test
     ($408446BC); the chime had been read as the death chime because it took 140 s against the
     SE where the IIci took 50. The RAM test failed at once because the board-layer change had
     dropped not only the mirror of the first 320 KiB to the motherboard but the local write
     with it: every store below $50000 vanished. Local write kept, mirror dropped (fs178).
   - **fs178, 04:29: the first picture on a Macintosh SE's screen drawn by the IIci ROM**
     (`docs/img/macse-first-picture-sadmac-0f-33.png`). The ROM passed its tests, scanned the
     SCSI bus through the translated 5380, found the slot-9 card, opened the video driver, took
     its first VBL through the slot interrupt, and the service core wrote the card's frame into
     the SE's screen buffer, 342 rows: a Sad Mac, codes $0000000F $00000033, system error 51,
     an unserviceable slot interrupt. The dispatcher found the slot-9 line asserted and no
     handler that owned it. The cause was a stale line: core 3 samples the card's interrupt
     sources and the bus layer kept its last sample; the ROM's dispatcher re-reads the slot
     flags right after a handler returns, saw the line still up, dispatched again, and the
     handler found nothing pending. On the IIci the input driver's catch-all handler (D0 = 1
     for anything left) had always been installed first, because there the card was the
     second screen and its driver opened late; on the SE the card is the main screen and its
     driver opens before any other. The line is evaluated on demand now (`psmac_slot_line_now`).
   - **fs180, 04:36: the IIci ROM boots on the Macintosh SE's board** and draws its desktop
     pattern, its cursor and the blinking disk on the SE's own screen
     (`docs/img/macse-iici-rom-asks-for-a-disk.png`): everything up to "find me a startup
     disk" runs through the translation. The SCSI scan found nothing because MAME's SE decodes
     5380 writes on the upper byte of the word while the SE writes the chip at odd addresses,
     which a 68000 serves by putting the byte on both halves of the bus; the bridge's byte
     writes now do the same (fs181). With that the 5380 answers its register test, arbitrates,
     selects the disk at ID 6 and gets a command across (fs183's trace, with polls folded: the
     ROM's READ of block 0, the data phase entered, status and message read, and the whole
     thing retried for ever). The retries were the pseudo-DMA: the SE's DACK window at $5FF200
     mirrors the registers, and the data comes from the input data register inside it,
     $5FF260, not from $5FF200 itself, which is the current-data register with DACK and reads
     whatever sits on the bus. The translation now reads $5FF260 a byte at a time (fs184).
   - **fs184, 05:04: System 7.1's Finder on the Macintosh SE's screen**
     (`docs/img/system71-finder-on-macse.png`): the IIci ROM read the boot blocks from the SE's
     own 5380, loaded the System from the SCSI disk, opened PiDisk and PiInput from the slot-9
     declaration ROM, ran PiMach and the other extensions, mounted PiDisk beside the SCSI
     volume, and the Finder drew its desktop on a 512 by 342 screen the Pi's card feeds into the
     SE's video RAM, 1,666 rows in all. Boot took three and a half minutes against the bridge
     with 16 MB. No exception, no bus error. What this proves: the whole translation, VIA, SCC,
     5380 with pseudo-DMA, RBV, ASC, VDAC stand-ins, the slot interrupt, the frame copy, works
     on the SE's logic board as MAME models it. What it does not yet: keyboard and mouse on
     that board (the SE's ADB through its VIA is wired but untried: the testbed's input has
     been the Pi's input ring), the IWM, sound, and anything about the real CPLD.
*(17 November, 03:30: the hardware is now known, a classic PiStorm with a Pi 3 in a socketed SE,
WiFi first; docs/12 orders the bring-up and supersedes the hardware items below.)*
1. MacTCP under System 6 (the same files; untested there), then a real client (Fetch, NCSA
   Telnet) against a TAP-bridged peer (`tools/pinet.py ... tap`), and the WiFi unit on hardware
   (the simulated radio proves the Mac side; the chip, the firmware and the SDIO path remain).
2. Hardware: the PiStorm GPIO transport in place of the semihosting bridge (`ps_protocol.c` is
   already behind the `ps_read/ps_write` API), the initramfs container on the SD card with the
   ROM, the declaration ROM and the firmware, and the first boot on a Macintosh SE.
3. The Pi 4 drivers for real: GENET and the VL805 xHCI (the DMA pool below 1 GiB), WiFi and
   Bluetooth firmware from the container, and the choice of board (docs/00) that decides which
   of them matter.
4. The second display (docs/10): frame buffer at the slot's super space, a video sResource, the
   HVS on the Pi.
5. Housekeeping: deliver bridge bus errors as 68030 access faults for the SCSI Manager's
   timeout path; drop the debug-only read-only mapping of the low-memory page; take the
   calibration patch and the rest of `rom/patches/iici.json` to the real Pi's timings.

29. **First silicon (17 November, 05:45).** The user put 0.3 on a Pi 3B v1.2 with an HDMI
   monitor and nothing else: the firmware's rainbow square, then nothing, and no error pattern
   on the ACT LED, so the firmware had handed over and the kernel died before Emu68's splash.
   The splash is the first mailbox call after `platform_post_init` identity-maps the
   VideoCore's memory as write-through normal memory, and on a Pi 3 with gpu_mem=64 that memory
   is reported as 0x3C000000-0x3FFFFFFF: right over the peripheral window this build keeps at
   0x3F000000, through which `support_rpi.c` drives the mailbox. A device behind a cacheable
   mapping polls its own stale status for ever; QEMU does not model memory attributes and never
   noticed. The mapping is clipped at the window now (the ARM cannot see SDRAM there anyway).
   Two things made the night's first hardware report possible without a serial cable: the
   kernel's log drawn on the HDMI screen (`mac.console=hdmi`, psmac_display.c) and the rainbow
   square left visible (no `disable_splash`), so a firmware that gave up and a kernel that did
   look different.

30. **The WiFi chip did not answer on SDIO (17 November, 08:00).** On the same Pi 3B the
   kernel's own log read "SD host: spec 2", then "no SDIO card (CMD5)" and "the WiFi chip does
   not answer on SDIO". First guess, wrong: that the 3B has WL_REG_ON on SoC GPIO 41 like the
   Zeros. Its device tree says otherwise: the 3B has the VideoCore GPIO expander (BT_ON line 0,
   WL_ON line 1, like the 3B+), and its GPIO 41 is PWM1_OUT, the audio. Only the Zero W and
   Zero 2 W drive WL_ON from GPIO 41; `wl_on()` now picks by board type (revision code bits 4-11)
   and logs which. What was wrong on the 3B is the SD host's pin pulls: `sdh_pins()` wrote the
   BCM2711 pull registers (0xec), which do not exist on a BCM2837, so GPIO 37-39 (DAT1-3) kept
   their reset pull-down. DAT3 is the card's SPI select during CMD0: a GO_IDLE with DAT3 low
   puts an SDIO card into SPI mode, after which the SD-mode CMD5 gets no answer. The pulls now
   go through GPPUD/GPPUDCLK1 on the older SoCs (CLK none, CMD and DAT up). The 32.768 kHz sleep
   clock (GPCLK2 on GPIO 43, WIFI_CLK) is start.elf's job on every board; the kernel logs its
   state and only programs it on a Zero when it is not running.

31. **Black screen on the first try of the fix (17 November, 08:15).** The console stopped in the
   middle of a heartbeat line and the screen went black for good, kernel still running. The
   VideoCore mailbox has one reply FIFO for every caller: `support_rpi.c` (CPU0 and CPU2, via
   FBReq) and the WiFi module on CPU1 (`mbox_tag_`). Emu68's receive loop matched only the
   channel. The 68k's RESET at the top of the ROM remakes the VideoCore buffer
   (`psmac_display_reinit`) at the same moment the WiFi start asks the firmware for the board
   revision: CPU0 took the WiFi's reply, read FBReq before the firmware had filled it (base
   64, pitch 0, so the old buffer was kept and cleared) and the firmware, answering the real
   request a moment later, moved the scan-out to a fresh buffer nobody drew into. One spinlock
   (`vc_mbox_lock`) is now held from send to receive by both users, and both match the reply by
   its buffer address as well as the channel. The heartbeat's WiFi line also read the board
   before it existed (`U(0)` from a null base): now "no board yet".

32. **Still no answer to CMD5 with the pulls right (17 November, 08:45).** The second 3B run
   confirmed the rest of the path: board type 08, WL_ON through the expander, the sleep clock
   running (CM_GP2CTL 0x291, CM_GP2DIV 0x249f00: the oscillator divided by 585.9375, 32.768 kHz),
   the Arasan host at 200 MHz, spec 3, and no reply to CMD5. What the Pi 4 never had to think
   about: on a Pi 3 the firmware boots from the SD card through this same Arasan host, routed to
   the card slot on GPIO 48-53 (ALT3, "SD1"); Linux moves the card to the sdhost controller
   (ALT0) before it gives the Arasan to the WiFi. Left as the firmware leaves it, selecting ALT3
   on GPIO 34-39 as well puts the card and the WiFi chip on the same host at once. `sdh_pins()`
   now returns GPIO 48-53 to inputs on the older SoCs first (the kernel does not read the card
   after boot: everything comes from the initramfs), and the CMD5 failure line carries the
   host's status, interrupt and control words for the next photo.

33. **The 3B's WiFi chip answers (17 November, 09:15).** With the card slot detached from the
   Arasan (its six pins had been on ALT3, "functions were 3ffff"), the same boot went: SDIO card
   with 2 functions, OCR a0ffff00, 4-bit bus at 25 MHz, chip a9a6 rev 1, "Cortex-M3 + SOCRAM,
   RAM 512 KB at 0, 7 cores, PMU rev 24", firmware and NVRAM loaded in 94 ms, firmware running
   after 97 ms, the chip's MAC address and the firmware banner (wl0 7.45.98, July 2021). That is
   the CM3/SOCRAM chip path of item 24 running on real silicon for the first time, and the whole
   Alice68 WiFi module running on a Pi 3 for the first time. Nothing on the Mac side uses it yet;
   joining a network is `wifi.ssid= wifi.psk= wifi.country=` in cmdline.txt.

34. **Joined a network (17 November, 09:40).** With `wifi.ssid= wifi.psk= wifi.country=AU` in
   cmdline.txt the 3B scanned (8 networks), found the office network at -50 dBm on channel 1
   (WPA2-PSK), derived the key from the password in 29 ms, authenticated and associated with
   status 0, finished the WPA handshake and reported "connected". The radio side of the Pi 3
   WiFi is complete; what remains for the Macintosh is a driver for the slot 9 network board
   (the Amiga side has one, the Mac side has none yet).

35. **The solo board: System 7 on a Pi with nothing behind it (17 November, 10:30-12:30).** With
   the WiFi up and the PiStorm board still in the post, the user asked for a disk image on the
   card that boots and brings up the second screen. That means the kernel has to be the logic
   board: `mac.board=solo` (psmac_board_solo.c, 700 lines) models the four chips the SE layer
   forwards to the real SE, in the IIci's address map the ROM uses: a 6522 (ports, both timers at
   783.36 kHz, the shift register; the 60.15 Hz vertical blank on CA1 and the clock chip's
   one-second tick on CA2), the clock/PRAM chip on port B bits 0-2 after MAME's macrtc.cpp (256
   bytes of PRAM seeded from the testbed IIci's rtc file, `psmac_pram_default.h`), the ADB
   transceiver state machine after macadb.cpp with no devices (every Talk ends the way an empty
   bus does), a Z8530 register file, an IWM whose sense lines say "no drive", and an NCR 5380 as
   the initiator sees it with a direct-access SCSI target per disk image (INQUIRY, READ/WRITE 6
   and 10, READ CAPACITY, MODE SENSE pages 1/3/4/8 and Apple's $30, REQUEST SENSE). The RBV, ASC
   and VDAC stay the SE layer's. Disk images (`*.img`, `*.hda`, `*.image`, `*.hfv`) come from the
   container, in the copy start.c makes on the heap, and are written there, so changes last until
   the power goes; the testbed takes host files (`mac.scsi0=` through semihosting without the
   bridge, `mac.semihost=1`). `tools/run-solo.sh` runs it under QEMU alone, no MAME, with
   screenshots through the QEMU monitor. Three things stood between the first boot and the Finder:
   - The first run reached "Welcome to Macintosh" in 90 s and then took a second per disk block.
     The ROM's SCSI Manager enables the RBV's flag bit 3 (the 5380's interrupt) and waits for the
     level-2 interrupt at the end of every transfer, falling back to a timeout. The chip's IRQ
     (set on a phase change during pseudo-DMA, on bus reset and on loss of BSY with monitor-BSY
     on) now drives that flag, level-sensitive.
   - With that fixed, the boot stopped in an extension at the same place every time, spinning on a
     flag that an interrupt would clear: the LAP Manager opening LocalTalk on SCC channel B (the
     solo report's SCC dump showed channel B in SDLC mode with NRZI and transmit interrupts
     enabled: 6,703 register writes). The testbed's PRAM had AppleTalk active on the printer port
     (SPConfig $01); the solo default has both ports async ($22, Basilisk II's default) and
     AppleTalk off. A real SCC model with interrupts would lift this; nothing on the bench needs it.
   - The plain System 7.1 image (`sys710.chd`) is not bootable at all (the ROM reads 152 blocks
     and shows the blinking question mark); `sys710pi.image` is. Not a solo problem.
   System 7.1 then boots from the image to the Finder on the second screen in about 100 s under
   QEMU, with PiDisk, PiMach and the PiPing startup item running (`docs/img/solo-finder.png`).
   No keyboard or mouse yet: the ADB bus is empty until the Pi has input devices to put on it.

36. **System 7 on the Pi 3B alone, pinging the router over the WiFi (17 November, 21:00 local).**
   The first hardware boot of the solo board ended in the blinking question mark: the Pi firmware
   puts a `followkernel` initramfs right behind the kernel, where a 1 MB container never reached
   anything but a 101 MB one lay across the kernel's early tables, and the image came out
   corrupted. `initramfs aurora68.cpio 0x10000000` in config.txt (now the release default) fixed
   it, and the boot then matched the QEMU log command for command. Two more things for the
   network: MacTCP's Ethernet link layer takes the first network card the Slot Manager lists, and
   the standard declaration ROM lists the wired one first, which a Pi 3 does not have, so
   `aurora68-wifi.declrom` (psmac_wifi_video: screen, PiDisk, input, WiFi, no wired card) is the
   one for the bench; and PiPing's peer address is now a Makefile parameter (`make PEER=`),
   with `tools/mac/mactcpcfg.py` setting the image's MacTCP to the user's subnet. Result: the
   Finder on the HDMI monitor, PiPing with "My address: 192.168.0.77" and three replies from the
   router in 16-32 ticks (`docs/img/solo-pi3b-piping.webp`, the user's photo). A Macintosh IIci
   ROM, System 7.1 and MacTCP running on a Raspberry Pi 3B with no Macintosh in sight. Still
   missing on the bench: a mouse and keyboard; the Mac-side input driver and the Pi's Bluetooth
   HID path exist from the Pi 4 work, so the next step is Bluetooth on the Pi 3's PL011.

37. **Bluetooth for the bench (17 November, 22:30).** The Alice68 Bluetooth host (bthost.c,
   btle.c: Classic and LE HID, written for the Pi 4) gets its Pi 3 specifics in bt_pi.c: BT_ON
   by board type (expander line 0 on the 3B/3B+/3A+/4/CM4, SoC GPIO 45 on the Zero W, 42 on the
   Zero 2 W), pulls through GPPUD on the BCM2837, and the one-PL011 problem: the kernel's log is
   on the PL011 and the Bluetooth half of the WiFi chip hangs off the same UART (GPIO 30-33), so
   `psmac_console_miniuart()` moves the log to the mini UART on GPIO 14/15 the moment the module
   takes the PL011. Pairing needed a pointer (PiBlue is a Mac application); `bt.pair=1` scans
   and pairs with the first keyboard or mouse in pairing mode, repeating every 20 s until one is
   connected, prints the passkey for a keyboard, and prints every paired device as the `bt.dev=`
   word for cmdline.txt. Built and smoke-tested under QEMU (no chip there: the module fails
   cleanly and the Mac still boots); the first real test waits for the Pi on the desk.
   First real test on the 3B: "the chip does not answer (command 0c03)". The 3B's Bluetooth UART
   has TX and RX only: its device tree's uart0_pins is GPIO 32/33, and Raspberry Pi OS's btuart
   runs it "noflow" at 921600 (the 3B+ and 3A+ are the same; the Pi 4 and the two Zeros wire
   CTS/RTS on GPIO 30/31). With CTS flow control on and nothing driving CTS, the PL011 held the
   first HCI_Reset in its FIFO for ever. Flow control is now chosen by board type, and only
   GPIO 32/33 are switched to the UART where 30/31 are not wired.
   Then an LE mouse (report ID 3 mouse plus keyboard reports, 297-byte Report Map, no boot
   characteristics) paired with `bt.pair=1`, reconnected from its `bt.dev=` word, and delivered
   its reports: the heartbeat counted 52 notifications and the Mac's input ring took 50 of them
   (18 November, 08:28). The interim "no keyboard or mouse reports found" and "reports 0" were
   a first connection before the Report Map handling settled and a mouse not yet moved; the
   heartbeat's LE line (stage, characteristics, map head, ATT error, frames since up) is what
   told them apart and stays. Still open: the pairing has to be copied from the screen into
   cmdline.txt by hand until the kernel can write the card.

38. **The card writer (18 November).** The bench needed two things kept across a power cycle:
   Bluetooth pairings (until now copied by hand from the screen into cmdline.txt) and what the
   Mac wrote to its disk image. psmac_card.c drives the boot card through the BCM2835's sdhost
   controller on GPIO 48-53 (ALT0), the split Linux uses on a Pi 3 (the Arasan is the WiFi's on
   34-39; sdio.c now only takes 48-53 off the Arasan when they are still ALT3, and every GPFSEL
   read-modify-write from the WiFi, Bluetooth, console and card code goes under one lock,
   `psmac_gpfsel_lock`, since they run on different cores). Register layout and reset sequence
   after Linux's bcm2835-sdhost; SD init per the Simplified Specification; single-block PIO
   reads and writes, the write's programming time polled across passes of core 3's loop so the
   Bluetooth UART beside it never waits. A FAT16/FAT32 layer that never moves or grows anything
   but its own file: `pairings.txt` (created in one cluster of the root directory, rewritten in
   place; bt_pi.c reads it after cmdline.txt's words and hands every change of the paired list
   back), and in-place sector writes into `aurora68.cpio`. cpio aligns file data to 4 bytes, so
   the image's blocks straddle card sectors: whole sectors are written from the container in
   memory, whose bytes around the image are the unchanged cpio headers. The container is only
   written after its size and first sector match the loaded copy. Tested under QEMU, whose GPIO
   model moves the SD card to the sdhost when 48-53 go to ALT0 (`tools/run-solo.sh` with
   `CARD=`): 99 KB written back after a System 7.1 boot, the extracted container parses with all
   changes inside the image, fsck.vfat is clean, the next boot from it reaches the Finder, and
   Bluetooth loads the pairing from the card.

39. **PiLog, C on the Mac, and the modifier keys (18 November).** The bench wanted the kernel's
   log on the Mac's own screen once it has booted. The kernel now copies every log character
   into a 64 KiB ring in the card window at 0x60000 (`psmb.h`, struct psmb_log; `psmac_bus.c`
   keeps what came before the window existed and copies it in when the window is made).
   `mac/pilog.c` shows it in a window that moves, resizes, zooms and scrolls. It follows new
   lines at the bottom, sleeps in WaitNextEvent between looks at the ring's head, and filters
   by part (Kernel, Mac, WiFi, Bluetooth, Network, Card; Option-click for one part alone). The
   image starts it from Startup Items. PiLog is the first Mac program in C: `mac/c` is a kit that
   turns m68k-linux-gnu-gcc output into an application without Apple headers. It has Toolbox
   glue generated from a one-line-per-call table, a self-relocating startup, and an ELF-to-CODE
   packager (`mac/c/README.md`). The first build hung at NewWindow: GCC's m68k ABI takes a
   pointer result from A0, and the glue left it only in D0. Testing the filter's Option-click
   with a new testbed script (`mac.inputclick=SECONDS,X,Y[,o]`) found a bug in `.PiInput`
   that predates PiLog. It kept KeyMap as if it were little-endian longwords (byte (k >> 3) ^ 3).
   The ROM's `_PostEvent` (IIci ROM 0xB406) builds an event's modifiers from the word at
   KeyMap+6 rotated left by one, i.e. code k at byte k >> 3, bit k & 7. So Command, Shift,
   Option and Control from a USB or Bluetooth keyboard never reached an event, and GetKeys saw
   every key in the wrong place. Fixed in both places, and the Option-click then worked.

40. **USB keyboards and mice on the Pi 3 (18 November).** The Pi 3's USB is a Synopsys DWC2
   controller (0x3F980000) with the LAN9514 hub behind its one port, not the Pi 4's xHCI, so
   Alice68's USB board did not apply. Alice68 has a second, smaller USB host for exactly this:
   `frf_usb/native`. It brings the DWC2 up as a host, resets the root port, enumerates the root
   device and follows a hub port by port. A full- or low-speed device behind a high-speed hub
   gets split transactions. The host takes the first HID boot keyboard and the first boot mouse
   and polls their interrupt endpoints. Nothing else on the bus is touched, so keyboard and mouse
   stay apart from any later USB stack, as in Alice68. Its three files are copied verbatim into
   `src/alice/usb/`. `src/psmac/psmac_usb.c` is the Pi 3 glue:
   - the controller's power (the firmware's SET_POWER_STATE, device 3, retried while the WiFi
     holds the mailbox);
   - its registers, byte-swapped for the big-endian kernel;
   - a transport;
   - a place in core 3's loop: enumeration at start, one poll a millisecond, and a look at the
     hub's ports every two seconds. A plugged or unplugged device starts a fresh enumeration,
     with every key released first.

   Reports go where the Bluetooth host's go: `frf_native_usb_keyboard_boot_report` and
   `aga_virtual_mouse_update`, into the input ring `.PiInput` reads.

   Alice68's transport moves data through the channel FIFOs. QEMU's DWC2 model has no FIFO mode
   (it is DMA only), so the glue adds a DMA transport behind the same one-packet interface:
   - the bounce buffer goes through the VideoCore's 0xC0000000 alias, with the cache cleaned and
     invalidated around it;
   - the same start-split, complete-split and NYET steps;
   - the same return codes.

   It is the default. `usb=pio` selects Alice68's own transport, compiled unchanged under other
   names (CMakeLists.txt renames its three functions); `usb=off` leaves the controller alone.

   Two testbed findings:
   - QEMU's model reports no high-speed PHY in GHWCFG2 (the BCM2837 reports UTMI+), which Alice68's
     init refuses. The glue reads the field as the chip's value.
   - QEMU's usb-hub is full speed, so no translator is behind it. The glue sends split
     transactions only when the root port runs at high speed, as the Pi's LAN9514 does.

   With `-device usb-kbd -device usb-mouse` (QEMU_EXTRA in `tools/run-solo.sh`), the log says
   `[USB] start rc=0: 1 keyboard, 1 mouse (hub ports 8 ...)`. The monitor's `mouse_move 20 12`,
   sent eight times, moved the Mac's cursor by exactly (160, 96), and the monitor's keys quit
   PiPing, made a new folder (Command-N) and typed its name. Not yet tested on hardware: the
   split path behind the real LAN9514, and the FIFO transport.

41. **Restart, the CPU type, and sound (18 November, afternoon).** Four reports from the user's
   Pi 3:
   - *Restart from the Finder gave a Sad Mac, 0000000F 00000033* (system error 51, an
     unserviceable slot interrupt: the code first seen on the SE board, item 30's fs178). The
     68k's RESET reached Emu68's `do_reset` and the motherboard's reset line, but not the
     virtual card. `.PiInput`'s ring stayed "attached", the Ethernet driver's version stayed in
     the board header and the video driver's VBL stayed on. The first mouse movement during
     the restart raised the slot line before any driver of the new session had a handler in.
     `ps_send_reset` now calls `psmac_card_reset` (`alice_psmac.c`), which turns those three
     off and resets the network board's session, as a NuBus card's /RESET would. In QEMU, Restart
     chosen with the mouse moving continuously comes back to the Finder.
   - *Speedometer said 68020.* The IIci ROM tells a 68030 from a 68020 by writing $2809 to CACR
     and looking for the write-allocate bit (13) in what comes back (ROM 0x408007C8). A 68040's
     CACR has no such bit, so CPUFlag ($12F) said 2, and Gestalt and every program agreed.
     The PSMAC build's `check_cacr` keeps the 68030's bits readable (WA, DBE, FD, ED, IBE, FI,
     EI; the clear strobes read 0), the ROM records a 68030 and takes its 68030 paths, which
     are the ones it was written for and the ones the MMU shim already speaks. Speedometer:
     "MC68030, MC68030 MMU". The FPU it reports as absent: next.
   - *Speedometer froze at the end of its tests, Lemmings froze.* The heartbeat showed the 68k
     at interrupt level 2 in the Sound Manager's ASC feeder, which writes both FIFOs until the
     status register ($50F14804) says full. That runs from the main screen's VBL task, and the
     main screen here is our card's. The ASC stand-in (`psmac_board_se.c`) always said "half
     empty, never full", so every tick wrote the whole request, three trapped accesses per byte:
     seconds per sixtieth of a second. The FIFOs now fill with each write and drain at the chip's
     22257 samples per second, half empty below 512, full at 1024. Run ALL Tests finishes ("The
     tests are done!", `docs/img/speedometer-all-tests.png`; in QEMU, Performance Rating 198,
     65217 Dhrystones/s), Speedometer quits to the Finder.
   - *The USB keyboard needed a replug at power-on.* The replug working is the first sign
     that the split transactions behind the real LAN9514 work. The enumeration at boot caught a
     device that was not ready, and a device that fails enumeration makes no port change.
     `psmac_usb.c` now tries again (four times, six seconds apart) while a keyboard or mouse is
     missing, and enumerates again when a device's polls have all failed for two seconds.
   - *Lemmings* reaches its title screen with its music and plays its first level
     (`docs/img/lemmings-playing.png`): the freeze was the ASC.

42. **Word and self-modifying code (18 November).** The user's Word 5.1 showed the spinning
   watch for ever. The image here has Word 4.0, which stopped at once with "bad F-Line
   instruction". The kernel log had one illegal instruction first ("Undefined Line0 000e",
   vector 4 at $00EAE12E) and then F-line traps at a nonsense PC. Word's trap glue is
   self-modifying: `lea (pc,$1E),a1; move.w (a0)+,(a1)` writes a trap word into an instruction
   28 bytes ahead, which then runs in the same straight run of code. Emu68 had translated the
   run as one block while the slot still held its $0000 placeholder, so it decoded
   `0000 45EE` as an ORI with an immediate operand and then `000E`. On a 68030 it works because
   the trap dispatcher's cache flush leaves the slot uncached. The translator (PSMAC build)
   now ends a block before any address that a LEA or PEA (d16,PC) points at up to 256 bytes
   ahead, so a patched instruction is translated when execution reaches it. Word 4.0 opens a
   document and takes typing (`docs/img/word4-typing.png`). Word 5.1 is the same family; to be
   checked by the user. The main keyboard's '.' types a period in Word here. The numeric
   keypad's '.' is Word's own menu key.

43. **AppleTalk on the Pi alone, and cmdline.txt from the Mac (18 November).** The user's
   Network control panel froze the Pi. With AppleTalk switched on in the Chooser, the testbed
   stopped at "Welcome to Macintosh" instead, in LocalTalk's transmit coroutine
   (`tst.b 1546(a2); bne` until a flag the transmitter's interrupt clears). The solo board's
   SCC was a register file with no transmitter: nothing ever underran, nothing ever
   interrupted. It now has one that sends nowhere at once:
   - each data write empties the buffer and, with transmit interrupts on, sets Tx IP;
   - 50 us later the transmitter underruns and the EOM latch rises (external/status IP with
     WR15 bit 6);
   - WR0's reset commands clear them; RR3 shows them, and RR2 read through channel B gives the
     vector modified by the highest;
   - the line goes to level 4 while WR9's MIE is set;
   - in SDLC mode RR0 says "hunt" (no frame arriving).

   With nobody answering its address claims, LocalTalk takes a node and carries on. The boot
   reaches the Finder with AppleTalk active, and the Network control panel opens and lists
   LocalTalk Built-in (`docs/img/network-cp-localtalk.png`). `mac.appletalk=1` presets
   AppleTalk on in PRAM, which the solo board seeds from defaults at every power-on.

   For the WiFi menu (item 44): `PSMB_INFO_SETOPT` hands the kernel an edit list for
   cmdline.txt in Alice68's form ("-NAME", "+NAME=VALUE"). The card writer applies it:
   - only the named options change, and every other word of the line is kept;
   - quoted values stay one word;
   - the result is written only if it fits the file's first cluster, then read back.

   `PSMB_INFO_OPTSTATE` reports pending, saved or failed. The edit list is not logged, since it
   carries the WiFi password.

44. **The WiFi menu (18 November).** The user asked for a WiFi menu at the right of the menu bar,
   like today's Macs: discover and join networks, and keep the choice in cmdline.txt.
   `mac/wifimenu` is an INIT in C (the mac/c kit; `tools/mac/elf2init.py` packs it, and its entry
   copies itself into the system heap and relocates there).
   - _DrawMenuBar is patched to draw a 16 x 12 fan left of the Help menu: solid arcs for the
     signal, faint ones beyond it, hollow when off.
   - Between events (jGNEFilter) it looks at unit 0 every second and redraws when the picture
     changes.
   - A press on the icon pops up a menu: the state, Turn Wi-Fi On/Off, the scanned networks
     (signal dots, "(open)", a tick on the one in use), Look for Networks, Disconnect, and Forget
     the Network Saved on the Pi.
   - A secured network asks for its password in a dialog with "Remember this network", ticked by
     default. The join goes through unit 0's command box as PiWiFi's does. Once the unit reports
     CONNECTED, `PSMB_INFO_SETOPT` writes `wifi.ssid=` / `wifi.psk=` into cmdline.txt (item 43).

   The first version patched _MenuSelect and answered the press there without calling the real
   MenuSelect. The Process Manager, whose own MenuSelect patch sits above an INIT's, then worked
   on menu state nobody had set up. It wrote 32 bytes over low memory (exception vectors $48-$60,
   and the OS trap table at $76A-$789, the _PPC trap among them), and the Finder died later with
   "bad F-Line instruction" when the Edition Manager's idle filter called _PPC. Diffing low
   memory around one press, then probing the trap entry after each step of the handler, showed
   the damage came after the handler returned. The press is now taken from the event stream
   instead: the filter turns it into a null event and returns false, and the Menu Manager never
   sees it.

   Tested in QEMU with `mac.wifisim=` and an SD card image (`CARD=`): the scan list, the password
   dialog, the join, a solid icon once connected, and cmdline.txt on the card, one line, the old
   options kept and `wifi.ssid="Aurora68" wifi.psk="pistorm68"` added
   (`docs/img/wifi-menu.png`, `docs/img/wifi-menu-password.png`).

   Two corrections, as the user asked that Mac programs follow the Human Interface Guidelines
   (1992):
   - The menu stood a full item below the menu bar. PopUpMenuSelect's MDEF found the frame of a
     menu whose first item was at MBarHeight + 1 touching the bar, and moved it down by an item.
     At MBarHeight + 2 it hangs from the bar.
   - The password dialog now has the default button's heavy rounded outline (a user item). A
     ModalDialog filter makes Return and Enter Join, Escape and Command-period Cancel, with the
     button flashing. The filter shows a bullet for each key typed and keeps the password itself.

45. **mDNS on the Mac, and PC keyboards (18 November).** The user's `ping macse.local` got no
   answer. Riptide's mDNS responder starts off, and the Amiga glue turns it on, but the Mac
   driver never did. Riptide now turns it on unless riptide.conf says `mdns off` (riptide
   a6ca4e4), and without a `hostname` line it takes the Macintosh Name from Sharing Setup
   ('STR ' -16413), "macintosh" when that is empty. `tools/pinet.py` learned `PINET_MDNS=NAME`:
   an mDNS query every ten seconds from the Mac's own subnet. With the Riptide image it logs
   `mDNS answer from 192.168.0.77: macintosh.local A 192.168.0.77`. For `macse.local`, put
   `hostname macse` in riptide.conf or name the Mac in Sharing Setup.
   PC keyboards: Apple's mapping stays the default (Windows = Command, Alt = Option);
   `mac.altcmd=1` swaps them by position.
46. **"No FPU" (18 November).** Speedometer said "No FPU", and so did Gestalt for every program.
   The FPU was found at start-up: HWCfgFlags ($B22) read $FC00 in the testbed, bit 12 (hwCbFPU)
   set. What failed is Gestalt's 'fpu ' (IIci ROM 0x40816600): it does an FSAVE and reads the
   frame's first word, $1F18 or $3F18 a 68881, $1F38 or $3F38 a 68882, anything else
   gestaltUnknownErr (-5550). Emu68 writes the 68040's idle frame, $41000000. With the CPU
   presented as a 68030 since item 41, a 68882 is what the ROM expects beside it, so the PSMAC
   build's FSAVE now writes a 68882 idle frame: $1F380000 and 56 bytes (`M68k_LINEF.c`; -(An)
   gets all 60 bytes, as every FSAVE in the ROM and System uses it, other modes the format long).
   FRESTORE (An)+ steps over a frame by its size byte (upstream knew only the 68881's $18).
   Speedometer's System Information: "FPU: MC68882" (`docs/img/speedometer-fpu-68882.png`).

47. **AppleTalk over Ethernet, and an AppleShare server on the wire (18 November).** The user
   wanted AppleTalk over Ethernet only, and asked whether Lagoon (the user's network-share
   program, which has a Mac port) could bring its own EtherTalk, since System 7.1 has none.
   Lagoon now does (its `src/mac/ethertalk`, an 'adev'/'atlk' pair called "Lagoon EtherTalk"),
   and the testbed was taught to talk back:
   - `mac.xpram=ADDR:HEX[,ADDR:HEX]` on the solo board presets extended PRAM. The AppleTalk
     connection is the long at $E0 (the Network control panel and the LAP Manager read and
     write it there): `mac.xpram=e0:098100a8` means slot 9, sResource $81 (the Ethernet unit),
     connection 168 (Lagoon's), so AppleTalk starts on it at boot without a trip through the
     control panel.
   - `tools/pinet.py` names AppleTalk Phase 2 frames (802.3 with 802.2 SNAP): AARP requests,
     responses and probes with their addresses, DDP headers, NBP lookups with their tuples,
     ZIP, RTMP. With `PINET_ATALK=NAME` it is a node as well, number 104 on whatever network the
     Mac uses: it answers AARP for that address, NBP lookups for NAME:AFPServer, ASP GetStatus
     (a server information block: AFP 2.1, guest access only), and sends the Mac an AppleTalk
     Echo request every ten seconds.

   What the runs showed, in order. The first boot with the connection in PRAM loaded Lagoon's
   'atlk' and opened `.ENET`, but .MPP wrote short-header datagrams to node $FF only: it had
   taken the link for a non-extended one, because the 'atlk' compared the whole of D0 when .MPP
   sets only its low word. With that fixed, the log showed nine AARP probes for 65375.1 (a node
   in the startup range), ZIP GetNetInfo with nobody answering, and NBP confirmations of the
   Mac's own name. Choosing LocalTalk in the Network control panel and then Lagoon again went
   through AClose, AShutDown, AInstall, AOpen and new probes.

   Datagrams going up took three more steps. At first .MPP discarded every one at once. A dump
   of the LAP Manager's resident code showed why: its LRdDispatch is a jump through 1052(A2)
   into .MPP, and on an extended network that routine compares the destination in the Read
   Header Area before reading anything, so it wants the whole 13-byte DDP header there, not
   LocalTalk's five bytes. With the header in place the Chooser's NBP replies reached their
   socket but listed nothing, and the Echo replies went to 65496.0:0. .MPP's echo responder keeps
   its place in D2 across ReadRest, and the 'atlk's ReadRest had used D2 as a counter. After that
   the Echo replies came back unicast to the peer's Ethernet address, and the Chooser's
   AppleShare panel listed "Lagoon Test". Choosing it sent ASP GetStatus, and the AppleShare
   client showed its login dialog ("Connect ... as: Guest"). The LAP Manager's 802.2 dispatcher
   leaves the frame's header behind A3 with the source address at -16(A3), which the 'atlk'
   now uses to learn its neighbours' addresses. Logging in is next: the peer does not answer
   ASP OpenSession yet.

48. **Speedometer locked up again: a stale interrupt level on the solo board (18-19 November).**
   The user's Pi 3B on the bench (solo board) "locked up again" in Speedometer, then the FPU tests
   "seem to have locked up the system although mouse still moves", and later "came back". (The card
   first held release 0.7, from before item 41's fixes; the later reports are from 0.10.)
   What the testbed showed, with the 0.10 kernel and the image item 41 was proved on:
   - Run ALL Tests ended in the colour benchmarks with "Sorry, a system error occurred. Finder: bad
     F-Line instruction". The log had an illegal instruction ($000D) at $01FD91C2, then a jump to
     $81000040. Before the crash the colour tests had slowed to two and three times their usual
     times.
   - A second run, the colour benchmarks alone, finished, and the Mac then froze with the pointer
     still moving. The heartbeat had the 68k in a loop at $6062E (a word-break routine, the byte
     loop `move.b (a2)+,d0; jsr $40822850; dbeq d1`) with A2 = $66625C18, a pointer nobody
     would make.

   The words at $01FD91C2 were not code but stack, the same 44 bytes over and over: a level-1
   interrupt frame (SR $2000, PC $000245E4, vector $64), the handler's MOVEM of D0-D3/A0-A3, and a
   return address into the handler ($40809B84). After the freeze a dump of the whole 32 MB (QEMU's
   `pmemsave`) found 1,595 such frames in a row below Speedometer's stack, 70 KB of them, reaching
   down into its heap: that was A2.

   The ROM's level-1 and level-2 handler ($40809B60), on its way back to level 0 with a deferred
   task queued, calls the Deferred Task Manager through $6E4. System 7.1 patches that vector: the
   patch (here at $245DA) calls the ROM's dispatcher and then does `andi #$F8FF,sr; rts`, so the
   interrupt mask is cleared two instructions before the handler's own MOVEM and RTE. On a 68030 an
   interrupt taken there is a new one, and rare. Here it was the old one. The solo board's VIA, SCC,
   SCSI chip and RBV are modelled in the kernel, and the level the JIT sees (INTF.IPL) was worked out
   again only by the service core's poll, once a millisecond. After the handler had cleared the VIA's
   flag the level stayed up for up to a millisecond; the ANDI let it in again; the nested handler
   found nothing to do, called the Deferred Task Manager again (busy, so it returned at once),
   cleared the mask again, and so on, 44 bytes deeper each time, until the poll caught up or the
   stack ran out.

   The fix (`psmac_board_solo.c`): every access to a modelled chip sets the level the chips now drive
   before it returns, as a real VIA drops its interrupt output the moment its flag is cleared. The
   RBV's accesses (`psmac_board_se.c`) do the same. The access and the service core's poll both do it
   under the board's lock, so neither can store a level the other has just made stale. The IIci and
   SE boards were not affected: there each bus cycle's reply carries the motherboard's level.

   Afterwards, in the solo testbed:
   - Run ALL Tests finishes ("The tests are done!") on item 41's image
     (`docs/img/speedometer-all-tests-fpu.png`) and on the full test image with every extension,
     the network attached and the WiFi module on (`docs/img/speedometer-all-tests-full-image.png`).
     Speedometer then quits to the desktop.
   - A dump of memory after Run ALL Tests on the full image holds no nested frames at all.

   The FPU tests, and the zeros. Speedometer's FPU tests use the 68882 directly (FADD, FMUL, FSIN,
   FCOS, FATAN, FETOX, FLOGN, FSQRT, FMOVEM), and every one of them runs on Emu68's native path: no
   "not implemented" lines in any run. The three FPU tests take a few seconds in QEMU
   (`docs/img/speedometer-fpu-tests.png`), and 100 iterations of each about 20 seconds, most of it
   Speedometer drawing between iterations. Speedometer times each iteration with Ticks ($16A,
   sixtieths of a second), as all its tests: the tick count before, the test, the tick count after,
   the differences added up and divided by the iterations in whole ticks. `mac.peek=16a,4`
   showed Ticks advancing at 60 a second all through the tests. In QEMU the FPU Fast Fourier takes
   one tick or none (0.017 or 0.000 s) and the FPU Whetstone one or two. A test that finishes
   within one tick measures as no time at all: Speedometer then shows 0.000, or, where it divides
   by that time, 2147483.647 (2^31/1000), the figure the user saw for the FPU Fast Fourier. On the Pi,
   faster than QEMU, more tests fall under one tick. The figures are Speedometer's resolution, not a
   timer that stopped; the 68030 Macs Speedometer was written for took seconds over the same work.
   More iterations do not help: with 100 of each, the Fast Fourier still read 0.000 and the
   Whetstone 2147483.647, because the sum of whole ticks is divided by the iterations in whole
   ticks.

   The processor, checked while Speedometer was up: CPUFlag ($12F) is 3 (68030); MMUType ($CB1) is
   4, which the ROM's numbering makes Gestalt's 'mmu ' 3, a 68030 MMU (Speedometer's own glue for
   older Systems subtracts one the same way); HWCfgFlags ($B22) $FC00 has the FPU bit. Speedometer:
   "MC68030, MC68882, MC68030 MMU". The user's "020" came from 0.7.

49. **Both network cards on one slot-9 card: the Pi 3's wired port, Riptide on two interfaces
   (19 November).** The baseline first, confirmed by the user on the real Pi 3B (solo board, a
   test build of e5ff17d): 1920 by 1200 works, and the WiFi works on the Mac with the wired card
   ($81) hidden. The user then asked for both: "get both wifi and ethernet going and have these
   both active as usable interfaces", on a Pi 3B, a 3B+ and a Pi 4 from one SD card.

   *The Pi 3's wired port.* A Pi 3B's RJ45 socket is a LAN9514: a USB hub (the one the HID host
   already enumerates, address 1) whose port 1 is an Ethernet function with the LAN9512's register
   set (USB 0424:ec00). A 3B+ has a LAN7515: a hub, a second hub on its port 1, and the LAN7800
   (0424:7800) on that one's port 1. `src/psmac/psmac_usbnet.c` (new, MPL-2.0, written from the
   Microchip datasheets; Linux's smsc95xx and lan78xx were read for register names and nothing
   copied) drives both, plus a USB CDC-ECM device, which is what QEMU's `usb-net` is:
   - It shares the DWC2 with the keyboard and mouse. Channel 0 stays psmac_usb.c's; the chips'
     registers (vendor requests $A0/$A1) go through the same `frf_dwc2_pio_packet` from the same
     core-3 loop. Bulk data uses channels 1 (in) and 2 (out) by DMA, started on one turn of the
     loop and looked at on a later one, so nothing waits on the bus. The chips are set to answer an
     empty bulk-in with a zero-length packet (HW_CFG/USB_CFG0 BIR clear), one frame per transfer
     (MEF and burst cap off); the receive buffer is 16 KB and the parser takes several frames per
     transfer anyway (`usbnet_frame.c`, portable, 53 native checks in `make -C mac check`:
     framing of both chips, the multicast hash bits checked against an independent CRC, the PHY
     decoding, the filter).
   - Every wait (lite reset, the PHY's reset, autonegotiation) is a state with a deadline; the
     link is polled every 500 ms with BMSR read in two halves on consecutive turns, so a turn does
     one control transfer for it. When the frames for the Mac's 16 receive slots back up, the chip
     is simply not read until a slot is free (its FIFO holds the rest), as GENET's ring does.
   - A fresh HID enumeration resets the root port and the chip with it: psmac_usb.c now calls
     `psmac_usbnet_bus_reset` first and moves a bus generation on afterwards; the chip is found
     and set up again, and the Mac sees the link go down and up.
   - The multicast filter: the board protocol carries only "promiscuous" and "all multicast"
     (the Mac drivers ask for all multicast when any group is registered), so the hash tables are
     all ones or empty; the hash code exists for when the protocol learns the list.
   - MAC address: the Pi's own, from the firmware (GET_BOARD_MAC_ADDRESS), else the device tree.
   The board decides, from the device tree before the 68k starts (`psmac_board_wired`: ethernet0's
   compatible, else the model): GENET (Pi 4, 400, CM4) stays alice_net.c's, the LAN9514 and LAN7800
   are this file's, the 3A+ and Zero 2 W have none, and only there (or with `eth=off`) is $81 left
   out of the declaration ROM; e5ff17d's hiding is the exception now. A chip that is then not found
   makes unit 1 report kind 0, so the Mac's wired driver fails its Open as with `eth=off`. The 3B+'s
   inner hub is enumerated by this file (address 20), since Alice68's HID host goes one hub deep;
   keyboards on that inner hub's other ports remain unseen, as before.

   *Two drivers, one interrupt flag.* Both network drivers share slot 9's interrupt and the
   network board's single irq flag. Each driver's slot handler cleared the flag and answered "mine"
   if it had been set, so with both open the wired driver took the WiFi's interrupts and the WiFi's
   frames waited for its VBL task, a quarter of a second. mac/enet.s now claims only for frames it
   took from its own ring, or when the other unit's ring is empty (a command or a state change);
   with frames waiting for the other driver it puts the flag back and answers 0. It also answers
   csLinkStatus (230, its own Control call: link, speed, 'A68L') for Riptide's failover.
   `tools/mac/check_offsets.py` now skips the WiFi branch of enet.s, whose unit-0 offsets had made
   `make -C mac check` fail since the WiFi driver was added.

   *Riptide on two interfaces* (Riptide's `mac-port` branch, its docs/MAC.md "Several cards"):
   every catNetwork/typeEtherNet sResource is listed (SGetTypeSRsrc with fAll, SReadDrvrName for its
   name) and opened with OpenSlot on its slot and id: two refnums, two protocol tables (one handler
   entry point per card in drvr.S), two rings. riptide.conf names cards by kind, id or place:
   `interface en0 device=.ENET card=ethernet`, `interface en1 device=.ENET slot=9 id=0x83`. DHCP's
   routes take the interface's metric (wired 10, WiFi 20); the core keeps the same default route
   through two interfaces; a link reported down stops an interface being used.

   *What the testbed showed.* Run 1, QEMU with a `usb-hub`, a `usb-kbd` and `usb-net` (slirp
   behind it) and the simulated WiFi on a second pinet peer (`PINET_NET=69`): "[ETH] CDC-ECM found
   at USB address 3 (0525:a4a2), bulk endpoints in 2 out 2, 64-byte packets", the keyboard polled
   beside it, and on the Mac "en0: 52:54:00:12:34:56 on .Network_EtherNet_PiStorm slot 9 id $81
   (refnum -53, metric 10)", "en1: 02:50:53:57:49:46 on .Network_WiFi_PiStorm slot 9 id $83
   (refnum -54, metric 20)", DHCP bound on both (10.0.2.15 through the USB bulk path from slirp,
   192.168.69.2 from the WiFi's peer), pings out of each, two default routes with metrics 10 and 20
   (`docs/img/system71-riptide-two-cards-usbnet.png`). The HID host's retries (no mouse) reset the
   bus twice; the first try of this run stopped QEMU itself: its DWC2 model keeps retrying a NAKed
   bulk-in after the channel is disabled and asserts when the address has gone with the reset, so
   the bus-reset hook now gives the channel one last SETUP that ends at once. After that the chip
   was found again each time. Run 2, the file bridge as the wired port and the simulated WiFi, both
   peers answering pings to anywhere (`PINET_INTERNET=1`), the wired cable pulled at 160 s and put
   back at 200 s (`mac.netdown=160,200`, new): a ping to 10.9.9.9 running throughout went out of en0,
   then from the next second out of en1 ("en0: link down (the next route takes over)", "MacTCP's
   address is now en1's"), then back out of en0 when the link returned, DHCP confirming the lease;
   of the 309 echoes 124 went wired, 40 over the WiFi and 144 wired again; one was lost, the one
   sent in the second the cable came out (`docs/img/system71-riptide-failback-wired.png`). An
   earlier run with a 100-second outage ended in a system error a moment after the link came back;
   the shorter run did not repeat it, and the cause is not known yet.

   *The WiFi's 80 ms pings.* The user's Pi 3B at 1920 by 1200 pinged its router over the WiFi in
   70 to 86 ms (IPv4 and IPv6), where a home network answers in a few. The testbed's file bridge
   answers within a tick, so the Mac's side is not it. Every echo through either unit is now timed
   in the kernel (`psmac_netprobe.c`): "[NET] echo wifi seq N: took->out .. us, out->back .. us,
   back->mac .. us", and every heartbeat says which part of the service loop's turn took longest
   ("[PSMAC]   service loop: N turns/5 s, longest: mailbox, card, network, USB, display"). The likely
   culprit is the loop itself: the WiFi thread runs once per turn and yields during every SDIO wait
   longer than 200 microseconds and while it wakes a sleeping chip (2.5 ms), and at 1920 by 1200 in
   millions of colours one turn converted the whole picture, 9 MB read, written and flushed, some
   20 ms, on every other 60 Hz tick. The picture is now converted a band of 128 K pixels per turn (a
   millisecond or so on a Pi 3), still at most 30 frames a second. In QEMU the probe showed the
   kernel's part of an echo at 0.5 to 12 ms (TCG timing) on both paths. The real numbers can only
   come from the Pi: the [NET] echo lines and the service-loop line.

   *One card for every board.* The kernel takes the WiFi firmware by the chip's id (and, new, the
   Zero 2 W's own 43436 files by the board) and the Bluetooth firmware by the board (BCM43430A1 on a
   3B, BCM43430B0 on a Zero 2 W, BCM4345C5 on a 400 or CM4, else BCM4345C0), so one container serves
   all of them; `tools/get-firmware.sh` (shipped in the release) fetches the Pi's boot files and
   every board's WiFi and Bluetooth firmware at pinned commits, checks each SHA-256, and prints the
   make-container.py line. The card writer stays Pi 3 only (a Pi 4 writes nothing back, by design
   until EMMC2 is supported), and Bluetooth pairings do not move between boards.

   *Not verified:* the LAN9514 and LAN7800 themselves, on a real Pi 3B and 3B+ (QEMU has neither:
   its raspi3b has no LAN9514, and it has no Pi 4 machine, so the GENET-plus-WiFi Pi 4 was reviewed
   and built, not run); the lan78xx path has not met hardware or an emulation at all. The log lines
   to look for on the Pi: "[ETH] wired port: LAN9514 (smsc95xx) on the USB bus; MAC ...", "[ETH]
   LAN9514 found at USB address 2 (0424:ec00), chip id ec00 rev ...", "[ETH] link up, 100 Mbit/s full
   duplex" (or "no link: is the cable in?"), and every 15 s "[ETH]   LAN9514: running, link 100 full,
   rx N frames ... tx N ...".

   *First results on the Pi, and the fixed delays (19 November, evening).* The user's Pi 3B (test
   set 3, 1df6f2a, solo board, 1920 by 1200 in millions of colours): **the LAN9514 wired port
   works**, the first real proof of `psmac_usbnet.c`. A ping took about 20 ms over the wire and
   still about 80 over the WiFi, to the router and to hosts on the Internet alike: the delay is all
   inside the Pi and the Mac. In the testbed, with the host peer made instant (pinet slept 10 ms per
   loop, which had been most of a testbed ping; it now polls every half millisecond while frames
   flow), the kernel's part of an echo (the Mac's frame taken from the ring, sent, the reply back,
   taken by the Mac) was 0.4 to 2.4 ms on both paths, under QEMU's emulated cores. The Riptide
   Terminal's ping showed the rest: with the Riptide of the afternoon the Mac's side of each echo
   was either about 5 ms or about 16 ms (5.1, 6.0, 7.0, 15.7, 16.5, 16.2, 22.4 ...), the second
   group one tick. A program waiting in Riptide (ping in recv) sits in WaitNextEvent with a sleep of
   one tick; the reply set its sleeper's flag but nothing woke the process, so it ran at the next
   tick. Riptide now calls WakeUpProcess for a waiting program's process when it wakes the sleeper
   (allowed at interrupt time), and the Mac's side became 4.0 to 8.2 ms for most echoes (outliers
   to 19 ms remain, other processes' turns), the floor being QEMU's slow emulation of the 68k; on
   the Pi it should be well under a millisecond. That accounts for most of the wired 20 ms. For the
   WiFi's extra 60 ms the likeliest cause is the radio's power saving: the kernel sets PM 0 before
   joining, but the firmware may take up its own on associating (Linux sets it again after every
   connect), and with PM_FAST the radio dozes between beacons once idle, the access point holding
   each reply until the next beacon. The kernel now reads PM back after the join, sets it to 0 if
   it is not, and logs `[WIFI] power save: PM N after joining ...`. Neither change is proved on the
   Pi yet; the `[NET] echo` lines (out->back is the chip, the air and the router; took->out and
   back->mac the kernel) and the service-loop line will say. The card loader of 1df6f2a reads four
   sectors per turn of the loop (about 170 us); the service-loop line's "card" figure is its share.

50. **Booting fast: the Pi as the SE's processor (19 November).** The user: "when this is dropped
   into a Mac SE with this acting as the CPU it needs to boot super fast to be ready for the Mac."
   From the moment the SE is switched on nothing happens on it until the Pi's firmware has read its
   files, the kernel has set up and the 68k is released, and then the IIci ROM does its own start.
   Nobody had timed any of it.

   *The clock.* `psmac_boot.c` prints one `[BOOT] t=N.NNNs what` line per phase. Each mark reads the
   ARM's generic timer (one instruction, any core); the first time the peripherals are mapped the
   generic timer is tied to the BCM2837's 1 MHz system timer, which the SoC's reset starts and the
   firmware never resets, so on real hardware t counts from power-on and the first line, "kernel
   entry", is the firmware's whole part (boot ROM, bootcode.bin, start.elf, config.txt, the device
   tree, kernel8.img and the initramfs read from the card). Under QEMU the system timer starts with
   QEMU and that line says nothing. The 68k's phases come from the PC the service core already
   samples for its heartbeat: the ROM checksum ($40844690) and the RAM test ($408446BC) as spans
   (first and last sample inside the routine), and from the Mac side the first log line (a
   declaration ROM driver's Open), the first INIT's line and the Finder (CurApName, $910, read ten
   times a second). The first SCSI selection and pseudo-DMA read on the SE board, the solo board's
   first READ and PiDisk's first block are marked where they happen. The JIT does no extra work.

   *Before* (solo board under QEMU, the user's set-up: a 101 MB container with the ROM, the
   declaration ROM and a 96 MB System 7.1 image, 128 MB for the Mac): kernel entry 0.003 s; the
   kernel's copy of the initramfs done 0.84 s; cores started 1.07 s; 68k released 1.19 s; ROM
   checksum 1.22 s; RAM test 8.38-10.54 s; first SCSI read 59.2 s; first INIT 101.9 s; Finder 104.3 s.
   The heartbeats between showed where the rest went:
   - 1.2 s to 8.4 s, PC $40807116: the startup chime, a wavetable loop of 30000 steps of 14 VIA reads
     each, timed by the reads themselves. Every VIA read is a trapped access to the kernel's VIA,
     17 us under QEMU, about 1 us on a Pi 3 and 2 to 3 us through the SE's real VIA. Nothing plays
     the ASC on an SE or a bare Pi: seven seconds of silence in QEMU, half a second to a second on
     hardware.
   - 8.4 s to 10.5 s, the RAM test: moving inversions over 128 MB. It tests the Pi's memory.
   - 14 s to 59 s and again to 100 s, PC $4080786A: the SCSI Manager's selection loop, waiting for
     BSY from six empty SCSI IDs. Its timeout is 250 ms ($62 of its globals, set at $40809554)
     times TimeSCSIDB bus accesses, twice per boot (the Start Manager's scan and the System's).
     About three seconds on hardware with one disk.
   - Three heartbeats at PC $40807A5A: the half second the SCSI Manager waits after resetting the
     bus, counted in TimeDBRA turns of a self-looping dbf. The kernel had given TimeDBRA as 0x7FFF
     (item 14), but since item 27 every such turn reads the system timer once, so the half second
     was 16 million reads: 23 s under QEMU, 1.5 to 3 s on a Pi 3.
   And before the kernel at all, on the hardware: the firmware reads the whole container from the
   card, at 10 to 20 MB/s on a Pi 3 (the firmware's SD speed is documented nowhere; the range is
   that of the card in 4-bit mode at 25 to 50 MHz), five to ten seconds for 101 MB; kernel8.img is
   5.3 MB more. The kernel then copied the 99 MB to the top of RAM (0.84 s under QEMU, about a
   tenth of a second on a Pi 3). And it printed 7 KB of log before the 68k started, at 115200 baud:
   0.6 s on hardware, spent waiting on the UART's FIFO.

   *What changed:*
   - **Disk images on the card** (`psmac_card.c`). An image can be a file of its own in the card's
     root (`MyMac.hda`, any `*.hda`, `*.img`, `*.image`, `*.hfv`, `*.dsk` over a megabyte, not
     `kernel*.img` or macOS's `._` files, or `mac.image=NAME`). Core 3 finds it at the card's first
     look, takes the memory from the kernel's free list (block 0, which psmac_setup_memory carved
     the Mac's RAM from), registers it as the solo board's next SCSI disk or a PiDisk volume, and
     reads it in 16 KB chunks, four sectors per pass of core 3's loop (CMD18, stopped by CMD12:
     about 170 us at 25 MHz, inside the 32-byte FIFO of the Bluetooth UART beside it). A chunk the
     Mac needs first is read first (`psmac_card_need`): from core 0 (the solo board's SCSI) core 3 is
     asked and the access waits, as the Mac would on a disk; from core 3 (PiDisk) it is read there
     and then. Writes need their chunk in memory first, so the loader never overwrites them, and go
     back into the same file through its own extents. The solo board's first SCSI selection waits
     for the card's first look when it has no disk yet. An image in the container still wins.
     The container is then the ROM, the declaration ROM and the WiFi/Bluetooth firmware, about
     1 MB: the firmware's five to ten seconds become a few hundredths.
   - **The ROM patch** (`rom/patches/iici.json`, so also the kernel's in-memory delta and
     make-container.py's): StartTest's call of the RAM test becomes a branch to its return point
     (sizing is a separate routine, $408431C0, and runs as before; the warm-start path of every
     Restart never ran the test; no later test reads what it leaves); the chime's outer count is 1
     instead of 30000; the selection timeout is 10 ms instead of 250 (a SCSI target must answer
     its selection within 200 us, the standard's selection abort time).
   - **TimeDBRA measured** (`psmac_mbox.c`): 4096 reads of the system timer at start give the dbf
     turns per millisecond, so the ROM's TimeDBRA delays last what they say.
   - **The log off the critical path** (`psmac_bus.c`): once the service core's loop runs, kprintf
     puts characters into a 16 KB ring that the loop sends whenever the UART has room; a full ring is
     sent from by the writer, and before the loop (or with `mac.synclog=1`) every character goes out
     at once as before, so a kernel that dies early still says where. A lock keeps the sender and
     Bluetooth's move of the log to the mini UART apart. The service core's heartbeat no longer
     stops the bus for a sixth of a second every five.
   - **The USB start on core 1** (`psmac_usb.c`, `alice_psmac.c`): the controller's power, reset and
     first enumeration, about a second on hardware, held core 3's mailbox, card and display services
     before their loop began. Core 1 had nothing to do.
   - **The ROM's first RESET** no longer asks the firmware for the VideoCore buffer psmac_display_init
     made a moment before and clears 9 MB again (`psmac_display.c`), and on the SE board the RESET
     instruction holds the motherboard's reset line 1 ms and waits 1 ms, not 30 and 10
     (`psmac_gpio.c`; a 68000 holds it 16 us; the power-on pulse keeps its timing).
   - **PiDisk's volume** is announced 5 s after the driver's Open and every 2 s until mounted, not
     first after 60 s (`mac/pidisk.s`), and logs only its first post.
   - **The firmware's part:** `boot_delay=0` in config.txt (some firmware waits a second by
     default; upstream Emu68's PiStorm config.txt says the same), and kernel8.img ships
     gzip-compressed, 1 MB instead of 5.3, which the firmware inflates itself (the config.txt
     documentation: a 64-bit kernel may be a gzip archive, recognised by its first bytes; upstream
     Emu68 ships Emu68-pistorm.gz so to every Pi 3 PiStorm). `disable_splash=1` and, in the SE,
     `gpu_mem=16` with start_cd.elf (the cut-down firmware, a third of the size) are offered as
     options: the first hides the rainbow square that tells a dead card from a dead board (the
     reason the template keeps it), the second does not leave room for the large HDMI screens.
     None of the firmware settings was measured on hardware.

   Not changed, on purpose: Lagoon EtherTalk's ten AARP probes at a fifth of a second (two seconds
   at INIT time when AppleTalk is on Ethernet) are AppleTalk's own rule for claiming an address;
   Riptide's INIT does not wait for DHCP. The kernel's copy of a large container stays: with the
   image on the card there is nothing large to copy. The SCSI bus reset's half second is the
   ROM's and the SE's disks may need it.

   *After*, the same timeline (QEMU, so the absolute times are QEMU's; TCG makes every trapped
   access 10 to 20 times dearer than on a Pi 3):

   | Phase | QEMU before | QEMU after, image in the container (1) | QEMU after, image on the card (2) | Pi 3B before (estimate) | Pi 3B after (estimate) |
   |---|---|---|---|---|---|
   | power-on to kernel entry (firmware) | not modelled | not modelled | not modelled | 7-14 s | 1.6-2.7 s |
   | kernel entry to 68k released | 1.19 s | 1.34 s | 0.36 s | 0.8-1 s | 0.2-0.3 s |
   | 68k released to the first read of the boot disk | 58 s | 23.6 s | 7.0 s | 5-8 s | 0.6-1 s |
   | the Finder (t) | 104.3 s | 48.4 s | 36.8 s | | about 15 s sooner than before |
   | SE board (MAME), first block from the SE's disk | 109.5 s | 35.1 s | | | |
   | SE board (MAME), the Finder | not reached in 300 s | 246.5 s | | | |

   (1) without the measured TimeDBRA: the SCSI Manager's half second after the bus reset still took
   about 20 s of QEMU's time there. (2) the final kernel, TimeDBRA measured (435 turns per ms
   under QEMU, from 0x7FFF); the same run without it took 30.9 s to the first read and 59.3 s to the
   Finder. The Pi 3B column is arithmetic, not measurement. Firmware: bootcode.bin and start.elf about
   1.5 to 2.5 s (a published Pi 3 measurement has 1.25 s to start.elf), plus a second where the
   firmware's boot_delay default is 1, plus kernel8.img (5.3 MB, 0.3-0.5 s at 10-20 MB/s; 1 MB
   gzipped and inflated by the firmware, about 0.1-0.2 s) plus the container (101 MB, 5-10 s; 0.5
   to 1 MB, a few hundredths). Kernel: the 99 MB copy (0.1 s), the log before the service core
   runs (7 KB at 115200 baud, 0.6 s, of which 2.3 KB still goes out synchronously, before core 2
   starts), the VideoCore mailbox. ROM: the chime (420,000 VIA reads, 0.4-1 s), the RAM test of
   128 MB (0.3-0.5 s), two scans of six empty SCSI IDs at 250 ms (3 s), the bus reset's half second
   at 0x7FFF reads per ms (1.5-3 s). With the image on the card the boot disk's blocks come from
   the card while the Mac asks for them, at the card's speed rather than RAM's; QEMU's SD model is
   far slower than a card (0.5 MB/s), and the card's speed on a real Pi 3 is what the user's [BOOT]
   lines will show. The target of under three seconds from power-on to the 68k's first instruction looks
   reachable with the container at a megabyte; that is for the user's Pi to confirm, with the
   first line's t.

   To do on the hardware: the [BOOT] lines from a Pi 3B with the small container and the image on
   the card; whether `gpu_mem=16` with start_cd.elf keeps the SE's HDMI picture; the card loader's
   speed at 25 MHz (50 MHz high speed is a later step if it is the bottleneck).

51. **Parameter RAM kept on the card (19 November).** The user set AppleTalk active in the
   Chooser and chose Lagoon EtherTalk in the Network control panel on the bench (solo board); a
   power-off would have lost both. A real Mac keeps its parameter RAM in a chip with a battery; the
   solo board's clock chip (`psmac_board_solo.c`) started from `psmac_pram_default.h` at every
   power-on. Now the card writer keeps it in `pram.bin` in the card's root, the way it keeps
   `pairings.txt` (`psmac_card.c`): 256 bytes of parameter RAM and then the clock, four bytes of the
   Mac's seconds since 1904, in one cluster created the first time it is written.
   - *Reading.* The card's first look on core 3 reads it. The chip's first command (the ROM's first
     PRAM access, a few milliseconds after the 68k starts now that the RAM test and the chime are
     gone) waits for that look, at most three seconds, then takes the 256 bytes over the defaults,
     the clock if it is later than the default's 17 November, and applies `mac.appletalk=` and
     `mac.xpram=` on top, as before.
   - *Writing.* Every write the chip takes that changes a byte counts as a change. Core 3 hands the
     256 bytes and the clock to the card a second after the last change, and only once the boot is
     over (the Finder seen in CurApName, or three minutes): the ROM and the System write parameter
     RAM while they start, and a power cut in a half-written file should not be the price of a
     boot. `mac.card=ro` and `off` stop the writing (`ro` still reads it).
   - *The clock* goes on from the last save, which is better than 17 November every time but still
     stands still while the Pi is off.

   Solo testbed with the card image (QEMU, `CARD=`): the first boot logs "pram.bin: not on the card
   yet" and "parameter RAM: the defaults", then after the Finder "pram.bin: parameter RAM saved": the
   Mac had changed $01 and $59 and written slot 9's parameter RAM at $46-$48 ('PS', $80). The next
   boot from the same card logs "pram.bin: read" and "parameter RAM from pram.bin on the card (260
   bytes)" before the ROM's first PRAM read.

52. **More than one disk on the Pi alone (19 November).** The user put a second image beside the
   System 7.1 image in the container (RiptideFix.img: 2 MB, Apple-partitioned, the full image's
   Iomega driver, a 4096-block HFS partition) and it never appeared on the desktop. The image is
   sound (hfsutils mounts it; its driver descriptor, map and driver match the full image's byte
   for byte). The new heartbeat line (every disk's counters and the selections per SCSI ID) and a
   command trace showed what happened to it as SCSI ID 1. The ROM selects it, reads its driver
   descriptor and loads the Iomega driver from its driver partition. The driver asks INQUIRY, START
   UNIT, TEST UNIT READY, READ CAPACITY, MODE SENSE and Iomega's vendor command $06 ("non-sense",
   page 2, 22 bytes; the target answered CHECK CONDITION), reads the partition map (blocks 0-4),
   and stops. Nothing ever reads the HFS volume, and the drive is never polled again. For ID 0
   the ROM mounts the startup volume itself, so the same driver looked fine there.
   - The solo target now answers $06 as a Zip drive does: zeros, which the driver reads as "no
     protection" (it takes the protection mode from byte 21 of the 22-byte page and a flag from
     byte 62 of the 63-byte one; found in the driver's code at offsets $BC82 and $AEDE of its
     partition). That changed nothing for ID 1: the driver still never mounts a second drive.
     The answer stays (harmless, and right for the driver), but extra disks no longer depend on it.
   - **The solo board's first disk image is SCSI ID 0, every further one a PiDisk volume**
     (`psmac_board_solo.c`, `psmac_mbox.c`). PiDisk is the slot-9 card's own driver, in the
     declaration ROM: it announces each volume until the File Manager reads it, so it mounts on
     any System with any image. An Apple-partitioned image is served as its first Apple_HFS
     partition (a bare HFS image as it is), on every board. `mac.disks=scsi` keeps every image a
     SCSI disk, as before, for images whose own driver mounts them.
   - **Several images on the card** (`psmac_card.c`): every disk image file in the card's root, in
     directory order (the order they were copied there), up to four, or the names in
     `mac.image=A.hda,B.img`.
   - **A bug that came with it:** PiDisk's write path for an image held in memory had the copy's
     source and destination swapped, so the first write to such a volume stopped core 3 with a store
     to address 0 (and with it the mailbox, the card and the display's VBL). The Finder writes its
     desktop database to a new volume at once, which is how the testbed found it. On the SE board a
     PiDisk volume from the container would have done the same at the first write.

   - **PiDisk waits longer for a block** (`mac/pidisk.s`: 100,000,000 turns instead of 2,000,000,
     well under a second at JIT speed). With both images on the card the volume's first block was
     still coming in from the card when the Finder asked for it; the driver gave up after 2-5 ms,
     tried twice more and said "This disk is unreadable". The loader now also reads the smallest
     image first (the startup disk's blocks still come first, as the Mac asks for them), so a small
     second disk is in memory before the Finder looks.

   Solo testbed with sys710pi-aurora-full.image and RiptideFix.img, both in the container and both as
   files on the card (with the declaration ROM rebuilt for the new PiDisk): "its Apple_HFS
   partition: blocks 491-4586", the Finder at 22 s (container) and 36 s (card, QEMU's slow SD
   model), and "Riptide Fix" on the desktop beside the startup disk in both
   (`docs/img/solo-second-disk-from-card.png`); the Finder's desktop database written to it goes
   back into RiptideFix.img on the card.

53. **A crash record for every bomb; Lagoon's "bad F-Line" was the stale interrupt level (19
   November).** The user's Internet Explorer 4.01, left idle for an hour on the bench (solo board,
   kernel bc44052, Riptide over the WiFi), came back to "Sorry, a system error occurred. 'Internet
   Explorer 4.01' bad F-Line instruction", the error seen before in long sessions (items 42 and 44,
   and Lagoon's docs). The dialog shows a number; the evidence is gone at Restart.

   The crash record (`psmac_crash.c`). Emu68 raises every exception translated code takes in
   `M68K_Exception` (`M68k_ExceptionEntry.c`); that is where it is taken, so the JIT's normal path
   is untouched.
   - *Unexpected exceptions* (bus and address errors, illegal instructions, zero divide, CHK,
     TRAPV, privilege, trace, F-line, coprocessor, format and FPU errors, uninitialised and
     spurious interrupts; not interrupts, TRAP #n or A-line). The registers, the 24 instruction
     words around the PC and the 16 longs at A7 are copied at once. The service core prints a
     block: what and where, the application (CurApName, $910) and its A5, the region the PC is in
     (ROM, the system heap, the application's heap, stack or A5 world, a card's space, other RAM),
     the registers, the code, the stack, and the last 32 PCs it sampled. At most eight blocks per
     power-on, then one-line notes.
   - *_SysError* ($A9C9), picked out of the A-line traps by its trap word (one 16-bit read per
     Toolbox call), for the errors the System or a program raises itself. "Welcome to Macintosh"
     (40) and the alerts from 20000 up are _SysError calls too and are left out. The ROM's own
     exception handlers do not go through the trap: a test program that runs an F-line opcode
     ($FE00, coprocessor 7) brought up "bad F-Line instruction" with no _SysError seen. So the
     exception's block is the record that matters.
   - The recorder is ordinary C. The exception entry is compiled to leave the 68k's registers
     (fixed ARM registers) alone, and C code may change x0-x18, so the call saves those around
     it. The one-line log it replaces called `kprintf` straight from the entry, which could
     change A0-A4 of the program that faulted. That is also why its "words at PC-16" came out as
     garbage.
   - The kernel's log outlives the Mac's restart, and PiLog shows its last 64 KB.

   Tested with that program, CrashTest, in Startup Items on the full test image:

   ```
   [CRASH] F-line (vector 02c) at PC 01f5a606 in "CrashTest", A5 01f9cdf4 (crash record 1 of at most 8)
   [CRASH] the PC is in the application's heap
   [CRASH]   D0-7 ffffffff 11111111 22222222 33333333 00000000 00000000 00000000 00000000
   [CRASH]   A0-7 0006bb44 01f58fa8 00004444 00000000 00000000 01f9cdf4 000001e4 01f9adf0
   [CRASH]   code at PC-16: 243c 2222 2222 263c 3333 3333 45f8 4444 |fe00 4e75 0000 ...
   ```

   The registers the program set (D1-D3, A2) and its two last instructions are there.

   Lagoon's "bad F-Line" in the Finder. On the 0.11 test kernel (e5ff17d, before item 48's fix)
   eight of nine solo-board runs with Lagoon installed ended in the Finder with "bad F-Line
   instruction" within one to three minutes of opening a window. The Lagoon worker's logs show the
   same signature as item 48's Speedometer crash: an illegal or CHK instruction at a PC near the
   top of the 16 MB RAM ($FD652A, $FD7698, $FD859E, $FDDB8E: the Finder's stack, not code), then a
   jump to $81000040. Rerun with their script and images on kernels with item 48's fix (42f15f2,
   and origin/main with the crash record), the disk window opened and watched for three to four
   minutes (four the last time), all three runs were clean, with no exception logged at all:
   - the full Lagoon image;
   - Lagoon EtherTalk alone;
   - the full Lagoon image again.

   It was the stale interrupt level, item 48. Whether the user's idle IE crash is the same is not
   known: their kernel already had the fix. The next one will leave a record.

   Code read from the solo board's disk. While looking for other ways to run stale code, one turned
   up. The translator re-checks RAM blocks (a CRC of their bytes) on their next entry after
   `psmac_icache_epoch` moves. That happens:
   - when the Mac clears the 68030 instruction cache (CACR CI/CEI: System 7.1's BlockMove does it,
     BlockMoveData does not; _HWPriv's FlushInstructionCache does it);
   - after a PiDisk block read;
   - after a SCSI read that goes over the wire to the motherboard.

   The solo board's 5380 is modelled in the kernel and never goes over the wire, so a code resource
   read from the boot disk into memory where other code had run relied on the System clearing the
   cache in between. The end of each data-in transfer now moves the epoch (`psmac_board_solo.c`).
   No crash was traced to this; it closes a gap the other boards never had.

54. **"Error type 41" from a disk image on the card (20 November).** The user's Pi 3B (solo board,
   1df6f2a, a 1 MB container, the 100 MB test image as a file on the card with `mac.image=`) stopped
   with "Sorry, a system error occurred. 'Finder' Error type 41" (the Finder could not be loaded).
   Restart then reached the desktop, and the next power-on did too: intermittent, and fine once the
   image was all in memory. So blocks were served before they were loaded.
   Every read of a memory image already asked for its whole range first (`psmac_card_need`, called
   by the solo board's SCSI READ and WRITE before the data phase, and by PiDisk's block service), and
   the pseudo-DMA and blind transfers only move what that range holds. The hole was in the asking.
   Core 0 (the 68k's own core, which runs the solo board's SCSI target) only posted the chunk it
   needed to core 3 and waited, and after ten seconds it gave up and served whatever was in memory,
   zeros. Core 3 runs the card between other work, and on the user's Pi the WiFi's turn (joining
   the network the cmdline names) could keep it away for that long; the testbed has no WiFi chip and
   never saw it.
   - *Reproduced* in the solo testbed with two test options: `mac.cardstall=S` keeps core 3 away from
     the card for S seconds once the image has started coming in, as the WiFi did, and
     `mac.cardslow=N` adds N microseconds per sector read, a slow card. With the old code and
     `mac.cardstall=20` the log said "block 64 not read from the card in 10 s" and the Mac crashed
     at once (an F-line trap in the system heap, in code that was zeros).
   - *Fixed* (`psmac_card.c`): the card has one owner at a time, a lock that core 3 holds for each
     pass of its card work and for PiDisk's blocks. Core 0 asks core 3 first and gives it two
     milliseconds; after that it reads the chunk itself whenever the lock is free (`chunk_load_direct`,
     the chunk in one command per extent run, after finishing any sector being programmed). It never
     gives up while the card answers; after a second it says so once ("the Mac waits for block N").
     Core 3's loader leaves a chunk alone if core 0 has read it meanwhile, so a block the Mac wrote
     after core 0 read it is not overwritten from the card; writes still need their chunk in memory
     first, as before.
   - *Proved* in the testbed, the test image with Octavo and Speedometer in Startup Items, booted from
     the card: with `mac.cardstall=20` the Finder at 29 s, Octavo drawing its first page and
     Speedometer starting; with `mac.cardslow=300` the same with the image only three-quarters read
     when the applications ran; with `mac.cardslow=1000` and `mac.cardstall=15`, and then
     `mac.cardslow=2000` on the same card (a second power-on), the Finder and Octavo again with the
     image 40 and 29 per cent read. No crash in any of the five boots, and the image on the card
     still mounts with hfsutils afterwards.
   - *Writes during loading:* the card writer's flush had waited until the whole image was in, so on
     a real card the Mac's changes could sit in memory for minutes. Core 3 now alternates a pass of
     flushing with a pass of loading; with `mac.cardslow=1000` the log read "144 KB written, 0 KB
     waiting, 40% read from the card".

   The workaround meanwhile, the image back inside the container, is safe with 1df6f2a: an image in
   the container is all in memory before the 68k starts and a card image is then not taken.

55. **The log as text: PiLog's Save Log, aurora68.log on the card (20 November).** The user reported
   problems by photographing PiLog's window, and wanted the log handed over as text. Two ways now:
   - *PiLog, File > Save Log* (mac/pilog.c): Standard File (SFPutFile, a new line in
     `mac/c/traps.txt`) and the File Manager's register calls (PBCreate, PBGetFInfo/PBSetFInfo,
     PBOpen, PBWrite, PBSetEOF, PBClose, PBFlushVol: eight small stubs in `mac/c/crt0.s`, the
     parameter block in `toolbox.h`) write a TEXT file of TeachText's. The text is the ring read
     again through the window's filter, from Clear's mark, under a first line naming the filter and
     the parts; Option takes the whole ring. TeachText refuses files over 32 KB: a whole ring
     (64 KB) is for a modern machine, reached through Lagoon's share.
   - *Save to Card*: a new INFO command, `PSMB_INFO_SAVELOG` (6), with `PSMB_INFO_LOGSTATE` (7) for
     the progress, asks the card writer for `aurora68.log` in the card's root. `psmac_log_copy`
     (psmac_bus.c) copies the ring oldest first; the card writer makes the file once, the size the
     ring needs (65 KB: 33 clusters of 2 KB on the testbed's FAT16 card), taking each new cluster
     (marked end of chain) before the chain points to it, so a power cut leaves at worst a lost
     cluster; afterwards it only rewrites the file in place. One sector per pass of core 3, as
     everything there. The first try wrote four sectors a pass: PiLog's question about the
     progress, which core 3 also answers, timed out under QEMU and PiLog said "Not saved" while the
     kernel said written. One sector a pass, and PiLog counting an unanswered question as "still
     busy" for a minute, mended it.
   - *After a crash*: psmac_crash.c asks for the same save after each `[CRASH]` record, so the
     record and what led to it are on the card before anyone switches off.
   - *Secrets*: the kernel echoes its bootargs in the "[PSMAC] options:" line, which can carry
     `wifi.psk`. PiLog (every line it reads) and the kernel (the snapshot for the card) replace the
     values of options named psk, pass, passwd, password, passphrase, key or secret (also as the end
     of a name after `_` or `-`), the password of `mac.wifisim=SSID:PASSWORD` and the key of
     `bt.dev=ADDRESS:KEY:FLAGS` with `***`; "monkey=7" and "keyboard=9" are left alone. Both
     scanners were checked on the host against the same lines.
   Testbed (solo board, QEMU, `CARD=` a 64 MB FAT16 card image): Save Log with the filter "slot"
   gave a 3.8 KB file of 37 lines, Option-Save the whole ring (64 KB, about 600 lines, in order), both
   TEXT/ttxt; Save to Card made aurora68.log (65,558 bytes), `mtype` reads it back, `fsck.vfat -n`
   is clean, and a second save rewrote it in place (`docs/img/system71-pilog-save.png`,
   `docs/img/system71-pilog-card.png`). The save after a crash record was not provoked in the
   testbed.

56. **Two disk images on the card: core 3 stopped for good (20 November).** The user's Pi 3B with
   447febf and `mac.image=aurora-test3.image,riptide-update.img`, both files on the card: the log
   ended with "[CARD] aurora-test3.image: the Mac waits for block 0 from the card" and nothing came
   after it. With only the first image named the same kernel booted. The cmdline also held a
   second `mac.image=` word, and the first one had silently won.
   - *Cause:* a self-deadlock on core 3. 447febf gave the card one owner, `card_lock`, which core 3
     holds for each pass of its card work. A second image is registered during that pass, and
     registering a PiDisk volume reads its partition map (`hfs_partition`), which asks for the
     image's first blocks through `psmac_card_need`; on core 3 that took `card_lock` again and spun
     on it forever. The Mac's own core then waited for core 3's chunk, and core 0's own reads wait
     for the lock: "waits for block 0". One image is never registered as a PiDisk volume, so it
     never got there.
   - *Fixed* (`psmac_card.c`): core 3 marks that it holds the lock for its pass, and the demand path
     on core 3 takes the lock only if it does not hold it already; either way it finishes the chunk
     in hand and then loads the one asked for before going on, so a block the Mac or PiDisk waits
     for still comes before any further background loading. PiDisk's block service runs between
     passes, so the demand path also finishes a sector the card writer has in programming before it
     reads (`flush_finish`, shared with core 0's direct reads).
   - *An option given twice* now says so (`psmac_boot.c`, from `psmac_read_options`): "[PSMAC]
     cmdline.txt gives mac.image= more than once: the first is used, mac.image=A,B; mac.image=C is
     ignored". It covers our options (mac., wifi, bt, net., usb, eth); for a name with psk, pass or
     key in it the values are not shown.
   - *Proved* in the solo testbed with a card holding the container, the 96 MB test image and two
     2 MB HFS images (`mac.image=t3.image,RiptideFix.img,Spare.img`). 447febf's card code with two
     images: the log stops after "disk image RiptideFix.img found", the same hang. The fix, six
     boots: plain with a second `mac.image=` word (the warning in the log, the Finder at 38.0 s),
     `mac.cardslow=300` (the Finder at 42.3 s, the large image 55 per cent read) and four with
     `mac.cardstall=20` (the Finder at 30.2, 42.4, 42.8 and 56.9 s); in each the Mac wrote to both
     small volumes (28 KB each in the card writer's count) and nothing bombed. Of two earlier boots
     of the fix with `mac.cardstall=20`, before `flush_finish`, one froze just after the Finder
     started: every core stopped logging, the heartbeat included, and the screen kept the grey
     desktop. It did not come back in the five boots since; QEMU's card model finishes every write
     at once, so `flush_finish` cannot be what cured it there, and the cause is not known. Watch for
     a log that simply stops.

57. **The WiFi password in the kernel's own log lines (20 November).** Item 55 hides secrets in
   PiLog and in aurora68.log, but the kernel's `[PSMAC] options:` line still printed the whole
   cmdline, `wifi.psk="..."` included, on the serial line and the HDMI screen, which users
   photograph. Every kernel line that repeats cmdline.txt now goes through the same rule
   (`psmac_redact`, a wrapper of the card log's `log_redact` in `psmac_card.c`): the options line,
   the duplicate-option warning (item 56) and the device-tree dump (`dt_dump_node`, behind PSMAC,
   which printed `/chosen/bootargs` in text and in hex). The rule gains "passkey" as a name.
   Testbed, `wifi.psk=hunter2 wifi.psk=other bt.passkey=1234 mac.wifisim=Net:secretpw` on the
   cmdline: "(bootargs: ... wifi.psk=*** wifi.psk=*** bt.passkey=*** mac.wifisim=Net:***)" and
   "cmdline.txt gives wifi.psk= more than once: the first is used, wifi.psk=***; wifi.psk=*** is
   ignored".

58. **Fifteen seconds of rainbow: the kernel waited for a serial port that does not send (20
   November).** The user's Pi 3B (447febf, one image on the card, a 1 MB container) showed the
   firmware's rainbow screen for about ten seconds, and its timeline had the 68k released at
   18.96 s: kernel entry 2.77 s (the firmware's part, fine), MMU and peripherals 3.48 s, "kernel
   moved to the top of RAM" 10.43 s (7.0 s), "cores 1-3 started" 18.40 s (8.0 s), then the ARM
   clock, the splash and the container in half a second. Neither step does anything slow: moving
   the kernel is a 64 MB copy with the caches on (KERNEL_SYS_PAGES, 32 blocks of 2 MB), and waking a core is a `sev` and a lock the core
   clears at once. What they have in common is the log. Between "MMU" and "kernel moved" the
   kernel prints about 960 characters, between "kernel moved" and "cores started" about 820, and
   7.3 and 9.8 ms per character is what `con_send`'s bounded wait costs when the UART never takes
   a byte: 100,000 reads of the PL011's flag register. On a Pi 3 with Bluetooth on the PL011 (the
   release's config.txt has no disable-bt overlay) the firmware leaves the PL011 switched off, so
   its FIFO stays full and every character waited the whole bound, until the service core's ring
   (item 50) took the log over just before the splash. Item 50's figure (half a second for 6 KB
   at 115200 baud) assumed a port that sends.
   - *Fixed* (`psmac_bus.c`): one full wait marks the UART stuck, and from then on a character goes
     to it only if it has room at that moment; the first character it takes again clears the mark.
     The service core says so once: "the serial port is not sending ...: the log is on the screen
     and in PiLog". The screen, PiLog and aurora68.log get every character either way. The mini
     UART's path is the same code.
   - *Measured* in QEMU, whose PL011 always sends, with a test option: `mac.deaduart=1` makes the
     port look full to the kernel (it still prints, so the log can be read) and `mac.deaduart=2`
     also waits at every character as the kernel used to. QEMU's register read is slower than the
     Pi's (22 ms per character), and the same two phases grew as on the user's Pi: with
     `mac.deaduart=2` MMU at 2.38 s, "kernel moved" at 25.26 s, "cores 1-3 started" at 49.58 s;
     with `mac.deaduart=1` the same phases at 0.04, 0.33 and 0.34 s and the 68k released at
     0.41 s, as with a working port (0.40 s).
   - *Expected on the Pi 3B:* the 68k about half a second after kernel entry, near 3.5 s from
     power-on instead of 19, and the Finder about 15 s earlier. The rest is the firmware (2.8 s,
     most of it reading the card) and the Mac's own start (2 s to the first SCSI read, 5 s more to
     the Finder on the user's Pi). Raising the ARM clock earlier is not worth doing now: what runs
     before it is the kernel's own setup, a few milliseconds without the waits.
   - Nothing else waits early: the kernel's `[BOOT]` marks between kernel entry and the 68k cover
     every step, and in QEMU with `mac.deaduart=1` they add up to 0.4 s.

59. **A remote control in the kernel: the Mac's screen, mouse, keyboard, log and reset over HTTP
   (20 November).** The user: an agent should be able to see and drive the Mac directly, so that
   problems can be reported and reproduced without photographs. The kernel already owns the frame
   buffer, the input ring, the log and the 68k, so the remote is a kernel service and needs nothing
   on the Mac (docs/13 is the design and the manual).
   - *Stage 1, the capabilities* (`psmac_remote.c`): a PNG of the main screen read from the Mac's own
     frame buffer at its depth through its colour table (`psmac_display_shot*` in psmac_display.c;
     the VideoCore buffer before the video driver opens), encoded by our own streaming deflate (fixed
     Huffman codes, matches one and two pixels back and one and two rows up), a band of 8 KB of rows
     per turn of the service loop, each band its own IDAT chunk; input through `.PiInput`'s ring with
     two new event types (an absolute mouse position, a Mac virtual key code: `mac/piinput.s`),
     paced by a queue in the kernel; the log ring with a byte cursor and secrets hidden, plus the
     `[BOOT] t=` and `[CRASH]` lines kept apart, since the 64 KB ring loses them within minutes; a
     JSON state (CurApName, Ticks, DSErrCode at $AF0, the crash records of item 53 since the last
     RESET, both network units, the disks); restart (the 68k sent to the ROM's reset entry
     $4080002A from `ProcessIRQ`, through the unused `INTF.RESET` byte of the interrupt word, so it
     works when the Mac is hung), NMI (`INTF.ARM_err`, level 7) and a watchdog reboot of the Pi.
     `mac.remotefile=BASE` serves it all through semihosting, which is how it was first tried.
   - *Stage 2, the transport* (`psmac_ip.c`): ARP answers, ICMP echo, DHCP with a client identifier
     and host name, mDNS for `aurora68-pi.local`, a listening TCP (four connections, 32 KB send
     buffers, go-back-N). Every driver shows the stack each received frame before the Mac's ring
     (one call; "the kernel's, the Mac's, or nobody's") and sends its frames when the Mac's ring is
     empty: the file bridge and the simulated radio (`alice_psmac.c`), the LAN9514/LAN7800/CDC-ECM
     (`psmac_usbnet.c`), the WiFi and GENET (`alice_net.c`, `#ifdef PSMAC`). Wired, the kernel has
     its own MAC address (the LAN7800's second perfect-filter slot; the LAN9514 and GENET
     promiscuous with software filtering while the remote is on); on the WiFi it shares the Mac's
     and gets its own IP address by its DHCP client identifier, or, if the router gives it the Mac's
     address or none, shares that too and takes TCP port 8068 alone. `remote=on` and a token of at
     least 8 characters are both needed, every request but the page needs the token, and sources
     outside RFC 1918 and link-local are refused unless `remote.any=1`.
   - *Stage 3*: `tools/aurora68-mcp.py` (MCP over stdio: screenshot, click, move, drag, type, key,
     log, state, restart) and `tools/aurora68.py` (the command line). `tools/pinet.py` learnt to give
     the kernel 192.168.N.3 by its host name and to relay a host port to the kernel's port 80
     (`PINET_KERNEL=PORT`), beside the Mac's own traffic.

   What the testbed showed (solo board, the full test image, `mac.net=` through pinet):
   - The kernel bound 192.168.68.3 by DHCP on the file bridge with its own address
     02:50:53:4d:41:c3 and announced itself by mDNS; the Mac's Riptide bound 192.168.68.2 beside it,
     answered pinet's IPv6 pings and its own mDNS name throughout.
   - A screenshot over HTTP matched `tools/vcshot.py`'s picture of the same moment pixel for pixel
     (640 by 480, 0 of 307,200 pixels differ, twice, on two kernels). A Finder screen is 10 to 30 KB.
   - Clicks and typing drove the Finder: a double-click opened the disk, a double-click its Apps
     folder, typing "Octavo" selected the folder and Command-O opened it
     (`docs/img/remote-finder-typed-cmd-o.png`), a double-click started Octavo, and a sentence typed
     into its document arrived whole (`docs/img/remote-octavo-typed.png`). The first try kept 12 of
     the 64 letters: Octavo under QEMU redraws for a second after each key and the Mac's event queue
     holds 20; the queue now waits while `.PiInput`'s ring or the Event Manager's queue ($14A) holds
     events (two seconds at most, since the system error dialog and the debugger take none).
   - CrashTest (item 53) opened from the Finder by typing its name and Command-O: the state said
     `"state": "system error"`, `ds_err_code` 10, "bad F-line instruction", the exception F-line at
     $00F5C412 (`docs/img/remote-system-error-crashtest.png`); `/log?tags=CRASH` returned the record.
     `POST /restart` from that dialog brought the Mac back to the Finder (the ROM's RESET reset the
     card as Restart does); `?how=nmi` brought up the System's ">" debugger prompt
     (`docs/img/remote-nmi-debugger.png`), and typing "G" and Return through the remote left it again;
     `?how=pi` rebooted the whole Pi in QEMU, which booted the Mac again.
   - The MCP server, run by a script as an MCP host would (initialize, tools/list, tools/call), returned
     the screenshot as an image and its double-click on the disk brought the disk's window to the front
     (`docs/img/remote-mcp-click.png`).
   - On the simulated WiFi with the wired bridge up as well (`remote.link=wifi`): the kernel used the
     Mac's MAC address 02:50:53:57:49:46 and pinet's DHCP gave it 192.168.69.3 by its host name while
     the Mac kept 192.168.69.2 and 192.168.68.2; the HTTP API answered through the WiFi peer. With
     `remote.wifi=shared` the kernel followed the Mac's address ("the Mac's WiFi address is now
     192.168.69.2") and answered on 192.168.69.2:8068, the Mac's IPv6 echoes going on beside it. One
     run of that setting (of two) had Riptide's WiFi interface take no frames at all and fall back to
     a link-local address, with Riptide's start-up lines missing from the log; the kernel's hook was
     checked natively to hand those frames to the Mac, and the second run bound normally. Not
     understood yet.
   - Cost. The 68k's own core runs none of this (only the interrupt word's check, which upstream
     makes on every block anyway). Speedometer 3.23's CPU test, three runs each, driven the same way
     (through the semihosting channel): remote off 27.5, 34.6, 35.3; remote on and idle 36.0, 32.0,
     33.9 (`docs/img/remote-speedometer.png`): no drop beyond QEMU's spread. Run while another program
     fetched screenshots without pause the figures fell to 23 and 18, but that was the host: four
     vCPU threads on a four-core host beside the clients and pinet. Core 3: the first version did the
     remote's rounds every turn, a fifth of the core's time under QEMU with nobody connected; it now
     returns at once when idle and looks at the clock every 32nd turn. A screenshot is encoded in
     bands of 8 KB of rows: 0.15 ms each natively on the host's x86 (1920 by 1200 in 31 ms in all),
     2 to 12 ms under QEMU depending on the host's load; on a Pi 3 a band should be under a
     millisecond and a 1920 by 1200 desktop a quarter of a second, with the Mac running throughout.

   Not verified on hardware: everything above ran in QEMU; the LAN9514's promiscuous mode, the
   LAN7800's second filter slot and GENET's promiscuous mode for the kernel's address, the WiFi
   path through the Broadcom firmware (frames to the shared MAC address steered by IP address, and
   whether the user's router keeps the two DHCP leases apart), the watchdog reboot on a real Pi,
   mDNS from a MacBook, and the encoder's speed on a Cortex-A53.

60. **A Pi that fits the SE: the 3A+ and the Zero 2 W (20 November).** The user's Pi 3B does not fit
   the classic PiStorm inside the SE: the board is laid out for the 3A+'s footprint. The user asked
   about a Pi Zero W; the original (2017, ARMv6) cannot run a 64-bit kernel, so the candidates are
   the **3A+** (BCM2837B0 at 1.4 GHz, 512 MB, BCM43455, one USB-A port on the SoC's DWC2, full HDMI,
   no Ethernet) and the **Zero 2 W** (RP3A0, a BCM2837 at 1.0 GHz, 512 MB, BCM43436, one micro-USB
   OTG port, mini-HDMI). Every board-dependent path was gone through for both before anything is
   bought or moved, and the 3A+ was booted in QEMU 8.2's `raspi3ap` (512 MB, revision 9020e0).
   What was wrong:
   - *The 3A+ was taken for a 3B+.* The Pi firmware has no device tree for the 3A+ (there is no
     `bcm2710-rpi-3-a-plus.dtb` in raspberrypi/firmware, at the pinned commit or on master; the
     mainline kernel's `bcm2837-rpi-3-a-plus` is not what the firmware loads) and passes it
     `bcm2710-rpi-3-b-plus.dtb`: compatible `raspberrypi,3-model-b-plus`, an `ethernet0` alias naming
     the LAN7800. `psmac_board_wired` read only the tree, so a 3A+ kept the wired card ($81) and
     looked for a LAN7800; MacTCP would have bound the dead card. The first `raspi3ap` run with that
     tree (the kernel of `dc3937c`) said exactly that: "the wired Ethernet card ($81) and the WiFi ($83)
     both on slot 9; wired port: LAN7800", "no LAN7800 found behind the 3B+'s hubs". The board now
     comes from the revision code, bits 4-11 (`psmac_board_type`, psmac_usbnet.c: the firmware's
     `/system/linux,revision`, else the mailbox's GET_BOARD_REVISION, asked once on core 0 before the
     services start), with the device tree as the fallback; one log line says it: "[PSMAC] board:
     Raspberry Pi 3 Model A+ (revision 9020e0, type 0e); wired port: none (WiFi only); 254 MiB of RAM
     free for the Mac and its disks", and "$81 left out". The Bluetooth firmware's choice uses the
     same table (it was right by accident: the 3B+'s tree means BCM4345C0 too).
   - *Unit 1 on a board without a wired port* was left to alice_net.c's GENET code, which called it
     an Ethernet port and, with `eth=on`, would have started a GENET this SoC does not have.
     psmac_usbnet.c now answers it as "no unit" there, as with `eth=off`. `eth=usb` (new) keeps $81
     on a 3A+ or Zero 2 W and takes a USB Ethernet adapter (LAN9500, LAN7800 or CDC-ECM) instead;
     Alice68's HID enumeration stops once it has a keyboard and a mouse, so an adapter on a later hub
     port was never reset or addressed, and `address_port` now does that. In QEMU, a `usb-net` on hub
     port 3 behind a keyboard and a mouse: "CDC-ECM found at USB address 4 (0525:a4a2)", "link up".
   - *Memory.* Without `mac.ram=` the kernel kept 192 MiB of its free block back, though its own pools
     (the JIT's 64 MiB, the 64 MiB of kernel pages, the VideoCore's) are already outside that block.
     On a 512 MB Pi (448 MiB for the ARM with gpu_mem=64) the block is 254 MiB, so the Mac got 72 MiB;
     with the user's 96 MB System 7.1 image in the container the block is 156 MiB and the Mac got
     **8 MiB** (the first `raspi3ap` run: "no mac.ram=: 8 MiB of the Pi's 156 MiB free go to the
     Mac"). What else comes out of the block later is a disk image on the card (psmac_card.c), so
     that is what is kept back now: 112 MiB (a 96 MB image and the loader's 16 MiB of slack) when the
     container has no image, 16 MiB when it has one. Both cases now give the Mac 128 MiB: "memory:
     Mac 128 MiB, 17 MiB left for the kernel" with the image inside, "Mac 128 MiB, 115 MiB left: a disk
     image on the card can be up to 99 MB" with it on the card, and the 96 MB image on the card loaded
     beside the Mac's 128 (QEMU: Finder at 39 s, the image read in the background). A 1 GB Pi is
     unchanged (128 MiB, 611 MB for card images). When the Mac gets less than 128, or an image does
     not fit, the log says so in a line starting MEMORY, with the figures and what to change.
   - *The kernel's firmware table had eight slots* (AN_FW_MAX, the board protocol's), and the
     one-card container get-firmware.sh prints held thirteen files, the 43455 set last: on a 3A+,
     3B+ or Pi 4 that container would have brought no WiFi firmware, silently. The kernel's own table
     now holds 24 (the 68k's offer still reads eight), and a file that does not fit is logged. In
     QEMU all fifteen files of the new list are taken.
   - *The Zero 2 W's two chips.* Zero 2 Ws carry a BCM43436 or, later, a 43436s; both answer as chip
     43430. Raspberry Pi's firmware package sends the name Linux gives revision 1
     (`brcmfmac43430-sdio.raspberrypi,model-zero-2-w.bin` and `.txt`) to `brcmfmac43436s-sdio`, with
     no CLM blob of its own (so the 43430's), and any other revision is `brcmfmac43436-sdio`. The
     kernel loaded the 43436 files on either; it now picks by the revision the same way and logs
     which. get-firmware.sh fetches the two 43436s files (pinned, SHA-256 checked) and the 3A+'s note.
   - *Low-speed keyboards and mice on the root port.* On a 3B everything sits behind the LAN9514,
     whose route tells the channel a device is low speed. On a 3A+ or Zero 2 W a keyboard can be on
     the root port itself; Alice68's enumeration never marks that one low speed, so the channel
     spoke full speed to it. psmac_usb.c sets HCCHAR's LSpdDev for every packet without a hub route
     when the root port is at low speed (a low-speed device cannot be a hub). A hub pulled out of the
     one port was not noticed (only its ports were watched); the root port's connection is now
     looked at too. QEMU cannot show either: its DWC2 has one port and QEMU puts a full-speed hub on
     it by itself as soon as a device is added, and its keyboard and mouse are full speed.
   - *The GPIO transport.* The Pi 3 timings are strobes counted in GPIO register writes, whose length
     is the peripheral bus's: the core clock, 400 MHz by default on the 3B, 3A+ and Zero 2 W alike,
     whatever the ARM's clock (1.2, 1.4, 1.0 GHz). So one set serves all three, as in upstream Emu68.
     The choice of set was by the generic timer's frequency (19.2 MHz on a Pi 3); QEMU's raspi3 says
     62.5 MHz, so the testbed got the Pi 4 set. It now follows the board type. GPCLK0's 200 MHz is
     PLLC_PER divided by 6, which assumes the firmware's 1200 MHz; the kernel now reads PLLC back
     (NDIV, fraction, PDIV, prescaler, PER divider, as Linux's clk-bcm2835) and logs it: "CPLD clock
     on GPIO 4: PLLC_PER 1200 MHz / 6 = 200 MHz" under QEMU, whose clock manager starts with the
     firmware's values. The divider of 6 is kept whenever it gives 200 MHz within a tenth; only a
     PLLC elsewhere (a core_freq= in config.txt) gets the nearest divider. And on the SE board the
     service core and the board layer's first access on core 0 both ran the CPLD's start, side by
     side (the log had it twice); the second now waits for the first.
   - *Bluetooth's flow control.* bt_pi.c said the 3B+ and 3A+ have no CTS/RTS to the chip. They do,
     on GPIO 30/31 (mainline's 3A+ tree says `uart-has-rtscts`; Raspberry Pi's tree routes them only
     for the mini UART), but Raspberry Pi's own set-up runs those boards without them ("noflow" at
     921600), and so does the kernel, at 115200: only the comment changed. The Zero 2 W's tree routes
     all four (uart0_pins 30-33) and the kernel uses them there, as before.
   - Checked and right as they were: WL_ON (the expander on the 3A+; GPIO 41 on the Zero 2 W) and BT_ON
     (expander line 0; GPIO 42), the sleep clock on GPIO 43, the SD card on the sdhost (GPIO 48-53)
     and the WiFi on the Arasan (GPIO 34-39) on both, the VideoCore frame buffer and HDMI (mini-HDMI
     is HDMI), the card writer, the BCM43455 firmware (`brcmfmac43455-sdio.txt` serves the 3A+,
     3B+ and 4: their board-specific names are links to it).
   - config.txt: the Zero 2 W is not a `[pi3]` to the firmware; a `[pi02]` section says so.

   *The QEMU runs* (`MACHINE=raspi3ap MEM=512M DTB=bcm2710-rpi-3-b-plus.dtb tools/run-solo.sh`, new
   options; `QEMU_MON` gives a run its own monitor socket): the solo board with a hub, a keyboard and a
   mouse; a key typed into PiPing's window closed it and the mouse opened the Apple menu
   (`docs/img/solo-pi3aplus-usb-apple-menu.png`); the Finder at 21.7 s with the image in the
   container, 37-39 s with it on the card. The SE board on `raspi3ap` (no MAME): "Pi 3 timings", the
   PLLC line, one start of the CPLD. A `raspi3b` run still says "Raspberry Pi 3 Model B (revision
   a02082, type 08); wired port: LAN9514" with $81 kept.

   *Not verified:* anything on a real 3A+ or Zero 2 W. The Zero 2 W only by reading (QEMU's `raspi0`
   is the ARMv6 Zero; there is no Zero 2 W model) and the board-table logic (revision type 0x12:
   no wired port, BCM43430B0, WL_ON on GPIO 41). The 43436s revision rule comes from Raspberry Pi's
   firmware links, not from a chip; which Bluetooth firmware the 43436s wants is not known (the kernel
   offers BCM43430B0, then A1). A low-speed keyboard on the root port, a hub pulled out, a
   LAN9500/LAN7800 USB adapter, and GPCLK0 measured on the pin are for the hardware. The log lines
   to send: "[PSMAC] board: ...", "[PSMAC] memory: ...", "[PSMAC:GPIO] CPLD clock ...", "[WIFI] chip
   ... rev ...", "[USB] start rc=...".

61. **The Pi's power state in the log (20 November).** In the SE the Pi takes its 5 V through the
   68000 socket from a thirty-year-old analog board, and a Pi 3B draws 0.5 to 0.8 A. The firmware's
   GET_THROTTLED tag (0x00030046) says when the supply sagged; alice_status.c read it for the
   Amiga's status page, but only with `status.sensors=mbox`. `psmac_power.c` now reads it every two
   seconds on the service core, on every board, and logs each change (`[PSMAC] power: under-voltage
   now; ARM frequency capped (0x30003)`, `power: normal again; ... occurred since boot`); the first
   under-voltage is also a `[BOOT]` line, the remote's `/state` has the bits as `power`, the status
   page gets them too. One mailbox call per reading, under the shared mailbox lock, never while the
   WiFi coroutine holds it (it yields inside its own calls on the same core), and without retries:
   `alice_mbox_once` in alice_net.c. QEMU 8.2's mailbox answers the buffer but not this tag (its
   response bit set, length 0): the kernel says "the firmware does not report it" once and stops.
   `mac.power=off` stops the watch. docs/12, "Power through the socket".

62. **Two screens on the SE board (20 November).** The user's bring-up starts now, and they asked
   for the SE's own screen plus HDMI as a second screen at any size and depth. docs/10, "Two screens
   on the SE", is the record; in short:
   - *Design*: two virtual cards in one declaration ROM file (`psmac-se-WxH.declrom`): slot 9 as
     before with its video sResource now the SE's 512 by 342 one-bit screen, and slot $A, a
     declaration ROM only, with the HDMI screen (`.Display_Video_PiHDMI`, pivideo.s with `SCREEN2`),
     its driver reaching the slot-9 window and taking slot 9's interrupt, its modes pointing into
     slot 9's space. The first version had both video sResources on one card; it worked, but Monitors
     takes a screen's depths from the first video sResource of its slot and offered only one bit for
     the HDMI screen. The kernel serves slot $A's ROM from the window's top 16 KiB, on the SE board
     with `mac.hdmi=screen` (the default) only; `mirror` and `log` keep one screen.
   - *PiScrn*, a new INIT, writes the 'scrn' resource the ROM needs to activate a second screen
     (docs/08 item 26) when there is none or it no longer fits, and restarts once.
   - *Found on the way*: the display block's "enabled" flag outlived the 68k's RESET, so after a
     restart the HDMI driver's first tick came before its interrupt handler and the Mac lived in its
     level-2 interrupt from then on (the flag is cleared at RESET; the driver turns VBLs on after
     installing the handler); `.PiInput` clipped every mouse movement to CrsrPin, the current
     screen, so no USB or Bluetooth mouse and no remote click ever reached a second screen (now the
     rectangle around every active screen); with `mac.console=hdmi` on the SE board the SE's own
     screen never changed, since its rows were sent only at the end of an HDMI conversion.
   - *Tested* in MAME's `macse_ps` with System 7.1: the Finder on the SE's screen and a second desktop
     on HDMI at 1024 by 768 and 1920 by 1200 in millions, PiColor on it, windows dragged across by the
     remote control, Monitors with two monitors and the HDMI screen's six depths (256 chosen there
     took effect at once), PiScrn's write-and-restart three times (new, HDMI size changed, slot
     changed); `mac.hdmi=mirror` with the old 512x342 ROM and `mac.console=hdmi` with the new file
     as regressions, and the solo board at 1920 by 1200. Not on hardware.

63. **The first boot on a real SE: a parameter RAM byte, not the memory size (20 November).** The
   user's first boot of the SE board on the Macintosh SE itself (classic PiStorm, Pi 3B, kernel of
   `5ff7f89`, cmdline `enable_cache mac.board=se mac.console=hdmi bt=off wifi=off`, so the default
   128 MiB) died in the IIci ROM within a second of its first instruction; the SE's screen was never
   written. The saved `aurora68.log` had the ROM's checksum test, the first SCSI selection, then
   "translating at bad PC 00000011" and two crash records. The PC trail ended in the ROM's MMU table
   builder (`link a4,#-8280` at 0x4080476C, called twice from InitMMU at 0x408042FE): a first pass
   writing descriptors into the 8,280-byte frame on its stack, the pass at 0x408049E4 turning them
   into tables at the top of RAM, then `moveml; unlk a4; rts` at 0x408047AC returning to $00000011
   with A4 = $00000011 and the stack above it $00000011 five times and $00000022 ten: the first pass
   had written past its frame, over the saved A4 and the return address. A5 = $007FFFF8 (InitMMU's
   own frame, the stack the ROM caps at 8 MB) and A6 = $07FFFFE4 (its globals at the top of 128 MiB)
   were the ROM's normal layout for that size, not a mis-sized bank.
   - *What the frame's size depends on.* The builder's input is the RAM chunk list, the decoder's
     template and the flags byte InitMMU keeps at A6-25, which is extended PRAM byte $8A as read at
     0x408043D4: bit 0 32-bit addressing (with it clear the 24-bit table is built as well), bit 4,
     only honoured with bit 0, a layout with RAM at logical $38000000 (0x408043EE adds $37FF0000 to
     the logical top of RAM). On the SE board the PRAM is the SE's own clock chip (343-0042-B, 256
     bytes) through the VIA; `pram.bin` on the card ("[CARD] pram.bin: read") is the solo board's and
     is not used there. Nothing on an SE has kept $8A meaningful for a IIci ROM, while every testbed
     run had either the solo board's default ($05) or MAME's PRAM set by hand (`tools/pram32.py`,
     $05).
   - *The MAME SE, with the PRAM aside* (`mame/nvramFS/macse_ps/rtc` copied away and restored after
     each run), 128 MiB, the stock ROM as on the card: $8A = $00 (24-bit) boots to the Finder in
     145 s, so neither 128 MiB nor 24-bit mode is the trigger. $8A = $15 (bits 0, 2 and 4) crashes at
     PC $00000011 with every data and address register, the stack and the PC trail exactly as on the
     hardware. At 8 MiB the same byte gives the mapping the user's second hardware boot
     (`mac.ram=8`, kernel se3) photographed, "switch #3: TC=80f05480 CRP=7fff0003:007ffce0" with
     "log 38000000-387effff -> phys 00010000": the SE's chip holds $8A with bits 0 and 4 set.
   - *The fix*, in the board layer (psmac_board_se.c): it follows the clock chip's serial protocol on
     port B (enable, clock, data; the command byte, the extended command's address byte, the
     direction) and, while byte $8A comes out of the chip, hands the ROM a mended bit 0 of port B:
     bit 4 cleared always, bits 0 and 2 set when the Mac has more than 8 MiB, the rest as the chip
     has them. Every reader sees the same byte (InitMMU, the Start Manager at 0x4080CC60,
     _ReadXPRam); writes go to the chip unchanged, and System 7.1 writes back the mode it started in,
     so after one boot the chip itself holds $05. One log line says what was changed and why: "[PSMAC:SE]
     parameter RAM $8A: the SE's clock chip says $15, the ROM is given $05: bit 4 cleared (...)";
     `mac.pram8a=keep` leaves the byte alone. The first version logged the chip as saying $14: the
     ROM's read-modify-write of the enable bit after the last data bit reads port B with the data line
     driven again, and that read was counted as the chip's bit 0; reads while DDRB bit 0 is an output
     are left alone now.
   - *Where 8 MiB stopped on the hardware.* With `mac.ram=8` the ROM got past InitMMU into the
     $38000000 layout and stalled: no video, the main code at 0x4080EFDE waiting for an I/O, the RBV
     stand-in with IFR $08 and IER $08, and every bus cycle a read of $5FF070, the 5380's
     reset-interrupt register. IFR bit 3 is the 5380's interrupt. The board layer kept whatever the
     ROM's power-on test wrote there (MAME's RBV sets IFR bits written with bit 7), so it read 1 from
     then on, and every time the SCSI Manager enabled it (IER bit 3, the store at 0x4080A08A) level 2
     came at once, for no interrupt, until it was disabled again. The MAME SE has always run with
     that (IFR $08 or $0A in every report line of a good boot, IER switching between $02, $0A, $12
     and $1A) and got past at its pace; on the hardware it is the likeliest reason for that stall,
     though the MAME SE could not show the stall itself, even in the $38000000 layout. On an SE the
     5380's interrupt is kept off the bus (VIA port B bit 6), so bit 3 now reads 0 there; the solo
     board still computes it from its 5380. Boot times in MAME did not change (Finder at 145 s with
     128 MiB), so nothing was waiting on that interrupt.
   - *Heartbeats*: on the SE board the wire carries the SE's addresses, which the heartbeat classed
     by the IIci's map, so every cycle on the hardware was "other". They are classed by the SE's map
     now ("via 1942 scc 0 scsi 46887 ... swim 16").
   - *Verified* in MAME's `macse_ps` with the stock ROM and System 7.1 from the SE's SCSI disk:
     128 MiB with $8A = $15 (the hardware's case) to the Finder in 145 s; 8 MiB with $15 to the
     Finder in 138 s with RAM at logical 0 ("log 00000000-007fffff -> phys 00000000"); 128 MiB with
     $00 to the Finder in 137 s, 32-bit. The solo board unchanged (`tools/run-solo.sh`, 128 MiB,
     Finder at 20 s). Not yet on the hardware. `mac.ram=8` on the old kernel avoids the crash, as the
     user's second boot showed, but not the stall after it.

64. **The second boot on the SE: a clock chip that does not answer (20 November).** The kernel of
   `c6d203f` on the same SE (Pi 3B, 128 MiB, `enable_cache mac.board=se mac.console=hdmi bt=off
   wifi=off mac.image=off`, no SCSI disk attached and no floppy in the drive) got past the MMU tables
   ("switch #3: TC=80f06b00 ... log 00000000-07ffffff -> phys 00000000", so item 63's mending did its
   job) and then sat at PC 0x4080EFDE for as long as it ran: never a video, about 2,130 VIA accesses
   every five seconds, no SCSI, SWIM or SCC access after the first five, the display block's flags 0
   and "rows sent 0". The log line of item 63 said "the SE's clock chip says $ff, the ROM is given
   $ef" three times.
   - *What the ROM waits for.* 0x4080EFDE is the File Manager's synchronous wait on ioResult
     (`movew a0@(16),d0; bgts`) after a call queued on FSQHdr ($360). The stack names the call: the
     trap dispatcher's return 0x40809A0A, the parameter block $040008AA, the name pointer 0x40801184
     (the string ".Sony") and the return address 0x4080113A, just after the `_Open` at 0x40801138. So
     the Start Manager had opened `.Sony` and the open never completed, and it had not opened slot
     9's video driver first (no "[MAC] PiVideo: open", which every MAME SE boot logs at this point).
   - *What the chip says.* $FF from byte $8A at all three reads, and the second and third came after
     the ROM's InitUtil (0x4080C5CC), which finds the signature 'NuMc' at $0C-$0F missing, writes it,
     clears the extended bytes and writes its defaults (0x4080C676, 0x4080C68A). A chip that
     answered would have said $00 at those reads, as the MAME SE does. This one neither answers nor
     keeps what it is told: the data line stays high (proven only as far as $8A; the other bytes were
     not logged).
   - *The MAME SE in that state.* MAME's PRAM copied aside first (`mame/nvramFS/macse_ps/rtc`,
     restored after every run), no disk, 128 MiB, the hardware's cmdline. All 256 bytes $FF with
     MAME's chip working: the ROM resets them and shows the question-mark disk on the SE's screen
     (so garbage alone is not the trigger). A chip that never drives the line (`mac.rtcsim=dead`, a
     testbed option that ORs 1 into port B bit 0 whenever DDRB bit 0 is an input): PC 0x4080EFDE,
     A0 = $040008AA, the same eight stack words, VIA traffic at IER and T2, RBV IER $08, "local
     reads 117 writes 31", all as on the hardware. Then, with the kernel answering every byte from
     the defaults below, the same bytes put back to $FF by halves: $80-$FF hangs, $00-$7F does not,
     and so on down to one byte, extended $AE. The ROM reads it at 0x4081AB44 (`_ReadXPRam`, one
     byte), compares it with a limit from its ROM resource table with a signed compare (0x4081AB66),
     so $FF, that is -1, passes, and goes on to choose its ROM resources with it; from the look of
     the code that set is what decides whether the video and `.Sony` come up as they should. Why
     exactly `.Sony`'s open never ends was not followed further.
   - *The fix* (psmac_board_se.c). Before the 68k's first instruction the board layer asks the chip
     itself, with the ROM's own sequence on port B (0x4080B1E4-B26A): the signature bytes $0C-$0F and
     the four bytes of the clock, eight reads and no write. Core 0 waits for it (at most a second)
     before it lets the 68k go, since the ROM's first RESET would reset the VIA under it. If all
     eight read $FF the chip is taken as absent and the board layer answers in its place, on the
     data bit of every port B read while a byte is coming out: parameter RAM from the solo board's
     validated defaults (`psmac_pram_default.h`, $8A mended as in item 63), the clock from 20
     November 2026 running with the Pi's timer, and whatever the ROM or the System writes kept for
     the rest of the session (a Restart included). The writes still go to the chip; the kernel never
     writes it. A chip that answers but holds no signature is left to the ROM, which resets it as any
     Mac does after a flat battery. One line says which: "[PSMAC:SE] clock chip: answers; parameter
     RAM valid (signature 4e 75 4d 63), clock e725c080", or "clock chip: reads $FF for the signature
     and the clock: it does not answer ... The board layer answers in its place ...", and the report
     line every five seconds adds "clock chip: answering|not answering, N bytes read (M of them $FF
     from the chip), K written". `mac.sepram=chip` turns the stand-in off; `mac.pram8a=keep` still
     only concerns $8A. Following the protocol is no longer limited to $8A: the file now tracks every
     read and write command (traditional and extended), the write-protect register, and the clock's
     bytes.
   - *Verified* in MAME's `macse_ps`: the dead chip with no disk reaches the question-mark disk on
     the SE's screen (the probe's line, video at 1 bit, "629 bytes read (629 of them $FF from the
     chip)"); MAME's own valid PRAM with System 7.1 on the SE's SCSI disk reaches the Finder and
     PiPing ("clock chip: answers; parameter RAM valid"), PRAM afterwards unchanged except slot $A's
     record, which the Slot Manager clears when that card is absent. The solo board unchanged
     (`tools/run-solo.sh`, 128 MiB, Finder at 27.8 s). *Inferred, not proven:* that the real chip
     reads $FF for every byte and not only for $8A; the probe's line on the next hardware boot says
     so either way. A chip that reads $FF while the SE is powered is more than a flat battery (the
     343-0042 runs from the logic board's 5 V when the Mac is on): a leaked battery's corrosion
     around the chip or the VIA, or a dead chip, are the usual reasons on an SE.

65. **A black HDMI screen with the log on it (20 November).** With `mac.console=hdmi` the kernel of
   `5ff7f89` (se3) showed its log on HDMI; `c6d203f` showed nothing. Its log says why it might:
   the firmware's boot buffer was "Framebuffer @ 3ea7a000" (1024 x 768, 16 bits, the HDMI mode of
   config.txt); the two-screen code of item 62 then asked, for the log, for a 32-bit buffer at the
   HDMI size, which is the same 1024 x 768, and got "VideoCore 1024x768 x32 pitch 4096 at
   0x3e8fa000", a new buffer whose top half overlaps the old one; se3 had asked for 512 x 342 there
   and got a buffer at 0x3eb4a000, which was on the screen. The console draws into whatever buffer
   the kernel last got (`vcfb`), so the log went into the new one; QEMU's VideoCore model scans out
   either. Why the firmware did not show it is not known (the solo board, without `mac.screen=`,
   also asks for a 32-bit buffer at the HDMI mode's size, and its picture has been on HDMI since item
   29, so "the same size" is probably not the whole story; a mailbox call from the WiFi module failed
   in that very moment, "[WIFI] mailbox tag 10002: error -4 (code 0)", which recalls item 31).
   What is known is that the boot buffer is on the screen on every Pi: the splash and the first lines
   of the log are drawn into it. So on the SE board with the log on HDMI and no second screen
   (`mac.hdmi=log`, which `mac.console=hdmi` means) the kernel now keeps the firmware's buffer as it
   is, 16 bits, and asks for nothing; a Restart's RESET leaves it alone as well. The line reads
   "VideoCore 1024x768 x16 pitch 2048 at ...3ea7a000: the firmware's own buffer, kept; the log stays
   there (mac.hdmi=log)". `mac.hdmi=mirror`, the two screens and the solo board remake the buffer as
   before. QEMU: the VideoCore photograph (`tools/vcshot.py`) shows the log at 16 bits, heartbeats
   and the clock chip's report line included. Not seen on hardware yet; if the two-screen mode
   (`mac.hdmi=screen`) shows a black HDMI screen at the HDMI mode's own size, this is where to look.

66. **USB retries held core 3 for 360 ms (20 November).** Both SE boots had "USB 360621 us" as the
   service loop's longest part, every five to ten seconds, with "[USB] retry rc=-45: 0 keyboards, 0
   mice (hub ports 5, devices seen 1, last port error -28)" and the LAN9514's network chip found
   again after each: no keyboard or mouse was plugged in, so item 40's retry (four enumerations,
   six seconds apart) ran in full, each one a root port reset, five hub ports powered, reset and
   waited for, on core 3, while the mailbox, the card and the network waited. Two changes in
   psmac_usb.c. Every enumeration after the start now runs on core 1, which did the first one and
   then idled: core 3 releases the keys, stops the network chip's bulk channels, takes `usb_on`
   down (so neither its polls nor psmac_usbnet.c touch the controller) and hands over the reason;
   core 1 enumerates and puts `usb_on` back. And a retry is made only when the enumeration saw a
   device on a hub port (or on the root port of a Pi without a hub) that did not come out as the
   keyboard or the mouse, not counting the board's own network chip on the 3B's and 3B+'s hub port
   1: with nothing plugged in there is nothing to find, and each enumeration resets the network chip
   too. QEMU, solo board with a hub and a keyboard: one enumeration, no retries (before: four); a
   mouse added later through the monitor (`device_add usb-mouse`) found by core 1 ("hub port 2:
   connected", "plug or unplug rc=0: 1 keyboard, 1 mouse") and polled from then on.

67. **A disk image as the SE's only disk is not a startup disk (20 November).** The user wants the
   card's disk images (`mac.image=`, PiDisk volumes on the SE board) and, later, a BlueSCSI on the SE's
   own 5380. The MAME SE with no SCSI disk, the stand-in parameter RAM of item 64 and System 7.1 as a
   PiDisk volume (`mac.disk0=`, the testbed's way of giving it a card image) stayed at the
   question-mark disk: "1 block device(s)", but `.PiDisk` was never opened. On the solo board too it
   opens only after the System has started from the SCSI disk (the first "[MAC] PiDisk: open" comes
   after 1,093 blocks read from SCSI ID 0), so a PiDisk volume has only ever mounted beside a startup
   disk. The IIci ROM starts from a NuBus card only one way: `_GetDefaultStartup` (0x40801434) names
   a slot and an sResource, and the Start Manager runs that sResource's boot record (ID 6) through
   the Slot Manager's sExec (0x40801540 with state 0, 0x4080222A with state 1) and boots from the
   driver it returns. PiDisk's sResource has no boot record. What it would take: a boot record in
   the declaration ROM that loads and opens `.PiDisk` (tools/mac/mkdeclrom.py, mac/), and the default
   startup device ($78-$7B) pointing at slot 9 and that sResource, which the stand-in parameter RAM
   can supply when the chip does not answer and the Startup Disk control panel otherwise. Until then
   the SE starts from a SCSI disk or a floppy and the card's images mount beside it.
   - *What the ROM does with a card* (read in the disassembly the same evening). The search starts at
     0x40801430: `_GetDefaultStartup`, and when the record names a slot (its second word positive)
     0x408014F0 asks the Slot Manager for the sResource (`sFindsRsrcPtr`), fills an SEBlock (slot,
     sResource, seDevice and sePartition from parameter RAM, seBootState 0) and runs its boot record
     with `sExec`. sExec (0x40805C3A) copies the block into the heap, checks revision 2 and a CPU id of
     at most 2, calls the code at offset field + offset with A0 = the SEBlock and every register saved,
     and disposes of the copy. A zero seStatus makes seRefNum the driver whose drives the first pass of
     the drive queue tries, a floppy included (the filter at 0x408016F0); the passes after it take any
     drive (0x40801590), in the queue's order. All this before the SCSI drivers are loaded
     (0x40801560). Later, once the System file is open, the ROM opens the slot drivers that are to be
     opened at start (0x40802240): for an sResource with a boot record it runs the record once more,
     seBootState 1 (0x4080222A), and leaves the driver to it. The ROM's own way of opening a slot
     driver is OpenSlot with ioFlags fMulti and an SEBlock (0x40800CA0).
   - *The boot record* (`mac/pidisk_boot.s`, sRsrcBootRec 6 in PiDisk's sResource,
     `mkdeclrom.py --boot`): both times it opens `.PiDisk` exactly that way; the driver's Open adds a
     drive for every image as before. At state 0 it asks the kernel which disk is to go first (a new
     BLOCK command, `PSMB_BLK_BOOT`) and returns that driver's reference number: PiDisk's own, or a SCSI
     disk's. A drive the first pass does not take stays in the queue, ahead of the SCSI drives the ROM
     loads next, so the image comes before the other SCSI disks.
   - *The default startup device*: from each RESET until the boot record has asked, the board layers
     show the Mac $78-$7B as 00 00 09 80 (slot 9, sResource $80) whenever an image is published, and
     report what the parameter RAM really holds (psmac_mbox.c, `psmac_startup_*`; the SE's clock chip,
     its stand-in and the solo board's chip). The first version only changed what reads returned, and
     the solo board's log showed the ROM writing the four bytes back: its parameter RAM test
     (0x40841DA2) reads all 256 bytes, writes a pattern and reads it back, and restores what it read.
     That test failed at $78 (the read-back was the override, not the pattern) and stored the slot-9
     record in the real parameter RAM, so the next start took it for the user's choice. Now the Mac's
     writes there go into the view until the boot record asks, and no further; on the SE's working
     chip, whose writes the board layer can rewrite but not hold back, the chip is sent its own byte
     back, bit for bit (`xp_wire_orb`). After the boot record has asked, the four bytes are the
     parameter RAM's own again, so the Startup Disk control panel sees and sets the real choice.
   - *Which disk first*, `mac.boot=`: `auto` (the default) the SCSI disk the parameter RAM names (ID 0
     when it names none or another card), then the card's image, then the other SCSI disks; `image` the
     card's image first; `scsi` the parameter RAM as it is (the kernel shows nothing). A floppy with a
     System is taken first in every case. The log says it in three lines: "startup disk: parameter RAM
     $78-$7B ff ff ff df, shown to the Mac as 00 00 09 80 ...", "startup disk: the SCSI disk the
     parameter RAM names first, then the card's image (mac.boot=auto): SCSI ID 0, refNum -33" and
     "[MAC] PiDisk boot: ...".
   - *The testbed's images*: `mac.disk0=` served the whole host file, and the System 7.1 image used for
     the card (an Iomega-formatted `.hda`) has an Apple partition map, so block 0 was the driver
     descriptor and the Start Manager found no boot blocks there (it read, ejected and read again,
     a "prime" and a "control" in the log every few seconds, at the question-mark disk). The card's
     images have always been served as their HFS partition (item 52); host files are now too.
   - *Verified* (`[BOOT]` times from the kernel's log). MAME's `macse_ps`, 128 MiB, no SCSI disk, the
     System 7.1 image as `mac.disk0=`, `mac.boot` left at auto: with `mac.rtcsim=dead`, "PiDisk: first
     block read (device 0, block 0)" at 19.3 s and the Finder at 31.1 s, PiPing from the image's
     Startup Items on the SE's screen; a Restart through the remote control's testbed channel
     (`tools/aurora68.py --semihost BASE restart`) started from the image again, the parameter RAM
     still saying ff ff ff df. With MAME's working chip and its own parameter RAM: the Finder at
     36.1 s, and the chip's $78-$7B unchanged afterwards (the ROM's test wrote them, the chip was sent
     its own bytes). With the working chip, the System 7.1 disk on the SE's SCSI ID 0 and the image as
     well: the SCSI disk first (its first pseudo-DMA block at 17.8 s, the image's first block at
     30.7 s, once the System was up), the Finder at 47.3 s, $78-$7B again unchanged. The solo board
     (`tools/run-solo.sh`, 128 MiB): the image alone (no SCSI disk) to the Finder in 17.0 and 19.0 s,
     from the PiDisk volume; the SCSI disk alone in 21.3 and 21.4 s (no image published, so nothing is
     shown to the Mac); the SCSI disk with the image beside it in 26.2 and 23.1 s from SCSI ID 0, the
     image mounted beside it ("Mac Volume" twice, the second with PiDisk's icon). Not yet on the
     hardware.

68. **The SE's top hundred rows: two address lines joined (20 November).** The SE board's second and
   third boots on the real SE (se5, se6 kernels) showed the grey desktop and the question-mark disk,
   and later the happy Mac and the Finder from a BlueSCSI, with the top hundred or so rows of the SE's
   screen as eight vertical bands of structured noise that never changed: the Finder's menu bar never
   appeared there, whatever the Mac drew, and the same bands stood across screens and reboots. The
   rows below were right.
   - *Reading the photographs.* The bands ended at row 100, and row 100 starts at $3FA700 + 100 x 64 =
     $3FC000. Rows 0-99 ($3FA700-$3FBFFF) have A14 = 0 and A15 = 1; every row from 100 down has
     A14 = A15 = 1, and so does every address of the SE's chips the kernel uses (VIA $EFE1FE, SCC
     $9FFFF8 and $BFFFF9, SCSI $5FF000-$5FF260, IWM $DFE1FF). A fault that only let A14 and A15
     agree would break those rows and nothing else; old memory showing through (the SE's DRAM as it
     powered up) fitted bands that never changed. Nothing in the kernel's path treats those addresses
     apart: every write sends the data, the low address word and the high byte afresh (psmac_gpio.c,
     the cycles word for word as upstream's ps_classic_protocol.c), the dirty bitmap's words hold 32
     rows (96 and 128 are its edges, not 100), and the overlay does not depend on A14. The CPLD
     latches A8-A15 into U2 (LTCH_A_8) with the same strobe as A0-A7.
   - *The cause*, found by the user: a sliver of solder joining U2's pins 22 (A14) and 23 (A15) on the
     hand-built PiStorm (Rev B layout, 74LVC16373A latches). With it removed the SE's screen is clean
     over its full height.
   - *What the kernel does about such a thing now* (psmac_board_se.c, `se_ram_lines`), before the 68k
     starts and before the first screen row: each address line A1-A21 is written as 0 and as 1 (a word
     at the screen's base and one at the base with that line flipped, both read back), and each pair
     of lines is taken through its four combinations, each written a word of its own and read back. A
     line stuck or not connected makes the first two addresses the same memory; two lines joined so
     that a 0 (or a 1) on either wins make three of the four one memory, which no stuck line does. A
     line that simply follows another cannot be told from a stuck one from the Pi's side (both leave
     the memory in two halves); its message says so. One line names the fault and the socket pins (68000 DIP-64: A1-A20 on
     pins 29-48, A21-A23 on 50-52), or says "address lines A1-A21 good". A read-back of the screen
     would not have seen it, since reads go astray as the writes do. `mac.selinefault=N` (stuck high)
     and `mac.selinefault=N,M` (N joined to M) put such faults on the testbed's wire to the SE's RAM.
     The first sends of rows 0 and 100 are read back once and logged as well.
   - *Two more things from the same investigation.* After every RESET instruction (the ROM's first act,
     and every Restart) the board layer reads $400000 once more: the RESET pulses the SE's reset line,
     which on an SE brings back the ROM overlay at 0, and until now only the start had cleared it (the
     comment above psmac_se_start said otherwise). The screen showed all the same, so the overlay does
     not seem to hide the kernel's writes to the screen buffer, but RAM is where it should be now. And
     the trap-table watch (item 12: low-memory page $50000 mapped read-only so that writes into the
     OS trap table could be reported) is off by default, `mac.watchtraps=1`
     to turn it on: on the SE's HDMI log the ROM's own trap table set-up (PC 0x4080C9DC) printed forty
     reports and their stacks at every start, and every write to that page, which holds all of page 0's
     low-memory globals (Ticks among them), was a data abort and a store by hand. With the watch off the
     page is mapped writable and costs nothing.
   - *Verified* in MAME's `macse_ps`: `mac.selinefault=14` gave "address line A14 does not reach the SE
     ... $3fa700 and $3fe700 are the same memory" and the SE's screen with its top 100 rows unwritten
     (white in MAME, whose RAM starts cleared) and the rest right, as on the hardware;
     `mac.selinefault=14&15` (the two joined, a 0 winning) gave "address lines A14 and A15 are joined:
     of $3f2700, $3fa700, $3f6700 and $3fe700 ... three are the same memory"; `mac.selinefault=14,15`
     (A14 following A15, the kind the user's bridge probably was) gave the A14 line of the stuck case,
     whose message names a short as one of the possibilities: from the Pi's side a line that follows
     another and a stuck line leave the memory in the same two halves. Without a fault: "address lines A1-A21 good", rows 0 and 100 read back as
     written, no `[PSMAC:WATCH]` lines (43 in the se6 kernel's run with the SCSI disk).

69. **"Parity has been disabled" on the SE (20 November).** With the se6 kernel booting System 7.1 from
   the BlueSCSI, the user's SE showed at startup "Parity has been disabled because the parity circuitry
   is not functioning. You may continue without parity checking." [Shut Down] [Continue]; the first
   BlueSCSI boot had not.
   - *Who says it.* The System's INIT 10 (compressed, 'dcmp' 2; decompressed for reading: 118 bytes)
     asks Gestalt 'prty'. Bit 0 clear: nothing. Bit 0 set and bit 1 (enabled) clear: the alert
     (ALRT/DITL -16392). Both set: it reads parameter RAM $F9-$FB and shows the alert, clearing $F9,
     when $F9 bit 0 is set and $FB is $8E, the start-up test's record of a failed parity test. Gestalt
     'prty' comes from the IIci's System patch (PTCH 1660): 0 unless UnivROMFlags ($DD4) bit 0 says the
     machine has parity; then bit 0, and bit 1 if VIA1 port B bit 6 reads 0.
   - *Why on the SE.* UnivROMFlags is the ROM's product table entry's (0x408032C4). The IIci's decoder
     has two entries, the IIci (0x408036C0, flags 0) and the IIci with parity (0x40803700, flags 1),
     told apart by VIA1 port A bit 4, read while the ROM has made it an input (0x4080321A). The board
     layer gave the ROM the IIci's identity bits 6, 2, 1 and 0 and the SE's own bits 7, 5, 4 and 3
     (item 28); on an SE bit 4 is the floppy drive select, an output, and as an input it
     reads whatever the pin is left at. When it read 1 the ROM took the SE for an IIci with parity, and
     port B bit 6, which the board layer reads as 1 (an input tied high on the IIci, the SCSI interrupt
     enable on the SE), said "not enabled": the alert at every start from then on. That it read 1
     only after a Restart is a guess, not confirmed; the stand-in parameter RAM was
     not the cause.
   - *The fix* (psmac_board_se.c): the board layer follows DDRA, and while bit 4 is an input the ROM
     reads 0 there (its DDRA shadow is cleared at each RESET, as the VIA's is). The Pi's memory has no
     parity, so the SE is always the plain IIci now. The stand-in parameter RAM's defaults
     (psmac_pram_default.h, shared with the solo board) also lose the testbed IIci's record of a failed
     start-up test ($F0-$F3 00 01 00 02, $F9 01, $FA-$FB 00 8E), which INIT 10 would act on on a Mac it
     took for one with parity. No mending of $F9 on the real chip is needed: with the plain IIci's
     entry, Gestalt 'prty' is 0 and INIT 10 looks no further. `mac.sepa4=1` (testbed) reads bit 4 as 1,
     as the user's SE did.
   - *Verified* in MAME's `macse_ps` (128 MiB, `mac.rtcsim=dead`, System 7.1 from the card's image):
     `mac.sepa4=1` reproduces the alert as the user photographed it, [Shut Down] [Continue], over the
     desktop at the start; without it the Finder and Startup Items come up with no alert, and again
     after a Restart through the remote control's testbed channel.

70. **An SE with nothing on its SCSI bus (20 November).** The se7 kernel on the user's SE, with no internal
   disk and the BlueSCSI taken off, `mac.boot=image` and a System 7.1 image on the card: the address lines
   good, the image found, "startup disk: parameter RAM $78-$7B a5 a5 a5 a5, shown to the Mac as 00 00 09
   80", and then the question-mark disk for good. No "[MAC] PiDisk boot" line. From about five seconds on
   the heartbeats showed the 68k at PC $408076D8, some 640,000 accesses a second to the 5380, the last at
   $5FF010.
   - *Where it sits.* $408076BE-$40807712 is the SCSI Manager's arbitration. It sets the mode register to
     ARBITRATE (`moveb #1,a3@(32)`) and waits for the ICR's "arbitration in progress" bit (`btst
     #6,a3@(16)`, the read at $5FF010), for TimeSCSIDB x 256 reads ($1F4 on the SE: 128,000 reads, a fifth
     of a second); on that timeout it returns 3. But once the bit is up it checks "lost arbitration" (ICR
     bit 5, $408076F4) and a higher ID on the data lines ($408076FC), and either sends it back to the start
     ($408076C2) with no timeout at all. The 5380 sets AIP when it has seen BSY and SEL false for a bus
     settle delay, and LA when SEL is asserted while it arbitrates.
   - *Why.* With the BlueSCSI attached SCSI had always worked: the BlueSCSI terminates the bus. With
     nothing on it the SE's bus has no terminator, the lines float, and a floating SCSI line reads as
     asserted (the bus is active low). Whether the real chip goes round the timeout or the retry without one
     is not known from the log; the PC samples fit either, and the retry would explain "for ever". MAME's
     SCSI bus is always clean, so the testbed had never shown it.
   - *Reproduced* in MAME's `macse_ps` with `mac.sefloat=1`, a testbed option that makes the real 5380's
     registers read as on a floating bus. With BSY and SEL alone read asserted (and AIP never set), the
     ROM timed out its arbitrations and the image still started (the Finder at 37 s): not the hardware's
     hang. With every line asserted and the ICR reading AIP and LA together, the bus seen free a moment
     and lost the next, the hang is the hardware's: the boot record ran ("PiDisk boot: the card's image is
     the startup disk"), and then the 68k sat in the retry at $408076F0-$408076F4, 45,000-56,000 5380
     accesses every five seconds (the bridge's pace), and the image's first block was never read
     (`mac.sescsi=real`, so the chip is used as it is). The hardware never logged the boot record; in the
     testbed the ROM reaches it before the first long scan, on the SE it evidently did not, which the
     different order and speed of the two may explain. Not followed further.
   - *The fix* (psmac_board_se.c, `scsi_probe`). Before the 68k starts, after the address-line test, the
     board layer clears our own 5380's ICR and mode register (it drives nothing then) and reads the
     Current SCSI Bus Status (register 4) and the Bus and Status register (5) eight times, 250 us apart.
     We are the only initiator and nobody has been selected, so a terminated bus, with devices or without,
     reads BSY and SEL false; if every read has one of them asserted the bus is taken as floating, and the
     5380's registers are answered by the kernel as an empty, terminated bus answers them (the solo board's
     5380 with no target): arbitration wins at once, every selection times out (the ROM's 10 ms, item
     50), the pseudo-DMA windows read $FF. One line says which:

         [PSMAC:SE] SCSI: the SE's SCSI bus is terminated and free (bus status $00, bus and status $08): the SE's 5380 is used
         [PSMAC:SE] SCSI: the SE's SCSI bus has nothing on it, or no terminator: BSY and SEL read asserted with nobody arbitrating (bus status $fe, bus and status $08). The board layer answers the 5380 as an empty bus: no SCSI disks this session (mac.sescsi=real to use the chip all the same)

     and with the empty bus every report line adds "SCSI: the empty bus answered here, N selections (none
     answered)". `mac.sescsi=auto` (the default) lets the probe decide, `real` always uses the SE's chip,
     `empty` always the empty bus. The probe runs again after each RESET in auto mode (a dozen bus cycles,
     logged only when the answer changes): the RESET resets the 5380 too. A disk plugged in while the SE is
     on is not supported in any case.
   - *Verified* in `macse_ps` (128 MiB, `mac.rtcsim=dead`, the System 7.1 image as `mac.disk0=`,
     `mac.boot=image`): the floating bus with the probe deciding gave the second line above, the boot
     record, the image's first block at 17.6 s and the Finder at 30.6 s, 15 selections answered "nobody"
     by the end; a terminated bus with the System 7.1 disk on SCSI ID 0 (no `mac.sefloat`) gave the first
     line, the SCSI disk's driver read at 17.9-19.5 s, the image's first block at 29-31 s and the Finder at
     42 s, with the image as the startup disk: the ROM's boot driver ($B34, set at $408015DC) $FFCE, the
     slot driver's -50, where SCSI ID 0's is -33. So `mac.boot=image` with a BlueSCSI attached starts from
     the image, the BlueSCSI mounting beside it. Not yet on the hardware; the line to look for is the
     `SCSI:` one, which names the bus status the probe read.

71. **Sound through the SE's own speaker (20 November).** Until now the ASC was answered in the kernel
   (docs/12) and its samples thrown away; the FIFO was only paced at 22257 Hz so the Sound Manager did not
   freeze (18 November). The user asked for the SE's speaker.
   - *The SE's sound hardware* (MAME's mac128.cpp, `mac_hblank`, `snd_push`, `mac_via_out_a_se`,
     `mac_via_out_b_se`; Guide to the Macintosh Family Hardware for port A bit 3). No sound chip: in every
     horizontal blanking interval the BBU reads one word of a 370-word buffer, the 342 visible lines and
     the 28 of vertical blanking, and the high byte sets the width of a pulse (an LS161 pair counting
     from it), so 370 samples a frame, 60.15 frames a second, 22,255 a second; the low byte is the 400K
     drive's speed PWM. The main buffer starts $300 below the top of RAM ($3FFD00 with 4 MB, $27FD00 with
     2.5, $1FFD00 with 2, $FFD00 with 1: MAME computes it from the RAM size, as it does the screen at
     $5900 below the top), the alternate one $5F00 below, chosen by VIA port A bit 3 = 0 (MAME's macse
     does not model the alternate sound buffer; its Plus does). VIA port A bits 0-2 are the volume (three
     resistors into the amplifier's feedback, so volume 0 is quiet but not silent), port B bit 7 low
     enables the sound; high forces the pulse output to a constant level. `mac.seram=2.5` is now accepted
     (it took whole megabytes only, though its comment named 2.5).
   - *What the IIci side plays.* The startup chime is the ASC's wavetable mode ($801 = 2): $40807040 fills
     the four 512-byte tables with a stepped wave ($40, $FF, $01, $C0 in runs of 64: two periods a table),
     sets $806 to $40 (version 0) and starts the four voices one by one every 300 steps with increments
     $018000, $020000, $018179 and $030000 (9.15 fixed point over a 256-entry period: 260.8, 347.8, 261.8
     and 521.6 Hz, C4, F4, a detuned C4 and C5), then for 30000 steps smooths the tables a little each step
     (the timbre darkens), 14 reads of the VIA apart. The project's ROM patch had cut the steps to 2
     (item 50: nothing played the ASC). System 7's sounds go through the Sound Manager in FIFO mode
     ($801 = 1), both channels, the volume in $806 bits 5-7 from the Sound control panel.
   - *The design* (psmac_board_se.c, "the SE's speaker"). asc_write keeps each channel's FIFO bytes in a
     ring of its own (4096 bytes, core 0 the producer), taking only what the modelled FIFO accepts, so the
     Sound Manager's pacing is exactly as before. The service core plays them: the SE's buffer is written
     in halves of 185 words, byte writes to the high bytes; whenever the SE's play position crosses into
     one half, the other half gets the next 185 samples, the two channels' mean (FIFO mode) or the four
     voices synthesised at the SE's rate as MAME's asc.cpp does (wavetable mode), the last sample held when
     there is none. The play position comes from the SE's vertical blanking: while sound plays the service
     core reads the VIA's IFR every millisecond (no side effects) and takes each CA1 edge, placing word
     342 there (MAME's first line of vertical blanking) and measuring the frame from consecutive edges.
     While the Mac has CA1 disabled (the chime plays before interrupts are on) the service core clears
     the flag itself to see the next edge. At most two halves are written per edge, so the samples go out
     at the SE's own rate whatever the estimate says; past two frames without an edge it runs free. The
     volume is $806's bits 5-7 on PA0-2, PA3 selects the main buffer, PB7 goes low once both halves hold
     fresh samples and high a quarter second after the last one (so no hum between sounds; volume 0 keeps
     it high). Those four bits are driven on the wire only: the service core rewrites every VIA write of
     the Mac's as it executes it (`psmac_se_wire_write`, called from the slot loop in psmac_bus.c) and
     writes the registers itself when they must change, port A through its no-handshake register 15 so
     the VBL flag is never cleared by it; the ROM reads back its own values. On the SE board the kernel
     now gives the chime its 30000 steps back in memory (`psmac_se_rom_fixup`, after the ROM delta, with
     the checksum word adjusted so test 1 passes): about a second on the SE's VIA, 69 s in the testbed.
     `mac.sesound=off` keeps everything as it was: bytes thrown away, the four bits the ROM's, the short
     chime. The report line every five seconds adds `sound: N starts, M samples played, H halves written
     (L late, K held for the next VBL), U underruns, D dropped | speaker on/off, volume v, ASC mode m |
     VBL E edges (C cleared here), frame F us, a half written in W us at most`.
   - *The testbed's transport.* Each bridge request through semihosting is a trap of its own, and a half
     is 185 posted writes: in the first runs a half took longer to write than to play (30% of halves
     "late", sidebands in the beep's spectrum). Posted writes through semihosting are now gathered, up to
     64, into one trap, flushed before any request that wants an answer and whenever the service loop runs
     out of work (psmac_bus.c, `psmac_bridge_flush`); MAME reads the same requests in the same order. The
     GPIO path is untouched: a byte write there is a microsecond or two, a half some 0.3 ms.
   - *Verified* in MAME's `macse_ps` with `-wavwrite` (128 MiB, `mac.rtcsim=dead`, System 7.1 from the
     card's image), the recordings analysed by FFT (Hann window, the strongest peaks). The chime: one start
     in wavetable mode, 1,537,165 samples played over 69 s of the Pi's timer, 3996 VBL edges all seen and
     cleared by the service core (frame measured 16.3 ms), 0 underruns, 0 dropped; in the recording 53 s
     above the threshold (the tail below it as the timbre darkens), peaks at 260.5, 347.5 and 522.5 Hz
     with 695.5 Hz (2 x 347.8), 782.5 (3 x 260.8), 1565, 2086 and 2608 Hz: the chord the table predicts,
     C4, F4 and C5, recognisably the Mac II family's chime. System 7's beep (SysBeep: the Finder's modal
     Find dialog, Cmd-F through the remote control's testbed channel, then a click outside it, twice): two
     more starts in FIFO mode at volume 3, each about 7,500 samples (a third of a second), a tone at
     438-440 Hz with its odd harmonics at 1320 and 2198 Hz, a square wave at A4 (Simple Beep).
     After the first two runs two changes. The service loop called the SE's service only when no request
     of the Mac's was waiting, and the chime polls the VIA without a pause, so the speaker is now also
     looked at after every request (`psmac_se_sound`; a check of the time and the rings when nothing is
     due), which matters as much on the GPIO; and the semihosting batching above. With both (run a5): the
     chime 39 s above the threshold in the recording, peaks 260.5, 349.0 and 521.5 Hz with 695.5, 1042,
     1563.5, 2086 and 2608 Hz; the two beeps 440 Hz with 1320, 2205, 3080 and 3960 Hz (3, 5, 7 and 9
     times 440), the Finder at 71 s instead of 96. Still, in the testbed 42% of the halves were noticed
     late and a half took up to 12 ms to write (MAME drains the FIFO at its own pace), so the recordings
     carry clicks and ±20 Hz sidebands where halves tore; that is the testbed's speed, not the design,
     but the hardware's figures (the report line's "late", "held" and "a half written in ... us") are
     what will tell. `mac.sesound=off` (a6): the old short chime, no sound line, the Finder at 28.6 s;
     the solo board unchanged (Finder at 24.2 s).
     Not verified: anything on the real SE (its speaker, the PWM's polarity and level, the volume steps,
     whether word 342 is where its VBL falls; a wrong guess there costs a quarter frame of margin, not
     sound), sounds other than the chime and the beep, wavetable mode beyond the chime, and the alternate
     buffer (never used: PA3 is held at the main one).

72. **Parameter RAM and clock on the card for the SE's stand-in (20 November).** Item 64's stand-in for a
   clock chip that does not answer kept what the Mac wrote "until the Pi is switched off": AppleTalk, the
   startup disk, Monitors' choices, the volume and the time were lost at every power-off, which the user's
   SE (whose chip reads $FF) now does every evening.
   - *The file.* The solo board's `pram.bin` (item 51) in the card's root: the 256 bytes, then the clock as
     the Mac's seconds since 1904, big-endian, 260 bytes in one cluster. One format for both boards, so a
     card moved between a bench Pi and the SE keeps one set of settings.
   - *Reading* (psmac_board_se.c, `xp_card_load`): before the stand-in serves its first byte, on core 0,
     waiting at most three seconds for the card's first look on core 3, as the solo board does. The clock
     is taken from the file if it is later than the default of 20 November 2026.
   - *Writing* (`psmac_se_pram_take`, called from the card loop in psmac_card.c beside the solo board's):
     a second after the Mac's last change, once the boot is over (the Finder seen, or three minutes), at
     most every ten seconds; and every ten minutes while the Mac runs, so the clock the next start resumes
     from is never older than that. Bytes that are back where they were saved are not a change (the ROM's
     parameter RAM test writes every byte at each start and restores it), a write to the clock is. A copy
     taken while the Mac was writing is left for the next pass (a generation count). `mac.card=ro` reads
     only.
   - *The clock.* The Pi has no clock of its own and the kernel's network stack no time service (no NTP:
     none was added for this), so the clock goes on from the last save: it stands still while the Pi is
     off, and the Date & Time control panel (or the Alarm Clock) sets it, which is saved like any change.
     The four bytes are the "offset" the design asked for: with no time source a saved value and an offset
     against the Pi's timer are the same thing.
   - *The mending rules, applied to what is loaded as to the defaults.* $8A: the file holds what the Mac
     wrote; every read is mended as before (bit 4 cleared, bits 0 and 2 set with more than 8 MiB), so a
     file can never bring back the $38000000 layout. $78-$7B: the Mac's view of the default startup device
     (item 67) is never stored, so the file holds the real choice; the view is laid over it at each start
     as before. The start-up test's failure record ($F0-$F3, $F9 bit 0 with $FB = $8E, item 69) is replaced
     on loading by the defaults' bytes: the ROM writes it again at each start on the SE board (the first
     save held $F0-$F3 00 01 00 02, $F9 01, $FA-$FB 00 8E), and a file from a solo kernel before item 69
     may hold it too. Harmless with the plain IIci's identity (Gestalt 'prty' is 0 and INIT 10 looks no
     further), but not carried from one start to the next. A chip that answers is never replaced by the
     file, the file is not written for it, and the kernel never writes the chip either way.
   - *Lines.* `[PSMAC:SE] stand-in parameter RAM from pram.bin on the card (260 bytes; signature 4e 75 4d
     63; a parity test record replaced by the defaults), clock e7253e5f, from the last save`, or `... the
     defaults (no pram.bin on the card, or no card) ...`, and `[CARD] pram.bin: parameter RAM saved`. The
     clock chip's probe line now says where the stand-in's values come from, and with a chip that answers
     "pram.bin on the card is not used".
   - *Verified* in `macse_ps` with a 64 MB FAT16 card image in QEMU (`CARD=` for run-psmac-qemu.sh, new):
     a first start with an empty card read "pram.bin: not on the card yet" and the defaults, reached the
     Finder at 96 s (the chime's 69 s included) and wrote pram.bin after it (the Mac had changed $46-$48 to
     slot 9's 'PS', $80, and written the test record), clock e7253e5f; QEMU and MAME stopped, a second start
     from the same card (a kernel restart, as at power-on) read it before the ROM's first parameter RAM
     access ("from pram.bin on the card (260 bytes ...; a parity test record replaced by the defaults),
     clock e7253e5f, from the last save") and did not write it again, nothing having changed.
     Two more starts from the same card (a4, a5, and a6 with `mac.sesound=off`) read it the same way and
     left it alone.
     Not verified: the real SE with its card, the ten-minute clock save (no run was that long), a clock
     set from Date & Time, and the solo board with the shared code (its path is unchanged).

73. **The HDMI screen of the SE board, made ready for the hardware (20 November).** The two screens of
   item 62 had run only on QEMU's model of the VideoCore, and item 65 had left one fact unexplained: on
   the user's SE a 32-bit buffer asked for at the HDMI mode's own size stayed black while the firmware's
   boot buffer was on the monitor. The two-screen path was gone through for what QEMU cannot show, against
   the two references:
   - *Upstream Emu68* (`init_display`, support_rpi.c) sends physical size, virtual size, depth 16,
     "allocate buffer" (alignment 64) and "get pitch" in one property message, handing the firmware the
     request's physical address. *Circle* (`CBcmFrameBuffer::Initialize`) sends physical size, virtual
     size, depth, virtual offset (0, 0), "allocate buffer" and "get pitch", hands over the request through
     the VideoCore's uncached alias (`BUS_ADDRESS`: 0xC0000000 on a Pi 2 and 3, as Linux's firmware driver
     does), and writes its 32-bit pixels with an alpha byte of $FF.
   - *What changed.* (1) Every property message, the kernel's and the WiFi module's, now goes through the
     uncached alias (`mbox_send`, alice_net.c's `mbox_tag_`). A bare physical address is the VideoCore's
     L1- and L2-cached view: the firmware may read a stale copy of the request or leave its answer in its
     own cache for a while; item 65's "[WIFI] mailbox tag 10002: error -4 (code 0)", an answer whose code
     word was still 0, looks exactly like that. QEMU models neither cache, and its GPU bus has the same
     aliases, so the testbed cannot tell. The USB DMA (psmac_usb.c, psmac_usbnet.c) already used that alias.
     (2) `psmac_vc_setup` follows Circle's order, adds the virtual offset and, for 32 bits, "set alpha mode:
     ignored" (tag 0x48007; the kernel's pixels carry 0 in that byte, and whether the firmware ignores it
     is otherwise `framebuffer_ignore_alpha` in config.txt; the solo board's 32-bit pictures on the user's
     3B, item 49, say this firmware does, but the tag makes it certain). (3) After any set-up a message that
     only reads (`psmac_vc_query`: physical and virtual size, depth, pitch, offset, pixel order) puts the
     firmware's own view into the log: `[PSMAC:DISP] VideoCore read back: physical 1024x768, virtual
     1024x768, depth 16, pitch 2048, offset 0,0, pixel order RGB: as asked`, or `NOT what was asked`.
     (4) The HDMI screen of the two screens now uses the firmware's boot buffer whenever it already has the
     screen's size, which is the usual case (`mac.screen=` equal to the `hdmi_mode`): it is the buffer the
     splash and the first lines of the log prove on the monitor, and item 65 kept it for the log for the same
     reason. It is 16 bits a pixel, so the conversion writes RGB565 (its path existed for QEMU's 16-bit-only
     answer): the Mac draws in millions, the monitor shows 65,536 colours. `mac.vcdepth=32` asks for a
     32-bit buffer by the new sequence instead, and a `mac.screen=` of another size gets a new buffer either
     way. A Restart's RESET leaves the kept buffer alone, as for the log.
   - *Testbed.* QEMU (solo board, 640 by 480): the read-back says "as asked" for the remade 32-bit buffer;
     every mailbox call (board revision, clocks, power, the WiFi's) answered through the alias. MAME's SE
     with `psmac-se-640x480.declrom` and `mac.screen=640x480`: "two screens ... VideoCore 640x480 x16 pitch
     1280 ...: the firmware's own buffer, kept", read back depth 16.
   - *Not verified on hardware:* whether the black 32-bit buffer of item 65 was the cached mailbox, the
     alpha byte, or something else; the kept 16-bit buffer should not depend on the answer.

74. **The Mac's sound in stereo on HDMI, and a sound card (20 November).** The user asked for the HDMI
   monitor's loudspeakers: the Mac's sound in stereo, the SE's speaker playing as well, from a virtual
   NuBus sound card, the mixing on core 1. Core 1, after the USB start of item 40, only waited in `wfe` for
   the enumerations of item 66; core 0 is the 68k, core 2 the bus, core 3 the services.
   - *Stage A: the ASC on HDMI* (`psmac_audio.c`, new). The ASC is answered in psmac_board_se.c on both
     boards; every FIFO byte it accepts (`asc_write`) is now also copied into a ring per channel for core 1
     (A left, B right, 16 KiB each), beside the SE speaker's rings of item 71. In wavetable mode (the
     chime) core 1 synthesises the four voices from the chip's registers itself, as MAME's asc.cpp and the
     speaker do. A polyphase filter turns the 22,257 Hz into the output's 48,000 (or 44,100, `mac.audiorate=`):
     a windowed sinc, Blackman-Harris, cut off at 0.45 of the input rate, 32 taps by 128 phases in Q15,
     written by `tools/mkfir.py` into `psmac_audio_fir.h` (the kernel keeps no floating point: the JIT owns
     the FP registers). The Mac's FIFO is paced by the Pi's timer (`asc_drain`) and the output by its own
     clock, so the ASC's ratio follows the rings' level, at most a quarter per cent either way, aiming at the
     level the sound started with; a sound starts once the first burst is in. The volume is the ASC's
     ($806 bits 5-7). The chime gets its 30000 steps back on any board whose sound the Pi plays
     (`psmac_se_rom_fixup`).
   - *Stage B: the sound card.* A block and a 32 KiB ring in slot 9's window (psmb.h `struct psmb_sound`,
     $11000 and $12000, reachable in 24-bit mode too): 16-bit big-endian stereo at 44.1 kHz, the Mac
     writing `head`, the Pi `tail`, its volume, state and counters beside them. On the Mac it is **Sound
     PiStorm** (`mac/pisound.s`, an INIT of 2.6 KB in GNU as): a Sound Manager 3 output device component,
     type 'sdev', subtype 'pist', registered at startup when the kernel says something plays the card,
     and named in the Sound Manager's 'odev' preference so that every sound goes there. The shape of an
     'sdev' was read from Sound Manager 3.0's own "Built-in" device in the testbed image (the disassembly
     of its 'sift' resource, for the calls and their order, nothing copied): the dispatcher, the Apple Mixer
     opened in InitOutputDevice with `OpenMixerSoundComponent` (_SoundDispatch $06140018) for the device's
     format (here 'twos', two channels, 16 bits, 44100.0), every source call passed on with
     `DelegateComponentCall`, GetInfo answering 'chan', 'chav', 'srat', 'srav', 'ssiz', 'ssav', 'hvol'
     itself and the mixer the rest, the output device chosen by the preference 'odev' "Output Device"
     (`SetSoundPreference`, $06340018: the Sound Manager's version, the ComponentDescription, the name).
     A Time Manager task every 10 ms takes the mixer's samples with `SoundComponentGetSourceData` and keeps
     the ring 2,816 frames (64 ms) ahead of the Pi; when the mixer has nothing the task stops and the Pi
     plays out what is queued. The extension runs after the Sound Manager's (its name sorts after "Sound
     Manager") and says in the Pi's log what it did; with a Sound Manager before 3.0 (System 7.1 without the
     extension) it says so and leaves the ASC in charge, and when the kernel plays no card (`mac.audio=off`,
     `mac.sndcard=off`, a DVI monitor) it points the preference back at the built-in device.
   - *Output.* `mac.audio=hdmi` (the default): IEC 958 subframes into the HDMI block's MAI FIFO, by a DMA
     channel looping over six blocks of 384 frames (two IEC 958 blocks, 8 ms each), channel from the
     firmware's mask (GET_DMA_CHANNELS), DREQ 17. Set-up after Circle's HDMISoundBaseDevice and Linux's
     vc4_hdmi (read, not copied): the MAI's divider from the HSM clock (CM_HSMDIV over PLLD), N = 128 fs /
     1000 and CTS from the pixel clock (PLLH over 10) for the sink's clock recovery, the MAI format,
     thresholds, channel map and configuration, the audio packet configuration, the audio InfoFrame in
     packet RAM 4, the PHY's random generator on, the MAI enabled. It needs HDMI mode, not DVI: when the
     packet RAM's enable bit is off the log says so and recommends `hdmi_drive=2`, now in the release's
     config.txt. `mac.audio=jack`: the 3B's, 3B+'s and 3A+'s headphone socket, PWM0 and PWM1 on GPIO 40
     (right) and 41 (left), ALT0, the PWM clock at 250 MHz from PLLD, one range per sample, DREQ 5, after
     Circle's PWMSoundBaseDevice; refused on a Zero 2 W, whose GPIO 41 is its WiFi's power. Core 1 fills
     every block behind the one the channel is on. In the testbed (`mac.audiodump=PATH`) the output goes
     to a host file through semihosting, timed by the Pi's timer: QEMU has no HDMI audio, and its DMA model
     ignores DREQ, so a looping channel would never end there.
   - *Core 1.* `psmac_audio_start` runs before the USB start, so the chime, which comes inside the USB
     start's second, is heard; the USB code's waits on core 1 feed the output (`delay_us`), and between
     enumerations core 1 sleeps in `wfe` woken by core 3's `sev` and by the generic timer's event stream
     (bit 13 of the counter, every 0.85 ms on a Pi 3), so it does not spin. Before an enumeration the ring is
     filled with silence, so the DMA plays nothing stale while core 1 is away.
   - *The SE's speaker plays the card too:* core 1 hands the speaker a mono feed at the ASC's rate (two
     frames averaged), which `snd_next` mixes with the ASC's bytes.
   - *Verified in the testbed* (`tools/audiocheck.py`, new: Hann-windowed spectra, standard library only):
     solo board, QEMU, `mac.audiodump=` and `mac.audiotest=3`: 625 blocks of 384 frames every five seconds
     (48,000 Hz); the kernel's test tone 440 Hz left and 660 Hz right with the channels apart by 86.5 and
     87.2 dB; the chime (wavetable, 294,000 samples at 22,257 Hz under QEMU's slow VIA) with peaks at 261.3,
     347.8 and 524.0 Hz and 695.5, 1043, 1565, 2086.5 Hz, item 71's chord. Sound PiStorm under Sound
     Manager 3.0 in the System 7.1 image: "[MAC] PiSound: Sound Manager 3.0; the sound card ... registered
     and chosen as the output device"; `mac.audiotest=card` gave 441 Hz on the left with 1323, 2205, 3087
     Hz and 882 Hz on the right with 2646, 4410 Hz (square waves, odd harmonics), apart by 85.6 and 96.1 dB;
     two System beeps (the Find dialog, a click outside it, through the remote's testbed channel) came
     through the card, 27,648 frames, 440 Hz with 1320, 2200, 3080 Hz, the volume set by the Sound Manager
     ('hvol' $006E006E), nothing through the ASC. With `mac.sndcard=off` the extension pointed the
     preference back at the built-in device and the beeps went through the ASC to the output. MAME's SE
     (`macse_ps`, 32 MiB, `psmac-se-640x480.declrom`, `mac.screen=640x480`, the same System with PiScrn,
     Sound PiStorm and PiColor): the chime from the SE's speaker and the output together, PiScrn's
     arrangement and restart, the Finder at 427 s with the HDMI screen in millions converted into the
     firmware's kept 16-bit buffer (QEMU's VideoCore photograph shows the desktop pattern there), Sound
     PiStorm registered, and two beeps through the card (33,630 frames, no underruns, 440 Hz with its odd
     harmonics) that the SE's speaker played too (its report line: a third start, "speaker on").
     The first SE run found one mistake: the speaker's feed from the card was fed silence whenever the
     card's ring was empty, which kept the SE's speaker "on" and the service core writing its buffer
     halves for ever, and slowed the whole boot; only frames really taken from the ring feed it now.
     A run with `mac.audio=jack` under QEMU stopped core 1 at the first write to the PWM block, which
     QEMU's raspi3b does not have: with semihosting (the testbed) the kernel now refuses both hardware
     outputs and says so.
   - *Seen on the way, not changed:* on the solo board under QEMU, Sound Manager 3.0's built-in device
     goes on writing the ASC's FIFO after a beep, for as long as the run lasts (the beep's tone, smeared,
     with gaps), with or without Sound PiStorm; through the card the same beeps end after a third of a
     second. The MAME SE of item 71 played them a third of a second long through the ASC. The ASC model's
     FIFO status or the missing ASC interrupt under QEMU's slower 68k is the likely place; to be looked at.
   - *Not verified:* everything on hardware: the HDMI audio registers, N/CTS and the InfoFrame on a real
     monitor, the DMA channel, the PWM jack, the PLL readings; the latency (about 50 ms of output ring plus
     64 ms of card ring) as heard; a long sound's drift control on real clocks.

75. **USB flash drives as disks on the Mac (20 November, evening).** The user's plan for USB on the Mac
   is to keep USB on the Pi and show the Mac each device as something it already understands, through
   the slot-9 card. Keyboards and mice already go that way (item 40). Flash drives were next: a stick
   plugged into the Pi 3B appears on the Mac's desktop while the Mac runs, ejects from the Finder, and a
   stick pulled out does not crash the Mac.
   - *What was there.* PiDisk (`psmac_mbox.c`, `mac/pidisk.s`) publishes up to four units in the window's
     device table, numbered in the order the images were found. The Mac learnt of them only at the
     driver's Open: one drive each, marked "fixed disk in place" (disk-in-place 8), with disk-inserted
     events posted until any drive was read. Its eject did nothing, and nothing could be added later. On
     the USB side, Alice68's enumeration (item 40) gives the hub address 1 and the device on port N
     address 1 + N; a device that is neither a keyboard, a mouse nor a hub it addresses and reads, then
     leaves unconfigured (its error -28); it stops once it has a keyboard and a mouse, leaving later
     ports unreset. A full- or low-speed device behind the LAN9514 gets split transactions, one packet of
     at most 64 bytes at a time on channel 0 (start split, complete split, again on NYET up to eight
     times: `dma_packet` in psmac_usb.c). The wired Ethernet's bulk path (item 49) speaks to high-speed
     devices only.
   - *Where the work runs.* Core 1, which already runs every enumeration (item 66) and the sound feed
     (item 74), runs everything a stick needs (`psmac_usbdisk.c`, new): its work never overlaps an
     enumeration, and core 3 (the mailbox, the card, the network) never waits for a slow stick. Core 1
     claims the sticks after each enumeration, before core 3 is let back onto the bus. It resets the hub
     ports the enumeration did not reach, reads every device's descriptors, and configures each device
     whose interface is class 8, subclass 6, protocol $50 (SCSI over bulk-only transport). Other
     mass-storage devices (CBI floppies, ATAPI) are logged and left alone. Its bulk transfers go on
     channels 3 and 4 and its control requests on channel 5, all by DMA through bounce buffers, as
     psmac_usbnet.c does on 1 and 2. psmac_usbnet.c's probe now leaves the sticks' addresses alone, so
     two control transfers never meet on one endpoint 0. Every wait feeds the sound output.
   - *Speeds.* A high-speed stick goes 64 KB per transfer (512-byte packets). A full-speed stick behind a
     full-speed hub goes 32 KB per transfer (the channel takes 1023 packets of 64 bytes). A full-speed
     stick behind the LAN9514 goes through the hub's translator, one packet at a time on channel 3 or 4,
     with the same steps as `dma_packet`, 8 KB per job.
   - *The protocol.* Each command is a CBW, data and a CSW. The recovery follows the class
     specification:
     - a stalled data stage is cleared and the CSW still read;
     - a stalled CSW is cleared and read once more, and a truncated one is read again;
     - a CSW that is not one, a phase error, a CBW that would not go or a transfer that timed out ends
       in a reset recovery (Bulk-Only Mass Storage Reset, then CLEAR_FEATURE(ENDPOINT_HALT) on both bulk
       endpoints);
     - a GET_MAX_LUN that stalls means one LUN, and a CSW sent in place of the data is taken as the CSW.

     These behaviours were read in Neptune's massstorage class (github.com/rondoval/neptune-usb-stack,
     AROS Public License), as a reference only: none of its code is used. The bring-up runs INQUIRY
     (anything but a disk is refused), then TEST UNIT READY with REQUEST SENSE: a unit attention is
     retried, "becoming ready" waited for, and "medium not present" (a card reader with no card)
     looked at again every two seconds. Then READ CAPACITY(10), and (16) when (10) says 0xFFFFFFFF.
     Sectors other than 512 bytes are refused with a line in the log. MODE SENSE(6) reads the
     write-protect bit; a stick that refuses it is taken as writable, and a write it then refuses with
     DATA PROTECT makes the unit read-only. Last, the first 64 blocks are read.
   - *What the Mac is offered.* System 7 reads HFS, and the Finder offers to initialize any disk it cannot
     read: a FAT stick from the user's PC would be one click from being erased. So only HFS goes to the
     Mac:
     - a bare HFS volume ('BD' in block 2), whole;
     - an Apple partition map's first Apple_HFS partition, as for the card's images;
     - an MBR partition of type $AF, or a GPT partition of Apple's HFS type, whose block 2 holds 'BD'.

     FAT, exFAT, NTFS, HFS Plus, Linux and blank sticks stay on the Pi. The log names the partition
     table and the first partition's type, and says why the stick was not offered. `mac.usbdisk=all`
     offers them anyway, as the whole stick capped at 2 GB, HFS's limit on System 7: for PC Exchange,
     or to initialize a stick on the Mac. `mac.usbdisk=off` leaves sticks alone.
   - *The Mac's side.* A stick is a PiDisk unit with a new flag, `PSMB_BD_REMOVABLE`. `psmb_blkdev`'s
     spare word is now `media`, a count that moves for each disk put into the unit. A unit without a
     disk loses `PSMB_BD_PRESENT`. A stick keeps its unit for the session, so a stick pulled out and put
     back is a disk put back into its drive. The driver keeps a VBL task (every two seconds, after the
     boot-time posting) that does what the Sony driver does for a floppy:
     - a unit with a disk whose drive is not yet in the queue, or whose `media` moved, gets its drive
       (`_AddDrive` from the VBL task: it only links the element into the queue) and disk-inserted
       events until the File Manager reads it, half a minute at most;
     - a unit that lost its disk leaves its drive empty (disk-in-place 0), and Prime answers offLinErr;
     - a stick's drive says "ejectable disk in place" (1), so the Finder ejects it, and Control's eject
       sends `PSMB_BLK_EJECT`.

     Prime now maps the Pi's statuses: no disk gives offLinErr, write-protected gives wPrErr. The
     posting is per drive now, where before any read stopped it for all.
   - *The data path.* Core 3 hands core 1 one job at a time (a read or write of at most 64 KB, or an eject)
     and copies the Mac's data in and out of the job's buffer itself; the command stays unanswered
     meanwhile. A stick can take longer than any card image: a slow write, or a bus being enumerated
     again when a keyboard is plugged in. So the command block's spare word is now `progress`, which
     core 3 moves while a command is pending; the driver's spin (item 52's 100,000,000 turns) starts
     again for as long as it moves. Core 3 bounds a command at a minute. A command the Mac stopped
     waiting for, or one cut off by a 68k RESET, is finished on core 1 and dropped without touching the
     Mac's memory. USB units do not count as the card's images for the startup disk (item 67).
   - *Plug, unplug and bus resets.* Plugging or pulling a stick changes a hub port, and psmac_usb.c
     enumerates again (item 66). That enumeration resets every device, so the same stick (same
     vendor, product and serial number) is checked again rather than offered again. With the same
     capacity it carries on, unnoticed by the Mac. psmac_usb.c's look at the ports now notes every
     port that changed (it used to stop at the first), so a stick taken out and put back between two
     looks is a new disk. A stick no enumeration finds is gone: an ejected one quietly, a mounted one
     with a line saying so. A stick that answers no command three times asks for one fresh
     enumeration. Sticks found no longer count as "a device that is not the keyboard" in item 66's
     retries. Writes: nothing is cached on the Pi, so the Mac's write returns after the stick's CSW.
     SYNCHRONIZE CACHE goes to the stick half a second after the last write, and before an eject; a
     stick that refuses it is not asked again.
   - *Two testbed findings.*
     - QEMU 8.2.2 hung for good on a second `device_add` while the guest's cores ran. Its main loop
       waited in `qmp_device_add`'s RCU drain for a vCPU that never left translated code (gdb's
       backtrace). With the vCPUs stopped around the monitor commands (`stop`, `drive_add`,
       `device_add`, `cont`) it did not happen again in the six plugs that followed.
     - The File Manager's "Please insert the disk" is `_SysError` 30 (dsReinsert), which the crash
       recorder of item 53 printed as a crash record. It is now one `[PSMAC:DISK]` line.
   - *Verified* in the solo testbed (QEMU, System 7.1, `-device usb-hub` with a keyboard and a mouse,
     sticks added through the monitor once the Finder was up):
     - A 32 MB bare HFS stick plugged in was on the desktop at the next screenshot, twenty seconds later
       (`docs/img/system71-usb-stick.png`). A 405,264-byte file dragged from it to the startup disk,
       and the Finder's Duplicate of it on the stick, both read back with hfsutils identical to the
       original (md5).
     - Put Away flushed and ejected it: "[USB] disk 0 ... ejected by the Mac (1605 blocks read, 814
       written): safe to unplug", and its icon left the desktop. The `device_del` afterwards was
       quiet.
     - Pulled while mounted with its window open: the drive went empty, and the Finder's copy from it
       stopped with "error of type -65" (no crash). Plugged back in, the same volume came back and the
       copy went through. Pulled again, Put Away removed it without a complaint.
     - An Apple-partitioned stick mounted its Apple_HFS partition, and kept working across a keyboard
       plugged in meanwhile ("answers again after the bus was reset; the Mac's disk carries on").
       Ejected with Command-E it stayed on the desktop offline. Opening a folder on it then asked
       "Please insert the disk: APM Stick" (`docs/img/system71-usb-stick-insert-again.png`), and
       plugging it back in answered the dialog and opened the folder.
     - A FAT16 stick was logged and not offered. With `mac.usbdisk=all` it was offered, the Finder
       asked to initialize it (`docs/img/system71-usb-stick-initialize.png`), and Initialize made an
       HFS volume hfsutils mounts.
     - A stick alone on the root port, at high speed (512-byte packets, 64 KB jobs): the same copy and
       duplicate test, identical.
     - `mac.usbdisk=off`: the stick left alone.

     MAME's `macse_ps` (128 MiB, `mac.rtcsim=dead`, no SCSI disk, System 7.1 as `mac.disk0=`) still
     started from the PiDisk image with the new driver (the Finder at 89 s, 90 s without USB devices:
     the same) and mounted a stick beside it. MAME's parameter RAM was a copy; the testbed's own was
     not touched.
   - *Not verified:* everything on hardware: a real stick behind the LAN9514, the split path for
     full-speed sticks (QEMU has no high-speed hub), card readers, the write-protect switch, sticks
     larger than 2 TB, the real controller's NAK handling on long bulk transfers, and whether a stick's
     own cache survives a pull after SYNCHRONIZE CACHE.

76. **Speedometer 3.23's "bad F-Line instruction" on the SE, and the level-2 interrupt that outlived
   its source (20 November, night).** The user's SE with the se9 kernel (5799de9), the two-screen
   declaration ROM for 1024 by 768, `mac.image=sys710pi.image mac.boot=image`, the full test image
   from the card and the sound card with Sound PiStorm, ran Speedometer 3.23 from the Utilities folder
   and got "Sorry, a system error occurred. 'Speedometer 3.23' bad F-Line instruction", its System
   Information window behind the alert. The same SE had run Speedometer 4 from the user's own BlueSCSI
   System (7.1.2) on an earlier kernel with one screen (docs/12, rating 90.2). No crash record came
   with the report.
   - *Speedometer's own instructions first.* CODE 1 to 8 of Speedometer 3.23 were disassembled. Its
     FPU code is FADD, FSUB, FMUL, FDIV, FNEG, FSIN, FCOS, FATAN, FETOX, FLOGN, FSQRT, FMOVE and
     FMOVEM in ordinary addressing modes, and its Gestalt glue for older Systems does `fnop; fsave
     -(sp)` and compares the frame's first word with $1F18, $3F18, $1F38 and $3F38, as the ROM's
     'fpu ' does (item 46). There is no PMOVE, PFLUSH, PTEST, MOVE16, FMOVECR or coprocessor
     save/restore; the two words objdump calls PRESTORE and FRESTORE in CODE 4 are entries of a jump
     table. It is item 48's set, all on Emu68's native path. Not an instruction the JIT lacks.
   - *The testbed, as the user had it.* MAME's `macse_ps` with 128 MiB, first the se9 kernel and its
     own declaration ROM, then main (262c344) with `psmac-se-1024x768.declrom`; a copy of the user's
     image on a 256 MB FAT32 card image (`CARD=` for run-psmac-qemu.sh) with `mac.image=` and
     `mac.boot=image`, Speedometer 3.23 added to Startup Items; `mac.audiodump=/dev/null`, because
     without an output the kernel plays no card and Sound PiStorm leaves the ASC in charge ("the Pi
     plays no sound card"), which was not the user's case; MAME's parameter RAM a private copy.
     The first start made PiScrn's arrangement and restarted, the second reached the Finder at 225 s.
     Speedometer was driven through the remote's testbed channel (`mac.remotefile=`): its splash, the
     shareware notice, the four pages of System Information (68030, 68882, 68030 MMU, 131072K; 32-bit
     QuickDraw 2.30 at 1 bit on 512 by 342; System 7.1.6; 32-bit addressing), Run ALL Tests (Command-A,
     the startup disk for the disk test): "The tests are done!", no exception logged, no crash record,
     and a dump of the Mac's 128 MiB afterwards held no nested interrupt frames. Run ALL Tests did the
     same twice more on main (with the counters below). The crash did not reproduce.
   - *What the testbed did show.* Item 48's runaway needs a level the JIT still sees after its source
     has gone, so the kernel now counts the interrupts the 68k takes (ExecutionLoop.c's ProcessIRQ, a
     few counters and no calls; one heartbeat line, `[PSMAC]   interrupts taken: level 1 N, 2 N, 4 N,
     other N; taken again at once N; nested N bytes deep`). "Taken again at once" is the same level
     at the same PC with the same stack: an RTE that went straight back into the handler. "Nested" is
     the same level at the same PC with the stack lower by the same stride each time: item 48's
     frames. On main, at the Finder with Speedometer up, level 2 was taken 2,200 to 6,200 times in five
     seconds, 87 to 95 per cent of them again at once; during Run ALL Tests up to 27,107 in five
     seconds, 26,844 of them again at once. The card's VBL interrupt is 60 a second, 300 in five
     seconds. On the solo board, with the same kernel and image, level 2 was 296 to 300 in five
     seconds.
   - *The cause.* On the SE board the IIci's level 2, the RBV, is the kernel's own
     (psmac_board_se.c): its slot flag follows the card's interrupt line, which the screens' VBLs, the
     network board and the input ring raise and the Mac's slot handlers clear by writing the slot-9
     window, Pi memory, without a bus cycle. The level the JIT tests (INTF.IPL) was stored only by the
     cycles on the wire (the SE's VIA and SCC) and by the service core's poll: 20 times a millisecond
     on the GPIO, once a millisecond in the testbed. The ROM's slot dispatcher ($40806EAA) reads the
     RBV again after each slot handler, and on the solo board that read stores the level since item
     48. On the SE board it did not: item 48's fix added the RBV's refresh for the solo board only
     (`if (psmac_board_solo)`), and its text says the SE board was not affected because each bus
     cycle's reply carries the motherboard's level, which is true of the VIA and the SCC and not of the
     RBV, the kernel's on that board. So after every VBL the level-2 handler's RTE went back into the
     handler, which found nothing, until the next wire cycle or poll; and when the handler returns
     with a deferred task queued and the Deferred Task Manager busy, System 7.1's patch at $6E4 (`andi
     #$F8FF,sr; rts` before the handler's own RTE) takes the stale level inside the handler, a level
     and 44 bytes of stack deeper each time, until the poll: item 48's runaway, which ended in "bad
     F-Line instruction" in Speedometer and in the Finder (items 48 and 53). The test image queues
     deferred tasks from Riptide's stack (its Time Manager task queues the next run while one is still
     going) and from Lagoon EtherTalk. The testbed's quiet network never lined one up with a VBL in
     these runs ("nested 0" throughout), but on the Pi each nested entry costs a fraction of what it
     costs under QEMU, and 50 microseconds of them can run a few tens of kilobytes down an
     application's stack into its heap. The two screens double the VBL interrupts; the earlier
     Speedometer 4 run had one screen and a System without Riptide or Lagoon.
   - *The fix* (`psmac_board_se.c`, `psmac_bus.c`): on the SE board every RBV access stores the level
     as the poll does, the SE's last level from the wire combined with the RBV's, and the wire's
     stores go through the same function (`psmac_se_set_level`) under one lock, so that neither
     stores a level the other has just made stale. The solo board is unchanged.
   - *Afterwards*, the same SE testbed and image: level 2 never more than 319 in five seconds at the
     Finder and through Run ALL Tests (300 while both screens run), again at once 6 to 50 in five
     seconds (level 1 entries in wait loops, mostly, as on the solo board's 3 to 62), nested 0; "The
     tests are done!"; the Finder at 205 s as before.
   - *Not known:* whether the user's crash was this. Their kernel had the hole, their image the
     deferred tasks and their declaration ROM two screens, but there is no crash record. What would
     settle it, from `aurora68.log` on the card (written after every crash record): the `[CRASH]` block
     (the vector, the PC and its region, the stack, the last PCs; item 48's signature is an F-line or
     illegal instruction at a PC in a stack, then a jump into the ROM's error code), and from the new
     kernel the `interrupts taken` lines while Speedometer runs. One thing the testbed cannot show:
     on the GPIO the VIA's level comes back with the cycle that clears its flag, read a moment after the
     write; if the CPLD or the SE's interrupt lines are slower than that read, level 1 could stand
     stale the same way. Level-1 "taken again at once" or "nested" on the hardware would show it.

77. **2560 by 1440, any HDMI size, and declaration ROMs from the card (20-21 November).** The user's new
   monitor is a Dell UP2716D, 2560 by 1440; their SE had run two screens on hardware at 1024 by 768 and
   1920 by 1200 (item 73, the se9 kernel). docs/10, "2560 by 1440" and "Any size", is the long form.
   - *The monitor.* 60 Hz at that size needs about 241 MHz of pixel clock, beyond a Pi 3's HDMI; 30 Hz
     with CVT reduced blanking is about 121 MHz: `hdmi_group=2`, `hdmi_mode=87`, `hdmi_cvt=2560 1440 30 3 0
     0 1`, `max_framebuffer_width=2560`, `max_framebuffer_height=1440`. Whether the monitor accepts it was
     being checked by the user while this was built.
   - *What stopped at 1920 by 1200.* `mac.screen=` (512..1920 by 342..1200); the big region behind
     $90000000, 9 MiB, where 2560 x 4 x 1440 needs 14.1 MiB; the declaration ROM size lists; the remote's
     PNG width (2048). The super slot space has 256 MiB, so the region's address range was never the limit,
     only the Pi RAM behind it: it now grows with the screen (9 MiB up to 1920 by 1200 as before, then the
     32-bit frame rounded up to a MiB, 15 MiB here, 32 MiB at most), so millions fit and nothing was capped
     at thousands. QuickDraw's rowBytes limit ($3FFE) holds 2560 at 32 bits (10,240). The firmware's 16-bit
     buffer is 7.4 MB (4.6 at 1920 by 1200) inside `gpu_mem=64`; a 32-bit one (`mac.vcdepth=32`) adds
     14.7 MB, and `gpu_mem=128` is the first thing to try if its read-back says `NOT what was asked`.
   - *The copy.* Scaled from item 49's 20 ms for a 1920 by 1200 frame in millions, a 2560 by 1440 one is
     some 22 ms of core 3 into the 16-bit buffer and 33 ms into a 32-bit one: at 30 a second, most of the
     core that also runs the disk, the network and the remote. The conversion now measures its bands and
     starts the next frame no sooner than the last one's busy time after it ended (half the core at most);
     frames under 1/60 s are unaffected. The 32- and 16-bit loops into the common buffer depths lost their
     per-pixel branches. The heartbeat's display line gained `VideoCore N frames in 5 s, longest U us in B
     bands`, the number a Pi 3 has to supply.
   - *Any size* (the user's next request, the same evening: a card that follows config.txt). The kernel
     rewrites the video sResource of the HDMI card ($80, slot $A) or of a one-screen card ($84, slot 9) in
     Pi RAM before the Slot Manager reads it: each mode's VPBlock gets the screen's bounds, its rowBytes
     and the vpBaseOffset of its place by the display block's own rule (one function, two users), a depth
     that fits nowhere leaves the list, and the CRC is made again. With no `mac.screen=` the size is the
     firmware's. Checked on the host first: a harness compiling the kernel's `declrom_resize` turned a 1024
     by 768 file into slot $A ROMs byte for byte equal to mkdeclrom.py's own for 640x480, 1366x768, 1920x1200,
     2048x1152 and 2560x1440, and at 4096 by 2160 left out millions (16,384-byte rows) with a valid CRC.
     Rows are padded to a long (1366 at one bit: 172 bytes, not 171); every older size is a multiple of 32
     wide and no existing file changed. The 1-, 2- and 4-bit conversion stopped writing up to seven pixels
     past a row that ends inside a byte. `psmac-se.declrom` (`aurora68-se.declrom`) is the one file a
     container needs; exact-size files are still preferred.
   - *Declaration ROMs from the card's root*, taken before the container's on a tie. The ROM must be in
     place before the 68k's first instruction, and the card has been core 3's alone (psmac_card.c): the one look
     is made on core 0 before psmac_alice_init starts core 3, and card_start takes over the mounted card.
     Only after a container boot, since a bare-ROM testbed run would wait 1.5 s for a card; `mac.cardrom=off`
     ignores the card. A release's card root holds every declaration ROM of the release, among them the
     screenless `aurora68-wifionly.declrom`, which the old rule ("the first unsized file") could have taken
     for a size no file names, leaving the Mac without a screen: a chosen file is now looked into, and one
     without a video sResource goes last.
   - *PiDepth HDMI N* sets the second screen's depth (plain PiDepth sets the main screen's, the SE's one bit).
   - *Tested* (MAME `macse_ps`, System 7.1, QEMU's VideoCore started at the HDMI size with
     `-global bcm2835-fb.xres/yres`, no `mac.screen=`): 2560 by 1440 from the card's `aurora68-se.declrom`
     over the container's 1024 by 768 file, PiScrn's restart, the Finder and PiColor in millions, all six
     depths through PiDepth HDMI; 1366 by 768 at all six depths (rowBytes 172 ... 5464); 2048 by 1152 in
     millions; 1024 by 768 from its own file without a card, unchanged; the solo board at 2560 by 1440 from
     the 640 by 480 file, all six depths into a 32-bit VideoCore buffer; a card with `aurora68-wifionly`
     listed first, where `aurora68-wifi` was taken. QEMU's screendump shears a 1366-wide 16-bit buffer (its
     display's stride is not the 2732 its mailbox reports); the buffer itself, read by the remote, is right.
   - *Not verified on hardware:* the 30 Hz mode on the UP2716D, the firmware's 2560 by 1440 buffer within
     `gpu_mem=64`, the conversion's cost on a Pi 3, the early card look on the real sdhost controller (the
     same code card_start always ran, run sooner, on core 0).

78. **Speedometer 3.23 again: a pointer disposed of twice, and a Memory Manager that looked for it below
   16 MB (21 November).** The user's SE with the se11 kernel (ba004d5) ran Speedometer 3.23 for a long
   time and got a crash record: an F-line exception at PC $FFFFFFFF in "Speedometer 3.23", D1 $07FDA01F,
   D3 $55555555, A0 $00FD7A4E, A3 $4080DB50, A4 $55555599, A6 $00555555, the last PCs in the ROM between
   $4080D3A0 and $4080DC5C. The interrupt watch of item 76 was normal (level 2 about 330 in five seconds,
   nested 0): not that runaway.
   - *The reading, checked against the ROM.* D1.W $A01F is _DisposPtr. Its ROM entry ($4080D3A0) pushes
     $44, its offset in the zone's table of routines, and jumps through $1EE4 to the dispatcher for calls
     that take a pointer ($4080DC2E; handles have a twin at $4080DC6E through $1EEC). The IIci ROM has one
     Memory Manager for both block layouts, 24-bit (8-byte headers, routines at $1E00) and 32-bit (12-byte
     headers, $1F00); low memory $1EFC holds its flags, read from parameter RAM byte $8A at start
     ($4080CC64): bit 2 the mode the Mac starts in, bit 0 the layout used now. The dispatcher checks the
     block in the current layout ($4080DB9C: the tag byte, 0 for a free block, -111; the zone the header
     names; and that zone's first block's back pointer, zone+60 or +56, which must be the zone, -113),
     and when that fails it flips bit 0 and checks again in the other layout, with the pointer masked to
     24 bits and a bus error handler at vector 8 ($4080DB50, which makes a faulting read return 0). If a
     check passes it jumps through the zone's table: the long at zone+32 (0 for $1E00) plus the offset.
     Speedometer's pointer $07FD7A4E was free (tag 0): -111, then the retry at $00FD7A4E. With 128 MB
     that is RAM, and it held $55 bytes, as did the "zone" at $00555555 the header named. The tag $55 says
     nonrelocatable, the zone's back pointer $55555555 masked is $555555, the zone itself: every test
     passed, the table pointer was $55555555, A4 = $44 + $55555555, and the long there, in the IIci's
     I/O space where the SE board answers nothing, read $FFFFFFFF. A pointer disposed of twice, by
     Speedometer or something on its behalf; its other addresses on the stack ($07EF382C, $07EDB028) are
     in its own heap.
   - *Any uniform fill from $40 to $7F does it*, because each value the checks read is the fill masked to
     24 bits, which points at more of the same fill. Zeros do not (tag 0: free). Nor does what the ROM's
     cold-start RAM test leaves on a real IIci (the three-byte pattern $6D $B6 $DB): its tag can be $6D,
     but the back pointer then has to land on the right phase of the pattern 56 bytes on. Where the $55
     came from was not found. Not from the ROM: item 50 skips the RAM test, and nothing else in it writes
     $55555555. Not from the kernel: it writes nothing into the Mac's RAM before the 68k starts but the
     reset vectors. Not in the testbed: QEMU's memory starts as zeros. On the Pi it is whatever the
     DRAM held when the kernel handed it to the Mac (power-on contents or the firmware's), or what an
     earlier session left: the Mac's restart does not clear it.
   - *A real IIci.* With 16 MB or less a pointer's high byte is zero in 32-bit mode, so the retry looks at
     the same address in the other layout and fails as the first check did: DisposPtr returns an error
     and nothing happens. Only with more than 16 MB does the mask send the retry somewhere else, and an
     IIci with 32 MB or more has the same hole, closed in practice by what its memory holds.
   - *Reproduced* with a test program, DisposeTest (`mac/disptest.s`, `make -C mac DisposeTest`, in
     Startup Items): it disposes of a pointer, disposes of it again (-111), then fills the pointer's
     24-bit image, -16 to +64, and $00555555 -16 to +96 with $55 (interrupts off, the bytes saved) and
     disposes of it a third time. The first version allocated its save area after freeing the pointer,
     got the same block back and disposed of a live one; it now allocates everything first. On the
     unpatched kernel (ba004d5) both testbeds stopped exactly where the user's SE did: PC $FFFFFFFF,
     D1 $07F8A01F, D3 $55555555, A0 the masked pointer ($00F487FC on the MAME SE with 128 MB,
     $00F4D164 on the solo board), A3 $4080DB50, A4 $55555599, A6 $00555555; the long at $55555599 reads
     $FFFFFFFF on both boards. (With interrupts off no exception record came; the 68k sat at $FFFFFFFF.)
   - *What the 24-bit retry is for.* It lets one ROM serve both layouts: a block of the other kind is
     found by trying the other way. Whether System 7.1 ever keeps a 24-bit zone while the 68k is in 32-bit
     mode was not settled, and the fix does not depend on it:
     a 24-bit zone lies below 16 MB, and in 32-bit mode the 68k uses an address as it is, so a pointer
     or a handle whose high byte is not zero cannot be in one.
   - *The fix*, a ROM patch the kernel applies at load (`rom/patches/iici.json`, docs/06 §2b): the two
     retries ($4080DC5C, $4080DCCE) jump to stubs in the zero padding at 0x62A40 and 0x62A80, which retry
     as the ROM did except when MMU32Bit ($CB2) is set, the failed check was the 32-bit one and the
     pointer (or handle) is above $00FFFFFF: then the 32-bit check's error is returned (-111 for a freed
     block). 24-bit mode, addresses below 16 MB and every call whose first check passes take the ROM's
     own path. Checksum 36AC1C28. Each item has its `expect`. A container made by make-container.py holds
     the ROM it patched then (369A5A97 and earlier); the kernel now brings such an image up to date too,
     since the delta is a superset of every earlier set (checked for 368FAA5C, 369A8139 and 369A5A97: the
     result is byte for byte the current image), and copies anything else back as it came (docs/06).
   - *Afterwards.* DisposeTest on the patched kernel: the ordinary calls 0, the second DisposPtr -111, the
     third, with the $55 fill, -111 and MemErr -111, and a bad handle above 16 MB with its image filled
     the same way -111, on the MAME SE (128 MB, two screens) and on the solo board (128 MB); both went on
     to the Finder. A 24-bit boot (the MAME SE with 8 MB and parameter RAM $8A bits 0 and 2 clear,
     MMU32Bit 0): the second DisposPtr -111 through the stub's 24-bit path, the Finder. (The solo board
     with mac.ram=8 still started in 32-bit mode, its parameter RAM's $8A being $05.) Speedometer 3.23's
     Run ALL Tests on the MAME SE with 128 MB and both screens: "The tests are done!", no exception.
     Rebased on item 77's kernel, the same SE: DisposeTest as above, then the user's own Speedometer
     4.02 (the one that ran on their SE from its BlueSCSI disk), Run ALL Tests: "The tests are done!".
   - *A card that was never mounted.* That rebased run first found no disk image: "[CARD] SDSC card,
     FAT16 at sector 0, 0-byte clusters". Item 77's early look for declaration ROMs runs only when the
     Pi booted from a container, and card_poll_locked marked it done (early_done = 2) before calling
     card_start, which then took the early look's results, two zeros, for a card nobody had initialised.
     On hardware the look always runs, so only the testbed (a bare ROM as initrd, `CARD=`) met it. The
     mark is now set after card_start (`psmac_card.c`); the run above is with that.
   - *PiDisk's log.* The user's 64 KB log had 1,790 lines, 1,300 of them "PiDisk: prime": Prime logged
     every read and write, through an INFO command each, and pushed everything else out. Prime, Control
     and Status now log only the first six requests after Open, and Prime's errors the first sixteen times
     ("PiDisk: prime error -NNN"). A boot to the Finder and Run ALL Tests now leave five such lines where
     they left about 1,200. The other drivers were already quiet: .ENET stops after 60 lines, PiInput's key
     trace and PiVideo's control trace after 48, the others log at open or once; WiFiMenu's per-call
     log is compiled only with WM_DEBUG.
   - *Not verified on hardware:* the patched ROM on the user's SE; Speedometer 3.23's own double dispose
     (whether the testbed's runs reached it is not known: on the patched ROM it leaves no trace, and
     the testbed's memory holds zeros where the user's held $55); the source of the $55 fill.

## Mailbox and Mac-side software (later the same day)
- The Pi is a **NuBus card in slot $9**: 1 MiB shared window at 0xF9000000 (0x900000 in 24-bit mode)
  and a 64 KiB declaration ROM at 0xF9FF0000, served from Pi RAM (`emu68/src/psmac/psmb.h`,
  `psmac_mbox.c`; design in docs/07, Mac side in docs/09).
- Verified in the testbed: the IIci ROM's Slot Manager accepts the declaration ROM built by
  `tools/mac/mkdeclrom.py`, the Start Manager loads `.PiDisk` from it at boot, the driver's Open
  reads the window and adds a drive queue entry, and **the PiDisk volume mounts on the System 6
  desktop with its own icon** (`docs/img/system6-pidisk-mounted.png`): the driver reposts a
  disk-inserted event from a VBL task until the File Manager reads the volume (events posted before
  the Finder runs are flushed), the Event Manager mounts it, 16 block reads through the mailbox.
  Bugs found on the way: the drive-queue flag bytes (byte 1 is the disk-in-place byte), `_AddDrive`
  takes drive<<16|refNum in D0, Status/Control must leave A1 (the DCE) intact for IODone, and the
  log macro's local labels collided with the caller's.
- An extension-file loader (`PiDisk.bin`) exists too but System 7.1 did not run it in the testbed
  (cause not found; the declaration ROM path is the one that matters and works).
- `.ENET`-interface Ethernet driver (`mac/enet.s`) and a test application (`mac/pinet.s`) are
  written and assemble; the end-to-end test with `tools/pinet.py` is next.
- `.PiInput` (`mac/piinput.s`): keyboards and mice on the Pi become Mac events through the input
  ring (docs/09); key repeat and a scripted testbed session (`mac.inputtest=`) on the service core.
- Testbed timing: the bridge's idle wait is now 200 us (was 1 ms); with 1 ms steps System 7.1's
  speed probe occasionally measured zero and bombed with a zero divide.
- Alice68 (the user's Pi 4 Amiga kernel, `reference/Alice68`, GPL-2.0-only) carries the hardware
  drivers to port: GENET, WiFi over SDIO, Bluetooth HCI, VL805 xHCI. The port plan (service code on
  core 3, boards carved inside the slot window, firmware from the initramfs container, interrupts
  as a virtual slot-9 interrupt) is recorded in docs/07.
