/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_fpu.c - the exact FPU: every 68881/68882 instruction (and the 68040's single and double rounding
 * forms), carried out in extended precision with Berkeley SoftFloat (issue #27, docs/17-exact-fpu.md).
 *
 * Backstory (22 November 2026). Photoshop's start-up spun for minutes in the IIci ROM's SANE, waiting for
 * an extended-precision refinement to settle (docs/08 item 101): Emu68 keeps the 68k's FPU registers as
 * AArch64 doubles, 53 bits of significand where the 68881 has 64, so a loop that stops when new minus
 * old is zero in extended precision may never stop. The fix then was to give the Mac the SE ROM's SANE,
 * which does extended arithmetic in integer instructions: exact, and 20 to 60 times slower than SANE on
 * an FPU. This file is the other fix: an FPU that is exact itself, so that the IIci ROM's FPU SANE, and
 * every program that uses the FPU directly, get the 68881's results bit for bit.
 *
 * How it runs. With mac.fpu=exact the JIT (M68k_LINEF.c, EMIT_FPU_exact) translates an FPU instruction
 * into a call here, with the AArch64 registers saved on the stack (the 68k's registers among them) and
 * the SR. This file reads the instruction from the 68k's memory, works out its effective address,
 * reads or writes the operands there (the 68k's addresses are the kernel's own on this core, as the
 * JIT's loads and stores use them; a bus address faults to the same handler as theirs) and does the
 * arithmetic on its own copy of the eight registers, FPCR, FPSR and FPIAR. SoftFloat 3e's extFloat80
 * is the 68881's format, its roundingPrecision the FPCR's precision control (significand rounded,
 * exponent range kept), its flags the IEEE exceptions; the 68881's own rules are added here: which NaN
 * a dyadic operation returns, SNaN against OPERR, the accrued byte, condition codes, the quotient byte,
 * the denormal convention of the memory format (below), FMOVEM's orders, FSAVE's frames.
 *
 * The memory format and SoftFloat. The 68881 reads a biased exponent of zero as 2^-16383 (Apple
 * Numerics Manual: v = (-1)^s 2^(e-16383) (i.f) for every e below 32767); SoftFloat, following the
 * 8087, reads it as 2^-16382, the weight of exponent one. So values are converted where they enter
 * and leave memory: a 68881 number with exponent zero becomes a SoftFloat denormal of the same value
 * (its last bit dropped when it has one: the only inexact case, below 2^-16382), and back. Numbers
 * with exponents above zero, every normal number a program meets, are the same bits in both.
 *
 * Exceptions. A trap enabled in the FPCR is taken after the instruction, as a 68040 does, with a
 * format 0 frame and the next instruction's PC; a 68882 would take it at the next FPU instruction and
 * describe it in its FSAVE frame, which this FPU does not keep (FSAVE writes an idle frame). The Mac
 * leaves the traps off (SANE's halts are its own), so no program seen so far notices.
 */
#include <stdbool.h>
#include <stdint.h>
#include "psmac_fpu.h"
#include "psmac_fpu_int.h"
#include "internals.h"

struct psmac_fpu_state psmac_fpu;
int psmac_fpu_exact = 1;

/* ------------------------------------------------------------------ memory */

#ifdef PSMAC_FPU_HOST
/* the host tests (tools/fpu_test.c) give a memory of their own */
uint8_t psmac_fpu_rd8(uint32_t a);
uint16_t psmac_fpu_rd16(uint32_t a);
uint32_t psmac_fpu_rd32(uint32_t a);
void psmac_fpu_wr8(uint32_t a, uint8_t v);
void psmac_fpu_wr16(uint32_t a, uint16_t v);
void psmac_fpu_wr32(uint32_t a, uint32_t v);
#define RD8  psmac_fpu_rd8
#define RD16 psmac_fpu_rd16
#define RD32 psmac_fpu_rd32
#define WR8  psmac_fpu_wr8
#define WR16 psmac_fpu_wr16
#define WR32 psmac_fpu_wr32
#else
/* the kernel is big-endian, as the 68k is: a load is the 68k's value. Plain loads and stores of one
   size each, the forms the bus fault handler decodes (aarch64/vectors.c) */
static uint8_t  RD8(uint32_t a)  { return *(volatile uint8_t *)(uintptr_t)a; }
static uint16_t RD16(uint32_t a) { return *(volatile uint16_t *)(uintptr_t)a; }
static uint32_t RD32(uint32_t a) { return *(volatile uint32_t *)(uintptr_t)a; }
static void WR8(uint32_t a, uint8_t v)   { *(volatile uint8_t *)(uintptr_t)a = v; }
static void WR16(uint32_t a, uint16_t v) { *(volatile uint16_t *)(uintptr_t)a = v; }
static void WR32(uint32_t a, uint32_t v) { *(volatile uint32_t *)(uintptr_t)a = v; }
#endif

/* ------------------------------------------------------------------ FPSR and FPCR */

#define CC_N    0x08000000u
#define CC_Z    0x04000000u
#define CC_I    0x02000000u
#define CC_NAN  0x01000000u
#define EX_BSUN  0x8000u
#define EX_SNAN  0x4000u
#define EX_OPERR 0x2000u
#define EX_OVFL  0x1000u
#define EX_UNFL  0x0800u
#define EX_DZ    0x0400u
#define EX_INEX2 0x0200u
#define EX_INEX1 0x0100u
#define AE_IOP  0x80u
#define AE_OVFL 0x40u
#define AE_UNFL 0x20u
#define AE_DZ   0x10u
#define AE_INEX 0x08u

/* results that need an exception taken: (format << 12) | the vector's offset in the table (vector * 4),
   the JIT's own convention (M68k.h: VECTOR_LINE_F is 0x02c) */
#define V_PRIV   (8u * 4)
#define V_LINEF  (11u * 4)
#define V_TRAPCC (0x2000u | 7u * 4)

static uint8_t prec_of_fpcr(void)
{
    switch ((psmac_fpu.fpcr >> 6) & 3) {
    case 1: return 32;
    case 2: return 64;
    default: return 80;
    }
}

static void set_mode(void)
{
    /* FPCR mode bits 5-4 and SoftFloat's modes are numbered alike: nearest, zero, minus, plus */
    softfloat_roundingMode = (uint_fast8_t)((psmac_fpu.fpcr >> 4) & 3);
}

/* ------------------------------------------------------------------ values */

static extFloat80_t X(uint16_t se, uint64_t m) { extFloat80_t x; x.signExp = se; x.signif = m; return x; }
static extFloat80_t reg_get(int n) { return X(psmac_fpu.fp[n].se, psmac_fpu.fp[n].m); }
static void reg_put(int n, extFloat80_t x) { psmac_fpu.fp[n].se = x.signExp; psmac_fpu.fp[n].m = x.signif; }

static bool x_isnan(extFloat80_t x) { return (x.signExp & 0x7fff) == 0x7fff && (x.signif << 1); }
static bool x_isinf(extFloat80_t x) { return (x.signExp & 0x7fff) == 0x7fff && !(x.signif << 1); }
static bool x_iszero(extFloat80_t x) { return !(x.signExp & 0x7fff) && !x.signif; }
static bool x_issnan(extFloat80_t x) { return x_isnan(x) && !(x.signif & UINT64_C(0x4000000000000000)); }
static bool x_neg(extFloat80_t x) { return x.signExp >> 15; }
static extFloat80_t x_quiet(extFloat80_t x) { x.signif |= UINT64_C(0xC000000000000000); return x; }
static const extFloat80_t x_default_nan = { .signExp = 0x7fff, .signif = UINT64_C(0xFFFFFFFFFFFFFFFF) };

/* the 68881's memory format to SoftFloat's (above): unnormals normalized, exponent zero converted */
static extFloat80_t x_from_mem(uint16_t se, uint64_t m)
{
    uint16_t sign = se & 0x8000;
    int32_t e = se & 0x7fff;
    if (e == 0x7fff)
        return (m << 1) ? X(se, m | UINT64_C(0x8000000000000000)) : X(se, UINT64_C(0x8000000000000000));
    if (!m)
        return X(sign, 0);
    int shift = __builtin_clzll(m);
    if (e - shift >= 1)
        return X(sign | (uint16_t)(e - shift), m << shift);
    /* tiny for SoftFloat: value m * 2^(e - 16446) is a denormal m' * 2^-16445 */
    if (e >= 1)
        return X(sign, m << (e - 1));
    m >>= 1;                                      /* exponent zero: one bit SoftFloat cannot hold */
    return X(sign, m);
}

static void x_to_mem(extFloat80_t x, uint16_t *se, uint64_t *m)
{
    *se = x.signExp;
    *m = x.signif;
    if (!(x.signExp & 0x7fff) && x.signif)
        *m = x.signif << 1;                       /* a SoftFloat denormal: the 68881's exponent-zero number */
}

/* round to prec as an operation's result would be (FMOVE, FABS, FNEG, FINT: the FPCR's precision) */
static extFloat80_t x_round(extFloat80_t x, uint8_t prec)
{
    int32_t e = x.signExp & 0x7fff;
    if (e == 0x7fff || x_iszero(x) || prec >= 80)
        return x;
    if (!e) e = 1;
    return softfloat_normRoundPackToExtF80(x_neg(x), e, x.signif, 0, prec);
}

extFloat80_t psmac_fpu_round(bool sign, int32_t exp, uint64_t sig, uint64_t extra, uint8_t prec)
{
    return softfloat_roundPackToExtF80(sign, exp, sig, extra, prec);
}

/* ------------------------------------------------------------------ the instruction's context */

struct ctx {
    uint64_t *r;
    uint32_t pc;        /* the instruction's address */
    uint32_t ext;       /* the next extension word */
    uint32_t sr;
    bool bad;           /* an addressing mode the instruction cannot have */
};

static uint32_t Dn(struct ctx *c, int n) { return (uint32_t)c->r[PSMAC_FPU_X_D(n)]; }
static uint32_t An(struct ctx *c, int n) { return (uint32_t)c->r[PSMAC_FPU_X_A(n)]; }
static void setDn(struct ctx *c, int n, uint32_t v) { c->r[PSMAC_FPU_X_D(n)] = v; }
static void setAn(struct ctx *c, int n, uint32_t v) { c->r[PSMAC_FPU_X_A(n)] = v; }
static uint16_t ext16(struct ctx *c) { uint16_t w = RD16(c->ext); c->ext += 2; return w; }
static uint32_t ext32(struct ctx *c) { uint32_t w = RD32(c->ext); c->ext += 4; return w; }

/* (d8,An,Xn) and the 68020's full extension word with its memory indirect forms */
static uint32_t ea_index(struct ctx *c, uint32_t base)
{
    uint16_t w = ext16(c);
    int xr = (w >> 12) & 15;
    uint32_t x = xr < 8 ? Dn(c, xr) : An(c, xr - 8);
    if (!(w & 0x0800)) x = (uint32_t)(int32_t)(int16_t)x;
    x <<= (w >> 9) & 3;
    if (!(w & 0x0100))
        return base + (uint32_t)(int32_t)(int8_t)w + x;
    int32_t bd = 0, od = 0;
    switch ((w >> 4) & 3) {
    case 0: c->bad = true; return 0;
    case 2: bd = (int16_t)ext16(c); break;
    case 3: bd = (int32_t)ext32(c); break;
    }
    if (w & 0x80) base = 0;
    if (w & 0x40) x = 0;
    int iis = w & 7;
    switch (iis & 3) {
    case 2: od = (int16_t)ext16(c); break;
    case 3: od = (int32_t)ext32(c); break;
    }
    if (!iis)
        return base + (uint32_t)bd + x;
    if (iis & 4)
        return RD32(base + (uint32_t)bd) + x + (uint32_t)od;
    return RD32(base + (uint32_t)bd + x) + (uint32_t)od;
}

/* set bits of a register list (__builtin_popcount wants NEON's CNT or libgcc, neither of which this file has) */
static int count_bits(uint32_t v) { int n = 0; for (; v; v &= v - 1) n++; return n; }

enum { EA_D, EA_A, EA_MEM, EA_IMM };
struct ea { int kind, reg; uint32_t addr; };

/* the effective address for an operand of size bytes; (An)+ and -(An) are applied at once */
static bool ea_get(struct ctx *c, int mode, int reg, int size, struct ea *e)
{
    e->reg = reg;
    e->addr = 0;
    switch (mode) {
    case 0: e->kind = EA_D; return true;
    case 1: e->kind = EA_A; return true;
    case 2: e->kind = EA_MEM; e->addr = An(c, reg); return true;
    case 3: {
        int step = (size == 1 && reg == 7) ? 2 : size;
        e->kind = EA_MEM; e->addr = An(c, reg); setAn(c, reg, e->addr + step); return true;
    }
    case 4: {
        int step = (size == 1 && reg == 7) ? 2 : size;
        e->kind = EA_MEM; e->addr = An(c, reg) - step; setAn(c, reg, e->addr); return true;
    }
    case 5: e->kind = EA_MEM; e->addr = An(c, reg) + (uint32_t)(int32_t)(int16_t)ext16(c); return true;
    case 6: e->kind = EA_MEM; e->addr = ea_index(c, An(c, reg)); return !c->bad;
    case 7:
        switch (reg) {
        case 0: e->kind = EA_MEM; e->addr = (uint32_t)(int32_t)(int16_t)ext16(c); return true;
        case 1: e->kind = EA_MEM; e->addr = ext32(c); return true;
        case 2: { uint32_t b = c->ext; e->kind = EA_MEM; e->addr = b + (uint32_t)(int32_t)(int16_t)ext16(c); return true; }
        case 3: { uint32_t b = c->ext; e->kind = EA_MEM; e->addr = ea_index(c, b); return !c->bad; }
        case 4:
            e->kind = EA_IMM;
            e->addr = c->ext + (size == 1 ? 1 : 0);
            c->ext += size == 1 ? 2 : (uint32_t)size;
            return true;
        }
    }
    return false;
}

/* extension words of an effective address with an operand of size bytes (for psmac_fpu_length) */
static int ea_words(int mode, int reg, int size, uint16_t first)
{
    if (mode == 5 || (mode == 7 && (reg == 0 || reg == 2))) return 1;
    if (mode == 7 && reg == 1) return 2;
    if (mode == 7 && reg == 4) return size <= 2 ? 1 : size / 2;
    if (mode == 6 || (mode == 7 && reg == 3)) {
        if (!(first & 0x0100)) return 1;
        int n = 1;
        switch ((first >> 4) & 3) { case 2: n += 1; break; case 3: n += 2; break; }
        switch (first & 3) { case 2: n += 1; break; case 3: n += 2; break; }
        return n;
    }
    if (mode == 7) return -1;
    return 0;
}

/* ------------------------------------------------------------------ data formats */

enum { F_L, F_S, F_X, F_P, F_W, F_D, F_B, F_PK };
static const int fmt_size[8] = { 4, 4, 12, 12, 2, 8, 1, 12 };

static bool f32_snan(uint32_t u) { return (u & 0x7fc00000) == 0x7f800000 && (u & 0x003fffff); }
static bool f64_snan(uint64_t u) { return (u & UINT64_C(0x7ff8000000000000)) == UINT64_C(0x7ff0000000000000) && (u & UINT64_C(0x0007ffffffffffff)); }

/* read a source operand in format fmt; *snan when it is a signalling NaN, *inex1 when a decimal
   string did not convert exactly. Returns false for a mode the format cannot have. */
static bool load_src(struct ctx *c, int fmt, int mode, int reg, extFloat80_t *out, bool *snan, bool *inex1)
{
    struct ea e;
    int size = fmt_size[fmt];
    *snan = false;
    *inex1 = false;
    if (!ea_get(c, mode, reg, size, &e)) return false;
    uint32_t v = 0;
    if (e.kind == EA_A) return false;
    if (e.kind == EA_D) {
        if (fmt == F_X || fmt == F_P || fmt == F_D) return false;
        v = Dn(c, reg);
    } else if (fmt == F_B) v = RD8(e.addr);
    else if (fmt == F_W) v = RD16(e.addr);
    else if (fmt == F_L || fmt == F_S) v = RD32(e.addr);
    switch (fmt) {
    case F_L: *out = i32_to_extF80((int32_t)v); return true;
    case F_W: *out = i32_to_extF80((int16_t)v); return true;
    case F_B: *out = i32_to_extF80((int8_t)v); return true;
    case F_S: { float32_t f; f.v = v; *snan = f32_snan(v); *out = f32_to_extF80(f); return true; }
    case F_D: {
        float64_t f;
        f.v = (uint64_t)RD32(e.addr) << 32 | RD32(e.addr + 4);
        *snan = f64_snan(f.v);
        *out = f64_to_extF80(f);
        return true;
    }
    case F_X: {
        uint16_t se = RD16(e.addr);
        uint64_t m = (uint64_t)RD32(e.addr + 4) << 32 | RD32(e.addr + 8);
        *out = x_from_mem(se, m);
        *snan = x_issnan(*out);
        return true;
    }
    case F_P: {
        uint32_t p[3] = { RD32(e.addr), RD32(e.addr + 4), RD32(e.addr + 8) };
        softfloat_exceptionFlags = 0;
        *out = psmac_fpu_from_packed(p, 80);
        *inex1 = softfloat_exceptionFlags & softfloat_flag_inexact;
        *snan = x_issnan(*out);
        return true;
    }
    }
    return false;
}

/* ------------------------------------------------------------------ flags */

static void set_cc(extFloat80_t r)
{
    uint32_t cc = 0;
    if (x_neg(r)) cc |= CC_N;
    if (x_isnan(r)) cc |= CC_NAN;
    else if (x_isinf(r)) cc |= CC_I;
    else if (x_iszero(r)) cc |= CC_Z;
    psmac_fpu.fpsr = (psmac_fpu.fpsr & ~0x0f000000u) | cc;
}

/* the exception byte from SoftFloat's flags and the 68881's own conditions, the accrued byte from it */
static void set_exc(uint32_t exc)
{
    uint_fast8_t f = softfloat_exceptionFlags;
    if ((f & softfloat_flag_invalid) && !(exc & EX_SNAN)) exc |= EX_OPERR;
    if (f & softfloat_flag_overflow) exc |= EX_OVFL;
    if (f & softfloat_flag_underflow) exc |= EX_UNFL;
    if (f & softfloat_flag_infinite) exc |= EX_DZ;
    if (f & softfloat_flag_inexact) exc |= EX_INEX2;
    uint32_t ae = 0;
    if (exc & (EX_SNAN | EX_OPERR)) ae |= AE_IOP;
    if (exc & EX_OVFL) ae |= AE_OVFL;
    if ((exc & EX_UNFL) && (exc & EX_INEX2)) ae |= AE_UNFL;
    if (exc & EX_DZ) ae |= AE_DZ;
    if (exc & (EX_INEX1 | EX_INEX2 | EX_OVFL)) ae |= AE_INEX;
    psmac_fpu.fpsr = (psmac_fpu.fpsr & ~0xff00u) | exc | ae;
}

/* an exception enabled in the FPCR that the last instruction raised: its vector, highest priority first */
static uint32_t trap_vector(void)
{
    uint32_t en = (psmac_fpu.fpcr & psmac_fpu.fpsr) & 0xff00u;
    if (!en) return 0;
    if (en & EX_BSUN) return 48 * 4;
    if (en & EX_SNAN) return 54 * 4;
    if (en & EX_OPERR) return 52 * 4;
    if (en & EX_OVFL) return 53 * 4;
    if (en & EX_UNFL) return 51 * 4;
    if (en & EX_DZ) return 50 * 4;
    return 49 * 4;                                /* INEX1, INEX2 */
}

/* ------------------------------------------------------------------ FMOD and FREM */

/* the remainder of a / b with the quotient rounded toward zero (FMOD) or to nearest (FREM), exactly;
   the quotient's sign and low seven bits go in the FPSR's quotient byte */
static extFloat80_t x_remainder(extFloat80_t a, extFloat80_t b, bool nearest, uint8_t prec)
{
    bool qsign = x_neg(a) ^ x_neg(b);
    uint32_t q = 0;
    extFloat80_t r;
    if (x_isinf(a) || x_iszero(b)) {
        softfloat_raiseFlags(softfloat_flag_invalid);
        r = x_default_nan;
        goto out;
    }
    if (x_isinf(b) || x_iszero(a)) {
        r = x_round(a, prec);
        goto out;
    }
    {
        struct exp32_sig64 na, nb;
        int32_t ea = a.signExp & 0x7fff, eb = b.signExp & 0x7fff;
        uint64_t sa = a.signif, sb = b.signif;
        if (!ea) { na = softfloat_normSubnormalExtF80Sig(sa); ea = 1 + na.exp; sa = na.sig; }
        if (!eb) { nb = softfloat_normSubnormalExtF80Sig(sb); eb = 1 + nb.exp; sb = nb.sig; }
        bool rsign = x_neg(a);
        int32_t d = ea - eb;
        /* r holds the remainder at b's scale, with one bit below it (so that a 2^-1 b is exact) */
        unsigned __int128 rr, bb = (unsigned __int128)sb << 1;
        if (d < -1) {
            r = x_round(a, prec);
            goto out;
        }
        if (d == -1) {
            rr = sa;                              /* a = sa * 2^(eb-1-63): at b's scale, sa / 2 */
            q = 0;
        } else {
            rr = (unsigned __int128)sa << 1;
            for (int32_t i = 0; i <= d; i++) {
                q <<= 1;
                if (rr >= bb) { rr -= bb; q |= 1; }
                if (i < d) rr <<= 1;
            }
        }
        if (nearest) {
            /* compare r with b/2: 2r against b, both at the scale above */
            unsigned __int128 twice = rr << 1;
            if (twice > bb || (twice == bb && (q & 1))) {
                rr = bb - rr;
                q += 1;
                rsign = !rsign;
            }
        }
        if (!rr) {
            r = X(rsign ? 0x8000 : 0, 0);
            goto out;
        }
        /* the remainder is rr * 2^(eb - 16383 - 64); normalized to bit 127 it is hi.lo * 2^(eb - lz - 16383),
           SoftFloat's exponent eb - lz + 63. Exact, unless the FPCR's precision is shorter. */
        int lz = (rr >> 64) ? __builtin_clzll((uint64_t)(rr >> 64)) : 64 + __builtin_clzll((uint64_t)rr);
        unsigned __int128 n = rr << lz;
        r = softfloat_roundPackToExtF80(rsign, eb - lz + 63, (uint64_t)(n >> 64), (uint64_t)n, prec);
    }
out:
    psmac_fpu.fpsr = (psmac_fpu.fpsr & ~0x00ff0000u) | (qsign ? 0x00800000u : 0) | ((q & 0x7f) << 16);
    return r;
}

/* ------------------------------------------------------------------ FSCALE, FGETEXP, FGETMAN */

static extFloat80_t fscale(extFloat80_t a, extFloat80_t b, uint8_t prec)
{
    if (x_isinf(b)) { softfloat_raiseFlags(softfloat_flag_invalid); return x_default_nan; }
    if (x_isinf(a) || x_iszero(a)) return a;
    int32_t n = extF80_to_i32_r_minMag(b, false);   /* toward zero; saturates on huge b, with OPERR from SoftFloat */
    softfloat_exceptionFlags &= ~softfloat_flag_invalid;
    if (n > 0x10000) n = 0x10000;
    if (n < -0x10000) n = -0x10000;
    int32_t e = a.signExp & 0x7fff;
    uint64_t s = a.signif;
    if (!e) { struct exp32_sig64 ns = softfloat_normSubnormalExtF80Sig(s); e = 1 + ns.exp; s = ns.sig; }
    return softfloat_roundPackToExtF80(x_neg(a), e + n, s, 0, prec);
}

static extFloat80_t fgetexp(extFloat80_t a)
{
    if (x_isinf(a)) { softfloat_raiseFlags(softfloat_flag_invalid); return x_default_nan; }
    if (x_iszero(a)) return a;
    int32_t e = a.signExp & 0x7fff;
    if (!e) e = 1 + softfloat_normSubnormalExtF80Sig(a.signif).exp;   /* SoftFloat: a denormal has exponent 1 */
    return i32_to_extF80(e - 0x3fff);
}

static extFloat80_t fgetman(extFloat80_t a)
{
    if (x_isinf(a)) { softfloat_raiseFlags(softfloat_flag_invalid); return x_default_nan; }
    if (x_iszero(a)) return a;
    uint64_t s = a.signif;
    if (!(a.signExp & 0x7fff)) s = softfloat_normSubnormalExtF80Sig(s).sig;
    return X((a.signExp & 0x8000) | 0x3fff, s);
}

/* ------------------------------------------------------------------ arithmetic (opclasses 000 and 010) */

static bool is_dyadic(int op)
{
    switch (op) {
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
    case 0x28: case 0x38:
    case 0x60: case 0x62: case 0x63: case 0x64: case 0x66: case 0x67: case 0x68: case 0x6c:
        return true;
    }
    return false;
}

static bool is_known_op(int op)
{
    if (op < 0x40) {
        switch (op) {
        case 0x05: case 0x07: case 0x0b: case 0x13: case 0x17: case 0x1b:
        case 0x29: case 0x2a: case 0x2b: case 0x2c: case 0x2d: case 0x2e: case 0x2f:
        case 0x39: case 0x3b: case 0x3c: case 0x3d: case 0x3e: case 0x3f:
            return false;
        }
        return true;
    }
    switch (op) {
    case 0x40: case 0x44: case 0x41: case 0x45: case 0x58: case 0x5c: case 0x5a: case 0x5e:
    case 0x60: case 0x64: case 0x62: case 0x66: case 0x63: case 0x67: case 0x68: case 0x6c:
        return true;
    }
    return false;
}

/* FPn = FPn op src (or op src); src already in SoftFloat's form */
static void arith(int op, int dst, extFloat80_t src, bool snan, bool inex1)
{
    extFloat80_t a = reg_get(dst), r;
    uint8_t prec = prec_of_fpcr();
    uint32_t exc = inex1 ? EX_INEX1 : 0;

    if (op >= 0x40) prec = (op & 4) ? 64 : 32;     /* FSxxx and FDxxx */
    if (op == 0x24 || op == 0x27) prec = 32;      /* FSGLDIV, FSGLMUL */
    if (is_dyadic(op) && x_issnan(a)) snan = true;
    if (snan) exc |= EX_SNAN;

    set_mode();
    extF80_roundingPrecision = prec;
    softfloat_exceptionFlags = 0;

    /* NaN operands: the destination's for a dyadic operation, else the source's, quieted */
    if (x_isnan(src) || (is_dyadic(op) && x_isnan(a))) {
        if (op == 0x38) {                         /* FCMP: unordered */
            psmac_fpu.fpsr = (psmac_fpu.fpsr & ~0x0f000000u) | CC_NAN;
            set_exc(exc);
            return;
        }
        r = x_quiet((is_dyadic(op) && x_isnan(a)) ? a : src);
        if (op == 0x3a) { set_cc(r); set_exc(exc); return; }          /* FTST */
        if (op >= 0x30 && op <= 0x37) reg_put(op & 7, r);           /* FSINCOS: both get it */
        if (op == 0x21 || op == 0x25) psmac_fpu.fpsr &= ~0x00ff0000u;
        reg_put(dst, r);
        set_cc(r);
        set_exc(exc);
        return;
    }

    switch (op) {
    case 0x00: case 0x40: case 0x44: r = x_round(src, prec); break;                  /* FMOVE */
    case 0x01: r = x_round(extF80_roundToInt(src, softfloat_roundingMode, true), prec); break;  /* FINT */
    case 0x03: r = x_round(extF80_roundToInt(src, softfloat_round_minMag, true), prec); break;   /* FINTRZ */
    case 0x04: case 0x41: case 0x45: r = extF80_sqrt(src); break;
    case 0x18: case 0x58: case 0x5c: src.signExp &= 0x7fff; r = x_round(src, prec); break;      /* FABS */
    case 0x1a: case 0x5a: case 0x5e: src.signExp ^= 0x8000; r = x_round(src, prec); break;      /* FNEG */
    case 0x1e: r = fgetexp(src); break;
    case 0x1f: r = fgetman(src); break;
    case 0x20: case 0x24: case 0x60: case 0x64: r = extF80_div(a, src); break;
    case 0x22: case 0x62: case 0x66: r = extF80_add(a, src); break;
    case 0x23: case 0x27: case 0x63: case 0x67: r = extF80_mul(a, src); break;
    case 0x28: case 0x68: case 0x6c: r = extF80_sub(a, src); break;
    case 0x21: r = x_remainder(a, src, false, prec); break;
    case 0x25: r = x_remainder(a, src, true, prec); break;
    case 0x26: r = fscale(a, src, prec); break;
    case 0x38: {                                  /* FCMP: the condition codes of dst - src */
        uint32_t cc = 0;
        if (extF80_eq(a, src)) {
            cc = CC_Z;
            if (x_neg(a) && x_isinf(a)) cc |= CC_N;
        } else if (extF80_lt(a, src)) cc = CC_N;
        psmac_fpu.fpsr = (psmac_fpu.fpsr & ~0x0f000000u) | cc;
        set_exc(exc);
        return;
    }
    case 0x3a: set_cc(src); set_exc(exc); return;                                      /* FTST */
    default:
        if (op >= 0x30 && op <= 0x37) {           /* FSINCOS: cosine to FPc (low bits), sine to FPs (dst) */
            extFloat80_t c;
            r = psmac_fpu_elementary(FOP_SINCOS, src, prec, &c);
            reg_put(op & 7, c);
        } else
            r = psmac_fpu_elementary(op, src, prec, 0);
        break;
    }
    reg_put(dst, r);
    set_cc(r);
    set_exc(exc);
}

/* ------------------------------------------------------------------ FMOVE out (opclass 011) */

static uint32_t to_int(extFloat80_t a, int bits, uint32_t *exc)
{
    int32_t lo = bits == 32 ? INT32_MIN : -(1 << (bits - 1)), hi = bits == 32 ? INT32_MAX : (1 << (bits - 1)) - 1;
    uint32_t mask = bits == 32 ? 0xffffffffu : (1u << bits) - 1;
    if (x_isnan(a)) {                             /* OPERR, and the NaN's leading significand bits */
        softfloat_raiseFlags(softfloat_flag_invalid);
        if (x_issnan(a)) *exc |= EX_SNAN | EX_OPERR;
        return (uint32_t)(a.signif >> (64 - bits)) & mask;
    }
    int32_t v = extF80_to_i32(a, softfloat_roundingMode, true);
    if (v < lo || v > hi) {
        softfloat_exceptionFlags = (softfloat_exceptionFlags & ~softfloat_flag_inexact) | softfloat_flag_invalid;
        v = x_neg(a) ? lo : hi;
    }
    return (uint32_t)v & mask;
}

static bool move_out(struct ctx *c, uint16_t op, uint16_t cmd)
{
    int fmt = (cmd >> 10) & 7, src = (cmd >> 7) & 7;
    int mode = (op >> 3) & 7, reg = op & 7;
    extFloat80_t a = reg_get(src);
    struct ea e;
    uint32_t exc = 0;
    if (x_issnan(a)) exc |= EX_SNAN;
    set_mode();
    softfloat_exceptionFlags = 0;
    if (!ea_get(c, mode, reg, fmt_size[fmt], &e) || e.kind == EA_A || e.kind == EA_IMM) return false;
    if (e.kind == EA_D && (fmt == F_X || fmt == F_P || fmt == F_D || fmt == F_PK)) return false;
    uint32_t v;
    switch (fmt) {
    case F_L: v = to_int(a, 32, &exc); if (e.kind == EA_D) setDn(c, reg, v); else WR32(e.addr, v); break;
    case F_W:
        v = to_int(a, 16, &exc);
        if (e.kind == EA_D) setDn(c, reg, (Dn(c, reg) & 0xffff0000u) | v); else WR16(e.addr, (uint16_t)v);
        break;
    case F_B:
        v = to_int(a, 8, &exc);
        if (e.kind == EA_D) setDn(c, reg, (Dn(c, reg) & 0xffffff00u) | v); else WR8(e.addr, (uint8_t)v);
        break;
    case F_S: v = extF80_to_f32(a).v; if (e.kind == EA_D) setDn(c, reg, v); else WR32(e.addr, v); break;
    case F_D: {
        uint64_t d = extF80_to_f64(a).v;
        WR32(e.addr, (uint32_t)(d >> 32));
        WR32(e.addr + 4, (uint32_t)d);
        break;
    }
    case F_X: {
        uint16_t se; uint64_t m;
        x_to_mem(a, &se, &m);
        WR16(e.addr, se);
        WR16(e.addr + 2, 0);
        WR32(e.addr + 4, (uint32_t)(m >> 32));
        WR32(e.addr + 8, (uint32_t)m);
        break;
    }
    case F_P: case F_PK: {
        int k = fmt == F_P ? (int8_t)((cmd & 0x7f) << 1) >> 1 : (int8_t)Dn(c, (cmd >> 4) & 7);
        uint32_t p[3];
        if (psmac_fpu_to_packed(a, k, p)) softfloat_raiseFlags(softfloat_flag_invalid);
        if (x_issnan(a)) exc |= EX_SNAN;
        WR32(e.addr, p[0]); WR32(e.addr + 4, p[1]); WR32(e.addr + 8, p[2]);
        break;
    }
    }
    set_exc(exc);
    return true;
}

/* ------------------------------------------------------------------ FMOVE and FMOVEM of FPCR, FPSR, FPIAR */

static bool move_control(struct ctx *c, uint16_t op, uint16_t cmd)
{
    bool to_mem = cmd & 0x2000;
    int sel = (cmd >> 10) & 7, n = count_bits(sel);
    int mode = (op >> 3) & 7, reg = op & 7;
    struct ea e;
    if (!sel) sel = 1, n = 1;                     /* none selected: FPIAR, as the 68881 does */
    if ((mode == 0 || mode == 1) && n != 1) return false;
    if (mode == 1 && sel != 1) return false;
    if (to_mem && mode == 7 && reg == 4) return false;
    if (!ea_get(c, mode, reg, 4 * n, &e)) return false;
    uint32_t *r[3] = { &psmac_fpu.fpcr, &psmac_fpu.fpsr, &psmac_fpu.fpiar };
    static const uint32_t mask[3] = { 0x0000fff0u, 0x0ffffff8u, 0xffffffffu };
    uint32_t addr = e.addr;
    for (int i = 0; i < 3; i++) {
        if (!(sel & (4 >> i))) continue;
        if (to_mem) {
            uint32_t v = *r[i];
            if (e.kind == EA_D) setDn(c, reg, v);
            else if (e.kind == EA_A) setAn(c, reg, v);
            else { WR32(addr, v); addr += 4; }
        } else {
            uint32_t v;
            if (e.kind == EA_D) v = Dn(c, reg);
            else if (e.kind == EA_A) v = An(c, reg);
            else { v = RD32(addr); addr += 4; }
            *r[i] = v & mask[i];
        }
    }
    return true;
}

/* ------------------------------------------------------------------ FMOVEM of data registers */

static bool movem(struct ctx *c, uint16_t op, uint16_t cmd)
{
    bool to_mem = cmd & 0x2000;
    int m = (cmd >> 11) & 3;                      /* 0, 1: predecrement order (static, dynamic list); 2, 3: the other */
    int mode = (op >> 3) & 7, reg = op & 7;
    uint8_t list = (m & 1) ? (uint8_t)Dn(c, (cmd >> 4) & 7) : (uint8_t)cmd;
    /* the list's order goes with -(An), the only mode that may have it (m 0 or 1); a program giving the
       other order with -(An), or the predecrement order with another mode, gets the mode's */
    if (mode == 0 || mode == 1 || (mode == 7 && reg == 4)) return false;
    if (!to_mem && mode == 4) return false;
    if (to_mem && mode == 3) return false;
    int count = count_bits(list);
    if (mode == 4) {
        /* list bit i is FPi; FP7 goes to the highest address, FP0 ends lowest */
        uint32_t a = An(c, reg);
        for (int i = 7; i >= 0; i--) {
            if (!(list & (1 << i))) continue;
            uint16_t se; uint64_t mm;
            x_to_mem(reg_get(i), &se, &mm);
            a -= 12;
            WR16(a, se); WR16(a + 2, 0); WR32(a + 4, (uint32_t)(mm >> 32)); WR32(a + 8, (uint32_t)mm);
        }
        setAn(c, reg, a);
        return true;
    }
    struct ea e;
    if (mode == 3) {
        e.addr = An(c, reg);
        setAn(c, reg, e.addr + 12 * count);
    } else if (!ea_get(c, mode, reg, 12, &e)) return false;
    uint32_t a = e.addr;
    /* control and postincrement: list bit 7 is FP0 */
    for (int i = 0; i < 8; i++) {
        if (!(list & (0x80 >> i))) continue;
        if (to_mem) {
            uint16_t se; uint64_t mm;
            x_to_mem(reg_get(i), &se, &mm);
            WR16(a, se); WR16(a + 2, 0); WR32(a + 4, (uint32_t)(mm >> 32)); WR32(a + 8, (uint32_t)mm);
        } else
            reg_put(i, x_from_mem(RD16(a), (uint64_t)RD32(a + 4) << 32 | RD32(a + 8)));
        a += 12;
    }
    return true;
}

/* ------------------------------------------------------------------ conditions */

/* the 32 predicates; 0x10 to 0x1f are the signalling forms of 0x00 to 0x0f (BSUN on NaN). *bsun_trap
   is set when that BSUN is enabled in the FPCR: the instruction then takes vector 48. */
static bool cond(int p, uint32_t *bsun_trap)
{
    uint32_t s = psmac_fpu.fpsr;
    bool n = s & CC_N, z = s & CC_Z, nan = s & CC_NAN;
    *bsun_trap = 0;
    if ((p & 0x10) && nan) {
        psmac_fpu.fpsr |= EX_BSUN | AE_IOP;
        if (psmac_fpu.fpcr & EX_BSUN) *bsun_trap = 48 * 4;
    }
    switch (p & 0x0f) {
    case 0x0: return false;                       /* F, SF */
    case 0x1: return z;                           /* EQ, SEQ */
    case 0x2: return !(nan || z || n);            /* OGT, GT */
    case 0x3: return z || !(nan || n);            /* OGE, GE */
    case 0x4: return n && !(nan || z);            /* OLT, LT */
    case 0x5: return z || (n && !nan);            /* OLE, LE */
    case 0x6: return !(nan || z);                 /* OGL, GL */
    case 0x7: return !nan;                        /* OR, GLE */
    case 0x8: return nan;                         /* UN, NGLE */
    case 0x9: return nan || z;                    /* UEQ, NGL */
    case 0xa: return nan || !(n || z);            /* UGT, NLE */
    case 0xb: return nan || z || !n;              /* UGE, NLT */
    case 0xc: return nan || (n && !z);            /* ULT, NGE */
    case 0xd: return nan || z || n;               /* ULE, NGT */
    case 0xe: return !z;                          /* NE, SNE */
    default:  return true;                        /* T, ST */
    }
}

/* ------------------------------------------------------------------ lengths */

int psmac_fpu_length(uint16_t op, uint16_t w1, uint16_t w2, int *ends)
{
    int type = (op >> 6) & 7, mode = (op >> 3) & 7, reg = op & 7, n;
    *ends = 0;
    if ((op & 0xfe00) != 0xf200) { *ends = 1; return 0; }
    switch (type) {
    case 0: {                                     /* general: opcode, command word, the address's words */
        int opclass = w1 >> 13, size = 12;
        if (opclass == 1) break;
        if (opclass == 0) return 2;               /* register to register: the address field is not used */
        if (opclass == 2 || opclass == 3) {
            if (opclass == 2 && ((w1 >> 10) & 7) == 7) return (op & 0x3f) == 0 ? 2 : 0;   /* FMOVECR */
            size = fmt_size[(w1 >> 10) & 7];
        }
        if (opclass == 4 || opclass == 5) {
            int sel = (w1 >> 10) & 7;
            size = 4 * (sel ? count_bits(sel) : 1);
        }
        n = ea_words(mode, reg, size, w2);
        if (n < 0) break;
        return 2 + n;
    }
    case 1:
        if (mode == 1) { *ends = 1; return 3; }                            /* FDBcc */
        if (mode == 7 && reg >= 2 && reg <= 4) { *ends = 1; return reg == 2 ? 3 : reg == 3 ? 4 : 2; } /* FTRAPcc */
        n = ea_words(mode, reg, 1, w2);                                   /* FScc */
        if (n < 0) break;
        return 2 + n;
    case 2: *ends = 1; return 2;                                          /* FBcc.W */
    case 3: *ends = 1; return 3;                                          /* FBcc.L */
    case 4: case 5:                                                       /* FSAVE, FRESTORE */
        n = ea_words(mode, reg, 4, w1);
        if (n < 0) break;
        return 1 + n;
    }
    *ends = 1;
    return 0;
}

/* ------------------------------------------------------------------ the instruction */

void psmac_fpu_reset(void)
{
    for (int i = 0; i < 8; i++) {
        psmac_fpu.fp[i].se = 0x7fff;
        psmac_fpu.fp[i].m = UINT64_C(0xffffffffffffffff);
    }
    psmac_fpu.fpcr = psmac_fpu.fpsr = psmac_fpu.fpiar = 0;
    psmac_fpu.null = 1;
}

static uint32_t general(struct ctx *c, uint16_t op, uint16_t cmd)
{
    int opclass = cmd >> 13;
    int mode = (op >> 3) & 7, reg = op & 7;
    switch (opclass) {
    case 0: case 2: {
        int opm = cmd & 0x7f, dst = (cmd >> 7) & 7;
        extFloat80_t src;
        bool snan = false, inex1 = false;
        if (opclass == 2 && ((cmd >> 10) & 7) == 7) {                     /* FMOVECR */
            if (op & 0x3f) return V_LINEF;
            set_mode();
            softfloat_exceptionFlags = 0;
            extFloat80_t k = psmac_fpu_constant(cmd & 0x7f, prec_of_fpcr());
            reg_put(dst, k);
            set_cc(k);
            set_exc(0);
            break;
        }
        if (!is_known_op(opm)) return V_LINEF;
        if (opclass == 0) {
            src = reg_get((cmd >> 10) & 7);
            snan = x_issnan(src);
        } else if (!load_src(c, (cmd >> 10) & 7, mode, reg, &src, &snan, &inex1))
            return V_LINEF;
        arith(opm, dst, src, snan, inex1);
        psmac_fpu.fpiar = c->pc;
        break;
    }
    case 3:
        if (!move_out(c, op, cmd)) return V_LINEF;
        psmac_fpu.fpiar = c->pc;
        break;
    case 4: case 5:
        if (!move_control(c, op, cmd)) return V_LINEF;
        return 0;
    case 6: case 7:
        if (!movem(c, op, cmd)) return V_LINEF;
        return 0;
    default:
        return V_LINEF;
    }
    return trap_vector();
}

/* an instruction that cannot be carried out: the exception is the instruction's own, its PC the instruction's */
static uint32_t fault(uint64_t *regs, uint32_t pc, uint32_t v)
{
    regs[PSMAC_FPU_X_PC] = pc;
    return v;
}

uint32_t psmac_fpu_exec(uint64_t *regs, uint32_t sr)
{
    struct ctx c = { .r = regs, .pc = (uint32_t)regs[PSMAC_FPU_X_PC], .sr = sr, .bad = false };
    uint16_t op = RD16(c.pc);
    int type = (op >> 6) & 7, mode = (op >> 3) & 7, reg = op & 7, ends;
    uint16_t w1 = RD16(c.pc + 2), w2 = RD16(c.pc + 4);
    int len = psmac_fpu_length(op, w1, w2, &ends);
    uint32_t next = c.pc + 2 * (uint32_t)len, v;

    if (!len) return V_LINEF;
    c.ext = c.pc + 2;
    regs[PSMAC_FPU_X_PC] = next;                  /* the JIT's PC is this from here on (EMIT_FPU_exact) */

    switch (type) {
    case 0:
        c.ext += 2;
        psmac_fpu.null = 0;
        v = general(&c, op, w1);
        if (v == V_LINEF) return fault(regs, c.pc, v);
        return v;

    case 1: {                                     /* FScc, FDBcc, FTRAPcc */
        int p = w1 & 0x3f;
        c.ext += 2;
        psmac_fpu.null = 0;
        psmac_fpu.fpiar = c.pc;
        if (p & 0x20) return fault(regs, c.pc, V_LINEF);
        if (mode == 1) {                          /* FDBcc Dn, label */
            int16_t disp = (int16_t)RD16(c.pc + 4);
            uint32_t target = c.pc + 4 + (uint32_t)(int32_t)disp;
            regs[PSMAC_FPU_X_PC] = next;
            if (!cond(p, &v)) {
                uint16_t cnt = (uint16_t)(Dn(&c, reg) - 1);
                setDn(&c, reg, (Dn(&c, reg) & 0xffff0000u) | cnt);
                if (cnt != 0xffff && !v) regs[PSMAC_FPU_X_PC] = target;
            }
            return v;
        }
        if (mode == 7 && reg >= 2 && reg <= 4) {  /* FTRAPcc (its operand is the handler's to read) */
            regs[PSMAC_FPU_X_PC] = next;
            bool t = cond(p, &v);
            return v ? v : t ? V_TRAPCC : 0;
        }
        struct ea e;                              /* FScc <ea> */
        bool t = cond(p, &v);
        if (!ea_get(&c, mode, reg, 1, &e) || e.kind == EA_IMM || e.kind == EA_A) return fault(regs, c.pc, V_LINEF);
        if (e.kind == EA_D) setDn(&c, reg, (Dn(&c, reg) & 0xffffff00u) | (t ? 0xff : 0));
        else WR8(e.addr, t ? 0xff : 0);
        return v;
    }

    case 2: case 3: {                             /* FBcc.W, FBcc.L (FNOP is FBF.W with no displacement) */
        int32_t disp = type == 2 ? (int16_t)w1 : (int32_t)((uint32_t)w1 << 16 | w2);
        psmac_fpu.null = 0;
        if (op & 0x20) return fault(regs, c.pc, V_LINEF);
        if (op != 0xf280 || w1) psmac_fpu.fpiar = c.pc;
        bool t = cond(op & 0x3f, &v);
        regs[PSMAC_FPU_X_PC] = t && !v ? c.pc + 2 + (uint32_t)disp : next;
        return v;
    }

    case 4: {                                     /* FSAVE */
        struct ea e;
        if (!(sr & 0x2000)) return fault(regs, c.pc, V_PRIV);
        if (mode == 0 || mode == 1 || mode == 3 || (mode == 7 && reg >= 2)) return fault(regs, c.pc, V_LINEF);
        /* a null frame after reset or FRESTORE of one; else a 68882's idle frame (version $1F, 56 bytes):
           the ROM's Gestalt 'fpu ' does fnop; fsave -(sp) and reads $1F38 as a 68882 (docs/08 item 46) */
        int size = psmac_fpu.null ? 4 : 4 + 0x38;
        if (!ea_get(&c, mode, reg, size, &e)) return fault(regs, c.pc, V_LINEF);
        if (psmac_fpu.null) WR32(e.addr, 0);
        else {
            WR32(e.addr, 0x1f380000u);
            for (int i = 4; i < size; i += 4)
                WR32(e.addr + (uint32_t)i, 0);
        }
        return 0;
    }

    case 5: {                                     /* FRESTORE */
        if (!(sr & 0x2000)) return fault(regs, c.pc, V_PRIV);
        if (mode == 0 || mode == 1 || mode == 4 || (mode == 7 && reg == 4)) return fault(regs, c.pc, V_LINEF);
        uint32_t addr, f;
        if (mode == 3) addr = An(&c, reg);
        else {
            struct ea e;
            if (!ea_get(&c, mode, reg, 4, &e)) return fault(regs, c.pc, V_LINEF);
            addr = e.addr;
        }
        f = RD32(addr);
        if (!(f >> 24)) {                         /* a null frame: the FPU's reset */
            psmac_fpu_reset();
            if (mode == 3) setAn(&c, reg, addr + 4);
            return 0;
        }
        /* an idle or busy frame: its registers are the FMOVEMs' to restore; FPU state carries none */
        psmac_fpu.null = 0;
        if (mode == 3) setAn(&c, reg, addr + 4 + ((f >> 16) & 0xff));
        return 0;
    }
    }
    return fault(regs, c.pc, V_LINEF);
}

#ifndef PSMAC_FPU_HOST
void kprintf(const char *format, ...);       /* support.h: not included, its RegLock.h wants the NEON registers this file may not touch */
extern const char *psmac_option(const char *key);

void psmac_fpu_init(void)
{
    const char *o = psmac_option("mac.fpu");
    psmac_fpu_reset();
    psmac_fpu_exact = !(o && o[0] == 'd');
    kprintf("[PSMAC] FPU: %s\n", psmac_fpu_exact ? "exact, a 68882 in extended precision (mac.fpu=exact)"
                                                 : "Emu68's, in double precision (mac.fpu=double)");
}
#endif
