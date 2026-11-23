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

79. **WiFi stopped the SE's service core, and an old declaration ROM on the card (21 November).** The
   user's SE (classic PiStorm, Pi 3B, kernel d9ff341, `mac.console=hdmi`, the early card look on, two
   declaration ROMs and the System 7.1 image on the card) with `wifi.ssid= wifi.psk= wifi.country=XX`
   in cmdline.txt never started: the SE's screen kept its power-on stripes, and the HDMI log repeated
   `[WIFI] starting: starting the WiFi chip (chip 0 rev 0, SD 0 MHz)`, 226 card reads (the early look and
   the FAT), `sys710pi.image: ... 0% read from the card`, no mailbox commands, the 68k in the IIci ROM's
   SCSI selection loop at $4080773E, and `service loop: 0 turns/5 s` with every part at 0 us (one earlier
   heartbeat: card 63,706 us, network 471,020 us). Core 3 had stopped for good. With `wifi=off` instead
   the Mac reached the question mark (below). WiFi had never run on the SE board before; on the user's
   bench Pi 3B (solo board) it had, in releases before the USB start moved to core 1 (item 50, 19
   November).
   - *The cause.* The WiFi is a coroutine on core 3 (alice_net.c): core 3's loop switches into it in
     `alice_net_poll`, and it switches back from its long waits (`an_yield`, through `wifi_udelay` and
     `wifi_relax`). Whether to switch was decided by `in_wifi`, one global for the whole Pi. The mailbox
     call `mbox_tag_` yields while it waits for the firmware, and it is not only the WiFi's: core 1's USB
     start asks the firmware to power the USB controller through it (`alice_mbox`, SET_POWER_STATE with
     its wait flag, psmac_usb.c), and core 0's early card look reads the board revision through it. When
     core 1 waited there while core 3 was inside the coroutine (`in_wifi` 1), core 1 "yielded": it cleared
     `in_wifi`, saved itself into the coroutine's context and loaded core 3's main context, and from then on
     ran core 3's loop on core 3's stack, while core 3 stayed in the coroutine with `in_wifi` 0, where
     every yield returned at once and every wait became a spin. What happened next depends on the order of
     the switches; in the bad orders two cores run on one stack, or a core loads a context while the other
     is still writing it, and core 3 never comes back. The timing explains why it struck now: with the
     early look (item 77) card_start takes the mounted card on core 3's first turn (63 ms) instead of
     initialising it (some hundreds of ms), so the WiFi's start, whose first 170 ms are the radio's power
     sequence of yields and its mailbox calls, coincides with core 1's USB power request.
     A second fault of the same call: it yielded with the Pi's mailbox lock (`vc_mbox_lock`, support_rpi.c)
     held. Every other user of that lock waits for it without yielding (the card's clock rate, the HDMI
     screen's frame buffer calls, core 1's sound): on another core they spun until core 3's loop came back
     to the coroutine, and on core 3 itself one of them would have waited for ever.
   - *Not the cause.* The country: `XX` would only have reached the firmware after the chip had started.
     The pins: the WiFi's GPIO 34-39 (GPFSEL3), 43 and the expander's WL_ON do not overlap the PiStorm's
     GPIO 0-27 (GPFSEL0-2, whole-register writes on the bus core); the SD card's 48-53 are left on the
     sdhost when the early look put them there. The early look left no lock taken (it runs before core 3).
   - *The fix* (alice_net.c, `#ifdef PSMAC`). The coroutine belongs to the core that switched into it
     (`wifi_core`, from MPIDR): `an_yield` and `wifi_udelay` switch only there (`an_on_coroutine`), so core
     1 and core 0 simply wait in the mailbox call, bounded by its limits. The mailbox call never yields with
     the lock held: the coroutine takes the lock with `spinlock_try_acquire` and yields between tries (at
     most a second), then waits for the answer without yielding for at most 100 ms (the firmware answers in
     well under a millisecond), reads the answer before releasing the lock (the buffer is shared by every
     core that calls it), and is not asked three more times after a timeout; other callers keep their 2 s.
     `mbox_busy` (the -9 that `alice_mbox` returns) now means "someone holds the mailbox". psmac_card.c's
     card clock no longer uses support_rpi.c's unbounded `get_clock_rate`: the core clock is asked once
     through `alice_mbox_once`, kept, and 400 MHz is assumed without an answer (a slower card clock, never
     a faster one). A WiFi chip that does not answer says so in plain words, `the WiFi chip does not answer
     (SDIO): WiFi off, the Mac carries on` (or `WiFi chip not recognised: ...`), and nothing tries again on
     its own; PiWiFi's Join or a restart does. `wifi.country=` must be two letters A-Z; `XX` is reported as
     the instructions' placeholder and the firmware keeps its default.
   - *The watch* (alice_psmac.c, called from the bus core, which kept going here). Core 3's loop marks each
     part of its turn (`mailbox`, `card`, `network: the WiFi's turn`, `network: the Ethernet`, `USB`,
     `display`, ...); when it has made no turn for 2 s the bus core prints `[PSMAC] WATCH: core 3's service
     loop has made no turn for N s: it is in "network: the WiFi's turn" (the WiFi: "pins and power")`, again
     every 10 s, and `core 3's service loop turns again, after N ms` when it does. The heartbeat's service
     line gains `the WiFi's longest turn U us, at "..."` when one of the WiFi's turns between two yields took
     20 ms or more, the part of "network" that is the WiFi's.
   - *The early look* (psmac_card.c) is bounded too: the root's walk stops after a second, and a card that
     does not answer or has no FAT partition is left as if untouched (early_done 0, state 0), so card_start
     starts it afresh with its own reset on core 3 and the container's declaration ROM serves meanwhile.
     789e265's fix (the early look's results taken only when there was one) is kept; it also explains the
     user's `mac.cardrom=off` boot of d9ff341, which reached the question mark: with no early look the card
     was never initialised on hardware either.
   - *The question mark with `wifi=off`.* The user's card root held an older release's declaration ROMs
     beside the two new ones: aurora68.declrom, -WxH, -wifi, -wifionly, -wifi-WxH (9,016 bytes, an old
     build), -se-WxH, -se and aurora68-512x342.declrom. On the SE board aurora68-512x342 and
     aurora68-wifi-512x342 rank the same (one-screen, the SE's size), and a tie went to the last one listed:
     the old WiFi-only file, whose PiDisk entry has no boot record (262c344 added it), so the Slot Manager
     had nothing to start from the image and the Mac showed "?" though the kernel had the image ready.
     declrom_choose now looks into the file it is about to take, as it already did for a screen: one without
     PiDisk's sRsrcBootRec (entry 6 of sResource $80) goes behind every file that has one and is taken only
     if nothing else will do, with a `***` line saying the Mac cannot start from a disk image; at the same
     rank the full card beats a file with "-wifi" in its name (it serves Pis without a wired port too, item
     49's rule drops its wired card there), then the card's root beats the container, then the last listed;
     and one line lists every other file and why it was passed over (`chosen over: aurora68-wifi-512x342.declrom
     (its PiDisk has no boot record (an older file): no startup disk from an image), aurora68-se.declrom (the
     two-screen card), ...`).
   - *Test options.* `mac.vcslow=US` makes alice_net.c's mailbox call take the answer no sooner than US
     microseconds after the request, as a real VideoCore's comes (QEMU's comes at once, so no caller ever
     waited in that loop in the testbed, and the race could not happen there). `mac.cardfail=early|all`: the
     card does not answer (1.5 s, as a missing one), for the early look only or for every start.
   - *Reproduced, then fixed* (QEMU, solo board, a container with the 3B's WiFi firmware so that the start
     runs, no WiFi chip: QEMU's Arasan host has nothing on it). The old kernel with `mac.vcslow=500` alone
     booted (the USB start's one request came before the WiFi's start); with `mac.vcslow=500
     mac.mboxstress=15` as well it stopped as the user's SE did, the same lines: `[WIFI] sleep clock: ...`
     as the last WiFi line (the expander's WL_ON request, the first mailbox call of the start), `starting:
     starting the WiFi chip (chip 0 rev 0, SD 0 MHz)`, `service loop: 0 turns/5 s, longest: mailbox 0 us,
     card 0 us, network 0 us, ...` from then on, and the stress's own closing line never printed (core 1
     was lost too). The new kernel with the same words: `the WiFi chip does not answer (SDIO): WiFi off, the
     Mac carries on`, the Finder, 9,400 calls from core 1 with 4 that found the mailbox busy and none
     failed, `wifi.country=XX is the instructions' placeholder`. `wifi=off`: the Finder as before. The
     simulated radio (`mac.wifisim=TestNet:... wifi.ssid=TestNet`): joined at boot, "connected".
     MAME `macse_ps` with the user's words (`mac.console=hdmi mac.image=sys710pi.image mac.boot=image
     mac.logsave=10`, WiFi words with `XX`), a container with the IIci ROM, aurora68-512x342.declrom and the
     WiFi firmware, and a card (`CARD=`) holding the image, aurora68.declrom, aurora68-se.declrom,
     aurora68-512x342.declrom, aurora68-wifionly.declrom and an aurora68-wifi-512x342.declrom built without
     PiDisk's boot record, in that order: aurora68-512x342.declrom from the card taken, `chosen over: the
     container's aurora68-512x342.declrom (the card's root wins a tie), ..., aurora68-wifi-512x342.declrom (a
     WiFi-only card ...)`, the WiFi given up, `PiDisk boot: the card's image is the startup disk`; with the
     HDMI screen as a second screen (no `mac.console=hdmi`) and `mac.vcslow=500 mac.mboxstress=15`,
     aurora68-se.declrom, the image as the startup disk and the Finder at 190 s (the testbed's FAT32 card has
     512-byte clusters, and the image came in slowly). A card with only aurora68.declrom and the old file:
     `aurora68.declrom ... taken`, the old one passed over for its missing boot record; with only the old
     file: taken, and the `***` line. The watch printed `WATCH: core 3's service loop has made no turn for 2
     s: it is in "card"` and `turns again, after 4252 ms` in those runs: card_start's first look at a 96 MB
     image in 512-byte clusters (the FAT chain walked on core 3), which the user's card, with 32 KB
     clusters, did in 63 ms. `mac.cardfail=early` with the card: the early look failed after 1.5 s, the
     container's declaration ROM served, card_start initialised and mounted the card on core 3 and found
     the image, the Finder; `mac.cardfail=all`: no card, the Finder.
   - *Not verified on hardware:* the WiFi start on the SE (the chip's power through the expander, the Arasan
     host beside the PiStorm), the firmware's real answer times in the mailbox call (the 100 ms limit), and
     the watch on the HDMI console. The race itself was shown only with the stress option; on the user's Pi
     the USB start's single power request, which waits for the controller's power, is enough by the timing
     above.
   - *On the real SE, the same afternoon (kernel 34283e9, "se12"):* with the WiFi words back in
     cmdline.txt the Mac started, the WiFi joined the owner's network, and the owner found the Mac's network
     "way better" for latency than before. The SE was running both screens (the HDMI one at 2560 by 1440)
     with the image on the card as its startup disk.

80. **IPv6 beyond the link lost on the SE's WiFi: the receive filter asked for too early (21 November).**
   With se12 the owner reported that IPv6 from the browser to hosts on the Internet no longer worked,
   where on 19 November Internet Explorer had reached an IPv6-only site over the bench Pi's WiFi
   (riptide docs/MAC.md). Reading the WiFi's command handler (`alice_net.c`, AN_CMD_FILTER): the Mac's
   `.ENET` driver asks for all multicast as soon as a multicast address is registered (`mac/enet.s`
   set_filter), and Riptide registers IPv6's all-nodes and solicited-node groups when it opens its
   interfaces. The handler applied that to the chip only if the chip was already up, and otherwise
   answered and forgot it, leaving the Broadcom firmware at its default of no multicast. On the bench the
   chip was up before Riptide opened; on the SE, since item 79, the WiFi comes up later than the Mac's
   extensions. Without multicast no router advertisement arrives (no global address, no default route)
   and the router's neighbour solicitations for the Mac's address go unanswered. The wired port was never
   affected: the LAN9514 path passes all multicast. The wish is now kept and applied when the chip
   comes up (`wifi_filter_apply`, logged "receive filter as the Mac asked before the chip was up").
   Built, not testable in QEMU (no chip; the simulated WiFi keeps its own filter). To check on the SE:
   in the Riptide Command Terminal, `ifconfig en1` should show a global IPv6 address beside the link-local
   one, and `ping6` to an outside IPv6 address should answer.
   - *Confirmed on the real SE the same afternoon* (kernel a99cd0f, "se13", over the WiFi): the owner
     reported "That fixed it": IPv6 from the browser to hosts on the Internet works again, as it did on the
     bench Pi on 19 November.

81. **PiDisplay: the HDMI monitor's EDID, its resolution and rotation chosen on the Mac (21 November).**
   With the UP2716D on the SE (item 77) the user asked for a Mac program to choose the HDMI resolution and
   rotation, read the monitor's details and see which resolutions suit it; the design (EDID through the
   mailbox, a file of the kernel's own that config.txt includes, a Pi restart with a keep-or-revert
   question, rotation in the copy) was agreed first. docs/10, "PiDisplay", is the long form.
   - *The EDID.* The firmware's property tag 0x00030020 hands out the EDID block by block
     (`psmac_vc_edid_block`). `psmac_edid.c` (pure C; `tools/edid_test.c` runs it on the host in `make -C mac
     check`) parses the base block, the CEA-861 extension (VICs, short audio descriptors, speaker
     allocation, the HDMI vendor block) and judges every mode against a Pi 3's pixel clock: 162 MHz
     (Raspberry Pi's documentation names 1920x1200 at 60 Hz reduced, 154 MHz, as the top mode before the
     Pi 4; the firmware lists 1600x1200 at 60 Hz, 162 MHz; `mac.hdmiclock=` moves it). Too fast offers CVT
     reduced blanking at the mode's refresh, 50 or 30 Hz; VESA's own CVT figures check the integer formulas
     (173.00, 241.50, 154.00 MHz). QEMU has no monitor, so `mac.edidtest=sample` gives a made-up 27-inch
     2560x1440 HDMI monitor with sound (built in code, not copied from any real EDID), `=FILE` any EDID.
   - *The file.* config.txt is never written. One line at its end, `include aurora68-display.txt`; the
     kernel writes that file (the old one as `.bak`) through the card writer, with VFAT long-name entries
     when it is new (`lfn_create`, psmac_card.c), and reads config.txt and the file on core 0 before the
     68k starts, so it knows the mode in force, whether the include is there or overridden, the rotation
     and whether a new mode waits to be confirmed.
   - *The restart.* `PSMB_INFO_DISPRESTART`, then the Shutdown Manager's restart; at the 68k's RESET the
     kernel saves the log to aurora68.log, waits for the card to be idle (the disk image's changes too) and
     pulls the watchdog as upstream's housekeeper does. After it PiScrn, now staying in the system heap when
     there is something to ask, puts up "Keep this resolution?" on the SE's screen from a GetNextEvent
     filter, counting down 15 seconds; Revert or silence puts the .bak back and restarts again. The kernel
     reverts by itself 25 s after the question appeared without an answer, or `mac.displaywait=` (300) s
     after its start if nothing asked.
   - *Rotation* is the kernel's: the copy into the VideoCore buffer turns the picture (tiles of 32 columns
     for 90 and 270), the Mac's screen is H by W, and at a Mac restart the HDMI card's declaration ROM is
     rewritten from the loaded file for the turned size before the Slot Manager reads it again.
   - *PiDisplay* (mac/pidisplay/), the 1992 guidelines throughout: a document window that grows, zooms to
     its screen and scrolls, one column or two; Monitor, Current, Resolutions, Rotation, Sound and
     Connection; Revert and Restart to Apply; Apple, File, Edit and Display menus with Balloon Help;
     Copy and Save Report As; the window's place in a preferences file, put back on a screen that exists.
   - *Tested* (MAME `macse_ps`, System 7.1, a FAT16 card image, `mac.edidtest=sample mac.hdmisim=1`): the
     sample's 17 modes judged and logged; PiDisplay on the SE's screen and zoomed to two columns on HDMI;
     2560x1440 at 30 Hz applied through a Pi restart, the question left to count down (reverted, the Pi
     restarted, 1920x1080 back), then applied again and kept; rotation 90 and 270 with a Mac restart each,
     and 2560x1440 turned 270 (the Mac's screen 1440x2560). Under QEMU the turned copy took 36-100 ms a
     frame against 17-24 unturned, 47-56 ms for 1080x1920 turned 90 once its bands were made twice as tall, and 380-520 ms for the same in the
     next run (QEMU's figures for a turned copy jump about; docs/10). The testbed's remote file channel loses its answers after QEMU's Pi
     restarts (docs/10).
   - *Not verified on hardware:* the EDID tag on the Pi 3's firmware, the UP2716D's EDID, the firmware with
     a missing included file, the watchdog restart with the SE on the bus, the turned copy on a Pi 3.

82. **PiPlay: MP3 files decoded on the card (21 November).** With the Mac's sound in stereo on the SE's
   HDMI monitor (item 74, heard through a Dell monitor's headphone socket), the user asked for MP3s. System
   7.1 on a 68k has no MP3 player, the test image has no sound-file player at all, and third-party Mac
   software is not to be fetched; a 68000-class Mac could not decode MP3 in real time in any case. Some
   sound cards of the period carried a DSP for such work; this one has core 1 of a Cortex-A53.
   - *The decoder.* minimp3 (github.com/lieff/minimp3, CC0, commit 1428237, `minimp3.h` only, unmodified,
     with its licence and a README naming the commit and checksum in `src/psmac/third_party/minimp3/`),
     compiled once in `psmac_mp3.c` with the kernel's own flags. It needs `memcpy`, `memset`, `memmove`
     and `memcmp`, which the kernel has; Ubuntu's compiler defines `_FORTIFY_SOURCE`, which pulled in
     `__memcpy_chk`, so the file undefines it. Its scratch is 17.8 KB of stack (`-fstack-usage`), within
     core 1's 64 KiB. It works in single precision: the kernel keeps floating point off core 0, where the
     JIT owns the vector registers, but core 1 runs no JIT and the global `-ffixed-v19`... reservations
     hold in this file too. Big-endian: the bit reader takes bytes, and a host harness run under
     `qemu-aarch64_be` (freestanding, raw system calls) decoded the test MP3s to within one LSB of the same
     source built for x86-64, with the scalar code and with NEON alike. The scalar code is used
     (`MINIMP3_NO_SIMD`): the NEON intrinsics were written for little-endian lanes, and the scalar decoder
     decodes 104 s of 192 kbit/s stereo in 0.14 s on the x86 host, which leaves a Cortex-A53 at 1.2 GHz
     (an estimate, some ten times slower per core) at one or two per cent of a core.
   - *The stream channel* (psmb.h struct psmb_stream, $11100, beside the card's block; a 44 KiB byte ring
     at $71000, the free space between the log's ring and slot $A's declaration ROM, so 24-bit systems
     reach it at $971000). The Mac writes the file's bytes as they are; core 1 copies them at once into
     2 MiB of its own (87 s of 192 kbit/s MP3, 12 s of CD-quality PCM), so the Mac fills that far ahead and
     a held menu, a dialog or another program in front costs nothing; then one slice of work per poll of
     core 1's loop: one MPEG frame decoded, or up to 1,152 PCM frames converted, and only while fewer than
     8,192 decoded frames wait. A slice is the longest core 1 is away from the output's DMA blocks and the
     USB work it shares the core with. The decoded frames go through a resampler of their own (the card's
     windowed-sinc filter, the ratio from the stream's rate) and are mixed with the ASC and the card's
     Sound Manager ring, so beeps sound over the music; the SE's speaker is now fed at the output's rate
     from what the card plays (before, from the card's frames at 44.1 kHz), and from the stream only with
     `mac.sestream=on` (below). The head the
     Mac writes is used only once it has stood for 50 µs: the 68k's stores come from the JIT on core 0
     with no barrier between the bytes and the head. The Xing/Info frame LAME and ffmpeg put first gives
     the length; its LAME tag's encoder delay and padding are dropped (delay + 529 samples, as minimp3's
     own player does), so a file starts at its first sample; without one, the length comes from the bit
     rate. Commands START, PAUSE, RESUME and STOP fade over 256 frames, so pausing does not click; a 68k
     RESET stops the stream (`psmac_card_reset`). Playback starts with two MPEG frames decoded and half a
     second of PCM or 48 KB of MPEG in hand, and starts again on those terms after an underrun rather
     than stuttering. mac.audio=off: the block says nothing plays and PiPlay says so.
   - *PiPlay* (`mac/piplay/`, C with PiLog's kit in `mac/c`, its own glue for the File Manager's HFS
     calls and the Folder Manager). It recognises a file by its content (an ID3v2 tag or an MPEG frame
     header confirmed by the next; "FORM"/"AIFF" or "AIFC" with 'NONE', 'twos' or 'sowt'; "RIFF"/"WAVE"
     with PCM or format $55), shows the title and artist of the ID3 tag (Latin-1, UTF-16 and UTF-8 into
     Mac Roman), and follows the Guidelines of 1992 as the user asked: a resizable, zoomable window whose
     contents follow its size, its place and size kept in "PiPlay Prefs" (Preferences folder or System
     Folder) and put back on the main screen when the screen it was on is gone, standard Apple, File,
     Edit and Controls menus with their keys, an About box and alerts of its own, centred, with the
     default button outlined, suspend and resume, canBackground; a bundle and an icon, so that a file
     dropped on it or opened with it from the Finder arrives as the application's files.
   - *Mistakes found in the testbed, in order.* (1) The hand-written Folder Manager and Apple Event glue
     did not keep D2, which a Pascal trap may change and GCC keeps live (a loop counter): the first
     PiPlay hung in its handler-installation loop; the glue now saves D2 as mktraps.py's does. (2)
     TextWidth's trap word was taken as $A88D, which is CharWidth; $A886 is right (the status at the
     right of the progress line was drawn off the window's edge). (3) A resize left the old grow box and
     label behind: the update now erases its region first. (4) The SIZE flag for "high-level-event aware"
     was set as $0002; it is bit 6, $0040. With it right, the Finder launched PiPlay with a document in
     Startup Items and PiPlay stopped at once with "bad F-Line instruction", before it logged anything
     after its start; without a document it ran. Rather than chase it, the Apple events went: the
     Finder hands an application that is not high-level-event aware its files the System 6 way
     (AppParmHandle), and PiPlay takes them so on both systems: a document with creator 'PIPY' in
     Startup Items then opened and played. (5) The first CD-quality AIFF had a fifth of its time in
     gaps: the Mac fed one 16 KB piece per pass of its event loop, each pass giving the Finder its turn;
     it now reads 32 KB pieces for up to two ticks a pass, and the Pi starts with half a second in hand.
   - *Verified in the testbed* (solo board under QEMU with System 7.1, `mac.audiodump=` and
     `tools/audiocheck.py`; PiPlay driven through `mac.remotefile=` with Standard File and the keyboard):
     the LR check as MP3, AIFF, WAV and AIFF-C 'sowt': 440.0 Hz left alone, 660.0 Hz right alone, 880.0 Hz
     on both, 2.05 s each with 0.45 s between, the other side silent; the sweep (MP3, and 8-bit mono WAV at
     22,050 Hz): 10.05 s of glide, then 1000.0 Hz for 3.05 s, 310, 978 and 3095 Hz in 50 ms windows where
     the glide should be at 312, 986 and 3112 Hz; the minuet (104 s MP3) to the end with no underrun, a
     4.8 s pause (silent 0.3 s after the key) and two System beeps through Sound PiStorm over the music
     (440, 1320, 2200 Hz), nothing clipped; Ode to Joy (MPEG-1 at 32 kHz) and The Entertainer (MPEG-2 at
     22.05 kHz) with their lengths 1:50 and 1:43 from their Info frames; LR check, WAV and sowt with no
     underrun after (5), the 18 MB AIFF still with 9 s of gaps in 104 (the testbed's disk image comes
     through semihosting at about 160 KB/s, CD-quality PCM needs 176); grow, zoom, Close and Open again,
     the About box, `mac.audio=off` (PiPlay says the Pi plays no sound), the document from the Finder.
     The decoder under QEMU's emulation took 30 to 76 per mille of core 1 and at most 11 to 28 ms a
     slice; the same code decodes 104 s in 0.14 s on the x86 host. A host model of the whole channel
     (`psmac_audio.c` and `psmac_mp3.c` built big-endian with a fake Mac, under `qemu-aarch64_be`) played
     every test file to its exact length before the first testbed run. MAME's SE (`macse_ps`, 32 MiB,
     the 512 by 342 declaration ROM, System 7.1 as `mac.disk0=`): the LR check right, PiPlay's window put
     back on the SE's one-bit screen from a place remembered on the 640 by 480 one.
   - *Open: a beep during music on the SE board.* In MAME's SE a System beep while the minuet played
     left the Mac hung inside the beep: the beep's square wave went round in the card's ring for as long
     as the run lasted, PiPlay took no more keys, and the heartbeats showed the 68k in the ROM with its
     interrupt mask at 7. The same beep with no stream playing ended after 0.37 s, and with the stream
     playing but `mac.sesound=off` it ended too and PiPlay went on. The difference is the SE's speaker,
     which the stream kept switched on and fed (psmac_board_se.c's speaker polls the VIA's CA1 flag and
     writes the speaker's VIA bits while it plays). Until that is understood the stream does not feed the
     SE's speaker unless `mac.sestream=on`; the music plays on HDMI or the jack. With that default the
     beep during music ended after 0.38 s in MAME's SE (16,928 card frames), and PiPlay stopped and quit.
   - *Ready for a radio stream* (the next step: an MP3 stream over MacTCP with its ICY metadata stripped
     on the Mac): the channel takes an open-ended stream (`data_bytes` 0, no `eof`), the decoder resumes
     at the next frame header after a gap or damage and counts gaps as underruns, STOP then START needs no
     reset of the Pi, and `title` (128 bytes, with `title_seq`) carries "now playing"; PiPlay already puts
     the ID3 title or the file's name there and the kernel logs it.
   - *Not verified:* everything on hardware: the decoder's load on a Cortex-A53 (the heartbeat's
     `stream:` line says it), the Mac's disk speed for PCM on a real SE's SCSI, HDMI and jack output of
     the stream; the SE-speaker hang above on real hardware.
   - *On the real SE the same afternoon* (kernel c4438d6, "se15", the test image of that day): PiPlay
     played "Ode to Joy" from the image's Music folder, its ID3 title and "MP3, 32 kHz stereo, 160 kbit/s"
     in the window, the progress line running (0:28 of 1:50), the window on the SE's own one-bit screen.
     Heard through the HDMI monitor's headphone socket: "sounds clean".

83. **PiPlay: internet radio over MacTCP (21 November).** With MP3 files playing on the card (item 82) and the
   user's SE on WiFi through Riptide, the user asked for SomaFM, listener-supported radio from San
   Francisco (https://somafm.com/), and for any plain-HTTP MP3 stream. The card already took an open-ended
   stream; what was missing was the Mac's side: TCP, HTTP and the stations' metadata. docs/09, "Internet
   radio in PiPlay", is the long form and has the user's steps.
   - *What SomaFM serves today* (21 November, from this machine's network, which passes plain HTTP through
     a proxy): its channel list (somafm.com/channels.json) and playlists (somafm.com/<channel>.pls) now
     name `https://ice6.somafm.com/<channel>-128-mp3` and three other ice servers; `http://ice2.somafm.com/
     groovesalad-128-mp3` on port 80 still answers 200 with `server: Icecast 2.4.0-kh22-Soma1.7`,
     `content-type: audio/mpeg`, `icy-br: 128`, `icy-metaint: 45000` and the long `icy-name` ("Groove Salad:
     a nicely chilled plate of ambient beats and grooves. [SomaFM]"), all header names in lower case, and a
     burst of some 380 KB (about 24 s) at the start. Asked with HTTP/1.1 the body came in chunks; asked with
     HTTP/1.0 this network's proxy answered "426 Upgrade Required" to SomaFM and to example.com alike, so
     what SomaFM itself does with HTTP/1.0 could not be seen from here (Icecast answers HTTP/1.0 clients,
     which Winamp-era players are). PiPlay therefore asks with HTTP/1.0, undoes chunks if they come
     anyway, and on a 426 asks again with HTTP/1.1. Thirty seconds of the stream captured that way went
     through the parser on the host: 740,625 bytes of audio out of 740,753 of body, one title, and the audio
     a chain of 1,772 MPEG frames with no break (nothing of SomaFM's is kept or distributed).
   - *The Mac's side* (`mac/piplay/radio.c`, `icy.c`, `net.s`; PiPlay 1.1, CODE 66 KB, SIZE 256 KB). MacTCP's
     interface only: `.IPP` opened by name; TCPCreate with a 32 KB buffer, TCPActiveOpen (30 s), TCPSend
     with a WDS, TCPRcv of 8 KB at a time, TCPAbort and TCPRelease; the parameter blocks are the MacTCP
     Programmer's Guide's (rcvBuffLen at +40, wdsPtr at +38 ...). The resolver is found as Apple's DNR glue
     finds it and called the glue's way (C convention, selector first: OPENRESOLVER, STRTOADDR with a
     Pascal result procedure, CLOSERESOLVER at Quit); the stub keeps D2, which MPW C may change and GCC
     does not expect to. The receive's completion routine runs at interrupt time and does the work there:
     the parser (`icy.c`, no allocation, no traps), the audio into the ring, the next TCPRcv; a receive that
     completes before PBControl returns is taken by the outer call's loop, not by recursion, and the loop's
     last look is made with interrupts masked. So the stream goes on with a menu held: in the testbed a
     menu held for 15 s left the tone without a break. The card is started paused and resumed at three
     seconds in hand (the Pi alone starts at 48 KB, three seconds only at 128 kbit/s); Pause keeps the
     connection, and the station goes on into the Pi's 2 MiB. A dropped connection or ten silent seconds
     (TCPRcv's command timeout) bring a reconnection after 1, 2, 4 ... 30 s; a connection that has brought
     64 KB resets the wait. Counts of reconnects (connections lost and made again) and underruns (times
     the card's `underruns` began to rise) are in the window and the log.
   - *The station list* is a TeachText file, "PiPlay Stations" ('TEXT', 'ttxt') in the Preferences folder,
     written at the first start from the built-in twelve SomaFM channels (`http://ice2.somafm.com/<channel>-
     128-mp3`) and read again whenever its modification date changes (PiPlay coming to the front, a click in
     the menu bar). The Radio menu's items are set with SetItem, so a "/" or ";" in a name is not taken as a
     menu command.
   - *The testbed's station* (`tools/radiotest.py`, standard library): an MP3 played as a live stream
     with one clock, Icecast's 64 KB burst, metadata every 16,000 bytes, a new title every 20 s, ICY and
     HTTP answers, redirections, a playlist, AAC refused, and a control port to drop or stall the
     listeners. Its stream, `tools/mksounds.py --radio`, is a steady 1 kHz left and 1.5 kHz right tone at 128
     kbit/s, and `tools/audiocheck.py --breaks` finds what broke it. The first version of that check looked
     for a jump in each sample's residual against the sine and found nothing at a junction where the
     host test knew 158 frames had been repeated: MPEG audio's overlapping windows blend the two sides
     over a millisecond or more, and a blend of two sines of one frequency is still a sine. It now
     measures phase and amplitude every 10 ms (a whole number of periods of both tones) and keeps the
     residual for damage.
   - *Verified in the testbed* (solo board under QEMU, System 7.1, the kernel of item 82 unchanged, PiPlay
     driven through the remote's semihosting channel, `tools/audiocheck.py --breaks 1000,1500` on
     `mac.audiodump=`). With Riptide and Riptide DNR: the resolver found in "Riptide DNR", `radio.testbed`
     looked up, a 302 followed, titles in the window (em dash and accent in Mac Roman) and in the kernel's
     log; 33 s through 33 metadata blocks with no phase step, dip or damaged sample; the server's drop
     answered by a reconnection in 2 s with no underrun and a 40 ms dip at the junction (the cut frame and
     the first new one without its bit reservoir), the tone unbroken after it; a menu held 15 s without a
     break; a 16 s stall: underrun counted, the receive's ten-second timeout, a reconnection, 5.1 s of
     silence; Pause and Resume; ICY 200 OK from a dotted address; a .pls naming https first; seven failures
     in their own words; the station list edited in SimpleText while PiPlay ran and read again at the
     resume event. With Apple's MacTCP 2.0.6 (the user's image as it is): the list written at the first
     start, a dotted station played and reconnected after a drop with the same 40 ms junction; a named
     host found MacTCP's own `'dnrp'` in its control panel, whose StrToAddr answered noNameServer (-23044:
     none configured), now said in those words. All in docs/09.
   - *Seen on the way:* (1) the first driver of the test picked the menu next to Radio (a stray dark column
     at the screen's edge counted as a title) - a test mistake. (2) With no Sound PiStorm in the image, each
     error's beep through the ASC kept the Mac inside the beep for one to four minutes (the solo board's
     ASC beep of item 74), and a Return meant for the error window then reached PiPlay's window and played
     the failing station again; with Sound PiStorm the beeps end at once. (3) After the stall the card
     played 1.04 s of the old connection after the silence: the decoder waits for its 16 KB window before it
     decodes, so an underrun can begin with a second undecoded; nothing is lost.
   - *Not verified:* SomaFM itself (no Internet in the testbed; its server's headers and 30 s of its
     stream were checked from this machine through the host-side parser only), a real DNS lookup through
     Riptide on the SE's WiFi, Apple's MacTCP with a name server, System 6.0.8, the 68k's load on a real SE
     while the completion routine parses at interrupt time, stations other than SomaFM. To send back from
     the SE: every `[MAC] PiPlay: radio:` line from the station's name on, the `[AUDIO] stream:` lines and a
     `[PSMAC]   stream:` heartbeat.
   - *On the real SE the same evening* (PiPlay 1.1 from 58f26bf on the kernel c4438d6, the test image of
     that day, Riptide over the WiFi): the owner reported "Streaming radio works": a SomaFM station
     through the Radio menu, fetched by the Mac over MacTCP, decoded on the Pi and heard on HDMI.
84. **The programs renamed "Aurora68 Xxxx", put into folders and given icons (21 November).** The owner
   asked: "rename all the apps to Aurora68 XXXXX so, PiPlay is Aurora68 Play etc. Put them all into
   relevant folders. Give them new custom icons." docs/09, "The programs and their folders", is the long
   form (the table of old and new names, the folders, the log tags, the icons).
   - *Names.* PiPlay, PiDisplay, PiSlots, PiLog, PiStatus became Aurora68 Play, Display, Slots, Log,
     Status; PiNet, PiWiFi, PiBlue, PiPing, PiColor, PiScreens and PiDepth became Aurora68 Network, Wi-Fi,
     Bluetooth, Ping, Colour (the project's spelling everywhere else), Screens and Depth; the extensions
     PiDisk, PiMach, PiScrn, Sound PiStorm and "Wi-Fi & Bluetooth" became Aurora68 Disk, Machine, Screen
     Setup, Sound and Aurora68 Wi-Fi & Bluetooth (the old name with the prefix: the menus are both). The
     Finder names, window titles (the assembly programs' titles were "PiWiFi - the Pi's WiFi" and the like;
     now the name alone, and PiStatus's length byte, 23 for 25 characters, which cut its title short, is
     gone with it: the lengths are counted now), About boxes, the Apple menu's About items, Aurora68
     Display's Balloon Help, alerts, the Stations file's comments and the HTTP User-Agent
     (`Aurora68-Play/1.1 (Macintosh)`) say the new names; the kernel's messages that name a program too
     (`Aurora68 Display chose ...`, `is Aurora68 Screen Setup installed?`, the simulated radio's "S / J in
     Aurora68 Wi-Fi"). Kept: creator codes and file types, the drivers' names, the declaration ROM's
     sResource names, the mailbox, the sources' file names, the drivers' own log lines and the kernel's
     tags. The build products are `mac/build/Aurora68-Play.bin` and so on (no spaces, for make and the
     shell; the MacBinary header carries the Finder name). The programs' log lines are `[MAC] A68 Play:`,
     `[MAC] A68 Sound:` and so on, short because the kernel keeps 127 characters of a line.
   - *Preferences.* The three programs with a preferences file read the old one when the new is missing
     ("PiPlay Prefs" for "Aurora68 Play Prefs"...) and write the new one; the radio's "PiPlay Stations", which
     users edit, is renamed (_HRename) to "Aurora68 Play Stations" at the first start, so that there is one
     list. Aurora68 Depth skips the "68" of its own name before it takes the last number ("Aurora68 Depth HDMI
     32"); the old way of reading the last number would have given 16 for "Aurora68 Depth 16" too, but 68
     for "Aurora68 Depth", which only fell back to 8 by luck.
   - *Order of the extensions.* System 7 runs extensions in the order of their names. "Sound PiStorm" was
     named to sort after Apple's "Sound Manager" (item 74); "Aurora68 Sound" sorts before it. Rather than a
     name outside the scheme, the extension now waits: finding a Sound Manager older than 3.0 it copies its
     whole code into the system heap and hooks the GetNextEvent filter (jGNEFilter), which runs the copy's
     start once the Sound Manager says 3.0, after every extension; then it registers the device as before.
     The first build of this crashed the Finder with an illegal instruction in the system heap, its PC in
     the middle of a message string (the kernel's crash record printed the bytes): `lea init_start(%pc)` and
     `bsr init_start` named the global symbol, which GNU as leaves to a linker as a relocation, and
     `objcopy -O binary` of the object put 0 in its place, so the copy began 398 bytes into the code and the
     filter's address pointed into text. A local label at the same place fixed it; `objdump -r` of every
     assembly program now shows no relocations.
   - *Folders.* `tools/mac/aurora68-layout.py` (standard library and hfsutils, each call with a HOME of its
     own, as hfsutils keeps its current volume in `$HOME/.hcwd`): `stage` for the release's `Mac/` folder,
     `disk` for Aurora68-Utilities.img, `install` for a startup disk (extensions into System Folder:
     Extensions, the folder "Aurora68" with Aurora68 Play and its Music folder, Display, Network, System;
     the old extensions taken out, the old programs too with `--remove-old`, sound files typed MPG3/AIFF/WAVE
     with the creator PIPY by `--music`, the desktop database deleted by `--rebuild-desktop`). hfsutils'
     `hls` answers 0 for a file that is not there, so the helper reads its output instead.
     `tools/mkrelease.sh`, `make -C mac install` and `dev/tools/mkfullimage.sh` use it.
   - *Icons.* `tools/mac/mkicons.py` draws every icon from nothing, one-bit first: a rounded tile, a night
     sky with an aurora across its top, a glyph below; puzzle pieces for the extensions; pages for Aurora68
     Play's MP3, AIFF and WAV documents and the preferences. ICN#, ics#, icl4, icl8, ics4, ics8 each, in
     the system palettes; a bundle (signature, BNDL, FREFs) in every program; `docs/img/aurora68-icons.png`
     is the sheet. The three extensions that share the creator 'PSMC' carry a common icon in their bundle
     and their own as a custom icon (ID -16455, hasCustomIcon), since the desktop database keeps one icon
     per creator and type. The old PiPlay icon (`mac/piplay/mkrsrc.py`, a loudspeaker and a note) is gone.
   - *IPv4 or IPv6* (the owner's addition the same hour, done here because it touches Aurora68 Play's
     radio): the counts line ends with "— IPv6" when MacTCP's remote address is one of Riptide's stand-ins
     (198.18.0.0/15, RFC 6535), "— IPv4" otherwise, and the log's "connected to" line says so; the middle
     dot first tried showed as a box, Geneva 9 having none. NPO Radio 2 (dual stack) joined the built-in
     list for `mactcp ipv6 prefer`.
   - *Tested* (the solo board under QEMU and MAME's SE once, System 7.1, a copy of the owner's test image
     with the set installed by `aurora68-layout.py install --remove-old --rebuild-desktop --music`, a
     "PiPlay Prefs", "PiSlots Prefs" and "PiPlay Stations" of the old kind put into its Preferences folder
     first, the kernel of this change, driven through the remote control's semihosting channel). The Finder
     rebuilt its desktop and showed every icon: in colour on the solo board's screen, where a copy of the
     program named "Aurora68 Depth 16" in Startup Items had set thousands of colours by its name
     (`docs/img/system71-aurora68-folders.png`), and at one bit on the SE's screen
     (`docs/img/se-aurora68-folders.png`); the MP3 files in the Music folder with their document icon. A
     double-click on "Minuet in G.mp3" started Aurora68 Play with it (`a file from the Finder`, `playing
     "Minuet in G.mp3"`), its window where "PiPlay Prefs" had put it and the volume from it (`the preferences
     of PiPlay, its old name, read`), the Radio menu holding the old list's three stations (`radio: "PiPlay
     Stations" renamed "Aurora68 Play Stations"`, `read: 3 stations`); on quitting, "Aurora68 Play Prefs" was
     written. On the SE the window's place from the 640 by 480 screen was found on no screen and the window
     put on the main one. Aurora68 Slots came up where "PiSlots Prefs" said. Every program started from its
     folder with its name in its window's title (Display, Screens, Network, Wi-Fi, Bluetooth, Ping, Slots,
     Status, Log), About Aurora68 Play and About Aurora68 Slots said the new names (the About box of Aurora68
     Play was then made taller: "and NPO Radio 2" had pushed its last line out; a last run showed it whole,
     and the radio's line ending "0 underruns — IPv6"), and the log said `[MAC] A68
     Machine: Gestalt mach now 5`, `[MAC] A68 Disk: ...`, `[MAC] A68 Network: started`, `[MAC] A68 Ping: ...`,
     `[MAC] A68 Slots: slot 9 "PiStorm-SE"; ...`, `[MAC] A68 Display: ...`. Aurora68 Sound waited and then
     registered: `A68 Sound: no Sound Manager 3.0 yet; waiting ...`, then `A68 Sound: Sound Manager 3.0; the
     sound card ... registered and chosen as the output device` once the Finder took events (on the SE, with
     no sound output in that testbed, the second run said so instead). The radio over IPv6: a station at
     `http://v6only.testbed/stream` (pinet's IPv6-only name, relayed to `tools/radiotest.py`) through Riptide
     gave `radio: connected to 198.18.0.1:80 (IPv6 through Riptide's stand-in)` and "IPv6" in the window
     (`docs/img/system71-aurora68-play-ipv6.png`);
     `http://radio.testbed:8000/stream` gave `connected to 192.168.68.1:8000 (IPv4)`.
   - *Not verified:* the icons on a real SE and on real HDMI; System 6.0.8 (no custom icons there: the three
     'PSMC' extensions show the common one); the extensions' new order with the owner's other extensions;
     NPO Radio 2 itself (no Internet in the testbed).
85. **The remote control lists the Mac's windows and photographs one (21 November).** The owner shows
   results by photographing the Mac's screens; they asked for the Mac's windows as a list and a
   screenshot of the window they choose. docs/13, "Windows", is the long form.
   - *Read by the kernel* (`emu68/src/psmac/psmac_windows.c`). The Window Manager's WindowRecords are in
     the Mac's RAM, which the kernel reads directly, so no Mac software is involved. The first version
     walked WindowList ($9D6) and on the solo board under System 7.1 found one window, the Calculator's,
     with the Finder's and PiLog's windows on the screen: under System 7 WindowList is only the
     current application's layer. A RAM dump through the remote's file channel (16 MB in 1.5 s, by a
     debugging request made for the day and left out of the commit) showed the layers: a layer is a
     WindowRecord whose `windowPic` field is its first window and whose `dataHandle` is its parent,
     with one window definition for all of them; $A90 holds the current layer; the root holds one
     layer that holds the applications' layers front to back; a layer's `refCon` is the Process
     Manager's record of its application (type and creator at 4, the layer at $70, a handle at $54 to
     the application's saved low memory with its CurApName at $38 and the layer again at $58). None of
     this is documented, so each step is checked (the parent chain ends at a root within six levels, a
     layer's parent link is the layer it is listed in, both back links agree before a name is taken)
     and anything that does not hold together falls back to WindowList. Two things the first reading
     got wrong: `hilited` is $FF, not 1, under System 7.1 (the record check rejected it), and MemTop
     ($108) is no bound for pointers, since the Process Manager leaves it at 2.8 MB on a 16 MB Mac (the
     whole list came back empty); the bound is now the RAM the kernel gave the Mac, 8 MB in 24-bit mode.
   - *Safety.* Pointers even and inside RAM before they are followed, handles masked in 24-bit mode
     ($CB2), purged handles skipped, regions checked, every loop bounded, the list read until two
     readings agree (up to five). On the host the same file ran against the dump with 20,000 random
     corruptions (pointers bent into loops, bytes set to $FF) under AddressSanitizer: no hang, no read
     outside the dump.
   - *The picture* (`psmac_remote.c`, `psmac_display.c`). The streaming PNG encoder now also composes a
     rectangle of the desktop from the screens' own frame buffers, each at its depth and colour table,
     grey where no screen is, clipped to the screens, with `scale` 2 to 4 doubling pixels. Each screen
     keeps its own screenshot parameters now, and the state endpoint looks at a screen without starting
     a picture (until today a /state during a picture of the HDMI screen switched that picture to the
     first screen's frame buffer half way).
   - *Endpoints and tools.* `GET /windows`; `GET /screenshot?window=ID|title=TEXT|screen=N|rect=l,t,r,b`
     with `content=1` and `scale=`; the page's Windows panel; `aurora68.py windows` and `screenshot
     --window/--rect/--content`; the MCP server's `list_windows` and the screenshot tool's `window`.
   - *Tested* in QEMU (the solo board, System 7.1 in 32-bit and in 24-bit mode, System 6.0.8 with the
     Finder alone and with MultiFinder) and MAME's SE with two screens (the HDMI screen in thousands and
     in millions), through the file channel and over HTTP, with the MCP server and the page in headless
     Chrome; docs/13 "What was tested" has the list. Under System 6's MultiFinder only the front
     application's windows are seen: there are no layers, and WindowList is swapped with the rest of an
     application's low memory. The encoder needed two more match distances for doubled pixels (the SE's
     grey desktop at scale 2: 780 KB before, 30 KB after). On the real SE the same evening `aurora68.py
     windows` from a laptop over WiFi listed Aurora68 Log and five Finder windows with their applications
     (the remote on the Mac's address, port 8068: item 86).

86. **The remote control on a real WiFi network: the kernel's own address unreachable (21 November).** The
   owner turned the remote on over WiFi. The kernel took an address of its own as designed (the Mac's MAC
   address, a second IPv4 address by its DHCP client identifier, announced by mDNS: the laptop resolved
   `aurora68-pi.local` to it), but nothing reached it: pings timed out, HTTP failed at once, and the
   laptop's ARP entry for that address stayed "incomplete" while the Mac's address beside it resolved to
   the shared MAC. The access point never delivered the ARP request for a second address on that
   station (many allow one IP address per wireless station, with proxy ARP or address binding from DHCP);
   the testbed's simulated radio has no such rule, which is why it passed there (item 61 stage 2). With
   `remote.wifi=shared` the kernel takes TCP port 8068 on the Mac's own address, and the remote answered at
   once. That is now the WiFi's default; `remote.wifi=own` asks for the second address on networks known
   to allow it. The wired port is unchanged (the kernel's own MAC address and IP). The heartbeat has a
   `[REMOTE]` line, printed when anything changed: the address and mode, frames in, frames queued and
   sent, ARP requests, pings, SYNs and resets, so the step where a connection stops shows in Aurora68 Log
   after the start-up lines have scrolled out of the 64 KB log.

87. **Aurora68 Play: forty stations, in submenus (21 November).** The owner asked for more stations in the
   Radio menu, grouped into submenus, with a list of plain-http MP3 streams that had answered from a test
   server that day (HTTP 200 or 206, `audio/mpeg`) and a list of stations not to add (Australia's ABC, FIP,
   Deutschlandfunk, RAI, Jazz24, Radio Caroline, Linn: no answer over plain http, or refused). docs/09,
   "Groups of stations: the Radio menu's submenus", is the long form.
   - *The file.* "Aurora68 Play Stations" keeps its form and gains headings: `[Group]` starts a submenu,
     `[Group/Sub]` one inside it (one level; the rest of the line is the inner name), `[]` goes back to the
     Radio menu itself, and stations above the first heading stand there. A file without headings, which
     every user had, makes the flat menu it made before; a heading seen again adds to its group (letters of
     either case alike); a group with no station is not shown. The parsing moved out of `radio.c` into
     `mac/piplay/stations.c`, portable C like `icy.c`, so that `make -C mac check` runs it on the host
     (`tools/stations_test.c`: the built-in list, a flat file of the first kind, odd headings, the limits,
     Add Default Stations three ways; 3,934 checks). Limits now 128 stations (was 32) and 48 groups; the file
     is read up to 32 KB.
   - *The built-in list*, written at the first start only: SomaFM ▸ (the twelve), Listener-supported ▸ (Radio
     Paradise Main, Mellow, Rock, Global; KEXP; WFMU; WWOZ; Radio Swiss Jazz, Classic, Pop) and By country ▸
     with nine submenus (Netherlands: NPO Radio 2, 3FM, Klassiek; Switzerland; Austria: Ö1; United Kingdom:
     Classic FM, BBC World Service; France: Radio Nova; Germany: 90s90s Pop; Greece: ERT Kosmos; New Zealand:
     RNZ Concert; United States: KEXP, WFMU, WWOZ, 181.fm The Beatles and Classic Hits): forty stations, the
     Radio Swiss three and KEXP, WFMU, WWOZ in two groups each. Ö is stored as Mac Roman $85, as TeachText
     and the Menu Manager read it. Looked up the same day through a public resolver, only NPO's
     `icecast.omroep.nl` has AAAA records (eight); none of the other hosts, nor the `livestreaming-node-N.srg-ssr.ch`
     servers that Radio Swiss's two redirections end on, has any.
   - *The menus* are the Menu Manager's hierarchical menus (Inside Macintosh V): a NewMenu per group with ID
     140 + its number, `InsertMenu(menu, -1)`, and on the parent item the command character $1B (SetItemCmd)
     and the submenu's ID as its mark (SetItemMark); application submenus must have IDs 1 to 235. Rebuilt,
     after DeleteMenu and DisposeMenu of the old submenus, whenever the file's modification date changes, as
     before. The playing station is checked in every menu where its address stands. Below the stations: Open
     URL… ⌘U as before, and the new **Add Default Stations**.
   - *Add Default Stations* never rewrites the user's file: it appends, after a note, each built-in station
     the file lacks in the same group (or at the top level, so the old flat SomaFM list is not repeated in a
     SomaFM submenu), under its heading and the heading's notes, then reads the file again and says how many
     it added; a second time it says the menu has them all. A file it cannot open for writing is reported
     and left alone. The optional Radio > Stations File… (opening the file in TeachText) was left out: it
     needs the Apple Event Manager, the Alias Manager and a desktop-database search for the editor, and
     Apple events were taken out of this program after item 82's "bad F-Line instruction".
   - *Tested* (the solo board under QEMU, System 7.1, copies of the owner's test image of the day with the
     new Aurora68 Play in its folder and in Startup Items, the kernel of item 84, driven through the remote
     control's semihosting channel; the stations of the testbed group are `tools/radiotest.py` reached
     through pinet as in item 83). The runs:
     (1) No list in the Preferences folder: `written from the built-in list`, `read: 40 stations in 12 groups`;
     the Radio menu showed SomaFM ▸, Listener-supported ▸, By country ▸, a line, Open URL… ⌘U and Add Default
     Stations, and By country ▸ Netherlands ▸ NPO Radio 2, NPO 3FM, NPO Klassiek opened as three levels
     (`docs/img/system71-aurora68-play-by-country.png`); By country ▸ Austria showed "Ö1" with its two dots
     (`docs/img/system71-aurora68-play-oe1.png`; the file written holds $85 there). Choosing NPO Radio 2 in the
     third level started it (`radio: NPO Radio 2, http://icecast.omroep.nl/radio2-bb-mp3`, the resolver opened,
     `looking up icecast.omroep.nl`) and, with no Internet in the testbed, ended in "The name could not be
     found: is the network up?". Add Default Stations: "Your Radio menu has every station of Aurora68 Play's
     own list already." The About box's new last line fits.
     (2) A list of the new kind put in first: `My Station` at the top, `[Testbed]` with the test station,
     `[Testbed/Over IPv6]` with `http://v6only.testbed/stream`, `[SomaFM]` with Groove Salad. Testbed ▸ Test
     tone connected, buffered 5.0 s and played, and was checked in its submenu; Add Default Stations added 39
     (SomaFM's other eleven under a second `[SomaFM]`, which joined the first: one SomaFM submenu, Groove Salad
     first, no channel twice), `read: 43 stations in 14 groups`, the menu then My Station, Testbed ▸, SomaFM ▸,
     Listener-supported ▸, By country ▸; a second Add Default Stations added nothing; My Station (Shoutcast's
     ICY answer) played from the top level. The file taken off the image afterwards: the user's lines
     untouched, then the note and the groups; the only addresses in it twice are the five the built-in list
     has in two groups. In that run, and in a fourth made for it, the drive script's choice inside Over IPv6 ▸
     missed (it measured where the second menu ends on rows the third one shared); item 88's runs, with the
     script mended, opened that third level and played its stations.
     (3) A flat list of the first kind (the old built-in thirteen, a test station first): the flat menu as
     before, the test station played and was checked (the dump: 1000 Hz left, 1500 Hz right); Add Default
     Stations added 27 (Listener-supported and By country; not SomaFM, not NPO Radio 2 again), `read: 41
     stations in 11 groups`, the old stations above the new submenus
     (`docs/img/system71-aurora68-play-added-defaults.png`: Netherlands ▸ NPO 3FM, NPO Klassiek); the file's
     first 1,477 bytes, the user's, byte for byte as before; a second time nothing added.

88. **Aurora68 Play over Riptide's .IPP6 (21 November).** The same afternoon Riptide gained `.IPP6` (its branch
   `mac-port`, commit f592402; its `docs/IPP6.md`): MacTCP's programming model with 20-byte addresses that
   hold IPv6 (IPv4 as `::ffff:a.b.c.d`) and a resolver in the driver, written with Aurora68 Play's
   interrupt-time receive chain in mind. The owner asked for Aurora68 Play to use it, with `.IPP` and the DNR
   kept, untouched, for every Mac without it. docs/09, "Riptide's .IPP6", is the long form.
   - *What changed in `radio.c`.* Gestalt `'ipp6'` (version 1 or later; `_Gestalt` from System 6.0.4 on, glue
     in `net.s`) and then OpenDriver `.IPP6`; else `.IPP` as before. A name goes to `ip6Resolve`
     (asynchronous, port filled in, IPv6 first: with RFC 6724's default the testbed's `fd68::` would come after
     IPv4), withdrawn with `ip6ResolveCancel` if the station is stopped while it runs; a bracketed IPv6
     address or a dotted one in the URL to `ip6StrToAddr`. TCPActiveOpen takes each address in turn at offset
     36 (the TCP6iopb's remoteAddr; its blocks are 130 bytes, so the three static blocks grew from 128 to 132)
     and goes on to the next when an open fails. TCPCreate, TCPSend, TCPRcv and its completion chain,
     TCPAbort and TCPRelease are unchanged, the same bytes for both drivers. The window's IPv4/IPv6 comes from
     the address the open answered with (IPv4-mapped: IPv4); on `.IPP` it is still Riptide's 198.18.0.0/15
     stand-ins that mean IPv6. Over `.IPP` an IPv6 URL is refused in plain words. Every station's start logs
     `radio: via .IPP6 ...` or `radio: via .IPP ...`. Riptide's header is copied into `mac/piplay/IPP6.h`, as
     its licence note allows (radio.c defines `__MACTCP__` first, since it names MacTCP's calls itself). CODE
     grew from 75.7 to 80.2 KB.
   - *Tested* (the solo board under QEMU, System 7.1, copies of the day's test image with this Aurora68 Play,
     a station list with `[Testbed]` and `[Testbed/Over IPv6]`, `tools/radiotest.py` behind pinet as in item
     83). With Riptide built from f592402 in the Extensions folder: `Riptide's .IPP6 is there, interface version
     1`; `http://radio.testbed:8000/stream` (IPv4 only) looked up by `ip6Resolve`, `connected to
     192.168.68.1:8000 (IPv4)`, played; `http://v6only.testbed/stream` (IPv6 only): `v6only.testbed is
     [fd68::80]`, `connected to [fd68::80]:80 (IPv6)`, played, "— IPv6" in the window;
     `http://peer.testbed:8000/stream` (both): `peer.testbed is [fd68::1], 192.168.68.1`, `no connection to
     [fd68::1]:8000, error -23015: trying the next address` (pinet relays its port 8000 over IPv4 only), then
     `connected to 192.168.68.1:8000 (IPv4)`, played; `http://[fd68::80]/stream`: no lookup, `connected to
     [fd68::80]:80 (IPv6)`, played (`docs/img/system71-aurora68-play-ipp6.png`). In each the dump held the
     test tone, 1000 Hz left and 1500 Hz right. With the image's older Riptide (no `.IPP6`): `via .IPP`, the
     IPv4 station played, v6only.testbed played through Riptide's stand-in (`connected to 198.18.0.1:80 (IPv6
     through Riptide's stand-in)`), peer.testbed over IPv4, and the bracketed address was refused (a first
     wording ran past the error window's three lines and was shortened). The three-level menu Radio >
     Testbed > Over IPv6 opened and chose its items (`docs/img/system71-aurora68-play-nested.png`; the drive
     script had to learn that a submenu of the first item opens 6 pixels lower, clear of the menu bar).
   - *Not verified:* `.IPP6` on the real SE and with a real station (no Internet in the testbed: NPO's servers
     are the built-in list's only ones with IPv6), an IPv6 address that does not answer (ipDestDeadErr after
     about 3 s, then the next address: the code path is the one the refused fd68::1 took), System 6.0.8.

89. **Files and programs on the Mac from the MacBook: the Aurora68 Agent (21 November).** The owner asked for "a
   command line on the client for jobs like this. Screen shots. Push files etc". docs/13, "Files and
   applications", is the long form; the window job's `front=1` is in docs/13's "Windows".
   - *Design.* Files need the Mac's File Manager (the kernel cannot write a mounted volume behind its cache, nor
     reach a SCSI disk), so an INIT on the Mac, `mac/piagent` (C, the `mac/c` kit, 22 KB), takes requests from a
     new block in the card window (`psmb.h`: request block at $5A000, a 24 KiB buffer at $52000, the free space
     after the input ring and after the HDMI display block) and does them from the GetNextEvent filter, which
     is application time on System 6 and 7. A request is a full path and an operation; files are opened and
     closed within it, since the filter runs in other programs' processes. The kernel side, `psmac_files.c`,
     serves `/files/list`, `/info`, `/data` (GET and PUT, MacBinary II or raw), `/mkdir`, `/delete`, `/rename`,
     `/apps`, `/apps/open`, `/apps/quit`, streaming chunk by chunk with deadlines; `aurora68.py` gained `ls`,
     `info`, `pull`, `push`, `mkdir`, `rm`, `mv`, `open`, `apps`, `quit`, the MCP server `list_files`,
     `read_file`, `write_file`, `open`, `list_apps`. An icon in the family: a terminal with a prompt and an
     arrow each way, on the extensions' puzzle piece.
   - *Found on the way* (QEMU's solo board, System 7.1, through HTTP and pinet):
     - The first boot hung right after the agent's log line, the PC at odd addresses. The stack held the text
       "startup volume ": the start-up message was built in a 48-byte buffer and is 70 characters. 100 bytes.
     - Then the Finder crashed within a minute (an RTS into the agent's data) and `apps` crashed the program it
       ran in (a CHK in its stack): GetFrontProcess's glue. Apple's interface pushes one more long, -1, after
       its argument (`moveq #-1,D0; move.l D0,-(SP)` before the selector); without it the Process Manager popped
       four bytes that were never pushed. The VBL task's WakeUpProcess, blamed first and taken out for a run,
       came back once the glue was right: without it each request waited for the front program's next null
       event, 120 to 200 ms (a megabyte took nine seconds instead of six).
     - A folder of 120 files listed one twice: the kernel began LIST at index 0 and the agent read 0 as 1.
     - Rename answered bdNamErr (-37): a bare new name in ioMisc is taken relative to the default volume; the
       full new path works. A folder made shortly before the Mac crashed was gone after the restart: the File
       Manager had kept the catalog change in its cache; mkdir, delete and rename now flush the volume.
     - An upload whose last piece was 324 bytes stalled until the 30 s idle timeout: the kernel's TCP sent a
       window update only once two segments' worth had come free. It now updates as soon as a window below one
       segment grows (`psmac_ip.c`). The receive window had been a fixed 8 KiB with everything accepted; it is
       now the file job's room (up to 24 KiB), so a large upload waits in the sender's socket.
     - LaunchApplication from inside the filter came back into the filter with the request still under way:
       the launch request was carried out twice (the second time finding the program it had just launched),
       and with a guard against that the filter spun until the kernel's deadline. The launch is now made after
       the request is answered, like System 6's Launch, which never comes back. A launch started while the
       previous one's process was still starting failed with "the application 'unknown' has unexpectedly quit,
       error 25": the FSSpec it pointed to was overwritten by the next request; each launch has its own now,
       and its Apple event is kept until the next launch.
     - Documents. The desktop database knew none of the disk's applications (PBDTGetAPPL -5012), and after
       each such look the Finder fell into a loop of alerts "an unexpected error ... of type -192"; the agent
       now searches the catalog (PBCatSearch) for an 'APPL' of the creator instead. The System 7.1 Finder
       takes no Apple events (AESend to it: noPortErr, -903), so it cannot be asked to open a document as a
       double-click would. Aurora68 Play takes none either (destPortErr, -906): a document reaches it as the
       Finder information of a launch with an 'odoc' event as launch parameters, which the Process Manager
       translates; a document for a running Play is refused with a clear reason.
     - System 6.0.8 did not run the agent at all: its system heap does not grow by itself, and an INIT that
       needs more than it has is skipped silently. A 'sysz' resource of 64 KB (`elf2init.py --sysz`) fixed it.
       System 6.0.8 reports SysVersion $0607, which the agent's log line and `/state` show as it is.
     - A MacBinary round trip is byte for byte the same but for bytes 16-127 of the resource fork, which the
       Mac rewrites with the file's name, type, creator and a date when the fork is written: that part of a
       resource fork is reserved for the system.
   - *Tested* (QEMU, the solo board, HTTP from the host through pinet and the semihosting channel): System 7.1
     (16 MB, 32-bit): a 1 MB MacBinary file (BINA/A68T, a 5 KB resource fork) pushed and pulled back,
     identical but for the reserved bytes above, its data fork byte for byte, type and creator kept, about
     140-165 KB/s each way; a text file with `--text` both ways; a second push refused (409) and replaced with
     `--force`; a folder of 120 files listed (unique names, 0.6 s); mkdir with and without `-p`, rm of a full
     folder refused, rm of a file and of empty folders, mv within a folder, to another folder, into a folder
     and with a new name; `open` of SimpleText and TeachText (launched; "running already" the second time),
     `apps`, `quit` of both, quit of the Finder refused; `open` of an MP3 started Aurora68 Play with it, and a
     1 MB upload while it played (0 underruns, the remote's screenshots during the transfer); the agent's own
     new build pushed into Extensions with `--force` and the Mac restarted through the remote, several times;
     without the agent, 503 after three seconds for `ls` and for an upload; the MCP server's five new tools.
     `front=1`: SimpleText's window under TeachText's (the agent, `front agent`, then TeachText back with
     `restore`), a Finder window under another (`front click`, the other back on top with `restore`), Aurora68
     Log's window under the Finder's (agent), refused with a Standard File dialog in front. System 6.0.8 (8 MB,
     24-bit, the Finder alone): the same file jobs (the MacBinary round trip, text, overwrite, mkdir, mv, rm),
     `apps` (the Finder), `open` of an application in place of the Finder, a document and `quit` refused (501).
   - *Not verified:* real hardware and its speed; System 7.5 and later; MultiFinder under System 6; a volume
     other than the startup disk; files over 16 MB.

90. **The test11 check: the Finder bombed while rebuilding its desktop, and a quit that could not be sent
   (21 November).** The check of the test11 image before it went to the owner found three faults.
   - *The Finder and "application died".* Programs the agent opened (SimpleText, Aurora68 Play) brought the
     Finder down when they quit, "bad F-Line instruction" (a jump to `$F8408026`), but only while the Finder
     was rebuilding its desktop file, which it does on the first start of a new image (the image is built
     with `--rebuild-desktop`); after the rebuild the same steps were fine, and Play started from Startup
     Items quit cleanly at any time. The cause is the launcher: LaunchApplication makes the program it is
     called in the new process's launcher, and the agent calls it in the GetNextEvent filter of the program
     in front, nearly always the Finder. A launcher whose SIZE resource asks for it (the 7.1 Finder's flags
     are `$7BE0`, acceptAppDiedEvents among them) is told of its child's death by an `'aevt' 'obit'`
     high-level event; with that flag cleared on a copy of the Finder, the crash went away. The 7.1 Finder
     takes the event in its own event loop (it has no `'obit'` handler in its Apple event table:
     AEGetEventHandler -1717), so the agent's filter takes it first (`take_obit`): an `'obit'` arriving at a
     launcher after one of the agent's own launches from it has died is accepted (AcceptHighLevelEvent) and
     replaced by a null event, with GetNextEvent's answer false. The filter sees it while the Finder is in
     front, as it is during the rebuild; a Finder in the background does not come through the filter (woken
     with WakeUpProcess, it still never did), so a launched program quit while another is in front during the
     rebuild is still a risk. Asking the Finder to open things itself was tried and dropped: it ignores
     `'aevt' 'odoc'`, and its "open selection" Finder event (`'FNDR' 'sope'`) sent from its own filter crashed
     it during the rebuild too.
   - *A quit that could not be sent.* An Apple event goes out from the program the filter runs in, and one
     that is not high-level-event aware has no port: with Aurora68 Log in front the agent's `quit` answered
     noPortErr (-903), "does not take Apple events", where the same quit from the Finder's filter worked.
     (The 7.1 Finder's -903 recorded in item 89 was probably the same thing.) A QUIT or OPEN request now waits
     until the filter runs in an aware program, and makes one the front program first: for QUIT the program
     to be quit, otherwise the Finder; after five seconds it is carried out where it is (`can_send_events`).
   - *`front=1` behind a message box.* Aurora68 Play's message boxes are windows without a title bar (kind 8),
     not Dialog Manager windows, so the remote clicked behind one. A front window without a title bar now
     counts as a dialog (409). The 504 for an agent whose heartbeat has stopped now says the Mac takes no
     interrupts: here it was the testbed's ASC (item 74's beep that never ends) holding interrupt level 2
     after Play's SysBeep, which the real SE does not have.
   - *Verified in the testbed (System 7.1, the test11 image):* during the desktop rebuild, SimpleText opened
     and quit with Command-Q, and Aurora68 Play quitting after its no-sound box, with the Finder up after both;
     after the rebuild, a quit sent with Aurora68 Log in front, a document opened, `front=1` with restore,
     409 with Play's box in front, the 1 MB round trip.
   - *Not verified:* real hardware; System 6 with MultiFinder.

91. **QuickTime 2.5.1 on the test image, and Sound Manager 3.2 (21 November).** The owner asked for
   QuickTime installed on the disk image, from their own copy of the QuickTime 2.5.1 installer (Apple
   software: it goes on the owner's image only and is never committed).
   - *Installed by Apple's own Installer in the testbed:* the test11 image booted on the solo board, the
     Installer opened through the Aurora68 Agent (`aurora68.py open`) and driven with remote clicks: Easy
     Install onto the startup disk, which quit the other programs, read both "Install Disk" folders and
     installed QuickTime™, QuickTime™ Musical Instruments, Sound Manager 3.2 (in place of System 7.1's 3.0)
     and MoviePlayer (in a QuickTime™ Folder). After the Installer's restart the Mac was shut down from the
     Finder, and that disk became the base the test images are built on.
   - *Found on the way:* the image script made folders from UTF-8 names, so "QuickTime™ Install Disk 1"
     arrived as "QuickTime‚Ñ¢ ..." and the Installer could not have found its archives; the folders were
     renamed with `aurora68.py mv` on the running Mac, and the script now converts folder names to Mac Roman.
   - *Verified in the testbed:* the Mac starts with QuickTime and MoviePlayer opens (its personalisation box
     was cancelled and left for the owner). With `mac.audiodump=`, Aurora68 Sound finds Sound Manager 3.2
     (it waits for the Sound Manager extension, which loads after it) and registers the card as the output
     device; the startup chime, two Finder beeps of half a second each and an MP3 in Aurora68 Play were in
     the recording. No movie was played: there is none on the disk.
   - *Not verified:* real hardware; movies.

92. **An outside code review: six findings checked and fixed (21 November).** Someone sent the owner a
   review of commit 865ebc6 (bus transport, storage, memory mapping, the latest agent change), each
   finding reproduced by its author on a host with the original C function and mocked hardware. Each
   was checked against the code here first; all six were real.
   - *A failed bus cycle reported as success* (`psmac_bus.c`, `psmac_gpio.c`). A cycle the CPLD did not
     finish within 100 ms came back with `$FF..` data and status 0, and any 16 of them over a whole run
     gave the link up. The SE's own bus-error timeout is 265 ms (the BBU, Guide to the Macintosh Family
     Hardware p. 126), and the SCSI chip's pseudo-DMA holds /DTACK until a slow drive has its byte, so a
     cycle the SE would have finished could be cut short and read as `$FF`. The timeout is now 300 ms;
     an unfinished cycle is status 1, counted and logged as a bus error; the link is given up only after
     16 in a row. *Still open:* the 68k is not given a bus error exception for it; it reads `$FF..`.
   - *Failed SD reads handed to the Mac as data* (`psmac_card.c`). After three failed reads the loaders
     marked a 16 KB chunk loaded with zeros in it, and PiDisk or the solo board's SCSI disk served them as
     good blocks. A chunk the card does not give is now also marked failed: a read or write that touches
     it gets an I/O error (PiDisk `PSMB_E_IO`, SCSI CHECK CONDITION, medium error 3/11 or 3/0C), and the
     next access tries the card again unless the Mac has written into the chunk since (`chunk_retry`).
     Write-back of that image stays off, as before. `mac.cardfail=B` (testbed) fails the first read
     covering block B. *Verified:* with `mac.cardfail=1000` the loader logged the failure and System 7.1
     started normally (the Mac never read those blocks); with `mac.cardfail=0` the loader's read of
     blocks 0-31 failed, the ROM's first SCSI read of block 0 retried them, "read from the card on a
     second try", and the Mac started from the image.
   - *USB eject after a failed flush* (`psmac_usbdisk.c`). `sync_cache` only logged a failure: the eject
     went on, said "safe to unplug", and the background flush forgot the writes. It now returns the
     result: a failed flush leaves the stick dirty (tried again half a second later) and refuses the
     eject with an I/O error and "NOT safe to unplug". *Not verified:* no USB stick under QEMU.
   - *A PFLUSH ignored* (`psmac_mmu.c`, `M68k_LINEF.c`). Every PFLUSH within 5 ms of a table switch was
     skipped, meant for the PFLUSHA SwapMMUMode issues straight after its PMOVE to TC (item 9). A
     descriptor changed and flushed in that time kept its old page. The JIT now tells the shim whether
     a PFLUSH is the very next instruction after a PMOVE (An),TC or d16(An),TC (bit 16 of the shim's
     argument), and only that one, within a millisecond of the switch's end, is skipped; every other
     PFLUSH re-walks the tables. *Verified:* System 7.1 on the solo board started and ran Speedometer
     and Octavo with the new kernel.
   - *The programmer's interrupt switch ignored* (`psmac_board_se.c`). On the SE the switch pulls /IPL2
     alone, raw level 4 (table 3-2); the level mapper kept only bits 0 and 1. Level 7 on a 68000 is taken
     on its edge, so the switch is not mapped to a level: a press (bit 2 seen without a break for 10 ms)
     asks once for the NMI the remote's `nmi` already uses (`INTF.ARM_err`), which brings up the debugger
     prompt (item 59). *Not verified:* needs the real switch.
   - *The agent taking another program's death notice* (`mac/piagent/agent.c`, item 90's `take_obit`).
     It accepted the first `'obit'` once any watched child of that launcher had died, without looking at
     whose death it was. It now reads the event's process serial number: the agent's own launch, the
     event ends there; another child's, the event is posted back to the same program unchanged
     (PostHighLevelEvent), and the watch stays for the agent's own event.
   - The review's other points went to the floppy and serial work (item 93): the IWM's timing against
     the speaker refill's 185 byte writes, and evidence that floppy and serial work at all.

93. **The SE's floppy, its modem port and the IWM's waits behind the speaker (21-22 November).** An outside
   review of the SE board (item 92 took its other findings) asked for evidence that the 800K floppy (the
   SE's IWM) and the serial ports (its SCC) work through the board layer, and named a risk: "Its
   bus-owning core can also perform 185 consecutive speaker-buffer byte writes without checking for a new
   CPU request inside that loop. That creates a plausible delay between IWM polls. Capture worst-case poll
   gaps with sound on and off." The owner cannot try either on the SE yet. Everything below is MAME's
   `macse_ps` (System 7.1 from the card's image as `mac.disk0=`, `mac.boot=image`, 32 MiB,
   `mac.rtcsim=dead`, `psmac-512x342.declrom`).
   - *A test program:* **Aurora68 IO Test** (`mac/iotest/`, `make -C mac iotest`; not in the release). Put
     in Startup Items it waits for a volume named Floppy, reads HOSTFILE (put there on the host), writes
     MACFILE (64 KiB of a fixed pattern), flushes, reads it back byte for byte, unmounts, ejects, waits for
     the disk to come back and compares again; then opens .AOut/.AIn at the speed in its own name (9600 when
     there is none), sends "A68SER <speed> BEGIN", 4096 bytes of a pattern and "A68SER END", and reads 4096
     bytes of a second pattern for up to a minute. Every step is a `[MAC] IO Test:` line with byte counts,
     CRC-32s, result codes and the driver's error flags; if the disk does not mount it reads blocks 0 and 2
     from the .Sony driver itself and logs the result code. "Sound" in its name plays a 50 s tone through
     the Sound Manager (again every 40 s while it waits), so the speaker's buffer is refilled while the IWM
     works. `tools/iotest_host.py` is the far end of the serial line in the testbed.
   - *The floppy drive was never selected* (psmac_board_se.c, `via_data`). With an 819,200-byte HFS image
     in MAME's internal drive the Mac never saw a disk: 157 IWM accesses at boot, then 20 every five
     seconds (the Sony driver's look for a disk), the desktop without a floppy. A tap on MAME's bus
     (Lua, `install_read_tap`/`install_write_tap` on $DFE000-$DFFFFF and the VIA's port A) showed why: the
     ROM's floppy driver, as it starts, writes DDRA with bit 4 clear (`$6F` on the wire), and on the SE port
     A bit 4 is vDriveSel, the choice between the two *internal* drives. A pin the 6522 does not drive is
     left to the TTL input it feeds, which floats high; MAME's 6522 presents it as 1, and `devsel_se_w`
     then connects no drive at all: every sense read came back 1, "no drive". On the IIci bit 4 is the
     overlay, an input after the boot (item 69 reads it for the parity question), so the ROM has no reason
     to drive it. The user's SE read that bit as 1 on one start (item 69), so the same is expected there.
     The board layer now drives bit 4 as an output at 0 on the wire, as it already drove bit 6 (the screen
     buffer); what the ROM reads back is unchanged (item 69's rule for an input bit 4 still applies). The
     second internal drive of a two-drive SE is not reachable (the IIci ROM knows of one internal drive
     and an external one, chosen by the IWM's SELECT line).
   - *After the fix* the drive answers: the sense lines (disk in place, ready after two index pulses),
     the motor, steps, and the IWM's data register, 12,000-16,000 IWM accesses every five seconds while the
     Mac reads. The Start Manager reads the disk at boot and ejects it (not a startup disk), and the Finder
     tries to mount it. It ends in "This disk is unreadable: Do you want to initialize it?".
   - *Why the testbed cannot read a floppy as it is.* MAME's emulated time is pinned to the wall clock
     (docs/06), the disk turns in emulated time, and a bridged cycle costs 45-450 us of wall clock under
     QEMU's semihosting: the tap's timestamps show the ROM's read loop sampling the data register every
     60-200 us, while a GCR byte lasts 16 us. The driver sees one byte in five or ten. The bridge now has a
     time divisor (`PSMAC_BRIDGE_TDIV`, the state register `TDIV`; reference/mame-psmac.patch, docs/06), and
     `mame/floppy.lua` slows emulated time by 64 while the IWM moves data. With it the reads are whole:
     address fields `D5 AA 96 96 9A 96 D9 D6 DE AA` (track 0, sector 2, side 0, format $22: two sides,
     2:1 interleave; checksum $20, the four exclusive-ored), data fields of `96`s (zeros) ending in `DE
     AA`, sync `FF`s, at the IWM's 16 us a byte. The mount still failed. The IO Test's own reads from the .Sony driver
     (the disk put in at 115 s, after the Finder had started the program; divisor 64): block 0 came back
     whole (result 0, 512 bytes, the zeros of a disk that is not a startup disk) in 134 ticks; block 2
     came back with -72, badDCksum, the data field's checksum wrong, with the buffer already holding the
     master directory block's signature `BD` and creation date (`4244E727`). So the bytes arrive and
     decode, and now and then one is lost: under QEMU a bridged cycle is held back 5-15 ms now and then by
     QEMU's own scheduling of its virtual cores (the bus-wait figures below), still 80-230 us of emulated
     time at 1/64, ten GCR bytes or more. A divisor that covered those would stretch a boot to hours. In
     the testbed the floppy is shown through the board layer as far as sectors read from the disk, not as
     a mounted volume.
   - *Not shown in the testbed:* a file written and read back on the floppy, eject and reinsert, and a boot
     from a floppy (no System 6.0.8 floppy among this machine's test assets). The IWM's write path was never
     reached, so MACFILE never existed for `hcopy` on the host (MAME writes the image back on an eject,
     `call_unload`, which the test relied on).
   - *Serial.* MAME's SE (and IIci) machines wire SCC channel A, the Mac's modem port (.AIn/.AOut), to
     the slot named `printer`, channel B to `modem`: `-printer null_modem -bitb socket.127.0.0.1:PORT` is
     the modem port. With nothing attached to channel A the IO Test's write at 9600 completed: 4096 bytes
     and the two lines, CRC-32 a4574272, in 454 ticks (7.6 s; the line itself needs 4.3 s). With MAME's null
     modem there (which asserts CTS, DCD and DSR) the write never completed at 9600 or 57600 (at 9600 the
     Mac took no level-4, SCC, interrupt in the whole run), and what reached the host was not the data: at
     the null modem's 57600 the bytes were `00` and `E0` (each one bit of a slower line, as a receiver at
     the wrong speed sees it); at 9600 a descending run `FE FD FC ...` of 255 bytes, the same in the
     `modem` slot as in `printer`, so not the test's. With MAME's RS-232 loopback in the slot instead (transmit back to receive) the write completed far too fast for the line: the 4132 bytes
     in 144 ticks at 9600 (2.4 s; the line needs 4.3 s) and in 4 ticks at 57600 (it needs 0.72 s), and the
     Mac got back 2064 of them at 9600 and 6 at 57600, with no error flag from the driver (the 2064 match no
     simple part of what was sent: not the first half, not every other byte). So in this testbed the SCC
     moves bytes in both directions, but at a speed that is not the one asked for, and the far end cannot
     decode them. A guess, not checked: MAME's `macse_ps` gives the SCC no TRxC clock (`configure_channels(
     C3_7M, 0, C3_7M, 0)`, as mac128.cpp's SE) where its `maciici` gives 3.6864 MHz on all four clock pins,
     and the IIci ROM's serial driver may take a different clock source from the SE ROM's. Serial is not
     shown working, in either direction, at either speed; the SE itself is the place to find out.
   - *The poll gaps.* `mac.buswait=1` (psmac_bus.c, `bw_report`): core 0 notes when it posts each request
     and how many cycles the wire had seen; the service core, as it takes the request, files the wait and
     the number of its own cycles that went out in between, by whether the SE's speaker is playing
     (`psmac_se_speaker_busy`), for every request and for the IWM's alone, and the heartbeat prints each
     five seconds' half, 99 % and 99.9 % (half-octave steps) and the longest since the start. Under QEMU
     the microseconds are mostly QEMU's own scheduling of its four virtual cores (waits of 5-15 ms with
     *no* cycle of the service core's in front of them); the cycle count is the figure that carries to the
     GPIO, where one byte write is a microsecond or two (item 71).
     The old refill (the same kernel with `psmac_serve_waiting` returning at once; System 7.1 with the
     chime, then the IO Test's tone; 400 s): with the speaker playing, up to **188** of the service core's
     own cycles went out in front of a request of the Mac's (185 of a half, the VBL poll, the VIA's speaker
     bits), up to **174** in front of an IWM access (134 of them came while it played); with the speaker quiet up to **160** (113 in front of
     an IWM access): the SE's screen rows, four of 32 words a call, have the same shape. On the GPIO that is
     some 0.2-0.4 ms, a dozen to two dozen GCR bytes: enough to lose a sector being read, and for a write,
     where the IWM must have its next byte within 16 us, an underrun. With `mac.sesound=off` and
     the old kernel (300 s): at most **128** in front of a request (four rows of 32 words), at most 1 in front
     of any of 107,668 IWM accesses, whose bursts happened not to meet a screen update in that run; in the
     sound run above one did, 113 cycles of rows in front of an IWM access.
   - *The fix* (psmac_bus.c `psmac_serve_waiting`, psmac_board_se.c). The request hand-off of the service
     loop is now a function of its own (`serve_request`), and the speaker's 185 byte writes and the screen
     rows' 32 word writes call `psmac_serve_waiting` after each one: a request that has come is carried out
     there and then, between two of the errand's cycles. The 68000's bus has no notion of who asked, and
     the two never touch the same addresses (the speaker and screen buffers are the kernel's part of the
     SE's RAM). serve_request does not call back into the speaker, so the errands cannot nest. On the
     GPIO a half with the Mac polling flat out now takes up to twice as long (one cycle of the Mac's per
     byte), well inside the quarter frame (4 ms) it has. The same run with the new kernel (400 s): with
     the speaker playing at most **4** of its own cycles in front of a request in all but two windows (22
     and 11 once each, during the startup chime), at most **1** in front of any of 335 IWM accesses; with
     the speaker quiet at most 1 in front of an IWM access (97,249 of them) and 33 once in front of any
     request, the one-time read-back of screen rows 0 and 100 (item 68), which stays as it was. The
     speaker's report line, old against new: halves noticed late 21 % against 12 %; underruns (the FIFO
     empty inside a sound) 0.37 against 1.2 million, with 2.8 against 4.0 million samples played. The
     underruns are the testbed's FIFO pacing over the bridge (item 71: they were there before, and the
     two runs played different amounts); they need the hardware's figures to be judged. A half now takes
     up to 14.7 ms there (4.2 ms before): each cycle of the Mac's let
     in is a semihosting round trip under QEMU; on the GPIO the 185 cycles at most let in add a third of a
     millisecond to the 0.3 ms a half takes. The kernel as merged with items 94-97, once more (240 s, the chime and
     the tone): the Finder at 95.8 s, at most 4 cycles in front of any of 422,376 requests with the
     speaker playing, at most 1 in front of an IWM access.
   - *Not verified:* anything on the real SE: the floppy through the PiStorm's GPIO (now that the drive is
     selected), the IWM's timing there with and without sound, writes, serial at any speed.

94. **No IPv6 station would play on the SE (21 November).** The owner: "can't connect to any IPv6 radio in
   Aurora68 Play". The only stations with IPv6 are NPO's, at icecast.omroep.nl. The streams still answer
   plain HTTP; the name is a CNAME to icecast.npocloud.nl, which has eight or more AAAA records beside its
   A records.
   - *Cause:* Riptide's `ip6Resolve` put every IPv6 address before every IPv4 one and cut the list to the
     caller's array, and Aurora68 Play asks for eight. So the answer held eight IPv6 addresses and no
     IPv4: on a network whose IPv6 does not reach NPO's servers, Play tried the eight, each until it gave
     up, and never fell back to IPv4. Item 88's test name had one address of each family and could not
     show it. Why IPv6 does not reach NPO from the owner's network is a separate question (the Mac's
     log will say which error each address gave).
   - *Fix:* Riptide 7b3c3b1 alternates the families when an answer does not fit (RFC 8305, section 4),
     so the second address is an IPv4 one; Aurora68 Play gives each address but the last 8 seconds to
     connect (15 before), so a dead IPv6 path costs 8 seconds. pinet answers a new name of NPO's shape,
     `many.testbed` (a CNAME, nine IPv6 addresses that answer nothing, the peer's IPv4 address).
   - *Verified in the testbed* (System 7.1, the QuickTime image with this Riptide and Play, a station
     list with `Many (like NPO) = http://many.testbed:8000/stream`, radiotest.py behind pinet): "many.testbed
     is [fd68::901], 192.168.68.1, [fd68::902] ...", "no connection to [fd68::901]:8000 ... trying the next
     address", "connected to 192.168.68.1:8000 (IPv4)", HTTP 200 and the stream playing.
   - *Not verified:* NPO from the owner's network.

95. **A push over the owner's WiFi that stalled and held the agent (21 November).** The owner pushed the
   fixed Riptide (537 KB, MacBinary) into System Folder:Extensions with `--force` over WiFi. The client
   hung; stopped and run again, it was told "another file job kept the agent busy", and every later
   attempt was refused. The log showed `[FILES] push` and then no end to it, about 15,000 frames each way
   in half a minute, 18 dropped, and the SYN count climbing with the client's retries.
   - *Three faults:*
     - **No timeout:** nothing ended an upload whose body stopped arriving (the agent's requests had
       deadlines, the wait for the network did not), so the stalled job held the one job slot until the
       Pi restarted.
     - **Unsafe overwrite:** `force=1` emptied the existing file at the start and wrote into it, so the
       Riptide extension on the owner's Mac was left part-written.
     - **Overlapping retransmissions dropped:** the kernel's TCP took only segments starting exactly at
       rcv.nxt, so a retransmission overlapping bytes already taken was dropped rather than trimmed.
   - *Fixed:*
     - `psmac_files.c`: an upload with nothing new for 20 s ends with 408 "the upload stopped arriving".
     - `force=1` writes a new file beside the old one ("name.part", cleaned up like any new file if the
       upload breaks); only once its information is set is the old file deleted and the new one renamed
       into place. If the old file cannot be deleted (in use), the new one stays beside it and the answer
       says so.
     - `psmac_ip.c` trims an overlapping segment to its new bytes.
     - pinet has `PINET_LOSS=P`, losing P per cent of TCP segments each way.
   - *Tested in the testbed:*
     - With 3% loss, the old kernel and the new one both pushed Riptide whole (13 and 18 s), the new one
       through Riptide.part, delete and rename.
     - The loss alone did not reproduce the owner's stall; what the owner's WiFi does that the testbed
       does not is still unknown. If it happens again, a packet capture on the client (`tcpdump` of port
       8068) will show it.
     - An upload that sent half its body with `force=1` and went silent: after 22 s the client had
       `408 Request Timeout`, "the upload stopped arriving: 268992 of 537984 bytes in, nothing more for
       20 s; the partial file was deleted". Riptide in Extensions was unchanged (537,566 bytes of
       resources before and after), no Riptide.part was left, and the next jobs ran at once.

96. **The Macintosh Plus, 128K, 512K and 512Ke; a first start from blank parameter RAM; a slow card; the
   SCSI ID the testbeds use (22 November).** The owner asked for the Plus as a board beside the SE, and
   then the original Macintosh. Neither owner nor project has one: everything here is the MAME
   testbed's, and the owner's testers will try the hardware.
   - *The testbed machines* (`reference/mame-psmac.patch`, `src/mame/apple/macplus_ps.cpp`):
     `macplus_ps` (the Plus's logic board, its 5380 with pseudo-DMA, the M0110A keyboard with `-kbd usp`,
     which wants `mame/roms/mackbd_m0110a.zip`), `mac512ke_ps` (the Plus without the 5380, 512K) and
     `mac512k_ps` (the 128K/512K board: the 20-byte clock chip, 400K drives, `-ramsize 128K` for the
     128K). None of them needs a ROM dump, the bridged CPU never running the board's own ROM; `macse_ps`
     now fills its ROM region the same way (it had asked for `macse.rom`, which this build machine never
     had, and would not start). `tools/run-fullstack.sh` takes `HD=none` for a machine without SCSI and
     `MAME_EXTRA=` for more options (a half-done edit had left `HD=` ignored for a few hours).
   - *The kernel* (`mac.board=plus`, `128k`, `512k`, `512ke`; `psmac_board_plus.c` beside
     `psmac_board_se.c`, whose SE layer they share): the overlay through VIA PA4, DDRB bits 3-6 left as
     inputs, a software ADB transceiver in place of the SE's ADB chip with the Plus's own keyboard and
     mouse (and USB and Bluetooth input) as ADB devices, SCSI pseudo-DMA by polling DRQ, the board's own
     RAM size for the screen and sound buffers. On the 128K and 512K, whose chip has no extended parameter
     RAM, the clock-chip stand-in answers for all of it from `pram.bin` on the card. Aurora68 Machine sets
     Gestalt: About This Macintosh reads "Macintosh Plus", "Macintosh 512Ke", and plain "Macintosh" on the
     128K and 512K (System 7.1's name for type 1; the owner chose to leave it so). All four reached the
     Finder under System 7.1: the 128K/512K/512Ke at 128 MiB from the card's image (they have no SCSI),
     the Plus at 32 MiB from its SCSI disk; 8 MiB in 24-bit mode crashes QuickTime on every board with the test
     image, so 32 MiB or more for tests.
   - *A first start from blank parameter RAM.* The first 512Ke boot stopped at the question-mark disk
     without opening PiDisk. A trace of $78-$7B showed why: the IIci ROM, finding no valid parameter RAM
     (a Plus's or a blank chip's), writes zeros to the four bytes, then ff ff ff df (SCSI ID 0, its
     default startup device), and reads them back; the view of item 67 kept every write the Mac made, so
     the Mac read the SCSI default and the card's boot record never ran. The view now gives a written byte
     back only to a read of that byte that comes next, with no other access to $78-$7B between (the ROM's
     parameter RAM test, 0x40841DA2, writes one byte and reads it at once); every other read is given
     slot 9. Verified from an all-zero `rtc` file: the card's image to the Finder, "Macintosh 512Ke".
     A real Plus or SE meets this at its first start with Aurora68.
   - *A slow card.* The first run after a power cut (the host's page cache cold) ended in "PiDisk: prime
     error -036" and the question-mark disk: one semihosting read of the image took 0.7 s, longer than
     PiDisk's spin on the done word, and only USB commands moved the progress word the driver then waits
     on. The BLOCK service now moves it before each step that may wait (a semihosting chunk, each card
     chunk `psmac_card_need` loads). Verified with `mac.cardslow=10000` and a 256 MB FAT16 card image
     (`CARD=`): the Finder, with mailbox calls of up to 3.7 s; the same kernel without the bump, prime
     error -036.
   - *The SCSI ID.* MAME puts these Macs' disk at SCSI ID 6. With the card's image published and the
     parameter RAM naming no disk, `mac.boot=auto` asks for ID 0 first; the Start Manager waits for it
     (0x40801460, about 15 s), then takes the first startup disk in the drive queue, the card's, whose
     drive is queued before the SCSI drivers load. So on the SE and the Plus a disk at ID 6 lost to the
     image; moved to ID 0 (`MAME_EXTRA='-scsibus:6 cdrom -scsibus:0 harddisk -hard ...'`) the SE started
     from SCSI and mounted the image after it. This is the order item 67 wrote down; whether a disk at
     another ID should come before the card is the owner's to decide.
   - *The first real Plus (22 November, a tester of the owner's).* System 7.0.1 started from a disk image on
     the card (the Plus's SCSI bus empty, answered by the kernel), keyboard and mouse working; but the pointer
     was jerky and moved on its own. The heartbeat over 114 s: 227,534 polls, 1,180 X1 edges and 49,584 Y1
     edges, 435 a second, so a Y line that chatters or reads that are not the line. The decoder counted a
     step at each X1 or Y1 edge read in RR0 and read X2/Y2 from port B after it, only when X1 or Y1 had moved:
     two edges between polls were no edge, and a port B line that changed between the two reads turned the
     step round. Rewritten as a quadrature decoder: both RR0s and port B read back to back at each poll
     (8,000 a second by default, `mac.plusmouse=`), each axis's two-bit state followed through every
     transition (two a step, the Plus's resolution), a level taken only when two polls in a row agree
     (`mac.plusdebounce=0` for none), a poll with both bits of an axis changed counted and ignored. A host
     simulation (a quadrature mouse with jittered polls): exact up to 3,000 transitions a second; at rest no
     movement with 5 or 20% of reads corrupted; moving with 5% corrupted, 98% of the distance (90% without
     the debounce). The old decoder in the same simulation was 205 steps out after 3,500 a second there and
     back. The heartbeat now counts each line's changes (X1, X2, Y1, Y2), jumps and one-poll glitches, which
     says on the next log whether the Y line itself is noisy. On the tester's machine (a 512Ke, run as
     mac.board=plus, which works because its 512 KB repeats through the Plus's address range): "miles
     better". Its next log: X1 4,782 and X2 4,768 changes, Y1 107,004 and Y2 5,566, and 211,218 one-poll
     glitches filtered: the Y1 line itself is noisy (the mouse's vertical encoder or its wire), and the
     decoder now cancels it.
   - *Still a little sluggish, and a faint whine (the same tester).* The compact Mac's screen was compared
     and sent at 30 frames a second, the HDMI conversion's rate, so the pointer on the glass moved at half
     the display's: now 60 (psmac_display.c; the SE testbed: 300 frames compared in 5 s, was 150). The
     speaker's buffer was left between sounds holding the last sound's 370 samples (or the board's RAM at
     power-on), played 60 times a second behind a switch that may not silence it all: it is now filled
     with one level when a sound ends and before the first ("buffer silenced" in the heartbeat; twice in
     the SE testbed, at the start and after the chime).
   - *Stray words on the 512Ke's screen, and the whine again.* With the screen at 60 frames a second the
     tester saw short dotted marks in the desktop pattern, single words, gone when the pointer passed over
     them: the Pi's copy right, the board's memory not. While no row is waiting, one row a tick is now read
     back and any word that differs written again (the screen every 5.7 s); the first twelve are logged
     with the word sent and the word found, to tell an address line from a data line. The whine stayed
     with the buffer silenced, so it is not the buffer: the mouse's polls, bus reads at a fixed 8 kHz,
     are now spread from half to one and a half periods (mac.plusjitter=0 for fixed), and mac.plusmouse=0
     tells whether they are the source. The same log: Y1 2,300 changes in 5 s with the mouse nearly still,
     Y2 none: the tester's mouse's Y1 line chatters by itself.
   - *The bus explained, in part (docs/18, the same evening).* The research found that the PiStorm's current
     CPLD firmware takes read data about one 68000 clock early (since an Amiga speed-up of June 2021), which
     the SE's BBU hides by delaying /DTACK and the Plus-era PALs do not; that the Z8530 wants 2.2 us between
     accesses on these boards, which only the later glue chips enforce; and that the tester's board is very
     likely a reproduction 512Ke with modern PLDs. So the earlier reading of "Y1 chatters by itself" was
     probably wrong: the kernel read channel A's RR0 and then channel B's one bus cycle apart, and Y1 was
     the one read second. Two kernel changes follow its recommendations K1 and K3: every SCC access on the
     128K/512K/512Ke/Plus waits until 3 us after the last (psmac_gpio.c, the Mac's and the kernel's alike);
     and on those boards the screen check reads nothing back, it rewrites one row a tick from the Pi's copy
     ("rows rewritten blind"; mac.screencheck=on for the read-back, off for neither). Neither can be seen
     in the testbed (MAME's SCC and RAM have no such timing); the tester's next log will say.
   - *The VIAs (22 November, from the owner).* Both pre-SE machines in the field have original Rockwell R6522
     VIAs, NMOS with TTL input levels, which take the PiStorm's 3.3 V as high: docs/18's concern about a WDC
     W65C22S (inputs at 0.8 VDD) does not apply to them. That leaves the CPLD's early read sampling against
     the boards' PAL (or reproduction GAL) /DTACK timing, and the SCC's recovery time, as the leads.

97. **FAT32 and exFAT on the Mac: disk images on sticks, and an HFS view of a stick or a card folder (21
   November, issue #20).** The owner wants sticks formatted by Windows, Linux or a modern Mac, and a folder
   on the SD card, usable on the SE as ordinary disks with nothing installed on the Mac. No file system is
   native to both: macOS 10.15 and later have no HFS, System 6 and 7 read only HFS (FAT needs PC Exchange
   from Mac OS 8). So the Pi translates. Until now (item 75) a non-HFS stick was refused with a log line.
   - *One reader* (`psmac_fatfs.c`): FAT12/16/32 and exFAT through the caller's sector function, no
     allocation; the volume on a bare stick, in an MBR partition, or in a GPT's Microsoft basic data
     partition (a stick from Disk Utility has a 200 MB EFI system partition, FAT32 itself, in front: it is
     passed over); VFAT long names, exFAT entry sets with their checksum and UTF-16 names, FAT chains and
     exFAT's NoFatChain runs, ValidDataLength. Directory reads are resumable so callers can bound a step.
     Found while testing: mkfs.vfat makes "FAT32" volumes with fewer than 65,525 clusters (a 256 MB card
     with 4 KB clusters), which Microsoft's cluster-count rule calls FAT16; as Linux does, a boot sector with
     no 16-bit FAT size and no fixed root is taken as FAT32. The card's own small FAT writer (pairings,
     logs, cmdline.txt, its disk images) stays as it was; the new reader serves the exchange folder and sticks.
   - *Stage 1, disk images on FAT/exFAT sticks* (`psmac_usbdisk.c`). A stick is now a list of volumes; the
     published slots and core 1's job slot are numbered by volume (four, PiDisk's units). Every disk image in
     a FAT/exFAT stick's root (`.image .dsk .hda .img .hfv`) that holds HFS, bare or as an Apple partition
     map's Apple_HFS partition, is a writable volume read and written in place in the file's clusters (at most
     256 pieces): writing inside a file changes no FAT, directory or size. An image that is not HFS (a Pi OS
     `.img`) is skipped with a line. Ejecting one volume leaves the others; "safe to unplug" when none is left.
   - *Stage 2, the HFS view* (`psmac_hfsview.c`): a genuine HFS volume made in memory from a folder (the
     card's "Mac Exchange", `mac.exchange=`) or a whole stick (`mac.usbview=off` to skip): boot blocks, MDB,
     volume bitmap, an extents B-tree that is only its header (every fork is one extent), a catalog B-tree;
     512-byte nodes as Inside Macintosh: Files describes them. Only metadata is in memory: the allocation
     blocks are laid out fork after fork and a read of one becomes a read of the clusters holding those bytes.
     AppleDouble `._name` files (entry 9 Finder info, entry 2 resource fork, read in place at its offset in
     the `._` file) give files their type, creator, flags and resource fork; the `._` files and dot files
     (.DS_Store, .Trashes, .Spotlight-V100) are not shown, FAT-hidden files are invisible. Names: UTF-16 to Mac
     Roman with decomposed accents composed first (macOS writes NFD), `?` for what Mac Roman lacks, `:` as `-`,
     names over 31 characters keep a short extension behind an ellipsis, clashes in one folder (in the File
     Manager's order: case folded, accents kept) get " #2", " #3" in directory order, so the same medium
     always gives the same names. DOS dates (local time, as the Mac's clock) to Mac time. The allocation block
     is the smallest multiple of 512 that fits everything in 65,535 blocks; over 2 GB (System 7.1's limit) the
     largest files are left out and the log says how many. At most 32,000 entries. The volume is locked
     (drAtrb bit 15, and the unit not writable), so the Finder refuses writes. Leaf nodes of the catalog are
     put together again at each read, so its bulk never sits in memory. The making is split into steps of at
     most 48 sectors (core 3 for the card, every other card pass; core 1 for sticks, between the Mac's jobs).
     The view is at least 800 KB (hfsutils refuses smaller volumes; the rest is free blocks).
   - *What hfsutils showed while it was written:* a map node's record must end at $1FA, 492 bytes, not 494
     ("malformed b*-tree map node"), which a 10,000-file catalog needs (more than 2048 nodes).
   - *What the testbed showed:* the first stick's view "gave read errors" without any transfer failing: the
     reader's counters were never zeroed, and the kernel's heap is not zeros as the host's fresh memory is.
     The host tests now fill every allocation with $A5, and reproduce it with the old reader.
   - *Memory per volume* (the log's "KB of memory"): about 130 bytes per file or folder, 8 per fragment, 4 per
     catalog record, 512 per index node, 8 KB of scratch: 342 KB for the card folder of 1,522 files and 27
     folders; 2.6 MB for 10,000 files. While it is made, the UTF-16 names too (freed after). 8 MB of the
     kernel's heap are always left to the rest; a volume that would need more fails with a line.
   - *Verified on the host* (`make -C mac check`: `tools/fatfs_test.sh`, `tools/hfsview_test.sh`): a test tree
     (Mac files with hand-made AppleDouble files laid out as macOS writes them, long, accented, CJK and emoji
     names, clashes, 20 folders deep, an empty file, a fragmented file, hidden and system names) as FAT16 and
     FAT32 (mkfs.vfat, mcopy) and exFAT (the new `tools/mkexfat.py`, fsck.exfat clean; bare, MBR, GPT after an
     EFI partition, NFD names, 4/32/64 KB clusters): the reader finds all 325 files with their bytes; the view,
     read with hfsutils (private HOME), has exactly the expected names, every file's data fork, and both forks,
     type and creator of the Mac files (`hcopy -m`); 10,000 files in one exFAT folder (2,770 catalog nodes,
     depth 5, a map node); three 900 MB files (sparse) of which the largest is left out and the rest read back
     with 29,184-byte blocks; a folder as the volume. No step reads more than 48 sectors.
   - *Verified in the solo testbed* (QEMU, System 7.1, the card with "Mac Exchange" holding SimpleText (from
     the test image, split into data and `._` file) and Aurora68 Slots (mac/build, split the same way) beside
     the test tree with 1,500 files in one folder; an exFAT stick in a GPT after an EFI partition with an HFS
     floppy image, Aurora68 Slots with its `._` file, 10,000 files in one folder), through the Aurora68 Agent,
     that is, the Mac's own File Manager: "Mac Exchange" made in 382 ms (224 reads), mounted, locked
     ("403: the volume is locked" for a push); its names as on the host ("Quarterly report for th… #2.doc",
     "?? #2.txt", "Grüße aus Köln.txt", "Ω and π.txt"); a file 20 folders down; SimpleText pulled back as
     MacBinary with both forks and APPL/ttxt identical to the original; a fragmented 300,000-byte file
     identical; SimpleText and Aurora68 Slots launched from the card's folder and running
     (`docs/img/system71-exchange-slots.png`). The floppy image on the exFAT stick mounted writable, a file
     pushed into it, and on the host the image inside the stick holds it ("written on the Mac").
     The stick's own file view failed in that run (the counters above). With the fix, the next run: "[USB]
     disk 0: ready as the HFS volume "USB STICK": 10002 files ... 2582 KB of memory; made in 241 ms (238
     reads)", a PiDisk unit of its own beside the floppy image; through the Agent the stick's root (Aurora68
     Slots APPL/PISL, Grüße.txt, Many with 10000 entries, f10000.txt there), Grüße.txt's text, and Aurora68
     Slots pulled back with its resource fork and type identical to mac/build's. The Finder built a desktop
     database for the locked stick in memory ("Rebuilding the desktop file" for about a minute with 10,000
     files, then quiet). In the Finder: "Mac Exchange" opens with the lock in the window's corner and "zero
     K available", the icons placed by the Finder (`docs/img/system71-exchange-window.png`); its
     International folder shows the converted names (`docs/img/system71-exchange-names.png`). Put Away of
     USB STICK: "ejected by the Mac; 1 more of its volumes are on the Mac: not yet safe to unplug"; of Stick
     Floppy: "ejected by the Mac (2085 blocks read, 2 written): safe to unplug", both icons gone.
   - *Not verified:* real hardware (a stick formatted on a modern Mac, the SE); System 6.0.8 (the view is
     plain HFS, which System 6 reads, but no run was made); the Finder's handling of a view of many thousand
     files in its windows at the SE's speed. Stage 3 (writes back to FAT/exFAT) is not done; its plan is in
     issue #20.
98. **The video card: MPEG-1 decoded on core 1, drawn into the Mac's own screens (22 November, issue #23).**
   The owner asked for YouTube on the Mac, "elegant", through a card that exists only in the Pi and that
   Alice68 could share; then, the same evening, that it play on the SE's own screen as well as on HDMI ("yes,
   it is going to look crap, but good proof of concept"), and that nothing in it lean on the remote control,
   which is for debugging. docs/16 is the design: a helper on the owner's network converts any video to an
   MPEG program stream (MPEG-1 video, MP2 sound, issue #22, done); the Mac's program fetches it over the Mac's
   own TCP/IP and copies the bytes into the card (issue #24, not yet written); the card only decodes.
   - *The decoder:* pl_mpeg (phoboslab, MIT, one header, vendored unmodified in `third_party/pl_mpeg`, compiled
     once in `psmac_plmpeg.c` with `PLM_NO_STDIO` and the TLSF heap). It reads its bit stream a byte at a
     time, so the big-endian kernel needs nothing special; it keeps times as doubles and its MP2 synthesis in
     floats, which core 1 may use (no JIT there, as for minimp3, item 82). It needs the next picture's start
     code before it decodes a picture, so a stream that runs dry mid-picture costs nothing but a wait.
   - *The core* (`psmac_video.c`, `psmac_video.h`, nothing NuBus in either): a 4 KiB block and a 512 KiB byte
     ring, the Mac writing head and the card tail, as the sound card's stream channel does (and with its rule
     that a new head is used only once it has stood for 50 us). Commands start, pause, resume and stop; a seek
     is stop, the new stream's bytes, start. The geometry (screen, rectangle, visible) is read under a
     sequence number. One slice per call from core 1's loop beside the mixer (`psmac_usb.c`): a picture
     decoded, a block of sound, or a picture drawn. The clock is the sound when the stream has some and the Pi
     an output: the stream time of the first sample given to the mixer plus the frames the mixer has played
     from the card's queue (`psmac_audio.c`, source 5, its own resampler, a pause stops taking from it). With
     no sound, or no output (QEMU has no HDMI audio), the Pi's timer, held while paused or dry. A picture is
     drawn even when late unless it is more than two frames late and the last was drawn under 80 ms ago:
     without B-frames every picture is the next one's reference, so only drawing can be skipped.
   - *Drawing* goes into the screen's own frame buffer, at its depth, scaled to the rectangle (nearest pixel):
     1 bit by an 8 x 8 Bayer threshold on the luminance; 2, 4 and 8 bits through a 32,768-entry table of the
     nearest entry of the screen's colour table with an ordered dither added (made again when the Mac changes
     the table: 67 ms for 256 colours under user-mode QEMU); 16 and 32 bits direct. The screens' own paths
     then carry it: the SE's changed rows over the wire (item 68), the HDMI screen to the VideoCore at 30 Hz.
     So the Mac's screenshots show the video too, and a window over it is the Mac's business (its program
     says "not visible"); the Aurora68 Log window, repainting as the log scrolled, overwrote the picture in
     the first runs, which is what it should do.
   - *The card* (`psmac_videocard.c`): slot $C, not the $B of docs/16's first draft. The IIci's built-in video
     answers in slot $B's space (its frame buffer is at $FBB08000, docs/10), so the ROM's tables do not map
     that space one to one; $C, $D and $E are the IIci's real NuBus slots, empty in the SE and in MAME. 1 MiB
     of Pi RAM after the big frame buffer is the window, mapped at $FC000000 and at $FCC00000 (where 24-bit
     mode reaches the slot); 16 KiB after it is the declaration ROM at the slot's top: 188 bytes, board
     "Aurora68 Video" (BoardId 'AV') and one sResource {$7050, 3, 1, 'AV'} "Aurora68_Video" with no driver,
     MinorBaseOS 0, MinorLength 1 MiB, compiled into the kernel from `tools/mac/mkvideodeclrom.py` (`make -C
     mac video-declrom`), so the card is there whichever declaration ROM slot 9 gets. Aurora68 Slots, in the
     solo board's start-up items, listed `slot 9 "PiStorm-SE", slot $C "Aurora68 Video"; empty: $A $B $D $E`.
     `mac.video=off` leaves slot $C empty and carves nothing.
   - *Testing before the Mac's program:* `mac.videotest=PATH,LEFT,TOP,WIDTH,HEIGHT,SCREEN[,DELAY[,PAUSE_AT[,SEEK_AT]]]`
     feeds a host file through semihosting into the ring as the program will (32 KiB at a time while there is
     room, eof at the end), with a two-second pause and a seek back to the start on demand. The test movies
     are ffmpeg's `testsrc` and a 440 Hz tone converted with the helper's own `ffmpeg_args` (320 x 240, 512 x
     342, 640 x 480; 25 frames a second, MP2 128 kbit/s). Before any QEMU run the core was built for Linux
     with stand-ins for the kernel (a fake screen at each depth, a mixer that plays in real time) and run
     under qemu-aarch64: 250 of 250 pictures shown in 9.98 s; with the sound clock and a 3 s pause, 13.00 s.
   - *Testbed, solo board* (QEMU, the Mac's 640 x 480 screen at 1 bit): 320 x 240 at 160,120, 250 decoded, 250
     shown, none dropped, never dry; decoding 1.1-1.5 ms a picture (at most 4.6), drawing 0.9-1.1 ms, 5-10 % of
     core 1; the pause (two seconds at 4 s) and the seek (stop at 6 s, the file again, start) both went
     through (stream 2 played to its end). 640 x 480 over the whole screen: decoding 7.5 ms (at most 13.3),
     drawing 4.3 ms, 30 % of core 1, 150 of 150 shown. Picture: `solo-board-1bit.png` (scratch, not
     committed: a screenshot of the Mac, from Apple's software).
   - *Testbed, SE board* (MAME's SE through the bridge, System 7.1 from the card image, the Finder at 100 s):
     320 x 240 at 96,51 of the SE's own screen at 220 s: 250 of 250 shown, decoding 1.1 ms, drawing 0.8 ms, 8 %
     of core 1; MAME's own screenshots show the dithered pattern and its counter moving on the SE's screen.
     The audio dump (`mac.audiodump=`) holds the 440 Hz tone from 220 to 224 s, nothing during the pause, and
     again from 226 to 232 s: ten seconds of sound in step with the pictures. The SE's screen rows went at
     about 1,600 a second over the testbed's bridge (6 to 7 pictures of 240 rows a second, so MAME shows two
     pictures torn into one now and then); the GPIO's ten milliseconds a screen (item 68) is ample for 15. The
     whole SE screen (512 x 342): 150 of 150 shown, decoding 3.8 ms, drawing 2.9 ms; about 1,450 rows a second
     reached MAME. With the HDMI screen (psmac-se.declrom, slot $A at 640 x 480, the Mac in millions): 250 of
     250 shown on screen 1 in colour, decoding 1.4 ms, drawing 5.7-6.8 ms (the colour conversion is the cost),
     22 % of core 1; the VideoCore screenshot shows the colour bars and the moving counter inside the
     desktop's pattern. Core 3's service loop turned 213,000-229,000 times in five seconds before, during and
     after the play: the card's work stays on core 1.
   - *Not verified on hardware:* the Pi 3's own decode time (QEMU's TCG is no measure of an A53: these numbers
     say only that the work fits and how it divides); the HDMI sound of a video (the testbed's output is the
     dump); the SE's speaker, which plays the video's sound only with `mac.sestream=on`, as it plays PiPlay's
     stream (item 82's beep hang is not understood yet); colour depths 2, 4 and 8 on a real screen (tested on
     the Linux build only, with made-up colour tables). Not done: the Mac's program (issue #24), a clipping
     region finer than "visible", and a scaler better than nearest pixel (the helper sends the size asked for,
     so it is rarely used).

99. **Aurora68 Play's Library: the music server's artists, albums and covers, and a queue (22 November).** The
   owner put about 120 albums (M4A, "Artist - Album" folders, a cover.jpg in each) behind the music server
   on a NAS and asked for Aurora68 Play to become "more like a Spotify type app": artists, album art, play
   any track, while the radio stays as it was.
   - *The division of work.* The server (`tools/music-server`) does what a 68k should not: it reads tags and
     lengths (ffprobe, per album when it is first listed, kept), converts M4A to MP3, and makes each cover a
     bitmap of the size and depth asked for: Atkinson's dithering for one bit, the System's 'clut' 8 for
     eight (Floyd-Steinberg onto the 6x6x6 cube and the grey ramp). Its answers to the Mac are text a C loop
     splits (`/mac/...`: Mac Roman, Return-ended records, tab-separated fields, pages of 200), not JSON;
     `mac/piplay/libparse.c` reads them and `tools/libparse_test.c` checks it on the host (15 checks, one of
     which found the Mac side reading a cover's row length from the wrong field before it ever ran).
   - *The Mac.* A second HTTP client (`fetch.c`) for lists and covers, one request at a time, every step from
     the event loop, beside the radio's connection; the window, the views, the queue and Now Playing in
     `library.c`. A track is a radio stream with an end (`radio_play_track`): when its connection closes,
     radio.c asks for the next track's address and connects again without stopping the card, so an album
     plays without gaps; where the next track begins is the card's position plus what it holds at that
     moment, and the display moves on when `pos_ms` gets there. A connection lost in a track is made again
     from the second its audio stopped (`/t/ID?start=S`). 512 KB partition (was 256), colour windows where
     Color QuickDraw is so that an 8-bit cover keeps its colours.
   - *Finding the server.* A UDP probe to port 8070. In the testbed Riptide's MacTCP interface refused both
     the limited broadcast and the network's broadcast (error -23033: its UDP streams are not marked for
     broadcast, unlike Apple's MacTCP, which sends them), so the probe also goes to the multicast group
     239.255.68.70, which the server joins; that one went through. (A Riptide change, SO_BROADCAST on
     MacTCP UDP streams, would let the broadcast through too; not made here.) The answer carries the
     server's IPv4 and IPv6 addresses; Play takes the family **Connect By** names and keeps the other: with
     the server reporting an unreachable `fd68::99`, the open failed (-23033) and Play went on to the IPv4
     address by itself. For IPv4 it uses the address the answer came from (the server's own idea of its
     address can be a container's).
   - *IPv6 first or IPv4 first* (the owner's requirement): one preference for the radio and the library,
     in the Library and Radio menus (Connect By), kept in the preferences file (now version 2, which also
     keeps the server, Shuffle and the Library window; a version 1 file is still read). Radio passes it to
     .IPP6's lookup as the order (Riptide alternates the families in a long answer, so the other family comes
     next on a failure); the library's client does the same and tries a name's next address when one does
     not connect.
   - *The owner's IPv6 Live report* (Open URL with `http://[a global IPv6 address]:8423/live` showed as the
     name only the address's first group with its bracket,
     "Stopped", no sound). In the testbed the same URL shape (`http://[fd68::1]:8423/live`) connected to
     the right address and port over IPv6 and the name showed whole, also with the owner's exact address;
     the bracketed name has been handled since 21 November (stations.c and Open URL alike), and the released
     0.12 Play has that code, so the cut name was not reproduced. The connection in the testbed failed only
     because pinet relayed PINET_FORWARD's ports over IPv4 alone (now over IPv6 too), after which the
     library's tracks streamed over IPv6. A failed connection now says, in its alert, the address and port
     tried and the stack's number in words ("could not connect to [2001:db8::20]:8423 (Riptide error -23065:
     this Macintosh has no address of that family to send from)"), which is what the owner's case needs to
     tell a Mac without a global IPv6 address from a server that refused.
   - *Testbed* (solo board, 512x342, Riptide, the server on the host through pinet's new `PINET_UDP` relay
     and `PINET_FORWARD` over IPv6): found by the probe; artists, an artist's albums with covers, an album's
     tracks under its cover; an album played track to track with the display moving on at each boundary,
     the queue ending ("the last track has played"); Find; the Connect By submenu; quit and start again with
     the server, the preference and the window remembered; Live over IPv4. Screenshots in the scratchpad of
     that session. The music server's test checks the `/mac/` interface (75 checks in all).
   - *Not verified:* the real SE and its WiFi; a colour screen (the 8-bit covers' palette indices are
     checked on the server, not seen drawn); Apple's MacTCP (the testbed image has Riptide); System 6; a
     library of the owner's size on the Mac (its pages are 200 rows, the owner's 79 artists one page).

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

100. **"This is not a Macintosh disk" on the BlueSCSI: Iomega's driver, twice over (22 November).** The
   owner's SE could not use its BlueSCSI disks: the Mac offered to initialize them ("This is not a Macintosh
   disk: Do you want to initialize it?"), with Iomega's Zip disk as the alert's icon.
   - *The icon was the clue:* a driver shows its own icon only for drives it has taken. Two Iomega drivers
     came down from the first test image, which was made from a Zip disk. One is in the partition map
     (Iomega_DiskInfo, Iomega's Zip 4.2 driver patched by `tools/iomega_patch.py` to take any disk,
     Iomega_backup): it drives the Pi's own disk path (`psmac_board_solo.c` answers Iomega's vendor command
     $06 as a Zip drive does) but cannot drive a BlueSCSI. The other is the **Iomega Driver extension**
     (INIT, 1995) in the image's `System Folder:Extensions`: it loads at every startup, scans the SCSI bus
     and takes drives over, so any System with it in place claimed the BlueSCSI's disks whatever their own
     driver was.
   - *The owner's BlueSCSI card was clean:* its three disks all carry Apple_Driver43
     (`tools/mac/swap_driver.py --show`). The extension came from the card image the SE started from.
   - *In the testbed* (`macse_ps`, the SE's 5380 as MAME emulates it, the current kernel), the System 7.1
     volume behind Apple_Driver43 on SCSI ID 0 started to the Finder: the kernel's SCSI path is not at
     fault, and MAME's disk was the startup disk already driven by Apple's driver.
   - *Fixed:* `tools/mac/swap_driver.py` moves an image's volume behind a donor disk's Apple_Driver43 (the
     owner's own disk: Apple's driver is not in the repository); the Iomega Driver extension is moved to
     `Extensions (Disabled)` in the base images, so images made from now on have neither. On an existing
     card image: start with Shift held (Extensions Off), then drag `Iomega Driver` out of the Extensions
     folder. The kernel keeps its answer to command $06 for older images on the Pi's own disk path.

101. **Photoshop never finished starting: the IIci ROM's SANE rounds extended to double (22 November).** On the
   owner's SE, Photoshop showed its splash screen and went no further. The kernel's heartbeat ran at a steady
   72 MIPS with nothing new to translate, the PC moving between Photoshop's code and $40870ACC, and the loop
   counters in D4, D5 and D7 stood still for minutes.
   - *What the loop was:* $40870ACC is in the IIci ROM's PACK 4 (FP68K, the SANE arithmetic package). The
     heartbeat's "code at" bytes decoded to a refinement step, then `_FP68K` FOSUB in extended precision (the
     new value minus the old), then a comparison of the difference with a tolerance. The loop waits for an
     extended-precision result to settle.
   - *Why it never settles:* the IIci's SANE is the version for a 68881/68882. It does its arithmetic with FPU
     instructions on extended operands. Emu68's FPU keeps the 96-bit extended format as a 64-bit double
     (`Load96bit` in `M68k_LINEF.c`): 53 bits of mantissa where SANE promises 64. A loop whose test was written
     for 64 bits can wait for ever on 53. The testbed shows the loss in one line: (1 + 2^-60) - 1 is 0 with the
     IIci's SANE, and 2^-60 on any real Macintosh.
   - *The fix: the SE ROM's SANE.* The SE's own ROM carries the other SANE: PACK 4 (FP68K, 4,544 bytes) and
     PACK 5 (Elems68K, 4,198 bytes) for a 68000 with no FPU. They do extended arithmetic exactly with integer
     instructions. They are Apple's, so nothing of them is in the repository. The kernel reads them from the
     SE's ROM chips over the wire at $400000 on the SE board, or from an SE ROM image named by `mac.serom=PATH`
     in the testbeds and on a Pi alone (`emu68/src/psmac/psmac_sane.c`). It finds them through the ROM's
     resource map, the way `tools/romres.py` does, and checks each package's 'PACK' header and number. The
     mailbox's INFO service hands them out (`PSMB_INFO_SANE`, 13, in `psmb.h`).
   - *On the Mac:* A68 Machine (`mac/pimach.s`) asks for both packages at startup. It copies each into a locked
     system-heap handle and stores the handle in AppPacks ($0AC8 for PACK 4, $0ACC for PACK 5), where the ROM's
     package dispatcher ($40826206 for `_Pack4`) looks. That alone was not enough: in the testbed the `_FP68K`
     and `_Elems68K` traps pointed straight into the ROM's packages, not through the dispatcher. So A68 Machine
     also moves any trap that points into the old package (within 16 KB of it) to the new one, and leaves a
     trap alone if something else has patched it. `mac.sane=rom` leaves the IIci ROM's SANE in place.
   - *Tested in the solo testbed* (System 7.1, 64 MB, `mac.serom=` the SE ROM image). A68 Machine measures
     SANE before and after and logs it:
     ```
     [MAC] A68 Machine: SANE, the IIci ROM's: (1 + 2^-60) - 1 = 0 (64-bit), square root of 2 settled in 6 steps, 349905 additions a second
     [PSMAC] SANE: from .../MacSE_B2E362A8.rom (checksum b2e362a8): PACK 4 (FP68K) 4544 bytes, PACK 5 (Elems68K) 4198 bytes, for A68 Machine to install
     [MAC] A68 Machine: SANE PACK 4 from the SE ROM installed: 4544 bytes at 0007D7E0 (the ROM's was at 40870AC0), trap at 0007D7E0
     [MAC] A68 Machine: SANE PACK 5 from the SE ROM installed: 4198 bytes at 00080968 (the ROM's was at 40871BE0), trap at 00080968
     [MAC] A68 Machine: SANE, the SE ROM's: (1 + 2^-60) - 1 = 2^-60 (exact), square root of 2 settled in 6 steps, 11709 additions a second
     [BOOT] t=47.505s Mac: the Finder is running (CurApName)
     ```
     The Newton iteration for the square root of 2 (x = (x + 2/x) / 2 until FOCMP finds no change) settled in
     6 steps both times. That is why the trouble is rare: most loops settle at 53 bits too, and only one whose
     test needs the last bits waits for ever. With `mac.sane=rom` the kernel logs "SANE: the IIci ROM's
     (mac.sane=rom)", A68 Machine "the IIci ROM's kept (Pi status -7)", and the Finder came up at 54.9 s.
   - *FP-heavy programs still work.* Through the remote control (`mac.remotefile=`), with the SE ROM's SANE
     installed: Excel 2.2 computed =SQRT(2), =SQRT(2)*SQRT(2)-2 (4.4409E-16, the double answer Excel keeps),
     =(1+2^-50)-1 (8.8818E-16), =EXP(1), =SIN(1), =100/7 and =LN(10) correctly, and Speedometer 3.23's Benchmark
     Mix ran to "The tests are done!" with no crash in the log.
   - *The price is speed.* Software SANE does the arithmetic in integer instructions, and in the testbed it
     is 20 to 60 times slower per addition than the FPU version: 11,709 and 26,685 additions a second against
     349,905 to 742,350 in four runs (QEMU's timing varies from run to run). Speedometer 3.23's Benchmark Mix,
     whose floating-point tests call SANE, shows the same in programs (ratings against a Mac Classic):

     | Test | IIci ROM's SANE (`mac.sane=rom`) | SE ROM's SANE |
     |---|---|---|
     | KWhetstones | 1,666.7 a second (228.3) | 61.9 a second (8.5) |
     | Fast Fourier | 0.950 s (206.0) | 25.650 s (7.6) |
     | F.P. Matrix | 0.650 s (166.3) | 14.333 s (7.5) |
     | Dhrystones, Towers, Sieve ... | integer, not affected | integer, not affected |

     Exact SANE leaves a SANE-bound program at about eight times a Classic; the FPU version runs it about 200
     times a Classic but rounds every extended result to a double. The trade is the owner's to choose:
     `mac.sane=rom` brings the fast version back. Speedometer's own FPU tests use FPU instructions directly and
     are not affected either way.
   - *The limit:* this fixes programs that call SANE (`_FP68K`, `_Elems68K`). Programs compiled for the FPU
     (code built for a 68881 with THINK C or MPW, or a program's "FPU version") execute FPU instructions
     directly. They still get 64-bit doubles from Emu68's FPU, because extended precision would need a change in
     the JIT's FPU itself.
   - *Not checked yet, needs the owner's SE:* the wire read of the real SE ROM (about 9 KB in some 5,000 bus
     reads at the Mac's first request), and Photoshop itself, which is not on the test image.
   - *On the owner's SE (22 November 2026, evening):* the kernel read both packages from the SE's own ROM
     chips over the wire ("from the SE's own ROM (checksum b2e362a8): PACK 4 (FP68K) 4544 bytes, PACK 5
     (Elems68K) 4198 bytes"), A68 Machine installed them, and its test changed from "(1 + 2^-60) - 1 = 0
     (64-bit), 895293 additions a second" to "= 2^-60 (exact), 159717 additions a second" (5.6 times
     slower on the hardware, not the testbed's 25). **Photoshop, which had sat under the stopwatch for
     minutes, opened.**

102. **Aurora68 Play on a modern Mac: other players, stations, new albums, seeking, an icon (22 November).** The
     owner's list, after a day with the macOS Aurora68 Play: "Photorealistic Icon for MacOS (modern); Needs to
     detect when new albums are added without restart; Pressing Live shows nothing... should there not be a list
     of stations; Perhaps other devices running Aurora Player need to broadcast they are online for redirection;
     I can't jump to a position in the song on the MacOS version (works on classic Mac)". (Their last item, FLAC
     and the like on the classic Play, is the classic Play's own work, against the same server interface.)
   - *Players as devices (the music server).* Every Aurora68 Play now announces itself every 20 seconds
     (`POST /api/devices/announce`; the classic Play `GET /mac/announce`), posts what it plays, and fetches
     commands by a long poll: the server holds `GET /api/devices/ID/commands?wait=20` until a command comes or
     20 seconds pass, so no player needs an open port (a Mac with its firewall on, MacTCP on the SE). A player
     not heard from for 60 seconds is offline. `GET /api/devices` lists them, the Live channel first as the
     device `live`; `POST /api/devices/ID/command` sends one "play this": the server turns `s:`, `f:`, `t:` or
     `r:N` into the tracks themselves (path for `/file/`, id for `/t/`, title, artist, album, length) or a radio
     station's address, so a player plays what it gets without asking again. The classic Play has the same in
     text (`/mac/state`, `/mac/devices`, `/mac/commands`). The interface is `tools/music-server/README.md`,
     "Devices"; both Plays were written against one shared design, so neither waits on the other.
   - *Stations.* `GET /api/radio` (and `/mac/radio`) hands every player the classic Play's built-in radio list
     (`mac/piplay/stations.c`), checked by the test against the C source so the two cannot drift apart, plus a
     stations file of the owner's own in the music folder. The macOS Play's Live page (now "Live and Radio")
     starts with stations: everything shuffled (on This Mac the server's own `/radio/?shuffle=1` stream), each
     artist shuffled, and the radio stations grouped as the classic Play's Radio menu, each playable on This
     Mac, on Live (the library only: Live plays no radio) or on another player.
   - *New albums.* The library scan already ran again when a request came 30 seconds after the last; nothing
     told the app. The scan now keeps a signature of the folders and files, and a scan that finds a change adds
     one to the library's generation (`/api/info`, the announce answer, `/mac/info`'s sixth field). The app
     watches it and fetches the catalogue again when it changes; the navigation keeps artists and albums by
     value and the views look them up again by id, so an open artist shows its new album where it is. A scan
     that finds nothing new no longer logs the library summary every 30 seconds.
   - *Seeking.* The position bar was a SwiftUI `Slider` that seeked in `onEditingChanged(false)`. On macOS a
     click on a slider's bar moves its value without that call coming, so the click left the knob "held" at the
     place clicked (the app showed the new time) and never seeked; only a drag that ended on the knob did. The bar
     is now a scrubber of the app's own (a `DragGesture` with no minimum distance: a click is a drag of length
     zero, and both end in a seek). The seek itself is exact (zero tolerance: AVFoundation otherwise picks a
     convenient point, seconds away in a long MP3), waits for an item that is not ready yet instead of being
     lost, and holds the clock until it finishes. The server now names `/file/`'s Content-Type itself for `.m4a`,
     `.flac` and the rest: a NAS's slim Linux image may have no `/etc/mime.types`, and AVFoundation decides by
     it whether it can play a file. The Range answers were already what AVFoundation asks for (206 with
     Content-Range for `bytes=0-1`, HEAD, Accept-Ranges).
   - *The icon.* `macos/Aurora68Play/make-icon.py` paints it with numpy and Pillow, nothing taken from anyone
     else's artwork: Apple's icon grid (an 824-pixel superellipse on 1024, a soft shadow), the aurora (curtains
     bright at their wavy lower edge, green turning violet above, with rays), stars, dark hills, and a black
     vinyl record in front (3.2-pixel grooves, four gaps between songs, the two glossy wedges tinted by the
     aurora, a specular, a rim, a soft shadow, an aurora-green label). Every size is made from the 1024-pixel
     picture; the record and its label still read at 16 pixels. The macOS workflow installs numpy and Pillow in a
     virtual environment and keeps a preview (artifact Aurora68-Play-icon).
   - *Tested:* the server test (now 127 checks, all passing) adds the devices on a server of its own with a
     one-second rescan and a three-second time to go offline (`MUSIC_RESCAN`, `MUSIC_DEVICE_TTL`): announce,
     state and the list in JSON and in text, commands fetched at once and during a wait, radio by index, `live`,
     refusals, offline and back; the radio list against stations.c; the generation with a new album. The macOS
     app's core tests add the devices' JSON, commands as player actions, the radio groups, the kept id, the
     generation watch, and the scrubber's arithmetic; GitHub's macOS runner builds and tests it.
   - *Not verified here (no Mac with a screen, no SE on this network):* the app on a real Mac (a click on the
     position bar; Play on another Mac and on the SE; a radio station on This Mac; a new album appearing; the
     icon in the Dock and Finder), and the classic Play's side of the devices.

103. **`mac.fpu=none`: a Mac with no FPU, as the SE is (22 November, issue #28).** Emu68 executes FPU
   instructions, and keeps the 68881's 96-bit extended format as a 64-bit double. Item 101 replaced the IIci
   ROM's FPU SANE with the SE ROM's software SANE, but a program built for the FPU (a 68881 build from THINK C
   or MPW, an application's "FPU version") still finds an FPU and runs its own FPU code at double precision.
   A real SE has no FPU: such programs take their software paths there. `mac.fpu=none` gives the Mac the same.
   - *The JIT already had it.* Upstream Emu68 has a `disable-fpu` property (`globals.fpu_disable`, read from
     the device tree in `aarch64/start.c`). With it, `EMIT_lineF` (`M68k_LINEF.c`) sends every coprocessor-1
     instruction (`(opcode & 0x0e00) == 0x0200`, FSAVE and FRESTORE included) to the line-F exception, vector
     11, as a 68000 or a 68LC040 would. The other F-line forms are not affected: the MMU shim's PMOVE/PFLUSH
     forms are coprocessor 0, MOVE16 and the cache instructions coprocessor 3 and 2. The new option only sets
     that flag (`psmac_read_options` in `psmac_bus.c`; `mac.fpu=on` sets it back).
   - *The first run did not reach the Finder.* With the flag set, the kernel's heartbeat showed HWCfgFlags
     ($0B22) still at $FC00, its bit 12 (hwCbFPU) set. System 7.1 then executed an FPU instruction at $0001C128
     during startup, and the Mac did not reach the Finder in 190 seconds. The IIci ROM's power-on FPU test
     ($408456FC, `fmove.l d0,fp0` at $40845704) trapped as expected, but that test only decides pass or fail.
   - *Where HWCfgFlags comes from.* The IIci ROM does not probe for the FPU to set HWCfgFlags. At $40800612 it
     loads UnivInfoPtr ($0DD8) and copies two words from the ProductInfo record it points to:
     `move.w 16(a0),$B22`. `mac.peek=dd8,4` showed UnivInfoPtr at $408036C0, so the flags come from ROM offset
     $36D0, where the word is $DC00 (bit 13 is set later during startup, hence $FC00 at run time). Gestalt's `'fpu '`
     function ($40816600) tests bit 12 first (`btst #4,$B22`) and answers 0 when it is clear; only when it is
     set does it run FSAVE and read the frame's first word ($1F18/$3F18 a 68881, $1F38/$3F38 a 68882).
   - *The fix: clear hwCbFPU in the ROM's record.* `psmac_fpu_rom_fixup` (`psmac_board_se.c`) runs at ROM load
     beside the SE's chime and SCSI fixups. With `mac.fpu=none` it checks the record's first 18 bytes against
     the IIci's, changes $DC00 to $CC00, and subtracts $1000 from the checksum word at offset 0 so that the
     ROM's start-up checksum test still passes. If the bytes do not match, it changes nothing and logs a
     warning. Only this one record is changed: the ROM's other ProductInfo records are for other models.
   - *Expected traps are not crashes.* The crash recorder treated vector 11 as a crash. With `mac.fpu=none`,
     `M68k_ExceptionEntry.c` now counts a coprocessor-1 opcode at the faulting PC instead of recording it, and
     the heartbeat reports the count and the last PC ("FPU (none): N FPU instructions took the line-F
     exception so far"). Other line-F opcodes are still recorded.
   - *SANE.* The IIci ROM's SANE is FPU code. With no FPU, `psmac_sane.c` ignores `mac.sane=rom` and says so,
     and warns if no SE ROM is at hand: then the first SANE call ends in system error 90 ("no FPU"). The second
     run showed one more problem: A68 Machine measures the IIci ROM's SANE before it installs the SE ROM's
     (item 101), and that first FOADD at $40870AD4 (`fmove.l fpsr,d5` in PACK 4) ended in system error 90.
     `mac/pimach.s` now skips that measurement when hwCbFPU is clear. It also logs Gestalt's answer:
     "A68 Machine: Gestalt fpu N".
   - *Tested.* Solo testbed, System 7.1, 64 MB, `mac.serom=` the SE ROM image, `mac.fpu=none`, two runs:
     ```
     [PSMAC] FPU: none (mac.fpu=none): FPU instructions take the line-F exception, as on a 68000 Mac; Gestalt reports no FPU
     [PSMAC] FPU: the ROM's ProductInfo HWCfgFlags $DC00 -> $CC00 (hwCbFPU clear: no FPU for Gestalt; checksum word now 36ac8157)
     [PSMAC]   the Mac's HWCfgFlags $ec00: no FPU found at start-up (mac.fpu=none)
     [PSMAC]   FPU (none): 1 FPU instructions took the line-F exception so far, the last at PC 40845704
     [MAC] A68 Machine: Gestalt fpu 0
     [MAC] A68 Machine: SANE, the SE ROM's: (1 + 2^-60) - 1 = 2^-60 (exact), square root of 2 settled in 6 steps, 25554 additions a second
     [BOOT] t=48.821s Mac: the Finder is running (CurApName)
     ```
     The only FPU instruction executed during startup was the ROM's own power-on test. There were no crash
     records. On the MAME SE board (`macse_ps`, 16 MB, the same disk as a CHD, no `mac.serom=`), the kernel read
     PACK 4 and 5 from the SE's own ROM over the wire. SANE gave 2^-60, again with one trap, and the Finder was
     running at 529 s (the SE board runs at the real machine's bus speed). With the FPU left on (no
     `mac.fpu=`), nothing changed: "FPU: present", HWCfgFlags $FC00, "Gestalt fpu 2", the IIci ROM's SANE 0
     (64-bit) and then the SE ROM's 2^-60, the Finder at 56 to 59 s, no crash records.
   - *Not yet tried:* Speedometer's FPU line and an FPU-only program (which should say it needs an FPU) with
     `mac.fpu=none`, and the owner's SE. The default stays `on` until issue #27's exact FPU decides the SE
     board's default.

104. **The exact FPU: `mac.fpu=exact`, a 68882 in extended precision, now the default (22 November, issue
   #27).** Item 101 found the IIci ROM's SANE rounding extended results to double, because Emu68 keeps the
   68881's 80-bit registers as 64-bit doubles; item 103 offered no FPU at all. This item gives the Mac an FPU
   that computes as the real one does. The design is docs/17: Berkeley SoftFloat 3e (vendored, BSD-3, with
   a 68881 specialization) does the arithmetic in C, the 68881's rules (FPCR precisions and rounding, FPSR
   flags, exceptions, FSAVE frames, packed decimal) sit on top in `psmac/psmac_fpu.c`, and the JIT
   (`EMIT_FPU_exact` in `M68k_LINEF.c`) translates every coprocessor-1 instruction into a call to it.
   - *Who moves the PC.* The first version advanced the 68k's PC in the JIT after the call
     (`EMIT_AdvancePC`), as Emu68's own FPU code does. But the call's exception path leaves the block through a
     local exit, and the JIT's pending PC offset is flushed into the exit it meets first: there it was the
     exception path's, and the normal path lost it. Now the C side sets x18 itself, to the next instruction
     (its length is `psmac_fpu_length`'s, the same function the translator uses), a branch's target, or the
     PC an exception stacks, and the JIT emits no advance after the call. `tools/fpu_test.c` checks the PC
     the C side leaves after every instruction that goes on in the block.
   - *Vectors.* The C side returned vector numbers; `M68K_Exception` takes the vector's offset (vector times 4,
     `VECTOR_LINE_F` is $02C). It returns offsets now, (format << 12) | offset.
   - *SANE.* On the exact FPU the IIci ROM's SANE gives the exact answer, so `psmac_sane.c` keeps it by
     default and logs "SANE: the IIci ROM's, on the exact FPU"; `mac.sane=se` still installs the SE ROM's
     packages. With `mac.fpu=double` the SE ROM's SANE is installed as before; with `mac.fpu=none` it always is.
   - *Merging item 103.* `mac.fpu=` now takes `exact` (the default; `on` means the same), `double` and `none`.
     The banner "FPU: present (... 64-bit doubles)" from item 103 was no longer true and is gone:
     `psmac_bus.c` announces `none`, `psmac_fpu_init` announces `exact` or `double`. The crash recorder no
     longer records the FPU's exceptions (vectors 48 to 54): they are taken only when a program has enabled
     them in the FPCR with a handler of its own (the test INIT's OPERR check was the first crash record).
   - *The test INIT on a Mac without an FPU.* "Aurora68 FPU Test" loads before A68 Machine, and with
     `mac.fpu=none` its first FPU instruction ended in system error 90 at startup. It now tests HWCfgFlags'
     hwCbFPU (`btst #4,$B22`, as Gestalt does) and logs "no FPU" instead.
   - *Tested.* Host: `tools/fpu_test.sh`, 20,273 checks, 0 failed (docs/17 "Testing" has the parts). Solo
     testbed, System 7.1, 64 MB, `mac.serom=` the SE ROM image, the merged kernel:
     ```
     (no mac.fpu)  [PSMAC] FPU: exact, a 68882 in extended precision (mac.fpu=exact)
                   [MAC] FPU test: 24 passed, 0 failed
                   [MAC] A68 Machine: SANE, the IIci ROM's: (1 + 2^-60) - 1 = 2^-60 (exact), ... 247902 additions a sec
                   [PSMAC] SANE: the IIci ROM's, on the exact FPU (mac.sane=se for the SE ROM's)
                   [BOOT] t=42.621s Mac: the Finder is running (CurApName)          no crash records
     mac.fpu=none  [MAC] FPU test: no FPU (HWCfgFlags hwCbFPU clear), nothing tested
                   [MAC] A68 Machine: SANE, the SE ROM's: (1 + 2^-60) - 1 = 2^-60 (exact), ... 22323 additions a second
                   [BOOT] t=44.048s Mac: the Finder is running (CurApName)          no crash records
     mac.fpu=double (the speed-only INIT) the IIci ROM's SANE 0 (64-bit), then the SE ROM's 2^-60,
                   the Finder at 43 s, no crash records
     ```
     `mac.fpu=exact mac.sane=se` installed the SE ROM's packages (2^-60) and reached the Finder. Speedometer
     3.23 (before the merge, the same FPU code) reported an MC68882 and ran its FPU Benchmarks to the end:
     3529 KWhetstones, matrix multiply 0.650 s, FFT 0.283 s, average 8.48 against a Mac II. Speeds against
     `double` are in docs/17: in QEMU the exact FPU is about 20 to 70 times slower per instruction, and the
     IIci ROM's SANE on it still several times faster than the SE ROM's software SANE.
   - *Not yet tried:* the MAME SE board and the owner's SE (a real Pi's speed for the exact FPU); Photoshop
     and other FPU builds of programs on the exact FPU; the full test INIT with `mac.fpu=double`, which ends in
     an F-line crash at $81000040 (Emu68's own FPU, not changed here: the speed-only INIT exists for that).
105. **The classic Aurora68 Play 1.3: FLAC, Ogg Vorbis and Apple Lossless, seeking, new albums, a device (22 November).**
     The owner's last item of item 102's list: "Legacy version needs to support FLAC etc not just MP3", with the
     classic Play's share of the rest (new albums without a restart, players that know of each other).
   - *Decoding on the card.* The stream channel's block is version 2 (`psmb.h`): formats 3 to 5 (FLAC, Ogg Vorbis,
     ALAC) and `start_ms`, the file's time at the first byte the Mac sends. Core 1 decodes them beside minimp3, behind
     one interface free of kernel calls (`psmac_codec.h`): dr_flac 0.13.4 and stb_vorbis 1.25, both vendored
     unmodified (public domain or MIT-0/MIT; licences and READMEs under `third_party/`), and Apple Lossless by a
     decoder written here (`psmac_alac.c`). The plan was Apple's own decoder; it is Apache-2.0, which the FSF holds
     incompatible with GPL version 2, and the kernel also holds the GPL-2.0-only Alice68 modules, so it could not be
     passed on in one binary. dr_flac pulls its bytes through a callback that cannot wait, so a frame is decoded only
     when the Pi's buffer holds more than a frame can take; stb_vorbis is fed through its pushdata interface from a
     window that grows until the three headers fit (a cover picture in the comments can be large) and allocates from
     one buffer, so no malloc and no alloca on core 1's stack; its setup's sin, cos, exp, log, pow and floor are the
     kernel's musl ones. AAC is refused: the free decoders are GPL/LGPL and large, and the music server converts.
   - *The Mac's half* (`mac/piplay/media.c`, portable C like libparse.c): a file is known by its contents, so files
     arriving from modern machines as `????` play; it sends FLAC's STREAMINFO alone and then the frames, Ogg's first
     pages up to the first audio page, and for .m4a the decoder's 24-byte configuration and each packet with its length,
     taken from the MP4 sample tables (often at the file's end) with the File Manager. Tags from Vorbis comments and the
     MP4 `ilst`, UTF-8 into Mac Roman. Seeking by a click on the progress line: FLAC's SEEKTABLE or a guess in proportion,
     the next Ogg page, the ALAC packet, MP3 and PCM in proportion; the card finds the real place itself (a FLAC frame's
     number, an Ogg page's granule position) and counts on from there.
   - *Library and devices* (`mac/piplay/devices.c`, portable C). Announce every 20 seconds (`/mac/announce`, an id kept in
     the preferences, now version 3), state on a change, commands by a 15-second long poll (MacTCP's receive gives up at
     20), all on the Library's one connection: the long poll gives way to the user's lists before any byte of its answer
     has come. Commands are carried out by library.c and the player (play, add, next, pause, resume, skip, previous,
     seek, volume, stop, radio); `X-Aurora: 1` on every request. The generation from the announce answer and `/mac/info`
     refreshes the list in view and keeps its selected row. Not done: a "Play On" submenu (the server names music by
     folder paths, which the classic Play's track ids do not give).
   - *Tested on the host:* `make -C mac check` adds tools/codec_test.sh (ffmpeg-made tones and noise: FLAC 16 and 24 bit,
     mono and stereo, a fast-encoded one, ALAC 16 and 24 bit and mono, Ogg Vorbis stereo and mono, decoded by media.c
     and the card's decoders in random-sized pieces): FLAC and ALAC identical to ffmpeg's decoding sample for sample,
     Vorbis within one LSB, the same after a seek to a third with the card's position right; tags (an e acute in UTF-8);
     AAC and Opus refused. tools/devices_test.c: 33 checks of the URLs, the answers and the state machine, and, given
     a server, the same code against it (announce, state, three commands with tracks, volume and a station, all
     received from the server of branch play-devices). Under ASan and UBSan: clean but for a signed shift in
     stb_vorbis's own CRC code, left as it is.
   - *Tested in the testbed* (solo board, System 7.1 with Riptide, the music server on the host through pinet): FLAC,
     Ogg (of type `????`, through File > Open), 24-bit ALAC and MP3 played to the end with no underrun, 6 to 37 per
     mille of core 1 under QEMU; a seek to 0:20 of a 40 s FLAC, the card starting at 20.062 s; the server listing the
     Play as a classic device and following its state; play `f:` (three tracks without a gap), pause, resume, skip,
     seek 12 s, volume 50 and stop from the server; a new album copied in, listed at the next announce with the
     selection kept (docs/09, "FLAC, Ogg Vorbis and Apple Lossless...", and `docs/img/system71-play13-*.png`). Two
     things on the way: pinet's ring files left from an aborted run made the second run's network silent (pinet
     started reading at the old offset); remove `BASE.rx`/`BASE.tx` before a new run. And the remote control's
     semihosting files must exist before the boot (the kernel opens them once).
   - *Not verified:* the owner's SE (core 1's load on a real Pi 3, a BlueSCSI disk's speed for lossless files at
     some 100 KB/s), files from the owner's own library (24-bit FLAC at 96 kHz, iTunes ALAC with the sample tables at
     the end, long Vorbis comments), and commands sent from the macOS Play or the panel to a real SE.

106. **Aurora68 Recovery: the Pi's SD card over the network when the Mac will not start (22 November).** In the SE,
     the Plus and the 512K the Pi sits in the 68000 socket under the logic board, and its microSD card cannot be
     reached without opening the case. A kernel8.img that does not start the Mac, a typing mistake in cmdline.txt or
     a disk image the Mac cannot boot from meant exactly that. The owner approved a design in which the kernel is its
     own way back: a start in which the 68k is never released, no Mac code runs, and the Pi serves its card over the
     network, says on the Mac's own screen and on HDMI where to find it, and takes a keyboard for what it needs to get
     onto a network. The same evening the owner added the WiFi settings and the common options as forms, a way in
     when the WiFi settings are wrong or absent, and a keyboard menu on the recovery screen.
   - *Getting in* (`emu68/src/psmac/psmac_recovery.c`, core 0 before the 68k would start; README "Recovery"):
     the boot counter (`boot.txt` in the card's root: starts in a row that did not reach the Finder, incremented
     before the 68k starts, back to 00 when CurApName says Finder; at `mac.bootcount=` (3) the next start is a
     recovery start); the programmer's interrupt switch held as the 68k would be released (the raw level's bit 2 on
     the wire, held 50 ms: SE, Plus, 512K); Esc held for a second on a USB keyboard in the kernel's first twenty
     seconds (the keyboard is not up before the 68k starts, so the kernel writes `recovery.txt` and restarts the Pi);
     `recovery.txt` in the root (taken away on entry; its line is shown as the reason), which the remote control's
     `POST /restart?how=recovery` (`aurora68.py restart recovery`) and the recovery page's own "restart into
     recovery" write; and `mac.recovery=1` in cmdline.txt, which "restart normally" takes out again. The counter is
     made once with one cluster and a text of fixed length, so later starts write one data sector and never a
     directory sector or the FAT: a power cut during that write can spoil the number (read as 0) and nothing else. A
     start the Pi was switched off in before the Finder counts as a failed one: there is no telling it from a start
     that hung and was switched off, and three in a row cost one press of "restart normally". A restart from the
     Finder never counts, since the counter counts the Pi's starts and the Finder was reached before it.
     `mac.bootcount=off` for a Mac that starts something other than the Finder.
   - *In recovery* the 68k is never released (core 0 waits for good); the card's disk images are not loaded, nothing
     of the Mac's is written, and `aurora68.log` stays as the last start left it, for the page to show. The network is
     `psmac_ip.c`'s, started by the recovery whether or not `remote=on` (the remote itself stays off: there is no Mac
     to drive): the wired port or a USB Ethernet adapter by DHCP, and when no DHCP server answers within about
     twelve seconds an address from 169.254/16 made from the MAC address (DHCP goes on asking every 15 s), so a laptop
     on a direct cable finds `http://aurora68.local/` by mDNS; on the WiFi (`wifi.ssid/psk/country`) an IPv4 address
     of its own with the radio's MAC address, nothing being shared with a Mac. The screen (HDMI through the VideoCore
     buffer, the console kept off it; on the SE, Plus and 512K boards the compact Mac's 512 by 342 one-bit screen
     through `psmac_se_mark`, the same row sender the Mac's own picture uses): "Aurora68 Recovery", the reason, the
     address and the `.local` name, a four-digit PIN in large digits, the wired and WiFi states, the card, the upload
     in progress or the last action, the rollback note, and the keyboard menu.
   - *Security.* Every request but the page needs the PIN (`X-Aurora-Pin`; a new one from the BCM2835's random
     generator at every recovery start); after ten wrong ones a new PIN and a minute of 429s, the screen showing
     "----" meanwhile. Requests from outside the local network are refused by the remote control's own rule
     (`psmac_remote_private_source`). The PIN is drawn on the screens only; the log says "PIN shown on screen", and
     `mac.recoverypin=` (the testbed's fixed PIN: scripts cannot read a screen) is hidden in the log like a password.
     The WiFi password and the remote's token never leave the card: cmdline.txt and cmdline.bak are served with them
     as `***`, the forms show "set" or "not set", and a line saved with `***` keeps the stored value.
   - *The card's root as a file manager sees it* (`psmac_card.c`, its last part). Until now the kernel only rewrote
     files of its own in place. Recovery lists, reads, writes (any size: a cluster at a time, CMD25 multi-sector
     writes with a fall-back to single sectors on the first failure), renames and deletes, with long names read and
     written, and is written so that a power cut at any moment leaves a card the firmware boots: a new file's entry is
     written empty first, then its data and its chain (each cluster marked the end before the one in front is joined
     to it), and only then its first cluster and size; a file's content is replaced by changing its own short
     entry's first cluster and size, one sector write, so `kernel8.img` is never absent for an instant. Where the two
     entries of a swap lie in different sectors there is a moment when both start with the same cluster; the next
     recovery start finds such a temporary name (`kernel8.new`, `aurora68.up`, `aurora68.tmp`, `kernel8.old` sharing
     kernel8.img's chain) and removes its entry without freeing the clusters, and no delete ever frees a chain another
     name starts with. An unfinished upload's temporary is deleted. The cost of the window is at worst a chain of lost
     clusters, never a broken file.
   - *Kernels.* An uploaded `kernel8.img` goes to `kernel8.new`, is read back from the card and checked: gzip header,
     deflate stream, CRC32 and length by libdeflate (already in the kernel), nothing after the stream, then the ARM64
     image header's `ARM\x64` at offset 56 and the texts `[BOOT] Build ID` (every Emu68) and `[PSMAC]` (every Aurora68).
     Only then: the old `kernel8.old` deleted, `kernel8.new` renamed `kernel8.old` in place (one write), and
     `kernel8.img` and `kernel8.old` swapped (one or two writes). Rollback is the same swap, after checking
     `kernel8.old` the same way (`force=1` skips the check); when the counter brought recovery and `kernel8.old`
     exists, the page puts "Roll back the kernel" first. Every other upload is read back and compared by CRC32, and
     the client compares the CRC32 the kernel answers with its own.
   - *The page* (`GET /`, 11 KB of HTML and JavaScript, for a phone as much as a laptop) and the API under `/r/`:
     state, files (get, rename, delete, upload with progress), WiFi (a scan of the visible networks, a test join that
     reports success and the address got, and only then a save of `wifi.ssid/psk/country`; a failed test joins the
     old settings again; `XX` refused, the placeholder the kernel ignores), the common options as a form with a line
     from README's table each (the resulting line shown before it is saved), cmdline.txt and config.txt as text (one
     line for cmdline.txt; `.bak` kept for both), the last start's `aurora68.log` and this start's log, and the
     restarts. Options are edited as words (`psmac_cmdline.c`, plain C99 tested on the host by
     `tools/cmdline_test.sh`, 29 checks, run by `make -C mac check`): only the word asked about changes, every other
     byte of the line stays (spacing, order, options nobody here knows), a quoted value stays one word, a value with
     a space is quoted, duplicates after the first go. `aurora68.py recovery --pin N ls|put|get|rm|mv|cmdline|config|
     opt|wifi|log|rollback|reboot|state` is the command line for the same API.
   - *The keyboard menu.* A USB keyboard on the Pi (and the Plus's and 512K's own keyboards, which this kernel reads
     itself and whose ADB codes are mapped to the menu) opens a menu on the recovery screen with Return: a scan list
     to choose a network from, the password (stars on the screen), the country, `mac.fpu`, `mac.boot` and
     `mac.console`, Save (only what changed, with cmdline.bak), restart normally, restart into recovery. The SE's own
     ADB keyboard is read by the Mac's ROM, which does not run in recovery: on the SE it takes a USB keyboard.
   - *Tested* (QEMU solo board unless said; a 128 MB FAT32 card image with an MBR and 1 KB clusters, `CARD=` with
     `mac.cardearly=1`; pinet on the wired port and a second pinet behind `mac.wifisim=`):
     `mac.recovery=1`: the page, 401 without the PIN, ten wrong PINs giving a new PIN and a minute of 429s with
     "----" on the screen. A 6,000,000-byte random file uploaded in 25 s (about 235 KB/s under QEMU; CMD25 never failed)
     and downloaded in 12 s, SHA-256 the same; renamed to a long name with spaces, deleted; deleting kernel8.img refused.
     Kernels: a gzip with one byte changed refused (CRC/deflate), 2 MB of random bytes refused (no `ARM\x64`), the
     kernel with its `[PSMAC]` texts changed refused, the good one installed in 6.9 s with the previous one as
     kernel8.old (MD5s compared), rolled back and forward. cmdline.txt: two lines refused, one line written with the
     line before in cmdline.bak, the password shown and downloaded as `***` and kept on a save; options previewed,
     `mac.board=amiga` refused, saved with the unknown words kept. recovery.txt put on the card by hand: "asked for:
     put there by hand", the file gone afterwards; "restart into recovery" from the page: the next start "asked for
     from the recovery page". The counter: three starts from a blank disk image (the floppy with the question mark)
     wrote 01, 02, 03; the fourth was recovery, "the last 3 starts did not reach the Finder", with the rollback offered;
     "restart normally" from it, System 7.1's Finder at t=43.7 s, boot.txt 00 (the first try showed the Finder hook
     compared with the number read, not the number written, and left 01: fixed). Esc held 1.5 s on QEMU's USB keyboard
     after the USB start: recovery.txt, a watchdog restart, then recovery "Esc held on a USB keyboard during the start".
     The WiFi: a scan of the simulated radio's four networks on the page and in the menu, a test with a wrong password
     failing ("the password was not accepted") and the old settings joined again, the right one joining, a save with
     XX refused and with GB written; recovery on the WiFi alone (no wired port): DHCP from the WiFi's pinet, the page
     in headless Chrome at phone width through it. The keyboard menu on QEMU's USB keyboard: the scan list, "Free
     Hotspot" chosen, an empty password (an open network), DE, `mac.fpu` double, Save: `wifi.ssid="Free Hotspot"`,
     `wifi.psk` taken out, `wifi.country=DE`, `mac.fpu=double`, the rest of the line as it was. The link-local address
     was seen once by accident: after a watchdog restart pinet stopped answering DHCP (its ring files and a reset kernel
     disagree, a testbed matter), and the kernel took 169.254.19.143 after twelve seconds; it was not reached over HTTP.
     MAME's SE (`macse_ps`, `mac.board=se`, `mac.switchsim=1`, MAME having no programmer's switch to press): recovery
     "the programmer's interrupt switch was held at power-on", the page on the SE's own screen (MAME's snapshot) and the
     state over HTTP saying `switch`, the counter put back to 00. After all of it `fsck.fat -n` found the card clean.
   - *Not done:* the access point. With no WiFi settings or wrong ones, recovery was to start a network
     "Aurora68-Recovery" of its own. Alice68's WiFi code drives the FullMAC firmware as a station only (`WLC_SET_INFRA
     1`, `WLC_SET_AP 0` in `alice_net.c`); the firmware of the 43430 and 43455 does run an access point (Linux's brcmfmac
     uses it: `WLC_SET_AP 1`, the `bss` iovar, WPA2-PSK with the firmware as authenticator), but it needs a second
     bring-up path in that code, a DHCP server in `psmac_ip.c` and frames for several stations, and the testbed's
     simulated radio has no access point at all, so nothing of it could be tried here. The wired port, USB Ethernet with
     its link-local fall-back and the keyboard menu cover the same need meanwhile; the access point is left for a
     change of its own, to be tried on a real Pi. Nor was a Mac-side "restart into recovery" program written (the
     remote and the page do it). (The access point came on 23 November, reached with the interrupt button: item 118.)
   - *Not verified:* real hardware: the real interrupt switch, a real SD card's CMD25 writes and their speed, real
     WiFi joins and scans, a USB Ethernet adapter's link-local case, the Plus's and 512K's keyboards in the menu (the
     testbed has no M0110A keyboard ROM, `mackbd_m0110a.zip`), the SE's video and speaker with the 68k never started
     (the screen buffer is shown by the SE's video as soon as RAM is at 0, as in every start; whether the speaker hums
     from an uninitialised sound buffer is for the owner to listen for). And the limit the design names: a kernel that
     dies before it has read the card cannot rescue itself, and as far as we know a Pi 3's firmware has no fallback
     kernel of its own; a microSD extension cable led out of the case is the physical way back.
   - *Renamed (22 November 2026, the owner: "Let's call it setup and recovery, as this can do config").* The
     page, the Mac's screen, the log and the documents now say "Aurora68 Setup and Recovery"; the page became
     tabbed the same evening (tools/recovery/page.html). The working names stay: mac.recovery=1, recovery.txt,
     boot.txt and `aurora68.py recovery`, so no card's settings change.

107. **Aurora68 Play: one code base for the classic and the macOS Play (22 November).** The owner asked for "a common
     code base between the two". The classic Play (`mac/piplay`, C for the 68000) and the macOS one
     (`macos/Aurora68Play`, SwiftUI) had been written the same day against one design of the server's interface, but
     each had its own copy of every rule: the classic Play's `icy.c`, `stations.c`, `devices.c`, `libparse.c` and
     what `library.c`, `radio.c` and `fetch.c` knew; the macOS Play's Address, TrackName, PlayQueue, Devices and
     Models in Swift, reading the server's JSON.
   - *The layout.* A new top-level `play/core` (README there) holds everything that is not interface, in C that both
     compilers take: `text.c`, `names.c`, `address.c`, `proto.c` (the server's protocol: every request's path and
     every answer taken apart), `catalog.c` (a page of a list as the Library window's rows), `queue.c`, and
     `devices.c`, `icy.c`, `stations.c` moved from `mac/piplay` with their history. `media.c` stays with the classic
     Play: only it reads music files itself. A SwiftPM target must lie inside its package's folder and the classic
     Makefile reaches the core from `mac/`, so `Package.swift` moved to the repository's top with targets that name
     their folders (`PlayCore` is `play/core`, its `include/` the module's headers, `PlayCore.h` the umbrella);
     `make-app.sh` builds with `--package-path ../..`, the workflow runs `swift test` at the top and now also for
     pushes touching `play/` or `Package.swift` (it had no branch filter, so the branch `play-core` ran it).
   - *One wire format.* The text API, because a 68000 reads it with a loop and a modern Mac as easily. What the macOS
     app showed that `/mac/` did not carry: an album's folder (its cover is `/art?dir=`, and `f:FOLDER` plays it on
     Live or a player), a track's path (the app plays the original file through `/file/`), the devices' whole state,
     the path and album of a command's tracks, the Live channel, a way to send a command. The server gained
     `full=1` (those records), `enc=utf8` (UTF-8 both ways; a name with a character Mac Roman lacks would otherwise
     arrive as "?"), `/mac/now` and `/mac/command` (to a player, or to `live` with its queue's own actions). Without
     the two parameters every answer is byte for byte what it was, so the classic Play's requests did not change.
     Paths and folders are exempt from the 120-character cut of `/mac/` fields (a folder of 160 characters arrives
     whole: tested). The JSON stays for the web panel.
   - *What the C keeps to.* No header and no C library call (the kit has five functions); no allocation, no shared
     state; answers split in place (so the core decides no field lengths, and the macOS app's long UTF-8 paths cost a
     68000 nothing); 32-bit masking where arithmetic wraps; no floating point (positions in thousandths). Two
     things found on the way: `dv_make_id` and the shuffle's generator, written for a 32-bit `long`, would have made
     other ids and orders on a 64-bit Mac (masked now; checked by running the core's units as a 68020 program
     under qemu-m68k, big-endian, beside the host); and a C function and a struct of the same name (`a68_head`) are
     legal C but collide when Swift imports them, so the reader became `a68_read_head`.
   - *The classic Play on the core* (1.4): the Library keeps its window, views, covers and network timing; its
     pages, queue, commands, addresses and the probe's answer are the core's. Gains: Library > Repeat (the macOS
     queue's rule; the gapless stream goes round to the first song), Previous on the first song restarting it,
     and a fix: the second and later songs of a gapless run, when their connection was lost and made again,
     resumed from a start offset left over from the song before (it was reset only when the card caught up).
     An address typed without a port now means 8000, the server's default (it was 8423, the owner's port). Size:
     at `-O2` the Play came out 5 KB larger (the request builders' helpers were inlined into each); with the core at
     `-Os`, a section per function and `--gc-sections`, CODE 1 is 168588 bytes against 168772 before (5772
     relocations against 5511), with Repeat added.
   - *The macOS Play on the core* (1.2): Aurora68Core became thin wrappers (`Core.swift` lends the core an answer in
     memory that stays put and turns its fields into Strings). Gains: a station whose address is a `.pls` or `.m3u`
     plays its first stream (the classic Play's `icy_playlist_url`); a server without radio stations offers the
     built-in list (the classic Play's own, read by `stations_parse`); after the probe the address the answer came
     from is tried first; songs added to a queue that has played to its end are the ones that play (it restarted
     the old queue). The library filter still folds case and accents with Foundation, the one rule left in Swift.
     The zipped app is 2,282,365 bytes against 2,304,093.
   - *Tested:* see docs/09, "The shared core": the host tests in `make -C mac check` (136 unit checks, 24 on the real
     server's answers, the 136 again as a 68020 program; icy, stations and devices as before), the server's test (141
     checks), XCTest on GitHub's runner (19 tests; runs 45070804702 and 45071242421), and the classic Play in the
     testbed: file, Library, gapless queue, Repeat round the end, Next round the end, every device command (pause by
     `/mac/command`), a station by Open URL, a new album refreshing the list, discovery with the sender first, the
     preference kept.
   - *Not verified:* the macOS app on a real Mac against a live server (the owner: the library, Play on Live and on
     the SE, a radio station, a playlist station, a new album appearing); the classic Play on the SE; Find and covers
     (the testbed library has none).
108. **The bus check: `mac.buscheck=1`, one page of numbers on the compact Mac's own screen (22 November).** The
   tester's 512Ke of item 96, on a socketed PiStorm, drew its screen right but for rare single wrong words, while
   everything it read looked wrong: the screen check found words that differed in 6,011 of 6,605 rows (29 of 32 on
   average), the mouse's Y1 line changed forty times as often as Y2, the keyboard and the button gave tens of
   thousands of phantom events. A read sampled too early fits; so does a bad joint in the new socket, a data line,
   the RAM, or the chips, and one log could not tell them apart, nor say whether a software retry would cure it. So
   that every tester's machine gives the same comparable numbers in one photograph, the kernel can now stop before
   the 68k starts and measure the bus itself.
   - *What runs* (`psmac_buscheck.c`, the kernel's half; `psmac_buscheck_core.c` and `.h`, the tests and their
     reading in portable C). `mac.buscheck=1` on `mac.board=se`, `plus`, `512ke`, `512k` or `128k` over the
     PiStorm's GPIO: the service core runs it in `psmac_se_start`, after the protocol is up and the overlay is
     cleared and instead of the address-line test of item 68, and core 0 never releases the 68k
     (`psmac_se_wait_ready` decides it from the options alone, so it holds even if the service core is late).
     `loop` runs it again and again until power-off; `force` (or `force,loop`) runs it over the testbed's bridge,
     which it otherwise refuses ("its cycles are MAME's"), as it refuses the solo board (no logic board). For the
     recovery mode of item 106, `psmac_buscheck_run(report, arg)` is the whole check for any caller that owns the
     transport with the 68k stopped; `report` gets each line of the page (with a callback the HDMI page is left to
     the caller). The tests, all in board RAM below the top 64 KB (only the screen and sound buffers there are the
     hardware's; the Mac's memory is the Pi's), on the VIA's port bits that are outputs on every board, and on the
     SCC's baud-rate constant and interrupt vector, which do nothing while nothing runs:
     1. *RAM data lines*: walking ones and zeros, `$0000 $FFFF $AAAA $5555`, four random patterns, at 256
        addresses; the same word everywhere written to all and read back, a random word read back at once (so that
        an address fault cannot change what is read). Each word read three times and as two bytes. Per data bit,
        the reads that expected 0 and 1 and how many read the other; for every pair of bits the same split by the
        other bit, which tells a line that follows another or two joined from a stuck one; whether the second read
        of a wrong word repeats it (systematic), is right (transient) or is another wrong word; what the wrong
        words were: the address's low half or the address-high register (the Pi's last words on the lines: a read
        taken before the board drove them), the word before, `$FFFF`, `$0000`, or the marks written into every
        word of the screen (`$C3A5`) and the sound buffer (`$96E1`) while the tests run (the video's or the sound's
        fetch still on the RAM's bus); byte writes and word writes judged by a majority of five and of fifteen
        reads; one word read 1,000 times.
     2. *RAM address lines*: item 68's pairwise test over the whole RAM, run first. Each of the four words is
        found by seven reads, each counted only if it is one of the four test words or the cleared `$0000` give or
        take one bit (`$9999 $AAAA $BBBB $CCCC`: four bits apart at least, so a stuck data line does not hide them,
        and a random error or an early read is no vote). Each pair's result is matched against models (either line
        stuck at 0 or 1, one following the other, the two joined with a 0 or a 1 winning) with the fault on reads,
        on writes, or on both; the lines are named from the votes, a line that "repeats" from one up to the top is
        RAM smaller than assumed ("the board has 512 KB, not the 4096 KB assumed"), and the data tests' addresses
        are then chosen around what was found.
     3. *The chips*: DDRA, DDRB, T1's latches, ACR, PCR and IER written and read back 10,000 times each (500 in the
        testbed), wrong bits by position; the IFR read twice in a row; port B's and port A's output bits against
        what was written and their input bits' changes (PB6 is H4 on the Plus-era boards and moves); the phase read
        at `$F00000` (D0, docs/18 section 2) on those boards, a read only; SCC RR0 of both channels 512 times with
        the pointer used up first; WR12/WR13 of channel B read back as RR12/RR13; WR2 read back as channel A's RR2,
        3 us apart and back to back; RR0 of A then B and B then A at 0, 0.5, 1, 2, 3 and 5 us between the two
        reads, counting the second read's changes. Every other SCC access is 3 us from the last (the kernel's own
        rule on the Plus-era boards since the same evening, item 96, `psmac_gpio.c`), whose wait the check turns
        off for the deliberately closer reads (`psmac_gpio_scc_unspaced`).
     4. *Timing*: each access's duration by kind (RAM word and byte reads and writes, VIA, SCC): min, median (of
        255 kept as an even sample) and max, and the transport's timeouts.
     5. *Retries*: 2,048 random words, each read once, then in pairs until two agree (four pairs at most), then
        three times by majority: what each would leave wrong.
     6. *Video*: CA1's rising and falling edges timed to find which begins the blank; then sixteen frames of reads
        in the blank and the same number a third of the way into the picture, and every other frame reads through
        the whole frame counted by eighths. Edges that make no compact Mac's frame (5 to 100 ms, a blank under a
        quarter of it) skip the test: the VIA's reads are then in doubt themselves.
     7. *Spacing*: 1,024 reads back to back, 5 us and 50 us apart.
     8. *The board's ROM* at `$400000`: 512 words read four times each, and the whole ROM (64, 128 or 256 KB by
        board) summed against the checksum in its first long. It answers at once with no video interleave, so
        clean ROM reads beside bad RAM reads put the fault in the RAM's cycles, and bad ROM reads too in where the
        PiStorm samples (docs/18 M6).
   - *The verdicts* (thresholds beside their constants in `psmac_buscheck_core.c`): no errors CLEAN; up to 10
     wrong in a million, and at least two allowed, STRAY; more FAIL. A data bit wrong in 95% of the reads that
     expected the other level and right in 95% of the rest, with 64 reads of each behind it, is stuck ("D11 always
     reads 0: a stuck data line (socket pin 58, the Pi's GPIO 19)"); the pair counts name a line that follows
     another or two joined the same way; 90% of the bit errors on three lines or fewer, not stuck, is "an
     intermittent line (a socket joint, a contact), not timing". Otherwise errors over the bits are a read
     problem: "mostly at random" when under 25% of the wrong words are repeated by the next read ("a timing problem,
     not a broken line"), "the same way each time" over 75%, "partly at random" between; a quarter of them being
     the Pi's last words adds "the data was read before the board drove it", a quarter being the screen's mark "the
     video's fetch". Errors three times as frequent while the video reads the RAM as in the blank: contention;
     reads 50 us apart three times better: "a cycle started too soon after the last"; SCC second reads that change
     at least ten times back to back and a third as often by some spacing: the SCC's recovery time (H2). A retry
     helps when the better of two-agree and three-voting leaves a tenth of one read's errors and at most 0.1%;
     "helps but does not cure" at a third. A named data line explains everything else, and the page says so instead
     of listing its consequences. Address findings while one word in a hundred reads wrong are marked uncertain; a
     tenth of the pairs without a clear answer makes the address test UNSURE. A line that reads and writes wrong
     is "stuck, not connected, or following another line": from the Pi's side, as item 68 found, the three leave
     the memory in the same two halves.
   - *The page.* 64 characters by 42 lines in Emu68's Topaz 8 by 8, drawn by the Pi into the board's main screen
     buffer (`psmac_se_fb_base`, VIA PA6 set for the main buffer, the speaker's PB7 off) and drawn again every ten
     seconds; on HDMI the same lines in the console's cells, after which the console holds (`psmac_console_page`)
     while the log goes on to the serial line, PiLog's ring and the card; the screen's lines and the details (per
     bit, per register, the pairs, the SCC table, the frame's eighths, mean durations) to the log as `[BUSCHECK]`
     and to `buscheck.txt` in the card's root (at most 4 KB, one cluster; a smaller cluster cuts the details: the
     card writer's small files grew from 1.5 to 4 KB for it). The first lines carry the kernel's git hash and build
     time, the board and RAM size and where the size came from, the link (Pi 3 or 4 timings, the CPLD's clock from
     PLLC), and the CPLD's usercode.
   - *The CPLD's usercode* (docs/18 K2). Which PiStorm firmware a tester has decides when a read is sampled (the
     stock image of today, `00185866`, about 128 ns before a 68000 would; the pre-2021 original, `0018736F`, in S7),
     and nothing said which. Emu68's `cpld.c` reads it through libxsvf, not linked in psmac; `psmac_gpio.c` now
     loads IDCODE (0x006) and USERCODE (0x007) by hand on GPIO 24-27 as the protocol comes up, leaves the TAP in
     Test-Logic-Reset and GPFSEL2 as it was, and logs `[PSMAC:GPIO] CPLD JTAG: IDCODE ..., usercode ...: name`
     with Emu68's table of names (`mac.jtag=off` leaves the pins alone). Never run on a CPLD yet.
   - *mac.busfault=* (testbed only, ignored over the GPIO): faults on the wire's reads alone, in `psmac_wire_read`:
     `dN`, `dNh` (data line N reads 0 or 1), `aN` (address line N reads as 0, RAM only), `aN,M` (N follows M),
     `rand:P` (P reads in a thousand with random bits flipped), joined with `+`. `mac.selinefault=` (item 68) stays
     the fault on reads and writes.
   - *Host test* (`tools/buscheck_test.c`, in `make -C mac check`): the core against a simulated board (RAM, ROM
     with its checksum, a VIA keeping its registers with CA1 in a 60 Hz blank, an SCC with its pointer) under
     twenty-one conditions; each summary must say the right thing and not the wrong one, and no line of the page may
     be cut at the right margin: clean at 128 KB, 512 KB and 4 MB; D11 stuck at 0, D3 at 1; A14 following A15 on
     reads and on both; A9 stuck on reads; a 512K board taken for 4 MB; random errors at 50% and 2% (a retry
     helps); early sampling; errors only while the video reads; only back to back; an intermittent D6; the chips
     alone; RAM bad and ROM clean; RAM and ROM both bad; wrong words that are the video's; RAM, ROM and chips all
     noisy at once; an SCC that needs 2 us.
   - *In the testbed* (MAME's `macse_ps`, `macplus_ps`, `mac512ke_ps` with `mac.buscheck=force`; screenshots by
     MAME's snapshots and of HDMI through QEMU's monitor):
     - *Clean*: the SE at 4 MB (A1-A21, 210 pairs), 13 to 14 s and some 260,000 bus cycles over the bridge, whose
       median cycle is 44 us; the Plus at 4 MB, 10 s, the phase read D0 0, PB6 (H4) changing; the 512Ke at 512 KB
       (A1-A18, 153 pairs), 10 to 11 s. All three: "All clean: 54272 reads of RAM, the VIA and the SCC, no errors.
       The bus is good." MAME's VBL came out at a 16.45 ms frame and a 1,227 us blank begun by CA1's rising edge (28
       lines of 370: the real figure). The ROM sums never match their checksum in the testbed, whose bridged machines
       carry no ROM image (an INFO line). `buscheck.txt`: 4,095 bytes written and read back on a 64 MB FAT16 card
       image with 4 KB clusters; with 2 KB clusters the card writer refused it ("-10: longer than a cluster"), which is
       why the text is now cut to the card's cluster at a line's end. `mac.buscheck=force,loop` on the 512Ke: seven
       runs in 110 s, the page renewed each time. Without `force`: "not run, this is the testbed's bridge, not a
       PiStorm: its cycles are MAME's (mac.buscheck=force runs it anyway); booting as usual".
     - *With faults*: `mac.busfault=d11` gave "D11 always reads 0: a stuck data line (socket pin 58, the Pi's GPIO
       19). Everything else that read wrong follows from that line." (before that last sentence existed the page went
       on to list the retries, the chips and the word writes that the line had spoiled); `d3h` on the 512Ke "D3 always
       reads 1"; `a14,15` "A14 follows A15 when reading: a short between them or at the latch (socket pins 42, 43)";
       `a9` "A9 is stuck at 0 when reading (socket pin 37)"; `mac.selinefault=14,15` (reads and writes) "A14 does not
       reach the RAM as both 0 and 1, reading and writing: stuck, not connected, or following another line"; `rand:20`
       "Reads of RAM fail on 16 of 16 bits, 1.9% of words, mostly at random (a second read repeats the wrong word 0%
       of the time): a timing problem, not a broken line. A retry would help: one read 2.4% wrong, two that agree 0,
       three voting 0.1%", the ROM's reads failing too (40 of 2048) and the chips' (the fault is on every read);
       `rand:300` on the 512Ke, the nearest to the tester's numbers, 30.1% of words, "a retry would help but not cure
       it" (31.3%, 3.5%, 15.1%), the address lines all decided in spite of it; `rand:500` 49.9%, the address test
       UNSURE with 51 of 210 pairs undecided.
     - *Learnt on the way*: random errors on the VIA's reads make CA1's flag seem to come and go (a "frame" of
       0.3 ms), hence the plausibility check before the video test; and with a line wrong on reads and writes alike
       both "stuck at 0" and "stuck at 1" fit every pair, so the page no longer names a level for it. The first
       version of the address test elected garbage words by majority; it now counts only reads that are one of the
       test words, which also keeps a read taken early (the address on the lines) from voting.
   - *Not verified:* everything on hardware: the page on a real compact Mac's screen, the JTAG read on a real CPLD,
     the timings over the GPIO (the testbed's bridge takes some 44 us a cycle, a PiStorm about one), the VBL edges
     and the SCC spacing on real chips, `buscheck.txt` on a real card. The tester's 512Ke is the first machine it
     is for: READ-ME-FIRST's "If your Mac misbehaves: the bus check" says what to do and what to photograph.
109. **Music videos in the music server and both Aurora68 Plays (22 November).** The owner's library on the NAS also
     holds music videos, in the same folders as the albums, and they asked for them in the server and in both Plays.
   - *The server* (`tools/music-server`, README "Music videos"). The scan finds `.mp4 .m4v .mov .mkv .webm .avi .mpg
     .mpeg .ts` files beside the songs, kept apart from the folders' songs (a folder of videos alone is no station,
     and the stations file did not change). The catalogue attaches each to its album when it sits in an album's
     folder or in a `Videos` folder inside one, to its artist in an artist's folder (one whose albums are folders
     inside it), else by its folder's or its own name; its title is its file's name tidied as a track's is, by the
     Python mirror of `play/core/names.c` the tracks already used (now one function, `tidy_title`). ffprobe says
     each video's length, size and codecs once, kept in the new cache folder (`MUSIC_CACHE`, `videos.json` by path,
     time and size): the NAS is slow, a rescan every 30 seconds must cost nothing, and a restart probes nothing it has
     seen (files without a picture are kept there too, or they would be probed at every scan: the test caught that).
     The copy rule applies with the videos' own order of kinds and without a track number. Videos count in the
     generation and `/api/info`.
   - *Three ways to send one*, chosen per file: the file as it is when AVFoundation plays it from a file (MP4, M4V,
     QuickTime; H.264 at 8 bits 4:2:0 or HEVC tagged `hvc1`; AAC, ALAC or MP3); else remuxed into fragmented MP4 with
     the picture copied (an MKV or TS of H.264; AC-3, DTS or Opus sound converted to AAC on the way, since sound is
     cheap and the picture is not; HEVC retagged `hvc1`, which QuickTime insists on); else transcoded to H.264 and AAC
     (VP9, AV1, MPEG-2, 10-bit H.264), at most `VIDEO_TRANSCODES` at once (1) and refused with `VIDEO_TRANSCODE=off`,
     the reason logged and answered in words. AVFoundation will not play a progressive file whose server cannot answer
     a byte range (error -11850), and a stream made as it is sent has no length, so the remuxed and transcoded are
     also HLS: `/video/ID/index.m3u8`, four-second fragmented-MP4 segments written to the cache as ffmpeg makes them,
     the playlist served with `EXT-X-START` at 0 (a growing "event" playlist otherwise starts near its end), a
     finished one kept for next time, one nobody fetches from for a minute stopped. Posters (`/vthumb/ID`, a frame a
     tenth of the way in), the sound alone (`/vaudio/ID`, an MP3 stream with an end exactly as `/t/ID`, converted once
     and kept; `/t/` takes a video's id too), and, at the coordinator's request for the video card's driver being
     written beside this, `/vmpeg/ID?w=&h=&start=`: an MPEG program stream with the video helper's own ffmpeg
     arguments and size rules (MPEG-1 fitted and padded black, 25 frames a second, no B-frames, MP2 at 44.1 kHz
     stereo, 128 kbit/s, at most 1280 x 720, even sizes), counted as a transcode, `mpeg1` in the capabilities. No
     Play uses it yet.
   - *The interface.* JSON: `/api/videos`, videos in `/api/tracks`, `/api/catalog`, `/api/list`, `/api/info`. The
     players' text gained a third parameter, `videos=1`: `V` records after an album's or an artist's tracks (id,
     title, artist, seconds, album, width, height; with `full=1` path, album title, folder, how it is sent),
     `/mac/videos`, a fifth field for artists (and artists with videos alone), `V` hits, `/mac/info`'s count and
     capabilities, the devices' 18th field (shows video), `V` lines in commands. Without it every `/mac/` answer is byte
     for byte what it was: the test compares fifteen of them, both encodings, with a server started with `VIDEOS=off`.
     Devices say whether they show video (`video=1` on the announce); `v:ID` plays a video anywhere (to one without a
     screen the answer adds `audio_only`, and Live queues its sound).
   - *The panel* lists a folder's videos with posters and plays them in the page (the file, HLS in Safari, the
     fragmented MP4 elsewhere) or on the chosen player. The coordinator then passed on the owner's request for the
     project's web pages to look like the Macintosh of the 1980s, as the Setup and Recovery page already did
     (`tools/recovery/page.html`): the panel took that page's CSS (the desktop pattern, pinstriped title bars with
     close boxes, hard shadows, the default button's ring, alert-box messages, Chicago and Monaco with fallbacks, the
     six-colour stripe and a colour for each window) and the Aurora68 logo (`docs/img/aurora68-logo.svg`) inline and
     as the favicon, its requests and functions unchanged. Rendered with Playwright's headless Chromium at 390 and
     1100 pixels wide (the library, an album with its video, the video's window): on a phone the windows stack, Now
     Playing first, its cover at half the width.
   - *The core* (`play/core`): `a68_client.videos`, `a68_video` and `a68_next_video`, `a68_next_item` (a track or a
     video; `a68_next_track` passes over `V`), `V` hits and command lines, the info's count and capabilities, the
     artist's count, the device's flag, `dv.video` for the announce, rows of kind `V` (not counted in an album's
     length), `A68_V_ARTIST` (an artist with videos alone), and the paths `/mac/videos`, `/vaudio/ID`, `/vthumb/ID`,
     `/video/ID` (the file for one sent as it is, else HLS), `v:ID`.
   - *The macOS Play 1.3*: a video is a `Track` with `video` set, so the queue, the commands and the player take it as
     they take songs; the `AVQueuePlayer` plays its item from the core's address, and a Video window (AVKit's
     `AVPlayerView` over the same player: its controls, full screen) opens when one starts. Videos follow an album's
     songs and an artist's albums as posters; Now Playing says video and shows the poster; Live and other players get
     `v:ID`. It announces that it shows video.
   - *The classic Play 1.5* asks with `videos=1`: a video is a row marked "Video:", without a number, after the
     album's tracks; playing it plays `/vaudio/ID` as a track plays `/t/ID`; an album's Play stays its songs; an artist
     with videos and no album opens on them; a video sent from elsewhere comes as a `V` line and plays the same way.
     It announces no video. The picture on the SE is the next step: the kernel's video card (docs/16, issue #24's
     Aurora68 Video program) will take `/vmpeg/ID` once its program exists.
   - *Tested.* The server test: 203 checks, all passing (62 new: four ffmpeg-made videos of a few seconds at 160 x 120 and a .mp4 without a picture in a temporary folder; the scan, the copy, the facts and their cache, Range, the remux checked by ffprobe and by its H.264 bytes being the original's, its HLS, the transcode, refusals with transcoding off and beyond the limit, no ffmpeg left, posters, soundtracks through the classic Play's parser, `/vmpeg/` at three sizes with no B-frames, the JSON, the text both ways, the byte-for-byte comparison, devices). The core: `tools/play_core_test.c` 158 unit checks (22 new) and 34 on the real server's answers (10 new: `tools/play_core_fixtures.py` adds two videos to its library; the core tidies their file names as the server does), the 158 again as a 68020 program under qemu-m68k; `devices_test` 33 as before. XCTest on GitHub's macOS runner: 21 tests (2 new, `VideoTests`; the older paths now end in `&videos=1`), runs 45084364767 and 45085274385, and the app built. The
     music-server workflow now runs its test and an image build on every branch that touches it (publishing from main
     only) and checks the Alpine image's ffmpeg has ffprobe, x264, AAC, MPEG-1, MP2 and LAME: runs 45084364758 and 45085274273, both green (ffprobe 8.1.3 in the image). The classic
     Play in the testbed: not run in this session. The emulator lock was held by another session from 23:39 to 01:09, and when it came free the build machine's disk was full (1.7 MB left; the scratch copy of the test image had to be deleted) and another QEMU was already running, so no run was started. The classic Play 1.5 builds cleanly (CODE 1 170152 bytes), and its new paths go through the core that the host and 68020 tests check against the real server's answers. The session is ready to run: a copy of b-play.image with the new Play installed, the server on a library with an MP4, an MKV and a WebM video, and a script that opens an album with a video, plays the video's sound, opens an artist that has only videos, and sends a video to the SE from the server. (Run on 23 November 2026, item 114: all four passed.)
   - *Not verified:* the macOS app on a real Mac (the Video window, full screen, HLS of a remux and of a transcode in
     AVFoundation, a video in the queue followed by a song, Now Playing's poster), the panel's video in Safari and on
     a phone, the owner's own videos (their containers and codecs, a NAS's speed at remuxing and transcoding), the
     Alpine image on the NAS, and the classic Play on the SE.

110. **The video decoder card: a driver in its ROM, QuickTime's decompressors on the Pi (22-23 November, issues #13
     and #24/#25).** Item 98's card took an MPEG stream from a program that had yet to be written (issue #24, Aurora68
     Video, with a helper computer doing YouTube). That evening the owner turned the work round: "This video card should
     leverage the Pi hardware and have no external dependencies. Think of it as an in-box video processing card that
     integrates into QuickTime and is also available to other programs through traditional means on classic Macs";
     "it's essentially another NuBus card"; "we will tackle YouTube later"; and its name, "video decoder card". Nothing
     of issue #24's network program had been written when the new order came, so nothing of it is kept; the helper
     (issue #22) stays as it is, an optional extra. docs/16 has the design as built ("The in-box card") and the study
     of the modern formats; `docs/video-decoder-api.md` the programming interface.
   - *A NuBus card of the period.* The card's declaration ROM (slot $C, `tools/mac/mkvideodeclrom.py`, compiled into
     the kernel) now names the board "Aurora68 Video Decoder" (vendor "Aurora68 project", revision 2, part
     "A68-VDEC") and carries its driver, ".A68Video" (`mac/a68video.s`, 68k assembly like the slot-9 drivers), in the
     functional sResource's driver directory; with no sRsrcFlags the Slot Manager opens it at start-up, before the
     first extension (`[MAC] A68Video: the Aurora68 Video Decoder card's driver is open ...`). The sResource's type
     {$7050, 3, 1, 'AV'} is unchanged; its name is "Aurora68_Video_Decoder". Control 100 to 114 and Status 100 to 103
     are the card's interface for any program (numbered from 100: Control 1 is the Device Manager's killCode).
   - *The frame decoder* (`psmac_vframes.c`): eight request slots in the card's window from $1100; a session per
     movie (open with the format, size, depth and the ImageDescription; decode; draw again; close), sixteen at once.
     The decoders are portable C without kernel calls (`psmac_vcodec.h`): Cinepak, Apple Video ("Road Pizza"),
     Animation (1 to 32 bits) and Graphics, each written from the format's public description (Tim Ferguson's
     Cinepak notes, the MultimediaWiki pages); FFmpeg's code was not looked at. `tools/vcodec_test.sh`, now part of
     `make -C mac check`, has FFmpeg encode nine small movies (Cinepak two ways, Apple Video two, Animation at 16, 24
     and 32 bits, Graphics two), extracts their samples (`tools/movsamples.py`) and compares the card's decoding with
     FFmpeg's: all nine identical byte for byte. The first Graphics comparison differed from the second picture on;
     the format's colour caches are filled from their first entry again in each picture, which the description
     leaves unsaid.
   - *Drawing into any pixel map:* the request names a 68k address, row bytes, depth, place, size and clip; the
     kernel reaches RAM page by page through the MMU shim's table and the screens by the addresses their GDevices use
     (`psmac_mac_span`, `psmac_mac_copy`, `psmac_mbox.c`). 32 and 16 bits as they are; 8, 4 and 2 through the map's
     own colour table (a 32,768-entry nearest-colour table per seed), an indexed picture with the same table index
     for index, others with an 8 x 8 ordered dither; 1 bit by a Bayer threshold. Each session keeps its whole last
     picture, and a picture drawn where the last one was redraws only the rectangle it changed (QuickTime's own
     decompressors draw only the blocks a picture brings): 298 of 300 Cinepak pictures and 299 of 300 Apple Video
     ones were drawn so in the testbed.
   - *QuickTime* (`mac/vdec`, the "Aurora68 Video Decoder" extension, type 'thng', `tools/mac/elf2thng.py`): four
     'imdc' components, manufacturer 'A68v', registered by QuickTime from the Extensions folder and chosen before its
     own. Each declines registration where ".A68Video" does not open, and opens QuickTime's own decompressor of its
     type beside itself to hand it whatever it does not do: everything when frame decoding is off (`mac.vdecode=off`
     or the driver's Control 101), a destination depth or picture the card refuses, a picture loaded in pieces, any
     selector it does not implement. The code is one resource that fixes itself up wherever the Component Manager
     loads it (`mac/vdec/entry.s`).
   - *Three faults on the way*, each found from the kernel's records: the GetComponentInfo glue used selector 6
     (GetComponentListModSeed) for 5 and left the stack five arguments deep: "bad F-Line instruction" at $100 while
     QuickTime registered the components (`[CRASH]` showed the jump to low memory from the component's code);
     StripAddress's answer came back only in D0, where GCC reads a pointer from A0, so the ImageDescription was read
     from a stale address and its type was 0; the driver used A1, in which the Device Manager hands over the DCE and
     IODone wants it back: System error 14 at MoviePlayer's first picture.
   - *Testbed, solo board* (System 7.1, QuickTime 2.5.1, MoviePlayer 2.5 driven through the remote control's
     semihosting channel; FFmpeg's movies, 320 x 240, 60 frames a second, Movie > Play All Frames). The component
     measures the time each picture spends in BandDecompress (Microseconds) whoever decodes it; the card's own times
     are its log's. At 1 bit: Cinepak through the card 1,028 us a picture (the Pi: about 200 us to decode, 150 to
     draw) against QuickTime's own 339 us; Apple Video 1,169 against 672; QuickTime's Animation 343 and Graphics
     745. At 8 bits (Aurora68 Depth 8): Cinepak through the card 1,296 us against QuickTime's own 2,207, and 42.8
     against 41.5 pictures a second; Apple Video 1,766 against 354; QuickTime's Animation 313. Pictures:
     `solo-cinepak-8bit.png`, `solo-rpza-1bit.png` in the scratchpad (screenshots of the Mac, not committed).
   - *What the numbers say.* On this machine the 68k is not a 68000: Emu68 translates QuickTime's own decompressors
     into ARM code, and at one bit, and for the simple formats, they are as fast as the card's whole round trip. In
     the testbed most of that trip is not decoding: of about 1 ms the Pi spends 0.35 ms; the rest is the 68k's call
     through the Device Manager and waiting on four QEMU threads that share four host cores (core 1 now keeps looking
     for a quarter of a second after each request instead of sleeping until an interrupt, which QEMU's wfe does; it
     made no measurable difference here and should on a Pi, whose timer events wake a sleeping core within 0.43 ms).
     Cinepak into a colour screen, the format and the case where QuickTime's own code works hardest, is where the
     card wins already, by 1.7 times. Not done: asynchronous decompression (codecCanAsync, the 68k going on while the
     Pi decodes, which would hide the round trip entirely), and a choice per format of whether to use the card.
   - *Testbed, SE board* (MAME's SE through the bridge, the SE's own 512 x 342 screen, System 7.1 from the card's
     image): MoviePlayer opened the Cinepak and Apple Video movies and the card drew their first pictures, dithered,
     into the SE's screen (`se-cinepak-1bit.png`); playing advanced three pictures in a minute while the 68k turned
     in a loop of the System's reading the VIA; with `mac.vdecode=off` QuickTime's own Cinepak did the same (the first picture, then the same loop, `se-cinepak-1bit-quicktime-own.png`), so the stall is the SE testbed's (QuickTime's clock over MAME's VIA, not followed further), not the card's.
   - *Not verified:* the real SE and HDMI on hardware; 16 and 32 bits (only 1 and 8 were run); a window partly off
     the screen or covered (QuickTime then gives the card its offscreen buffer, untested); the Animation and Graphics
     formats in MoviePlayer through the card at 8 bits (the runs' automation lost those windows).

111. **Setup and Recovery's Display tab: the HDMI monitor's mode chosen without the Mac (22 November).** Aurora68
   Display (docs/10) needs a Mac that starts. The recovery page now has a seventh tab, Display (its colour swatch all
   six colours: the colour screen's tab), that does the same from a phone, and `aurora68.py recovery display`.
   - *The EDID in a recovery start.* Already read: psmac_recovery_phase1 runs in psmac_setup_memory, but
     psmac_mbox_init and with it psmac_display_init and psmac_hdmi_init (the EDID, core 0) still run before core 0
     parks, and the console is kept off the recovery page from phase 1 on. The testbed log of a recovery start lists
     the sample's 17 modes as any start does. One thing did disturb recovery: psmac_hdmi_poll's 300-second "keep the
     new mode?" clock ran on core 3 in recovery too, and would have put the .bak back and restarted the Pi under the
     page; it now waits for the next normal start (with `mac.displaywait=30` the mark stayed pending past 60 s).
   - *A bug found on the way:* psmac_hdmi.c read config.txt into 4 KB, the release's own config.txt is 5.3 KB, and
     psmac_card_read_file refuses a file longer than the buffer, so on a release card Aurora68 Display said
     "config.txt was not read" and missed the include line. Now 16 KB. Before the fix the testbed's start said
     "config.txt not found"; after it, "in force: 1920x1080 at 60 Hz, aurora68-display.txt: hdmi_group=1 hdmi_mode=16".
   - *API.* `GET /r/display` (PIN as always): the monitor (name, maker, product code, the serial number cut to its
     last four characters, size in mm and inches with the diagonal, the panel's size and shape, HDMI or DVI and its
     sound, EDID version), the mode in force from the firmware's buffer and what set it, the preferred mode, the
     suggestion and its reason, every mode with its verdict (usable, alternative, too_fast, interlaced, mac_size),
     refresh, clock, alternative and the `WxH@Hz` that chooses it, aurora68-display.txt as it is and what it sets
     from the next start, config.txt's include state and cmdline.txt's mac.screen. `POST /r/display` with
     `mode=WxH[@Hz]|auto`, `rotation=`, `include=1`, `confirm=0|1`, `screen=keep`: the file by psmac_edid.c's
     writer, the old one as aurora68-display.bak, each read back and compared; a mode with no include line in
     config.txt is refused (409) unless include=1, which puts the line at the end (config.bak kept); a mac.screen=
     follows the new size (taken out for Automatic), by docs/10 "Any size"'s rule that it is needed only to differ.
   - *psmac_edid.c* gained edid_mode_choosable (Aurora68 Display's rule), edid_find_mode, edid_display_auto
     (`hdmi_group=0`, the firmware's documented auto-detect, with `#aurora68 confirm=auto`), edid_config_include and
     edid_standard_modes (no EDID: every 60 Hz DMT mode and 2560x1440, judged by the clock alone); tools/edid_test.c
     checks all of them (make -C mac check: passed).
   - *Tested* (QEMU solo board, `mac.recovery=1 mac.edidtest=sample mac.hdmisim=1`, a 128 MB FAT32 card image with
     the release's config.txt less its include line and `mac.screen=640x480` in cmdline.txt, pinet on the wired
     port): GET 401 without the PIN, 200 with 5 KB of JSON; a mode without the include line 409, 2560x1440@60 422,
     rotation 45 422; "Use the suggestion" in headless Chromium: 2560x1440 at 30 Hz written with `hdmi_cvt=2560 1440
     30 3 0 0 1`, the include line added (config.bak 5,250 bytes), mac.screen=2560x1440 (cmdline.bak); then from
     aurora68.py `use auto --rotate 90` (mac.screen taken out), `rotate 0`, `use 1920x1080@60` (`hdmi_group=1
     hdmi_mode=16`), `include` (present). fsck.fat found the card clean. A new start from that card: mac.hdmisim made
     the 1920x1080 buffer the firmware would, and the tab said so. A 64 MB FAT32 image was first taken for FAT16 by
     the kernel (fewer than 65,525 clusters: the FAT specification's rule), which is right and only a testbed matter.
     After "restart normally" QEMU's Pi restarted by the watchdog, but pinet did not answer DHCP again (item 106's
     known testbed matter); the second start was therefore a fresh run.
   - *Not verified:* the firmware's side. Whether a Pi 3's firmware takes `hdmi_group=0` and the written modes, and a
     real monitor's EDID, need a Pi and a monitor. The recovery screen on a large HDMI mode fills only its top-left
     640 by 640 pixels (80 columns of 8 x 16 cells, as before this change).

112. **Aurora68 Video: MPEG-1 films from the Mac's disks on the video decoder card (23 November, issues #24 and #25).**
     After item 110 the owner's order was MPEG-1 from local files next, the network browsing later. Aurora68 Video
     (`mac/a68player`) is the card's player: an MPEG-1 program stream read from disk straight into the card's ring
     (PBRead into the ring's own address, so no copying on the 68k), the stream started, paused, resumed, stopped and
     placed through the driver ".A68Video" (item 110), its picture drawn by the card into the window's place on its
     screen, "hidden" while the window is behind, dragged or under a menu. The kernel gained one thing: the last
     picture drawn again when the place changes while the stream is paused or dry (`psmac_video.c`), so a paused film
     follows its window and an uncovered one comes back without the program painting anything.
   - *Seeking* is stop, the first 2 KB pack, the file from the nearest pack boundary, start: without the first pack
     (its system header and the video's sequence header, written once by the encoders) the card answered "no MPEG-1
     video found at the start of the stream". The length is the first pack's mux_rate when believable, else an
     estimate from the decoder's progress; FFmpeg's mux_rate for a test pattern's film said 55 MB a second.
   - *Tested on the solo board* (System 7.1, a 12-second FFmpeg film, 320 x 240 at 25 frames with MP2): Standard File
     and the Finder (the bundle's 'MPEG' and 'MPG ' documents) opened it; 300 of 300 pictures shown, never dry; pause,
     a drag and the redraw at the new place; a seek to the middle (150 pictures to the end); the Card menu's switch for
     QuickTime decoding. Under QEMU 0.8-1.0 ms to decode a picture, 0.7-0.8 to draw it, 1.3-4.7 per cent of core 1.
   - *QuickTime's own MPEG-1* (MoviePlayer opening a .mpg, issue #25's first wish): designed in docs/16 ("MPEG-1 from
     the Mac's own disks": an 'eat ' import that indexes the stream into two tracks, a fifth 'imdc' around pl_mpeg's
     video decoder, an MP2 'sdec'), not built: QuickTime 2.5 has no separate decoding and display order for
     B-pictures, and the sound decompressor is a second piece as large as the import. The Finder hands MPEG files to
     Aurora68 Video meanwhile.
   - *Not verified:* the film's sound (the testbed has no sound output; on the SE it goes to HDMI or the jack, or the
     speaker with `mac.sestream=on`), the SE board and HDMI with Aurora68 Video, a film larger than the window's
     screen, System 6.

113. **MPEG-1 films in MoviePlayer: the Aurora68 Video Decoder's movie import (23 November, issue #25).** Issue #25's
     first wish, an MPEG-1 file played in MoviePlayer through the card, which item 112 left designed and not built.
     Built in the extension of item 110: a movie import ('eat ', for the file types 'MPEG' and 'MPG ',
     `mac/vdec/import.c`) that makes a QuickTime movie of the file without copying it, a fifth decompressor ('mpg1')
     around a new kind of session of the card's frame decoder (pl_mpeg's video decoder one picture at a time,
     `psmac_plmpeg.c`), and the MP2 sound decoded by the card once, at import, into PCM (a sound session, op 5).
     The indexer that reads the file (`mac/vdec/mpgindex.c`) is portable C that the 68k and the host test share.
     docs/16 "MPEG-1 in QuickTime" is the description; what follows is how it went.
   - *Opening.* The import is marked canMovieImportInPlace. From the Finder, MoviePlayer 2.5 refused a film it was
     handed ("Couldn't open the file because the file is not a movie file") without calling the import. Through
     File > Open... the films were listed with a "Convert" button; Convert called the import twice (Standard File's
     preview, then the opening), and both times the import said paramErr: it had refused calls without
     movieImportCreateTrack, and QuickTime's in-place import sets no flag at all. Refusing only
     movieImportMustUseTrack, Convert opened the film in place, in a window with its name, nothing saved.
   - *Playing.* The first press of Play left the movie on its first two pictures, QuickTime asking for samples 1
     and 2 again and again (192 PreDecompress calls in a minute); the sound track was one chunk of 1.4 MB. With the
     PCM in half-second blocks, four bytes apart, so that each is a chunk of its own, the movie played at once. The
     window then went white at the end: the sound track was 26 ms longer than the pictures (MP2's last frame); it is
     now cut to their length.
   - *Jumps.* A click in the middle of the controller showed a picture whose digit looked like an 8: the test
     pattern's 0 and 4 laid over each other. The card's new log (`mac.vframelog=1`, a line a picture) showed why:
     QuickTime 2.5 handed the decompressor sample 104, a P and a B, and nothing before it, so the B was decoded
     against the references of picture 22. QuickTime does not decode from the preceding sync sample when it
     jumps, nor when it drops frames while playing; it passes the sample's own number as frameNumber (1, 2, 3...).
     So the import puts its index into the ImageDescription (an atom 'mpgx': the film's FSSpec and size, every
     picture's start and type), and the decompressor, given a sample that is neither the next one nor a sync
     sample, reads the samples from the sync sample before it from the film (the B-pictures alone left out) and has
     the card decode them undrawn first. The same click then showed picture 103, which matched FFmpeg's in 99.7 per
     cent of the pixels, better than its neighbours. Without an index (more than 32,768 pictures, or a saved
     reference movie whose film moved) jumps show what the card makes of the sample alone.
   - *B-pictures.* The samples are in display order; the first B after a reference carries the reference's bytes
     in front of its own, and the reference's own sample comes when it is shown, which the card recognises by a
     hash of its bytes and shows without decoding (lower case in the frame log: "p"). The design's other way, a
     decoder holding a picture back, would have shown everything a frame late.
   - *Without the card.* `mac.vdecode=off` and `mac.video=off` both tested: the film opens, its video track disabled,
     a text track (QuickTime's handle data handler) saying what is missing, white on black.
   - *Tested* (solo board, System 7.1, QuickTime 2.5.1, MoviePlayer 2.5 at one bit; FFmpeg films of eight seconds,
     320 x 240, 25 frames a second without and with B-pictures, 29.97 with B-pictures, MP2 at 128 kbit/s): all
     pictures through the card with Play All Frames (200, 200, 240), frames 5 (a B), 21, 103 and 199 compared with
     FFmpeg's decoding dithered as the card dithers (99.7 per cent of the pixels; each its own frame best); in real
     time QuickTime fell back to key frames (the testbed's 68k runs at about 20 MIPS) and the sound reached the
     card (`mac.audiodump=`: the 440 Hz tone, with the testbed's underruns). The import took 2 to 2.6 s for 440 KB
     under QEMU, 9 to 12 s with the sound decoded and written. Host test `tools/mpg1_test.sh` in `make -C mac
     check`: four films, every picture against FFmpeg (52 dB or better), seeks, jumps with the catch-up, redraws,
     the sound (59 dB with a constant gain allowed: pl_mpeg's synthesis is 0.2 to 0.7 dB quieter than FFmpeg's).
   - *Elsewhere:* `tools/mac/mktraps.py` learned selector-dispatched traps (`AAAA/0188`), for the Movie Toolbox,
     Alias, Folder and FSSpec calls of `mac/vdec/traps.txt`; their numbers come from QuickTime 2.5's interfaces
     (Universal Interfaces 2.1.4, read, not copied). `tools/mac/aurora68-layout.py install --films` puts films into
     Aurora68 Video's "Films" folder, typed 'MPEG'; `aurora68.py push` types .mpg files the same way.
   - *Not verified:* speed and the sound on the SE (the testbed's 68k is about a twentieth of a Pi's, and it has no
     sound output), colour screens and HDMI, MPEG-1 from encoders other than FFmpeg, long films, System 6.

114. **The classic Aurora68 Play 1.6: music videos on the video decoder card; the owed test of 1.5 (23 November).**
     Item 109 left the classic Play playing a video's sound and the test of that in the testbed undone; item 112 made
     the card's MPEG-1 stream channel a program's to use, and docs/16 ("Follow-up") said how Play could use it.
   - *1.5 in the testbed first* (the solo board, System 7.1 with Riptide, the music server on the host through pinet
     on a library of ffmpeg-made test media with an MP4, a MOV, an MKV and a WebM video; the scripts item 109 left).
     An album listed "1 Opening" and "Video: Opening (Official Video)"; Return on the video played
     `/vaudio/b29434fe` to its end ("the track's stream is in, 48901 bytes"); the artist Delta, with videos alone,
     opened on "Video: Webm Clip", which played its sound the same way; `play v:7dadadb6` from the server's devices
     interface was answered `audio_only` and the SE played that video's sound, then stopped on the server's stop.
     All four as designed; nothing to fix.
   - *1.6.* With the card there (`mac/piplay/vcard.c` opens ".A68Video" by name, as Aurora68 Video does, and checks
     the block) and a server that offers `mpeg1`, a video plays as itself: `/vmpeg/ID?w=&h=&rate=&start=` (built by
     the core: `a68_path_vmpeg`; the size of the video's own shape for the screen the player window is on,
     `a68_vmpeg_size`; the rate, `a68_vmpeg_rate`) over the radio's connection, the receive chain writing the
     answer's body into the card's ring at interrupt time. A video window (docs/09 "Music videos on the video decoder
     card") the size of the picture; the card told its place and whether it may draw (a window of the Play's over
     it, a menu down, a window dragged or sized, a dialog up, another program in front, the window hidden); seeking
     by a new request with `start=S`; the queue moving on when the card says "ended", never gaplessly into or out of
     a video; the announce's `video=1`. icy.c learnt `video/mpeg` (ICY_K_VIDEO), a catalogue row a video's size.
     Without the card, or with a server older than `/vmpeg/`, everything is 1.5's.
   - *Two faults on the way.* The first start of 1.6 in the testbed ended in "bad F-line instruction" at $72652073:
     the bytes "re s" of "videos are shown", a log line longer than its 100-byte buffer, over the return address
     (the remote control's state said so with the exception's PC). And a corner of a picture stayed on the desktop
     where the video window had been closed: HideWindow paints the uncovered desktop at once, with no update event,
     and the card drew one more picture before it was told to stop; the Play now tells the card before it hides,
     disposes or resizes the window.
   - *The network's rate, the card's numbers* (QEMU under four host cores, pinet's ring files; each stream's numbers
     are logged by the Play when it ends: `A68 Play: video: ...`). At the server's own rate (2,500 kbit/s of video
     asked at 320 x 240): a 40-second 480 x 270 film as 320 x 180 (6,392 KB, 1.28 Mbit/s for that test pattern) came
     in at 508 kbit/s on average and took 102 s; a 20-second Mandelbrot film at 320 x 240 (5,730 KB, 2.3 Mbit/s) at
     452-459 kbit/s, 102-103 s; the card held under 8 KB for most of the time it played (9.6 of 10.1 s of a sample),
     and still showed every picture (1000 of 1000, 500 of 500, none not drawn) and counted no time dry: with sound,
     the sound is the card's clock, and a stream that comes too slowly stretches the sound and the pictures together
     instead of running dry. A 64 KB TCP buffer and 16 KB receives (instead of 32 and 8) carried no more (455-555
     kbit/s), so the limit is the testbed's network, not the Play's receive chain; they went back. Hence the
     server's new `rate=` (150 to 2,500 kbit/s of video; without it the helper's arguments as before) and the Play's
     400 kbit/s at 320 x 240. With it the 40-second film, asked at 320 x 180 and 300 kbit/s (450 kbit/s with its
     sound), kept time: 28.8 s of it in about 31 s, and after a seek its last ten seconds in 11.2 s with under 8 KB in
     hand for 0.35 s of them (437 kbit/s), 250 of 250 pictures shown; played through in a later run, in 43 s (419-429
     kbit/s, 1000 of 1000 pictures); the busier Mandelbrot film at 320 x 240 and 400 kbit/s (570 kbit/s with its
     sound) still came a little slowly (7.1 s of it in 10 s). On a 1024 x 768 screen the same film came at 512
     x 288 and 768 kbit/s, more than the testbed carries again (406-458 kbit/s; 19.4 s of film in 40.5 s). Under QEMU the card decoded a 320 x 180 picture in 1.6-2.2 ms and a 320 x 240
     one in 4.4-6.4 ms, drew it in 0.5-2.2 ms at one bit, 17-77 per mille of core 1; 512 x 384 at 256 colours, 2.6-2.8
     ms to decode and 6.2-6.6 to draw, 112-114 per mille.
   - *Tested in the testbed* (screenshots in the scratchpad, `pv/log2/`, `pv/log5/`, `pv/log6/` and `pv/log7/`, not committed): a video
     row played in its window below the player window, dithered to one bit (`p2-playing.png`); pause by a click in
     the picture (the card paused at 12.5 s with 189 KB waiting) and resume; the Library brought over it, the picture
     not drawn over the Library (`p2-covered.png`), and back when uncovered; dragged by 50 x 40 pixels, the card's
     rectangle following (`p2-moved.png`); a menu held down (`p2-menu.png`); paused while covered and the last
     picture drawn again when uncovered (`p2-paused-front.png`); the end of the 40-second video and the album's
     second video after it; `play v:ID` from the server's devices interface (the SE's record with its 18th field 1,
     version 1.6, no `audio_only`) and an added song after it, which played when the video had ended; a seek from
     the server (`start=5`), the next request taken by the server's one encoder without a 503; a click at three
     quarters of the player window's progress line (`start=30`, `p5-seek.png`, Now Playing saying "Video 2 of 2 -
     MPEG-1 320x240" once the album's second video had followed); the window resized for a video of another shape;
     the screen at 1024 x 768 (`mac.screen=` and QEMU's VideoCore at that size), the picture 512 x 288 at one bit
     (`p6-playing.png`) and, after Aurora68 Depth 8, 512 x 384 in 256 colours (`p6-colour.png`); the close box (the
     picture hidden, the sound on; the next video opens the window again) and Controls > Show Video; after the fix above, the window closed three times while playing,
     nothing of the picture left on the desktop or the Library (`p7-closed1.png` to `p7-closed3.png`), and Stop
     taking the window away (`p7-stopped.png`).
     `make -C mac check`: play_core_test 205 checks on the host and 171 as a 68020 program, icy_test 94; the server's
     test 204 checks; GitHub's runner: XCTest 21 tests and the server's test (runs 45101492555 and 45101492625).
   - *Not verified:* the SE. On the SE board in MAME, item 110 found QuickTime's clock stalled over MAME's VIA; the
     card's stream channel has its own clock, but it was not tried there (one emulator at a time, and the lock was
     shared with the QuickTime MPEG-1 work). On the owner's SE: the network's rate over the Pi's Ethernet or WiFi
     (the log's "network ... kbit/s" and "under 8 KB in hand for ... ms of ... ms played" at the end of each video),
     whether 400 kbit/s keeps time and looks acceptable dithered on the 9-inch screen, the sound through HDMI or the
     jack, HDMI in colour with the window on that screen (larger pictures there: 512 x 384 below 1280 wide, 640 x
     480 above), and a long video's memory (the card's ring is 512 KB; the Play holds nothing of the video itself).
     In the testbed neither a connection lost in the middle of a video (the Play stops the card and asks again from
     the picture shown) nor a 503 from a busy encoder (asked again after a second) happened, so those two paths have
     not run; nor has the SE board's second screen (the window on HDMI beside the SE's own).

115. **Music videos linked to songs; albums in track order; Aurora68 Play 1.4 for macOS (23 November).** The owner
     tried the macOS Play 1.3 against the music server on the NAS and asked: "we need this in the artist page or
     something to make it easy to see videos - or even link them to a song to alternate". Looking at it they also
     found the album "Discovery" (Daft Punk) listed by title (Aerodynamic 2, Crescendolls 5, Digital Love 3, Face to
     Face 13...) with the right numbers beside the titles, and another album the same way (9, 25, 17, 5...).
   - *The order.* Those albums came from a downloader that names files by title alone ("Aerodynamic.flac"; the number
     only in the tags). The macOS Play shows the tracks in the order the server sends them, so the server was at fault.
     `/mac/tracks` sorted a page's rows with `rows.sort(key=lambda r: (r[1] or 999, ...) if any(x[1] for x in rows)
     else 0)`: the key looks at `rows` while `rows` is being sorted, and CPython empties a list for the length of its
     sort (any look inside it sees nothing), so `any(...)` was always false, every key 0, and the stable sort left the
     files' order, by name. With names that begin "01." the files' order is the tracks', which is why it never showed
     before. Two smaller faults beside it: the sort came after the paging (an album of more than one page would have
     been in order page by page), and disc numbers were not read. `/api/tracks` sorted correctly (its key did not look
     at the list) but by number alone. Now `Catalogue.ordered` puts each album's tracks in disc and track order (the
     disc and track tags, read with the album's first listing as before, else the numbers in the names; an album none
     of whose tracks has a number keeps its files' order) before any paging, for `/mac/tracks` in every form (the
     classic Play's too) and `/api/tracks`.
   - *Links.* A video is a song's when the two have the same artist (the video's own, from where it sits, or the one in
     front of its title; compared by the first name, so "Daft Punk, Julian Casablancas" is Daft Punk) and the same
     title from the files' names, NFKC, casefolded, punctuation as spaces, with the tags in brackets or after " - "
     left out when they only say what kind of file it is (official/music/lyric video, visualizer, audio, HD, 4K,
     1080p, remastered and a year, explicit, M/V, featured artists) and kept when they name a version (Live, Remix,
     Radio Edit, Acoustic Version). Three tries, the first that finds anything wins: whole against whole, the video's
     plain title against a song's whole, plain against plain; so a Live video goes to the Live song, else to the studio
     one, and "One More Time (Official Video)" to "One More Time (Radio Edit)" only when there is no plain "One More
     Time". A video may be of several songs (an album and its deluxe edition) and is linked to all; a song keeps one,
     its best. Names only, never tags: the links come with the catalogue at each scan (`video_links`, kept while the
     folder's signature is the same), cost no ffprobe, and do not change with what has been probed. Exposed: in
     `/api/tracks` (`"video"` on a song, `"tracks"` on each video, also in `/api/videos`), as a tenth field of a
     song's `T` record and an eighth of an album's record in `/mac/` with `full=1&videos=1` only (the macOS Play sends
     both; the classic Play neither, so its records are byte for byte what they were, which the server's test checks
     against a server with `VIDEOS=off`), in `/api/list` (`"file_videos"`, for the panel), and in the lists: an
     album's videos are its own and its songs' (from the artist's folder too), in its songs' order, each once; an
     artist's those attached to it and its songs'.
   - *The core.* `a68_track.video` (the tenth field, "" when empty or absent) and `a68_album.videos` (the eighth, 0);
     an older server's records read as none.
   - *The macOS Play 1.4.* Songs with a video have a film button (the video plays, the album's videos after it, as a
     poster's click does) and Play Video in their menu; a double click still plays the song. "Play Videos Instead of
     Songs" (Settings and the Controls menu, `@AppStorage`) replaces each song by its video, one for one, in whatever is
     played here or sent to another Mac (an album or artist played whole is then sent as its tracks rather than
     `f:FOLDER`); Live keeps the songs, since the SE hears it. The bar says "Music video". An artist's page starts with
     its videos as a row of posters that scrolls sideways, with See All for the grid; artists and album tiles with
     videos have a film mark; Videos in the sidebar lists every video by artist (`/mac/videos`), filtered by the search
     field. The web panel marks songs with a video (a film frame that opens it as the poster does).
   - *Tests.* The server's test builds its own library (`links_check`): six FLACs of *Discovery* named by title with
     their numbers in the tags, read in pages of four in both forms and as JSON; two discs in one folder; an official
     video and a Live video in the artist's folder, an "(Official Music Video) [HD]" and a "(Visualizer)" in its
     Videos folder, a deluxe edition and a live album, another artist's video of the same title; the keys of a dozen
     titles; 218 checks (0 failed), here and on GitHub's runner (run 45121946040). `make -C mac check`: play_core_test 212 checks on
     the host and 175 as a 68020 program (the new fields present, empty and absent, and the real server's answers:
     "Closing Time (Video)" is Closing Time's). GitHub's runner: XCTest 24 tests (21 before) and the app built, run 45121946107.
   - *Not verified:* the app's screens. Swift is compiled and tested only by the workflow, so the new pages, the film
     buttons, the row of posters, the toggle in the Controls menu and in Settings have been compiled, not seen; nor
     have the links been tried on the owner's library (which names its videos however its sources do: a name the
     rules above do not foresee stays unlinked, and is still listed with its artist and album as before).

116. **Aurora68 Video on the real SE: "no MPEG-1 video found at the start of the stream" (23 November).** The owner's
   first try on the SE with the test14 image: Aurora68 Video opened *Colour Bars SE.mpg* and at once said "no MPEG-1
   video found at the start of the stream", with nothing drawn. The film on the image was byte for byte the one made
   with FFmpeg, and pl_mpeg on the host found its headers in the first 4 KB, so the card never got the bytes.
   - *Cause.* Aurora68 Video reads a film straight into the card's ring (a PBRead whose buffer is the ring's address,
     item 112). On the SE the System disk is the card's image, served by the PiDisk driver, whose reads the mailbox
     copies on the Pi side through `mac_page()` (psmac_mbox.c). That function served Mac RAM, the slot-9 window and the
     screens, not the video card's window at $FC000000 (or its 24-bit form $FCC00000): every read into the ring failed
     with a bad address, the program took the error for the end of the file and sent end of stream (Control 114) with
     no byte delivered, and the card's probe saw a finished stream without headers. The solo testbed of items 112-114
     had played films because its disk is the emulated SCSI drive (`mac.scsi0=`), whose bytes the 68k moves itself.
   - *Fix.* `psmac_mmu_video_phys()` (psmac_mmu.c) gives the card window's Pi address for either form, and `mac_page()`
     serves it, so disk (and USB stick) reads may land in the ring.
   - *Test.* The solo board with the test14 image on SCSI and a 4 MB PiDisk volume (`mac.disk0=`) holding the film,
     opened in Aurora68 Video through the Agent: with the previous kernel the same message as on the SE; with the
     fix, "stream 1: MPEG-1 512x342 at 25.00 frames a second" and "played to the end, 9.9 s: 250 pictures decoded,
     250 shown, 0 not drawn, 0 times dry".

117. **The owner's music videos: "Albums/videos", catalogue numbers (23 November).** The owner's downloader keeps every
   video in one folder beside the artist folders, `Albums/videos/The Warning - Bite My Tongue [555225235].mp4`, some
   twice under two numbers. The macOS Play 1.4 listed them under one artist and linked none to its song. Copied on the
   host (the same layout, tones and FFmpeg's test picture): the folder `Albums/videos` is no album's and no artist's, so
   the rule of item 109 made its name's folder ("Albums") the artist of every video; and the number stayed in the title
   ("Bite My Tongue [555225235]"), which matched no song and made two downloads of one video two videos.
   - *Fix* (aurora68_music_server.py): `video_stem()` leaves out a catalogue number at the end of a video's name (five or
     more digits in brackets, or an 11-character id with a digit, '_' or '-': "(Live)", "[HD]", "(2019)" and
     "(Instrumental)" stay); in a folder that is no album's or artist's the file's own "Artist - " comes before the
     folder's name; copies of a video are compared without the number.
   - *Test:* `downloads_check` in test_server.py (the owner's layout with an unfinished `tmp...` folder): the artist from
     the name, the number gone, one of the two downloads kept, each linked to its song; 219 checks, 0 failed.
118. **Setup and Recovery as an access point: set up with nothing but a phone (23 November).** The owner: "for the
   early startup option, is there a way we can trigger the Pi going into an AP mode so we can connect to it for initial
   setup or later rescue. Have it broadcast its own SSID which we can connect to with instructions on the screen", and
   "This would not be the default but would need to use the programmer's buttons." Item 106 had left the access point
   out: Alice68's WiFi code drives the firmware as a station only, the IP stack had no DHCP server, and the testbed's
   simulated radio had no access point to try anything against.
   - *The gesture.* On the recovery screen, **the interrupt button held for two seconds**, after it has been seen let
     go for a second. Recovery already reads the button: phase 2 looks at the wire's level (bit 2) as the 68k would
     be released, and the service loop's poll keeps that level fresh while no Mac runs, so the recovery screen can
     read it too (`switch_poll`, psmac_recovery.c). The alternatives were weighed: "the button pressed twice to
     enter" would have to be timed at power-on, before any screen is up to say what is happening; a single press on
     the recovery screen is one glitch on /IPL2 away from happening by itself. Two seconds held is far longer than a
     glitch (milliseconds) or the press that gives a running Mac its NMI, it means nothing else on this screen, the
     screen counts it down ("Keep holding the interrupt button: the access point starts in 1 s"), and because the
     button must first be let go, a finger still on it from switching on (the switch's own way into recovery) does
     not start it. The recovery screen says it on the boards that have the button: "Hold the interrupt button 2 s: a
     WiFi network of the Pi's own for a phone". The same is in the keyboard menu (an eleventh item, "Start an access
     point"), and `recovery.ap=on` in cmdline.txt makes a recovery start that goes straight to it, for the bench.
     Nothing starts it otherwise: not the boot counter, not a missing WiFi setting. The kernel's log line for the
     button says "pressed (Setup and Recovery: no Mac runs ...)" in recovery instead of "an NMI for the Mac".
   - *The firmware's part* (`alice_net.c`, under `#ifdef PSMAC`; `src/alice/wifi/fmac_ap.c` and `.h`, new, Aurora68's
     own and GPL-2.0-only like the code around it). A command for unit 0, `AN_CMD_AP` (13) with `struct an_ap` (name,
     passphrase, country, channel, flags), added to alice_net.h without moving an offset (`make -C mac check`). The
     firmware's SoftAP is set up in the order of Linux brcmfmac's `brcmf_cfg80211_start_ap` for one interface with
     the firmware as authenticator, read from cfg80211.c and written here from that reading, built as a list of steps
     (`fm_ap_steps`) that the driver sends, the testbed's radio checks and the host test reads: the event mask with an
     access point's events (a station associated, left); `sup_wpa` 0, `mpc` 0 (minimum power consumption would switch
     the radio off with no association of its own), `apsta` 0; `WLC_DOWN`; the country; `WLC_SET_BCNPRD` 100,
     `WLC_SET_DTIMPRD` 2; `WLC_SET_INFRA` 1; `WLC_SET_AP` 1; `chanspec` (0x1006 for channel 6 in the 802.11ac encoding,
     0x2b06 in the older 802.11n one, chosen by `WLC_GET_VERSION` as Linux does); `WLC_UP`; for WPA2
     `WLC_SET_WSEC_PMK` with the 32-byte key (PBKDF2 of the passphrase and the SSID, `key_len` 32, `flags` 0, as
     `brcmf_set_pmk`), `auth` 0, `wsec` AES, `wpa_auth` WPA2_AUTH_PSK, `mfp` 0; for an open network `wsec` 0 and
     `wpa_auth` 0; `WLC_SET_SSID` with join parameters that hold only the SSID; `closednet` 0. Two fall-backs Linux does
     not need but a firmware nobody here has seen might: `WLC_SET_CHANNEL` with the plain number when `chanspec` is
     refused, and the key as a passphrase (`flags` 1) when the binary key is. Every step is logged (`[WIFI] access
     point: WLC_SET_AP 1 ok (the access point role)`; the key's bytes never). Refusals of the event mask, `sup_wpa`, `mpc`, `apsta`,
     the country, the beacon and DTIM periods, `auth`, `mfp` and `closednet` are logged and passed over, as Linux
     passes over most of them; the first refusal of any other step stops the sequence and becomes the unit's message,
     which the recovery screen shows: "the WiFi chip refused AP mode: WLC_SET_AP error -23" (an answer that never came
     reads "no answer from the chip"). After `WLC_SET_SSID` the driver waits up to three seconds for `E_LINK` or
     `E_SET_SSID`'s status and logs which came, or that neither did; the first sixty events of an access point are
     logged in full (type, status, reason, flags; no addresses: they are the guests'), so the first try on a real chip
     shows what it sends. Then the unit is "connected" with the access point's own name, channel and security and the
     link up, and associations and departures change its message ("1 device joined"). `JOIN` and `DISCONNECT` end the
     access point (down, `WLC_SET_AP` 0, infrastructure, up); a chip that is lost and started again (the driver's five
     retries) starts the access point again.
   - *WPA2 or open.* With WPA2 the 4-way handshake runs in the firmware (Linux offers its access point offload,
     `NL80211_EXT_FEATURE_4WAY_HANDSHAKE_AP_PSK`, only with `BRCMF_FEAT_FWAUTH`, which it reads as `idauth` in the
     firmware's `cap` list). The driver reads the same list, already logged at every start ("firmware features: ..."):
     with `idauth`, WPA2-PSK with AES; without it, an open network, said on the screen ("no password: this WiFi
     firmware cannot do WPA2 as an access point; the PIN guards the page") and in the log, and every request but the
     page still needs the PIN. Doing the handshake in the kernel instead (an authenticator of its own: the PTK, the
     MIC, AES key wrap for the group key, the keys installed with `wsec_key`) was considered and left for a change of
     its own, if a real Pi 3's firmware turns out to lack `idauth`. `recovery.apsec=open` asks for an open network,
     `recovery.apsec=wpa2` for WPA2 even without `idauth` (an experiment: phones would associate and wait for a
     handshake that never comes). The password is ten characters from the 31 that cannot be misread on a one-bit
     screen (no 0/o, 1/l/i), taken from SHA-1 of sixteen words of the BCM2835's random generator and the counters it
     is mixed with (about 50 bits, for a network that lives for one start, beside the PIN); it is drawn on the
     screens only, never logged, never served by the page or the state.
   - *The network* (`psmac_ip.c`; `psmac_apnet.c` and `.h`, new, portable C). `psmac_ip_ap` gives the WiFi the fixed
     address 192.168.4.1/24, uses the WiFi alone (the page is served on the access point; a cable in the wired port
     is left alone meanwhile), runs no DHCP client, and serves the guests: a DHCP server with sixteen leases,
     192.168.4.10 to .25, ten minutes, keyed by the hardware address (a phone that comes back gets its address
     again), the Pi as router and DNS server, a NAK at once for a request of another network's address (a phone last
     on a home network asks for that first), RELEASE, DECLINE and INFORM; and a DNS answerer that gives every name's A
     record as 192.168.4.1 for ten seconds and answers AAAA and the rest with no records, from the address the query
     was sent to (a phone set to its own DNS server sends it here through the "router"). Phones check a new network
     by fetching a known page (`generate_204`, `hotspot-detect.html`); with every name leading to the Pi that fetch
     reaches the recovery page's server, which answers any GET outside `/r/` with a 302 to `http://192.168.4.1/`, the
     captive portal that phones then offer as "Sign in to network". Replies still go back to the Ethernet address the
     request came from, so nothing changed for ARP. The heartbeat's `[REMOTE]` lines gain the access point's counts.
     When the access point does not start, `psmac_ip_ap_off` puts the links back and the saved WiFi (cmdline.txt's,
     or this start's options) is joined again, so a refusal costs nothing but the attempt.
   - *The screens.* A page of its own for the access point, written for 64 columns and 21 rows: "On a phone or
     laptop, join the WiFi network", the name in large letters, "with the password (WPA2)", the password in large
     letters, "then open http://192.168.4.1/ and give the PIN", the PIN, the devices with an address, the last action,
     "To leave: Restart on the page, or press the reset button." The name is `Aurora68-` and the radio's MAC address'
     last two bytes in hex (the testbed's simulated radio, a locally administered address, gives `Aurora68-4946`).
     While it starts: "Starting the access point ...", how it was asked for and the PIN; when it failed, the normal
     recovery page with "The access point did not start: <the chip's refusal>". The screen's lines were cut at 80
     characters before the 64-column wrap of item 106 ever saw them, which cut the end off a long refusal; they now
     hold two screen lines' worth.
   - *The page* (tools/recovery/page.html, psmac_recovery_page.h made by mkpage.py). The state has an `ap` block
     (state, name, security, channel, address, the guests with a lease, how it was asked for, the error; never the
     password); the Overview shows it as a card; the WiFi tab, while the access point runs, hides "Test join now" (a
     test join would end the network the phone is on; the API answers it with 409) and offers **Save and restart to
     join this network**, which saves the settings and restarts normally. "Restart normally" takes `recovery.ap` out
     of cmdline.txt as it does `mac.recovery=1`.
   - *Leaving.* Restart on the page, or the programmer's **reset button**: on the SE the PiStorm sees the 68000's
     reset line (`psmac_gpio_reset_asserted`), which until now nothing used in a running start; in a recovery start a
     press (the line low for a tenth of a second, armed once it has been free for three seconds, long after the
     Mac's own power-on reset) restarts the Pi by the watchdog. The access point is never kept: nothing of it is
     written to the card, and the next start is as cmdline.txt says.
   - *A bug found on the way.* Timestamps kept as `t | 1`, so that 0 could mean "none", are one more than `t` when `t`
     is even, and `now - x` in the same millisecond is then 4294967295: the first gesture test armed the button at
     once and took the power-on hold of the testbed's switch for the gesture, the very case the arming is there to
     refuse. They are `t ? t : 1` now, and so is item 106's Esc watch, which had the same flaw: an Esc whose poll came
     in the millisecond it was pressed would have counted as held for a second.
   - *Tested on the host* (`tools/apnet_test.sh`, run by `make -C mac check`: 66 checks): DHCP DISCOVER, OFFER (to the
     client's own hardware address, or broadcast when it asks), REQUEST and ACK, the same address for a client that
     comes back, a second client, a REQUEST naming another server (no answer, the offer freed), another network's
     address and another client's (NAK, broadcast), INIT-REBOOT into a free address after the Pi restarted, renewal
     to the client's own address, RELEASE, INFORM, sixteen leases and no seventeenth, an unused offer free again after
     a minute, malformed messages ignored; DNS A (one record, id kept, authoritative, TTL 10), AAAA (none), ANY,
     answers, two questions, a compressed name and a cut query refused; the WPA key against IEEE 802.11i's test vector
     ("password", "IEEE"); the WPA2 and the open sequences step by step against Linux's order with the values that
     matter (WLC_SET_AP 1, chanspec 0x1006 or 0x2b06, wsec AES, wpa_auth 0x80, the PMK's 132 bytes, WLC_SET_SSID's 52
     with a zero BSSID); a refused WLC_SET_AP stopping with "WLC_SET_AP error -23", a refused chanspec falling back to
     WLC_SET_CHANNEL, a refused binary key to the passphrase, a refused mpc passed over; `idauth` as a whole word only.
   - *Tested in the testbed* (QEMU solo board, a 128 MB FAT32 card image, the simulated radio extended for
     `AN_CMD_AP`: it runs the very step list against a model of the firmware, the role and infrastructure only while
     down, the key only while up as an access point, the SSID last and with the key when WPA2 is asked for, answering
     with Broadcom's error numbers; `tools/pinet.py` with `PINET_APCLIENT=1` as a phone that joined: DHCP client, DNS
     queries, then the page through `PINET_KERNEL`). `mac.recovery=1 mac.switchsim=ap` (the testbed's switch: held at
     power-on, let go, held again 12 s later) with the radio joined to its network: the screen's hint, "Keep holding
     the interrupt button: the access point starts in 1 s" at the hold, the access point two seconds later, all
     nineteen steps accepted, up 1.5 s later; the phone was offered and given 192.168.4.10 with router and DNS
     192.168.4.1 and a 600 s lease; `connectivitycheck.gstatic.com` and `aurora68.local` answered 192.168.4.1,
     `captive.apple.com` AAAA no records; the page over the access point byte for byte tools/recovery/page.html
     (39,849 bytes); `/generate_204` and `/hotspot-detect.html` 302 to `http://192.168.4.1/`; 401 without the PIN; a
     test join 409; a scan in access point mode listed the four networks and the access point stayed up; a save wrote
     `wifi.ssid` and `wifi.psk` beside the line's other words with cmdline.bak; "restart normally" restarted the Pi by
     the watchdog. `recovery.ap=on mac.wifisimap=noidauth`: an open network, said on both screens. `recovery.ap=on
     mac.wifisimap=refuse:WLC_SET_AP`: "the WiFi chip refused AP mode: WLC_SET_AP error -23" on the screens, the links
     back, the radio joined its network again and DHCP gave the kernel its usual address there. The keyboard menu on
     QEMU's USB keyboard (Return, Up to the last item, Return) started it; `mac.resetsim=40`, the testbed's reset
     button, restarted the Pi ("the reset button: the Pi restarts (the access point is not kept)"). The page's script
     was run under Node against a stub of the page and the state recorded in the first run: the Overview's card, the
     WiFi tab's buttons in and out of access point mode, save then restart in that order. Headless Chrome would not
     start in this environment (no GPU, no D-Bus), so the page itself was not photographed. Screenshots, from the
     testbed: `docs/img/recovery-ap-se.png` (the compact Mac's screen, photographed through `/r/screen.pbm` with the
     testbed's `mac.recoveryscreen=1`), `recovery-ap-hdmi.png`, `recovery-ap-hold-hdmi.png` (the countdown),
     `recovery-ap-open-se.png`, `recovery-ap-refused-se.png`.
   - *Not verified until the owner tries a real Pi.* Everything the firmware does: whether the Pi 3B's BCM43430
     firmware takes this sequence (each step's answer is in the log), which of `E_LINK` and `E_SET_SSID` it sends,
     whether its `cap` list has `idauth`, and so whether the network is WPA2 or open; whether a phone joins it and
     finishes WPA2's handshake; the channel's encoding (`WLC_GET_VERSION`'s value is logged); a real phone's sign-in
     page (Android, iOS) and its DNS behaviour; the interrupt button's level on the recovery screen with the SE's real
     wire (the service loop's poll is what keeps it fresh; the testbed's switch is simulated); the reset button's line
     reaching the Pi in a recovery start (if it does not, power off and on: the access point is not kept either way).
     If the screen says "the WiFi chip refused AP mode: <step> error <n>", that line and the `[WIFI] access point:`
     lines of the log are what is needed to fix it.

119. **The access point on the owner's Pi: "Done" after the PIN (23 November).** The first try on real hardware worked:
   the Pi 3B's chip took the access point's sequence (open, as its firmware lacks `idauth`), a Mac joined
   `Aurora68-...`, macOS opened the page in its "Join Network" sheet, and the PIN was accepted. But the sheet kept
   its **Cancel** button, and Cancel leaves the network. A Mac, an iPhone or an Android phone calls a network signed
   in when its own check page (`hotspot-detect.html`, `generate_204`, `connecttest.txt` ...) answers as on the internet,
   and every check was still redirected to the page.
   - *Fix.* A device that has given the right PIN (any `/r/` request with it, in AP mode) is remembered by its address
     (16 at most, forgotten at restart); its checks then get the answers they expect (Apple's "Success" page, an empty
     204, Microsoft's and Firefox's and NetworkManager's texts), others' are still sent to the page. The page reloads
     itself once after the first state answer in AP mode (sessionStorage keeps the PIN), which makes the sheet check
     again: its button becomes **Done**, the device stays joined and the home page (Overview) shows.
   - *Not verified here:* the emulator was busy with another run; the owner's Mac is the test.
   - *The owner's second try:* Done came, but pressing it showed the home page in the sheet for a moment, and the setup
     belongs in the browser. Now the checks' redirect goes to `/?signin=1`, which the page takes for the sign-in sheet: it
     asks for the PIN, signs the device in, goes to `/?signin=2` (the sheet checks again: Done) and shows only a short
     "Connected" card ("press Done, then open http://192.168.4.1/ in your browser"), never the setup tabs. A browser opened
     at `http://192.168.4.1/` gets the whole page as before.
   - *Third try:* the owner wanted Done to open the page in the browser without the PIN again. Done is the system's
     button and runs no page code, and the sheet's and the browser's storage are separate. So the Connected card's main
     button opens `http://192.168.4.1/#pin=NNNN` in a new window (the sheet hands such links to the browser on a Mac),
     which the page reads into its own storage and drops from the address; then Done closes the sheet. Typing the
     address by hand still asks for the PIN: trusting a device by its address alone would open the setup to anyone on
     an open access point who borrows that address.

120. **The classic Aurora68 Play 1.7: parity with the macOS Play 1.5, and the music server found by itself (23
   November).** The owner: "lets get the classic mac version up to parity. Also have it automatically discover server
   hosts". docs/09 "Aurora68 Play: classic and macOS" has the table: every feature of the macOS Play 1.5's README, and
   for each whether the classic Play had it, has it now or leaves it out, and why.
   - *What it had.* More than its menus suggested: the library, covers, Find and typing to select, a gapless queue,
     Shuffle and Repeat, seeking, Listen Live, the radio, new albums without a restart, a device that others send to and
     that shows videos (1.6), Connect By, and the server's own order of an album's tracks (it never sorted them, so
     item 115's fix reached it unchanged).
   - *Finding the server.* `discover_start` and `discover_poll` (fetch.c) existed, but the probe ran only when the
     Library window was shown with no server remembered, and stopped at the first answer. Now it runs at the start when
     no server is remembered (the window shown or not), and when the remembered one does not answer (after the other
     addresses its last answer named); the address stays in the preferences meanwhile. Every answer within a second of
     the first is kept, each server (the address it answered from and its port) once; the one chosen before is taken
     by its name (kept in the preferences' new tail, so a new address does not matter), or the only one; with several
     and none chosen, the Library window names them and Library > Servers lists them. The old `discover_stop` waited
     for an outstanding two-second read with the event loop held (up to four seconds); the read now times out after a
     second and the stream is released only when no read is out.
   - *Links, in Mac Roman.* The links of item 115 come only with `full=1&videos=1`. The core reads every record by its
     fields, and `full=1` changes nothing else of the classic Play's reading but the length of the records (paths and
     folders), so the classic Play now asks an artist's albums and an album's tracks with both, still in Mac Roman: a
     row keeps the song's video (`a68_row.video`, new) and the album's count (`a68_row.videos`, now set for albums too).
     An older server answers `full=1` with its longer records without the fields, read as no links; a server older than
     `full=1` passes over the parameter. A list that no longer fits the 48 KB answer buffer with the paths (a box set
     of some 200 songs) is asked again without them (`fetch.too_long`). The other requests (artists, a search, the
     device's) are as before. Two views of videos alone joined the core: `A68_V_VIDEOS` (`/mac/videos`, every video, the
     server's order by artist) and `A68_V_ARTVIDEOS` (an artist's).
   - *Play Videos Instead of Songs* replaces a song by its video when the queue is made (`row_item`, `to_video`), from
     the list's own `V` rows (an album lists its songs' videos, item 115) or, for a queue fetched for an artist or for
     Play Next and Add to Queue, from the `V` records of that answer; one for one, so the place chosen is the place
     played. Only with the card and a server that makes MPEG-1: otherwise the songs play.
   - *The video window's sizes.* The card scales a picture to the rectangle it is given (nearest pixel), so a larger
     window costs the network nothing: Normal Size, Double Size, Fill Screen (Command-1, 2, 3) and the zoom box
     (window kind 12, no grow box) toggling Normal and Fill Screen, the size kept for the next video.
   - *The menus on the SE's screen.* The Library menu would have had 22 lines (352 pixels of 342): Previous and Next
     went to the Controls menu, Connect to Server... into the new Servers submenu; it has 19 (304 pixels).
   - *The preferences' tail.* An older Play reads `sizeof` its record and checks the count it got, so the record could
     not grow; the tail (the server's name, Play Videos Instead of Songs, the window's size, marked `PL17`) follows the
     1.3 record in the same version-3 file: 1.6 reads the file as before, and 1.7 reads a 1.6 file without a tail.
   - *Left out, with reasons in the table:* "Play on" another player (the SE is the one others send to), posters (a
     picture fetched over MacTCP for each, little to see at 32 pixels and one bit), filtering at each keystroke, Live's
     queue and listeners, Control Center and media keys.
   - *Tests.* `make -C mac check`: play_core_test 221 checks on the host (9 new: a song's video in its row present, empty
     and absent, an album's count, every video and an artist's as rows and their requests, and on the real server's
     answers in Mac Roman with `full=1&videos=1`: "Closing Time" names its video, which is among the album's rows) and 179
     as a 68020 program (175 before); the server's test unchanged and green (219); GitHub's runner: the macOS Play built and its XCTest
     green with the changed core (run 45128624449), the server's workflow too (45128624433). In the testbed (the solo
     board, System 7.1 with Riptide, a copy of the test14 image, the k46 kernel; two servers on the host, "Test Library"
     and "Second Server", their TCP ports forwarded by pinet and the probe relayed to both: `PINET_UDP` now takes several
     destinations, `8070=127.0.0.1:18570+127.0.0.1:18571`): the four discovery cases of docs/09 (no preferences with one
     server and with two, Search Again with two, a remembered server stopped), the links, Play Video, Play Videos Instead
     of Songs, Videos, Up Next, Play Next, Add to Queue, Repeat One, the three sizes and the zoom box on 640 x 480 and on
     512 x 342 (`mac.screen=512x342` with `psmac-512x342.declrom`; the 512 x 342 declaration ROM alone left the solo
     board's screen at 640 x 480), and the server from before item 115. Riptide refused both broadcasts (error -23033) and
     the multicast probe was answered, as in item 107. Screenshots: `docs/img/system71-play17-two-servers.png`,
     `system71-512x342-play17-film-marks.png`, `-servers.png`, `-fill-screen.png`; the rest stay in the scratchpad.
   - *Found in the testbed and fixed:* a song put in with Play Next or Add to Queue after the last song's stream had come
     whole was never played (radio.c asks for the next stream only while one is coming; at the queue's end lib_poll moved
     on only into a video): what follows now starts as a stream of its own. The Library window took a place kept from a
     640 x 480 screen on the 512 x 342 one by its left end alone (1.2's rule; the player window's checks both ends): it
     now checks both ends and the height. The card drew the video over Standard File's dialog (1.6): File > Open... holds
     the picture now (not tried).
   - *Seen once, not explained:* in the first session the Mac stopped 4 seconds into the second video of a queue, with no
     interrupt taken any more, the 68k in the ROM's level-2 handler ($40806EAA) and in system-heap code at $9AC98 (the
     network's interrupt path), and the kernel logging "slot-9 interrupt not serviced in 500 ms: network board flag
     cleared". The Play's code had not changed there; after a restart nine more video streams, five of them following a
     video, ran without it. Worth watching for on the SE.
   - *Not verified:* the SE (and MAME's SE board); Apple's MacTCP, which sends the broadcasts; servers on two machines; the
     fallback without the links for a very long album; the Standard File hold.

121. **Videos whose folder names nobody (23 November).** On the SE, the classic Play 1.7's Videos list showed a group with
   no artist: The Warning's and Daft Punk's videos under "_", as the macOS Play had shown earlier ("_ — Bite My
   Tongue"). A downloader writes "_" where it has no artist or album, as a folder (`Albums/_/Kerosene.mp4`) or in front
   of a name (`_ - Kerosene.mp4`), and the server took that for the artist.
   - *Fix* (aurora68_music_server.py): `no_name()` (no letter or digit, or "Unknown Artist") marks such a folder or
     "Artist - " as nobody's. Such a video's artist is then the file's own artist tag (ffprobe now reads the format's
     artist and title tags with the rest; the cache's older entries are probed once more), else the one artist who has a
     song of that title, else the library's name; "_ - " is taken off the title.
   - *Test:* `downloads_check` with `Albums/_/A Tagged Clip.mp4` (artist tag) and `Albums/_/Bloodsport (Live) [...].mp4`
     (The Warning's, by the song): 219 checks, 0 failed.

122. **Setup and Recovery: every option, a Diagnostics tab, one-shot debug starts and a report bundle (23 November).**
   The owner: "do we have all the debug options in there for Mac, Mac Plus etc. Can we trigger a debug run directly in
   there for this and other platforms? With a report in the Web GUI?" The Options tab knew sixteen options: a list in
   the page's script, a second list of allowed values in psmac_recovery.c, and a third (the page's OB map) of the boards
   that use seven of them. The kernel reads more than a hundred (`psmac_option`, `find_token` and `psmac_bootarg_val`
   across src/psmac and src/alice), and the bus check of item 108 could only be had by editing cmdline.txt.
   - *One table* (`psmac_diag.c` and `.h`, new, portable C99): every option a user or a tester sets, 86 of them, each
     with its group (Machine, Display, Sound, Storage, Network, Video card, Debug), the boards that use it (se, plus,
     512ke, 512k, 128k, solo, and the testbed's IIci when no mac.board is given), its values and a line of plain
     English. Values are alternatives of words or small patterns (`#` digits, `%` hex digits, `*` any text, the rest
     literal: `#x#` a screen size, `%,#|%` mac.peek's address and length, `#|#,*` mac.setrace's count and chip), so the
     validator (`pdg_value_ok`) accepts exactly what the page shows, and its refusal names them ("mac.fpu: fast is not
     one of exact|on|double|none"). The page no longer carries a list: `GET /r/options` sends the table (`defs`,
     `groups`) with the line's values and the options set that the board does not use, and the page draws the groups
     from it, a menu where an option takes words, a field with an example where it takes a number or a name, a write-only
     field for the secrets; choosing the board hides the options it does not use, as the OB map did, now for all of
     them. Per board on the Options tab (the WiFi tab's three left out): the SE 71 (Machine 11, Display 8, Sound 6,
     Storage 10, Network 18, Video card 2, Debug 16), the Plus 76 (15, 8, 6, 8, 18, 2, 19), the 512Ke, 512K and 128K 73
     each (14, 8, 6, 7, 18, 2, 18), the solo board 59 (11, 5, 4, 8, 18, 2, 11), the IIci 58. The SE has its SCSI options
     and the speaker's, the Plus family its mouse, keyboard and ADB options, all five the bus check, the JTAG switch and
     the screen check; the solo board has its own traces and neither. The testbed's options (mac.remotefile, mac.wifisim,
     mac.switchsim, mac.hdmisim, mac.resetsim, mac.recoverypin, mac.recoveryscreen, mac.edidtest, mac.cardearly,
     mac.busfault and the rest: README's table marks each "testbed only", and the rows the table lacked were added) are
     not offered; an edit that names one is written as it is, like any word the page does not know.
   - *Run now* (the Diagnostics tab, between Options and Display; its swatch the bus check's black-and-white page).
     The **bus check** (item 108) runs on the service core, which owns the transport: core 3 asks
     (`psmac_buscheck_request`), the service core's loop runs `psmac_buscheck_run` at its next turn, and each test's
     name (a new `stage` callback in `struct bc_bus`, called by `bc_run` before each of its seven tests) and then each
     line of the report go into a list core 3 reads with acquire, so `GET /r/diag?from=N` gives the page the lines after
     N as they come. The check draws its page on the compact Mac's screen as before; recovery stops drawing there from
     the check's start, then marks the same page (`psmac_buscheck_render`, the check's drawing into a caller's buffer)
     through `psmac_se_mark`, so the row sender and the check agree, and puts the lines on HDMI with
     `psmac_console_page` and a last line with the PIN. The report holds both screens, drawn again every ten seconds,
     until "Give the screens back to Setup and Recovery" (`POST /r/diag/screen`) or any key. Recovery writes
     `buscheck.txt` itself (`psmac_card_rec_put`, the whole text): in a recovery start the card writer's queue of small
     files is never served, and `psmac_card_put_file` would have left `psmac_card_idle` false, holding every restart for
     ten seconds. Where it does not run, the check's own refusal is the answer (422) and the row says it: the solo board
     "has no Macintosh logic board behind the Pi", the bridge "is the testbed's bridge, not a PiStorm", with a "Run over
     the bridge (testbed)" button (`force=1`) that only the bridge ever shows. In a recovery start `mac.buscheck=1` no
     longer holds the board at the start (`psmac_buscheck_boot` says the tab runs it), and `psmac_buscheck_will_hold`
     is false in recovery. The RAM size probe is the check's own address test ("the board has 512 KB, not the 4096 KB
     assumed"). The **card read test** runs here on core 3, a 32 KB read per turn of the largest file on the card (16 MB
     at most), timing only the reads: speed, the first and the slowest read, read errors and a verdict (GOOD from 4 MB/s,
     SLOW from 1 MB/s with the time a 20 MB disk image takes to load, VERY SLOW below, FAIL on a read error), written to
     `cardcheck.txt`. The **CPLD's usercode** is shown as read at the start (item 108's JTAG read) and read again on the
     service core (`psmac_gpio_jtag`, the same routine, between its turns), only over the PiStorm's GPIO and not with
     `mac.jtag=off`. The EDID stays on the Display tab, linked from the list.
   - *Run on the next start: a one-shot debug start.* The tab ticks debug options (those marked `PDG_F_ONCE`, for the
     board: mac.logsave, mac.synclog, mac.audiotest, mac.screencheck, mac.console, mac.buscheck, mac.vframelog, the
     SCSI, chip, ADB, keyboard and bus traces, mac.buswait, mac.peek, mac.pcwatch, mac.pctrail, mac.watchtraps; ten on
     the solo board, fifteen on the SE, seventeen on the Plus) and writes them as one line to `debug-once.txt` in the
     card's root (`POST /r/diag/once`, checked by `pdg_once_check`: key=value words of those options only). At the next
     start `psmac_recovery_once_early`, the first thing `psmac_setup_memory` does, before `psmac_read_options` and any
     other reader of the boot arguments, mounts the card (`psmac_card_early`, the container's or `mac.cardearly`'s rule),
     reads the file, renames it `debug-once.old` at once (`psmac_card_once_take`, core 0, the recovery's own rename under
     the card's lock) and merges its words into the device tree's `bootargs` (`pdg_once_merge`: a key already there is
     replaced in place, a new one goes at the end, so every `psmac_option` and `find_token` sees it; words that do not
     check are left out and named). So it happens once whatever that start does: a start that hangs is followed by a
     normal one, which the boot counter counts as before. A start that is going to be a recovery start (mac.recovery=1,
     recovery.ap, recovery.txt, the counter at its limit) leaves the file armed; one that becomes recovery only at the
     programmer's switch (phase 2, after the arguments were read) has it renamed back by `card_first_look`. A card that
     cannot be written (`mac.card=ro`) does not use it, since it could not be taken away. The Overview shows an armed one
     with its words, **Restart normally to run it** and **Cancel it** (`?cancel=1` deletes the file); the recovery screen
     says so in a line; the tab also shows what this start used and the last one (`debug-once.old`).
   - *Reports.* `GET /r/reports` lists `buscheck.txt`, `cardcheck.txt`, `aurora68.log`, `debug-once.old` and any other
     `.log` in the root with its size and a verdict line taken from it: the bus check's SUMMARY bar's word and the
     summary's first sentence, the card test's Verdict line, a log's first line and its count of `[CRASH]` lines. View
     shows it in the page, Get downloads it. The Pi has no clock and the kernel writes a fixed date, so the time is shown
     only for files a computer wrote (`rec_ent` gained the entry's write date and time); "this start" marks a report
     the tab wrote now. The list is made in two steps, the root's walk first and the reads after it: the first version
     read each report from inside the walk's callback, which holds the card's lock, and core 3 hung on its own lock
     (the testbed's first run: the service loop's watch said "no turn for 42 s: it is in recovery").
   - *The report bundle* (`GET /r/bundle`, `aurora68-report.txt`): the machine (board and model, the kernel's git hash
     and build time, the CPLD's usercode or why there is none, the bus, the Mac's memory and the logic board's own, the
     card, why this is a recovery start and the boot counter, the network, the one-shot debug start armed and last used,
     the boot arguments in effect, cmdline.txt, and the options set there that the board does not use), this start's bus
     check and card test, `buscheck.txt`, `cardcheck.txt`, the last start's `aurora68.log` and this start's log, made in a
     256 KB buffer and then scrubbed (`pdg_scrub`) into a second: the value of every key whose last dotted part is psk,
     pass, password, passphrase, key, secret, token or pin (`mac.recoverypin`), the password part of `mac.wifisim` and
     the key part of `bt.dev`, become "(hidden)", and so does every literal occurrence of the secrets this start knows
     (the WiFi password and the remote's token from the boot arguments and cmdline.txt, the simulated radio's password,
     the access point's password), wherever a log repeats one. The PIN, four digits, is hidden where a key names it and
     nowhere else, since four digits occur in any log.
   - *Host test* (`tools/diag_test.c` and `diag_test.sh`, in `make -C mac check`: 4,344 checks): every option's every
     word accepted in either case and its example accepted, nonsense refused, a group, boards and text, no key twice,
     thirty-two testbed options absent; the patterns (sizes, hex, mac.setrace, remote.ip, remote.mac, a name with a
     space but no quote, the channel 1 to 13); the boards each option must and must not show on; the one-shot words
     (allowed; a non-debug option, a bare word, a bad value, two lines, nothing, a secret refused); the merge (replaced
     in place with the spacing kept, new ones at the end, a bad word skipped and named, the last of a key winning, too
     small an output refused, into empty arguments); the scrub (each kind of secret in its place, literal copies,
     quoted values, the PIN only after its key, the rest untouched, the buffer's end). `mkpage.py --check` now fails
     the check when psmac_recovery_page.h is not what page.html makes.
   - *Tested in the testbed.* QEMU's solo board (a 128 MB FAT32 card image with 1 KB clusters, `CARD=` and
     `mac.cardearly=1`, pinet on the wired port, `mac.recovery=1`, `mac.recoverypin=` for the scripts; the card's
     cmdline.txt and the kernel's own arguments carrying a made-up `wifi.psk=PASSWORD` and `remote.token=YOURTOKEN`, and
     an `aurora68.log` on the card with two `[CRASH]` lines and the password written into a line of its own): the page
     over HTTP byte for byte page.html; `GET /r/options` with 86 definitions in seven groups; `mac.fpu=fast`,
     `mac.board=amiga`, `mac.screen=1024`, `mac.buscheck=2`, `recovery.apchannel=14`, `mac.peek=zz` refused (422, each
     naming its values) and `mac.fpu=none`, `mac.screen=1024x768`, `mac.buscheck=force,loop`, `mac.peek=910,32`,
     `mac.setrace=200,VIA`, `mac.seram=2.5` and a remote.ip taken; for each of se, plus, 512ke, 512k, 128k and solo a
     dry run with `mac.plustrace`, `mac.sescsi` and `mac.solotrace` in the line gave the board back and named the ones
     it does not use (the SE: plustrace and solotrace; the Plus family: sescsi and solotrace; solo: sescsi and plustrace);
     a save with `mac.screencheck=on mac.logsave=10` kept every other word (`wifi.psk` and `remote.token` shown as `***`).
     The Diagnostics tab on the solo board: the bus check refused (422) with "the solo board has no Macintosh logic board
     behind the Pi: there is no bus to check", the JTAG read with "there is no CPLD to read"; the card test read
     kernel8.img's 1,550 KB at 314 to 421 KB/s in three runs (VERY SLOW, as QEMU's emulated controller and a file on the
     host make it; the note says so) and wrote `cardcheck.txt`; one-shot words refused (`mac.fpu=none`, `mac.logsave=soon`,
     a bare `mac.synclog`, `wifi.psk=x`) and `mac.logsave=5 mac.synclog=1 mac.solotrace=20 mac.audiotest=2` armed, the
     state saying so and the recovery screen with its line; the reports with their verdicts ("saved after a crash record;
     2 [CRASH] lines", the card test's); the bundle (18 KB, `Content-Disposition: attachment`) with no `PASSWORD`, no
     `YOURTOKEN` and the PIN's key hidden: the boot arguments' and cmdline.txt's secrets and the log's literal copy all
     "(hidden)". "Restart normally" then restarted the Pi by the watchdog into recovery again (QEMU keeps its
     `mac.recovery=1`), and that start said "debug-once.txt is on the card: kept for the next normal start". The next run
     without `mac.recovery` (the same card): "a one-shot debug start (debug-once.txt, now debug-once.old): 4 options
     added to the boot arguments", then "the log goes into aurora68.log every 5 s (mac.logsave=5)" from the card writer,
     which read the merged arguments. The run after it, a recovery start again: no `[DEBUG]` line, nothing armed, the last
     one listed from debug-once.old, the boot arguments in the bundle without `mac.logsave=5`, and among the reports the
     58 KB `aurora68.log` the one-shot start had saved ("saved on its own (mac.logsave); no crash record"), whole in the
     71 KB bundle with no secret. `fsck.fat -n` found the card clean afterwards. MAME's SE (`macse_ps`, `mac.board=se`,
     the same card, `mac.recovery=1`): the bus check refused without force ("this is the testbed's bridge, not a PiStorm")
     and the JTAG read too; with `force=1` the page's polls showed "Test 1 of 7" to "Test 7 of 7" over 19 s and then the
     49 lines of the report, CLEAN ("All clean: 54272 reads of RAM, the VIA and the SCC, no errors. The bus is good"),
     265,095 bus cycles over the bridge; the SE's own screen (MAME's snapshot) showed the bus check's page while the
     report held it, `buscheck.txt` (4,095 bytes) was written and listed with its verdict, the bundle had it and no
     secret, and "Give the screens back" brought the recovery screen with the line "Bus check: CLEAN All clean ...".
     MAME's Plus (`macplus_ps`, `mac.board=plus`, no SCSI disk): the same, "Macintosh Plus RAM 4096 KB", 201,304 bus
     cycles in 12.4 s, CLEAN, `buscheck.txt` written and in the bundle; the first test's line now fits the 64 columns (on
     the SE run it was cut, "... (and how much": the stage's words were shortened).
     The page's script under Node against a stub with the answers recorded in these runs: the Overview's armed card, the
     Options tab's groups, the board filter for every board (the counts above, the bus check on the five compact boards
     only, the Plus family's traces, the SE's SCSI, the solo board's trace), the Diagnostics list, the report area, the
     one-shot list for the board, the reports, and "Arm and restart normally" posting the words and then the restart.
     Headless Chrome did start this time (with D-Bus errors it survives), against a small stub server with the SE run's
     answers: `docs/img/recovery-options-page.png` and `docs/img/recovery-diag-page.png` (the address changed to the
     documents' 192.168.1.20); `docs/img/recovery-diag-buscheck-se.png` is the SE's screen with the report (MAME).
   - *Not verified until a real compact Mac:* the bus check over a real PiStorm's GPIO from the tab (the same
     `psmac_buscheck_run` as `mac.buscheck=1`, now on the service core between its turns of the recovery start, which on
     the SE also polls the interrupt button: the poll is paused for the check's seconds); the JTAG read again on a real
     CPLD (the same routine as at the start, but with the protocol long up); a real card's read speed (the thresholds
     are a guess at what makes a disk image load slowly); the one-shot merge on a Pi that boots from a container (the
     card's first look is the same one the declaration ROMs use, so it should be there); the compact Mac's screen
     showing the report on real hardware (as `mac.buscheck=1`'s page, written the same way).

123. **Pushes five seconds a chunk from a Mac; progress on the command line (23 November).** The owner, on the SE over
   WiFi: `push --force Aurora68-Play.bin "Mac Volume:Aurora68:Aurora68 Play:Aurora68 Play"` answered `222208 bytes
   (MacBinary: APPL/PIPY, data 0, resources 222001) in 71.11 s, 3.1 KB/s`, and "we need more feedback on progress
   when pushing or downloading files. its also super slow". The testbed had measured 140 KB/s (item 94).
   - *Measured first.* The kernel's job line already gave the agent's share: in the testbed the same push was 14
     requests and 0.4 s of the Mac's time on a card volume. The difference had to be the network, and the one thing
     the testbed's sender did not do as the owner's does was TCP: pinet's bridge to the kernel filled every window to
     the byte. pinet got a second sender that behaves as macOS's (BSD's): full 1460-byte segments only, a remainder
     shorter than a segment and than half the largest window seen kept back (the sender's silly-window avoidance, RFC
     1122 4.2.3.4), and the persist timer, at least five seconds (BSD's TCPTV_PERSMIN), before it goes anyway
     (`PINET_KERNEL_BSD=PORT`: a second listener beside `PINET_KERNEL`, so one boot measures both). With it the
     owner's file took 45.6 s to the SCSI disk and 45.2 s to the card volume (4.8 KB/s), a 300 KB file timed out
     after 60 s, and pinet logged the persist timer nine times for the file, eight of them "with a window of 1216".
   - *The cause.* The upload's buffer in psmac_files.c was one chunk, 24 KiB, the window it advertised at most that,
     and a chunk went to the Mac only once whole. 24,576 bytes are 16 segments of 1460 and 1,216 bytes: the sender
     sent the 16, held the 1,216 for five seconds, and the kernel waited for exactly those bytes. Ten chunks, nine
     waits: 45 s, and over the owner's WiFi with its round trips and the timer's backoff, 71 s. Downloads never had
     it (the kernel sends, and sends whatever the window allows).
   - *Fixed* (`psmac_files.c`, `psmac_ip.c`): the upload's buffer is 64 KiB, the receive window up to 65,535 bytes
     (the most without window scaling), and a chunk is posted to the agent as soon as 24 KiB are in, so the window
     never closes on bytes the job is waiting for; whatever is in hand is written anyway after 150 ms without new
     bytes (a safety net: a write needs no particular length). The receive queue holds 64 frames instead of 16, for
     a window's 45 segments sent back to back.
   - *Numbers* (QEMU solo board, System 7.1, the agent in Extensions; push and pull of the owner's 222,208-byte
     MacBinary file; "card" a 20 MB HFS image as `mac.disk0=`, "SCSI" the startup disk behind the emulated SCSI;
     seconds end to end at the client, including its `info` before a push):

     | | before | after |
     |---|---|---|
     | push, BSD sender, card | 45.23 s (4.8 KB/s) | 0.40 s (541 KB/s) |
     | push, BSD sender, SCSI | 45.61 s (4.8 KB/s) | 2.33 s (93 KB/s) |
     | push 300 KB raw, BSD sender, card / SCSI | timed out at 60 s | 0.45 s / 2.47 s |
     | push, plain sender, card / SCSI | 0.41 s / 4.50 s | 0.31 s / 1.73 s |
     | pull, card / SCSI (either sender) | 0.10-0.18 s / 1.25-5.09 s | 0.17 s / 1.36-1.39 s |
     | 3,000,000 bytes, BSD sender, card: push / pull | (not tried) | 2.53 s / 1.34 s (1.1-2.1 MB/s) |
     | 2.5 MB MacBinary with both forks, BSD sender: card push / pull; SCSI push / pull | (not tried) | 2.32 s / 1.31 s; 18.2 s / 17.6 s |

     The two-fork file came back identical but for the header's dates (the test file had none, the Mac set them)
     and its CRC, and bytes 16-127 of the resource fork (item 94).

     Through the semihosting channel nothing changed (it never had the window): 1.9 s to SCSI, 0.35-0.5 s to the card.
     What is left is the Mac's: the job line now splits it, e.g. `push PiVol:raw300: done, 307200 bytes in 457 ms,
     656 KB/s (17 requests to the agent: 162 ms waiting for the Mac to come round, 292 ms of its work; 0 ms waiting
     for the network)`; on the SCSI disk each 24 KiB write is about 110 ms of the agent's work under QEMU, on the card
     about 17 ms. Recovery uploads (the same TCP) were tried before and after with both senders: 1 MB in 4.3-5.9 s
     before, 5.0 s after, never stalled (the card writer drains its ring steadily).
   - *Where the time goes, from now on.* The agent writes the request's number into a new field, `taken_seq`, when it
     begins one (agent version 2; `chained` counts requests served back to back in one GetNextEvent); the kernel
     times each request from posting to that and from that to the answer, and a transfer's waits for the network
     (an upload with less than a chunk in hand, a download whose answer buffer is full). `mac.filestrace=1` logs
     every chunk. Unchanged layout of the block: the two fields were padding.
   - *Progress.* `GET /files/progress` answers on its own connection while a job holds the agent: the job, its phase,
     the fork, the forks' bytes written or read on the Mac of their total, what the Pi has received or sent, and the
     times; between jobs the last one. aurora68.py's `push` polls it every 0.4 s and shows a line on stderr, redrawn
     on a terminal (`push raw3m  1.6 MB of 2.9 MB  56%  written on the Mac  155.0 KB/s  0:08 left  (the Pi has 1.7
     MB)`), a line every five seconds elsewhere, nothing with `-q`; `pull` counts the bytes as they come (each read on
     the Mac already; the fork from the MacBinary header's lengths); `recovery put` polls `/r/state`'s written bytes
     and `get` streams. The remote's web page has no file transfers; Setup and Recovery's page had its progress bar.
   - *Compatibility, tried:* the new client with the old kernel (404 from `/files/progress`: the bytes sent, then
     "waiting for the Mac" with the time; the old kernel's stall still there with the BSD sender, as expected); the new
     kernel with the old agent (version 1: everything works, the wait for the Mac counted as its work); the old client
     with the new kernel (the bench above ran both). The mouse moved during a 3 MB push to the SCSI disk (two `move`s
     answered in 70 ms, the pointer where it was sent in both screenshots, Aurora68 Log scrolling meanwhile).
   - *Not verified:* the owner's SE and WiFi. The testbed's BSD sender reproduces the five-second stall, which explains
     45 of the 71 seconds by itself; the WiFi's round trips and losses will now show as "waiting for the network" in
     the job line, and `mac.filestrace=1` would show them chunk by chunk.
124. **The classic Aurora68 Play 1.8: windows that resize, the artist in the titles, IPv6 first, videos in the background
   (23 November).** The owner, from the real SE running Play 1.7 with a 2560 x 1440 HDMI screen in colour and a music
   video playing through the card: "Resizable windows"; "Add the artist name into the window also"; "I need to be able
   to connect to server and have it prefer its IPv6 address then its IPv4" (Now Playing had said "IPv4" after
   discovery); "Video in the window stops playing if it isn't focused. This should work in the background. Audio keeps
   working though"; "it also locked up when I was moving through the list of videos using the scroll bar". docs/09
   "Aurora68 Play 1.8" says what each does now.
   - *The video window's size box.* 1.7's window was a zoomNoGrow (kind 12). A size box inside the picture would be
     drawn over by the card at every picture, so the window became a zoomDocProc (8) with a strip of 15 pixels below the
     picture: the size box at its right, the time at its left (library.c hands it the text once a second), a line along
     its top. `vc_grow` holds the card (`vc_hold`) while GrowWindow drags its outline, then takes the side dragged further
     (each side's change against its length), makes the other follow the video's shape, 160 pixels wide at least and
     Fill Screen at most, and snaps the window to it; the card is told the new place with the usual Control 112. The
     width is a fourth size, `VC_CUSTOM`, used for the next video and kept in the preferences (an older Play reads that
     zoom as Normal). The zoom box now toggles Fill Screen and the size the window had (Inside Macintosh's "user state"),
     and the place before Fill Screen comes back with it. On the way: a new window opened at Double Size (1.7) or at the
     size box's size went to the screen's top left corner, because its size was worked out from its contRgn, which is
     empty until the window is shown; the content's rectangle now comes from the port.
   - *The other windows.* The player window (1.1) and the Library window (1.2) had a size box and a zoom box already,
     their smallest sizes (340 x 106, 360 x 200) inside the SE's screen, their contents laid out from their size. What
     was missing was their places by screen: the preferences kept one place each, and a window used on the HDMI screen
     came back on the SE's own screen when the HDMI screen was off (1.7's on-screen check), and then stayed there. The
     preferences' second tail (marked PL18, after 1.7's PL17, so that 1.7 still reads the file) keeps both windows'
     rectangles for the four latest arrangements of screens (the desktop's bounding box), the one for the arrangement at
     the start taken, still checked as before.
   - *The titles.* "Artist — Title" for the video window; Now Playing's second line the artist and the album, either
     alone; the Library's strip "Artist — 0:46 of 3:49". `named()` in library.c: a name with no letter or digit (Mac
     Roman's accented letters count) is shown as nothing there, and as "Unknown artist" at the head of a group in
     Library > Videos and in an artist's row; the server replaces "_" where it can since item 121, and the Play no
     longer shows the underscore when one gets through (the owner's photo: "Kerosene" over "_", and "— — 0:46 of 3:49").
   - *IPv6 first.* Why "IPv4": the probe's answer names the server's IPv4 and IPv6 addresses and 1.7 tried them in
     Connect By's order (IPv6 first by default), but only when the probe ran; after that the preferences kept the address
     that had answered, and a remembered server is not probed. A server first reached at its IPv4 address (typed in
     Connect to Server..., found while its answer named no IPv6 address, or after an IPv6 address that did not answer
     then) stayed at IPv4 for good, and the log never said which addresses the answer named. 1.8 keeps the addresses by
     family (`srv_v6`, `srv_v4[2]`: the one the answer came from and the one it names) in the preferences' new tail;
     tries them in Connect By's order at every start; gives a connection five seconds while another address remains
     (`fetch.quick`; Riptide answers an IPv6 address with no neighbour with ipDestDeadErr, -23033, sooner); moves any
     request whose connection could not be made (a list, a queue, a cover; and a stream, through radio.c's new
     `other_host`) to the next address and asks again; and keeps the one that answered for the session. A server
     reached over IPv4 with IPv6 wanted and its IPv6 address unknown, or a remembered IPv6 address that did not answer
     (the server may have another now), gets one quiet probe a session, and the Play moves to the IPv6 address its
     answer names if that is not the one that failed and if it answers. Connect By gained IPv4 Only (a name's IPv6 addresses passed over, an
     IPv6 address typed refused with words); changing Connect By moves to the family named first. The log names the
     addresses the probe's answer gives ("it names IPv4 192.168.68.1, IPv6 fd68::1"), the order tried, each that fails,
     and the one kept ("the server answers at fd68::1 (IPv6): that address is used from now on (IPv6 First)").
   - *Riptide dropping its IPv6 address (seen in the testbed, worth looking for on the SE).* In both long testbed
     sessions Riptide lost its fd68:: address four or five minutes after the start: its mDNS responder announced the
     name again with the link-local address alone, no router solicitation followed, and connections to fd68::1 failed
     with -23033 from then on (the Play moved to IPv4 by itself). There was no link change and pinet's router
     advertisement gives the prefix a day. Riptide's lifetimes (src/core/ip6.c, `rt_ip6_seconds`) count seconds by
     adding `now - last` of a millisecond clock (`rt_now_ms`, from `_Microseconds` on the Mac) as an unsigned number,
     so one step backwards of that clock would add some 49 days at once and expire every address with a lifetime; that
     is a guess at the cause, not a finding, and it belongs to Riptide. If it happens on the owner's network too, it
     would also explain an "IPv4" in Now Playing with IPv6 configured; the Play's log now shows when the IPv6 address
     stops answering.
   - *Videos in the background: the card reads the window's region.* The rule since 1.6 was "visible" only with Play in
     front and no window over any corner of the picture (the card draws rectangles; Aurora68 Video's rule was stricter:
     only while its window was the front one). The card's block (version 3; main's buffering of item 125 made it 2) now
     takes, in the Mac's half's padding, `clip_tag`, `vis_rgn` and `vis_flags` (psmac_video.h; docs/video-decoder-api.md):
     the program writes its window's visRgn handle and a tag naming the geom_seq of the Control 112 it is about to make,
     and the card, at every picture (`vis_parts` in psmac_video.c), reads the handle's master pointer and the region,
     turns its inversion lines into rectangles (bands of the same columns merged downwards, at most 16) cut to the
     picture and the screen, and draws only those. The Window Manager keeps the region current for every window
     whatever program is in front, so the card follows a Finder window put over the video while Play gets no time. A
     stale tag (another program's Control 112) means the whole rectangle as before, so the QuickTime import and older
     programs are unchanged; a region that does not read as one (a block moving in the heap as it is read) loses that
     picture only. The card reads the Mac's RAM directly (`psmac_video_rd32`: inside the RAM, aligned): a first version
     went through the 68k's MMU tables as the crash record does, and the kernel stopped during a start-up while a
     debugging line read a handle from them. Play and Aurora68 Video check the block's version and keep 1.7's rule with
     an older kernel. The region parser was also checked on the host against hand-made regions (an L shape, a hole, a
     rectangle, empty, garbage).
   - *Menus.* Menus are drawn outside every window's region, and a program in the background gets no time while another
     tracks its menu, so the card itself draws nothing while a menu is down, whichever program's: the menu bar
     definition's saved-bits handle at MBSaveLoc ($B5C), whose first word is the offset of the last menu's record, 0
     when none is drawn ($1C with the Finder's Apple menu held, in the testbed). The first try looked at TheMenu ($A26),
     which stayed 0 while the menu was held: the Menu Manager sets it for the title left highlighted after a choice, not
     while tracking. Play still holds the card while its own menu is down, its windows are dragged or sized and its
     dialogs are up. The kernel's heartbeat line says "region parts N, M under a menu, K unread". The SIZE resource
     already had canBackground (elf2app.py's 0x5880) and the Play polls the card's place on every pass of its event
     loop; what had stopped the picture in the background was only the `fg` in the visibility.
   - *Buffering.* With item 125's buffer in the Pi (block version 2 and later), Now Playing says "Buffering 45%, 1.3 s"
     from the card's `buffer_pct` and `buffer_ms` while the card buffers.
   - *The lock-up while scrolling: not reproduced.* With a 90-second video playing and its window partly under the
     Library window, the Videos list's down arrow held 12 seconds, the up arrow 12, 24 clicks in the page areas, three
     drags of the thumb and the arrow held 8 more: nothing locked up and the picture went on. Read on the way and made
     safe: while an arrow is held the Control Manager keeps the event loop from running, and a stream that had stopped for
     room in the card's ring stayed stopped (less likely since item 125's buffer empties the ring; the scroll bar's
     action procedure now starts it again, `radio_keep`); the event loop's "a receive completed without its completion
     routine" check called the completion routine with its test of `in_done` and the setting of it two steps apart, so
     the real completion arriving between them would run the receive loop twice at once (the same bytes twice into the
     ring, two receives on one parameter block), and a late real completion read the next receive's "in progress" as an
     error and ended the chain with a receive outstanding: the test and set are now made with interrupts off and a call
     for a receive still in progress does nothing (counted and logged); the scroll bar is no longer highlighted again at
     every step while tracked. A pass of the event loop that took three seconds or more is logged with the stream's
     state ("the event loop was away 12.2 s; video stream, a receive out, 512 KB free in the ring, last bytes 0 s ago,
     receives 20"), so a lock-up on the SE leaves what went before it in the log; with the kernel's heartbeat (the 68k's
     PC, the slot-9 line) that should tell where it stopped.
   - *Tests* (the solo board, System 7.1 with Riptide, the music server on the host through pinet over IPv4 and IPv6, a
     generated library of 46 videos by ten artists with one in `Albums/_/_/` whose artist is "_", `MUSIC_COPIES=show` so
     that the short test clips were not taken for copies of one another): the IPv6 and fallback cases, the size box at
     640 x 480 and 512 x 342, the background, a Finder window and the Library window over the picture, the Finder's menu
     held over it, the titles, the places restored after a session on the other screen size, the scrolling above; docs/09
     has the details and `docs/img/system71-play18-*.png`, `system71-512x342-play18-size-box.png` the screenshots.
     `make -C mac check` green.
   - *Not verified:* the SE and its HDMI screen (the region read on the SE board's screens, the menu check with the SE
     board's ROM, colour depths); MAME's SE board; Apple's MacTCP (IPv4 only, unchanged); the lock-up itself; a real
     network's IPv6 (unique local and global addresses, a router's advertisements).

125. **Music videos: a buffer in the Pi behind the video card's ring, buffering by time, rebuffering (23 November).** The
     owner, after the first music videos on the real SE over WiFi (the classic Play 1.7 streaming the music server's
     `/vmpeg` through the card, the picture on HDMI): "might need a bit more buffering also". The videos stuttered and
     stopped whenever the network slowed for a moment.
   - *Why it stuttered.* The card's ring is 512 KiB: 3.5 s at 512 x 342, about 3 s at the rates Play asks for HDMI
     (768 kbit/s of video and 128 of sound), so a network pause longer than that ran the card dry, and while the ring
     was full the Mac stopped reading, so what the network could have carried in the meantime was never asked for. A
     stream started after 64 KiB, under half a second at those rates. And with sound the sound is the clock: the
     mixer played each packet's worth as it came and stopped again, the pictures with it, so a starved stream
     stuttered frame by frame (and the card's "times dry" stayed 0, item 114's "stretches").
   - *The design* (docs/16 "Buffering", `psmac_video.c`; the window, the ring and the commands are unchanged, so Play
     1.6/1.7, Aurora68 Video and the QuickTime components needed nothing). A buffer in the Pi's memory behind the ring,
     8 MB by default (`mac.vbuffer=MB`, 0 to 32; 0 is the card as it was), taken from the kernel's heap at the first
     stream with 8 MB of the heap always left to the rest (halved until that holds, down to 1 MB) and kept. The card's
     core (core 1, `psmac_video_poll` between the sound's turns) moves the ring's bytes into it every slice, up to 256
     KB at a time, under the same 50 us rule for `head` and the same barriers as load_cb used, and advances `tail`;
     load_cb now reads the buffer. So the ring is nearly always empty and the Mac's TCP keeps the network's rate until
     8 MB are held (two minutes at the Play's 320 x 240 rate, over a minute at its HDMI rate). STOP empties both (a seek
     is STOP and a new stream). A stream starts when it holds `mac.vprefill=`'s first number of seconds (3), all of
     itself, or a full buffer; seconds are bytes over the stream's rate, which the card measures from the pack headers'
     SCR as the bytes go by (a jump of more than two seconds, a seek's, left out). When the decoder starves while the
     stream is not all in (no picture, or the sound under 100 ms in the mixer), the clock stops (the mixer paused with
     what it holds, or the timer held) and the card buffers to the second number (2 s) before going on: one pause,
     counted in `underruns`, its time in `rebuffer_ms`. The block is version 2: six new fields in its spare words at
     $68 (`buffer_ms`, `buffer_pct`, `buffer_size`, `rebuffer_ms`, `byte_rate`, `buffer_target_ms`;
     docs/video-decoder-api.md "Buffering"), `buffered` now counts the Pi's buffer too (Aurora68 Video's length
     estimate subtracts the ring from it, so it still works), and the message while buffering is "buffering N%".
     Each stream logs when it starts ("starts after 0.6 s with 3.1 s held (586 KB at 1581 kbit/s)"), each rebuffer
     (the first ten), and at its end the high-water mark, the rebuffers and their time, and the measured rate.
     Setup and Recovery's Options offer both settings (group Video card), README's table has them.
   - *Found by the host test:* a rate from the closed SCR steps alone read low at the start of an FFmpeg stream (its
     first pack is a few KB followed by half a second of SCR, `-muxdelay 0.5`, then 250 KB in one step): 360 kbit/s of
     a 1,370 kbit/s film, and the stream started with less than a second held. The bytes after the last pack now count
     with the time up to it, so the estimate can only be high (it starts at 1,581 kbit/s and settles at 1,350), and a
     start waits longer, never shorter; before any step the card assumes 2.6 Mbit/s, the server's most. And a first
     version counted the drain as the slice's work, so the decoder never ran while bytes were coming in.
   - *Tested on the host* (`tools/vbuf_test.sh`, in `make -C mac check`; `psmac_video.c` itself built with
     `PSMAC_VIDEO_HOST` against a simulated clock, network and mixer; a 20-second FFmpeg film made at test time with the
     server's arguments at 1,200 kbit/s of video, so that the ring holds 3 s). Every scenario decodes and shows all 500
     pictures, the same ones (one hash of what was drawn), with no gap in the sound, starting with at least 3 s held:
     steady at twice the film's rate (no rebuffer); bursts of 1.5 s at six times its rate with 4.5 s of nothing between
     (no rebuffer); 1.25 times its rate with 6 s of nothing from 8 s, through a 1 MB buffer so that it wraps (one
     rebuffer, 3.3-3.4 s); the same without sound, the timer the clock (one, 3.1 s); a 3-second pause with the network
     going on; a STOP at 5 s (ring and buffer empty, idle) and the film again from its start. The card as it was, the
     same harness on the previous `psmac_video.c` (scratch, not committed): the sound ran out 3 times (1.2 s in all) in
     the bursts and 4 times (4.4 s) in the outage, each started after its first 64 KB; the new code with
     `mac.vbuffer=0 mac.vprefill=0` stopped 1,216 and 4,394 times, frame by frame. Paused for 8 s in the middle of a
     rebuffer, a stream rebuffered once and went on when resumed (scratch variant).

126. **The owner's SE confirms Play 1.8, the card's buffering and Riptide's clock fix (23 November).** On the real SE
   (PiStorm, WiFi, HDMI colour), with the kernel of items 124-125, Play 1.8, Aurora68 Video 1.1 and the Riptide with
   the IPv6 seconds clock guarded against steps back: "Streaming video is working with IPv6, resizing windows, plays in
   background, seems to buffer okay". So the video window's size box, drawing through the window's visible region with
   another program in front (item 124), the Pi-side buffer and the start and rebuffer rules (item 125) and IPv6 First
   with a lasting IPv6 address (item 124 and the Riptide change) all hold on hardware. Still open from the same
   sessions: the lock-ups after minutes of video and while scrolling the Videos list (no log yet; Play 1.8 logs any
   event-loop pass of 3 s or more).
   - *The pointer over the picture (same day).* The owner, after a long video on HDMI: "mouse has vanished and screen
     is a bit corrupted" (the Mac still running). The card drew every picture straight over the Mac's pointer, which
     the Mac draws into the frame buffer itself and saves what it covers; the pointer vanished over the picture and,
     moving on, left old pieces of picture behind. psmac_video.c now leaves a box 24 pixels each side of the Mouse
     point ($830) alone when it is on the picture's screen (found through the device list $8A8: the GDevice whose
     PixMap is that screen's, its gdRect's corner); the box is drawn again once the pointer has left it. Not yet seen
     on the SE (the emulator was busy); the owner's next run is the test.

127. **The owner's two lock-ups in a video: a TCPAbort Riptide never saw; Aurora68 Play 1.8.1 (23 November).** Twice on
   the real SE (Play 1.8, Riptide over WiFi, a video through the card on the HDMI screen, Octavo open), after minutes of
   video the Mac froze: the pointer moved, windows could not be moved or brought forward, the keyboard did nothing. The
   heartbeats, 17 and 13 in a row, all said the same: PC 4080bb8c, SR 2010, the JIT at 169 MIPS, interrupts of levels 1
   and 2 still taken, Aurora68 Play in front. In the IIci ROM 4080bb8c is `movew %a0@(16),%d0; bgts 4080bb8c`, the
   Device Manager's wait for a synchronous call's ioResult to stop being positive. Before each, Riptide had logged
   "tcp: memory pressure, 256 KB in socket buffers" (the second time seven times, as a new video's stream, the second
   of a queue, began), and the video sat at "buffering, 1 KB waiting".
   - *Which call, from the registers.* A0 was 07f1aa3c the first time and 07ed68c4 the second, beside Play's globals.
     Play's image is linked at 0 with its globals in it, so a static at offset X sits at the load address plus X; of
     Play's parameter blocks (`nm mac/build/piplay.elf`; the committed binary differs from a rebuild only in its
     header) one gives a load address that explains the other registers too, both times: fetch.c's `pb_end`
     (0x2c29c), with A4 at 0xf08 (`memset`) and A3 at 0xc2b2 (`net_ipp`), the registers fetch.c's `teardown()` keeps
     them in, A2 (saved by the trap dispatcher) at 0x1024 (`mac_PBControl`), and the stack's other block `pb_end +
     0x84`, `pb_rcv` (A1 after memset). So the Library connection's `teardown()`, TCPAbort or TCPRelease; the D1 the
     dispatcher saved (0x100, then 0) rules out the TCPRelease, before which D1 holds Ticks: the TCPAbort. That
     teardown ends every list, cover, state report and command poll of the Library, several a second while a video
     plays with the device on.
   - *Reproduced in the testbed* (solo board, System 7.1, Riptide of mac-port fed1a6d, Play 1.8, the music server on
     the host, a queue of two 4-minute videos). With the card's buffer at its 8 MB the testbed's intake (about 150
     KB/s through pinet, the film 50 KB/s) never filled it in four minutes and nothing locked up; Riptide logged
     "memory pressure, 171 KB" 27 times as on the SE. With `mac.vbuffer=1` (1 MB, so that the card's ring fills and
     the receive chain stops for room, as the SE's did once its 8 MB were full: "8756 KB waiting (46.0 s, 100%)" in
     the owner's log) the freeze came twice in two tries, after 112 and 57 s of video: "library: The music server
     stopped answering." and then PC 4080bb8c beat after beat with D3-D7 00000008 00000961 00000001 00000003
     00000000 and a stack of the same shape as the owner's. main's new heartbeat line did not show at first: it read
     the PC's two words with `mac_rd32`, which reads RAM only, and the loop is in the ROM; with `psmac_mac_rd32`
     (fixed here) it said `the Mac waits for a driver: .IPP6 (ioRefNum -56) csCode 39, trap a004, ioResult 1,
     ioCompletion 00000000`: TCPAbort, synchronous, on .IPP6.
   - *Why Riptide never completed it.* Riptide's TCPAbort cannot wait: it is answered in the stack's next round. So the
     stack never had it. Riptide's .IPP and .IPP6 are concurrent drivers (its drvr.S): a call that must wait is put on
     the stack's own list and taken off the Device Manager's queue (`_Dequeue`), and drvrActive is cleared. The IIci
     ROM's Device Manager (4080b5f6) puts every call on the driver's queue and, if drvrActive was clear, sets it and
     calls the driver for the queue's *head*; while it is set a call is only queued, and IODone, when the driver
     finishes one, takes the next off the head. drvrActive is set with interrupts on, and the stack's deferred task
     runs at the end of any interrupt that comes while a program is at level 0, which a program in `_Control` is; that
     task calls the completion routine of Play's receive chain, which makes the next TCPRcv. So a call can come while
     the driver is taking another. Two faults then lost it. Riptide linked the call it was taking into its own list
     through its qLink, which was still the Device Manager's link to the call behind, so that call was cut off the
     queue, and the `_Dequeue` that followed left the queue empty; and the path never looked at the queue again, as
     IODone would have. The call behind was never handed to the driver nor completed: a TCPRcv of the chain (the
     video stood still at "1 KB waiting") or of the Library (40 s later "stopped answering", which brought the
     teardown), or, the next time, the teardown's TCPAbort itself, for which Play then waited for ever. Made by hand in
     Riptide's IPP6 test program (drvrActive set with interrupts off, an ip6GetInfo, queued only, drvrActive cleared,
     a second ip6GetInfo, which the Device Manager hands the driver as the head, the first): with fed1a6d the second
     never completed (`DBG after q2: head 00000000 tail 00000000 ... res2 1`, a scratch variant printing the DCE's
     queue). The burst at the network's full rate (4.7 MB/s on the owner's WiFi since item 125's buffer) made it
     likely: the deferred task ran at every frame, so the window between the Device Manager's `bset` and the driver's
     `_Dequeue` was hit far more often. "Memory pressure" is the burst's mark (socket buffers full of a stream read
     faster than the card plays it), not a cause: nothing in Riptide waits on socket memory (tcp_mem.c only stops
     buffers growing and, above mem_high, drops segments for the sender to send again).
   - *Riptide's fix*, on its branch `play-hang` from mac-port: the Control entries (ipp.c, ipp6.c) take a call that
     waits off the Device Manager's queue themselves (`ipp_dm_dequeue`, interrupts off) before its qLink is used, and
     the path for a call that waits (drvr.S) looks at the queue with interrupts off: a call there is taken as IODone
     takes it, the driver staying active, through the entry its ioTrap names. The IPP6 test has the case ("a call
     queued while the driver was busy completes", "... and so does the call behind it"): 95 passed and 2 failed
     with the fix, 94 and 3 without (the two others fail in this testbed either way: a scope name and riptide.conf
     host lines it does not have); Riptide's host suite 42 passed, 0 failed, 18 skipped.
   - *Tested after the fixes* (the same testbed): Play 1.8 with the fixed Riptide and the 1 MB buffer played the first
     video to its end (240 s, the ring full throughout) and a minute of the second (it had frozen in both tries before). Play 1.8.1 with the fixed Riptide and the card's 8 MB, the
     Library's traffic and the scratch program that makes the old driver lose the next call: the first video to its
     end and the second begun, no call left, the intake resting at 20 s held and going on at 10 ("rest 1" ... "rest 3"), the first video's
     whole stream 15,730 KB in 241 s (533 kbit/s, its own rate) where Play 1.8 had pulled 7.4 MB in the first 50 s.
     Play 1.8.1 with the old Riptide and the scratch program: no freeze; its first version's receive chain stood still
     on a lost receive, so a receive that has not come back 30 s past its own timeout (and an open or a send after 30
     or 45 s) now ends the connection, which is made again from where it was (seen: the receive's block and buffer left
     to the driver, the video again from 182 s; the five-second wait of a lost synchronous call did not come up, the
     lost calls were receives); that watchdog's first version fired on
     every rest (the time of the last byte, from before the rest) and then on a receive made at interrupt time after
     the event loop had read Ticks (unsigned, so "later" was "long ago"): it now counts from the receive's own start,
     against Ticks read then, signed. The HDMI sizing is tested on the host only (the solo board has no slot $A
     screen); on the SE's screen the stream is today's 320 x 240.
   - *Play 1.8.1* (docs/09 "Aurora68 Play 1.8.1"): no synchronous call to a network driver at all. Each is made
     asynchronously and waited for five seconds at most, the receive chain kept going meanwhile (`net_call`); one not
     back is logged and its block, and the buffers the driver may still write into, left to the driver and replaced
     (`net_leave`), given back once the driver is done with them, a stream left so released then. An open, a send or a
     receive that never comes back (past the driver's own timeouts) ends the connection instead of standing still
     for ever. The video's intake rests while the card holds more than 20 seconds and goes on below 10. And, the owner's
     wish of the same day, a stream sized for the video window when it is on the HDMI screen, up to 960 x 540, with
     Controls > Video Quality.


128. **A splash on HDMI instead of the log (23 November).** The owner: "we need a subtle splash screen on the HDMI
   monitor that has the Aurora logo and version and not show the logs unless specifically called for (it's just noise
   now)". Since 17 November the HDMI console drew the kernel's log from power-on until the Mac's video driver took
   the screen (Emu68's logo first), the bench's only window into a Pi with no serial cable.
   - *What HDMI shows now* (`psmac_splash.c`, new, portable C; `psmac_display.c`, "the splash"): a background of
     #0E0F12, the Aurora68 logo a little above the middle at a fifth of the screen's height (96 to 256 pixels), the name
     in Emu68's Topaz at twice the size of the rest, the kernel's version string (`Emu68 1.1.0-beta.2 (date)
     git:hash`, the log's first line, so a photograph names its build), a quiet status line a tenth of the height
     from the bottom and an amber alert line above it. Status: "Starting...", "Loading the disk image NAME...",
     "Starting the Macintosh..." (68k released), "The Macintosh is testing its ROM/memory..." (psmac_boot.c's
     spans), "Starting from the disk image on the SD card..." / "from the SCSI disk..." / "from the disk..." (the
     first PiDisk, SE pseudo-DMA or solo SCSI read), "The Macintosh is starting up..." (its first log line), "Loading
     the extensions..." (the first INIT), "The Macintosh is running" (the Finder), "Waiting for the Wi-Fi..." and
     "Connected to the Wi-Fi network" (alice_net.c's states through `frf_post_wifi`), "Restarting the Macintosh..."
     (a 68k RESET after the display was on). Alerts: no Macintosh ROM, no declaration ROM, a declaration ROM without
     PiDisk's boot record, under-voltage, the WiFi failing to join (cleared when it connects), a System error (the
     `_SysError` record of psmac_crash.c, not "Welcome" or the disk-switch dialog). Setup and Recovery's reason to start
     is on its own screens, which still take HDMI from the splash as they took it from the log.
   - *When:* `display_logo` (start_rpi64.c) draws it into the firmware's 16-bit buffer instead of Emu68's logo,
     `psmac_display_init` again after remaking the buffer, then the service core alone (`psmac_display_poll`, 60 times a
     second it checks whether a line changed: only the two lines' rows are redrawn) until the Mac's display flag is
     set. A setter called before the service loop runs draws its line itself. A Restart clears the flag
     (`psmac_display_bus_reset`), so the splash comes back until the driver opens; with no declaration ROM it stays.
   - *The log when asked:* `mac.console=boot` is the old default (Emu68's logo, then the log until the Mac's screen),
     `mac.console=hdmi` (and `mac.hdmi=log`) the log for the whole run, `mac.console=splash` or nothing the splash. The
     mode is read from the boot arguments in `display_logo` (`psmac_splash_boot`) and again by `psmac_read_options`
     (`psmac_console_mode`), since a one-shot debug start can add `mac.console=` in between; going from the splash to
     the log clears the screen. F12 on a USB keyboard (`psmac_splash_key`, before the key reaches the Mac) changes
     between the splash and the log while HDMI is not the Mac's. The serial line, the log ring, the remote's `/log` and
     the card's aurora68.log are unchanged. The options table (`psmac_diag.c`) has `splash|boot|hdmi`; Setup and
     Recovery's keyboard menu takes its values from the table (it offered "off" and "hdmi"), the default written as no
     word.
   - *The logo* (`tools/mklogo.py`, new): the SVG drawn in pure Python rather than the sizes PNG decoded (that one is
     drawn on a checkerboard with no alpha): the purple square clipped to the rounded rectangle, the six circles blurred
     as SVG 1.1 says a renderer may (three box blurs of d = floor(s * 3 * sqrt(2 pi) / 4 + 0.5), the even-d rule
     included) in linear light, the white band blurred inside its filter region, the stripes covered exactly along x,
     the gradient mask, the black outline; blended over the background into 192 x 192 R,G,B bytes,
     `psmac_splash_logo.h` (387 KB of text, 110,592 bytes in the kernel). `--check` (in `make -C mac check`, with one
     unit of tolerance for another libm's rounding) remakes it and compares, and fails if the SVG's numbers moved; the
     kernel scales it with an area average (smaller) or bilinear (larger) in 16.16 fixed point, so the service core
     never touches the FPU.
   - *Tested.* Host: `tools/splash_test.sh` (in `make -C mac check`) draws the splash at 640x480 to 2560x1440, 480x640,
     512x342 and 64x64 into 16- and 32-bit buffers with a guard band: corners the background, the logo in the middle,
     nothing written past a row or the buffer, the 16-bit picture the 32-bit one rounded, a line redraw that touches only
     the bottom rows and an empty alert that clears its line (86 checks). QEMU, solo board, kernel 3783aca plus this
     change: the default at 640 x 480 (`docs/img/hdmi-splash-640x480.png`, the ROM's checksum test) until PiVideo
     opened 14 s in, then the Mac's picture as before; 1920 x 1080 without a declaration ROM (`docs/img/hdmi-splash.png`:
     the amber line, the splash for the whole run); `mac.console=boot` (the log, then the Mac's picture);
     `mac.console=hdmi` (the log after 30 s); F12 through QEMU's USB keyboard (the log, and back to the splash); a
     recovery start (`mac.recovery=1`: Setup and Recovery's screen over it, unchanged) and its keyboard menu cycling
     `mac.console` splash, boot, hdmi, splash. Not tried: the SE board in MAME (its kept 16-bit buffer and the
     512 x 342 mirror case are in the host test), a real HDMI monitor, a PiDisplay rotation (the splash is drawn
     upright; the monitor may be on its side).
   - *For testers:* the release's `cmdline.txt` still carries `mac.console=hdmi` (the log for the whole run, one screen
     on the SE); take it out, or write `mac.console=boot`, to see the splash or the old start.

129. **Aurora68 Status 2.0: the Pi's heat and speed as gauges on the Mac (23 November).** The owner's Pi 3B was being
   frequency-capped during long videos (overheating): the 68k fell from 169 to 31 MIPS, which only the heartbeat's
   `JIT: ... MIPS` line showed. He asked: "might be good to get this on the Mac Screen, CPU util, temp etc. Gauges in
   a specific app window". Items 127 and 128 were taken by other work of the same day.
   - *The page.* Alice68's status page (`alice_status.h`, slot-$9 window + 0x50000) had the temperature, the ARM
     clock, the throttled word and the cores, filled for the Amiga's cores: on the Macintosh core 1's figure was
     really core 3's (alice_status.c runs on the core that runs alice_net_poll, core 3 here), the names were the
     Amiga's, and on a Pi 3 the temperature was never measured (Alice68 reads the BCM2711's AVS register, which a
     BCM2837 does not have). The header's own rule for new fields is the end of the structure with `size` telling,
     the version changing only if a meaning does; so the page grew from 0x1a0 to 0x200, version 1 still: the nominal
     and measured ARM clock, the firmware's temperature limit, the 68k's instructions in the last second, the
     kernel's heap, a summary of the video card and of the sound card and its stream (docs/09 lists them). The
     Amiga's build fills them with 0xFFFFFFFF. `alice_psmac_status.c` is the Macintosh half: the cores' roles and
     names, their loads (core 1 from the ticks it spends in `wfe`, counted in psmac_usb.c's loop; core 2 from the
     ticks of the bus loop's turns that served a 68k cycle, one counter read a turn in psmac_bus.c; core 3 Alice68's
     own figure moved to its place; core 0 never idles, so its speed instead), the 68k's rate from INSN_COUNT, the
     heap from tlsf, the cards from their blocks (the video card's through a new `psmac_videocard_block()`), and the
     firmware's GET_TEMPERATURE, GET_MAX_TEMPERATURE, GET_CLOCK_RATE, GET_CLOCK_RATE_MEASURED and GET_MAX_CLOCK_RATE
     (ARM) once a second through `alice_mbox_once`: one call each under the mailbox's lock, never while the WiFi
     holds it, a tag the firmware does not answer asked once and logged. The reading happens before `seq` goes odd,
     so a slow answer never leaves a reader waiting. `mac.sensors=off` stops the calls; `mac.sensorsim=on`
     (testbed) makes the sensors up (a Pi 3B heating to 84 °C and capped from 1200 to 600 MHz above 80), with a flag
     on the page.
   - *The window* (docs/09 "Aurora68 Status 2.0"; `mac/pistatus/status.c`, `model.c`, in C with the `mac/c` kit; the
     assembly `mac/pistatus.s` is gone): dials for the temperature (60 and 80 °C marked), the ARM clock (the nominal
     one marked) and the MIPS; four lights for the throttled word, now and since boot; strip charts of the three
     over the last five minutes (a point every two seconds, more minutes when the window is larger); the cores' loads
     with what they run; the Pi's memory, the network's rates, the cards. A compact view (232 x 50) by ⌘2 or the zoom
     box. Colour in a colour window on a screen of four bits or more, patterns on one bit; only what changed is drawn
     again, the charts scrolled a pixel with ScrollRect where the window is unobscured. The normal view is 488 x 292:
     the SE's screen under its menu bar.
   - *The remote control.* `/state` has a `pi` object with the same numbers (docs/13).
   - *QEMU's firmware*, found on the way: QEMU 8.2's raspi3b answers GET_TEMPERATURE with 25.0 °C (limit 99) that
     never moves, and the clock rates (700 MHz, the maximum the same), but not GET_CLOCK_RATE_MEASURED or
     GET_THROTTLED. So the testbed shows a temperature that means nothing; the window cannot tell it from a real one.
     Under QEMU, `wfe` returns at once, so core 1 reads about 87% busy; on a Pi it sleeps until the next event.
   - *Tested* on the host (`tools/pistatus_test.c`, 98 checks, also as a 68020 program under qemu-m68k,
     `tools/pistatus_m68k.sh`; both in `make -C mac check`) and in the testbed at 640 x 480 in one and eight bits and
     at 512 x 342 in one bit, with the simulated sensors, QEMU's, and none (docs/09 has the list and the screenshots).
     One thing looked wrong and was not: a downscaled screenshot seemed to show the MIPS needle on the wrong side at a
     negative angle; the full-size pixels showed it right, and the 68020 test now checks the needle's arithmetic there.
   - *Not verified:* a real Pi's sensors (the temperature moving, the measured clock, the throttled bits): the
     owner's SE during a long video is the test, with the heartbeat's MIPS line beside the window's.

130. **The USB keyboard that sent nothing until it was re-plugged; the host channel's bits in the log (23
   November).** The owner's Pi 3B (the SE's PiStorm, the LAN9514 hub) has long needed the USB keyboard unplugged and
   plugged in again after power-on (item 41 put it down to a device not ready at the boot's enumeration and added
   retries). This time the heartbeat said more: `kb 1 (ok 0 nak 598565 err 1)` for long stretches, so the keyboard
   *was* found and configured and polled about a hundred times a second, one poll had failed, and every poll since
   had been answered NAK; after a re-plug, `kb 1 (ok 49 ...)`. Nothing in the log said what the failed poll was.
   - *Reading the code against known-good hosts.* The polls are Alice68's (`frf_usb_native_hid.c`, interrupt IN
     through the hub's transaction translator: start split, complete split, again on NYET), over the glue's DMA
     transport (`psmac_usb.c`, `dma_packet`). Read for comparison, none of their code used: Linux's dwc2 host
     (`drivers/usb/dwc2/hcd_intr.c`) and Circle (`lib/usb/dwhcidevice.cpp`, `dwhciframeschedper.cpp`,
     `usbhiddevice.cpp`, `usbkeyboard.cpp`). Three differences:
     - *The order the halt bits were read in.* `dma_packet` took STALL, then NAK, then the errors, and XFERCOMPL
       last. Linux takes XFERCOMPL first, whatever else is set beside it, then STALL, then the transaction, babble,
       frame-overrun and toggle errors, NYET and NAK last ("Must handle xacterr before nak or ack. Could get a
       xacterr at the same time as either of these"). With NAK first, a halt that carried a NAK bit beside an error
       was a NAK: no fault counted, nothing done, polled again; and one beside a completed report dropped the report
       with the host's data toggle left where it was while the keyboard's had moved on. That is a path that ends in
       "one failure, then only NAKs", though whether the owner's controller sets the bits that way only its log can
       say.
     - *What happens after a failed poll.* Nothing did: Alice68 keeps the toggle, polls on, and a device whose polls
       all fail for two seconds is enumerated again (item 41), but a stream of NAKs is never "failing". A failure in
       the middle of a split transaction is exactly where the two toggles can part: the translator may have taken the
       keyboard's report and acknowledged it, so the keyboard moved to the next toggle, while the host saw the
       failure. Standard toggles mend themselves after one lost report; a keyboard or a translator that does not is
       the one the owner has.
     - *The keyboard's set-up.* Alice68 sends SET_CONFIGURATION and SET_PROTOCOL(boot). Linux's usbhid also sends
       SET_IDLE(0) to a boot keyboard; Circle sets the LEDs ("setting the LED status forces some keyboard adapters to
       work").
     Also checked, and the same as the hosts above: the toggle starts at DATA0 after SET_CONFIGURATION (the
     endpoint record is zeroed at every enumeration), the interrupt endpoint's interval is the descriptor's, the
     channel's ODDFRM is set for the next (micro)frame, NAK is not counted as a failure. The split timing (a complete
     split 125 us after the start split, up to eight more on NYET) is looser than Circle's (the start split never in
     microframe 6, the complete split two microframes on, at most three) but it is the timing that works after a
     re-plug, so it was left alone.
   - *What changed* (all in the glue, `psmac_usb.c`, and a new portable `usb_hcint.c`; Alice68's files are untouched):
     - `dma_packet` reads the bits in Linux's order (`uhc_classify`). The FIFO transport (`usb=pio`) is Alice68's and
       keeps its own order.
     - After every enumeration the keyboard gets SET_IDLE(0) and SET_REPORT with its LEDs off, by its own route (an
       8-byte endpoint 0, which every device takes; a refusal is logged and harmless). A new line says where the
       keyboard is and what it took: `[USB] keyboard at address 3, interface 0, endpoint 1 IN, every 10 ms, full speed
       through the hub's translator: SET_IDLE taken, LEDs taken`.
     - The keyboard's watch (`usb_hcint.c`, `ukw_*`): after a failed poll of a kind that can leave the toggles apart
       (a STALL, a transaction, babble or toggle error, NYET to the last complete split, a timeout; not a frame
       overrun, which never reached the bus), CLEAR_FEATURE(ENDPOINT_HALT) goes to the keyboard's interrupt endpoint,
       which sets its toggle to DATA0 halted or not (USB 2.0, 9.4.5), and the host's goes to DATA0 with it; at most
       once a second. The mouse gets the same. And a keyboard that has sent no report at all in 20 seconds since its
       set-up, after at least one failed poll, has the bus enumerated once more, which is what the re-plug did: once,
       until a real plug or unplug (an untouched keyboard that never failed is left alone, however long it sends
       nothing).
     - The log. Every failed transfer (a NAK is not a failure) gets one line naming the channel's bits, the first
       eight after each enumeration: `[USB] the keyboard's poll failed: transaction error (rc -6), HCINT CHHLTD
       XACTERR; address 3 endpoint 1 IN, full speed through the hub's translator (hub 1 port 2), DATA0`. The
       thirty-second heartbeat adds, when there were any, the keyboard's failed polls by kind, the last one's bits and
       the toggle resets: `[USB]   keyboard's failed polls: transaction error 1, the last HCINT CHHLTD XACTERR;
       toggle set again 1 time (0 refused); no report since its set-up`. With `usb.trace=1` every failed transfer is
       logged in full: the bits after the start split and after each complete split, HCTSIZ (bytes and packets left,
       the PID), HCCHAR, HCSPLT and the frame number, and the keyboard's first twenty NAKed polls after a set-up and one
       in ten thousand after that, so the owner's log shows what a NAK really carried. `usb.trace=2` also resets the
       keyboard's toggle every ten seconds, to try that path where nothing fails (the testbed). Setup and Recovery
       lists the option under Debug.
   - *Tested.* `tools/usb_hcint_test.c` (in `make -C mac check`, 62 checks): the bit order (a NAK beside XFERCOMPL is
     a completed transfer, beside XACTERR an error; STALL before the errors), the names, which outcomes ask for a
     toggle reset, and the watch (the owner's case: one failure, a reset at once, a second one not before a second
     has passed, one enumeration at 20 s and no second; none for a keyboard that never failed or has sent a report;
     a plug allows one more; the millisecond counter's wrap). QEMU (the solo board, System 7.1): with QEMU's hub, `usb-kbd`, `usb-mouse` and an
     HFS `usb-storage` stick, and `usb.trace=2`: `[USB] keyboard at address 2, interface 0, endpoint 1 IN, every 10
     ms, full speed: SET_IDLE taken, LEDs taken`; the sampled NAKs read `HCINT CHHLTD NAK`; the toggle reset every ten
     seconds went out each time and QEMU's HID model refused it (it STALLs CLEAR_FEATURE(ENDPOINT_HALT), which it does
     not implement; a real keyboard must take it), the host's toggle went to DATA0 all the same, and the keys typed
     after twenty of those resets reached the Finder (Command-N made a new folder; 16 reports), the monitor's
     `mouse_move 20 12` eight times moved the pointer by exactly (160, 96) as in item 40, and the stick mounted as
     USBSTICK on the desktop. With the keyboard alone and no option: only the set-up line and the heartbeats, no
     failed transfer, 18 reports, the typed name in the Finder. What QEMU cannot show: a failed poll (its DWC2 model
     never fails one), so the resync after a real failure and the rescue enumeration ran only in the host test.
   - *Only the owner's Pi can say* whether this cures it: QEMU's DWC2 model has no transaction translator, no
     split transactions and no data toggles to lose. What to look for after a power-on with the keyboard plugged in:
     the `[USB] keyboard at address ...` line (SET_IDLE and the LEDs taken or refused); any `[USB] the keyboard's
     poll failed: ...` line, which names the bits; then either `its data toggle set again` and reports (`ok` growing
     in the heartbeat when keys are pressed), or 20 seconds later `the keyboard has sent no report in 20 s since its
     set-up ... enumerating again`, and whether the keyboard works after that. If it still does not, a start with
     `usb.trace=1` (Setup and Recovery, Diagnostics, for one start) logs the failing poll's every split and the bits
     the NAKs carry.
   - *USB flash drives (paused by the owner the same day).* The owner's SanDisk "3.2 Gen1" (0781:55b1, 128 GB, high
     speed behind the LAN9514) enumerates and is configured, but its first command block (the 31-byte bulk OUT for
     INQUIRY) fails every time, also after the bulk-only reset recovery. The work on it is paused; only the log was
     changed, so that the next try says why: the reset recovery's line now carries the bits and registers of the
     command's failed transfer (`[USB] disk 0: the command block did not go (the transfer's HCINT CHHLTD XACTERR,
     HCTSIZ ..., HCCHAR ...): reset recovery ...`; "no transfer failed" when the stick answered but not with a CSW),
     and `usb.trace=1` logs every failed transfer of a stick. A separate commit, which can be left out. Findings from reading `psmac_usbdisk.c`'s
     `xfer` against Linux and Circle, for when the work resumes (not changed):
     - A halted channel without XFERCOMPL is a failure at once, except STALL: no retry. Linux and Circle retry a bulk
       transaction error up to three times, and Linux resets the error count when the halt also shows NYET, NAK or
       ACK after a PING ("XactErr with NYET/NAK/ACK", the OUT NAK enhancement of DWC2 cores from 2.71a; the BCM2837's
       is 2.80a). A high-speed stick that answers its first OUT with NAK or NYET and then PING is the likeliest
       difference from the control transfers, which work (a SETUP is never NAKed).
     - A retry has to restart from what was taken: bytes and packets left from HCTSIZ, the toggle from HCTSIZ's PID,
       and for a high-speed OUT the DOPNG bit (HCTSIZ bit 31) so that the channel PINGs first.
     - Checked and not the cause by reading: the CBW buffer is 64-byte aligned and cleaned before the transfer; the
       endpoint numbers and the 512-byte packet size come from the descriptor (the CLEAR_FEATUREs on them are
       taken); the toggles are DATA0 after SET_CONFIGURATION and after each CLEAR_FEATURE; a high-speed device behind
       the high-speed hub gets no split; the non-periodic TX FIFO (256 words) holds a 512-byte packet.
     - Also to do then: the 128 GB exFAT stick's view (item 97 stage 2), which the owner wants for moving files
       between the Mac and other machines, and what the Mac is told about a stick that carries no disk image.

131. **The edge jump: the pointer to the other screen anywhere along the side (23 November).** The owner, with the SE's
   own screen at (-512,0)-(0,342) beside the 2560 by 1440 HDMI screen: "When I move the mouse to the small screen I need
   to precisely match its position in Monitors. Can we just have it so moving to the side where the screen is moves the
   mouse regardless." The ROM crosses between screens only where they touch, here the top 342 of HDMI's 1440 rows.
   - *Where to do it.* Every source of movement ends in the ROM's cursor task (IIci ROM $4082DF94, through JCrsrTask,
     $8EE, called by the vertical-blank handler of the cursor's screen at $4080A33E): the ADB mouse handler only adds
     to MTemp and sets CrsrNew = CrsrCouple ($4080AB26), .PiInput sets MTemp and RawMouse for USB, Bluetooth and the
     remote. The task moves RawMouse by the accelerated difference, pins it to CrsrPin, and on a point past an edge
     searches the device list for a screen holding it ($4082E14A; found: CrsrDevice, CrsrPin ($4082E510) and the
     cursor's depth change and it moves there). A VBL task of our own could not see the push: by the time it ran the
     cursor task had usually pinned MTemp back to the edge, and the order of the two is the hardware's. So .PiInput,
     which is on every board under System 6 and 7 from the declaration ROM, puts a cursor task in front of the ROM's:
     before it, MTemp past an edge and on no screen, with a screen on that side, becomes the proportional point on that
     screen (MTemp, RawMouse, CrsrNew = CrsrCouple, then the ROM's task makes the move); after it, a crossing the ROM
     made where the screens touch is moved along the edge to the same proportion, so both ways and every height map
     alike. Remote absolute moves (flagged by .PiInput) and points far from the edge are left alone; a 4-pixel inset
     and a 10-tick dead time after a wall jump keep the pointer from bouncing back. The hook goes in from the VBL task
     and is put back in front should something take the vector without calling it (a reentrancy guard keeps a chain
     that calls ours from looping). `mac.edgejump=off` sets bit 0 of the input ring's new `options` word (the
     long at offset 28 that was padding; alice_psmac.h `ALICE_IN_OPT_NOEDGE`), which the hook reads each time;
     psmac_diag.c lists the option under Display. docs/10 "The edge jump" has the whole of it.
   - *Tested* in MAME's SE (System 7.1, HDMI 1024 by 768 to the right of the SE's screen) with a new scripted mouse,
     `mac.inputedge=SECONDS`: across the touching part at y 100 to HDMI's y 225, against HDMI's left edge at y 725
     onto the SE's screen at (507,323), and back to y 725, before the Finder and with it; the remote's absolute moves
     near the edge landed where they were put; `mac.edgejump=off` kept the wall. Not yet seen: the SE's own ADB mouse
     (MAME's SE gave no mouse input here), the owner's 2560 by 1440 arrangement, System 6, and the feel of the inset
     and dead time on hardware. The owner needs the new declaration ROM (its .PiInput) as well as the kernel; with the
     new declaration ROM and an older kernel the jump is simply on.

132. **The Aurora68 PC Card: a PC in a window on the Mac's desktop (23 November).** The owner: "Aurora86 needs to run in a
   window on the Mac desktop." Aurora86 (a project beside this one: Bochs 3.1 with Libreboot's QEMU x86 firmware, built as
   `libaurora86.a` with this kernel's own flags, whose only outside names are six `a86_host_*` functions) had planned the
   card (its `docs/DESIGN.md`, "P3: the window"); this is the card as built. docs/19-pc-card.md has the whole of it.
   - *The pieces.* `psmac_pc.c`/`.h` (the card: slot $D's window and declaration ROM with the driver `.A68PC`, the block
     'A68P', the PC's memory, files, start, slices, drawing, input), `mac/a68pc.s` (`.A68PC`), `mac/a68pc/pc.c` (the program
     **Aurora68 PC**), `tools/mkpcfont.py` (misc-fixed's 6 x 13 and 6 x 12, public domain, in code page 437),
     `tools/mac/mkpcdeclrom.py`. Only with `-DPSMAC_PC=ON -DAURORA86_DIR=...`; every hook in a shared file
     (`psmac_bus.c`, `psmac_mmu.c`, `psmac_usb.c`, `psmac_card.c`, `psmb.h`, `psmac_diag.c`) is inside `#ifdef PSMAC_PC`.
     Checked: the kernel from the commit before the card and from the card's tree with the option off, both built outside git
     with the same `SOURCE_DATE_EPOCH` (the version string and `__DATE__`/`__TIME__` otherwise differ, and with them, through
     the linker's merged strings, thousands of words), are the same `Emu68.img`, byte for byte.
   - *Shared with the video card.* The window's visRgn as rectangles, the box around the pointer and the menu-down test moved
     out of `psmac_video.c` and `psmac_videocard.c` into `psmac_wclip.c` (a commit of its own, nothing changed in what they
     do; `tools/vbuf_test.sh` passes as before). The PC card adds one thing of its own: no pointer box while the pointer
     is hidden (CrsrVis, $8CC, 0), found in the first QEMU run, where the hidden pointer's box left a black square in the
     middle of the window while the PC had the mouse.
   - *Memory.* The PC needs some 95 MiB besides its RAM and the 12 MiB firmware; the kernel's heap is about 52 MB. So the
     card's memory (window, ROM page, `pc.ram` + 104 MiB as a TLSF pool of its own: 120 MiB with 16 MB of PC RAM) is carved by
     `psmac_setup_memory` beside the frame buffers and the video card's window. In QEMU: "the PC is built in 220 ms: 16 MB of
     RAM, 104 MiB of its memory in use". Bochs's 30 C++ constructors run with the kernel's own (`start.c`'s `__init_start`
     list) and take their few bytes from the kernel's heap.
   - *Which core.* The design said core 2 on the solo board and the IIci testbed; the IIci testbed's core 2 carries every
     I/O cycle over MAME's bridge, so the PC runs on core 2 on the solo board only (slices of about 2 ms between the service
     loop's turns: the heartbeat and the VIA's time), and on core 1 elsewhere (slices of about 1 ms beside the sound output).
   - *The SE's text is 6 x 12.* 80 x 25 in 6 x 13 is 325 lines; the SE's 342 less the menu bar and a document window's title
     and frame leave 301. Below 366 lines the card uses 6 x 12 (480 x 300); 6 x 13 above.
   - *The host test.* `tools/pccard_test.sh` (in `make -C mac check`, skipped without `AURORA86_DIR`) builds `psmac_pc.c`
     itself on the host with Aurora86's x86-64 library and a page of stand-ins for the kernel, draws into a pretend 512 x 342
     one-bit screen and an 8-bit one, and types into the ring as the program does: Libreboot's banner, FreeDOS 1.4 to its
     installer, "No - Return to DOS", `ver` typed and answered (pictures: `docs/img/pc-card-host-*.png`).
   - *QEMU, the solo board with System 7.1* (`pc=on`, the firmware, VGA BIOS and FreeDOS image as host files through
     semihosting, Aurora68 PC in Startup Items, QEMU's USB keyboard and mouse driven through its monitor): the PC started
     with the kernel, Libreboot's banner and FreeDOS's installer within seconds of the Mac's start (QEMU's clock), the
     driver opened by the Slot Manager ("A68PC: the Aurora68 PC Card's driver is open"), the program's window showing the
     installer at 640 x 480 and at 512 x 342 (one bit, 6 x 13 and 6 x 12), a click capturing ("Aurora68 PC: keyboard and mouse
     to the PC"), Return, Down, Return out of the installer and `ver` typed at `C:\>` and answered, Control-Option-Command-P
     giving them back, the window dragged by its title and redrawn at its new place, the program's About box over the window
     left intact while the PC's cursor blinked behind it, the window redrawn when it went, made smaller with its grow box
     and zoomed back to the PC's natural size (`docs/img/pc-card-qemu-*.png`). Moving QEMU's USB mouse through
     `tools/qemu-mon.py` took 2.5 s a command (it waits for the monitor to fall silent), too slow to hit a 15-pixel box with
     hundreds of small moves before the run ended: a monitor client that sends without waiting did it in seconds.
   - *Speed.* The PC's clock is paced (`clock: sync=both`, `ips=40000000`), so it never runs faster than real time; in QEMU it
     ran at the pace while the firmware and the installer waited (20 MIPS by the Pi's clock with `ips` at 20 M, the core
     mostly idle: "19.9 MIPS, 44 per mille of the core busy, 926 idle"), and 1-2 MIPS with the core busy at FreeDOS's prompt,
     whose keyboard polling (port I/O and interrupts, no HLT) is Bochs's most expensive kind of work; QEMU's translator says
     nothing of a Cortex-A53 (Aurora86's STATUS item 16: about 9-32 MIPS estimated). The card's drawing took 50-250 us a
     picture in QEMU.
   - *Not yet:* the IIci testbed with MAME, the SE board, real hardware; the SD card path (`psmac_card_pc_open/_read`) is
     written but only the semihosting path was run; graphics modes were not exercised (FreeDOS stays in text); attaching
     disks from the program, writing the overlay back, USB keyboards straight to the PC, the keyboard LEDs, HDMI full screen.

133. **The PC card polished: its disks written back, CDs from the Disks menu, the text's first column, the pointer's box, FDAPM
   (23 November).** The first slice of the card (item 132) was tested only in QEMU; before the owner tries it on the SE, five
   things from its first pictures and its "not yet" list. docs/19-pc-card.md has the whole of each.
   - *"D-ROM not configured".* The 640 x 480 picture of the captured PC read "D-ROM not configured" and "one processing startup
     files". The pixels said otherwise: the C was there, its left stroke one pixel from the window's frame line, which it
     joined (misc-fixed's glyphs, and the VGA's, use their cell's first column; the right column is the blank one). No
     offset was wrong; the text had no margin. Now two pixels of paper each side (`TEXT_INSET`, in the colours of the row's
     first and last cells, so the installer's grey title bar runs to the frame), the natural width 484 for 80 columns
     (Aurora68 PC's first window too), and the dirty cells marked through `mark_cells`, which knows the inset. Before the
     PC's first picture the window is the PC's background as its text will be drawn (white on one bit), not black.
   - *The pointer's box.* The box the card leaves alone around the Mac's pointer went back into its one dirty rectangle at
     every picture: drawn once the pointer had moved, but the rectangle then spanned the pointer and the cursor's blinking
     cell (much of the window, 30 times a second). Now the part left out is kept with the box it was left for and drawn at
     once when the box moves or goes, and again 100 ms later (the Mac puts back its saved copy at the VBL after the Mouse
     global changes). The testbed then showed the other half: with text scrolling under the resting pointer at 512 x 342,
     moving the pointer away left two letters blank just right of the box ("CD.ERROR %  RORLEVEL%"). The Mac saves and puts
     back what is under its pointer in whole longs of the frame buffer, 32 pixels at one bit, so what it puts back reaches
     up to 31 pixels beyond the 16-pixel pointer, past a box of 24 each side. `psmac_wclip_cursor_box` now rounds the box
     out to 32 / depth pixels across (64 or 96 wide at one bit, 48 to 54 at 8 bits); the video decoder card shares it, and draws a
     paused, ended or starved film's last picture into the box the same way once the pointer leaves (`pointer_moved`;
     `tools/vbuf_test.sh` passes).
   - *Writing back.* What the PC writes (the overlay, by sector) now goes back into the image file in place: into the file's
     own clusters, listed at its open (`psmac_card_pc_write`, a cluster's sectors per command through `sd_write_run`, under
     the card's lock as the reads are), never growing it or touching a directory entry or FAT sector; through semihosting
     ("r+b") in the testbed. 2 s after the PC's last write to that disk, 10 s after the oldest unsaved sector at most, all of
     it at PC > Power Off and Reset and when an image leaves its drive; 4 ms a turn at most on the PC's core; a failed
     write stops it for that disk and says so. `pc.disk0ro=on` keeps the file as it is. The block's `disk_dirty` counts
     what is not yet written. Found on the way: a `freedos.img` in the card's root was also taken as one of the Mac's disk
     images (every `.img` is), loaded into the Mac's memory and offered to the Finder; images the PC's options name are no
     longer the Mac's (`pc_owns` in `psmac_card.c`).
   - *Disks.* Two hard disks (`pc.disk0=`, `pc.disk1=`), a CD-ROM drive on the second IDE channel (`pc.cd=`), and the
     Disks menu: the options' images and the card root's `.iso` (and `.vhd`, `.hdd`, floppy) files, Look on the Card Again,
     a CD put in or taken out while DOS runs through Aurora86's new `a86_pc_media()` (Bochs's runtime media change).
     FreeDOS 1.4's start finds the empty drive (UDVD2, SHSUCDX: "CD-ROM configured as D:") and reads a CD put in later.
     `tools/mkiso.py` makes the test's CDs (no ISO tool on the machine). Floppies do not work and are refused with the
     reason: SeaBIOS under coreboot registers only the floppy drives coreboot's CBFS lists (`etc/floppy0`, SeaBIOS's
     `hw/floppy.c`; CMOS drive types only in its QEMU builds), Libreboot's QEMU image lists none, and DOS reads floppies
     through the BIOS ("unknown command given to driver" with a drive configured in Bochs and an image in it).
   - *Idle at the prompt.* FreeDOS polls the keyboard at `C:\>` without halting: 999 per mille of the core busy. Its own
     `FDAPM APMDOS` makes DOS halt while it waits, which Bochs's pacing turns into rest: on the host 8 busy and 990 idle; in
     QEMU 993 busy before, 93 busy and 894 idle after. FreeDOS 1.4's `FDAUTO.BAT` has the line as `rem FDAPM APMDOS`; the
     owner removes the `rem` from inside the PC (`edit fdauto.bat`), which the card now saves. Bochs has no cheap way to
     see a polling loop from outside, so nothing in the card does it.
   - *Tested.* On the host (`tools/pccard_test.sh`, in `make -C mac check` with `AURORA86_DIR`): a scratch copy of FreeDOS
     1.4; a file made at `C:\>` read back by a second process (a restarted kernel), and not kept with `pc.disk0ro=on`; two
     CDs put in and taken out in turn and read at D:. In QEMU (solo board, System 7.1, the files on a 64 MiB SD card image
     through `run-solo.sh`'s `CARD=`, not semihosting: the firmware read from the card in about 25 s of QEMU's time): at
     640 x 480 a file written at `C:\>` ("freedos.img: 4 sectors written back to the card (16 ms)"), read after PC > Reset,
     the CD put in from the Disks menu; at 512 x 342 after the kernel was started again with the same card image, the file
     read back and the CD read at D:; `mtype` on the host found the file inside the card image's `freedos.img`. Pictures:
     `docs/img/pc-card-qemu-*-2.png`.

134. **Every program on the SE's own screen: 512 by 342, one bit (23 November).** The owner uses the SE's built-in
   screen as well as the 2560 by 1440 HDMI screen, and the SE's can be the main or the only one (no HDMI attached):
   every Aurora68 program must be usable there. docs/09 "Every program on the SE's own screen" has the table of
   programs and windows, before and after, with the pictures (`docs/img/se512-*.png`).
   - *The runs.* The solo board in QEMU at 512 x 342 (`mac.screen=512x342`, `psmac-512x342.declrom`), one bit, System
     7.1, the programs installed by `aurora68-layout.py`; each program opened through the Agent (`aurora68.py open`),
     its windows read from `/windows` (their structure rectangles, global), its About box from the Apple menu, its
     dialogs by their keys, a picture of the screen each time. Once with main's programs, once with the changed ones.
     Kept places: a 1024 x 768 run of the changed image, each window dragged to (300, 42) and grown to the screen's
     corner, the programs quit (their preferences written), the preferences copied into the other image, then both at
     512 x 342. Play needs a sound output to stay open: `mac.audiodump=/dev/null`.
   - *Found.* Bluetooth's window 320 high at 40, its bottom at 362; Screens 600 wide; Colour put at (660, 40), off the
     only screen, and waiting for a click nobody could make; with the HDMI screen main it went on the SE's screen at
     600 x 400. Play's About text ran under its OK button (228 pixels given). Video's About and "not an MPEG-1 film"
     lines past its window's edge, and without the card a 420-pixel line in a 320-pixel window. Places kept from a
     larger screen were taken back when 40 pixels of the title bar showed: Slots and Display at (42, 300)-(758, 1018),
     Status's compact view to 535, their grow and zoom boxes out of reach. Video's film window and the PC window sized
     by the desktop's bounding box, which with the HDMI screen beside the SE's is 3072 x 1440. Wrong Pascal-string
     length bytes in the assembly programs: Wi-Fi's scan heading cut at "Secu", Ping's "installed?" without its "?",
     Colour's last word one letter short, Network's "Pi: fw = " a byte too long. Network, Ping, Wi-Fi, Log, Status,
     Display (its alerts and "Keep this resolution?"), Play's player, Library and Open URL, Video's window and the PC's
     already fitted.
   - *Changed.* `mac/c/screens.h` (new, header only: the screens, the one a window is mostly on, its room below the
     menu bar and a title bar, `scr_fit`); Slots, Status, Display, Play and its Library fit a kept place to its screen;
     Video and the PC size by their own screen and move to lie on it; Log and Play zoom on their own screen; Play's
     message boxes measure their text (`text_lines`, TextBox's word wrap) and widen where 340 pixels would be too tall;
     Video's status line ends at the window's edge with an ellipsis, shorter About and error lines, the no-card text on
     three lines; Bluetooth 472 x 296, its lists stopped above the status line, as Wi-Fi's; Screens 496 wide, two lines a
     screen; Colour on a second screen only where it has room (640 x 460), else on the main screen as large as fits,
     the picture scaled, and a line in one bit saying the colours show as black or white (on the way: its 16-grey row
     had fallen through into the new placement code, found in the first picture with the grey row missing). The Wi-Fi
     and Bluetooth menus' icons, which at 512 pixels sit where a long menu bar ends, are the menu-bar clock's branch's.
   - *Rebuilt:* Bluetooth, Colour, Display, Log, Network, PC, Ping (with `PEER=192.168.0.1`, as the release's), Play,
     Screens, Slots, Status, Video, Wi-Fi. `make -C mac check` passes.
   - *Not verified:* two screens at once (the solo board has one): the HDMI screen main with a window moved to the SE's,
     Colour's choice of screen, the zoom boxes on the second screen, are read from the code, not run; the SE board in
     MAME; Play's dialogs that need a server or a station; Video with a film; RiptidePrefs, Lagoon and Octavo were not
     installed in the image and not looked at.
