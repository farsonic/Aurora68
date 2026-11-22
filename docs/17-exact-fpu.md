# The exact FPU: a 68882 in extended precision

22 November 2026, GitHub issue #27. This is the design of `mac.fpu=exact`, the PSMAC build's FPU that keeps
the 68881's 80-bit extended format to the last bit, and the record of how it was built and tested.

## Why
Emu68 translates the 68k's FPU instructions into AArch64 floating point: FP0 to FP7 live in the NEON
registers d8 to d15 as 64-bit doubles, and `Load96bit`/`Store96bit` (`M68k_LINEF.c`) convert the 96-bit
extended format of memory on the way in and out. That is fast and right for most programs, but a double
has a 53-bit significand where the 68881 has 64, and an exponent range of 11 bits where it has 15. On
the Macintosh this is not a corner case: SANE, the Mac's numerics, computes in extended precision, and the
IIci ROM's SANE (PACK 4 and 5 for a 68881/68882) does so with FPU instructions. Photoshop's start-up
refined a value until new minus old was zero in extended precision and, in doubles, never got there
(docs/08 item 101). The fix that day, `mac.sane` (the SE ROM's integer SANE in place of the IIci's),
is exact and 20 to 60 times slower; programs that use the FPU directly (Excel, Mathematica, the
Speedometer FPU tests, anything compiled for a 68881) still got doubles.

What was wanted: the 68881's results bit for bit, its flags (the FPSR's exception and accrued bytes, the
condition codes, the quotient byte), its rounding modes and precisions, its NaNs and denormals, FMOVEM,
FSAVE and FRESTORE as the Mac uses them, and every instruction a 68882 has, transcendental ones
included, with the double-precision path kept as an option.

## The pieces
| | |
|---|---|
| `emu68/src/psmac/psmac_fpu.c` | the instructions: decoding, effective addresses, the data formats, the arithmetic on SoftFloat, FPCR/FPSR/FPIAR, FMOVE(M), FScc/FDBcc/FTRAPcc/FBcc, FSAVE/FRESTORE, `mac.fpu` |
| `emu68/src/psmac/psmac_fpu_math.c` | the elementary functions (FSIN ... FTENTOX), FMOVECR's constants, the packed decimal format |
| `emu68/src/psmac/psmac_fpu_consts.h` | constants to 128 bits and 2/pi to 16,700 bits, made by `tools/mkfpuconsts.py` |
| `emu68/src/psmac/third_party/softfloat/` | Berkeley SoftFloat 3e, BSD 3-clause, with a 68881 specialization (its `README.md`) |
| `emu68/src/M68k_LINEF.c`, `EMIT_FPU_exact` | the JIT's half: the call, and the exceptions it returns |
| `tools/fpu_test.sh`, `fpu_test.c`, `fpu_refcheck.py` | the host tests |
| `mac/fputest.s` | "Aurora68 FPU Test", the testbed's check from the 68k's side (`make -C mac fputest`) |

## How an instruction runs
The JIT keeps the 68k's integer registers in fixed AArch64 registers (`include/RegisterMapping.h`: PC in
w18, D0-D7 in w19-w26, A0-A4 in w13-w17, A5-A7 in w27-w29) and the SR in a register its allocator holds.
With `mac.fpu=exact`, `EMIT_lineF` hands every coprocessor-1 instruction to `EMIT_FPU_exact`, which emits:

1. the pending PC committed to w18 (`EMIT_FlushPC`), and the SR loaded into a register (`RA_ModifyCC`)
   before any branch, so that every path out of the instruction agrees on where it is;
2. x0 to x30 stored on the stack, as the 68030 MMU shim's calls already do (`psmac_emit_mmu_call`);
3. a call to `psmac_fpu_exec(regs, sr)` with x0 pointing at that save area, which is therefore the
   68k's register file for the C side: it reads the instruction at regs[18], computes its effective
   address from the saved registers, reads and writes the operands in the 68k's memory (the 68k's
   addresses are the kernel's own on core 0, exactly as the translated loads and stores use them, and a
   bus address faults to the same handler as theirs), writes back any address register an (An)+ or
   -(An) changed, and does the arithmetic;
4. the registers loaded again, the call's result in a register of the allocator's;
5. if that result is not zero, the exception it names taken there and then: (format << 12) | the vector's offset (vector times 4, `M68K_Exception`'s convention) in
   w1, the SR in w0, the instruction's address pushed as a format 2 frame's address field (FTRAPcc's),
   `M68K_Exception` called, the block left through a local exit, as `EMIT_TRAPcc` leaves it;
6. otherwise, for an instruction that only computes, the block goes on; for FBcc, FDBcc and FTRAPcc
   it ends. Either way the C side has already left x18 at the next instruction (whose address it knows
   from `psmac_fpu_length`, as the translator does), a branch's target or the exception's PC, so the
   JIT emits no `EMIT_AdvancePC` after the call: a pending PC offset at that point would be flushed into
   the exception path's exit only and lost on the normal path (22 November 2026).

So there are no FPU registers in the AArch64's NEON file any more: the state is a C structure
(`psmac_fpu`: eight `extFloat80_t`, FPCR, FPSR, FPIAR, and whether the FPU is in its null state).
`psmac_fpu.c`, `psmac_fpu_math.c` and SoftFloat are compiled with `-mgeneral-regs-only`: core 0's
floating-point registers stay the JIT's and the double-precision FPU's.

## The arithmetic
SoftFloat's `extFloat80_t` is the 68881's extended format (sign, 15-bit exponent, 64-bit significand with
its integer bit), and its `extF80_roundingPrecision` (32, 64, 80) is the FPCR's precision control exactly:
the significand rounded to 24 or 53 bits, the exponent range left extended. Its rounding modes are numbered
as the FPCR's (nearest, zero, minus, plus) and its flags are the IEEE exceptions. What is the 68881's own
is added in `psmac_fpu.c`:

- **NaNs.** The default NaN is `7FFF FFFFFFFF FFFFFFFF`; of two NaN operands the destination's is kept;
  a signalling NaN is quieted (bit 62) and raises SNAN, not OPERR (the specialization, and `set_exc`).
- **The FPSR.** The exception byte of each arithmetic instruction (BSUN SNAN OPERR OVFL UNFL DZ INEX2
  INEX1) from SoftFloat's flags, the accrued byte from it by the 68881's rules (IOP from SNAN or OPERR,
  UNFL only with INEX2, INEX from INEX1, INEX2 or OVFL), the condition codes N Z I NAN from the result,
  the quotient byte from FMOD and FREM.
- **The memory format's exponent zero.** The 68881 reads a biased exponent of 0 as 2^-16383 (Apple
  Numerics Manual: v = (-1)^s 2^(e-16383) (i.f) for every e below 32767), SoftFloat (like the 8087) as
  2^-16382. Values are converted where they cross memory: a 68881 exponent-0 number becomes a SoftFloat
  denormal of the same value and back, unnormals are normalized on the way in. One bit is lost for
  exponent-0 numbers with the integer bit clear and the last bit set, below 2^-16382; nothing at all for
  any number a program is likely to meet.
- **Which operations round to which precision.** Everything that writes an FP register rounds to the
  FPCR's precision (FMOVE, FABS and FNEG included), the 68040's FSxxx and FDxxx to single and double,
  FSGLMUL and FSGLDIV to single; FMOVE.X out is exact.
- **FMOD and FREM** by an exact long division of the significands (the quotient's low seven bits and
  sign kept), rounded once; FSCALE, FGETEXP and FGETMAN on the unpacked number.
- **Integers out** (FMOVE.B/W/L) in the FPCR's rounding mode; out of range, OPERR and the largest
  integer of the operand's sign; a NaN, OPERR and the NaN's leading significand bits.

The elementary functions are in `psmac_fpu_math.c`, in SoftFloat's 128-bit format (113 significant bits)
with one rounding at the end to the FPCR's precision and mode: argument reduction (Cody and Waite's split
ln 2 for the exponentials; Payne and Hanek's 2/pi for the trigonometric functions, so that FSIN of
2^16383 is right; three angle halvings for FATAN), short series, and exact answers where the 68881 is
exact (FSIN 0, FETOX 0, FTWOTOX of an integer, FLOG2 of a power of two, FLOG10 of 10^0 to 10^27).
FMOVECR's constants are stored to 128 bits and rounded on use, so FMOVECR #0 in round-toward-zero is
pi's lower neighbour and INEX2 is set for every irrational one, as on the chip. The packed decimal format
(FMOVE.P, static and dynamic k-factor) uses the same arithmetic and FMOVECR's powers of ten.

The 68881's transcendental functions are not correctly rounded themselves (Motorola specifies about an
ulp); these are, to within about 2^-45 of a rounding boundary. A program will see answers at least as
good as a real 68882's, not always the chip's own last bit.

## FSAVE, FRESTORE and the null state
The Mac presents a 68030, whose FPU is a 68881 or 68882, and Gestalt's 'fpu ' (IIci ROM 0x40816600) does
`fnop; fsave -(sp)` and reads the frame's first word: $1F38 is a 68882 (docs/08 item 46). The exact FPU
keeps the real chip's two states: after reset, or FRESTORE of a null frame, FSAVE writes the 4-byte null
frame; after any other FPU instruction it writes the 68882's 60-byte idle frame, $1F380000 and 56 zero
bytes. FRESTORE of a null frame resets the FPU (registers NaN, FPCR, FPSR and FPIAR zero); of another
frame it steps over it. The 68k's RESET instruction resets the FPU too (`ps_send_reset`).

## Exceptions
An exception enabled in the FPCR's enable byte is taken after the instruction that raised it, with a
format 0 frame and the next instruction's PC, the way a 68040 reports post-instruction exceptions. A
68882 takes it at the next FPU instruction and describes it in a busy FSAVE frame, which this FPU does
not produce. The Mac leaves these enables off (SANE's halts are SANE's own, in software), and no program
seen so far turns them on; it is the one place where a 68882's handler would see something different.
Vectors: BSUN 48, INEX 49, DZ 50, UNFL 51, OPERR 52, OVFL 53, SNAN 54, in that priority from the top
(BSUN, SNAN, OPERR, OVFL, UNFL, DZ, INEX). FTRAPcc takes vector 7 with a format 2 frame; an undefined
opmode or addressing mode the F-line vector, 11; FSAVE and FRESTORE in user mode, 8.

## What stays as it was
- `mac.fpu=double` gives Emu68's own translation, unchanged. `mac.fpu=none` (docs/08 item 103) gives no
  FPU at all; `exact` is the default (also `mac.fpu=on`).
- The interpreter (`M68k_INT_LINEF.cpp`), which Emu68 uses only when tracing or with its cache off, still
  computes in doubles: a run with those debugging options gets the old FPU.
- Speed. Every FPU instruction is now a call into C with 31 registers saved and restored, and SoftFloat
  works in integer arithmetic. The numbers are in "Testing" below.

## Testing
Run on 22 November 2026; the record with the logs is docs/08 item 104.

- *On the host*, `tools/fpu_test.sh` builds `psmac_fpu.c` and SoftFloat natively with `tools/fpu_test.c`:
  every instruction's decoding, length and effective addresses, the PC the C side leaves (the next
  instruction unless it branches or faults), the FPCR's rounding modes and precisions, the FPSR's flags,
  FMOVEM, FSAVE/FRESTORE, conditions and the exceptions' vector offsets, the packed decimal format, 20,000
  random operations against the host's x87 (0 different), and 2,000 arguments for each transcendental
  function against 80-digit decimal (0 not correctly rounded, worst 0.500 ulp): 20,273 checks, 0 failed.
  The lines it prints of 2 or 3 ulp are the C library's own error against the same 80-digit values.
- *In the testbed*, `mac/fputest.s` ("Aurora68 FPU Test", `make -C mac fputest`, not in the release) runs
  24 checks from the 68k's side at startup, through the JIT's call, and times the arithmetic: 24 passed, 0
  failed. On a Mac without an FPU it logs that and does nothing. `make -C mac fputest-speed` builds the
  timing alone with ten times the loops, for comparing with `mac.fpu=double`, on which the checks
  themselves end in a crash (Emu68's own FPU, not changed here).
- *Speed* (solo testbed in QEMU, System 7.1, 64 MB; ticks are sixtieths of a second, and QEMU's speed is not
  the Pi's, so only the ratios mean anything): 2,000,000 FADD and FMUL on registers in 46 ticks exact
  against 1 double; 2,000,000 with memory operands (.X and .D) 71 against 3 to 4; 200,000 FSIN 210 against
  3 to 5. SANE (A68 Machine's additions a second): the IIci ROM's on the exact FPU 94,000 to 258,000, on
  `double` 379,000 to 681,000 (but rounded to double), the SE ROM's software SANE 11,500 to 26,000.
  Speedometer 3.23's FPU Benchmarks on the exact FPU: 3529 KWhetstones (4.94 against a Mac II), matrix
  multiply 0.650 s (11.03), FFT 0.283 s (9.47), average 8.48.
- *SANE.* With the exact FPU the IIci ROM's SANE is kept: `(1 + 2^-60) - 1 = 2^-60`, the exact answer, as
  the SE ROM's SANE gives and `double` does not.
