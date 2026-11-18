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
