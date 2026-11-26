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
#include "globals.h"

extern int psmac_mmu_full;
int psmac_mmu_translating(void);
uint8_t *psmac_mmu_ram_ptr(uint32_t logical);
uint8_t *psmac_mmu_ram_ptr_fc(uint32_t logical, int fc);
extern struct M68KState *__m68k_state;

/* One per translated 68k instruction (or fused group). arm = 0xffff marks the header, whose pc is the count. */
struct psmac_fault_entry { uint16_t arm; uint8_t cc; uint8_t snap; uint32_t pc; uint8_t fpsr, fpcr; uint16_t pad; };
uint8_t RA_PsmacFPSRTemp(void);
uint8_t RA_PsmacFPCRTemp(void);
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

int psmac_mmu_stub;                              /* the block being translated is an instruction fetch's bus error */
void psmac_mmu_jit_begin(void) { ft_n = 0; psmac_mmu_stub = 0; }

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
        e->fpsr = e->fpcr = 0xff; e->pad = 0;
        return;
    }
    e->arm = (uint16_t)(ctx->tc_CodePtr - ctx->tc_CodeStart);
    e->pc = (uint32_t)(uintptr_t)pc;
    e->cc = (RA_IsCCLoaded() && RA_IsCCModified()) ? RA_GetCC(ctx) : 0xff;
    e->fpsr = RA_PsmacFPSRTemp(); e->fpcr = RA_PsmacFPCRTemp(); e->pad = 0;
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
    ft[0].arm = FT_MAGIC; ft[0].cc = 0; ft[0].snap = 0; ft[0].pc = ft_n; ft[0].fpsr = ft[0].fpcr = 0xff; ft[0].pad = 0;
    for (uint32_t i = 0; i <= ft_n; i++)
        d[i] = ft[i];
    return (ft_n + 1) * sizeof(struct psmac_fault_entry);
}


/* mac.mmu=full, phase 3 (docs/23-mmu.md section 12): where translated code has just changed the SR (RTE, MOVE, ANDI and
   EORI to SR, STOP; each of them ends its block), the view for the new S bit goes into TTBR0_EL1 when the configuration
   has a user and a supervisor space apart (psmac_views.split, psmac.h). Eleven instructions, six of them when the views
   are one; with the option only. sr: the register holding the new SR. */
void psmac_mmu_emit_view(struct TranslatorContext *ctx, uint8_t sr)
{
    union { uint64_t u64; uint16_t u16[4]; } u;
    u.u64 = (uintptr_t)&psmac_views;
    uint8_t t1 = RA_AllocARMRegister(ctx), t2 = RA_AllocARMRegister(ctx);
    EMIT(ctx,
        mov64_immed_u16(t1, u.u16[3], 0),
        movk64_immed_u16(t1, u.u16[2], 1),
        movk64_immed_u16(t1, u.u16[1], 2),
        movk64_immed_u16(t1, u.u16[0], 3),
        ldr_offset(t1, t2, offsetof(struct psmac_mmu_views, split)),
        cbz(t2, 6),
        ubfx(t2, sr, SRB_S, 1),
        ldr64_regoffset(t1, t2, t2, UXTW, 1),
        I32(0xd5182000 | t2),                   /* msr TTBR0_EL1, x(t2) */
        I32(0xd5033fdf),                        /* isb */
        str64_offset(t1, t2, offsetof(struct psmac_mmu_views, cur))
    );
    RA_FreeARMRegister(ctx, t2);
    RA_FreeARMRegister(ctx, t1);
}

/* the 68k's SR, stack pointers, FPSR and FPCR in their NEON homes (M68k.h); RegisterMapping.h's accessors come
   with global register variables this file cannot have */
static inline uint32_t getSR(void) { uint32_t v; __asm__ volatile("umov %w0, " REG_SR_ASM : "=r"(v)); return v; }
static inline void setSR(uint32_t v) { __asm__ volatile("mov " REG_SR_ASM ", %w0" : : "r"(v)); }
static inline uint32_t getISP(void) { uint32_t v; __asm__ volatile("umov %w0, " REG_ISP_ASM : "=r"(v)); return v; }
static inline uint32_t getMSP(void) { uint32_t v; __asm__ volatile("umov %w0, " REG_MSP_ASM : "=r"(v)); return v; }
static inline void setUSP(uint32_t v) { __asm__ volatile("mov " REG_USP_ASM ", %w0" : : "r"(v)); }
static inline void setFPSR(uint32_t v) { __asm__ volatile("mov " REG_FPSR_ASM ", %w0" : : "r"(v)); }
static inline void setFPCR(uint32_t v) { __asm__ volatile("mov " REG_FPCR_ASM ", %w0" : : "r"(v)); }

/* ---------------- the instruction fetch, at translation (M68k_Translator.c) ---------------- */

int psmac_mmu_probe(uint32_t a, uint32_t len, int fc, int write, uint32_t *bad);
uint32_t psmac_mmu_fetch_fault(uint32_t pc, uint32_t addr, uint64_t *regs, uint32_t sr);

/* The block's code calls psmac_mmu_fetch_fault with every AArch64 register saved on the stack (the save area
   is the register file the C side reads and writes, as for the exact FPU and the PMMU instructions) and the
   SR in w3; the new SR comes back into the condition codes' register, the PC is the handler's, and the block
   ends there. */
static void emit_fetch_fault(struct TranslatorContext *ctx, uint32_t pc, uint32_t addr)
{
    union { uint64_t u64; uint16_t u16[4]; } u;
    u.u64 = (uintptr_t)psmac_mmu_fetch_fault;
    EMIT_FlushPC(ctx);
    uint8_t sr = RA_ModifyCC(ctx);
    EMIT(ctx, stp64_preindex(31, 0, 1, -256));
    for (int i = 2; i < 30; i += 2)
        EMIT(ctx, stp64(31, i, i + 1, i * 8));
    EMIT(ctx, str64_offset(31, 30, 240), mov_reg(3, sr));
    EMIT_LoadImmediate(ctx, 0, pc);
    EMIT_LoadImmediate(ctx, 1, addr);
    EMIT(ctx,
        add64_immed(2, 31, 0),
        mov64_immed_u16(4, u.u16[3], 0),
        movk64_immed_u16(4, u.u16[2], 1),
        movk64_immed_u16(4, u.u16[1], 2),
        movk64_immed_u16(4, u.u16[0], 3),
        blr(4),
        str64_offset(31, 0, sr * 8)
    );
    for (int i = 2; i < 30; i += 2)
        EMIT(ctx, ldp64(31, i, i + 1, i * 8));
    EMIT(ctx,
        ldr64_offset(31, 30, 240),
        ldp64_postindex(31, 0, 1, 256)
    );                                          /* the translator ends the block here (psmac_mmu_jit_fetch returns 2) */
}

/* Before each instruction with mac.mmu=full: 0 to translate it, 1 to end the block before it (it starts on
   another 4 KiB page than the block, or runs into a page the guest's tables mark invalid), 2 when its fetch
   faults and the fault's code was emitted in its place (the block ends after it). Blocks stay inside one
   page, so a page's blocks are its own and never run from a valid page into an invalid one. */
int psmac_mmu_jit_fetch(struct TranslatorContext *ctx, uint16_t *in_code, uint16_t *block_start, int insn_count)
{
    if (!psmac_mmu_translating())
        return 0;
    uint32_t pc = (uint32_t)(uintptr_t)in_code, page = pc & ~0xfffu, bad;
    int fc = (getSR() & SR_S) ? 6 : 2;          /* supervisor or user program: the mode the block will run in */
    if (insn_count && page != ((uint32_t)(uintptr_t)block_start & ~0xfffu))
        return 1;
    if (!insn_count) {
        void psmac_mmu_check_code_space(uint32_t pc, int super);
        psmac_mmu_check_code_space(pc, fc == 6);
    }
    if (!psmac_mmu_probe(pc, 2, fc, 0, &bad)) {
        if (insn_count) return 1;
        emit_fetch_fault(ctx, pc, pc);
        psmac_mmu_stub = 1;
        return 2;
    }
    if ((pc & 0xfffu) > 0x1000u - 22u && !psmac_mmu_probe(page + 0x1000u, 2, fc, 0, &bad)) {
        uint16_t buf[12];
        int n = (int)((0x1000u - (pc & 0xfffu)) / 2);
        for (int i = 0; i < 12; i++)
            buf[i] = i < n ? cache_read_16(ICACHE, (uintptr_t)(in_code + i)) : 0;
        if (M68K_GetINSNLength(buf) > n) {
            if (insn_count) return 1;
            emit_fetch_fault(ctx, pc, page + 0x1000u);
            psmac_mmu_stub = 1;
            return 2;
        }
    }
    return 0;
}

/* ---------------- the bus-error frame ---------------- */

static uint32_t delivered, not_delivered, lazy_maps, helper_faults, fetch_faults;
static uint64_t el1_ticks;

static inline uint64_t cnt_now(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v)); return v; }
static inline uint64_t cnt_frq(void) { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v; }

/* the frame and the vector: supervisor data (FC 5), whichever view is installed when the fault is taken */
static int put16(uint32_t a, uint16_t v)
{
    uint8_t *p = psmac_mmu_ram_ptr_fc(a, 5);
    if (!p) return 0;
    *(volatile uint16_t *)p = v;
    return 1;
}
static int put32(uint32_t a, uint32_t v) { return put16(a, v >> 16) && put16(a + 2, v & 0xffff); }
static uint16_t get16_fc(uint32_t a, int fc, int *ok)
{
    uint8_t *p = psmac_mmu_ram_ptr_fc(a, fc);
    if (!p) { *ok = 0; return 0; }
    return *(volatile uint16_t *)p;
}
static uint16_t get16(uint32_t a, int *ok) { return get16_fc(a, 5, ok); }

static inline uint16_t swap_vc(uint16_t sr) { return ((sr & 3) == 1 || (sr & 3) == 2) ? (sr ^ 3) : sr; }

/* The 68030's bus-cycle fault frames (MC68030UM 8.2.3 and 8.4.4). Short, format $A, 32 bytes: SR, PC,
   format/vector, an internal word, the special status word, instruction pipe stages C and B, the data cycle
   fault address, the data output buffer. Long, format $B, 92 bytes: the same 32, then the stage B address
   (+36), the data input buffer (+44) and internal state. The 030 uses the short frame when the fault comes
   at an instruction boundary, typically the write that ends an instruction, and the long one in the middle
   of an instruction, which is where a read leaves it: here a write gets $A, a read and an instruction fetch
   get $B. An instruction fetch fault sets FB (stage B) and RB with the faulting word's address at +36, which
   is where VM's frame decoder looks for it (ptch 42, the 030 path); DF and the data address are a data fault. */
#define SSW_FC  0x8000      /* fault on stage C */
#define SSW_FB  0x4000      /* fault on stage B */
#define SSW_RB  0x1000      /* rerun stage B */
#define SSW_DF  0x0100      /* data fault: the cycle is rerun on RTE */
#define SSW_RW  0x0040      /* 1 = read */
struct bframe { uint32_t sr, pc, addr; int write, size, fc, ifetch; };

/* Push the frame below ssp; *fa is its address, *vec the bus-error handler. 0 when the stack or the vector table
   cannot be written or read (a real 030 would halt with a double bus fault) */
static int push_frame(uint32_t ssp, const struct bframe *b, uint32_t *fa, uint32_t *vec, uint16_t *ssw_out)
{
    int fmt_b = b->ifetch || !b->write;
    uint32_t size = fmt_b ? 92 : 32, f = ssp - size;
    uint16_t ssw;
    if (b->ifetch) ssw = SSW_FB | SSW_RB | (uint16_t)(b->fc & 7);
    else ssw = SSW_DF | (b->write ? 0 : SSW_RW) | (b->size == 1 ? 0x10 : b->size == 2 ? 0x20 : 0) | (uint16_t)(b->fc & 7);
    int ok = 1;
    int pfc = (b->sr & SR_S) ? 6 : 2;              /* the instruction's words, in its own mode's space */
    uint16_t stage_c = get16_fc(b->pc + 2, pfc, &ok), stage_b = ok ? get16_fc(b->pc + 4, pfc, &ok) : 0;
    if (!ok) stage_c = stage_b = 0;
    int w = put16(f + 0, swap_vc((uint16_t)b->sr)) && put32(f + 2, b->pc) && put16(f + 6, (fmt_b ? 0xB000 : 0xA000) | 0x008) &&
            put16(f + 8, 0) && put16(f + 10, ssw) && put16(f + 12, stage_c) && put16(f + 14, stage_b) &&
            put32(f + 16, b->ifetch ? 0 : b->addr) && put32(f + 20, 0) && put32(f + 24, 0) && put32(f + 28, 0);
    if (w && fmt_b) {
        w = put32(f + 32, 0) && put32(f + 36, b->ifetch ? b->addr : b->pc + 4) && put32(f + 40, 0) && put32(f + 44, 0);
        for (uint32_t o = 48; w && o < 92; o += 4)
            w = put32(f + o, 0);
    }
    if (!w) return 0;
    uint32_t vbr = __m68k_state->VBR;
    int vok = 1;
    uint32_t hiw = get16(vbr + 8, &vok), low = vok ? get16(vbr + 10, &vok) : 0;
    if (!vok) return 0;
    *vec = (hiw << 16) | low; *fa = f; *ssw_out = ssw;
    return 1;
}

static void psmac_mmu_halt(void) __attribute__((noreturn));
static int miss(uint32_t la, uint64_t elr, const char *why)
{
    not_delivered++;
    if (not_delivered <= 8 || (not_delivered & 1023) == 0)
        kprintf("[PSMAC:MMU] fault at %08x (ARM %p) not delivered as a bus error: %s (%u so far)\n", la, (void *)elr, why, not_delivered);
    return 0;
}

static void log_delivery(const char *how, uint32_t pc, int write, uint32_t la, uint16_t ssw, int user, uint32_t vec)
{
    uint32_t n = delivered + helper_faults + fetch_faults;
    if (n <= 6 || (n & 1023) == 0)
        kprintf("[PSMAC:MMU] bus error #%u (%s): PC %08x %s %08x (SSW %04x, %s) -> handler %08x; %u lazy maps, %u ns on average in the abort\n",
                n, how, pc, write ? "write" : "read", la, ssw, user ? "user" : "supervisor", vec, lazy_maps,
                delivered ? (uint32_t)(el1_ticks * 1000000000ull / cnt_frq() / delivered) : 0);
}

/* ---------------- from the C helpers: a save area instead of live registers ---------------- */

static inline uint32_t *sa_reg(uint64_t *regs, int n)
{
    static const uint8_t map[16] = { 19, 20, 21, 22, 23, 24, 25, 26, 13, 14, 15, 16, 17, 27, 28, 29 };
    return (uint32_t *)&regs[map[n & 15]] + 1;
}

/* Deliver the bus error of an instruction whose helper found its operand unreachable before touching it
   (psmac_pmmu_exec), or whose fetch faulted at translation (psmac_mmu_fetch_fault). The registers are as
   they were at the instruction's start; the save area gets the new A7 and PC. Returns the new SR | 0x10000,
   or the SR unchanged | 0x10000 with the PC unchanged if even the frame could not be written. */
static uint32_t deliver_from_save_area(uint64_t *regs, const struct bframe *b, const char *how)
{
    int user = !(b->sr & SR_S);
    uint32_t a7 = *sa_reg(regs, 15), ssp = a7;
    if (user) ssp = (b->sr & SR_M) ? getMSP() : getISP();
    uint32_t f, vec; uint16_t ssw;
    if (!push_frame(ssp, b, &f, &vec, &ssw)) {
        /* a 68030 halts (double bus fault); until 26 November 2026 the instruction was tried again for ever */
        not_delivered++;
        kprintf("[PSMAC:MMU] double bus fault: the %s bus error of the instruction at %08x (%08x) could not be stacked at %08x "
                "(SR %04x, VBR %08x): the 68030 halts\n", how, b->pc, b->addr, ssp, b->sr, __m68k_state->VBR);
        void psmac_mmu_debug_walk(uint32_t la, int fc);
        psmac_mmu_debug_walk(ssp - 8, 5);
        psmac_mmu_debug_walk(__m68k_state->VBR + 8, 5);
        psmac_mmu_halt();
    }
    if (user) setUSP(a7);
    *sa_reg(regs, 15) = f;
    regs[18] = vec;
    psmac_view_for(SR_S);                       /* the handler runs in the supervisor's view */
    log_delivery(how, b->pc, b->write, b->addr, ssw, user, vec);
    return ((b->sr | SR_S) & ~(SR_T0 | SR_T1)) | 0x10000;
}

uint32_t psmac_mmu_deliver_helper(uint64_t *regs, uint32_t sr, uint32_t pc, uint32_t addr, int write, int size, int fc)
{
    struct bframe b = { sr, pc, addr, write, size, fc, 0 };
    helper_faults++;
    return deliver_from_save_area(regs, &b, "operand");
}

uint32_t psmac_mmu_fetch_fault(uint32_t pc, uint32_t addr, uint64_t *regs, uint32_t sr)
{
    struct bframe b = { sr, pc, addr, 0, 2, (sr & SR_S) ? 6 : 2, 1 };
    fetch_faults++;
    {   /* This block is the fault and nothing else, made while the page could not be fetched. Once the handler has made
           it valid and returned, the same address must be translated again, not found here: the unit (x12, the running
           block) no longer answers to its address. It is not freed while it runs; the cache's LRU takes it later. */
        uint64_t entry = regs[12];
        if (entry & 0x0000001000000000ull) {
            struct M68KTranslationUnit *u = (void *)((entry & ~0x0000001000000000ull) - offsetof(struct M68KTranslationUnit, mt_ARMCode));
            if (u->mt_M68kAddress == pc) u->mt_M68kAddress = 0xffffffffu;
        }
    }
    return deliver_from_save_area(regs, &b, "fetch") & 0xffff;
}

void psmac_mmu_fault_counts(uint32_t *d, uint32_t *h, uint32_t *f, uint32_t *l, uint32_t *m)
{
    *d = delivered; *h = helper_faults; *f = fetch_faults; *l = lazy_maps; *m = not_delivered;
}

/* The guard of a C helper that touches 68k memory as it works (the exact FPU, psmac_fpu.c): armed around the
   helper, an access to a page the guest's tables refuse returns from the abort to the landing below instead of
   reading $BAD0BAD0, and the landing jumps back into the helper's guard, which restores its state and calls
   psmac_mmu_helper_fault. */
struct psmac_hguard { void *jb[5]; volatile int armed; uint32_t addr; int write; } psmac_hguard;
void psmac_helper_fault_landing(void)
{
    psmac_hguard.armed = 0;
    __builtin_longjmp(psmac_hguard.jb, 1);
}
uint32_t psmac_mmu_helper_fault(uint64_t *regs, uint32_t sr, uint32_t pc)
{
    struct bframe b = { sr, pc, psmac_hguard.addr, psmac_hguard.write, 4, (sr & SR_S) ? 5 : 1, 0 };
    helper_faults++;
    return (1u << 31) | (deliver_from_save_area(regs, &b, "helper") & 0xffff);
}

/* SFC and DFC for the PMMU instructions' function-code operand (psmac_mmu.c) */
int psmac_m68k_fcreg(int dfc) { return (dfc ? __m68k_state->DFC : __m68k_state->SFC) & 7; }

/* ---------------- abort side (EL1) ---------------- */

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

/* Is a an address of translated code? Blocks run from the JIT arena's read-only alias (start.c: the arena's
   virtual base with bit 36 set; its size is a device-tree property below 4 GiB, and the PowerPC arena's alias
   would be 8 GiB further up). */
static inline int in_jit_code(uint64_t a)
{
    uint64_t lo = (uint64_t)(uintptr_t)globals.m68k_jit_virt_base | 0x0000001000000000ull;
    return globals.m68k_jit_virt_base && a >= lo && a - lo < 0x100000000ull;
}

/* The running block and its fault-table entry for the faulting ARM instruction; NULL and *why when the abort
   did not come from a block's own code (a C helper, the translator, the code cache's checksum, the kernel's own C).
   x12 holds the running block's entry only while translated code runs: anywhere else it is whatever the C code left
   in it. So the faulting instruction must itself be translated code before x12 is believed. Until 27 November 2026
   only x12's tag bit was tested; on the real SE the kernel's own probe read of the VIA in psmac_setup_memory (an
   abort on 68k space before the 68k has started) met an x12 of 0x74a1cbb2fd7a278d, tag bit set, and the unit
   "found" there was read at a wild address: core 0 died before the ROM's first instruction (docs/23 section 13). */
static const struct psmac_fault_entry *locate(uint64_t *ctx, uint64_t elr, uint64_t *lr_out, const char **why)
{
    uint64_t entry = ctx[12];
    if (!in_jit_code(elr)) { *why = "not translated code"; return NULL; }
    if (!(entry & 0x0000001000000000ull) || !in_jit_code(entry) || elr < entry) { *why = "not in the running block"; return NULL; }
    struct M68KTranslationUnit *u = (void *)((entry & ~0x0000001000000000ull) - offsetof(struct M68KTranslationUnit, mt_ARMCode));
    if (elr >= entry + 4ull * u->mt_ARMInsnCnt) { *why = "not in the running block"; return NULL; }
    const struct psmac_fault_entry *t = (const void *)u->mt_LocalState;
    if (!t || t[0].arm != FT_MAGIC || t[0].pc == 0) { *why = "block has no fault table"; return NULL; }
    /* the block's own return goes to the JIT loop: x30 must be the address after its blr x12 */
    uint64_t lr = ctx[19];
    {   extern void JITLoop(void), JITLoopWithCRC32(void), MainLoop(void);
        uintptr_t f1 = (uintptr_t)JITLoop, f2 = (uintptr_t)JITLoopWithCRC32, f3 = (uintptr_t)MainLoop;
        uintptr_t lo_f = f1 < f2 ? f1 : f2, hi_f = f1 > f2 ? f1 : f2;
        if (f3 < lo_f) lo_f = f3;
        if (f3 > hi_f) hi_f = f3;
        if (lr < lo_f + 4 || lr >= hi_f + 0x400 || (lr & 3)) { *why = "x30 is not the JIT loop (inside a helper call)"; return NULL; }
        if (LE32(*(volatile uint32_t *)(lr - 4)) != 0xd63f0180u) { *why = "x30 is not after the JIT loop's blr x12"; return NULL; }
    }
    /* the instruction: the last entry starting at or before the faulting ARM instruction */
    uint32_t off = (uint32_t)((elr - entry) / 4), n = t[0].pc, lo = 1, hi = n;
    if (t[1].arm > off) { *why = "before the first instruction"; return NULL; }
    while (lo < hi) {
        uint32_t mid = (lo + hi + 1) / 2;
        if (t[mid].arm <= off) lo = mid; else hi = mid - 1;
    }
    if (!t[lo].pc) { *why = "past the end of the block's fault table"; return NULL; }
    *lr_out = lr;
    return &t[lo];
}

int psmac_mmu_access(uint32_t la, int fc, int write, uint16_t *mmusr);
int psmac_mmu_cur_view(void);

/* A 68030 that takes a bus error while it stacks an exception's frame or reads its vector stops (HALT) until it is
   reset. Here core 0 stops running the 68k; the heartbeat goes on, and its JIT line stays at zero. */
static void psmac_mmu_halt(void)
{
    for (uint32_t n = 0; ; n++) {
        if ((n & 0xffff) == 0)
            kprintf("[PSMAC:MMU] the 68030 is halted (double bus fault); restart the machine\n");
        for (volatile uint32_t d = 0; d < 20000000u; d++) { }
    }
}

/* Returns 1 when the abort was dealt with here: the page was mapped (the ARM instruction runs again), or the
   access became a 68k bus error (ELR and the registers changed). 0 lets the caller emulate the access as
   before. Only with mac.mmu=full, from SYSHandler's data-abort path. */
int psmac_mmu_fault(uint64_t *ctx, uint64_t elr, uint64_t esr, uint64_t far)
{
    uint64_t t0 = cnt_now();
    /* the kernel's alias of the 68k's space below zero (TTBR1, a 68k address computation that wrapped) is empty while
       spaces are in use (mmu_space_leave_alias): such an access is checked here and emulated (psmac_bus.c) */
    int top_alias = (far >> 32) == 0xffffffffull;
    if ((far >> 32) == 1 || top_alias) far &= 0xffffffffull;
    if (far >> 32) return 0;
    uint32_t la = (uint32_t)far;
    if (!psmac_mmu_translating()) return 0;
    int write = (esr >> 6) & 1;

    const char *why = NULL; uint64_t lr = 0;
    const struct psmac_fault_entry *e = locate(ctx, elr, &lr, &why);
    uint32_t sr_int = getSR();
    if (e && e->cc != 0xff)
        sr_int = (uint32_t)ctx[e->cc] & 0xffff;            /* condition codes still in the block's temporary */
    /* The mode of the access: the view TTBR0 holds when the user and supervisor views are apart (the S bit and the view
       agree everywhere but in an exception's entry, which installs the supervisor's view before it pushes); else the SR's,
       and supervisor while an exception's frame is being pushed (the SR still has the old mode then). */
    int view_now = psmac_mmu_cur_view(), user;
    if (psmac_views.guard) user = 0;
    else if (view_now >= 0) user = view_now == 0;
    else user = !(sr_int & SR_S);

    /* the soft TLB first: a valid page is mapped and the access runs again, wherever it came from */
    uint16_t mmusr = 0;
    int r = psmac_mmu_access(la, user ? 1 : 5, write, &mmusr);
    if (r > 0 && !top_alias) { lazy_maps++; return 1; }
    if (r >= 0) return 0;
    if (!e && psmac_views.guard) {
        /* the frame of an exception, or its vector, on a page the tables refuse: a 68030 halts (a double bus fault) */
        psmac_views.guard = 0;
        uint32_t a7; __asm__ volatile("mov %w0, w29" : "=r"(a7));
        kprintf("[PSMAC:MMU] double bus fault: %s %08x refused (MMUSR %04x) while an exception's frame was pushed or its vector "
                "read (SR %04x, A7 %08x, VBR %08x): the 68030 halts\n", write ? "write" : "read", la, mmusr, sr_int,
                a7, __m68k_state->VBR);
        void psmac_mmu_debug_walk(uint32_t la, int fc);
        psmac_mmu_debug_walk(la, 5);
        uint64_t halt = (uint64_t)(uintptr_t)psmac_mmu_halt;
        __asm__ volatile("msr ELR_EL1, %0" :: "r"(halt));
        return 1;
    }
    if (!e) {
        if (psmac_hguard.armed) {                          /* a guarded helper: back into its guard */
            psmac_hguard.armed = 0; psmac_hguard.addr = la; psmac_hguard.write = write;
            uint64_t land = (uint64_t)(uintptr_t)psmac_helper_fault_landing;
            __asm__ volatile("msr ELR_EL1, %0" :: "r"(land));
            return 1;
        }
        return miss(la, elr, why);
    }

    /* the 68k state at the instruction's start */
    uint32_t a[8];
    for (int i = 0; i < 5; i++) a[i] = (uint32_t)ctx[13 + i];
    __asm__ volatile("mov %w0, w27" : "=r"(a[5]));
    __asm__ volatile("mov %w0, w28" : "=r"(a[6]));
    __asm__ volatile("mov %w0, w29" : "=r"(a[7]));
    for (int i = 0; i < 8; i++) {
        if (e->snap & (1u << i)) a[i] = snap_lane(i);
    }
    uint32_t ssp, usp = 0;
    if (user) { usp = a[7]; ssp = (sr_int & SR_M) ? getMSP() : getISP(); }
    else ssp = a[7];

    uint32_t insn = LE32(*(volatile uint32_t *)elr), size = 4;
    if ((insn & 0x3a000000u) == 0x38000000u) size = 1u << (insn >> 30);      /* LDR/STR, any addressing */
    struct bframe b = { sr_int, e->pc, la, write, (int)size, user ? 1 : 5, 0 };
    uint32_t f, vec; uint16_t ssw;
    if (!push_frame(ssp, &b, &f, &vec, &ssw)) {
        /* The bus error's own frame cannot be stacked (or its vector read): a 68030 halts here, a double bus fault, as in
           the guard's case above. Until 26 November 2026 this was a "not delivered" line and the access was emulated,
           $BAD0BAD0 for a read and a write dropped: A/UX's kernel on the MAME SE, its stack run into an unmapped page,
           went on with those values, used one as a pointer, and core 0 died in the abort handler with the console lock
           held, so the log stopped mid-line (docs/23 section 15). Halting keeps the heartbeat and the diagnostics. */
        not_delivered++;
        kprintf("[PSMAC:MMU] double bus fault: %s %08x by the instruction at %08x refused (MMUSR %04x), and its bus error's frame "
                "could not be stacked at %08x (SR %04x, VBR %08x): the 68030 halts\n", write ? "write" : "read", la, e->pc, mmusr,
                ssp, sr_int, __m68k_state->VBR);
        void psmac_mmu_debug_walk(uint32_t la, int fc);
        psmac_mmu_debug_walk(la, user ? 1 : 5);
        if ((ssp & ~0xfffu) != (la & ~0xfffu)) psmac_mmu_debug_walk(ssp - 8, 5);
        uint64_t halt = (uint64_t)(uintptr_t)psmac_mmu_halt;
        __asm__ volatile("msr ELR_EL1, %0" :: "r"(halt));
        return 1;
    }
    {   /* the same address faulting again and again: what the tables say (a diagnostic, docs/23 section 11) */
        static uint32_t last_la, same;
        if (la == last_la) same++; else { last_la = la; same = 0; }
        if (same == 3 || same == 100) {
            void psmac_mmu_debug_walk(uint32_t la, int fc);
            kprintf("[PSMAC:MMU] %u bus errors in a row at %08x (PC %08x, %s, MMUSR %04x)\n", same + 1, la, e->pc, write ? "write" : "read", mmusr);
            psmac_mmu_debug_walk(la, user ? 1 : 5);
        }
    }

    /* commit: registers, FPSR/FPCR the block had not stored yet, SR, stacks, PC; the abort returns into the JIT loop */
    if (e->fpsr != 0xff) setFPSR((uint32_t)ctx[e->fpsr]);
    if (e->fpcr != 0xff) setFPCR((uint32_t)ctx[e->fpcr] & 0xffff);
    for (int i = 0; i < 5; i++) ctx[13 + i] = a[i];
    __asm__ volatile("mov w27, %w0" :: "r"(a[5]));
    __asm__ volatile("mov w28, %w0" :: "r"(a[6]));
    __asm__ volatile("mov w29, %w0" :: "r"(f));
    if (user) setUSP(usp);
    setSR((sr_int | SR_S) & ~(SR_T0 | SR_T1));
    psmac_view_for(SR_S);                       /* the handler runs in the supervisor's view */
    ctx[18] = vec;
    __asm__ volatile("msr ELR_EL1, %0" :: "r"(lr));

    delivered++;
    el1_ticks += cnt_now() - t0;
    log_delivery(write ? "data write" : "data read", e->pc, write, la, ssw, user, vec);
    return 1;
}
