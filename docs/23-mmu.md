<!-- SPDX-License-Identifier: MPL-2.0 -->
# 23. A real MMU for the Macintosh: a design note, and the spike that tested its hard part

*25 November 2026.* The owner asked for "that MMU stuff": a memory management unit the Macintosh's software can
really use, so that, in order, System 7's Virtual Memory works, A/UX 3.x boots (the owner will supply A/UX), and
later NetBSD and Linux for the m68k. This is phase 1: what the code does today, what it shows, what we conclude,
and a spike on branch `mmu-experimental` that tried the riskiest point, delivering a precise page fault from
translated code and restarting the instruction. All of it is experimental and behind a new kernel option,
`mac.mmu=full`; without it the kernel behaves exactly as before (section 8 measures that). Where something comes
from the code it names the file and the function; where it is our reading or a guess, it says *inference*.
Section 11 is phase 2 (25 and 26 November): the complete 68030 PMMU on a soft TLB, and System 7.1's Virtual Memory
running on it, on the solo board and in the MAME full stack.

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

## 11. Phase 2: System 7's Virtual Memory (25 and 26 November 2026)

The owner approved phases 2 and 3 to run overnight. This section is phase 2's record: what was looked at first, what
VM turned out to need and to issue, what was built, and what it showed. Everything is still behind `mac.mmu=full` on
branch `mmu-experimental`; section 11.7 checks the stock path.

### 11.1 Looking first

**The Memory control panel offers Virtual Memory.** System 7.1 on the solo board with `mac.mmu=full` and
`mac.ram=8`: the Memory control panel (v7.3) shows the Virtual Memory group with On and Off, the disk to use ("Mac
Volume", 68M available on disk, 8M of built-in memory) and, once On is chosen, "After restart" with arrows (16M
offered first; sixteen clicks made it 32M). Its test is in the control panel's code (`cdev` -4064, the routine at
offset `$2F42`): Gestalt `'proc'`, `'mmu '` and `'romv'`; VM is offered when the processor is a 68020 with a 68851, or
a 68030, or the MMU is a 68040's, and the ROM version's low byte is at least `$78` and not `$7A` (the IIci's ROM is
`$067C`). So it would be offered without the option too: the kernel has said 68030 since docs/08 item 41.

**Where VM keeps its settings.** Not in parameter RAM: the control panel writes the System file's `'vm  '` resource
0 (37 bytes; it was all zeros, off; after the panel: version 1, `$02000000` bytes, the volume "Mac Volume") and
creates `VM Storage` in that volume's root, a file of the virtual memory's size (32 MB, type `ZSYS`, creator
`MACS`), so a copy of the disk image carries VM's setting from run to run. The tests below run on a copy of the System
7.1 image in the scratch area; the original under `images/` is never written.

**Where VM is.** `'ptch'` 42 of the System file (27,896 bytes): it installs Gestalt `'vm  '`, `'vmcl'`, `'pgsz'` and
`'vmbs'`, its own vector table (VBR moved to a copy whose bus-error entry is VM's), and a privilege-violation handler,
because **VM runs the Macintosh in user mode**: every privileged instruction a program or the System executes (`MOVE
to/from SR`, `RTE`, `RESET`, `MOVEC`, and the MMU instructions themselves) traps, and the handler emulates it
(offsets `$4372`-`$4560`: it compares the faulting opcode with `$4E73`, `$4E70`, `$0E79`, `$F000` and `$F400`). The
MMU instructions in it, all of them through a table of routines that picks the 68030 or the 68040 form by `MMUType`
(`$CB1`, 4 or 5):

| offset | instruction | what for |
|---|---|---|
| `$35F4`, `$451C` | `PTESTR Dn,(A0),#7,A1` with the FC in D0 | find a page's descriptor (A1) and its state (MMUSR) |
| `$35F8`, `$4544` | `PMOVE MMUSR,(SP)` | read PTEST's answer |
| `$3618`, `$361E` | `PLOADR Dn,(A0)`, `PLOADW Dn,(A0)` | load a page's translation after paging it in |
| `$3624`, `$4560` | `PFLUSHA` | after editing descriptors |
| `$3638`-`$3640` | `PMOVE (A1),TC`; `PMOVE (A0),CRP`; `PMOVE 8(A0),TC` | install its own tables |
| `$43BC` | `PMOVE (SP),TC` (zero) | the emulated `RESET` turns translation off |

Its 68030 bus-error handler (`$2E4`, reached from the vector at `$2D74`) decodes the frame (`$A5A`): with DF set the
fault address is the frame's `+16` and the function code the SSW's; without DF it is an instruction fetch, whose
address it takes from the stage B address (`+36`) of a format `$B` frame, less 2 if FB is clear, or from the PC (`+2`)
plus 4 or 2 in a format `$A` frame. If the page is not resident it pages it in; if it is, it runs PTEST and gives up
(the original handler: a system error) on B or L, calls a write-protect path on W without I, and otherwise returns
with RTE to retry. It also uses unused bits of the page descriptors (bit 8 of a 4 KiB page's address field) as its own
flags, and clears M itself after writing a page out.

**The first boot with VM on, on phase 1's kernel** (`runs/look1`, kept out of the repository): VM installs its tables,
TC `$80C07760` (4 KiB pages, IS 0, TIA 7, TIB 7, TIC 6) and CRP `$80000002:$00035C00` (short descriptors, the lower
limit flag with limit 0), probes the top of its logical space and takes its first bus errors correctly (`read
01000000`, `read 00FFF000`), then goes wrong: PTEST and PLOAD were skipped by the shim and MMUSR kept a stale value,
and eleven thousand faults later the 68k was jumping through `$BAD0BAD0`. That fixed the order of work.

### 11.2 The 68030 PMMU (`psmac_mmu.c`, under the option)

- `pwalk()`: the 68030's table search for one address (MC68030UM 9.5): CRP, or SRP for supervisor accesses when TC's
  SRE is set; the function-code level when FCL is set; up to four levels of short or long descriptors; limits (upper
  and lower) from the root pointer and long table descriptors; early termination at any level; indirect descriptors
  where a page descriptor belongs; WP gathered along the way, S from long descriptors; U set in every descriptor as it
  is fetched and M in the page descriptor on a write, as a real access's search does. A descriptor outside RAM is a
  bus error during the search (MMUSR B).
- TT0 and TT1: base and mask, the function-code base and mask, R/W and RWM; a match is one to one with no search.
- PTEST, levels 1 to 7: the search without changing U or M, MMUSR's B, L, S, W, I, M and N, and A*n* gets the address
  of the last descriptor fetched. Level 0 (the ATC alone): T for a TT match, else what the soft TLB holds, else I.
- PLOADR and PLOADW: the search as the access would make it (U; M for PLOADW) and the page mapped.
- PFLUSHA, PFLUSH by function code and mask, PFLUSH by function code, mask and address. The soft TLB's pages carry no
  function code, so the first two drop (or keep, below) the whole set and the third the named page: flushing more
  than named is always correct.
- PMOVE to and from TC, SRP, CRP, TT0, TT1 and MMUSR, with any effective address (the control modes computed by the
  block, `(An)+`, `-(An)` and register direct in C); without FD a load flushes, as the 030's does. CRP's deferred
  switch (section 2) is kept.
- Page sizes: 256 bytes to 32 KiB, as TC allows. Pages of 4 KiB and more are mapped; a page below 4 KiB cannot be an
  ARM page, so every access to it is emulated through the walk (slow but exact; nothing seen uses them).
- SRP and SRE are honoured by the walk and by PTEST, but the ARM mapping is one view for both modes (phase 3).

### 11.3 The soft TLB

A set (one per TC/CRP, as before) starts with an empty ARM mapping. The first access to a page takes an ARM data
abort; `psmac_mmu_access` walks the guest's tables and maps the chunk the same descriptor covers (up to 32 KiB) with
4 KiB ARM pages (`mmu_low4g_page`: one PTE, a single-address TLB invalidate when an entry is replaced, none when one is
added), read-only while M is clear, writable once it is set; the access then runs again. This works wherever the
access came from, a block, a C helper, the translator or the code cache's checksum, because a retry needs no 68k state.
A write to a page mapped read-only for M sets M and remaps it. A write to a WP page, a user access to an S page and
any access to an invalid page are bus errors (11.4). Pages that are not memory the ARM can map (the slots' I/O), and
writes to the ROM and to the RBV frame buffer area below `$50000` (docs/10), are emulated as before; their translation
is cached (512 entries) for `psmac_mmu_l2p`, which the bus emulation, the mailbox and the diagnostics call, some of them
from other cores, and which otherwise walks without changing anything.

A PFLUSH no longer re-walks everything into a 4 GiB table. It revalidates what it names: a mapped page is kept if its
descriptor still gives the same frame, U is still set (a guest that clears U wants to see the next access), and, if it
is mapped writable, M is still set and WP clear; anything else is unmapped. A page whose frame changed may hold other
code, so the code cache is told (`psmac_icache_epoch`: its blocks are checked by checksum on their next entry). A
switch to a cached set compares a hash of its tables with the one taken when it was left (U and M aside: they are
not in the hash) and flushes it if they differ, since a real 030 flushes its ATC at the PMOVE.

### 11.4 Faults

- *Data faults in blocks* are delivered as in phase 1, with two changes: a read gets the 68030's long frame (format
  `$B`, 92 bytes: the stage B address at `+36`, the data input buffer at `+44`) and a write the short frame (`$A`),
  which is how the 030 chooses between them (a fault in the middle of an instruction against one at its end); and the
  FPSR and FPCR a block holds in temporaries (`RA_PsmacFPSRTemp`, `RA_PsmacFPCRTemp`, recorded in the fault table
  beside the condition codes) are stored before the block is abandoned.
- *Instruction fetches.* With the option a block ends at a 4 KiB page boundary, so a page's blocks are its own, and
  before each instruction the translator asks the walk whether its words can be fetched (`psmac_mmu_jit_fetch`): if
  not, the block ends before it, or, as the block's first instruction, it becomes a call that delivers a long frame
  with FB and RB set and the faulting word's address at `+36` (where VM's decoder reads it), its PC the instruction's.
  An instruction that runs into the next page is caught by decoding its length from what can be read
  (`M68K_GetINSNLength` on a copy).
- *C helpers.* PMOVE's operand is checked before it is touched and its bus error delivered from the helper's register
  save area. The exact FPU (the default since docs/17) changes registers and its state as it goes, so `psmac_fpu_exec`
  is wrapped in a guard: a copy of both is kept, an abort on a refused page inside it returns to a landing function
  that jumps back into the guard (`__builtin_setjmp`/`__builtin_longjmp`), the copy is put back and the bus error is
  delivered from the instruction's start; `EMIT_FPU_exact` emits, with the option only, the three instructions that
  take the new SR and leave the block. `M68K_Exception`'s own frame push is not guarded: the supervisor stack is always
  resident under VM (it holds it), and a real 030 would halt on a double bus fault there anyway.
- *Privilege violations.* Under VM they are the System's ordinary work (thousands a second, 11.6), so with the option
  those from user mode are counted (`psmac_priv_user`) instead of going into the crash record, whose per-exception
  copy of code and stack cost a walk per longword.

### 11.5 Virtual Memory on, on the solo board

System 7.1 (the test image's copy), `mac.ram=8`, `mac.mmu=full`, VM on with 32 MB on "Mac Volume":

- **The Finder** at 44 to 47 s from power-on (47.2 s in `runs/vm1`; 41.0 s to the first INIT's log line), against 39 to
  42 s with VM off. About This Macintosh: "Built-in Memory: 8,192K", "Total Memory: 32,768K", "Largest Unused Block:
  31,426K", "32,768K used as RAM on Mac Volume" (`docs/img/mmu-vm-about-32mb.png`). VM's own start: TC `$80C07760`,
  CRP `$80000002:$00035C00`, a new set mapped on demand in 4.6 ms; its first bus errors are its probes of the top of
  logical memory (`read 01000000`, `read 00FFF000`, `read 00FFE000`), each answered by `PFLUSHA`, `PFLUSHA`, `PLOADR`.
- **The first try of the test failed, and showed why.** The first run that touched more than the physical memory
  died at VM's first eviction: one write (`PC 00597E8C write 00C29000`) faulted over a thousand times in a row, VM
  taking its "resident" path each time (PTEST, then RTE to retry), until the application ran off to `$0000000C`
  (system error 3). VM keeps a flag in bit 8 of its 4 KiB page descriptors (`ptch` 42 at `$1ED8`, `bset #0,2(a4)`),
  which on a 68030 is an unused bit of the address field; the walk had added it to the physical address. The walk now
  takes only the address bits above the page (or the early-terminated span), as the chip does.
- **Aurora68 VM Test** (`mac/vmtest.s`, `make -C mac vmtest`, a 26 MB partition, in Startup Items): Gestalt
  `'vm  '` 1, `'ram '` 8192 K, `'lram'` 32768 K; 24 blocks of 1 MB at `$0059831C`-`$01D9842F`; then, in ticks (QEMU
  on the solo board, `runs/vm7`):

  | pass | ticks | mismatches | sum (written `$47600000`) |
  |---|---|---|---|
  | 1, write every longword | 11,536 (192 s) | 0 | `$47600000` |
  | 2, read forwards | 15,535 (259 s) | 0 | `$47600000` |
  | 3, read backwards | 10,253 (171 s) | 0 | `$47600000` |

  (`docs/img/mmu-vm-test-24mb.png`). Repeated on the final kernel, after the two fixes of 11.8 (`runs/vm8`): 11,625,
  15,319 and 10,372 ticks, 0 mismatches, the same sums, 16,239 bus errors and 33,410 PFLUSHA. The program holds its own code segment (`HoldMemory`) because the kernel reads its
  log lines from it through the card's INFO service, as a DMA would; without that, a line whose page VM had moved out
  was refused by the mailbox (`PSMB_E_BADADDR`) and lost, which is what a real Mac's DMA would have done as well.
- **The kernel's counts at the end of it** (the heartbeat's two lines): 16,239 bus errors delivered (all data faults
  from blocks; none undelivered), 102,335 lazy maps, 94,918 privilege violations from user mode; 7.6 million walks,
  118,574 maps of 118,979 pages, 179 first writes (M); PFLUSH: 33,409 PFLUSHA (two for each page VM brings in),
  6,530,192 mapped pages kept by the revalidation and 118,248 dropped, 28.8 s in all (860 µs each); PLOAD 16,239 (one
  per page brought in); PTEST 0 (VM's handler never took its "resident" path once the flag bit was right); 2 set
  switches; 28.2 million emulated accesses.
- **Where the time goes.** Paging ran at about 200 KB/s each way. The emulated accesses are the solo board's SCSI chip
  (its pseudo-DMA data register at `$50F06000`, one ARM abort per transfer; the kernel's stand-in, the same for every
  disk access with or without the option) and the writes to the area below `$50000` (the RBV frame buffer's, docs/10:
  read-only to the 68k on every board, low memory's globals among them), which the idle Finder makes at the same
  rate without VM (2 to 3 MIPS idle in this testbed either way, `runs/look1` against `runs/vm2`). The PFLUSHAs'
  revalidation is 7 % of the test's time. An abort that ends in a bus error costs 25 to 32 µs in QEMU on average.

### 11.6 The test ROM

`mac/mmutest.s` gained six cases (12 to 17), run after phase 1's eleven and skipped when no bus error has been taken
by then (without the option), so that the stock kernel still reaches the benchmark:

12. PTEST at level 7: a valid page gives MMUSR `$0003` (three levels) and A3 the level C descriptor's address; an
    invalid one `$0403`; a write-protected, modified one `$0A03`; level 1 gives `$0001` and the level A descriptor.
13. U set by a read and M by the first write, in the guest's descriptor; set again after the guest clears them and
    flushes; PLOADR sets U, PLOADW U and M.
14. A read of a WP page works; a write is a bus error (SSW `$0105`, the instruction's PC), and the handler's
    revalidation lets it complete.
15. `PFLUSH #0,#0,(A0)` and `PFLUSH #5,#7` drop a page whose descriptor went invalid: the next access faults.
16. TT0 over a page the tables mark invalid: no fault, PTEST level 0 says T; TT0 off: it faults.
17. `PMOVE 0x10(A0),TT1` with the operand's page invalid: the bus error comes from the PMOVE (its PC, the operand's
    address), which then runs again.

Case 1 now expects format `$B` (a read) and case 2 `$A` (a write). `ROM=mac/build/mmutest.rom tools/run-solo.sh 45
mac.mmu=full`: **`MMU test: 82 passed, 0 failed, 117 bus errors taken`**. The first version of the logger lost the
summary line: the tests now end in under two seconds, before the kernel's mailbox service is up (phase 1's
remaps of 130 ms each had hidden that), so it waits for the answer.

### 11.7 The stock path

Without the option nothing new is emitted: every addition to the translator (`EMIT_lineF`, `EMIT_FPU_exact`, the
fetch check in `M68K_Translate`) tests `psmac_mmu_full` at translation and emits the old code; the C paths
(`M68K_Exception`, `psmac_mmu_l2p`, `SYSHandler`) test it once per call. The benchmark loop of `mmutest`, solo board,
5-second samples after the first two, main's kernel (`5bba1fd`) and the branch's alternated:

| kernel | option | test result | median MIPS (13 samples) |
|---|---|---|---|
| main | none | 16 passed, 35 failed, 0 bus errors | 344 |
| branch | none | 16 passed, 35 failed, 0 bus errors | 343 |
| main | none | the same | 349 |
| branch | none | the same | 345 |
| branch | `mac.mmu=full` | 82 passed, 0 failed, 117 bus errors | 348 |

The same results, and the speeds inside QEMU's spread, as in phase 1.

System 7.1 to the Finder on the solo board (a fresh copy of the test image, VM off, `mac.ram=8`, seconds from power-on
to "the Finder is running"), the kernels alternated:

| kernel | option | run 1 | run 2 |
|---|---|---|---|
| main (`5bba1fd`) | none | 36.9 | 41.2 |
| branch | none | 42.6 | 38.8 |
| branch | `mac.mmu=full` | 47.0 | 47.6 |

Without the option the branch is inside main's spread. With it, and no VM, the boot takes 5 to 8 s more than phase
1's 40 to 42: no bus error, 79 lazy maps of 628 pages, but 287,000 walks for 2.3 million emulated accesses whose
page was not in the 512-entry cache (the I/O and the frame-buffer area below `$50000`). A bigger cache, or keeping
the emulated pages' translations per set, would win most of that back; it is the option's cost, not the stock path's.

### 11.8 The MAME full stack

The reference environment: MAME's IIci (`maciici_ps`) with the kernel under QEMU, the patched ROM
(`rom/patched/MacIIci_psmac.rom`), `mac.ram=8`, `mac.mmu=full`, the same image (converted to an uncompressed CHD), and
a private copy of MAME's PRAM (`$8A` = 5, 32-bit) and configuration, so that the shared ones were not touched. Two
faults the solo board had not shown came first:

1. **The ROM ran off after its first TC load** (heartbeat PC `$00004384` for minutes, then `$55555555`). The IIci ROM
   switches with `pmove (a3),tc; jmp (a5)`, run twice (`$40804376`-`$40804390`): the first pass only puts the two
   instructions in the 030's instruction cache, so that the JMP still runs after the PMOVE has taken its address
   away. Translated code is this emulation's instruction cache, and phase 2's first emission ended the block after
   every MMU instruction, so the JMP was fetched again through the new tables. The block now goes on after the
   instruction, as the stock shim's always did, and leaves through a local exit only on a privilege violation or an
   operand's bus error. A control run without the option on the same setup booted normally, which is what pointed at
   the new code.
2. **Low memory landed in the frame buffer.** With RBV video, the ROM's tables map logical low memory through an
   early-terminated descriptor whose base lies above the frame buffer and is not aligned to its span. The fix for VM's
   flag bit (11.5) had made the walk concatenate the span's offset; a 68030 adds it, and ignores only the bits below
   the page size. The walk now does exactly that (the solo board has no RBV video and maps logical 0 to physical 0,
   which is why it did not notice).

Then, in `runs/fs5`: VM installs its tables (TC `$80C07760`, CRP `$80000002:$00085C00`), the same first bus errors as
on the solo board, **the Finder at 253 s** from power-on (the full stack without VM takes about four minutes, CLAUDE.md),
and Aurora68 VM Test starts: Gestalt `'vm  '` 1, `'lram'` 32768 K, 24 blocks at `$00543880`-`$01D43993`, and **pass
1 complete: 52,527 ticks, 0 mismatches, sum `$4CE00000` as written** (MAME's own screen,
`docs/img/mmu-vm-mame-pass1.png`), with 6,829 bus errors delivered, none undelivered, 14,589 PFLUSHA and 6,828
PLOADs. The read passes did not finish in the run's 25 minutes: every page goes through MAME's SCSI chip over the bridge.

### 11.9 What is left for phase 3 (A/UX)

- **SRE, the supervisor root, and S.** The walk and PTEST honour them, and a set's key includes SRP when SRE is set,
  but the ARM mapping is one view for both modes: a page mapped during a supervisor access stays reachable from user
  mode. Phase 3 needs two views per set (user and supervisor ARM tables) switched wherever the 68k's S bit changes:
  exceptions, RTE, MOVE/ANDI/ORI/EORI to SR, and `M68K_Exception`'s stack switch.
- **MOVES with SFC/DFC**, which address the other view; the PMMU instructions already take SFC/DFC as function codes.
- **Function codes in the soft TLB.** PFLUSH by function code and mask flushes everything (correct, more than needed);
  TT matches with the access's function code, but a page mapped through a TT stays mapped for the other codes.
- **An address space per process.** Today's six sets, each four level-1 descriptors swapped with two full TLB flushes,
  will not carry A/UX's processes: one level-1 table per set tagged with an ASID and a TTBR0 write (section 5), and
  more sets than six with an eviction that does not unmap page by page.
- **The code cache keyed by space** (section 6): blocks are keyed by logical address only; under A/UX the same address
  holds different code in different processes. With blocks now confined to one 4 KiB page, keying by (space, page) is
  a smaller change than it was.
- **`M68K_Exception`'s frame push** is not guarded (11.4); under A/UX the kernel's stacks are its own business.
- **Pages below 4 KiB** are emulated access by access; A/UX uses 4 KiB pages on a 68030 (*inference*).

### 11.10 What is not settled

- The MAME full stack (11.8) is the reference; its numbers are the bridge's, not the MMU's.
- Real hardware: an abort on a Cortex-A53 (the soft TLB takes one per page per flush; VM's two PFLUSHAs per page-in
  revalidate every mapped page, 860 µs each in QEMU) and the 4 KiB ARM pages instead of the stock path's 2 MiB blocks
  (TLB reach) are unmeasured. If the revalidation is too slow there, a per-set generation count of pages touched since
  the last flush would cut it.
- The 68030's continuation model (DF cleared by a handler that completes the cycle itself) is still a restart here;
  VM never clears DF (its handlers return with RTE and let the access run again).
- A guest that edits descriptors without flushing keeps its old mapping as long as the soft TLB does, which can be far
  longer than the 030's 22-entry ATC; VM flushes after every edit, so nothing showed it.
- 24-bit addressing with VM was not tried (the test image runs in 32-bit mode; PRAM `$8A` = 5).
