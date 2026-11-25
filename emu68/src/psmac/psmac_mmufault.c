// SPDX-License-Identifier: MPL-2.0
/*
 * psmac_mmufault.c — precise 68k bus errors from translated code (Aurora68 fork of Emu68, mac.mmu=full).
 *
 * 25 November 2026, the first step of the real-MMU work (docs/23-mmu.md). The MMU shim (psmac_mmu.c) walks
 * the guest's 68030 tables into the ARM page tables, so a translated load or store to a page the guest marks
 * invalid takes an ARM data abort. Until now the abort handler answered it like a hole in the bus ($BAD0BAD0
 * for a read, a write ignored). System 7's Virtual Memory and A/UX need what a 68030 does instead: a bus
 * error whose frame lets the handler page the memory in and return, after which the access happens.
 *
 * How the faulting instruction is found and restarted. Emu68 translates a run of 68k instructions into one
 * AArch64 block, keyed by the 68k address it starts at. The 68k registers live in fixed ARM registers (D0-D7
 * in w19-w26, A0-A7 in w13-w17/w27-w29, the PC in w18 plus an offset the translator carries), the condition
 * codes in a temporary register for most of a block. None of that says, at an ARM instruction in the middle
 * of a block, which 68k instruction it belongs to or what the SR is. So with mac.mmu=full the translator
 * records, for every 68k instruction it emits, an entry in a table kept at the end of the block
 * (M68k_Translator.c): the ARM offset where its code starts, its 68k address, the temporary holding modified
 * condition codes at that point (if any), and which address registers it may change before its last memory
 * access. Those it saves in v23/v24 (lanes free in this build: the PowerPC JIT that uses them does not run)
 * with one instruction each before the instruction's own code; nearly every other instruction costs nothing.
 *
 * On a fault (psmac_mmu_fault, called from the abort dispatcher in vectors.c) the entry whose code holds the
 * faulting ARM instruction gives the 68k instruction; its address registers are put back, the SR is rebuilt,
 * a 68030 short bus-cycle fault frame (format $A, vector 2) is pushed on the supervisor stack, the PC becomes
 * the bus-error vector, and the abort returns straight into the JIT loop, which is where the block's own
 * return would have gone (x30 is checked to be the loop's call). The guest's handler runs as ordinary 68k
 * code; its RTE (EMIT_RTE accepts formats $7, $A and $B under mac.mmu=full) pops the frame and the
 * instruction runs again from its start: a restart, the 68040's model, delivered in the 68030's frame. A
 * 68030 would continue the instruction from the frame's internal state instead; no handler looks inside
 * that state, so the difference shows only to a handler that completes the access itself and clears DF
 * (docs/23-mmu.md, "Precise faults").
 *
 * This file is compiled with x19-x29 fixed, like vectors.c, so the abort path can read and write the 68k
 * registers held there directly.
 */
#include <stdint.h>
#include <stddef.h>
#include "support.h"
#include "M68k.h"
#include "A64.h"
#include "RegisterAllocator.h"
#include "cache.h"
#include "psmac.h"

extern int psmac_mmu_full;
int psmac_mmu_translating(void);
uint8_t *psmac_mmu_ram_ptr(uint32_t logical);
extern struct M68KState *__m68k_state;

/* One per translated 68k instruction (or fused group). arm = 0xffff marks the header, whose pc is the count. */
struct psmac_fault_entry { uint16_t arm; uint8_t cc; uint8_t snap; uint32_t pc; };
#define FT_MAGIC 0xffffu
#define FT_MAX   (2 * (JCCB_INSN_DEPTH_MASK + 1) + 4)      /* the translator's own local_state bound */
static struct psmac_fault_entry ft[FT_MAX + 2];          /* header, entries, and room for the sentinel */
static uint32_t ft_n, ft_limit = FT_MAX;

/* ---------------- translation side ---------------- */

/* The address registers an instruction may change before (or between) its memory accesses, so that a fault
   on a later access would leave them half-updated. Conservative: a register that is listed but not changed
   costs one ARM instruction and is put back unchanged. A single (An)+ or -(An) access is translated as one
   ARM load or store with writeback, which an abort leaves undone, so the list matters for the second access
   of MOVE (An)+,(Am)+, for CMPM, ADDX/SUBX/ABCD/SBCD/PACK/UNPK -(An),-(Am), MOVEM -(An) (its first store
   writes the base back), MOVE16, and the instructions that push or pop the stack. */
static uint8_t snap_mask(const uint16_t *pc)
{
    uint16_t op = cache_read_16(ICACHE, (uintptr_t)pc);
    unsigned line = op >> 12, mode = (op >> 3) & 7, reg = op & 7;
    uint8_t m = 0;

    if (line == 6)
        return (op & 0xff00) == 0x6100 ? 0x80 : 0;          /* BSR pushes; Bcc's low bits are a displacement */
    if (line == 7 || line == 0xA)
        return 0;                                            /* MOVEQ; A-line traps go through the exception path */
    if (mode == 3 || mode == 4)
        m |= 1u << reg;                                      /* the effective address in the low six bits */
    if (line >= 1 && line <= 3) {                            /* MOVE: the destination's mode is in bits 8-6 */
        unsigned dm = (op >> 6) & 7;
        if (dm == 3 || dm == 4)
            m |= 1u << ((op >> 9) & 7);
    }
    if ((line == 8 || line == 9 || line == 0xB || line == 0xC || line == 0xD) && (op & 0x0138) == 0x0108)
        m |= (1u << reg) | (1u << ((op >> 9) & 7));         /* -(Ay),-(Ax) / (Ay)+,(Ax)+ register-memory forms */
    if (line == 4 && ((op & 0xffc0) == 0x4e80 || (op & 0xffc0) == 0x4840 || (op & 0xfff0) == 0x4e50 ||
                      (op & 0xfff8) == 0x4808 || (op >= 0x4e73 && op <= 0x4e77)))
        m |= 0x80;                                           /* JSR, PEA, LINK, UNLK, LINK.L, RTE, RTD, RTS, RTR */
    if (line == 0xF && (op & 0xfff8) == 0xf620)              /* MOVE16 (Ax)+,(Ay)+: Ay in the extension word */
        m |= 1u << ((cache_read_16(ICACHE, (uintptr_t)(pc + 1)) >> 12) & 7);
    return m;
}

void psmac_mmu_jit_begin(void) { ft_n = 0; }

/* At the start of each instruction's code (M68k_Translator.c, before EmitINSN): note where it is and save the
   address registers it may change. Only called with mac.mmu=full. */
void psmac_mmu_jit_insn(struct TranslatorContext *ctx, uint16_t *pc)
{
    if (ft_n > ft_limit)
        return;
    struct psmac_fault_entry *e = &ft[1 + ft_n++];
    if (ft_n > ft_limit) {         /* full: a sentinel (pc 0) says the rest of the block is not covered, and a fault
                                      there falls back (psmac_mmu_fault) instead of landing on the wrong instruction */
        e->arm = (uint16_t)(ctx->tc_CodePtr - ctx->tc_CodeStart); e->pc = 0; e->cc = 0xff; e->snap = 0;
        return;
    }
    e->arm = (uint16_t)(ctx->tc_CodePtr - ctx->tc_CodeStart);
    e->pc = (uint32_t)(uintptr_t)pc;
    e->cc = (RA_IsCCLoaded() && RA_IsCCModified()) ? RA_GetCC(ctx) : 0xff;
    e->snap = psmac_mmu_full == 2 ? 0 : snap_mask(pc);      /* 2: mac.mmu=nosnap, the diagnostic without copies */
    for (int n = 0; n < 8; n++) {
        if (e->snap & (1u << n))
            EMIT(ctx, mov_reg_to_simd(n < 4 ? 23 : 24, TS_S, n & 3, RA_MapM68kRegister(ctx, 8 + n)));
    }
}

/* Words the translator keeps free at the end of a unit for the table of a block of at most 'depth' instructions
   (each loop of M68K_Translate emits one instruction or fused group, and there are at most depth of them) */
uint32_t psmac_mmu_jit_reserve_words(uint32_t depth)
{
    ft_limit = depth + 2 < FT_MAX ? depth + 2 : FT_MAX;
    return (ft_limit + 2) * sizeof(struct psmac_fault_entry) / 4;
}

/* Copy the table behind the block's code; returns its size in bytes */
uint32_t psmac_mmu_jit_finish(void *dst)
{
    struct psmac_fault_entry *d = dst;
    ft[0].arm = FT_MAGIC; ft[0].cc = 0; ft[0].snap = 0; ft[0].pc = ft_n;
    for (uint32_t i = 0; i <= ft_n; i++)
        d[i] = ft[i];
    return (ft_n + 1) * sizeof(struct psmac_fault_entry);
}

/* ---------------- abort side (EL1) ---------------- */

static uint32_t delivered, not_delivered;
static uint64_t el1_ticks;

static inline uint64_t cnt_now(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v)); return v; }
static inline uint64_t cnt_frq(void) { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v; }

static int put16(uint32_t a, uint16_t v)
{
    uint8_t *p = psmac_mmu_ram_ptr(a);
    if (!p) return 0;
    *(volatile uint16_t *)p = v;
    return 1;
}
static int put32(uint32_t a, uint32_t v) { return put16(a, v >> 16) && put16(a + 2, v & 0xffff); }
static uint16_t get16(uint32_t a, int *ok)
{
    uint8_t *p = psmac_mmu_ram_ptr(a);
    if (!p) { *ok = 0; return 0; }
    return *(volatile uint16_t *)p;
}

static uint32_t snap_lane(int n)
{
    uint32_t v = 0;
    switch (n) {
    case 0: __asm__ volatile("umov %w0, v23.s[0]" : "=r"(v)); break;
    case 1: __asm__ volatile("umov %w0, v23.s[1]" : "=r"(v)); break;
    case 2: __asm__ volatile("umov %w0, v23.s[2]" : "=r"(v)); break;
    case 3: __asm__ volatile("umov %w0, v23.s[3]" : "=r"(v)); break;
    case 4: __asm__ volatile("umov %w0, v24.s[0]" : "=r"(v)); break;
    case 5: __asm__ volatile("umov %w0, v24.s[1]" : "=r"(v)); break;
    case 6: __asm__ volatile("umov %w0, v24.s[2]" : "=r"(v)); break;
    default: __asm__ volatile("umov %w0, v24.s[3]" : "=r"(v)); break;
    }
    return v;
}

static int miss(uint32_t la, uint64_t elr, const char *why)
{
    not_delivered++;
    if (not_delivered <= 8 || (not_delivered & 1023) == 0)
        kprintf("[PSMAC:MMU] fault at %08x (ARM %p) not delivered as a bus error: %s (%u so far)\n", la, (void *)elr, why, not_delivered);
    return 0;
}

static inline uint16_t swap_vc(uint16_t sr) { return ((sr & 3) == 1 || (sr & 3) == 2) ? (sr ^ 3) : sr; }

/* The 68030's short bus-cycle fault frame, format $A (MC68030UM 8.2.3): SR, PC, format/vector, an internal
   word, the special status word, instruction pipe stages C and B, the fault address, two internal words, the
   data output buffer and two internal words: 32 bytes */
#define FRAME_A_SIZE 32
#define SSW_DF  0x0100      /* data fault: the cycle is rerun on RTE */
#define SSW_RW  0x0040      /* 1 = read */

/* Returns 1 when the abort became a 68k bus error (ELR and the registers changed), 0 to let the caller treat
   the access as before. Only with mac.mmu=full, from SYSHandler's data-abort path. */
int psmac_mmu_fault(uint64_t *ctx, uint64_t elr, uint64_t esr, uint64_t far)
{
    uint64_t t0 = cnt_now();
    if ((far >> 32) == 1 || (far >> 32) == 0xffffffffull) far &= 0xffffffffull;
    if (far >> 32) return 0;
    uint32_t la = (uint32_t)far;
    if (!psmac_mmu_translating()) return 0;
    {   int valid; psmac_mmu_l2p(la, &valid);
        if (valid) return 0; }                     /* mapped for the guest: I/O, the bridge's business */

    /* the block: the JIT loops call it through x12 (ARMCode), the executable alias of the unit's code */
    uint64_t entry = ctx[12];
    if (!(entry & 0x0000001000000000ull) || elr < entry) return miss(la, elr, "not in the running block");
    struct M68KTranslationUnit *u = (void *)((entry & ~0x0000001000000000ull) - offsetof(struct M68KTranslationUnit, mt_ARMCode));
    if (elr >= entry + 4ull * u->mt_ARMInsnCnt) return miss(la, elr, "not in the running block");
    const struct psmac_fault_entry *t = (const void *)u->mt_LocalState;
    if (!t || t[0].arm != FT_MAGIC || t[0].pc == 0) return miss(la, elr, "block has no fault table");
    /* the block's own return goes to the JIT loop: x30 must be the address after its blr x12 */
    uint64_t lr = ctx[19];
    {   extern void JITLoop(void), JITLoopWithCRC32(void), MainLoop(void);
        uintptr_t f1 = (uintptr_t)JITLoop, f2 = (uintptr_t)JITLoopWithCRC32, f3 = (uintptr_t)MainLoop;
        uintptr_t lo_f = f1 < f2 ? f1 : f2, hi_f = f1 > f2 ? f1 : f2;
        if (f3 < lo_f) lo_f = f3;
        if (f3 > hi_f) hi_f = f3;
        if (lr < lo_f + 4 || lr >= hi_f + 0x400 || (lr & 3)) return miss(la, elr, "x30 is not the JIT loop (inside a helper call)");
        if (LE32(*(volatile uint32_t *)(lr - 4)) != 0xd63f0180u) return miss(la, elr, "x30 is not after the JIT loop's blr x12");
    }

    /* the instruction: the last entry starting at or before the faulting ARM instruction */
    uint32_t off = (uint32_t)((elr - entry) / 4), n = t[0].pc, lo = 1, hi = n;
    if (t[1].arm > off) return miss(la, elr, "before the first instruction");
    while (lo < hi) {
        uint32_t mid = (lo + hi + 1) / 2;
        if (t[mid].arm <= off) lo = mid; else hi = mid - 1;
    }
    const struct psmac_fault_entry *e = &t[lo];
    if (!e->pc) return miss(la, elr, "past the end of the block's fault table");

    /* the 68k state at the instruction's start */
    uint32_t sr_int;
    __asm__ volatile("umov %w0, v19.h[5]" : "=r"(sr_int));
    if (e->cc != 0xff)
        sr_int = (uint32_t)ctx[e->cc] & 0xffff;            /* condition codes still in the block's temporary */
    uint32_t a[8];
    for (int i = 0; i < 5; i++) a[i] = (uint32_t)ctx[13 + i];
    __asm__ volatile("mov %w0, w27" : "=r"(a[5]));
    __asm__ volatile("mov %w0, w28" : "=r"(a[6]));
    __asm__ volatile("mov %w0, w29" : "=r"(a[7]));
    for (int i = 0; i < 8; i++) {
        if (e->snap & (1u << i)) a[i] = snap_lane(i);
    }

    /* the supervisor stack */
    uint32_t ssp, usp = 0;
    int user = !(sr_int & SR_S);
    if (user) {
        usp = a[7];
        if (sr_int & SR_M) __asm__ volatile("umov %w0, v21.s[3]" : "=r"(ssp));
        else __asm__ volatile("umov %w0, v21.s[2]" : "=r"(ssp));
    } else
        ssp = a[7];

    /* the frame */
    int write = (esr >> 6) & 1;
    uint32_t insn = LE32(*(volatile uint32_t *)elr), size = 4;
    if ((insn & 0x3a000000u) == 0x38000000u) size = 1u << (insn >> 30);      /* LDR/STR, any addressing */
    uint16_t ssw = SSW_DF | (write ? 0 : SSW_RW) | (size == 1 ? 0x10 : size == 2 ? 0x20 : 0) | (user ? 1 : 5);
    int ok = 1;
    uint16_t stage_c = get16(e->pc, &ok), stage_b = ok ? get16(e->pc + 2, &ok) : 0;
    uint32_t f = ssp - FRAME_A_SIZE;
    int w = put16(f + 0, swap_vc((uint16_t)sr_int)) && put32(f + 2, e->pc) && put16(f + 6, 0xA008) && put16(f + 8, 0) &&
            put16(f + 10, ssw) && put16(f + 12, stage_c) && put16(f + 14, stage_b) && put32(f + 16, la) &&
            put32(f + 20, 0) && put32(f + 24, 0) && put32(f + 28, 0);
    if (!w) return miss(la, elr, "the supervisor stack is not mapped (double bus fault)");
    uint32_t vbr = __m68k_state->VBR, vec = 0;
    {   int vok = 1;
        uint32_t hiw = get16(vbr + 8, &vok), low = vok ? get16(vbr + 10, &vok) : 0;
        if (!vok) return miss(la, elr, "the vector table is not mapped");
        vec = (hiw << 16) | low; }

    /* commit: registers, SR, stacks, PC; the abort returns into the JIT loop */
    for (int i = 0; i < 5; i++) ctx[13 + i] = a[i];
    __asm__ volatile("mov w27, %w0" :: "r"(a[5]));
    __asm__ volatile("mov w28, %w0" :: "r"(a[6]));
    __asm__ volatile("mov w29, %w0" :: "r"(f));
    if (user) __asm__ volatile("mov v21.s[1], %w0" :: "r"(usp));
    uint32_t nsr = (sr_int | SR_S) & ~(SR_T0 | SR_T1);
    __asm__ volatile("mov v19.h[5], %w0" :: "r"(nsr));
    ctx[18] = vec;
    __asm__ volatile("msr ELR_EL1, %0" :: "r"(lr));

    delivered++;
    el1_ticks += cnt_now() - t0;
    if (delivered <= 4 || (delivered & 15) == 0)
        kprintf("[PSMAC:MMU] bus error #%u: PC %08x %s %08x (SSW %04x, %s) -> handler %08x; %u ns on average in the abort\n",
                delivered, e->pc, write ? "write" : "read", la, ssw, user ? "user" : "supervisor", vec,
                (uint32_t)(el1_ticks * 1000000000ull / cnt_frq() / delivered));
    return 1;
}
