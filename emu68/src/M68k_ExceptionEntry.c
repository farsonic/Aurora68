/*
    Copyright © 2019-2025 Michal Schulz <michal.schulz@gmx.de>
    https://github.com/michalsc

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#include <stdint.h>
#include <A64.h>
#include <M68k.h>
#include <support.h>
#include <stdbool.h>
#include <globals.h>

#ifdef PSMAC
extern int psmac_mmu_full;                  /* psmac/psmac_mmu.c */
extern volatile uint32_t psmac_priv_user;   /* privilege violations from user mode under mac.mmu=full */
struct psmac_crash_args { uint32_t vec, pc, sr, ea, fault, usp, isp, regs[16]; };   /* psmac/psmac_crash.c */
#include "psmac/psmac.h"                    /* psmac_view_for, psmac_views: mac.mmu=full's user and supervisor views */
#endif

static inline struct M68KState *getCTX()
{
    struct M68KState *ctx;
    __asm__ volatile("mov %0, "CTX_POINTER_ASM:"=r"(ctx));
    return ctx;
}

/*
    type_and_format: 16 bit value. Bits 0..11 - exception type, bits 15..12 - exception format
*/
uint32_t M68K_Exception(uint32_t M68k_SR, uint32_t type_and_format, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t ea, uint32_t fault)
{
    register uint32_t M68k_PC __asm__("w18");
    register void *M68k_A7 __asm__("x29");

#ifdef PSMAC
    {   /* Aurora68: the crash record (psmac/psmac_crash.c). Only two kinds of exception go there:
           the unexpected ones, and the A-line trap that is _SysError ($A9C9, with or without the
           auto-pop bit), which draws the System's error dialog. Every other A-line trap is a
           Toolbox call and costs one read of its trap word here; the decoder has just read that
           word from the same address, so it is mapped. */
        uint32_t vec = type_and_format & 0x0fff;
        int record;
        if (vec == 0x28)
            record = ((*(volatile uint16_t *)(uintptr_t)(M68k_PC & ~1u)) & ~0x0400) == 0xA9C9;
        else if (vec == 0x2c && globals.fpu_disable &&
                 ((*(volatile uint16_t *)(uintptr_t)(M68k_PC & ~1u)) & 0x0e00) == 0x0200) {
            /* mac.fpu=none: an FPU instruction (coprocessor 1) on a Mac without one is the probe of the ROM,
               of Gestalt's callers or of a program looking for an FPU, not a crash: counted, not recorded */
            extern volatile uint32_t psmac_fpu_traps, psmac_fpu_trap_pc;
            psmac_fpu_traps++; psmac_fpu_trap_pc = M68k_PC;
            record = 0;
        }
        else if (vec == 0x20 && psmac_mmu_full && !(M68k_SR & SR_S)) {
            /* mac.mmu=full (docs/23-mmu.md): System 7's Virtual Memory runs the Mac in user mode and emulates every
               privileged instruction it meets there, so these are its ordinary work, thousands a second: counted */
            psmac_priv_user++;
            record = 0;
        }
        else
            /* not the autovectors, not the TRAP #n instructions, and not the FPU's exceptions (vectors 48-54, $C0-$D8):
               those are taken only when a program has enabled them in the FPCR, with its own handler, as the exact
               FPU's test INIT does (22 November 2026, issue #27) */
            record = !(vec >= 0x64 && vec <= 0x7c) && !(vec >= 0x80 && vec <= 0xbc) && !(vec >= 0xc0 && vec <= 0xd8) && vec < 0x100;
        if (unlikely(record)) {
            /* The 68k's registers live in fixed ARM registers (M68k.h, REG_D0...), which this file
               is compiled to leave alone; the recorder is ordinary C, so the call saves every
               register the C convention lets it change (x0-x11, x13-x18, x30) and restores them. */
            struct psmac_crash_args a;
            __asm__ volatile(
                "stp w19, w20, [%0, #0]\n\t"  "stp w21, w22, [%0, #8]\n\t"
                "stp w23, w24, [%0, #16]\n\t" "stp w25, w26, [%0, #24]\n\t"
                "stp w13, w14, [%0, #32]\n\t" "stp w15, w16, [%0, #40]\n\t"
                "stp w17, w27, [%0, #48]\n\t" "str w28, [%0, #56]"
                : : "r"(a.regs) : "memory");
            a.regs[15] = (uint32_t)(uintptr_t)M68k_A7;
            __asm__ volatile("umov %w0, " REG_USP_ASM : "=r"(a.usp));
            __asm__ volatile("umov %w0, " REG_ISP_ASM : "=r"(a.isp));
            a.vec = vec; a.pc = M68k_PC; a.sr = M68k_SR; a.ea = ea; a.fault = fault;
            __asm__ volatile(
                "sub sp, sp, #160\n\t"
                "stp x0, x1, [sp, #0]\n\t"     "stp x2, x3, [sp, #16]\n\t"
                "stp x4, x5, [sp, #32]\n\t"    "stp x6, x7, [sp, #48]\n\t"
                "stp x8, x9, [sp, #64]\n\t"    "stp x10, x11, [sp, #80]\n\t"
                "stp x13, x14, [sp, #96]\n\t"  "stp x15, x16, [sp, #112]\n\t"
                "stp x17, x18, [sp, #128]\n\t" "str x30, [sp, #144]\n\t"
                "mov x0, %0\n\t"
                "bl psmac_crash_exception\n\t"
                "ldp x0, x1, [sp, #0]\n\t"     "ldp x2, x3, [sp, #16]\n\t"
                "ldp x4, x5, [sp, #32]\n\t"    "ldp x6, x7, [sp, #48]\n\t"
                "ldp x8, x9, [sp, #64]\n\t"    "ldp x10, x11, [sp, #80]\n\t"
                "ldp x13, x14, [sp, #96]\n\t"  "ldp x15, x16, [sp, #112]\n\t"
                "ldp x17, x18, [sp, #128]\n\t" "ldr x30, [sp, #144]\n\t"
                "add sp, sp, #160"
                : : "r"(&a) : "memory", "cc");
        }
    }
#endif

#ifdef PSMAC
    /* mac.mmu=full (docs/23-mmu.md section 12): the frame goes onto the supervisor stack through the supervisor's view,
       and a fault while it is pushed or the vector read is the 68030's double bus fault (psmac_mmufault.c), not a bus
       error and not a write into nothing */
    if (unlikely(psmac_mmu_full)) {
        psmac_view_for(SR_S);
        psmac_views.guard = 1;
    }
#endif

    /* If we are not in supervisor mode, swap stacks */
    if ((M68k_SR & SR_S) == 0) {
        /* Store current A7 to USP */
        __asm__ volatile("mov "REG_USP_ASM", %w0": :"r"(M68k_A7));
        /* If SR_M is set, load MSP into A7, otherwise load ISP */
        if (unlikely(M68k_SR & SR_M)) {
            __asm__ volatile("mov %w0, "REG_MSP_ASM:"=r"(M68k_A7));
        }
        else {
            __asm__ volatile("mov %w0, "REG_ISP_ASM:"=r"(M68k_A7));
        }
    }

    /* Depending on exception format prepare place on stack frame and eventually push extra parameters */
    if (unlikely((type_and_format & 0xf000) == 0x2000 || (type_and_format & 0xf000) == 0x3000))
    {
        // format 2 and format 3 - store EA
        __asm__ volatile("str %w1, [%0, #-4]!":"=r"(M68k_A7):"r"(ea),"0"(M68k_A7));
    }
    else if (unlikely((type_and_format & 0xf000) == 0x4000))
    {
        // format 4 - store fault address and EA
        __asm__ volatile("str %w1, [%0, #-8]!\t\nstr %w2, [%0, #4]":"=r"(M68k_A7):"r"(fault),"r"(ea),"0"(M68k_A7));
    }

    uint32_t tmp = M68k_SR;
    if ((tmp & 3) != 0 && (tmp & 3) < 3)
    tmp ^= 3;

    __asm__ volatile(
        "strh %w1, [%0, #-8]!   \n\t"
        "str  %w2, [%0, #2]     \n\t"
        "strh %w3, [%0, #6]"
    :"=r"(M68k_A7)
    :"r"(tmp),
     "r"(M68k_PC),
     "r"(type_and_format),
     "0"(M68k_A7)
    );

    /* Clear trace flags, set supervisor */
    M68k_SR |= SR_S;
    M68k_SR &= ~(SR_T0 | SR_T1);

    uint32_t vbr = getCTX()->VBR + (type_and_format & 0x0fff);
    M68k_PC = *(uint32_t *)(uintptr_t)vbr;
#ifdef PSMAC
    psmac_views.guard = 0;
#endif

    __asm__ volatile("": :"r"(M68k_PC));

    return M68k_SR;
}
