/*
 * psmac_mmu.c — 68030 MMU shim for the Macintosh IIci ROM on Emu68. MPL-2.0.
 * Stage 1: PMOVE to/from TC, SRP, CRP, TT0, TT1 and MMUSR are recorded and read back; translation
 * itself is still identity. Stage 2 (next): walk the ROM's tables on TC/CRP load and reprogram the
 * ARM mapping (24-bit mode, I/O alias 0x5000xxxx, RAM beyond the frame buffer).
 */
#include <stdint.h>
#include "support.h"
#include "psmac.h"

static struct { uint32_t tc, tt0, tt1, srp_hi, srp_lo, crp_hi, crp_lo; uint16_t mmusr; } mmu;

static inline uint32_t rd32(uint32_t ea) { return *(volatile uint32_t *)(uintptr_t)ea; }
static inline void wr32(uint32_t ea, uint32_t v) { *(volatile uint32_t *)(uintptr_t)ea = v; }

void psmac_pmove(uint32_t ea, uint32_t ext)
{
    int fmt = (ext >> 13) & 7, preg = (ext >> 10) & 7, rw = (ext >> 9) & 1;
    static int n; if (n < 16) { n++; kprintf("[PSMAC:MMU] PMOVE ext %04x ea %08x\n", ext, ea); }
    switch (fmt) {
    case 2: /* TC, SRP, CRP */
        if (preg == 0) {
            if (rw) wr32(ea, mmu.tc);
            else {
                uint32_t old = mmu.tc; mmu.tc = rd32(ea);
                if ((old ^ mmu.tc) & 0x80000000)
                    kprintf("[PSMAC:MMU] TC=%08x (%s) CRP=%08x:%08x\n", mmu.tc, (mmu.tc & 0x80000000) ? "enabled" : "disabled", mmu.crp_hi, mmu.crp_lo);
            }
        } else if (preg == 2) {
            if (rw) { wr32(ea, mmu.srp_hi); wr32(ea + 4, mmu.srp_lo); } else { mmu.srp_hi = rd32(ea); mmu.srp_lo = rd32(ea + 4); }
        } else if (preg == 3) {
            if (rw) { wr32(ea, mmu.crp_hi); wr32(ea + 4, mmu.crp_lo); }
            else { mmu.crp_hi = rd32(ea); mmu.crp_lo = rd32(ea + 4); kprintf("[PSMAC:MMU] CRP=%08x:%08x\n", mmu.crp_hi, mmu.crp_lo); }
        } else kprintf("[PSMAC:MMU] PMOVE fmt2 preg %d unsupported\n", preg);
        break;
    case 0: /* TT0 / TT1 */
        if (preg == 2) { if (rw) wr32(ea, mmu.tt0); else mmu.tt0 = rd32(ea); }
        else if (preg == 3) { if (rw) wr32(ea, mmu.tt1); else mmu.tt1 = rd32(ea); }
        else kprintf("[PSMAC:MMU] PMOVE fmt0 preg %d unsupported\n", preg);
        break;
    case 3: /* MMUSR */
        if (rw) *(volatile uint16_t *)(uintptr_t)ea = mmu.mmusr; else mmu.mmusr = *(volatile uint16_t *)(uintptr_t)ea;
        break;
    default:
        kprintf("[PSMAC:MMU] PMOVE ext %04x unsupported\n", ext);
    }
}
