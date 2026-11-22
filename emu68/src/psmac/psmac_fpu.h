/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_fpu.h - the exact FPU: a 68881/68882 in extended precision, for the Macintosh build of Emu68
 * (issue #27, docs/17-exact-fpu.md).
 *
 * 22 November 2026. Emu68's FPU keeps the 68k's eight registers as 64-bit doubles in the AArch64's
 * NEON registers: fast, and eleven bits short of the 96-bit extended format, which is what the
 * Macintosh's SANE and every program that asks it for extended arithmetic count on. mac.fpu=exact
 * (the default) translates each FPU instruction into a call to psmac_fpu_exec, which decodes it from
 * the 68k's memory and carries it out with Berkeley SoftFloat (third_party/softfloat) on registers
 * held in this file's state, to the last bit and with the 68881's flags, rounding precisions, NaNs
 * and exceptions. mac.fpu=double keeps Emu68's own translation.
 *
 * The interface is the JIT's: psmac_fpu_exec gets the AArch64 registers as M68k_LINEF.c saved them
 * on the stack (x0 to x30; the 68k's PC in x18, D0-D7 in x19-x26, A0-A4 in x13-x17, A5-A7 in
 * x27-x29, include/RegisterMapping.h) and the 68k's SR.
 */
#ifndef PSMAC_FPU_H
#define PSMAC_FPU_H
#include <stdint.h>

/* the 68k's registers in the saved AArch64 register file */
#define PSMAC_FPU_X_PC   18
#define PSMAC_FPU_X_D(n) (19 + (n))
#define PSMAC_FPU_X_A(n) ((n) < 5 ? 13 + (n) : 22 + (n))

/* the extended value as SoftFloat keeps it (its extFloat80_t): sign and exponent, significand */
struct psmac_fpu_x { uint16_t se; uint64_t m; };

struct psmac_fpu_state {
    struct psmac_fpu_x fp[8];
    uint32_t fpcr, fpsr, fpiar;
    int null;                       /* 1 after reset and FRESTORE of a null frame: FSAVE writes a null frame */
};
extern struct psmac_fpu_state psmac_fpu;
extern int psmac_fpu_exact;         /* mac.fpu=exact (1, the default) or double (0) */

/* Carry out the FPU instruction at regs[PSMAC_FPU_X_PC]. Returns 0, or an exception for the JIT to take:
   (format << 12) | vector, with regs[PSMAC_FPU_X_PC] the PC to stack. For an instruction that
   psmac_fpu_length says goes on in the block, the PC is left alone unless an exception is returned. */
uint32_t psmac_fpu_exec(uint64_t *regs, uint32_t sr);

/* The instruction's length in 16-bit words from its first three words (0: not an instruction this
   FPU knows, which psmac_fpu_exec turns into an F-line exception); *ends is set when it changes the
   flow (FBcc, FDBcc, FTRAPcc, unknown): psmac_fpu_exec then sets the PC itself and the block ends. */
int psmac_fpu_length(uint16_t op, uint16_t w1, uint16_t w2, int *ends);

void psmac_fpu_reset(void);
void psmac_fpu_init(void);          /* reads mac.fpu (kernel only) */

/* the elementary functions and decimal conversions (psmac_fpu_math.c) work on SoftFloat's types; the
   core passes them through these, declared with SoftFloat's headers in psmac_fpu_int.h */
#endif
