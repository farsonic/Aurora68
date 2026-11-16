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

## Next steps (in order, as of the evening of 16 November)
1. MacTCP under System 6 (the same files; untested there), then a real client (Fetch, NCSA
   Telnet) against a TAP-bridged peer (`tools/pinet.py ... tap`), and the WiFi unit on hardware.
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
