<!-- SPDX-License-Identifier: MPL-2.0 -->
# 23. A real MMU for the Macintosh: a design note, and the spike that tested its hard part

*25 November 2026.* The owner asked for "that MMU stuff": a memory management unit the Macintosh's software can
really use, so that, in order, System 7's Virtual Memory works, A/UX 3.x boots (the owner will supply A/UX), and
later NetBSD and Linux for the m68k. This is phase 1: what the code does today, what it shows, what we conclude,
and a spike on branch `mmu-experimental` that tried the riskiest point, delivering a precise page fault from
translated code and restarting the instruction. All of it is experimental and behind a new kernel option,
`mac.mmu=full`; without it the kernel behaves exactly as before (section 8 measures that). Where something comes
from the code it names the file and the function; where it is our reading or a guess, it says *inference*.

## 1. What was read

- `emu68/src/psmac/psmac_mmu.c`, the 68030 MMU shim; `docs/03-iici-rom-analysis.md`, "MMU usage"; `docs/08` items
  8, 9, 12, 41 ("Speedometer said 68020"), 46 ("No FPU"), 48, 92 (the PFLUSH right after a TC load).
- The JIT: `M68k_Translator.c` (`M68K_Translate`, `M68K_GetTranslationUnit`, `EMIT_AdvancePC`/`EMIT_FlushPC`),
  `ExecutionLoop.c` (`JITLoop`, `JITLoopWithCRC32`, `ProcessIRQ`, `M68K_LoadContext`), `M68k_Exception.c`
  (`EMIT_Exception`), `M68k_ExceptionEntry.c` (`M68K_Exception`), `aarch64/RegisterAllocator64.c` (how the CC,
  FPSR and FPCR are cached), `M68k_EA.c` (how `(An)+` and `-(An)` are emitted), `M68k_LINE4.c` (`EMIT_MOVEM`,
  `EMIT_RTE`, `EMIT_MOVEC`, `check_cacr`), `M68k_MOVE.c` (instruction fusion), `M68k_LINEF.c` (the PMOVE/PFLUSH
  path and `psmac_emit_mmu_call`, FSAVE's 68882 frame), `aarch64/vectors.c` (`SYSHandler`,
  `SYSPageFaultReadHandler`/`WriteHandler`, `SYSValidateUnit`), `aarch64/mmu.c` (`mmu_low4g_set`), and the
  interpreter, `M68k_Interpreter.cpp` and `M68k_INT_*.cpp`.
- `psmac_bus.c` (`psmac_bus_read`/`psmac_bus_write`, what an invalid logical page does today), `CMakeLists.txt`
  (which files are compiled with which registers fixed).

## 2. What exists today

**The shim** (`psmac_mmu.c`, written for the IIci ROM). `PMOVE` to or from TC, CRP, SRP, TT0, TT1 and MMUSR is
trapped through the F-line emitter (`M68k_LINEF.c` calls `psmac_pmove`). On a TC or CRP load the 68030 tables are
walked once (short and long descriptors, early termination, limits, the IS aliases) into a table of 4 KiB
logical pages, `l2p`, and the ARM mapping of the 68k's first 4 GiB is rebuilt from it: coalesced runs, with the
ROM, the frame buffer and write-protected pages read-only. Each (TC, CRP) pair gets a cached ARM subtree, six of
them, swapped by rewriting four level-1 descriptors (`mmu_low4g_set`, with two full TLB flushes). `PFLUSH`
re-walks the tables and remaps the set if a hash of the descriptors changed; `PLOAD` and `PTEST` are skipped with
their extension words, and MMUSR keeps whatever was last written to it. SRE (a separate supervisor root),
function-code lookup and pages below 4 KiB are not supported (the file's own header). The ROM's 32-bit set is
TC `$80F08900`, CRP `$7FFF0002:$00FFF9E0`: 32 KiB pages, TIA 8, TIB 9, mapping RAM one to one and
`$40000000-$FFFFFFFF` one to one (any solo-board log, "switch #3").

**The faults.** The JIT runs at EL0. A translated load or store to a page that is not mapped takes an ARM data
abort into `SYSHandler` (`vectors.c`), which decodes the ARM instruction, performs it through
`psmac_bus_read`/`psmac_bus_write` and returns to the next ARM instruction (`elr += 4`). Those translate the
address with the shim's table: a page the guest's tables mark invalid reads `$BAD0BAD0` and ignores writes
(`psmac_bus.c`, "would be a bus error"). Nothing ever reaches the 68k. `EMIT_RTE` accepts frame formats 0 and 2
only; any other is a format error.

**Upstream.** Emu68 has no MMU: `MOVEC` to TC clears the E bit ("do not allow turning on MMU", `EMIT_MOVEC`,
case `0x003`; the interpreter's `M68k_INT_LINE4.cpp` says the same), URP, SRP, ITT and DTT are only stored, and
the 68040's `PFLUSH` and `PTEST` are ignored (`M68k_LINEF.c`, "PFLUSHA or PTEST - ignore";
`M68k_INT_LINEF.cpp`, `PFLUSH`, `PTEST`).

**What the Macintosh is told.** Not a 68040. Emu68 executes the 68040's instruction set, but every place where
software asks says 68030: the ROM's probe writes `$2809` to CACR and looks for the 68030's write-allocate bit,
which the PSMAC build's `check_cacr` keeps readable (`M68k_LINE4.c`; docs/08, "Speedometer said 68020"), so
CPUFlag (`$12F`) is 3; MMUType (`$CB1`) is 4, which the ROM's Gestalt turns into `'mmu '` = 3, gestalt68030MMU
(docs/08 item 48); and FSAVE writes a 68882's idle frame, `$1F38`, so `'fpu '` is 2, a 68882 (`M68k_LINEF.c`,
docs/08 item 46, docs/17). Speedometer: "MC68030, MC68882, MC68030 MMU". So Gestalt's `'proc'` is 4
(gestalt68030), `'mmu '` 3 and `'fpu '` 2. CLAUDE.md's standing decision, "the kernel presents a 68040", is true
of the instruction set; the identity is a 68030's and has been since docs/08 item 41.

## 3. Which MMU the guest gets

**What the software expects.**
- *The IIci ROM* knows only the 68030's PMMU: 22 `PMOVE`s to TC and CRP, three `PFLUSH`, one `PTEST`, its tables in
  68030 formats (docs/03). It cannot run on a 68040 MMU at all: on a real 68040 its `PMOVE`s are F-line traps.
- *System 7.1's Virtual Memory* has code for the 68851, the 68030 and the 68040 (*inference*: VM shipped for all
  three kinds of machine). Which it runs is decided by the machine and its MMU type, and on this configuration
  every source says 68030: Gestalt `'mmu '` 3, CPUFlag 3, MMUType 4. VM would build 68030 tables and use
  `PMOVE`, `PFLUSH` and, *inference*, `PTEST` to find descriptors, which the shim does not do yet; until phase 2
  Virtual Memory must stay off (as docs/03 already says). The Memory control panel offers Virtual Memory only on
  a machine whose MMU VM supports, a 68030's included (*inference*: not checked; the first step of phase 2 is to
  open the Memory control panel on the test image and see).
- *A/UX 3.0.1 and 3.1* support the 68030 Macintoshes (the IIci among them) and the 68040 Quadras. A/UX Startup
  runs under Mac OS and knows the machine from Gestalt; its kernel has 68030 and 68040 MMU code (*inference*,
  without A/UX in hand). On "a IIci with a 68040" the answer is not in doubt, because there is no such Apple
  machine: what A/UX would see is the IIci it sees today, a 68030 with a 68030 MMU, which is a configuration it
  supports. A machine reporting IIci and 68040 is one A/UX was never written for (*inference*).
- *NetBSD/mac68k and Linux/m68k* support both and are told the machine and processor by their Mac OS booters
  (Booter, Penguin) from Gestalt; a 68030 is the path they take on a IIci.

**Recommendation: keep the 68030 identity and give it a complete 68030 PMMU.** That is the MMU the ROM programs,
the one Gestalt and Speedometer already report, the one A/UX supports on a IIci, and one model instead of two.
Emu68 goes on executing the 68040's instruction set; software that asks is told 68030 and will not use `MOVE16`
or `CINV`/`CPUSH` (if it does, they work). What the 68030 PMMU needs beyond the shim: `PTEST` with MMUSR and its
An result, `PLOAD`, every `PFLUSH` form (by address, by function code and mask), the U and M bits, write
protection as a fault, the S bit of long-format descriptors, SRE (A/UX and the BSDs, *inference*), TT0/TT1
matching, function-code lookup if anyone uses it, and the bus-error frames, short `$A` and long `$B`. The 68040 MMU
(`MOVEC` URP/SRP/TC/ITT0-1/DTT0-1, the 68040's `PFLUSH`/`PTEST`, frame `$7`) becomes useful only with a 68040
Macintosh's ROM, which is another project; the fault machinery below is the same for it, and `EMIT_RTE` under
`mac.mmu=full` already accepts frame `$7`.

**Consequences.** The FPU stays a 68882 to software (FSAVE's `$1F38` frames; `mac.fpu=exact` unchanged, docs/17);
switching to a 68040 identity would have meant 68040 FSAVE frames, `'fpu '` 3, and software expecting the 68040's
missing transcendental instructions to trap into an FPSP the IIci ROM does not have. Speedometer and Gestalt keep
saying "MC68030, MC68882, MC68030 MMU". If the owner wants software to see a 68040, that is a question for the
owner, not something this work should change.

## 4. Precise access faults in the JIT

**What a fault has to produce.** For a bus error the 68k needs, at the faulting instruction: its PC, the SR, the
registers as the frame's handler will see them, and, for the restart, the registers as they were before the
instruction began. In a translated block none of that is at hand:
- the PC is `w18` plus an offset the translator carries in `_pc_rel` and folds in only now and then
  (`EMIT_AdvancePC`, `EMIT_GetOffsetPC`, `EMIT_FlushPC`);
- the condition codes live in a temporary ARM register for most of a block (`RA_GetCC`/`RA_ModifyCC`, written back
  by `RA_FlushCC` at the block's end), the FPSR and FPCR too (`RA_GetFPSR`, `RA_GetFPCR`), and FPIAR is stored once
  at the end;
- an instruction with two memory accesses may have moved an address register before the second one faults: `MOVE
  (An)+,(Am)+` loads with a post-indexed `LDR` that moves An, then stores; `MOVEM -(An)` writes the base back with
  its first `STP` and then stores at offsets (`EMIT_MOVEM`); `CMPM`, `ADDX -(An),-(Am)` and their kin likewise.

What Emu68 does have: the 68k registers are in fixed ARM registers (`RegisterMapping.h`; D0-D7 in `w19-w26`, A0-A7
in `w13-w17` and `w27-w29`); the translator already records, per instruction, the ARM offset where its code
starts, its 68k address and the PC offset (`local_state` in `M68K_Translate`), and throws them away; the unit
structure even has an unused `mt_LocalState` field. A single `(An)+` or `-(An)` access is one ARM load or store with
writeback (`M68k_EA.c`), which an abort leaves undone: AArch64 synchronous aborts are precise and the writeback
does not happen (the architecture; seen under QEMU in the spike, to be confirmed on a Cortex-A53). MOVEM's
`(An)+` form adds to the base only after its last load. And the blocks are called from the JIT loops with `blr
x12` and return with `ret`, so the abort handler can leave a block by returning to the loop.

**The interpreter.** `M68k_Interpreter.cpp` with `M68k_INT_*.cpp` (about 7,700 lines) is a complete-looking
upstream interpreter, run when CACR's IE is clear and `JCCF_INTERPRETER` is set. It does not help: its memory
accesses are C++ code in the middle of a function, so a fault in it is no easier to undo than one in a block, and
it has none of the PSMAC paths (the 68030 `PMOVE`, the exact FPU).

**What was considered.**
1. *Continue, the 68030's way*: keep the ARM state of the faulting block and resume at the faulting ARM instruction
   after the handler's RTE. No rollback is needed, but the suspended state must live until the RTE, which under
   A/UX is after other processes have run (the frame sits on a sleeping process's kernel stack, any number of them),
   the block may be evicted from the cache meanwhile, and the handler may legitimately change the registers.
   Rejected.
2. *One instruction per block* under `mac.mmu=full`: precise by construction, and a block dispatch per instruction,
   several times slower. Kept as the fallback if (3) had failed.
3. *Re-execute in the interpreter*: no help, as above.
4. *Restart from a fault table and a register snapshot*: chosen, and built in the spike.

**The approach (built).** With `mac.mmu=full` the translator records, for every instruction (or fused group) it
emits, an entry in a table stored at the end of the unit, behind its code (`psmac_mmu_jit_insn`, called from
`M68K_Translate` beside the `local_state` record; `M68K_GetTranslationUnit` reserves the room and points
`mt_LocalState` at it): the ARM offset where the instruction's code starts, its 68k address, which temporary holds
modified condition codes at that point, if one does, and which address registers the instruction may change before
its last memory access. A small decoder (`snap_mask`) names those: `(An)+`/`-(An)` in either operand, the
`-(Ay),-(Ax)` register-memory forms, `MOVE16`, and the stack's users (BSR, JSR, PEA, LINK, UNLK, RTS, RTD, RTR,
RTE). Each one named is copied into a lane of `v23`/`v24` with one instruction before the instruction's own code
(those NEON registers are reserved from the compiler in every file and used only by the PowerPC JIT, which this
build never runs). Listing a register that is not changed costs that one instruction and nothing else.

On an abort, `SYSHandler` asks `psmac_mmu_fault` (`psmac_mmufault.c`, compiled like `vectors.c` with `x19-x29`
fixed, so it can read and write the 68k registers held there) first. If the guest translates, the logical page is
invalid in its tables, the faulting ARM instruction is inside the running block (x12) and x30 is the JIT loop's
return address (the word before it is `blr x12`), it finds the instruction's entry by binary search, rebuilds the
SR from the CC temporary or `v19`, puts the snapshot registers back, switches to the supervisor stack if the 68k
was in user mode (USP saved in `v21`), pushes a 68030 short bus-cycle fault frame (format `$A`, vector `$008`; the
special status word with DF, R/W, the size and the function code, stages C and B, the fault address), sets the
SR (S set, trace cleared), sets the PC to the bus-error vector and returns from the abort straight to the JIT loop.
The guest's handler runs as ordinary translated code. Its RTE, under `mac.mmu=full`, accepts formats `$7`, `$A` and
`$B` as well as 0 and 2 (`EMIT_RTE`), pops the frame and runs the instruction again from its start.

That is the 68040's restart model in the 68030's frame. A 68030 would continue the instruction from the frame's
internal state; no handler interprets that state, so the difference shows only to a handler that completes the
cycle itself and clears DF: for a write the restart writes the same value again, for a read the restart reads
memory instead of the handler's data input buffer (*inference*: neither VM nor A/UX is known to do that; to be
watched). An instruction whose own condition codes were computed before its faulting access recomputes them on the
restart, from the same inputs; ADDX's X survives because its loads come before its arithmetic (test 7 below).

**What the spike does not cover yet**, each found by reading and listed for phase 2:
- faults inside C helpers called from blocks: the exact FPU (`psmac_fpu_exec` reads and writes memory), `PMOVE`'s
  memory operand, `M68K_Exception`'s frame push. They are recognised (x30 is not the loop) and fall back to the old
  behaviour with a log line. The fix is to probe the pages before the helper touches them;
- instruction fetches: the translator reads the 68k code at translation time through the same mapping, so a block
  starting on, or running into, an invalid page reads `$BAD0BAD0`. Under `mac.mmu=full` a block must stop at an
  invalid page and a fetch must raise a fault with the instruction-stage bits;
- the FPSR and FPCR cached in temporaries (the table has room for them) and FPIAR;
- write-protection faults (today a write to a guest-WP page is emulated), the long frame `$B`, the data output
  buffer;
- code moved out of line by the translator (the exit stubs) is attributed to the block's last instruction; it does
  no memory access today;
- MOVEM with its base register in the list of a non-postincrement load is wrong in Emu68 with or without an MMU.

## 5. Address spaces and speed

**Today's map sets will not carry Virtual Memory.** The spike measured them: each `PFLUSH` after a changed
descriptor re-walked the whole of the tables into the 4 GiB `l2p` and remapped the set, 125 to 171 ms in QEMU
(`[PSMAC:MMU] PFLUSH remap #...`, 224 of them for 112 faults, about 130 ms on average). A handler that validates one
page must cost microseconds, not a tenth of a second. The abort itself is cheap: about 40 µs on average in QEMU over
112 faults, most of it the log lines of the first few.

**The design for phase 2: a soft TLB on the ARM page tables.**
- *Lazy population.* A set's ARM mapping starts empty; the first access to a page takes an ARM abort, the walker
  (the shim's, one page at a time instead of the whole table) finds its descriptor and maps that page. A fault on a
  page the guest's tables mark invalid becomes the bus error above. A switch to a new set costs nothing up front.
- *PFLUSH by its operands.* `PFLUSH (An)` unmaps that page (one PTE and one `TLBI VAE1`), `PFLUSH` by function code
  and mask the pages that match, `PFLUSHA` the whole set (the per-set bitmap of mapped pages already exists,
  `unmap_set`). `PLOAD` maps a page ahead. Nothing re-walks or hashes the whole table any more.
- *U and M bits.* A 68030 sets U in every descriptor its walk reads and M in the page descriptor on the first write;
  VM picks pages to evict by them and A/UX and the BSDs track dirty pages with M (*inference* for VM and A/UX). So a
  page is mapped read-only until it is written, the first write faults, the walker sets M in the guest's descriptor
  and remaps it writable; U is set by the walk that maps it. A guest that clears U or M must flush (it must on a real
  68030 too, its ATC caches them), and the flush unmaps the page so the next access sets them again.
- *Protection.* WP in any descriptor of the walk: mapped read-only, a write is a bus error. The S bit (long format
  only) and SRE need the user and supervisor views to differ: two ARM views of the same set, switched when the 68k's
  S bit changes (every exception and every RTE, MOVE/ANDI/ORI to SR). Mac OS uses neither (*inference*: the ROM's
  descriptors are short format and SRE is clear); A/UX and the BSDs set SRE (*inference*), so that switch is phase 3.
- *Context switches.* Today a switch writes four level-1 descriptors and flushes the whole TLB twice
  (`mmu_low4g_set`). With an address space per process (A/UX) the better shape is one level-1 table per set tagged
  with an ASID and a write of TTBR0, which needs no TLB flush; the JIT runs at EL0, so the switch is a trap to EL1
  (an SVC, a few hundred cycles). Cost of a context switch then: the trap, and the TLB refills of a cold process.
- *Do we watch the guest's tables?* No: a 68030's ATC holds stale translations until a flush, so correct software
  flushes after every edit. One risk is software that forgot a flush and got away with it because the 68030's ATC
  holds only 22 entries; an ARM mapping holds a stale page indefinitely. If that turns up, flushing everything every
  few milliseconds imitates the small ATC.

## 6. The JIT's code cache

Blocks are keyed by the 68k address they start at, the logical one (`mt_M68kAddress`, hashed in `ICache`); the
code is read through the 68k's mapping; a block is re-checked by checksum after the Mac clears its instruction
cache (`psmac_icache_epoch`, `M68K_VerifyUnitCRC32`), and the ROM is exempt by asking the shim where a block's
address lands (`psmac_mmu_l2p`).
- *One address space (Mac OS with VM):* logical keys are right. A page of code that VM pages out and in again comes
  back at the same logical address; Mac OS clears the caches after loading code, which bumps the epoch, and
  *inference*: VM does so after paging code in, as a real 68030 needs it to. Under `mac.mmu=full`, a flush that
  changes the physical frame behind a logical page should also drop the blocks whose range touches that page
  (the LRU already has `LRU_InvalidateByM68kAddress`), and blocks should end at a page boundary so that a page's
  blocks are its own and a block never runs from a valid page into an invalid one.
- *Several address spaces (A/UX):* the same logical address holds different code in different processes. The key
  must say which space: `mt_Key` is the pair (epoch, 68k address), and making the epoch half carry a space tag (the
  set's number, or the CRP's root) keeps the lookup as it is while separating the spaces. Shared text at the same
  logical address is then translated once per process; keying by physical address would share it but costs a
  translation at every block entry and breaks on the logical constants a block contains.
- *Self-modifying code* is unchanged by any of this (the PSMAC `smc_stop` rule, the checksum).

## 7. The spike

Branch `mmu-experimental`. What it adds, all of it active only with `mac.mmu=full`:
- `emu68/src/psmac/psmac_mmufault.c` (new): the fault table, the snapshot decoder, and `psmac_mmu_fault`, the
  delivery (section 4);
- `emu68/src/M68k_Translator.c` (two `#ifdef PSMAC` islands): the per-instruction call and the table at the end of
  the unit;
- `emu68/src/aarch64/vectors.c` (one island in `SYSHandler`): asks `psmac_mmu_fault` before emulating the access;
- `emu68/src/M68k_LINE4.c` (one island in `EMIT_RTE`): formats `$7`, `$A`, `$B`;
- `emu68/src/psmac/psmac_mmu.c`: the option (`mac.mmu=full`, and `mac.mmu=nosnap`, the diagnostic below), a
  pointer to Mac RAM behind a logical address for building the frame, the remap's timing;
- `mac/mmutest.s` (new, `make -C mac mmutest`): the test, a bare 68k "ROM" of 3 KiB for the solo board, so it needs
  no Apple software: the kernel loads any ROM image and repeats it over 512 KiB (`psmac_bus.c`).

**The test.** The program builds its own 68030 tables (TC `$80C04880`: 4 KiB pages, TIA/TIB/TIC 4/8/8, everything
one to one, a level-C table for the megabyte at `$00100000` where the test pages are), loads CRP and TC, and runs
eleven cases. Each writes its data while the page is valid, marks the page invalid in table C and flushes, then
runs one instruction that touches it. Its bus-error handler does what VM's would, in miniature: reads the fault
address from the frame, marks that page valid, `PFLUSHA`, counts, `RTE`. Each case checks the data, the registers,
the number of faults, and where it matters the frame (format `$A008`, the PC of the faulting instruction, the SSW):
1. `MOVE.L (A0),D2`, a read; 2. `MOVE.L D1,(A0)`, a write;
3. `MOVE.L (A0)+,(A1)+`, the store faulting after the load moved A0;
4. `MOVEM.L (A0)+,D0-D5/A2-A3`, reading across from a valid into an invalid page;
5. `MOVEM.L D0-D5/A2-A3,-(A1)`, its third store faulting after the first moved A1;
6. `ADDQ.L #1,(A0)+`, a read-modify-write;
7. `ADDX.L -(A0),-(A1)` with X clear, then with X set (X must survive the restart);
8. `CMPM.L (A0)+,(A1)+`; 9. a read in user mode (frame on the supervisor stack, USP kept, SSW's function code 1);
10. `TST.L D3` then `MOVEA.L (A0),A2` and `SEQ`: condition codes set just before the faulting instruction and
    still in the block's temporary, with Z set and clear; 11. a hundred faults in a row over 32 pages.

Run with `ROM=mac/build/mmutest.rom tools/run-solo.sh 45 mac.mmu=full` (the declaration ROM and disk image are
not read):
```
[PSMAC:MMU] mac.mmu=full: guest page faults become 68030 bus errors (experimental, docs/23-mmu.md)
[PSMAC:MMU] bus error #1: PC 40800108 read 00101010 (SSW 0145, supervisor) -> handler 408007d6; ...
[PSMAC:MMU] bus error #3: PC 40800204 write 00104040 (SSW 0105, supervisor) -> handler 408007d6; ...
[MAC] MMU test: 50 passed, 0 failed, 112 bus errors taken
```
Bus error #3 is case 3's store: the fault came after the load had moved A0, and A0 was right afterwards.

**The negative control.** `mac.mmu=nosnap` delivers the same faults without the snapshot. Cases 3, 5 and 7 fail,
exactly the ones whose restart needs it, with the address registers moved twice and the data in the wrong place:
```
[MAC] MMU test: 40 passed, 10 failed, 112 bus errors taken
[MAC] t3=00000000 t3a0=00103108 t5a1=00107FD0 t5m4=00000000 t5m7=00000000 t7=00000007 t7a0=0010A00C t7x=00000008
```

**Without the option**, on the branch's kernel and on main's, the same program gets no bus errors: every read of an
invalid page returns `$BAD0BAD0` and the writes vanish, as before (`16 passed, 34 failed, 0 bus errors taken`).

**System 7.1 under `mac.mmu=full`.** The stock IIci ROM and the System 7.1 test image on the solo board, with the
option: the Finder at 42.0 s and 40.4 s (two boots, `[BOOT] ... the Finder is running`), the
Aurora68 drivers and startup items as usual, and no bus error delivered and none refused: the ROM's 32-bit tables map
every page anything touches, so nothing changed for it. A real IIci would take a bus error on an access to an
invalid page too, so a stock System that gets one under the option has found a difference worth knowing about.

**Conclusion.** The hard part works: a fault in the middle of a translated block becomes a correct 68030 bus error,
and the instruction restarts, including the cases where the instruction had already moved a register, for the
price of one NEON move per such register in instructions that might, and nothing elsewhere. The approach stands.
What remains is mostly the shim (section 5) and the corners listed at the end of section 4.

## 8. The stock path

Without `mac.mmu=full`: the translator tests the flag once per instruction it translates and once per unit, and
emits exactly the code it did before; `EMIT_RTE` tests it once at translation; `SYSHandler` tests it once per data
abort, before the emulation it always did (an abort costs microseconds; the test is one load). Nothing is added to
translated code. The benchmark loop at the end of `mmutest.s` (a copy loop, arithmetic, `MOVEM` and `BSR`/`RTS`),
millions of 68k instructions a second from the heartbeat's JIT line, solo board in QEMU, 5-second samples after
the first:

| kernel | option | run | samples (MIPS) | median |
|---|---|---|---|---|
| main (`5bba1fd`) | none | 1 | 305 293 289 280 328 302 274 314 297 292 | 295 |
| main | none | 2 | 282 312 290 312 304 314 317 325 332 305 | 312 |
| `mmu-experimental` | none | 1 | 299 324 327 327 304 306 323 304 261 266 | 305 |
| `mmu-experimental` | none | 2 | 312 314 296 323 341 315 328 319 295 320 | 317 |
| `mmu-experimental` | `mac.mmu=full` | 1 | 396 393 357 355 | 375 |
| `mmu-experimental` | `mac.mmu=full` | 2 | 366 372 361 376 376 | 372 |

The two configurations without the option were run alternately, and an earlier pair gave main 326 to 341 and
the branch 337 to 374. The runs with the option are faster than those without, which they cannot really be (they
add a NEON move before every copy, push and pop of the loop): their samples come later in the run, after
half a minute of tests, and QEMU's speed drifts over a run. The figures show that the difference between main
and the branch without the option is inside QEMU's spread; they cannot show anything smaller.

The other check, System 7.1 to the Finder on the solo board, alternating the kernels (seconds from power-on to
"the Finder is running"): main 38.8, 41.3, 40.9, 37.1; the branch without the option 43.7, 37.2, 38.6; with it
42.0, 40.4. The same spread, the same conclusion.

## 9. The plan

**Phase 2: System 7's Virtual Memory** (the solo testbed first, then the MAME full stack, which stays the
reference). Each step adds cases to `mac/mmutest.s`, which runs in under a minute and needs no Apple software.
1. Look first: open the Memory control panel on the test image with the option and see whether VM is offered;
   watch which MMU instructions VM's code issues (a log of every `PMOVE`/`PTEST`/`PFLUSH` with its PC).
2. `PTEST` (levels, the An result, MMUSR's B/L/S/W/I/M/T/N bits), `PLOAD`, the `PFLUSH` forms, TT0/TT1, long
   descriptors with S, limits, page sizes 4 to 32 KiB: the shim's walker, one address at a time.
3. The soft TLB of section 5: lazy population, per-page flushes, U and M by faults, write-protect faults. Test: U and
   M as a 68030 sets them, a flush of one page in microseconds.
4. The corners: instruction-fetch faults (blocks end at page boundaries; a fault block raises the frame with the
   instruction bits), probes before the C helpers touch memory, FPSR/FPCR in the fault table, frame `$B` where the
   68030 would use it.
5. VM on: System 7.1 with `mac.ram=8` and 32 MB of virtual memory, the Finder, Speedometer, and a new test
   application that allocates more than the physical memory and checks a pattern in all of it; then the same in
   the MAME full stack.
6. On the Pi 3B (the solo board, then the SE): the cost of an abort on a Cortex-A53, and the behaviour of a faulting
   `LDP`/`STP` there.

Size: about 1,500 to 2,000 lines of C (most of it the shim's rewrite into a soft TLB) and 500 of test, plus
whatever VM turns up; three or four sessions of this one's size, the last of them mostly in the testbed.

**Phase 3: A/UX 3.x.** A/UX Startup is a Mac OS application: it reads the kernel from the A/UX partition, then
takes the machine, the MMU with its own tables included, and jumps into the kernel, which installs its own vectors
and drives the hardware itself (*inference*, from A/UX's documentation; to be watched in the testbed once A/UX is
here). What it adds over phase 2: SRE and the supervisor root (two views switched at S changes), `MOVES` with
SFC/DFC across the two spaces, function codes in TT0/TT1, an address space per process (ASID-tagged sets, TTBR0
switches), the code cache keyed by space (section 6), and A/UX's own drivers meeting the hardware: in the MAME full
stack the real IIci chips, on the solo board the kernel's own stand-ins (`psmac_board_solo.c`), which were written
for the Mac OS's use of them and are the first risk. Card P and the SE on real hardware differ only in what the
board answers (docs/22): the MMU is all in the Pi, and the costs are the A53's, not QEMU's.

**Phase 4: NetBSD and Linux**, booted by their Mac OS booters, on the same 68030 machinery; they add little beyond
A/UX's needs (*inference*).

**Risks.** VM or A/UX relying on the 68030's continuation (DF cleared) rather than a rerun; software that edits
descriptors without flushing; the cost of S-bit switches under A/UX on the A53; the solo board's chip stand-ins
under A/UX; the 4 KiB ARM page against 68030 pages below 4 KiB (none of the targets uses them, *inference*).

## 10. What could not be settled

- Which MMU instructions System 7.1's VM issues, its page size on a IIci, and what the Memory control panel checks:
  read from the code, not run; phase 2's first step.
- A/UX's choice of MMU code and its use of SRE: A/UX is not here yet.
- A faulting `LDP`/`STP` and a pre- or post-indexed load on a real Cortex-A53: the architecture says the writeback
  does not happen, QEMU agrees; not yet seen on the chip.
- The stock path's cost below QEMU's ten per cent noise: argued from the code (nothing is added to translated code),
  not measured finer.
