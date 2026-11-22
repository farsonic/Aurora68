/* SPDX-License-Identifier: MPL-2.0
 *
 * fpu_test.c - the exact FPU (emu68/src/psmac/psmac_fpu.c, psmac_fpu_math.c and SoftFloat) on the host,
 * built from the kernel's own sources (tools/fpu_test.sh; issue #27, docs/17-exact-fpu.md).
 *
 * The 68k's memory is an array here, its registers the saved-register block the JIT passes. Each test
 * writes instruction words and operands, calls psmac_fpu_exec as the translated code would, and checks
 * registers, memory and FPSR. The oracle for the arithmetic is the host's own x87 (an x86-64's long
 * double is the 68881's format, and its precision control rounds the significand alone as the FPCR's
 * does): random operands in every rounding mode and precision must give the same bits and the same
 * inexact, overflow, invalid and divide-by-zero flags. The elementary functions are compared with the C
 * library's long double ones (correct to an ulp or two) and must stay within two ulps; the cases where
 * the 68881 is exact are checked to be exact. Written 22 November 2026.
 *
 * usage: fpu_test [-v] [--dump FILE] [random rounds, default 20000]
 */
#define _GNU_SOURCE
#include <fenv.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "psmac_fpu.h"

/* ------------------------------------------------------------------ the 68k's memory */
#define MEM_SIZE 0x100000u
static uint8_t mem[MEM_SIZE];
uint8_t psmac_fpu_rd8(uint32_t a) { return mem[a % MEM_SIZE]; }
uint16_t psmac_fpu_rd16(uint32_t a) { return (uint16_t)(mem[a % MEM_SIZE] << 8 | mem[(a + 1) % MEM_SIZE]); }
uint32_t psmac_fpu_rd32(uint32_t a) { return (uint32_t)psmac_fpu_rd16(a) << 16 | psmac_fpu_rd16(a + 2); }
void psmac_fpu_wr8(uint32_t a, uint8_t v) { mem[a % MEM_SIZE] = v; }
void psmac_fpu_wr16(uint32_t a, uint16_t v) { psmac_fpu_wr8(a, (uint8_t)(v >> 8)); psmac_fpu_wr8(a + 1, (uint8_t)v); }
void psmac_fpu_wr32(uint32_t a, uint32_t v) { psmac_fpu_wr16(a, (uint16_t)(v >> 16)); psmac_fpu_wr16(a + 2, (uint16_t)v); }

static uint64_t regs[31];
static int verbose, failures, checks;
static FILE *dump;                      /* --dump FILE: the elementary functions' results, for tools/fpu_refcheck.py */
#define CODE 0x1000u
#define DATA 0x2000u
#define SUPER 0x2700u

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; if (failures < 60) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static void setD(int n, uint32_t v) { regs[PSMAC_FPU_X_D(n)] = v; }
static uint32_t getD(int n) { return (uint32_t)regs[PSMAC_FPU_X_D(n)]; }
static void setA(int n, uint32_t v) { regs[PSMAC_FPU_X_A(n)] = v; }
static uint32_t getA(int n) { return (uint32_t)regs[PSMAC_FPU_X_A(n)]; }

/* run one instruction of n words at CODE; returns psmac_fpu_exec's result, *next the PC after */
static uint32_t run(const uint16_t *w, int n, uint32_t *pc_after)
{
    for (int i = 0; i < n; i++)
        psmac_fpu_wr16(CODE + 2 * (uint32_t)i, w[i]);
    regs[PSMAC_FPU_X_PC] = CODE;
    int ends;
    int len = psmac_fpu_length(w[0], w[1], n > 2 ? w[2] : 0, &ends);
    uint32_t v = psmac_fpu_exec(regs, SUPER);
    if (!ends && !v) regs[PSMAC_FPU_X_PC] = CODE + 2 * (uint32_t)len;   /* the JIT advances the PC itself */
    if (pc_after) *pc_after = (uint32_t)regs[PSMAC_FPU_X_PC];
    if (len != n && !v) { printf("length %d for %d words (%04x %04x)\n", len, n, w[0], w[1]); failures++; }
    return v;
}

static uint32_t run2(uint16_t a, uint16_t b) { uint16_t w[2] = { a, b }; return run(w, 2, 0); }

/* ------------------------------------------------------------------ values */
typedef union { long double ld; struct { uint64_t m; uint16_t se; } x; } LD;

static long double ld_of(uint16_t se, uint64_t m) { LD u; memset(&u, 0, sizeof u); u.x.se = se; u.x.m = m; return u.ld; }
static void put_fp(int n, long double v) { LD u; u.ld = v; psmac_fpu.fp[n].se = u.x.se; psmac_fpu.fp[n].m = u.x.m; }
static long double get_fp(int n) { return ld_of(psmac_fpu.fp[n].se, psmac_fpu.fp[n].m); }
static int same(long double a, long double b) { LD x, y; x.ld = a; y.ld = b; return x.x.se == y.x.se && x.x.m == y.x.m; }

static uint64_t rnd64(void) { return (uint64_t)rand() << 62 ^ (uint64_t)rand() << 31 ^ (uint64_t)rand(); }

/* a random extended value: mostly ordinary, some near the edges of the range, some denormal */
static long double rnd_x(void)
{
    int k = rand() % 16;
    uint16_t e;
    uint64_t m = rnd64() | UINT64_C(0x8000000000000000);
    if (k == 0) e = (uint16_t)(rand() % 64 + 1);                   /* tiny */
    else if (k == 1) e = (uint16_t)(0x7ffe - rand() % 64);         /* huge */
    else if (k == 2) { e = 0; m >>= rand() % 63 + 1; }             /* denormal */
    else if (k == 3) { e = (uint16_t)(0x3fff + rand() % 8); m &= ~((UINT64_C(1) << (rand() % 64)) - 1); m |= UINT64_C(1) << 63; }
    else e = (uint16_t)(0x3fff + rand() % 200 - 100);
    if (rand() & 1) e |= 0x8000;
    return ld_of(e, m);
}

static void x87_pc(int pc)       /* x87 precision control: 0 single, 2 double, 3 extended */
{
    uint16_t cw;
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    cw = (uint16_t)((cw & ~0x300) | (pc << 8));
    __asm__ volatile("fldcw %0" : : "m"(cw));
}

static const int fe_mode[4] = { FE_TONEAREST, FE_TOWARDZERO, FE_DOWNWARD, FE_UPWARD };

/* ------------------------------------------------------------------ arithmetic against the x87 */
static void test_random_arith(int rounds)
{
    static const struct { uint8_t opm; const char *name; } ops[] = {
        { 0x22, "FADD" }, { 0x28, "FSUB" }, { 0x23, "FMUL" }, { 0x20, "FDIV" }, { 0x04, "FSQRT" },
        { 0x25, "FREM" }, { 0x21, "FMOD" }, { 0x01, "FINT" }, { 0x03, "FINTRZ" }
    };
    int bad = 0;
    for (int i = 0; i < rounds; i++) {
        int o = rand() % 9, mode = rand() % 4, prec = rand() % 3;    /* prec: FPCR 0 ext, 1 single, 2 double */
        if (o >= 5) prec = 0;
        long double a = rnd_x(), b = rnd_x(), r;
        if (o == 4 || o == 7 || o == 8) b = fabsl(b) + (o == 4 ? 0 : 0);
        psmac_fpu.fpcr = (uint32_t)(prec << 6 | mode << 4);
        psmac_fpu.fpsr = 0;
        put_fp(1, a);
        put_fp(2, b);
        run2(0xf200, (uint16_t)(2 << 10 | 1 << 7 | ops[o].opm));       /* FPn = FP1 op FP2 */
        long double got = get_fp(1);
        uint32_t fpsr = psmac_fpu.fpsr;
        fesetround(fe_mode[mode]);
        x87_pc(prec == 0 ? 3 : prec == 1 ? 0 : 2);
        feclearexcept(FE_ALL_EXCEPT);
        volatile long double va = a, vb = b;
        switch (o) {
        case 0: r = va + vb; break;
        case 1: r = va - vb; break;
        case 2: r = va * vb; break;
        case 3: r = va / vb; break;
        case 4: r = sqrtl(vb); break;
        case 5: r = remainderl(va, vb); break;
        case 6: r = fmodl(va, vb); break;
        case 7: r = rintl(vb); break;
        default: r = truncl(vb); break;
        }
        int fx = fetestexcept(FE_INEXACT | FE_OVERFLOW | FE_INVALID | FE_DIVBYZERO);
        x87_pc(3);
        fesetround(FE_TONEAREST);
        if (isnan(r) && isnan(got)) continue;   /* the default NaNs differ (the 68881's has every bit set) */
        int ok = same(r, got);
        /* flags: inexact (not for FINT, the C library's rintl raises it its own way), overflow, invalid, divide by zero */
        if (ok && o < 5) {
            if (!!(fx & FE_INEXACT) != !!(fpsr & 0x0200)) ok = 0;
            if (!!(fx & FE_OVERFLOW) != !!(fpsr & 0x1000)) ok = 0;
            if (!!(fx & FE_INVALID) != !!(fpsr & 0x2000)) ok = 0;
            if (!!(fx & FE_DIVBYZERO) != !!(fpsr & 0x0400)) ok = 0;
        }
        if (!ok && bad++ < 20) {
            LD x, y, ua, ub; x.ld = r; y.ld = got; ua.ld = a; ub.ld = b;
            printf("FAIL %s mode %d prec %d: %04x:%016llx op %04x:%016llx = %04x:%016llx (x87 %04x:%016llx) fpsr %08x x87 flags %x\n",
                   ops[o].name, mode, prec, ua.x.se, (unsigned long long)ua.x.m, ub.x.se, (unsigned long long)ub.x.m,
                   y.x.se, (unsigned long long)y.x.m, x.x.se, (unsigned long long)x.x.m, fpsr, fx);
        }
        checks++;
        if (!ok) failures++;
    }
    printf("random arithmetic against the x87: %d operations, %d different\n", rounds, bad);
}

/* ------------------------------------------------------------------ the issue's example and memory forms */
static void put_x(uint32_t a, uint16_t se, uint64_t m)
{
    psmac_fpu_wr16(a, se); psmac_fpu_wr16(a + 2, 0);
    psmac_fpu_wr32(a + 4, (uint32_t)(m >> 32)); psmac_fpu_wr32(a + 8, (uint32_t)m);
}

static void test_basics(void)
{
    psmac_fpu_reset();
    /* FMOVE.X (A0),FP0 with 1 + 2^-60; FSUB.X (A1),FP0 with 1: FP0 = 2^-60 exactly (doubles give 0) */
    put_x(DATA, 0x3fff, UINT64_C(0x8000000000000008));   /* bit 3: 2^-60 */
    put_x(DATA + 16, 0x3fff, UINT64_C(0x8000000000000000));
    setA(0, DATA); setA(1, DATA + 16);
    run2(0xf210, 0x4800);                         /* fmove.x (a0),fp0 */
    run2(0xf211, 0x4828);                         /* fsub.x (a1),fp0 */
    CHECK(psmac_fpu.fp[0].se == 0x3fc3 && psmac_fpu.fp[0].m == UINT64_C(0x8000000000000000),
          "(1+2^-60)-1 = %04x:%016llx", psmac_fpu.fp[0].se, (unsigned long long)psmac_fpu.fp[0].m);
    CHECK(!(psmac_fpu.fpsr & 0x0f000000), "cc of 2^-60: %08x", psmac_fpu.fpsr);

    /* FMOVE.L #-7,FP3 ; FMOVE.B FP3,D1 ; FMOVE.W FP3,(A0)+ */
    { uint16_t w[4] = { 0xf23c, 0x4180, 0xffff, 0xfff9 }; run(w, 4, 0); }
    CHECK(same(get_fp(3), -7.0L), "fmove.l #-7");
    setD(1, 0x12345678);
    run2(0xf201, 0x7980);                         /* fmove.b fp3,d1 */
    CHECK(getD(1) == 0x123456f9, "fmove.b fp3,d1 = %08x", getD(1));
    setA(0, DATA);
    run2(0xf218, 0x7180);                         /* fmove.w fp3,(a0)+ */
    CHECK(psmac_fpu_rd16(DATA) == 0xfff9 && getA(0) == DATA + 2, "fmove.w (a0)+");

    /* FMOVE.S / .D in and out */
    psmac_fpu_wr32(DATA, 0x3fc00000);             /* 1.5f */
    setA(0, DATA);
    run2(0xf210, 0x4400);                         /* fmove.s (a0),fp0 */
    CHECK(same(get_fp(0), 1.5L), "fmove.s in");
    psmac_fpu_wr32(DATA, 0x400921fb); psmac_fpu_wr32(DATA + 4, 0x54442d18);
    run2(0xf210, 0x5480);                         /* fmove.d (a0),fp1 */
    CHECK(same(get_fp(1), (long double)3.141592653589793), "fmove.d in");
    put_fp(2, 1.0L / 3.0L);
    run2(0xf210, 0x7500);                         /* fmove.d fp2,(a0) */
    CHECK(psmac_fpu_rd32(DATA) == 0x3fd55555 && psmac_fpu_rd32(DATA + 4) == 0x55555555, "fmove.d out");
    CHECK(psmac_fpu.fpsr & 0x0200, "inexact on fmove.d out");

    /* the denormal convention of the memory format: exponent 0 with the integer bit is 2^-16383 */
    put_x(DATA, 0x0000, UINT64_C(0x8000000000000000));
    run2(0xf210, 0x4800);                         /* fmove.x (a0),fp0 */
    CHECK(same(get_fp(0), ldexpl(1.0L, -16383)), "68881 exponent 0 = 2^-16383");
    put_x(DATA + 16, 0x3fff, UINT64_C(0xc000000000000000));
    setA(1, DATA + 16);
    run2(0xf211, 0x4823);                         /* fmul.x (a1),fp0: 1.5 * 2^-16383 */
    run2(0xf211, 0x6800);                         /* fmove.x fp0,(a1) */
    CHECK(psmac_fpu_rd16(DATA + 16) == 0x0000 && psmac_fpu_rd32(DATA + 20) == 0xc0000000 && psmac_fpu_rd32(DATA + 24) == 0,
          "1.5 * 2^-16383 stored as exponent 0, 0xc0000000: %04x %08x", psmac_fpu_rd16(DATA + 16), psmac_fpu_rd32(DATA + 20));
    put_x(DATA, 0x3fff, UINT64_C(0x4000000000000000));   /* an unnormal: 0.5 */
    run2(0xf210, 0x4800);
    CHECK(same(get_fp(0), 0.5L), "unnormal normalized");

    /* NaNs: the destination's wins; an SNaN is quieted and flagged */
    put_fp(1, ld_of(0x7fff, UINT64_C(0xc000000000001111)));
    put_fp(2, ld_of(0x7fff, UINT64_C(0xc000000000002222)));
    psmac_fpu.fpsr = 0;
    run2(0xf200, 0x08a2);                         /* fadd fp2,fp1 */
    CHECK(psmac_fpu.fp[1].m == UINT64_C(0xc000000000001111), "dst NaN propagated");
    CHECK((psmac_fpu.fpsr & 0x0f000000) == 0x01000000, "NAN cc %08x", psmac_fpu.fpsr);
    put_fp(1, 1.0L);
    put_fp(2, ld_of(0x7fff, UINT64_C(0x8000000000002222)));   /* SNaN */
    psmac_fpu.fpsr = 0;
    run2(0xf200, 0x08a2);
    CHECK(psmac_fpu.fp[1].m == UINT64_C(0xc000000000002222), "SNaN quieted");
    CHECK((psmac_fpu.fpsr & 0xff00) == 0x4000 && (psmac_fpu.fpsr & 0x80), "SNAN and IOP: %08x", psmac_fpu.fpsr);
    put_fp(1, INFINITY); put_fp(2, INFINITY);
    psmac_fpu.fpsr = 0;
    run2(0xf200, 0x08a8);                         /* fsub fp2,fp1: inf - inf */
    CHECK(psmac_fpu.fp[1].se == 0x7fff && psmac_fpu.fp[1].m == UINT64_C(0xffffffffffffffff), "default NaN");
    CHECK((psmac_fpu.fpsr & 0xff00) == 0x2000, "OPERR %08x", psmac_fpu.fpsr);
    put_fp(1, 1.0L); put_fp(2, 0.0L);
    psmac_fpu.fpsr = 0;
    run2(0xf200, 0x08a0);                         /* fdiv fp2,fp1 */
    CHECK(isinf(get_fp(1)) && (psmac_fpu.fpsr & 0xff00) == 0x0400 && (psmac_fpu.fpsr & 0x10), "DZ %08x", psmac_fpu.fpsr);

    /* overflow and underflow, and the accrued byte */
    put_fp(1, ldexpl(1.0L, 16000)); put_fp(2, ldexpl(1.0L, 16000));
    psmac_fpu.fpsr = 0;
    run2(0xf200, 0x08a3);                         /* fmul: overflow */
    CHECK(isinf(get_fp(1)) && (psmac_fpu.fpsr & 0xff00) == 0x1200 && (psmac_fpu.fpsr & 0xff) == 0x48, "OVFL %08x", psmac_fpu.fpsr);
    put_fp(1, ldexpl(1.0L, -16000)); put_fp(2, ldexpl(3.0L, 400));
    psmac_fpu.fpsr = 0;
    run2(0xf200, 0x08a0);                         /* fdiv: underflow, inexact */
    CHECK((psmac_fpu.fpsr & 0x0a00) == 0x0a00 && (psmac_fpu.fpsr & 0x28) == 0x28, "UNFL %08x", psmac_fpu.fpsr);

    /* FMOVECR */
    psmac_fpu.fpcr = 0; psmac_fpu.fpsr = 0;
    run2(0xf200, 0x5c00);                         /* fmovecr #0,fp0 */
    CHECK(psmac_fpu.fp[0].se == 0x4000 && psmac_fpu.fp[0].m == UINT64_C(0xc90fdaa22168c235) && (psmac_fpu.fpsr & 0x0200), "pi");
    psmac_fpu.fpcr = 0x10;                        /* toward zero */
    run2(0xf200, 0x5c00);
    CHECK(psmac_fpu.fp[0].m == UINT64_C(0xc90fdaa22168c234), "pi toward zero");
    psmac_fpu.fpcr = 0x40;                        /* single precision */
    run2(0xf200, 0x5c00);
    CHECK(psmac_fpu.fp[0].m == UINT64_C(0xc90fdb0000000000), "pi in single precision %016llx", (unsigned long long)psmac_fpu.fp[0].m);
    psmac_fpu.fpcr = 0; psmac_fpu.fpsr = 0;
    run2(0xf200, 0x5c32);                         /* fmovecr #$32,fp0: 1.0 */
    CHECK(same(get_fp(0), 1.0L) && !(psmac_fpu.fpsr & 0x0200), "1.0 exact");
    run2(0xf200, 0x5c3f);
    CHECK(psmac_fpu.fp[0].se == 0x7525, "10^4096 exponent %04x", psmac_fpu.fp[0].se);
    run2(0xf200, 0x5c0f);
    CHECK(same(get_fp(0), 0.0L) && (psmac_fpu.fpsr & 0x04000000), "0.0");

    /* rounding precision: 1/3 in single precision has a 24-bit significand */
    psmac_fpu.fpcr = 0x40;
    put_fp(1, 1.0L); put_fp(2, 3.0L);
    run2(0xf200, 0x08a0);
    CHECK(psmac_fpu.fp[1].m == UINT64_C(0xaaaaab0000000000), "1/3 single %016llx", (unsigned long long)psmac_fpu.fp[1].m);
    psmac_fpu.fpcr = 0;                           /* the 68040's FSDIV rounds to single whatever the FPCR says */
    put_fp(1, 1.0L);
    run2(0xf200, 0x08e0);                         /* fsdiv fp2,fp1 */
    CHECK(psmac_fpu.fp[1].m == UINT64_C(0xaaaaab0000000000), "fsdiv");
    put_fp(1, 1.0L);
    run2(0xf200, 0x08e4);                         /* fddiv fp2,fp1 */
    CHECK(psmac_fpu.fp[1].m == UINT64_C(0xaaaaaaaaaaaaa800), "fddiv %016llx", (unsigned long long)psmac_fpu.fp[1].m);
}

/* ------------------------------------------------------------------ FMOVEM, control registers, FSAVE */
static void test_movem(void)
{
    psmac_fpu_reset();
    for (int i = 0; i < 8; i++)
        put_fp(i, (long double)(i + 1));
    setA(7, DATA + 0x200);
    run2(0xf227, 0xe0ff);                         /* fmovem.x fp0-fp7,-(sp) */
    CHECK(getA(7) == DATA + 0x200 - 96, "predecrement by 96");
    CHECK(same(ld_of(psmac_fpu_rd16(getA(7)), (uint64_t)psmac_fpu_rd32(getA(7) + 4) << 32 | psmac_fpu_rd32(getA(7) + 8)), 1.0L),
          "FP0 at the lowest address");
    setA(7, DATA + 0x200);
    run2(0xf227, 0xe080);                         /* fmovem.x fp7,-(sp): list bit 7 is FP7 here */
    CHECK(same(ld_of(psmac_fpu_rd16(getA(7)), (uint64_t)psmac_fpu_rd32(getA(7) + 4) << 32 | psmac_fpu_rd32(getA(7) + 8)), 8.0L),
          "predecrement bit 7 = FP7");
    for (int i = 0; i < 8; i++)
        put_fp(i, 0.0L);
    setA(7, DATA + 0x200 - 96);
    run2(0xf21f, 0xd0ff);                         /* fmovem.x (sp)+,fp0-fp7 */
    CHECK(getA(7) == DATA + 0x200, "postincrement");
    int ok = 1;
    for (int i = 0; i < 8; i++)
        if (!same(get_fp(i), (long double)(i + 1))) ok = 0;
    CHECK(ok, "fmovem round trip");
    setA(0, DATA);
    run2(0xf210, 0xf080);                         /* fmovem.x fp0,(a0): control, bit 7 = FP0 */
    CHECK(same(ld_of(psmac_fpu_rd16(DATA), (uint64_t)psmac_fpu_rd32(DATA + 4) << 32 | psmac_fpu_rd32(DATA + 8)), 1.0L), "control bit 7 = FP0");
    setD(2, 0x0c);                                /* dynamic list: FP4 and FP5 in control order */
    run2(0xf210, 0xf820);                         /* fmovem.x d2,(a0) */
    CHECK(same(ld_of(psmac_fpu_rd16(DATA), (uint64_t)psmac_fpu_rd32(DATA + 4) << 32 | psmac_fpu_rd32(DATA + 8)), 5.0L), "dynamic list");

    /* control registers */
    psmac_fpu.fpcr = 0x3450; psmac_fpu.fpsr = 0x0800aa08; psmac_fpu.fpiar = 0x12345678;
    setA(7, DATA + 0x100);
    run2(0xf227, 0xbc00);                         /* fmovem.l fpcr/fpsr/fpiar,-(sp) */
    CHECK(getA(7) == DATA + 0x100 - 12 && psmac_fpu_rd32(getA(7)) == 0x3450 && psmac_fpu_rd32(getA(7) + 4) == 0x0800aa08 &&
          psmac_fpu_rd32(getA(7) + 8) == 0x12345678, "fmovem.l control,-(sp)");
    psmac_fpu.fpcr = psmac_fpu.fpsr = psmac_fpu.fpiar = 0;
    run2(0xf21f, 0x9c00);                         /* fmovem.l (sp)+,fpcr/fpsr/fpiar */
    CHECK(psmac_fpu.fpcr == 0x3450 && psmac_fpu.fpsr == 0x0800aa08 && psmac_fpu.fpiar == 0x12345678 && getA(7) == DATA + 0x100, "back");
    setD(0, 0xffffffff);
    run2(0xf200, 0x9000);                         /* fmove.l d0,fpcr: masked */
    CHECK(psmac_fpu.fpcr == 0xfff0, "fpcr mask %08x", psmac_fpu.fpcr);
    run2(0xf201, 0xa800);                         /* fmove.l fpsr,d1 */
    CHECK(getD(1) == 0x0800aa08, "fmove.l fpsr,d1");
    { uint16_t w[4] = { 0xf23c, 0x9000, 0x0000, 0x0020 }; run(w, 4, 0); }   /* fmove.l #$20,fpcr */
    CHECK(psmac_fpu.fpcr == 0x20, "fmove.l #imm,fpcr");

    /* FSAVE and FRESTORE: null after reset, the 68882's idle frame after an FPU instruction */
    psmac_fpu_reset();
    setA(7, DATA + 0x300);
    psmac_fpu_wr16(CODE, 0xf327);
    regs[PSMAC_FPU_X_PC] = CODE;
    psmac_fpu_exec(regs, SUPER);                  /* fsave -(sp) */
    CHECK(getA(7) == DATA + 0x300 - 4 && psmac_fpu_rd32(getA(7)) == 0, "null frame");
    run2(0xf280, 0x0000);                         /* fnop */
    setA(7, DATA + 0x300);
    regs[PSMAC_FPU_X_PC] = CODE;
    psmac_fpu_wr16(CODE, 0xf327);
    psmac_fpu_exec(regs, SUPER);
    CHECK(getA(7) == DATA + 0x300 - 60 && psmac_fpu_rd16(getA(7)) == 0x1f38, "idle frame $1F38: %04x", psmac_fpu_rd16(getA(7)));
    psmac_fpu.fpcr = 0x30;
    psmac_fpu_wr16(CODE, 0xf35f);                 /* frestore (sp)+ */
    regs[PSMAC_FPU_X_PC] = CODE;
    psmac_fpu_exec(regs, SUPER);
    CHECK(getA(7) == DATA + 0x300 && psmac_fpu.fpcr == 0x30, "frestore idle frame steps over it, keeps state");
    psmac_fpu_wr32(DATA + 0x400, 0);
    setA(0, DATA + 0x400);
    psmac_fpu_wr16(CODE, 0xf350);                 /* frestore (a0): null frame resets */
    regs[PSMAC_FPU_X_PC] = CODE;
    psmac_fpu_exec(regs, SUPER);
    CHECK(psmac_fpu.fpcr == 0 && psmac_fpu.null && psmac_fpu.fp[0].se == 0x7fff, "frestore null resets");
    regs[PSMAC_FPU_X_PC] = CODE;
    CHECK(psmac_fpu_exec(regs, 0) == 8, "fsave in user mode: privilege violation");
}

/* ------------------------------------------------------------------ conditions and branches */
static void test_conditions(void)
{
    psmac_fpu_reset();
    uint32_t pc;
    put_fp(0, 1.0L); put_fp(1, 2.0L);
    run2(0xf200, 0x0038);                         /* fcmp fp0,fp0 */
    CHECK(psmac_fpu.fpsr & 0x04000000, "fcmp equal sets Z");
    run2(0xf200, 0x0438);                         /* fcmp fp1,fp0: 1 < 2 */
    CHECK((psmac_fpu.fpsr & 0x0f000000) == 0x08000000, "fcmp less sets N");
    uint16_t blt[2] = { 0xf284, 0x0010 };          /* fbolt.w *+$12 */
    run(blt, 2, &pc);
    CHECK(pc == CODE + 2 + 0x10, "fblt taken: %08x", pc);
    uint16_t bgt[2] = { 0xf292, 0x0010 };
    run(bgt, 2, &pc);
    CHECK(pc == CODE + 4, "fbgt not taken");
    put_fp(2, NAN);
    run2(0xf200, 0x0838);                         /* fcmp fp2,fp0: unordered */
    CHECK((psmac_fpu.fpsr & 0x0f000000) == 0x01000000, "unordered");
    uint16_t bngl[2] = { 0xf289, 0x0010 };         /* fbueq: true on NaN, no BSUN */
    run(bngl, 2, &pc);
    CHECK(pc == CODE + 0x12, "fbueq taken on NaN");
    psmac_fpu.fpsr &= ~0xff00u;
    uint16_t bgts[2] = { 0xf292, 0x0010 };         /* fbgt (signalling): BSUN */
    run(bgts, 2, &pc);
    CHECK(psmac_fpu.fpsr & 0x8000, "BSUN on a signalling predicate");
    psmac_fpu.fpcr = 0x8000;
    CHECK(run(bgts, 2, &pc) == 48, "BSUN enabled takes vector 48");
    psmac_fpu.fpcr = 0;
    /* fdbne d3: counts while not equal */
    run2(0xf200, 0x0438);                         /* not equal */
    setD(3, 0x00010002);
    uint16_t dbf[3] = { 0xf24b, 0x0001, 0xfffc };  /* fdbeq d3,*-2: condition false (not equal) -> count */
    run(dbf, 3, &pc);
    CHECK(getD(3) == 0x00010001 && pc == CODE + 4 - 4, "fdbeq loops: d3 %08x pc %08x", getD(3), pc);
    setD(3, 0x00010000);
    run(dbf, 3, &pc);
    CHECK(getD(3) == 0x0001ffff && pc == CODE + 6, "fdbeq falls through at -1");
    setD(4, 0);
    run2(0xf244, 0x000e);                         /* fsne d4 */
    CHECK((getD(4) & 0xff) == 0xff, "fsne");
    uint16_t trap[2] = { 0xf27c, 0x000f };         /* ftrapt */
    CHECK(run(trap, 2, &pc) == (0x2000 | 7) && pc == CODE + 4, "ftrapt: format 2 vector 7");
    uint16_t trapf[2] = { 0xf27c, 0x0000 };
    CHECK(run(trapf, 2, &pc) == 0 && pc == CODE + 4, "ftrapf");
    CHECK(run2(0xf200, 0x0005) == 11, "an undefined opmode is an F-line exception");
}

/* ------------------------------------------------------------------ conversions */
static void test_conversions(void)
{
    psmac_fpu_reset();
    static const long double vals[] = { 2.5L, -2.5L, 3.5L, 1e10L, -1e10L, 70000.7L, -129.5L, 0.49999L, 2147483647.5L, -2147483648.5L };
    for (unsigned i = 0; i < sizeof vals / sizeof vals[0]; i++) {
        for (int mode = 0; mode < 4; mode++) {
            put_fp(0, vals[i]);
            psmac_fpu.fpcr = (uint32_t)mode << 4;
            psmac_fpu.fpsr = 0;
            run2(0xf200, 0x6000);                 /* fmove.l fp0,d0 */
            volatile long double vv = vals[i];
            fesetround(fe_mode[mode]);
            long double r = rintl(vv);
            fesetround(FE_TONEAREST);
            int32_t want = r > 2147483647.0L ? INT32_MAX : r < -2147483648.0L ? INT32_MIN : (int32_t)r;
            CHECK((int32_t)getD(0) == want, "fmove.l %Lg mode %d: %d, want %d", vals[i], mode, (int32_t)getD(0), want);
            int ovf = r > 2147483647.0L || r < -2147483648.0L;
            CHECK(!!(psmac_fpu.fpsr & 0x2000) == ovf, "OPERR on overflow %Lg", vals[i]);
            setD(0, 0);
            run2(0xf200, 0x7000);                 /* fmove.w fp0,d0 */
            int32_t ww = r > 32767 ? 32767 : r < -32768 ? -32768 : (int32_t)r;
            CHECK((int16_t)getD(0) == ww, "fmove.w %Lg mode %d", vals[i], mode);
            /* single */
            run2(0xf200, 0x6400);
            fesetround(fe_mode[mode]);
            volatile float f = (float)vv;
            fesetround(FE_TONEAREST);
            uint32_t fb; memcpy(&fb, (const void *)&f, 4);
            CHECK(getD(0) == fb, "fmove.s %Lg mode %d", vals[i], mode);
        }
    }
    psmac_fpu.fpcr = 0;
    put_fp(0, NAN);
    run2(0xf200, 0x6000);
    CHECK(psmac_fpu.fpsr & 0x2000, "fmove.l of a NaN: OPERR");
}

/* ------------------------------------------------------------------ packed decimal */
static void test_packed(void)
{
    psmac_fpu_reset();
    setA(0, DATA);
    /* in: 1.2345678901234567E+3 */
    psmac_fpu_wr32(DATA, 0x00030001); psmac_fpu_wr32(DATA + 4, 0x23456789); psmac_fpu_wr32(DATA + 8, 0x01234567);
    run2(0xf210, 0x4c00);                         /* fmove.p (a0),fp0 */
    CHECK(same(get_fp(0), 1234.5678901234567L), "packed in %.20Lg", get_fp(0));
    /* in: -5E-1 */
    psmac_fpu_wr32(DATA, 0xc0010005); psmac_fpu_wr32(DATA + 4, 0); psmac_fpu_wr32(DATA + 8, 0);
    run2(0xf210, 0x4c00);
    CHECK(same(get_fp(0), -0.5L), "packed -5E-1");
    /* out with k = 5: 1234.5678.. -> 1.2346E+3 */
    put_fp(1, 1234.5678901234567L);
    run2(0xf210, 0x6c85);                         /* fmove.p fp1,(a0){#5} */
    CHECK(psmac_fpu_rd32(DATA) == 0x00030001 && psmac_fpu_rd32(DATA + 4) == 0x23460000 && psmac_fpu_rd32(DATA + 8) == 0,
          "packed out k=5: %08x %08x %08x", psmac_fpu_rd32(DATA), psmac_fpu_rd32(DATA + 4), psmac_fpu_rd32(DATA + 8));
    /* k = -2: two digits after the point: 1234.57 */
    run2(0xf210, 0x6cfe);                         /* {#-2} */
    CHECK(psmac_fpu_rd32(DATA) == 0x00030001 && psmac_fpu_rd32(DATA + 4) == 0x23457000, "packed out k=-2: %08x %08x",
          psmac_fpu_rd32(DATA), psmac_fpu_rd32(DATA + 4));
    /* 1/3 with 17 digits, and back: within the 68881's accuracy */
    put_fp(1, 1.0L / 3.0L);
    run2(0xf210, 0x6c91);                         /* {#17} */
    CHECK(psmac_fpu_rd32(DATA) == 0x40010003 && psmac_fpu_rd32(DATA + 4) == 0x33333333 && psmac_fpu_rd32(DATA + 8) == 0x33333333,
          "1/3 packed: %08x %08x %08x", psmac_fpu_rd32(DATA), psmac_fpu_rd32(DATA + 4), psmac_fpu_rd32(DATA + 8));
    run2(0xf210, 0x6c92);                         /* {#18}: OPERR, 17 digits */
    CHECK(psmac_fpu.fpsr & 0x2000, "k-factor 18: OPERR");
    /* random values: 17 digits out and in again come back within an ulp */
    int far = 0;
    for (int i = 0; i < 2000; i++) {
        long double v = ldexpl((long double)(rnd64() | 1) / 18446744073709551616.0L, rand() % 6000 - 3000);
        put_fp(1, v);
        run2(0xf210, 0x6c91);
        run2(0xf210, 0x4c00);
        long double back = get_fp(0);
        if (fabsl(back - v) > fabsl(v) * 5.1e-17L) {   /* half a unit of the 17th digit */
            if (far++ < 5) {
                LD u; u.ld = v;
                printf("  packed: %04x:%016llx -> %08x %08x %08x -> %.20Lg (from %.20Lg)\n", u.x.se, (unsigned long long)u.x.m,
                       psmac_fpu_rd32(DATA), psmac_fpu_rd32(DATA + 4), psmac_fpu_rd32(DATA + 8), back, v);
            }
        }
    }
    CHECK(far == 0, "packed round trips off by more than the 17 digits allow: %d", far);
}

/* ------------------------------------------------------------------ the elementary functions */
static long double ulp_err(long double got, long double want)
{
    if (isnan(got) || isnan(want)) return isnan(got) && isnan(want) ? 0 : 1e9L;
    if (got == want) return 0;
    if (isinf(got) || isinf(want)) return 1e9L;
    int e;
    frexpl(want, &e);
    long double ulp = ldexpl(1.0L, (e - 64 < -16445) ? -16445 : e - 64);
    return fabsl(got - want) / ulp;
}

static void test_elementary(int rounds)
{
    static const struct { uint8_t opm; const char *name; long double (*f)(long double); long double lo, hi; } fns[] = {
        { 0x0e, "FSIN", sinl, -10, 10 }, { 0x1d, "FCOS", cosl, -10, 10 }, { 0x0f, "FTAN", tanl, -3, 3 },
        { 0x0a, "FATAN", atanl, -50, 50 }, { 0x0c, "FASIN", asinl, -1, 1 }, { 0x1c, "FACOS", acosl, -1, 1 },
        { 0x10, "FETOX", expl, -100, 100 }, { 0x08, "FETOXM1", expm1l, -5, 5 }, { 0x11, "FTWOTOX", exp2l, -100, 100 },
        { 0x12, "FTENTOX", exp10l, -30, 30 }, { 0x14, "FLOGN", logl, 0, 1000 }, { 0x15, "FLOG10", log10l, 0, 1000 },
        { 0x16, "FLOG2", log2l, 0, 1000 }, { 0x06, "FLOGNP1", log1pl, -0.9L, 10 }, { 0x02, "FSINH", sinhl, -20, 20 },
        { 0x19, "FCOSH", coshl, -20, 20 }, { 0x09, "FTANH", tanhl, -5, 5 }, { 0x0d, "FATANH", atanhl, -0.99L, 0.99L },
    };
    psmac_fpu_reset();
    for (unsigned f = 0; f < sizeof fns / sizeof fns[0]; f++) {
        long double worst = 0, wx = 0;
        for (int i = 0; i < rounds; i++) {
            long double u = (long double)rnd64() / 18446744073709551616.0L, x;
            if (i % 4 == 0) x = fns[f].lo + (fns[f].hi - fns[f].lo) * u * u * u * u;   /* more near the low end */
            else x = fns[f].lo + (fns[f].hi - fns[f].lo) * u;
            if (i % 50 == 0) x = ldexpl(u, -(rand() % 70));                             /* small arguments */
            if (x < fns[f].lo || x > fns[f].hi) continue;
            put_fp(1, x);
            psmac_fpu.fpcr = 0;
            run2(0xf200, (uint16_t)(1 << 10 | 0 << 7 | fns[f].opm));
            long double got = get_fp(0), want = fns[f].f(x);
            if (dump) {
                LD ux, ug; ux.ld = x; ug.ld = got;
                fprintf(dump, "%s %04x %016llx %04x %016llx\n", fns[f].name, ux.x.se, (unsigned long long)ux.x.m, ug.x.se, (unsigned long long)ug.x.m);
            }
            long double err = ulp_err(got, want);
            if (err > worst) { worst = err; wx = x; }
        }
        /* the C library's long double functions are themselves off by up to 3 ulps here (glibc's own
           tables say as much); tools/fpu_refcheck.py checks these results against 80-digit arithmetic */
        CHECK(worst <= 3.0L, "%s: %.2Lf ulps at %.21Lg", fns[f].name, worst, wx);
        if (verbose || worst > 1.0L) {
            LD u, g; u.ld = wx; put_fp(1, wx); run2(0xf200, (uint16_t)(1 << 10 | fns[f].opm)); g.ld = get_fp(0);
            printf("  %-8s worst %.2Lf ulp against the C library: x %04x:%016llx, here %04x:%016llx\n", fns[f].name, worst,
                   u.x.se, (unsigned long long)u.x.m, g.x.se, (unsigned long long)g.x.m);
        }
    }
    /* large arguments of the trigonometric functions: reduction by Payne and Hanek */
    long double big[] = { 1e22L, 1e300L, ldexpl(1.0L, 16000), 1.0e4000L, 9223372036854775807.0L };
    for (unsigned i = 0; i < sizeof big / sizeof big[0]; i++) {
        put_fp(1, big[i]);
        run2(0xf200, 0x040e);
        CHECK(ulp_err(get_fp(0), sinl(big[i])) <= 2.0L, "fsin(%Lg) = %.21Lg, C %.21Lg", big[i], get_fp(0), sinl(big[i]));
    }
    /* exact cases: no inexact flag */
    struct { uint8_t opm; long double x, r; } ex[] = {
        { 0x0e, 0.0L, 0.0L }, { 0x1d, 0.0L, 1.0L }, { 0x10, 0.0L, 1.0L }, { 0x11, 10.0L, 1024.0L }, { 0x11, -3.0L, 0.125L },
        { 0x12, 3.0L, 1000.0L }, { 0x16, 1024.0L, 10.0L }, { 0x15, 1e6L, 6.0L }, { 0x14, 1.0L, 0.0L }, { 0x09, INFINITY, 1.0L },
    };
    for (unsigned i = 0; i < sizeof ex / sizeof ex[0]; i++) {
        put_fp(1, ex[i].x);
        psmac_fpu.fpsr = 0;
        run2(0xf200, (uint16_t)(1 << 10 | ex[i].opm));
        CHECK(same(get_fp(0), ex[i].r) && !(psmac_fpu.fpsr & 0x0200), "exact op %02x(%Lg) = %Lg fpsr %08x", ex[i].opm, ex[i].x, get_fp(0), psmac_fpu.fpsr);
    }
    /* domain errors and poles */
    put_fp(1, -1.0L); psmac_fpu.fpsr = 0; run2(0xf200, 0x0414);
    CHECK(isnan(get_fp(0)) && (psmac_fpu.fpsr & 0x2000), "flogn(-1): OPERR");
    put_fp(1, 0.0L); psmac_fpu.fpsr = 0; run2(0xf200, 0x0414);
    CHECK(isinf(get_fp(0)) && get_fp(0) < 0 && (psmac_fpu.fpsr & 0x0400), "flogn(0): DZ, -inf");
    put_fp(1, 2.0L); psmac_fpu.fpsr = 0; run2(0xf200, 0x040c);
    CHECK(isnan(get_fp(0)) && (psmac_fpu.fpsr & 0x2000), "fasin(2): OPERR");
    put_fp(1, 20000.0L); psmac_fpu.fpsr = 0; run2(0xf200, 0x0410);
    CHECK(isinf(get_fp(0)) && (psmac_fpu.fpsr & 0x1000), "fetox(20000): OVFL");
    put_fp(1, -20000.0L); psmac_fpu.fpsr = 0; run2(0xf200, 0x0410);
    CHECK(get_fp(0) == 0 && (psmac_fpu.fpsr & 0x0800), "fetox(-20000): UNFL");
    /* FSINCOS */
    put_fp(1, 0.5L);
    run2(0xf200, 0x05b2);                         /* fsincos fp1,fp2:fp3 */
    CHECK(ulp_err(get_fp(3), sinl(0.5L)) <= 1 && ulp_err(get_fp(2), cosl(0.5L)) <= 1, "fsincos");
    /* FGETEXP, FGETMAN, FSCALE */
    put_fp(1, 12.0L);
    run2(0xf200, 0x041e); CHECK(same(get_fp(0), 3.0L), "fgetexp(12) = 3");
    run2(0xf200, 0x041f); CHECK(same(get_fp(0), 1.5L), "fgetman(12) = 1.5");
    put_fp(0, 3.0L); put_fp(1, -2.7L);
    run2(0xf200, 0x0426); CHECK(same(get_fp(0), 0.75L), "fscale(3, -2.7) = 0.75");
    /* FREM's quotient byte */
    put_fp(0, 7.0L); put_fp(1, 2.0L);
    run2(0xf200, 0x0425);                         /* frem fp1,fp0: 7 rem 2 = -1, quotient 4 */
    CHECK(same(get_fp(0), -1.0L) && ((psmac_fpu.fpsr >> 16) & 0xff) == 4, "frem 7,2 = %Lg q %02x", get_fp(0), (psmac_fpu.fpsr >> 16) & 0xff);
    put_fp(0, -7.0L); put_fp(1, 2.0L);
    run2(0xf200, 0x0421);                         /* fmod: -7 mod 2 = -1, quotient -3 */
    CHECK(same(get_fp(0), -1.0L) && ((psmac_fpu.fpsr >> 16) & 0xff) == 0x83, "fmod -7,2 q %02x", (psmac_fpu.fpsr >> 16) & 0xff);
}

int main(int argc, char **argv)
{
    int rounds = 20000;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump = fopen(argv[++i], "w");
        else rounds = atoi(argv[i]);
    }
    srand(68881);
    test_basics();
    test_movem();
    test_conditions();
    test_conversions();
    test_packed();
    test_random_arith(rounds);
    test_elementary(rounds / 10 > 200 ? rounds / 10 : 200);
    printf("fpu_test: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
