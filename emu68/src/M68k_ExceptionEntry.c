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

#ifdef PSMAC
    {   /* PiStorm-SE: log unexpected exceptions (not interrupts 0x60-0x7C, traps 0x80-0xBC, A-line 0x28) */
        extern void kprintf(const char *, ...);
        static int logged;
        uint32_t vec = type_and_format & 0x0fff;
        if (logged < 40 && !(vec >= 0x60 && vec <= 0x7c) && !(vec >= 0x80 && vec <= 0xbc) && vec != 0x28) {
            logged++;
            kprintf("[PSMAC:EXC] vector %03x format %x at PC %08x SR %04x ea %08x fault %08x\n", vec, type_and_format >> 12, M68k_PC, M68k_SR, ea, fault);
            {   /* the instruction words around the faulting PC, read through the 68k's own translation
                   (the shim's tables are live on this core), if both ends of the span are mapped */
                extern uint32_t psmac_mmu_l2p(uint32_t logical, int *valid);
                uint32_t pc = M68k_PC & ~1u; int v0, v1;
                psmac_mmu_l2p(pc - 16, &v0); psmac_mmu_l2p(pc + 30, &v1);
                if (v0 && v1) {
                    kprintf("[PSMAC:EXC]   words at PC-16:");
                    for (int i = -8; i < 16; i++) kprintf("%s%04x", i == 0 ? " |" : " ", *(volatile uint16_t *)(uintptr_t)(pc + 2 * i));
                    kprintf("\n");
                }
            }
            { extern volatile uint32_t psmac_exc_flag; if (!psmac_exc_flag) psmac_exc_flag = vec | 0x10000; }   /* service core dumps the PC trail */
        }
    }
#endif
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

    __asm__ volatile("": :"r"(M68k_PC));

    return M68k_SR;
}
