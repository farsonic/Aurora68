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
running on it, on the solo board and in the MAME full stack. Section 12 is phase 3 (26 November): address spaces with
ASIDs, separate user and supervisor views, MOVES, the code cache keyed by space, the double bus fault: what A/UX needs
before it can be tried. Section 13 (27 November) is the first try on the real SE, which stopped before the 68k
started, and its fix. Section 14 (26 November) is A/UX on the real SE as far as "Checking...", section 15 (26 November) A/UX
in the MAME SE as far as init, the ten faults on the way and their fixes. Sections 16 to 20 (26 and 27 November) are the
real SE's own faults on the way into A/UX's kernel, and section 21 (27 November) the two faults that kept A/UX's user
programs from running in the MAME SE; with them mended A/UX 3.1.1 reaches its Finder there. Sections 1 to 10 are phase 1's record and are kept as written; where a later phase found a
statement wrong or overtook it, a bracketed note says so.

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

**The faults.** The JIT runs at EL0. [Phase 3: wrong. Translated code runs at EL1: `MainLoop` is called from the
kernel's boot path in `start.c` with no drop to EL0, and the abort dumps show EL1 (ESR class `$25`, a data abort from
the same level). Section 12 relies on it: a block writes TTBR0_EL1 itself.] A translated load or store to a page that is not mapped takes an ARM data
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

**What the spike does not cover yet**, each found by reading and listed for phase 2 [phase 2 did the first four,
section 11.4; `M68K_Exception`'s frame push was left for phase 3, section 12.5]:
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
  [Phase 3 built it (section 12), without the trap: the JIT runs at EL1 and writes TTBR0 directly.]
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
- [Phase 3 (section 12.4): blocks carry the address space they were translated in, the TTBR0 value, and match only
  there; the first form below, a space tag beside the address, is what was built.]
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

`mac/mmutest.s` gained seven cases (12 to 18), run after phase 1's eleven and skipped when no bus error has been taken
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
18. `FMOVE.L (A0),FP0` with the operand's page invalid: the exact FPU's guard (11.4) delivers the bus error from the
    FMOVE, which then runs again and loads the value.

Case 1 now expects format `$B` (a read) and case 2 `$A` (a write). `ROM=mac/build/mmutest.rom tools/run-solo.sh 45
mac.mmu=full`: **`MMU test: 86 passed, 0 failed, 118 bus errors taken`** (82 and 117 before case 18; the stock kernel:
`16 passed, 35 failed, 0 bus errors taken`, then the benchmark). The first version of the logger lost the
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

[Phase 3 (section 12) did the first six of these; pages below 4 KiB are still emulated access by access.]

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

## 12. Phase 3: the groundwork A/UX needs (26 November 2026)

The owner will try A/UX 3 install images in the morning (`mac.mmu=full scsi.cd=... scsi.disk=...:4 mac.boot=cd`, with
the virtual SCSI devices of docs/08 item 148, merged from `main` into this branch first). There is no A/UX here, so
this phase built what section 11.9 listed from the 68030's manual and the code, and tested it with synthetic cases
only: `mac/mmutest.s`, the System 7.1 test image (Virtual Memory off and on) on the solo board. Everything is still
behind `mac.mmu=full`; the identity stays a 68030's (CPUFlag 3, Gestalt `'mmu '` 3, MMUType 4).

### 12.1 Address spaces and ASIDs

Phase 2's sets (six, each four level-1 descriptors swapped into the shared user L1 table with two full TLB flushes)
are gone from the option's path. Each 68030 translation the soft TLB keeps is now an *address space*: its own L1
table and ASID (`aarch64/mmu.c`, `mmu_space_new`, `_page`, `_entry`, `_reset`, `_flush_asid`, `_install`), keyed by TC,
a root pointer and a mode (section 12.2). Entries 0 to 3 of a space's L1 point at its own L2 tables (4 to 7 repeat them,
the 4-8 GiB alias of a 68k address computation that carries), the rest is the user L1 of the moment, and the Pi's
windows inside the first 4 GiB are copied in from the pristine subtree. The 68k's pages are mapped non-global
(`MMU_NG`), so the TLB keeps every space's translations apart, and a switch is one write of TTBR0_EL1 with the space's
ASID and an ISB, no TLB maintenance. Thirty-two spaces are kept (ASIDs 1 to 32; the kernel's own mmu_user_L1 is ASID 0
and core 0 never runs on it again after the first space); a new root takes the least recently used space, which is
emptied (its L3 tables back to the pool, a TLB flush, the code cache's blocks of it dropped, 12.4). Translation off is
a space too, the identity mapped page by page like any other, so core 0 is on spaces from the start. Each space keeps
a bitmap of the pages it maps and, new, a summary bitmap of the bitmap's non-zero words, so a flush visits only what is
mapped: VM's PFLUSHA, 860 µs in phase 2, takes 140 to 225 µs (12.7).

The kernel's alias of the first 4 GiB in TTBR1 (L1 entries 508-511, for a 68k address computation that wraps below
zero) holds global entries and could only ever show one space: it is emptied when the first space is installed
(`mmu_space_leave_alias`), and an access through it faults and is checked by the walk and emulated
(`psmac_mmu_ram_access` for memory, `psmac_bus.c`). The other cores keep mmu_user_L1 and see the stock identity set
in it; none of them dereferences 68k logical addresses (they translate with `psmac_mmu_l2p`).

**A context switch.** `PMOVE` to CRP flushes the 68030's ATC and the new root is searched at once: the new root's space
is installed at the PMOVE. A space that comes back keeps the pages it maps if its tables still say what the mappings
say (the revalidation of phase 2's PFLUSH: the same frame, U still set, M for a writable page, no WP), because the
real chip would search the tables again after the flush and a guest may have edited a sleeping process's tables
without a flush, which a 68030 allows. That costs a walk per descriptor span mapped, not a walk of the whole tree.
`PMOVEFD` to CRP keeps the ATC: the old translations go on until the next TC load or PFLUSH, which is what the IIci
ROM's SwapMMUMode counts on (`ploadr 8(a0); pmovefd (a0),crp; pmove 8(a0),tc`, ROM `$40804BFC`; phases 1 and 2
deferred every CRP load that way, which a process switch would not survive: mmutest's T19 read process B's page
under A's root on the phase 2 kernel).

### 12.2 User and supervisor views

A configuration (TC, CRP, SRP, TT0, TT1) has a user view and a supervisor view. They are one space unless the two
modes can see different memory: TC.SRE with an SRP other than the CRP, TC.FCL, an enabled TT register that compares
FC2, or a long descriptor's S bit met by a walk. Then they are two spaces, the user's keyed by the CRP and the
supervisor's by the SRP (or the CRP again, with mode 2 in the key), and the 68k's S bit picks the one in TTBR0:

- translated code switches where it changes S: RTE, MOVE to SR, ANDI and EORI to SR, STOP (`psmac_mmu_emit_view`,
  eleven AArch64 instructions after the stack swap, six when the views are one, the last of them keeping
  `psmac_views.cur` equal to TTBR0; each of these instructions already ends
  its block). ORI cannot clear S. With the option only;
- the C side switches with `psmac_view_for` (`psmac.h`, inline: `M68k_ExceptionEntry.c` and `ExecutionLoop.c` are
  compiled with the 68k's registers fixed and may not call ordinary C): `M68K_Exception` and `ProcessIRQ` install the
  supervisor's view *before* they push the frame, the bus-error delivery after it, the remote restart at its SR `$2700`.

The fault path takes the access's function code from the view TTBR0 holds when the views are apart, from the SR when
they are one, and supervisor while an exception's frame is pushed. A supervisor page met while the modes share a space
is not mapped (user mode could reach it through the mapping): it is emulated, the first time is logged, and the next
flush or switch gives the modes a space each. `PFLUSH #fc,#mask` and `PFLUSH #fc,#mask,<ea>` flush the views whose
function codes match (user 1 and 2, supervisor 5 and 6; with one view, any of the four); `PLOAD` maps into its
function code's view and `PTEST` at level 0 looks there. The mailbox's and the diagnostics' translations
(`psmac_mmu_l2p`, also from other cores) use the view core 0's TTBR0 holds, and the supervisor's from another core.

### 12.3 MOVES

`MOVES` was the upstream emitter's plain access through the current mapping. Under the option it becomes a call
(`EMIT_MOVES_full`, `M68k_LINE0.c`, built like `EMIT_PMMU_full`) to `psmac_moves_exec` (`psmac_mmu.c`): the address
is searched at SFC's function code (a read) or DFC's (a write), with U and M set as the access would, WP, S and invalid
pages refused, and the bytes go to the physical page (Pi RAM, or the bus for I/O), whichever view is installed. FC 7,
CPU space, where nothing on a Macintosh answers a data cycle, is a bus error, as is a refused page; the SSW carries the
function code. All sizes and memory modes, `(An)+` and `-(An)` with A7's byte step, the register written back as the
68030 does when it is also the base. MOVEC of SFC and DFC was already there (`EMIT_MOVEC`). MOVES is a C call and a
walk per access, about a microsecond in QEMU: A/UX's copyin and copyout will pay that per long word moved.

### 12.4 The code cache, keyed by space

A translated block now carries the address space it was translated in (`mt_PsmacSpace`, the value of TTBR0_EL1 at
translation, `M68k.h`), and the JIT loop's lookups (`FindUnitNoLRU`, the one the Macintosh runs, and `FindUnitQuick`'s
keyed search) accept it only in that space (`PSMAC_SPACE_OK`, `ExecutionLoop.c`: one load of `psmac_views.cur` once
the address has matched, and nothing without the option; an `mrs` of TTBR0 there was the first version); the address-keyed quick cache is bypassed under the option. So the same
logical address in two processes is two blocks, and shared text is translated once per process. A space emptied for
another root drops its blocks (`psmac_jit_purge_space`, `M68k_Translator.c`), never the running one's. Writes to code
are what they were: the Mac's (and A/UX's) instruction-cache clears through CACR bump `psmac_icache_epoch`, after which
each block is checked by its checksum, and a PFLUSH that finds a page on a new frame bumps it too.

Two corrections to phase 2 on the way. A block that is an instruction fetch's bus error and nothing else (the first
instruction's page refused at translation) kept answering to its address after the handler had made the page valid,
so the handler's RTE would have found the fault again for ever; it no longer answers once delivered. Its fingerprint
and checksum were read from the refused page (three "not delivered" lines per fetch fault): not any more. And the fetch
probe used FC 6 for every block; it uses the mode the block will run in.

Not done: a block's code is read through the data view, while a 68030 fetches it at FC 2 or 6. The two differ only with
TC.FCL or a TT register comparing FC0/FC1; then the translator compares both searches for each block's first
instruction and logs a difference (`code at ...: the program space gives ..., the data space ...`, the first eight).

### 12.5 The double bus fault

`M68K_Exception`'s frame push and vector fetch, and `ProcessIRQ`'s, run under a guard (`psmac_views.guard`). A page
that is only not yet mapped is mapped as before and the push goes on; a page the tables refuse is what a 68030 calls a
double bus fault, and it halts: the log says `[PSMAC:MMU] double bus fault: write 001c57f8 refused (MMUSR 0403) while an
exception's frame was pushed or its vector read (SR ..., A7 ..., VBR ...): the 68030 halts`, the walk of the address
follows, then `the 68030 is halted (double bus fault); restart the machine`, and core 0 stops running the 68k (the
heartbeat goes on, its JIT line at 0 MIPS). Before, the push wrote into nothing and the RTE took whatever was there.

### 12.6 The test ROM

`mac/mmutest.s` gained these cases, after phase 2's (all need the option, and are skipped without it as T12-T18 are):

19. Two processes: a second tree (`mktree`), process A mapping logical pages `$C0` (data) and `$C1` (code) to frames
    `$D0`/`$D1`, process B to `$D2`/`$D3`, each code page "move.l #id,d0; rts". PMOVE to B's CRP, read and call, to A's,
    read and call, to B's again (its space kept), a write in B's page: each sees its own data, runs its own code, and
    the write lands in B's frame only. No cache clear in between, so a block found by address alone would run A's code
    under B.
20. 4000 PMOVEs to CRP, alternating, with a read and a call in each process: the sum is checked, the kernel reports the
    switches and their average time.
21. SRE: a third tree as the SRP, page `$C2` on frame `$D5` for the supervisor and `$D4` for the user; the supervisor
    reads its frame, user mode (entered by ANDI to SR, by an RTE of a format 0 frame, and by MOVE to SR) the user's.
22. MOVES with SFC/DFC 1 reads and writes the user's page from supervisor mode, SFC 5 the supervisor's; SFC 7 is a bus
    error with SSW `$0147` from the MOVES (the handler sets SFC to 1 and the MOVES runs again); a user page the tables
    refuse is a bus error with SSW `$0141` and the page's address.
23. With both modes' entries of page `$C3` in their spaces and both descriptors made invalid, `PFLUSH #1,#4` flushes the
    user's only (the supervisor's stale entry still answers, as on a 68030; the user's access faults), and
    `PFLUSH #4,#4` the supervisor's only, the other way round.
24. With SRE, page `$C4` valid in the SRP's tables and invalid in the CRP's, the supervisor stack moved onto it: a TRAP
    from user mode pushes its frame there (format 0, vector 32 read back), no bus error.
25. (`mmutest-halt.rom`, at the end instead of the benchmark) the supervisor stack on a page invalid in every table, and
    a TRAP from user mode: the double bus fault of 12.5.
26. Code on a page the tables refuse, never run before: the fetch is a bus error (format `$B`, SSW `$5006`: FB, RB,
    supervisor program; the page's address in the stage B address at `+36`), the handler maps the page and the code runs.
27. 48 roots, more than the kernel's 32 spaces (the same tables, the root pointer's limit field telling them apart):
    the least recently used spaces are emptied and reused, with their blocks, and every process still reads its page and
    runs its code.

The handler validates a user fault in the CRP's level C table and a supervisor fault in the SRP's (by the SSW's FC2),
takes a fetch fault's address from the stage B address when DF is clear, and treats FC 7 as said in T22.

**Results** (solo board in QEMU, `mac.ram=8`, the final kernel): `ROM=mac/build/mmutest.rom tools/run-solo.sh 60
mac.mmu=full` gives **`MMU test: 137 passed, 0 failed, 123 bus errors taken`** (86 before this phase), with no "not
delivered" line. The kernel's own figures for it: 4057 switches, 32 spaces in use at the end, 54 made and 22 reused (T27),
7 MOVES, **57 µs a switch on average** in QEMU, nearly all of it the revalidation of about sixty mapped pages (about a
microsecond a page); the switch itself is a TTBR0 write. `mmutest-halt.rom`: the 137 again, then

```
[MAC] MMU test: T25, an exception whose frame cannot be pushed: the 68030 should halt now
[PSMAC:MMU] double bus fault: write 001c57f8 refused (MMUSR 0403) while an exception's frame was pushed or its vector read (SR 0004, A7 001c5800, VBR 00060000): the 68030 halts
[PSMAC:MMU] the 68030 is halted (double bus fault); restart the machine
```

and the heartbeat's JIT line at 0 MIPS from then on. Without the option the new cases are skipped and the result is
phase 2's, `16 passed, 35 failed, 0 bus errors taken`, then the benchmark.

**The negative control** is the interim kernel built at the start of this phase (phase 2's code with `main` merged,
`aux-kbuild-interim`): with T24 the run never reports (T24's frame push went into nothing, `fault at 001c47f8 ... not
delivered as a bus error: not in the running block`, three times, and the 68k was lost after the RTE); with T24 taken
out of a scratch copy of the test, `101 passed, 23 failed`, the first of the failures `t19a=BBBB0019
t19acode=B0B0B0B0 t19writea=19191919 t20sum=F0F0E4A0 t21user=5555AA21 t22read=5555AA21` (process A saw B's page and
ran B's code, the user saw the supervisor's page, MOVES ignored its function code). T26 on an earlier kernel of this
phase, before the fetch fault's block was kept from reading its page, passed with three "not delivered" lines.

### 12.7 Phase 2's results, again

**System 7.1's Virtual Memory** (solo board, a fresh copy of the test image, `mac.ram=8`, 32 MB of VM on "Mac Volume",
Aurora68 VM Test in Startup Items). Phase 2 had switched VM on in the Memory control panel; this time the image was
prepared on the host: the System file's `'vm  '` resource 0 written with `tools/mac/rsrcadd.py` and a 32 MB file
`VM Storage` (type `ZSYS`, creator `MACS`) put in the volume's root with hfsutils. The resource's layout, from VM's own
reading of it (`ptch` 42, `$4F42`-`$4FCC`): byte 3 bit 0 set for on (with it clear, bit 1 makes VM call a routine with the volume's name and clear
the bit, *inference*: the storage file's removal),
bytes 4-7 the size in bytes, byte 9 onward the volume's name as a Pascal string, 37 bytes in all
(`00000001 02000000 00 0A "Mac Volume"`). VM took it at the next start without a word.

| kernel | Finder | pass 1 (write) | pass 2 (read forwards) | pass 3 (read backwards) | mismatches | bus errors | PFLUSHA, time each |
|---|---|---|---|---|---|---|---|
| phase 2 (`runs/vm8`, section 11.5) | 47.2 s | 11,625 | 15,319 | 10,372 | 0 | 16,239 | 33,410, 860 µs |
| phase 3, first build | 48.2 s | 12,051 | 15,201 | 9,650 | 0 | 16,219 | 33,364, 165 µs |
| phase 3, final kernel | 50.7 s | 10,555 | 14,033 | 9,909 | 0 | 16,238 | 33,404, 138 µs |
| phase 3, final kernel, again | 67.5 s * | 10,941 | 18,236 | 10,326 | 0 | 16,239 | 33,405, 224 µs |

(ticks; * the host was converting a disk image for MAME during that boot, which slowed QEMU). Every bus error
delivered, none refused, VM's three spaces (the identity, the ROM's 32-bit tables, VM's own) made once and never
switched again; the sum is the one written in every pass. The flush is five times cheaper than phase 2's because a
space now keeps a summary of its bitmap and a flush visits only the words that map something.

**The stock path.** Without the option nothing new is emitted, and the C paths test the flag once
(`M68K_Exception`, `ProcessIRQ`, `psmac_bus_read`/`_write`, the code cache's lookup after an address has matched).
mmutest's benchmark loop, median MIPS of the 5-second samples after the first two, main's kernel (`8a4e5a0`, the full
build with the PC and Apple II cards) and this branch's final kernel, alternated:

| kernel | option | test result | run 1 | run 2 |
|---|---|---|---|---|
| main | none | 16 passed, 35 failed, 0 bus errors | 376.5 | 402.5 |
| branch | none | the same | 384 | 402 |
| branch | `mac.mmu=full` | 137 passed, 0 failed, 123 bus errors | 327 | 336 |

An earlier set of the same, with the first final build: main 342.5 and 374.5, the branch 378.5 and 397.5, with the
option 301. The phase 2 kernel with this test (T24 taken out) gave 306 with the option on the same afternoon, so the
option's cost in this loop (its NEON copies before every `(An)+` and the stack instructions, blocks ending at page
boundaries) is phase 2's, not this phase's; section 11.7's 348 for it came from another session's QEMU.

System 7.1 to the Finder (a fresh copy of the test image, VM off, seconds from power-on), the kernels alternated, two
sets:

| kernel | option | runs |
|---|---|---|
| main | none | 36.0, 35.2, 33.7, 37.7 |
| branch | none | 39.4, 37.1, 35.2, 37.2 |
| branch | `mac.mmu=full` | 46.0, 46.5, 44.5, 45.7 |

The branch without the option is inside main's spread; with it the boot is as phase 2's (47.0 and 47.6 in section
11.7), the cost of the emulated accesses noted there.

### 12.8 What A/UX should find, and what is missing

**What it should find.** A 68030 PMMU as the manual describes it, phase 2's and this phase's: tables of any shape the
68030 takes (short and long descriptors, early termination, limits, indirect descriptors, FCL, page sizes 256 bytes to
32 KiB), TT0/TT1, SRE with its own root, the S bit, WP, U and M kept in the descriptors, PTEST, PLOAD, every PFLUSH,
PMOVE to and from every register with and without FD, MOVES in either direction and any function code, bus errors with
the 68030's frames ($A for a write, $B for a read and a fetch) and their restart, a process switch that costs a TTBR0
write and the revalidation of what the process had mapped, and a halt on a double bus fault. Its own vectors, VBR and
SR changes are the 68k's as before.

**What is missing or different.**
- *Restart, not continuation.* As since phase 1 (section 4): an RTE of a fault frame runs the instruction again. A
  handler that completes a cycle itself and clears DF (or RB, for a fetch) sees it run again. Whether A/UX's does is
  not known; VM's does not.
- *The program space* (12.4): blocks are read through the data view; a difference is logged.
- *The ATC's size.* The spaces keep every translation until a flush or a switch revalidates it; a guest that edits a
  descriptor of the running process and does not flush keeps the old mapping longer than a 68030's 22-entry ATC would.
- *Pages below 4 KiB* are emulated access by access (A/UX's are 4 KiB on a 68030, *inference*).
- *MOVES's cost* (12.3), and a context switch's: about 55 µs in QEMU for a process with a few dozen pages mapped
  (T20), more with more pages mapped; not measured on a Cortex-A53.
- *Interrupts from user mode with the views apart* go through `ProcessIRQ`'s switch, which no test exercises (the test
  ROM takes no interrupts); exceptions from user mode do (T21-T24).
- *The chips.* A/UX drives the hardware itself; on the solo board that is the kernel's stand-ins for the VIAs, the SCC,
  the 5380 (now with the virtual devices of item 148) and the RBV, written for Mac OS's use of them. That is the first
  risk, and none of it is an MMU matter.
- The `'mmu '` 3, 68030 and 68882 identity is unchanged, as is everything without the option.

**What to look for in the log** (`mac.mmu=full scsi.cd=... scsi.disk=...:4 mac.boot=cd`):
- `[PSMAC:MMU] switch #N: TC=... CRP=...` with `SRE SRP=...` and `-> user space N (ASID n, new), supervisor space M`
  is A/UX installing its tables (the first ones `new`, a process switch `cached`). A `TC=... is not a valid
  configuration` line means the 68030 would have taken an MMU configuration exception (vector 56), which is not raised.
- `[PSMAC:MMU] bus error #N (data read|data write|operand|fetch|helper): PC ... SSW ...` are page faults delivered, the
  first six and every 1024th; A/UX's paging makes many. `... not delivered as a bus error: ...` means a fault that
  could not become one (a C helper the guard does not cover); the access read `$BAD0BAD0` or was dropped. Report it.
- `[PSMAC:MMU] N bus errors in a row at ...` with the walk after it: the same access faulting again and again (a
  handler that does not fix what the frame names, or a disagreement about the tables).
- `[PSMAC:MMU] double bus fault: ...` then `the 68030 is halted`: what a real IIci would do (a stuck machine); the walk
  line says which page was refused.
- `[PSMAC:MMU] MOVES #N at ...: read|write ... fc F size S` (the first eight, then every 4096th): the kernel reaching
  user space. `[PSMAC:MMU] PTEST #N` and the PMOVE lines (the first 64, then every 1024th) show the MMU instructions
  with their PCs.
- `[PSMAC:MMU] a long descriptor's S bit at ...`: the modes are given a space each from the next flush.
- `[PSMAC:MMU] code at ...: the program space gives ...`: 12.4.
- `[PSMAC:MMU]   space N (ASID n) reused` and `N translated blocks of the space dropped`: more than 32 roots in use.
- The heartbeat's two `[PSMAC:MMU]` lines every five seconds: bus errors (data, operand, fetch), lazy maps, privilege
  violations from user mode, walks, maps, PFLUSH by kind with the pages kept and dropped, PLOAD, PTEST, PMOVE, MOVES,
  switches, spaces in use, `user and supervisor views apart` or `one view`, and the average switch in µs.
- Unrelated to the MMU but likely: the SCSI lines of item 148 (`[SCSI] ID 3 (CD-ROM): command $xx ... not
  supported`), and the crash record of an unexpected exception (`psmac_crash.c`).

### 12.9 What is not settled

- A/UX itself: everything above is from the manual and synthetic tests.
- Real hardware: TTBR0 writes and ASID-tagged entries on a Cortex-A53 behave as the architecture says (QEMU agrees);
  not seen on the chip. The cost of a switch and of a MOVES there.
- The MAME full stack, the reference: with the final kernel and the option, MAME's IIci (the patched ROM, `mac.ram=8`, the
  System 7.1 image as an uncompressed CHD, a private PRAM in 32-bit mode) reached **the Finder at 322 s** (MAME's own
  screen shows it; section 11.8 had 253 s on another afternoon, and the full stack's time is the bridge's): the identity
  space, the ROM's 32-bit tables (TC `$80F05750`, CRP `$7FFF0003:$007FFCC0`, long descriptors, one space for both modes)
  with RBV video's low memory, 232 lazy maps, no bus error, nothing undelivered. VM was not run there this time.
- 24-bit mode, with or without VM (the test image runs in 32-bit mode).

## 13. The first try on the real SE: core 0 died before the 68k started (27 November 2026)

**What the owner saw.** The full kernel of phase 3 (`6b7bad4`, with the PC and Apple II cards) on the real SE board
(`mac.board=se`), `mac.mmu=full`, 128 MB, first with an A/UX disk as a virtual SCSI disk at ID 0 and then with nothing
but the option and the usual System 7.1 image from the card: the SE's screen showed its own uninitialised video RAM
(eight bands of fine repeating structure, a diagonal edge between lighter and darker halves) and never changed. The
saved log said why: every heartbeat had 0 bus transfers and PC `00000000`, and on core 0, just after the start-up's
diagnostic lines `[PSMAC] map 00000000 -> phys 0xffffffffffffffff` (and eight more like it),

```
[JIT:SYS] Unhandled page fault: opcode b85dc022, read from 0x74a1cba2fd7a2769
[JIT:SYS] Exception with vector 0200 on CPU0. ELR=0xffffff80000b50c8, ... FAR=0x74a1cba2fd7a2769
```

**Where.** Against the build's `Emu68.elf`, ELR is `psmac_mmu_fault` + `$88`, the first load of `locate()` (inlined),
which reads the running block's unit from x12; x3, the abort's own ELR passed in, is `psmac_setup_memory` + `$1E64`, the
`ldrb` of `[PSMAC] EL1 probe read of 50f01c00`: the kernel's own read of the VIA through the 68k's address space,
before the 68k starts, which every board makes and which has always been an abort that the bus code answers. Phase 3
made two things true at that moment that were not before: translation off is a space too, so `psmac_mmu_translating()`
is true from the first space on (phase 2's test was `translate_on`, false until the ROM's first TC load), and the abort
therefore went through `locate()`. `locate()` took x12 for the running block's entry whenever its tag bit (bit 36, the
JIT arena's read-only alias) was set and the ELR was above it. In the kernel's own C x12 is whatever the compiler left
there: on the real Pi `0x74a1cbb2fd7a278d`, bit 36 set, and the "unit" read below it was at a wild address. Core 0 took
an unhandled abort inside the abort handler and stopped. The `map ... -> phys 0xffffffffffffffff` lines are not the
fault: `mmu_virt2phys` asks the ARM tables of the identity space, which the option maps on demand, so nothing is mapped
yet (the stock path prints Pi addresses there).

**Why the testbeds missed it.** Under QEMU the same probe read met an x12 without the tag bit (or below the ELR), so
`locate()` gave up at once and the access was emulated: `EL1 probe read of 50f01c00 -> 80` in every run of phases 2
and 3, the solo board, the MAME IIci and, this morning, the MAME SE. It is no SE matter: any board on a Pi with such a
register value at that instruction dies the same way, and the Plus, 128K, 512K and 512Ke boards run the same start-up.

**The MAME SE first** (`MACHINE=macse_ps`, `mac.board=se`, the 512x342 declaration ROM, a copy of the System 7.1 CHD at
ID 6, `PSMAC_RAM=128`, `mac.mmu=full`, the owner's kernel): no fault; the chime, then "Welcome to Macintosh" and the
INITs at 240 s, the end of that run. It did not reproduce the hardware, which is what pointed at a difference in state
rather than in the board's code.

**Reproduced in QEMU.** A scratch build (not committed) set x12 to the owner's value in the probe read's own asm
statement. On the solo board with `mac.mmu=full`: `Unhandled page fault: opcode b85dc022, read from
0x74a1cba2fd7a2769`, the owner's opcode and address, at the same point of the start. The phase 2 kernel cannot take
this path at that moment (its `psmac_mmu_translating` is `translate_on && ...`, in the interim build's code as in
`c9538d0`), which puts the cause in phase 3.

**The fix** (`psmac_mmufault.c`, `locate()`): x12 is believed only if the faulting instruction is itself translated
code, in the JIT arena's read-only alias, and the entry lies there as well; otherwise the abort is "not translated
code" and goes on as before (mapped, emulated, or a guarded helper's landing). The same scratch build with the fix:
the probe read gives `80` with the bad x12 in place and System 7.1 reaches the Finder at 40.6 s on the solo board.

**Tests** (the fixed branch):
- `make -C mac check`: passes.
- `mmutest` on the solo board, `mac.ram=8`: with the option `137 passed, 0 failed, 123 bus errors taken`, none
  undelivered (so blocks' faults are still found by `locate()`); without it `16 passed, 35 failed, 0 bus errors
  taken` and the benchmark (366 to 425 MIPS in QEMU). Without the option `psmac_mmu_fault` is never called, so the
  stock path is unchanged.
- The owner's full kernel rebuilt from the branch (PC and Apple II cards), on the MAME SE, `PSMAC_RAM=128`,
  `mac.mmu=full`: the ROM's 32-bit tables (TC `$80F06B00`, CRP `$7FFF0002:$07FFFCC0`, one space for both modes), the
  SE's screen rows sent and read back right, the Finder at 282 s (the full stack's SE without the option: about 290
  s), no bus error, nothing undelivered.

**Still to be seen on the real SE.** That it now starts; then, for A/UX, the list of section 12.8. The emulated
accesses (5.5 million in that boot, most of them the SCSI chip's and the VIA's pages) are the option's cost here as on
the other boards; on the SE every one of them is a bus cycle over the GPIO anyway.

## 14. A/UX on the real SE: as far as "Checking..." (26 November 2026)

With section 13's fix the SE started System 7.1 with `mac.mmu=full`, and the Memory control panel offered Virtual
Memory, which it does only for a 68030 MMU. Then A/UX 3.1.1 (the Workgroup Server 95 edition, a prebuilt 1 GB disk
served as a virtual SCSI disk at ID 0, `scsi.disk=AUX.dsk:0`), in four steps:

- **Started from System 7.1, A/UX Startup said "No SCSI disk port" (IN-8).** Its machine check (Gestalt `'mach'`)
  refuses machine types 1 to 5, 10, 17 and 19, the compact Macs among them, and Aurora68 Machine (`mac/pimach.s`)
  answers 5, the SE. With the extension moved out of the Extensions folder the answer was the ROM's 11, the IIci,
  and the FPU and PMMU checks (Gestalt `'fpu '`, `'mmu '`) passed as well.
- **Then "Standalone program space is too small" (IN-2).** A/UX Startup puts its standalone shell in the first
  1.5 MB of memory (it compares against $17FC00 and $1A0000), which it has only when A/UX's own small System
  launches it at start-up; under System 7.1 with Aurora68's extensions the system heap is above that.
- **Started from A/UX's own Mac OS** (Startup Disk set to its volume, and `mac.boot=scsi` so the kernel does not
  try the card's image first), A/UX Startup came up by itself, showed its copyright window and "Checking...".
- **There the Mac stopped**, the pointer frozen. The log: the 68k in a loop at $548C8-$548F2 in the standalone
  shell (low memory, its data at $11001622-$11001628), SR $2700/$2400, no bus cycle at all (+0 a beat, no SCC,
  VIA or SCSI access), the soft TLB with 4 spaces, user and supervisor apart; and level 4 taken 4,117,802 times
  in five seconds, every one "taken again at once". Level 4 is the SE's SCC (its level 2, `psmac_se_ipl`). So the
  SCC holds its interrupt and the shell's handler returns without touching it: a livelock. On a IIci the SCC
  would have nothing pending at that moment; on the SE something is (which is not yet known), or the handler's
  access to the SCC goes somewhere other than the chip.
- **`mac.sccirq=off`** (this commit) keeps the SCC's line from the 68k, a diagnostic to see whether the check
  goes on without it. Next, if it does: which of the SCC's sources is pending (RR3, `mac.setrace=N,SCC`), and
  whether A/UX's own driver reaches the chip through its page tables.
- **`mac.sccirq=off` stopped the Mac OS itself** (A/UX's System 7.0.1, the same morning): "Welcome to Macintosh", the
  68k at $4080C098 in the ROM after 160,000 SCC accesses in one beat, then no bus cycle at all, waiting for the
  interrupt that was kept away. So **`mac.sccirq=aux`** keeps the line away only while the MMU's user and supervisor
  views are apart (`psmac_mmu_views_apart`): A/UX's own tables, which the Mac OS never sets up.
- **With `aux` the shell got to "Checking..." again and stopped on level 2 instead** (the next run, a fresh copy of
  the disk): no SCC interrupt any more, but level 2 taken 4.1 million times in five seconds, every one "taken again
  at once", RBV IFR $02 IER $02 (the slot interrupt), the slot-9 line held by the network board, the HDMI screen's
  VBL pending; level 1 not taken at all. The shell's idle loop is `STOP #$2000`, and its handler for these levels
  is `ORI.W #$0700,(SP); RTE` (code at $548C8): it raises the mask and returns, acknowledging nothing, and goes back
  to STOP. On a IIci nothing would be asserted then; here Aurora68's NuBus cards are, and they starve level 1, the
  VIA's clock the shell waits for (the system-error code at $AF0 stayed 0: no system error). `aux` now keeps the slot
  interrupt away too (RBV bit 1 out of the level, the RBV's other sources such as the SCSI chip's still in it).

## 15. A/UX in the MAME SE: past "Checking..." to init, and what stopped it on the way (26 November 2026)

The owner's A/UX 3.1.1 disk (the Workgroup Server 95 edition, prebuilt for Shoebill; its partition map in section 14)
was served from a host file as a virtual SCSI disk at ID 0 in the MAME SE full stack (`MACHINE=macse_ps`, `HD=none`,
`mac.board=se`, the 512x342 declaration ROM, `PSMAC_RAM=128`, `mac.mmu=full`, `mac.boot=scsi`, MAME's SE PRAM as found
and restored after each run). Every run started from a fresh copy of the disk unless said otherwise (A/UX writes to it,
and some of the bugs below damaged it). A/UX's own Mac OS (7.0.1) started, launched A/UX Startup, and A/UX Startup
launched the kernel: in MAME the disk was clean, so the "Checking..." of section 14 passed in a moment. What followed
was a chain of faults, each found from the kernel's message buffer or a register dump in a RAM image of the Mac, each
fixed in the kernel, the board layer or the Mac-side driver, and each in a way that makes the SE board more like a IIci
for any software that drives the hardware itself.

**How the guest was read.** `mac.ramdump=BEAT[+EVERY[-LAST]],MB,PATH` (new, the testbed only) writes the Mac's physical
memory to a host file at a heartbeat; the heartbeat's register line now has USP and VBR. The A/UX kernel loads at
physical $5C000 (logical $10000000, its data at $11000000), and its message buffer (`msgbuf`, logical $120199F4 under the
standalone loader's tables) holds what the console printed; a 68030 table walker and a reader for A/UX's BSD file
system on the disk image (both host scripts, not committed) gave the rest. A/UX's low memory at $54000-$5C000 is not
the "standalone shell" section 14 took it for but A/UX Startup's and the kernel's shared low-level code (`locore`: the
interrupt entry at $54B5E, `spl` at $54884, the idle loop at $548D4 that section 14 saw, and the stub at $548CA,
`MOVE #$2700,SR; ORI.W #$0700,(SP); RTE`, which the kernel puts in all six autovectors while its drivers initialise and
takes out again before it enables interrupts).

### 15.1 What was wrong, in the order A/UX met it

1. **"Attempt to use unsupported A-line trap: A072", then "panic: Unsupported kernel A-line trap".** A/UX opens the slot
   card's video driver from the declaration ROM (`PiVideo: open` in the log, a second time, under A/UX's tables) and
   runs its slot interrupt handler in the kernel. `mac/pivideo.s` called the Vertical Retrace Manager as trap $A072
   (`_DoVBLTask`), which A/UX does not emulate in the kernel. Apple's own video drivers call it through the low-memory
   vector `JVBLTask` ($D28); PiVideo does that now. The panic re-enabled interrupts, the next VBL panicked again ("Double
   panic", nine levels deep), the kernel stack ran off its page, and:
2. **A double bus fault that was not one.** A translated instruction's page fault whose own frame cannot be stacked was
   "not delivered" and emulated ($BAD0BAD0 for a read): the kernel went on with those values, used one as a pointer,
   and core 0 died in the abort handler with the console lock held, so the log stopped mid-line. Such a fault now halts
   the 68k as the guard's case of section 12.5 does, with the walks of the address and of the stack.
3. **"no slot 10 interrupt handler" ... "panic: Polling routine didn't handle interrupt".** A/UX's level-2 handler reads
   the RBV's IFR at $1A03 and IER at $1C13 and the slot flags at **$1E02**: offsets that name the right register on a VIA2
   (registers 13, 14 and 15 by A9-A12) and on the RBV, which decodes the low address lines (Linux's `mac_via.h` names
   rIFR $1A03 and rIER $1C13 for the same reason). `psmac_board_se.c` read an offset of $100 or more as a VIA register
   number only (13 the IFR, 14 the IER, as MAME's `rbv.cpp` does), so $1E02 read 0: every slot interrupting. A/UX called
   slot 9's handler with nothing pending and panicked. `rbv_regno` now takes the low five bits when they are not zero,
   and keeps the VIA reading for $1A00/$1C00, which the IIci ROM uses; the IIci testbed's slot-9 merge (`psmac_bus.c`)
   decodes the same way. Section 14's level-2 storm on the real SE was most likely this (*inference*: an unclaimed slot
   interrupt, the SIFR read as all slots asserted), not a card that cannot be quietened.
4. **The SCSI scan never finished.** A/UX's SCSI driver ("I/O Acceleration") reads the status byte by pseudo-DMA: DMA
   mode, start DMA initiator receive, wait for DRQ in the bus and status register, one read of the window. The virtual
   devices answered the window in the data-in phase only; the status read $FF without an ACK, the target stayed in the
   status phase and the driver retried the same command for ever. The window now takes status and message bytes too.
   (The scan of IDs 1-6 with no device takes about two minutes each in the testbed: A/UX polls BSY 100,000 times, and
   each poll is a bridge cycle to MAME's real 5380. On the SE it is a fraction of a second.)
5. **A/UX's init could not be given its page faults.** The first user program took a fetch fault on its first page of
   text, and its frame could not be stacked: `push_frame` wrote the frame and read the vector through whichever view
   TTBR0 held, and a fault from user mode is taken before the supervisor's view is installed, so with the views apart
   the supervisor stack ($12FFxxxx) and the vectors were looked for in the user's tables. The frame and the vector are
   now always supervisor data (`psmac_mmu_ram_ptr_fc`), the stage C and B words the faulting mode's program space.
   mmutest T28 is the case (a data and a fetch fault from user mode, the supervisor stack on a page only the SRP's tables
   map): 141 passed on this kernel, and the previous kernel never reports.
6. **Live condition codes judged dead.** With init paging its text in, the log had 531 "not delivered" reads from
   `cache_read_16`: the translator's condition-code lookahead (`M68K_GetSRMask`) reads the instructions after the one it
   translates, following branches and jumps, and met pages A/UX had not loaded yet. The abort handler answered
   $BAD0BAD0, and $BAD0 is `CMPA.W (A0),A5`, which sets every flag and reads none, so the flags of the instruction being
   translated were left out. The scan now stops, keeping what it has not seen overwritten, at an instruction on another
   4 KiB page (or one that could run into the next). Only with the option; the reads of the code cache's checksum of a
   block whose page has gone (`cache_read_32`/`_64`, a few thousand per boot) remain, and are harmless: they only fail
   the checksum.
7. **The disk.** After the first A/UX boot the next one said, from A/UX Startup, "/dev/default: BAD SUPER BLOCK: MAGIC
   NUMBER WRONG": the root file system's superblock (at its fourth kilobyte, written back by A/UX at mount) was one byte
   late on the disk. A/UX's SCSI driver writes the pseudo-DMA window once before it starts a transfer, in either
   direction ("pseudo-DMA write outside DMA mode ... MR $02" in the new log lines: DMA mode set, no start DMA yet), and the
   model took that byte as data. A 5380 moves a byte through DACK only once a start DMA register has been written; the
   window now loads the output data register outside DMA mode (and reads the bus), as the chip does. scsidev_test has a
   write and a read with such a byte; the superblock after a mount now differs from the pristine one in 15 bytes (its
   time and clean flag), the magic where it belongs.
8. **The reboot.** On its first boot A/UX restarted the Mac by itself (a RESET instruction, then the ROM; its
   autoconfiguration building a kernel for the machine, *inference* from the `newunix` A/UX Startup later looked for).
   The Mac OS that came up next stopped with level 2 asserted for ever: the RBV's IFR still had the sound bit that
   A/UX's and the ROM's $90 writes to $1A03 had set, and the new session enabled it. The RBV and the ASC, which
   `psmac_board_se.c` answers for the IIci, are now reset by the 68k's RESET as a IIci's are.
9. **An illegal instruction killed core 0.** A user program reached an illegal opcode (`$37E0`, a MOVE to a PC-relative
   destination); the translator's path for it dumps the 68k code around it from the exception handler, through the 68k's
   MMU, and the page before it was one A/UX had not mapped: the read faulted inside the handler. The dump is left out
   with the option; the program gets its illegal instruction exception.

10. **Old code after an exec.** A flush that drops a page because its descriptor is no longer valid did not bump the
    code cache's epoch (a page found on a new frame did). A process's text replaced in place (an exec, the new pages
    paged in on demand, no instruction-cache clear, which a 68030's 256-byte cache never needs there) could run the old
    process's translated blocks. mmutest T29 (code on a page made invalid, flushed, then valid on another frame, no cache
    clear): 143 passed; without the change T29 ran the old code (`t29new=29292929`).

### 15.2 How far A/UX gets now

With all of the above, from a fresh copy of the disk in the MAME SE (about ten minutes to the kernel, five more to
init): A/UX Startup, the kernel, the SCSI scan, init:

```
Running on mc68030
mc68882 Floating Point Coprocessor ID 1
Onboard SCC serial driver.
mc68030 caching enabled
...
fd:  floppy driver ver 2.00; IWM chip, d1 is 800K (2 head), d0 is 800K (2 head)
I/O Acceleration:  version g21,  maxba=16,  maxwb=16
SCSI bus 1, id 0: DISK: AURORA68Virtual Disk    1.0
```

then init on the screen (verbose): `/etc/rc`, errdemon, startmsg, cron, catsearchd, `telinit v`. Thousands of
processes run (8,600 context switches, 157 address spaces made, 125 reused, 25,000 page faults delivered in one run).

**What stops it there: user programs die with "Memory fault".** `/etc/rc`'s `chown adm /etc/wtmp` (pid 107, on every
boot) dumps core; later `/etc/loginrc`'s `macgetty` gives way to `getty`, which dumps core too, and init respawns it
without end, so the console never shows a login (thousands of processes, the screen unchanged for twenty minutes). The
cores (read from the disk image with a reader for A/UX's BSD file system) hold a format $A bus error frame: PC $2DC2
in chown (`PEA -28(A6)`, after a call of `strcmp` in libc_s at $47F05564), a write to $000001E0, SSW $0101, with A1 and
A7 both $1E0: the user stack pointer has taken a string pointer's value. getty's core holds the same frame and nearly
the same data segment, which no getty can produce, so the u-area and data the kernel dumped for it were chown's frames.
Not found. [Found on 27 November, section 21: an instruction lying across two pages, translated with $BAD0 for the
words on the page not yet in memory.] Two candidates were followed and ruled out as the cause (both fixed anyway): the code cache's epoch (item 10)
and the memory table below; the same crash at the same PC comes with 64 MB.

**A/UX's memory table on 128 MB** is wrong as well. The kernel prints

```
total memory size: 201326592 bytes
available  memory: 96227328 bytes
```

192 MB on a 128 MB Mac. A/UX's physical memory table (locore, $5B0C4 count, 16-byte entries from $5B0C8) is
`{0, $2EC000, $08000000}` and `{$04000000, $04000000, $08000000}`: the first chunk is all 128 MB and the second is bank
B again, so the frames from 64 to 128 MB are on A/UX's free list twice. The ROM's own RAM table at the top of memory is
right (`WLSC`, then `{0, $04000000}`, `{$04000000, $04000000}`). A/UX Startup builds its table from `GetPhysical`
(`_MemoryDispatch`) over the logical RAM, merging adjacent runs (its CODE 4, $3D0C and $3B32), and adds ranges from other
queries ($39D6, $3C0E); which of those adds bank B a second time was not settled. With `mac.ram=64` (bank A only) the
table is one chunk and the kernel says 64 MB, so that is the setting to use until this is understood; the
"Memory fault" crashes are the same with it.

### 15.3 What the owner's SE showed, read again

- *Level 2 on the real SE* (section 14, with `mac.sccirq=aux`): item 3 above. The slot masking `aux` had grown is taken
  out again (`mac.sccirq=aux` keeps the SCC's line alone, only while the views are apart; `off` as before), since A/UX
  needs its slot interrupts for the screen's VBL.
- *"PC $54912, SR $270D", "no interrupts at all"*: the kernel in locore's idle loop with every autovector on the masking
  stub, which is where its driver initialisation waits; in MAME the same PC and SR appear for a beat at a time on every
  boot. With items 1-4 the kernel no longer stops in that phase in MAME.
- *Level 4 on the real SE, 4.1 million a second, no SCC access*: not reproduced. MAME's SE raises no SCC interrupt (no
  device on either port), and A/UX touches the SCC about 570 times while its driver initialises and not again (both
  serial gettys are off in this edition's inittab). The stub above is the level-4 handler at that moment, so a pending
  SCC interrupt would be taken without end until the kernel's own handler is in. What the SE's SCC has pending then
  (an external/status change on a port's handshake or carrier input, which the SE wires differently from a IIci) needs the
  hardware: the new kernel's log with `mac.setrace=200,SCC` around A/UX Startup's launch, and the irq watch's line.
- *"bad clist count"* in the peek: a message string among locore's constants at $5B03A, present on every boot, not an
  error A/UX raised.

### 15.4 Tests (the final kernel of this section)

- `make -C mac check`: passes (scsidev_test 283, 328 and 164 checks; the four new ones fail on the previous code);
  main merged into the branch first (the shared WiFi ARP fix, Aurora68 Machine's machine type for A/UX Startup).
- mmutest, solo board, `mac.ram=8`: with `mac.mmu=full` **143 passed, 0 failed, 125 bus errors taken**, none
  undelivered (T28 and T29 new); without the option `16 passed, 35 failed, 0 bus errors taken` and the benchmark at
  363-381 MIPS, as before.
- System 7.1 on the solo board: the Finder at 36.3 s without the option, 45.7 s with it (section 12.7: 33.7-37.7 and
  44.5-46.5).
- System 7.1 on the MAME SE (`PSMAC_RAM=128`, a copy of the test CHD at ID 6): the Finder at 253.8 s without the
  option and 265.4 s with it (section 13: about 290 and 282 s), no fault, nothing undelivered, level 2 taken about 60
  times a second (the screens' VBL), nothing taken again without cause.
- A/UX's own System 7.0.1 on the MAME SE, from the virtual disk, with the option: the Finder at about 157 s, then A/UX
  Startup, in every run above.
- The owner's full kernel (`-DPSMAC_PC=ON -DPSMAC_A2=ON`, Aurora86 and Aurora65) built from the branch.

### 15.5 What the owner can run, and what is not settled

`mac.board=se mac.mmu=full mac.ram=64 scsi.disk=AUX.dsk:0 mac.boot=scsi` (no `mac.sccirq`), with A/UX's own Mac OS as
the startup disk and the Aurora68 Machine of main (it gives A/UX Startup the ROM's machine type). Expected on the SE:
A/UX Startup, the kernel's messages, init, and then what the MAME SE showed (the "Memory fault" crashes), unless the SE's
SCC stops it earlier as in section 14; if it does, the log with `mac.setrace=200,SCC` is what to send. Not settled:

- the user programs' "Memory fault" (15.2), and with it the login and A/UX's Finder; [settled in section 21]
- A/UX's 192 MB on 128 MB (15.2);
- the level-4 storm of the real SE (15.3);
- the "Checking..." of a disk A/UX did not unmount cleanly: in MAME that check reads the root file system through the
  Mac OS SCSI Manager at about 20 KB/s and was not waited for (40,000 sectors read after twenty minutes); on the SE the
  virtual disk is served without the bridge.

## 16. The real SE at "Launching...": interrupts nobody clears, and the storm guard (26 November 2026)

Test kernel 6 (section 15's) on the owner's SE, from System 7.1 on the card with A/UX Startup launched from it, A/UX's
disk at ID 0, `mac.mmu=full mac.ram=64`. A/UX Startup passed its checks (Aurora68 Machine's machine type, main's
change) and put up "Welcome to The Apple Workgroup Server 95 / Launching..." with the bar at about a fifth, and stopped.
The saved log (`mac.logsave=10`):

- **First boot, no `mac.sccirq`.** The 68k in A/UX Startup's standalone shell (PC $548CA-$54912, its own tables on:
  switch #7, `TC=82C07760` with SRE, the views apart), level 4 (the SE's SCC) taken 4,125,649 times in 5 s, every one
  "taken again at once", level 1 none, no SCSI command since. The shell's loop is `STOP #$2000`, then a test of a flag
  the clock's handler sets ($11001626); its interrupt handler is `ORI.W #$0700,(SP); RTE` at $548C8 (section 14): it
  touches no chip. A source that stays asserted gets the 68k back after every RTE, and the clock below it never does.
  This is section 14's storm again, which the MAME SE never showed (15.3), and leaving `mac.sccirq=aux` out of the
  command line had been our mistake.
- **Second boot, `mac.sccirq=aux`.** Level 4 gone; level 2 instead, 4,125,669 entries in 5 s, level 1 none. The RBV's
  IFR $02 and IER $02 (the slot interrupt), SIER $3F; the card's slot-9 line 1 with the network board's interrupt up
  (`net irq 1`, left from the Mac OS session's Ethernet driver; WiFi traffic for the shared address keeps arriving) and
  the HDMI screen's VBL pending (the shell opens the card's video drivers itself right after the switch, "PiHDMI: VBL
  handler installed", but its handler is the ORI/RTE above, so the driver's never runs). Section 15's RBV decoding fix
  was not the issue here: the flags were read right, and nobody read them.

Two changes, both only while the views are apart (A/UX's tables; Mac OS never splits them):

1. **The hand-over** (`psmac_card_handoff`, alice_psmac.c). The first time the views come apart after a RESET the
   network board and .PiInput's ring are dropped, as a RESET drops them (psmac_card_reset): the Mac OS session that
   opened them is over and neither the shell nor the A/UX kernel has a handler for them. They come back when a driver
   opens them. The screens are left alone.
2. **The storm guard** (`mac.irqguard=on`, the default; psmac_board_se.c, irq_guard). A level is kept from the 68k
   for 20 ms when the interrupt watch (ExecutionLoop.c) sees it taken 20,000 times in a row at the same PC and stack
   (a handler that returned with its source still asserted and nothing else done: the SE's pattern, about 5 ms of
   such a storm), or 4 times in a row at the same PC, each entry deeper on the stack than the last (a handler that
   lowers the mask with its source still up and takes itself again). Then the level comes back and is judged again.
   Normal running takes a level again at once a few dozen times in 5 s, never 20,000 in a row. A first version gave
   the level back at the next access to its chip (the RBV, the SCC); the MAME SE showed the shell reading the RBV
   right after the switch, so a guard keyed on accesses could be cleared on every turn, and time it is.
   `mac.slotstorm=1` (testbed) holds the slot line up for 60 s from 3 s after the switch.

### 16.1 What the MAME SE showed

- `mac.slotstorm=1` with the line held from the very moment of the switch (run 19): the handler of that moment
  (code at $1004xxxx, the shell's environment) lowers the mask and takes itself again, 24 frames, 3,564 bytes, until
  the stack runs out: a double bus fault at $100460E2. Not the SE's pattern (there the entries were "again at once",
  nested 0 bytes deep), so the test was moved 3 s later and the nesting rule added.
- 3 s after the switch (run 20): the guard held level 2 four times ("taken inside its own handler again and again at
  PC $548E0") and the stack still ran out after them, at $1004848A: in that phase, in the emulator, the shell's
  handler nests faster than 20 ms windows let it unwind. So the guard is a second line; the first is to have no
  source up: the hand-over above for the network board and the input ring, and `mac.hdmi=log` on the SE, which
  leaves out the HDMI screen's card and its VBL (psmac_bus.c: the second card exists only with `mac.hdmi=screen`).
- Without the test option (run 21, `mac.ram=64`): A/UX as section 15, the kernel, init and /etc/rc with its "Memory
  fault", the guard never firing, the hand-over logged once at the switch.

### 16.2 What the owner can run

Section 15.5's line with `mac.sccirq=aux` kept (the SE's SCC) and `mac.hdmi=log` added, a fresh copy of the disk, and
A/UX's own Mac OS as the startup disk if it can be had (System 7.1 from the card launches A/UX Startup too, now).
If the slot storm comes back regardless, the log's `irqguard` lines say which level and which handler.

## 17. The real SE in A/UX's kernel: a panic after the first bus error (26 November 2026)

With section 16's kernel, `mac.hdmi=log` and the declaration ROMs rebuilt on 25 November copied to the card (the
SE's card still had ROMs from before PiVideo's JVBLTask change, so the first try panicked with "Unsupported kernel
A-line trap: A072" as the MAME SE had before 8576784), the owner's SE went past "Launching..." into A/UX's kernel and
stopped with "trap type 11 / pc = 0x12FFF30C sr = 0x2000 up->u_procp = 0x1BC91E pid = 1 exec = 'sched'" and "Double
panic: unexpected kernel trap". The PC is in the kernel stack's region ($12FFFxxx; the heartbeat's D3 held the same
value and A2 $12FFF000). The log (aurora68.log, the last 64 KB) said one bus error had been delivered, an operand
fault, and nothing after; the line naming it had left the 64 KB, behind the SCSI trace (`scsi.trace=1300`) and the
stalled machine's PC trail. The MAME SE takes its first two bus errors at the same stage, operand faults from the
standalone shell's PC $5571A writing $00000030 and $FFFFFFFC (memory probes, handler $54C08), and goes on.

The first four bus errors delivered are now repeated in every heartbeat's MMU report ("the first bus errors: #1 o
PC ... w ... SSW ... sup -> handler"), so the next log names the SE's.

## 18. The SE's first bus error, followed: the page table A/UX Startup could not find (26 November 2026)

The next SE log named it: `#1 o PC 0005571a w 00000030 SSW 0101 sup -> 00054c08`, the same as the MAME SE's: a MOVES
of the standalone shell writing $30 in the new process (init's arguments), a page not there yet. The crash record then
showed an F-line at $12FFF30C, the 68k having run zero words from $12FFF20C up to there. The shell's bus error handler,
disassembled from a MAME RAM dump (`mac.ramdump=70,4,...`, the beat before the fault), is at $54C08: a supervisor
frame goes to $54C52, which calls $59850 (the page fault handler) and resumes with RTE when it returns 0, or takes the
error path at $54CF6 otherwise. $59850 checks the SSW (DF, FC 1 or 2), finds the region of the fault address and calls
$5A866 to bring the page in; $5A866 first walks the process's 68030 tables in software ($56062: the root from the proc
structure, three levels, `BFEXTU` on the address) for the page's descriptor. The PC trails tell the two machines apart
there: the MAME SE goes $5998E-$599AA-$599B4-$59B10 (page in, return 0, RTE, the MOVES again: "MOVES #2 at 0005571a"),
the real SE $5A898-$5A8C4-$5A8DA-$5A974 ($56062 found no table: "no page", the error path, $54CF6, the kernel's trap
code and the panic). Same code, so different data: the tables the shell reads are not what the kernel wrote, on the
Pi only. The likeliest cause is a stale translation in the Cortex-A53's TLB: QEMU's softmmu flushes its whole TLB at
every TTBR change and so hides a missing TLBI, which a real core, keeping each ASID's entries, does not.

`mac.mmutlb=flush` (aarch64/mmu.c `mmu_tlb_paranoid`, a diagnostic) drops every TLB entry after each change to a
space's descriptors, each install of a space, each PFLUSH and each view switch the translated code makes. On the MAME
SE it changes nothing (the MOVES is retried as before). If the SE passes this point with it, the missing TLBI is the
cause and the next step is to find which change lacks one.

The line with the owner's SE (test kernel 11): `u_procp 001bc91e, saved USP 00000000, root 0004d000, root[0] 00000000`,
the MAME SE's `root[0] 0004d40a, next 0004d20a`. The same process and the same root table at $4D000, but on the Pi the
table's first entry is still zero when the shell walks it: a write of A/UX's (the entry, which the MAME SE has by
then) is not in the Pi's copy of $4D000. That page lies in the bottom $50000 the board keeps read-only for the
IIci's frame buffer (ro_phys), so every write to it is emulated (psmac_bus_write) and translated with the view TTBR0
holds (psmac_mmu_cur_view); a write whose logical page has no translation in that view is dropped without a word.
Two checks are now logged (the first eight of each): an abort where the SR's S bit and the view in TTBR0 disagree
outside an exception's entry ("view mismatch"), and a write dropped for want of a translation ("write dropped").
The MAME SE shows neither up to init.

Test kernel 12 on the SE showed neither a view mismatch nor a dropped write. The MAME SE, with `mac.watchpage=4d000`
(the emulated writes to that physical page, from A/UX's switch on) and `mac.watchpc=10023146` (the emulated writes of
the translated block at that PC): after switch #8 the shell clears the table pages ($561C6/$561D2), then the kernel's
block at $10023146 writes the root entry with `MOVE.L D0,([-16,A6])` ($4D000 <- $0004D402), walks with $55FE6 and fills
the next level from $100231C6 ($4D400 <- $0004D202), all before the MOVES that faults. The SE's PC trail passes
$10023146 and $55FE6 but not $100231C6, and its root entry is 0. The kernel's stack is in the same low $50000 ($4CEE0),
so its pushes go through the emulated path as well.

Test kernel 13 on the SE, `mac.watchpage=4d000 mac.watchpc=10023146`: the same clearing of the three table pages, the
same push of the return address ($4CEE0 <- $10023146), and then `W4 00000000 <- 0004d402 from the block at 10023146`
where the MAME SE has `W4 0004d000 <- 0004d402`. The kernel's `MOVE.L D0,([-16,A6])` took 0 for the pointer at A6-16,
a slot of its own stack (the page at $4C000, below $50000, read through the ARM mapping and written through the
emulated path). The root table entry went to address 0, and the shell's walk found none. Next: at a write to the first
bytes of memory under A/UX's tables, the ARM store, its base register, A6, the slot's value by the walk and the walk's
view of that stack page are logged.

Test kernel 14's "low write" lines on the SE were the shell's own writes of its vectors at 0-$C (A6 $7FE20), which used
the four; the kernel's store was not among them and, this time, not among the watched block's emulated writes either
(its pointer then fell outside the low $50000). The kernel's code ($1002302x-$100231FA, from the MAME RAM dump): the
pointer is $55FAC's result, stored at A6-16 by `MOVE.L A0,-16(A6)` at $100230CC, tested at $100230E6 and used by
memory-indirect operands at $100230D4, $100230EC and $10023154. The MAME SE with `mac.watchpc=100230ca,10023146`
(up to four blocks now): $4CF14 <- $0004D000 (the slot, in the kernel's stack page $4C000), then $4D000 <- $0004D402.
The "low write" check now takes only the kernel's frames (A6 $12xxxxxx).

Test kernel 15 on the SE: `W4 0004cf14 <- 0004d000 from the block at 100230ca`, exactly as the MAME SE, and then no
emulated write from the block at $10023146 at all and no "low write": the pointer the kernel read back from $4CF14
was neither $4D000 nor below $10, and the root entry went to some ordinary page. So on the Pi a stack slot read through
the ARM mapping was not what had just been written to it through the abort path (KERNEL_ALIAS + the Pi's copy), where
QEMU, which models no caches and keeps no TLB across a TTBR change, always agreed. Which of the two views was stale is
not settled; both are cacheable inner-shareable aliases of one physical page, so the suspicion falls on a translation
the Pi kept (a stale ARM mapping of the stack's logical page) rather than on the caches.

The write-through frame buffer exists for the IIci, whose RBV scans the first 320 KiB of bank A; the SE board never
mirrored those writes (psmac_bus_write, `m = !psmac_board_se`). So ro_phys no longer keeps that memory read-only on
the SE board, the solo board and the Plus: A/UX's kernel stack and page tables are now ordinary memory, read and
written through the same mapping. The MAME SE: A/UX to init as before, emulated accesses 8.7 million to init where they
were 130 million; System 7.1 on the solo board at the Finder in 24.9 s (36 s) and 31.7 s with mac.mmu=full (45 s);
System 7.1 on the MAME SE at the Finder in 268 s.

## 19. The cause: a page's second address in the TLB (26 November 2026)

Test kernel 16 (the low 320 KiB ordinary memory on the SE board) changed nothing on the SE: the root entry was still
0 at the first fault. So the path the write took was not the fault; the read was. Three things, put together:

1. **The read is not at the address it seems to be.** The kernel's `MOVE.L D0,([-16,A6])` at $10023154 (and the
   `([-16,A6])` operands before it) is a 68020 memory-indirect operand with a word base displacement. M68k_EA.c
   translates its pointer fetch as `ldr w_bd, [x_A6, w_bd, UXTW]` with w_bd = $FFFFFFF0: the 32-bit displacement is
   zero-extended, so the ARM address is A6 + $FFFFFFF0 = $1_12FFFF14, in the shadow of the first 4 GiB that Emu68
   keeps at +4 GiB for exactly this sum (L1 entries 4-7 point at the tables of entries 0-3; mirror_page upstream,
   mmu_space_new here). `MOVE.L A0,-16(A6)` at $100230CC, an ordinary d16(An), is `stur w, [x_A6, #-16]`: $12FFFF14.
   One descriptor, two addresses, two TLB entries.
2. **A/UX moves that page under the kernel's feet, one page at a time.** The u-area and kernel stack of the running
   process are always at $12FFF000; a context switch rewrites that page's descriptor and flushes that page alone. The
   MAME SE, with a line at every map and unmap of $12FFF000: mapped on $15A000 (process 0), then "unmap, this page's
   address only", mapped on $4C000 (process 1) just before switch #8, and from there on unmapped and mapped again at
   every switch ($4E000, $4F000, ... $2269000, $4C000 ...).
3. **That unmap flushed one of the two addresses.** mmu_space_page invalidated the page with `TLBI VAE1IS` at its own
   address ($12FFF000); nothing invalidated $1_12FFF000. A real Cortex-A53 keeps that entry (its TLB holds 512), so
   the kernel of process 1 read `([-16,A6])` from process 0's stack page: whatever was in that slot there (0 in one
   run, something else in the next), never the $4D000 it had just stored through the other address. QEMU drops its
   whole TLB whenever a write to TTBR0 changes the ASID, which the two views do at every change of mode, and its TLB
   is small: it never had the stale entry, and the MAME SE went on.

It accounts for what was seen: the pointer stored right ($4CF14 <- $4D000) and read wrong; `12(A6)` read right in the
same block; the wrong value different from run to run; no difference whether the stack's writes were emulated or direct
(section 18's change); the same every boot on the Pi and never in QEMU. One observation does not fit and is not
explained: test kernel 10's `mac.mmutlb=flush` drops every TLB entry at each of these changes and should have cured
it, and the panic was reported with it. That boot's log does not show the option's line (the card's log keeps the last
64 KB), so whether it was on the command line is not known.

The fix (aarch64/mmu.c, tlbi_page_everywhere): a page replaced or removed is invalidated at its own address, at
+4 GiB, and at the kernel's alias of it at the top (which mac.mmu=full has emptied, and the older PFLUSH path
has not). The same omission was in mmu_low4g_page, the path of the mode without the option.

And a check the hardware alone can fail (psmac_mmu.c, alias_selfcheck), at the start with mac.mmu=full: a scratch
page is moved from one page of RAM to another and read at +4 GiB, once with the flush as it was and once as it is, and
the log says which page each read saw: "[PSMAC:MMU] TLB self-check: ...". QEMU: "new" and "new". A Pi is expected
to say "OLD" and "new"; if it says "new" to both, this section is wrong about the cause.

System 7 with virtual memory on moves pages and flushes them one at a time too; the same stale reads were waiting
there.

## 20. Past the panic on the SE; asleep in STOP with nobody to wake it (27 November 2026)

Test kernel 17 (section 19's flush) on the owner's SE: no panic. The log at the end: 350 bus errors delivered, the
first four the MAME SE's own (#2 `PC 0005571a w fffffffc`, #3 a fetch at $A8 from user mode), 544 PFLUSHA and 490
PFLUSH by address, 17,908 MOVES, 139 switches, 12 spaces: A/UX's first processes ran. The MAME SE at the same counts
(333 delivered, 539/504, 17,774 MOVES, 170 switches) is idle for about ninety seconds, taking the clock's and the
VBL's interrupts, and then goes on. The SE stopped there: no instruction run, no bus cycle, no interrupt taken in
any heartbeat, the PC sample $54912 and SR $270D of the last block, the return address on the stack $10028A44. The
SE's screen blank (the MAME SE shows init's lines only later).

$10028A16 is the kernel's swtch: with no process to run it calls the standalone shell's idle routine ($548E0), which
is `MOVE.W #1,$5AFA0; STOP #$2000` in a loop until a flag is set ($548D4-$54918). The translated STOP (M68k_LINE4.c,
EMIT_STOP) sleeps in a WFE until the context's interrupt word is not zero. A WFE sleeps until an event; upstream's
housekeeper sends one (SEV) with each level it stores (pistorm/ps_protocol.c), and this fork's stores of the level
(psmac_board_se.c store_level, psmac_board_solo.c, psmac_bus.c) sent none. QEMU treats WFE as a no-op. On the Pi a
stopped 68k woke when something else happened to send an event (the bus layer's own SEV, the USB core's) or an ARM
interrupt was pending; with the clock's level asserted ("ipl 1") and the slot's too (RBV IFR $12, IER $0A), A/UX slept.
Mac OS never executes STOP, so nothing before A/UX depended on it. That the same loop woke earlier in this boot (the
shell's waits before the kernel) is not explained in detail: every bus cycle of the 68k's sends the SEV that sets its
own event register, which lets one WFE through.

The changes: PSMAC_STORE_IPL (psmac.h) sends an event when a level that is not zero and not the one before is stored,
on the SE, solo and bridged boards, and M68kReportInterrupt with it; core 0's timer event stream is on from the 68k's
start (CNTKCTL_EL1, bit 13: 0.85 ms), so a stopped 68k looks at its interrupt word that often whatever else happens;
and the heartbeat says how long the 68k was in STOP and, if a level is asserted with no instruction run, that it was
not woken.

## 21. A/UX to its Finder: an instruction across two pages, and the Pi's windows in the 68k's space (27 November 2026)

Section 15 left A/UX in the MAME SE at init with user programs dying of "Memory fault"; sections 16 to 20 were the
real SE's own faults, none of which shows in QEMU. This section is the Memory fault, found in the MAME SE
(`PSMAC_RAM=64`, `mac.mmu=full`, a fresh copy of the disk for every run), and a second fault behind it. With both
mended A/UX 3.1.1 starts its Macintosh environment and reaches its Finder.

### 21.1 Where a program died, and with what

**A trace of user mode.** `mac.usertrace=PATH` (new, the testbed only) keeps a ring of the last 131,072 blocks the 68k
entered in user mode (PC, SR, A7, A6, A0, A1, D0, D1 at each block's start; the JIT loop stores them, nothing is
called) and the bus errors delivered among them. The first time a user program's write below $1000 is refused, or any
access of its at $BAD0xxxx, the 68k stands still while another core writes the ring to PATH and the Mac's memory to
PATH.ram; the log gets the registers and the MMU's (TC, CRP, SRP), which is what a table walker on the host needs. The
heartbeat's MMU report also says which ARM instructions the faults "not delivered" came from and who called them.

**Run 50, the kernel as section 20 left it.** `/etc/rc: 107 Memory fault - core dumped` as before, and the trace did
not fire: no write below $1000. The core, read from the disk image, is chown's (`/bin/chown adm /etc/wtmp`), and the
frame at the top of its kernel stack ($12FFFFA2) is a format $B bus error: **PC $1FFE, SSW $0141 (a data read in user
mode), fault address $BAD0BAD0**; D0 and A0 $004030E5, A6 $FFFFFB34, USP $FFFFFB18. This is not the frame section 15.2
described (PC $2DC2, a write to $1E0), which was not seen again. $BAD0BAD0 is what the abort path answers for a read
of an empty bus: the program had been given that value as an address. The instruction at $1FFE is
`MOVE.L $004030F0,$004030F8` (`23F9 0040 30F0 0040 30F8`), in malloc's first call: ten bytes, the opcode in the last
word of page $1000 and both addresses on page $2000. Run 50's 1,381 "not delivered" faults by the time of the crash
were all reads by `cache_read_64` (1,003), `cache_read_32` (217) and `cache_read_16` (161): the code cache's checksum
and fingerprint, and the translator.

### 21.2 The first cause: the length of an instruction, read from the wrong place

A translated block stays inside one 4 KiB page, and an instruction that runs from a valid page into one the tables
refuse must take the fetch's bus error instead of being translated (`psmac_mmu_jit_fetch`, section 12.4). To know
whether it runs over, the check copied the words left in the page to an array on the kernel's stack, zeros after
them, and asked `M68K_GetINSNLength` for the length of that. But the length decoders read their instruction through
`cache_read_16`, by its **32-bit 68k address**: the array's address, cut to 32 bits, is some address in the 68k's own
space ($0007FBC8 in this kernel). The decoder never saw the copy. Under A/UX it read the running program's text at
$7FBC8 or, for a small program, a hole: $BAD0, `CMPA.W (A0),A5`, one word. One word fits in what is left of any page,
so no crossing was seen, the translator went on, read the instruction's remaining words from the refused page
(another "not delivered" read, $BAD0BAD0 each) and compiled `MOVE.L $BAD0BAD0,$BAD0BAD0`.

The log of run 51, with the old answer computed beside the new one:

```
[PSMAC:MMU] fetch check at 00001ffe (opcode 23f9, 1 words left in its page): the copy at 0xffffff800007fbc8 read as bad0 said 1 words, the instruction itself 5
```

eight such lines in that boot: $1FFE four times (chown's instruction, by its opcode), $8FFC and $AFFE twice each in
other programs. An objdump of the binaries shows who is exposed: chown has four instructions across a page boundary
($FFC, $1FFE, $2FFA, $6FFE), getty five ($1FFE, $2FFE, $3FFE, $5FFE, $8FFC), the shared C library `libc1_s` two.
Which of them goes wrong depends on which pages are in memory when the block is first translated, which is the same
on every boot of the same disk: hence chown, pid 107, every time. System 7's Virtual Memory did not show it: there the
68k's low memory is ordinary RAM, and what lay at that address was not a one-word instruction when it mattered
(*inference*; it was not looked for). mmutest's T26 has its whole instruction on the refused page.

**The fix.** The decoders are given the instruction where it is. A word they need from the refused page is an ARM
abort like any other; while the check runs (`fetch_probe`), `psmac_mmu_fault` notes it instead of counting a fault
"not delivered", and the check then says "runs over" if a word was refused or the length exceeds the page.

### 21.3 The second cause: the Pi's own windows inside the 68k's 4 GiB

Run 51 (the first fix): no Memory fault from `/etc/rc`, and init went on to start A/UX's Macintosh environment, whose
programs then died. The trace fired in `/mac/bin/CommandShell -u`: a byte written to address 0 by `strcpy` in the
shared library ($47F059FC) after three `brk` calls had come back with EINVAL; the last core on the disk was
`/mac/bin/startmac`'s, `getopt` reading `argv[1]` as 0. Both programs have their stack just below $40000000
(A7 $3FFFFBB4, where an ordinary A/UX program's is just below $00000000 of the next lap, $FFFFFxxx). In the RAM dump
the stack page the tables name for $3FFFF000 (frame $02477000) holds what the kernel had put there, the arguments and
the environment, and **nothing a user instruction had written**: zeros where the trace shows return addresses and
frame pointers in use, and those values are nowhere in the Mac's 64 MB. So the program's own stack accesses did not go
where the 68030's tables say, while the kernel's (MOVES, through the tables) did: `brk` read its argument from the
kernel's view of the stack, 0, hence EINVAL.

Upstream Emu68 maps the Pi's peripherals inside the first 4 GiB "so that they can be accessed from m68k"; this fork
keeps that window at $3F000000 (docs/08 item 21) and the address spaces of section 12 copied it, and everything else
the kernel had mapped below 4 GiB, into every space. The log now lists what that was:

```
[PSMAC:MMU] the Pi's windows in the first 4 GiB, which the 68k's address spaces leave out:
[MMU]   35000000-35ffffff: blocks at 35000000, attributes 74c
[MMU]   3c000000-3fffffff: blocks at 3c000000, attributes 74c
[MMU]   40000000-401fffff: pages, table at 3bffc000, attributes 000
[MMU]   dea00000-debfffff: pages, table at 3bff9000, attributes 000
```

(the VideoCore's frame buffer memory, 16 MiB; the VideoCore's memory and, from $3F000000, the peripherals; the
interrupt controller; the page of Emu68's debug port). The
peripherals are mapped without access from EL0 (`PERIPH_EL0`), which docs/08 took to keep the 68k out; but translated
code runs at EL1, the kernel's own level (its blocks write TTBR0_EL1 themselves, section 12.2), so that bit keeps
nobody out. A 68k access to $3F000000-$3FFFFFFF went straight to the Pi's peripheral space, past the 68030's tables;
`sp_map4k` refused to map a 68k page there, and `mmu_space_page` refused to replace a block. Under QEMU the unassigned
part of that space behaves as memory (*inference*, from a stack that worked for the program that used it); on a Pi it
is the peripherals' bus. Mac OS has nothing at those logical addresses. A/UX's Macintosh environment has the stack of
every one of its programs there.

mmutest's new T31 (the tables map $3FFFF000 and $40110000 onto frames of RAM; a long written through each, a long
read through each) on the kernel with the first fix only: `155 passed, 2 failed`, `t31write=00000000
t31read=00000000`: the write through $3FFFFFF0 never reached its frame and the read saw none of it. ($40110000
passed: that window is a table with one page in it, not blocks, so the access aborted and the abort path emulated it
through the 68030's tables.)

**The fix.** With `mac.mmu=full` a space carries none of the Pi's windows (`no_windows`, psmac_mmu.c): the whole
4 GiB is the 68k's, and a page at $3FFFF000 is walked and mapped like any other. The kernel's own code does not need
the windows there: the fork's drivers have used the peripherals' mapping in the kernel's half (`PSMAC_PERIPH_HI`)
since docs/08 item 21, and the VideoCore's memory through `KERNEL_ALIAS`. One user was left on the 68k's core:
upstream's firmware mailbox in support_rpi.c, which `psmac_vc_setup` calls when the Mac's screen is set up (four
accesses to $3F00B880-$3F00B8A0 in the first run without the windows, served by the abort path); its base is now a
variable that moves to the kernel's mapping when the first space is installed. Two nets for what may be left: the
abort path serves the kernel's own access to the peripherals' low window through the kernel's mapping
(`periph_own`, psmac_bus.c), and the first four accesses by kernel code to where the windows were are logged with
their caller (`the kernel's code at ... where the Pi's windows were`). None in the runs below. Without the option
nothing changes: the stock path's sets keep the windows, and a 68k access to $3F000000 still reaches the Pi.

### 21.4 How far A/UX gets now

Run 52 (both fixes), from a fresh disk: A/UX Startup, the kernel, init, and then no text console at all: `startmac -o
minimal` takes the screen for A/UX's own start-up dialog, "Welcome to The Apple Workgroup Server 95 / Starting
background processes..." with its progress bar and Messages button (screenshot 52 of the run, about 520 s), the
Macintosh environment starts (the desktop pattern, extensions' icons along the bottom, 590 to 650 s), no login dialog
is shown (this disk logs in as root by itself: startmac's environment in run 51's core has `LOGNAME=root`), and
**the A/UX Finder's desktop is complete at about 740 s**: the menu
bar (File, Edit, View, Label, Special), the "/" disk, "Macintosh HD" and the Trash, unchanged to the end of the run at
1,150 s. No core file on the disk afterwards. The kernel's counts at the end: 4,753 bus errors delivered (3,442 data,
672 operand, 639 fetch), 192,567 privileged instructions trapped from user mode (the Macintosh environment runs Mac OS
in user mode and A/UX emulates what it may not do), 3.4 million MOVES, 17,306 context switches over 27 address
spaces, none reused. The trace fired once, at `startmac`'s first write to address 0, which A/UX answered by giving the
program its page there (the Macintosh environment's low memory): that trigger is a crash only in an ordinary A/UX
program.

The 1,398 faults "not delivered" of that run, by the ARM instruction and its caller: 1,338 are the code cache's
checksum and fingerprint (`CalcCRC32`, and one read beside it), 60 the evidence line above, taken out since. The
checksum's are of a block checked again after its page has gone, and of a new block whose range runs into a refused
page: 86 of them were called for a new block, at $10004528-$1000453C (*inference*: the translator follows a jump to
see where the block ends, and the checksum's range then reaches the target on the next page; such a checksum is of
$BAD0s and fails when the page comes, which costs a translation and nothing else). None is an access of the
program's.

**getty.** The getty that section 15 saw dying is the console's: inittab's `co` line respawns `/etc/loginrc`, which
runs `/etc/macgetty` and falls back to `/etc/getty console co_9600`; the two serial gettys (`tty0`, `tty1`) are `off`
in this disk's inittab. No getty's core was caught in these runs (run 50's core was still chown's at 700 s), so its
frame was not compared with chown's; getty has five instructions across a page boundary and its fault came, like
chown's, with nothing but paging going on. With the fixes no getty dies and nothing touches a serial device.

### 21.5 Tests (the final kernel of this section)

- mmutest, solo board, `mac.ram=8`, `mac.mmu=full`: **157 passed, 0 failed, 127 bus errors taken**, none undelivered
  (143 before; T30, ten checks: an instruction whose first word ends a valid page with its immediate, or its
  full-format extension word, on a refused one: a fetch fault with the refused page as the stage B address and the
  instruction's own PC, then the right result; T31, four checks, above). Without the option `16 passed, 35 failed, 0
  bus errors taken` and the benchmark at 415-428 MIPS, as before (392-402 with the option).
- The negative controls. T31 on the kernel before its fix: above. T30 on a kernel with the old length check put
  back: the test never reports; the 68k runs `MOVE.L #$BAD0BAD0,D0` and whatever follows and ends in `unexpected
  exception, format/vector 00000028 at PC 001D0000`. (T30 first fills the machine's unused memory with NOPs, so
  that the old check finds a one-word instruction at its copy's address whatever the kernel's stack is.)
- System 7.1 on the solo board (a copy of the test image): the Finder at 23.8 s without the option, 27.0 s with it,
  no fault undelivered, no access by the kernel's code where the windows were.
- A/UX in the MAME SE with the final kernel, `mac.mmu=full` alone (run 53, 960 s, a fresh disk): the same course,
  the Finder's desktop complete at about 700 s (the same picture as run 52's, byte for byte) and unchanged to the end;
  no core file on the disk; 4,753 bus errors delivered (3,441 data, 673 operand, 639 fetch), 135,496 privileged
  instructions trapped from user mode, 2.7 million MOVES, 12,741 switches over 28 spaces; 1,335 faults "not
  delivered", every one the checksum's or the fingerprint's; no access by the kernel's code where the windows were.
- `tools/diag_test.sh` (the options table, with `mac.usertrace`): 7,075 checks, 0 failed.

### 21.6 What is not settled

- The real SE. Both faults are the emulator's own and do not depend on the hardware, but what a Pi's peripheral space
  did with a 68k program's stack was never seen there (A/UX on the SE had not got that far), and whether any kernel
  code on the 68k's core still reaches for a low window on real hardware (a driver QEMU does not exercise) will show
  in the log line named above.
- The frame section 15.2 read in chown's core (PC $2DC2, a write to $1E0) was not reproduced; chown has a second
  instruction across a page boundary at $2FFA, in the function that frame was in, and a block compiled with $BAD0
  words in other places would fail in other ways (*inference*).
- The translator's other reads ahead of what it translates (a second MOVE to fuse with, the condition codes'
  scan of section 15.1 item 6) still meet refused pages and read $BAD0; each was looked at and none of them changes
  what is emitted for the instruction in hand, but the "not delivered" count is not zero and a new one would hide
  among the checksum's.
- A/UX's memory table on 128 MB (15.2), the SE's level-4 storm (15.3), and everything past the Finder's desktop:
  no program was started, nothing was typed (the testbed has no input for A/UX yet), networking and the serial
  ports were not touched.

## 22. The SE at A/UX's boot dialog: the SCC's interrupt masked, and the clock hidden behind it (27 November 2026)

Test kernel 19 (section 21's two fixes) on the owner's SE: no "Memory fault"; A/UX's own dialog, "Welcome to The
Apple Workgroup Server 95 / Initializing device drivers...", the bar at three quarters, and there it stayed. The log's
heartbeats, all alike: the 68k in STOP for the whole five seconds, level 2 taken 301 times (the screen's VBL, from
the RBV this side answers), level 1 not once, no cycle on the wire, and the wire's level reading 2. By then 2,811
bus errors delivered, 918 switches, 311,125 MOVES: A/UX's Macintosh environment was starting.

On an SE the wire's level is not two independent lines. The VIA asks at level 1 and the SCC at level 2, and while
the SCC asks the level reads 2 whatever the VIA does (the Plus's and the SE's interrupt logic gates the VIA's line
with the SCC's; Guide to the Macintosh Family Hardware). The logs agree: in 268 heartbeats from this SE the wire
said 0, 1 or 2, never 3. So psmac_se_ipl, which reads bit 1 as the SCC and bit 0 as the VIA, sees no VIA at all
while the SCC asks. `mac.sccirq=aux` (section 14) kept the SCC's level from the 68k under A/UX's tables: once A/UX's
own serial driver had enabled the chip's interrupts, nobody took them, the line stayed up, the VIA's 60 Hz never
showed, and A/UX slept in its idle loop without a clock. The MAME SE runs without the option and A/UX serves its SCC.

So the option had become the fault it was a diagnostic for. `aux` now masks nothing (the option's line in the log
says so), and what it was for, an SCC asking with no taker during the standalone shell's stage, is the storm guard's:
when the guard trips on level 4 (20,000 entries in a row with nothing done), the service core takes the SCC's master
interrupt enable away (WR9 = 0, after a read of the control register to reset its pointer), so the line drops and the
VIA shows again; a driver that programs the chip sets it back. One line in the log, the first four times.
