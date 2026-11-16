# Emu68 "psmac" port: status (2026-11-16, end of first session)

## What exists
Fork: `emu68/` (branch `psmac`, MPL-2.0), build dir `psmac-build/`, image `psmac-build/Emu68.img`
(to be renamed `kernel8.img` for SD cards). Build:
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

## Next steps (in order)
1. Deliver bridge bus errors as 68030-style access faults for the SCSI Manager's timeout path;
   remove the debug-only read-only mapping of the low-memory page once the watch is no longer
   needed (every low-memory write traps today); make the calibration produce a real count
   instead of the 0xFFFF saturation (docs/06 §2 options).
2. Performance: the JIT runs in its CRC-verifying loop because 68030 code never sets the 68040
   CACR IE bit; decide between forcing the fast loop with explicit invalidation on CACR CI or
   keeping verification.
3. Timing patches for the real Pi (docs/06-rom-patches.md) and TimeDBRA behaviour under JIT.
4. Mailbox window + RAM-disk `block` service on the service core + `.PiDisk` ROM driver
   (docs/07), then SD/exFAT, `.ENET`, WiFi/BT/USB on CM4.
5. Real hardware: replace the semihosting transport with the PiStorm GPIO protocol
   (`ps_protocol.c` functions are already behind the same `ps_read/ps_write` API).
