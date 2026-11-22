/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_fpu_math.c - the exact FPU's elementary functions (FSIN to FTENTOX), FMOVECR's constants and the
 * packed decimal format (issue #27, docs/17-exact-fpu.md).
 *
 * Backstory (22 November 2026). SoftFloat has the four operations, the square root, the remainder and the
 * conversions; the 68881 also has twenty-odd transcendental functions in its microcode, the "elementary"
 * ones of Motorola's manual, and a decimal format. Neither SoftFloat nor the kernel has them in the
 * extended format (the kernel's libm, src/math, is in doubles and uses the FPU registers, which belong to
 * the 68k on this core), so they are here, in integer code only.
 *
 * The method: each function is computed in SoftFloat's 128-bit binary format (113 bits of significand,
 * 49 more than the result has) by argument reduction and a short series, then rounded once to the FPCR's
 * precision and mode. The error before that rounding is a few units in the 110th bit, so the results are
 * the correctly rounded ones except when the exact value lies within about 2^-45 of a rounding boundary:
 * a 68881's own results are within an ulp or so ("accuracy of the transcendental functions", MC68881UM
 * section 4), so a program sees answers at least as good as the chip's, though not always the chip's own
 * last bit. Exact cases (sin 0, e^0, 2^n, log2 of a power of two, log10 of a power of ten up to 10^27)
 * are exact, with no inexact flag. Intermediate values are kept as a 128-bit significand and a separate
 * binary exponent where they could leave the extended range, so overflow and underflow are decided by the
 * final rounding alone, as on the chip.
 *
 * The reductions:
 *  - e^x, 2^x, 10^x, sinh, cosh, tanh, e^x - 1: x = n ln2 + r (Cody and Waite: ln 2 in two parts, the
 *    first short enough that n times it is exact), e^r - 1 by its series on r / 256, then eight steps of
 *    E <- E (E + 2), which square e^r while keeping e^r - 1's relative accuracy for small r.
 *  - ln, log2, log10, ln(1 + x): x = m 2^e with m in [1/sqrt 2, sqrt 2), ln m = 2 atanh((m-1)/(m+1)).
 *  - sin, cos, tan: Payne and Hanek with 2/pi to 16,700 bits (tools/mkfpuconsts.py), so that the
 *    largest extended number reduces correctly, then the Taylor series on |r| <= pi/4.
 *  - atan: three halvings of the angle (a / (1 + sqrt(1 + a^2))), then the series; asin, acos through it.
 *
 * Packed decimal (FMOVE.P): 17 digits and a 3-digit exponent, converted with the same 128-bit arithmetic
 * and powers of ten built from FMOVECR's table; FMOVE.P out honours the k-factor (positive: significant
 * digits; zero or negative: digits after the point), rounds in the FPCR's mode, and signals OPERR as the
 * 68881 does for k above 17 and for a decimal exponent of four digits.
 */
#include <stdbool.h>
#include <stdint.h>
#include "psmac_fpu_int.h"
#include "internals.h"
#include "psmac_fpu_consts.h"

typedef float128_t Q;

/* ------------------------------------------------------------------ 128-bit helpers */

static Q q_make(uint64_t hi, uint64_t lo) { union ui128_f128 u; u.ui.v64 = hi; u.ui.v0 = lo; return u.f; }
static uint64_t q_hi(Q a) { union ui128_f128 u; u.f = a; return u.ui.v64; }
static Q q_c(const uint64_t c[2]) { return q_make(c[0], c[1]); }
static Q qa(Q a, Q b) { return f128_add(a, b); }
static Q qs(Q a, Q b) { return f128_sub(a, b); }
static Q qm(Q a, Q b) { return f128_mul(a, b); }
static Q qd(Q a, Q b) { return f128_div(a, b); }
static Q qi(int32_t i) { return i32_to_f128(i); }
static Q qneg(Q a) { union ui128_f128 u; u.f = a; u.ui.v64 ^= UINT64_C(0x8000000000000000); return u.f; }
static Q qabs(Q a) { union ui128_f128 u; u.f = a; u.ui.v64 &= ~UINT64_C(0x8000000000000000); return u.f; }
static int32_t q_exp(Q a) { return (int32_t)((q_hi(a) >> 48) & 0x7fff); }
static bool q_zero(Q a) { union ui128_f128 u; u.f = a; return !(u.ui.v64 << 1) && !u.ui.v0; }

/* a times 2^n for a normal a whose result stays normal */
static Q q_scale(Q a, int32_t n)
{
    union ui128_f128 u;
    u.f = a;
    if (q_zero(a)) return a;
    u.ui.v64 += (uint64_t)(int64_t)n << 48;
    return u.f;
}

static const uint64_t C_PI[2] = PSMAC_Q_PI, C_PI_2[2] = PSMAC_Q_PI_2, C_PI_4[2] = PSMAC_Q_PI_4,
    C_LN2[2] = PSMAC_Q_LN2, C_LN2_HI[2] = PSMAC_Q_LN2_HI, C_LN2_LO[2] = PSMAC_Q_LN2_LO,
    C_LOG2E[2] = PSMAC_Q_LOG2E, C_LOG10E[2] = PSMAC_Q_LOG10E, C_LN10[2] = PSMAC_Q_LN10;

static Q ONE, TWO, HALF, SQRT2, RECIP[72];          /* RECIP[n] = 1/n */
static Q P10[13];                                    /* 10^(2^i), i = 0..12, with P10E their exponents */
static int32_t P10E[13];
static bool ready;

static void init(void)
{
    uint_fast8_t m = softfloat_roundingMode, f = softfloat_exceptionFlags;
    softfloat_roundingMode = softfloat_round_near_even;
    ONE = qi(1); TWO = qi(2); HALF = qd(ONE, TWO);
    SQRT2 = f128_sqrt(TWO);
    for (int i = 1; i < 72; i++)
        RECIP[i] = qd(ONE, qi(i));
    /* FMOVECR's 10^(2^i) (offsets 0x33 + i) as a significand in [1, 2) and an exponent */
    for (int i = 0; i < 13; i++) {
        const struct psmac_fpu_rom *r = 0;
        for (unsigned k = 0; k < sizeof psmac_fpu_rom / sizeof psmac_fpu_rom[0]; k++)
            if (psmac_fpu_rom[k].offset == 0x33 + i) r = &psmac_fpu_rom[k];
        /* the 128 bits sig.extra rounded to 113: sig's 63 bits after the leading one, extra's 49 */
        uint64_t hi = (r->sig << 1) >> 16, lo = (r->sig << 49) | (r->extra >> 15);
        bool up = (r->extra >> 14) & 1;               /* round to nearest (the sticky bits below decide ties) */
        if (up && (((r->extra & 0x3fff) != 0) || (lo & 1))) { if (!++lo) hi++; }
        P10[i] = q_make((UINT64_C(0x3fff) << 48) + hi, lo);
        P10E[i] = (int32_t)r->exp - 0x3fff;
    }
    softfloat_roundingMode = m;
    softfloat_exceptionFlags = f;
    ready = true;
}

/* a value with an exponent of its own: m (normal, any size) times 2^e */
struct qe { Q m; int32_t e; };

static struct qe qe_norm(Q m, int32_t e)
{
    struct qe r;
    if (q_zero(m)) { r.m = m; r.e = 0; return r; }
    int32_t x = q_exp(m) - 0x3fff;
    r.m = q_scale(m, -x);
    r.e = e + x;
    return r;
}

static struct qe qe_mul(struct qe a, struct qe b) { return qe_norm(qm(a.m, b.m), a.e + b.e); }
static struct qe qe_div(struct qe a, struct qe b) { return qe_norm(qd(a.m, b.m), a.e - b.e); }

/* 10^n, n >= 0 (exact up to 10^27 and beyond while 5^n fits 113 bits) */
static struct qe pow10_qe(int32_t n)
{
    struct qe r = { ONE, 0 };
    for (int i = 0; i < 13 && n; i++, n >>= 1)
        if (n & 1) {
            struct qe t = { P10[i], P10E[i] };
            r = qe_mul(r, t);
        }
    return r;
}

/* round m 2^scale to an extended value (prec bits) in the current rounding mode, with its flags */
static extFloat80_t q_round(Q a, int32_t scale, uint8_t prec)
{
    union ui128_f128 u;
    u.f = a;
    bool sign = u.ui.v64 >> 63;
    int32_t e = (int32_t)((u.ui.v64 >> 48) & 0x7fff);
    uint64_t fh = u.ui.v64 & UINT64_C(0x0000ffffffffffff), fl = u.ui.v0;
    extFloat80_t z;
    if (e == 0x7fff) {
        z.signExp = (uint16_t)((sign ? 0x8000 : 0) | 0x7fff);
        z.signif = (fh | fl) ? UINT64_C(0xffffffffffffffff) : UINT64_C(0x8000000000000000);
        return z;
    }
    unsigned __int128 s = ((unsigned __int128)fh << 64) | fl;
    if (e) s |= (unsigned __int128)1 << 112;
    else e = 1;
    if (!s) { z.signExp = sign ? 0x8000 : 0; z.signif = 0; return z; }
    int lz = (s >> 64) ? __builtin_clzll((uint64_t)(s >> 64)) : 64 + __builtin_clzll((uint64_t)s);
    s <<= lz;
    return softfloat_roundPackToExtF80(sign, e - (lz - 15) + scale, (uint64_t)(s >> 64), (uint64_t)s, prec);
}

static extFloat80_t x_make(uint16_t se, uint64_t m) { extFloat80_t x; x.signExp = se; x.signif = m; return x; }
static const uint64_t BIT63 = UINT64_C(0x8000000000000000);
static extFloat80_t x_one(bool neg) { return x_make((neg ? 0x8000 : 0) | 0x3fff, BIT63); }
static extFloat80_t x_zero(bool neg) { return x_make(neg ? 0x8000 : 0, 0); }
static extFloat80_t x_inf(bool neg) { return x_make((neg ? 0x8000 : 0) | 0x7fff, BIT63); }
static extFloat80_t x_nan(void) { softfloat_raiseFlags(softfloat_flag_invalid); return x_make(0x7fff, ~UINT64_C(0)); }

/* ------------------------------------------------------------------ exponentials */

/* e^r - 1 for |r| <= 0.35: the series on r / 256, then E <- E (E + 2) eight times */
static Q expm1_small(Q r)
{
    if (q_zero(r) || q_exp(r) < 0x3fff - 16000) return r;
    Q t = q_scale(r, -8), acc = ONE;
    for (int k = 11; k >= 2; k--)
        acc = qa(ONE, qm(qm(t, acc), RECIP[k]));
    Q E = qm(t, acc);
    for (int i = 0; i < 8; i++)
        E = qm(E, qa(E, TWO));
    return E;
}

/* e^x = 2^n (1 + E), |x| <= 20000 */
static Q exp_split(Q x, int32_t *n)
{
    *n = f128_to_i32(qm(x, q_c(C_LOG2E)), softfloat_round_near_even, false);
    Q nq = qi(*n);
    Q r = qs(qs(x, qm(nq, q_c(C_LN2_HI))), qm(nq, q_c(C_LN2_LO)));
    return expm1_small(r);
}

/* ------------------------------------------------------------------ logarithms */

/* 2 atanh(s) for |s| <= 0.1716 */
static Q atanh2(Q s)
{
    Q s2 = qm(s, s), acc = RECIP[49];
    for (int k = 23; k >= 0; k--)
        acc = qa(RECIP[2 * k + 1], qm(s2, acc));
    return qm(q_scale(s, 1), acc);
}

/* ln(m) + e ln 2 for m in [1, 2) (normal), as a 128-bit value */
static Q ln_me(Q m, int32_t e)
{
    if (f128_lt(SQRT2, m)) { m = q_scale(m, -1); e++; }
    Q s = qd(qs(m, ONE), qa(m, ONE));
    Q l = atanh2(s);
    if (!e) return l;
    Q eq = qi(e);
    return qa(qm(eq, q_c(C_LN2_HI)), qa(qm(eq, q_c(C_LN2_LO)), l));
}

/* the significand of a finite nonzero extended value in [1, 2) and its exponent */
static Q x_split(extFloat80_t x, int32_t *e)
{
    int32_t ex = x.signExp & 0x7fff;
    uint64_t s = x.signif;
    if (!ex) { struct exp32_sig64 n = softfloat_normSubnormalExtF80Sig(s); ex = 1 + n.exp; s = n.sig; }
    *e = ex - 0x3fff;
    extFloat80_t m = x_make(0x3fff, s);
    return extF80_to_f128(m);
}

/* ------------------------------------------------------------------ trigonometry */

static uint32_t tw(int32_t i) { return i >= 0 && i < (int32_t)(sizeof psmac_two_over_pi / 4) ? psmac_two_over_pi[i] : 0; }

/* bits of weight 2^-p to 2^-(p+63) of 2/pi (zero above the point) */
static uint64_t tbits(int32_t p)
{
    if (p < 1) {
        if (1 - p >= 64) return 0;
        return tbits(1) >> (1 - p);
    }
    int32_t wi = (p - 1) / 32, bo = (p - 1) % 32;
    unsigned __int128 t = ((unsigned __int128)tw(wi) << 64) | ((unsigned __int128)tw(wi + 1) << 32) | tw(wi + 2);
    return (uint64_t)(t >> (32 - bo));
}

/* |x| = (quadrant + f) pi/2 with |f| <= 1/2: returns the quadrant (mod 4), *r = f pi/2 */
static int reduce(extFloat80_t x, Q *r)
{
    Q ax = qabs(extF80_to_f128(x));               /* the 128-bit format holds every extended value */
    if (f128_le(ax, q_c(C_PI_4))) {
        *r = ax;
        return 0;
    }
    int32_t ex = x.signExp & 0x7fff;
    uint64_t mant = x.signif;
    if (!ex) { struct exp32_sig64 n = softfloat_normSubnormalExtF80Sig(mant); ex = 1 + n.exp; mant = n.sig; }
    int32_t t = ex - 0x3fff - 63, i0 = t - 1;
    uint64_t w[4];
    for (int k = 0; k < 4; k++)
        w[k] = tbits(i0 + 64 * k);
    /* p = mant * w, 320 bits, p[0] the most significant limb */
    uint64_t p[5] = { 0, 0, 0, 0, 0 };
    unsigned __int128 carry = 0;
    for (int k = 3; k >= 0; k--) {
        unsigned __int128 pr = (unsigned __int128)mant * w[k] + carry;
        p[k + 1] = (uint64_t)pr;
        carry = pr >> 64;
    }
    p[0] = (uint64_t)carry;
    /* y = p / 2^254: bits 255..254 the quadrant, below them the fraction */
    int quad = (int)((p[1] >> 62) & 3);
    uint64_t f0 = (p[1] << 2) | (p[2] >> 62), f1 = (p[2] << 2) | (p[3] >> 62), f2 = (p[3] << 2) | (p[4] >> 62);
    bool neg = false;
    if (f0 >> 63) {                               /* f >= 1/2: take f - 1 and the next quadrant */
        neg = true;
        quad = (quad + 1) & 3;
        f2 = ~f2 + 1;
        f1 = ~f1 + (f2 == 0);
        f0 = ~f0 + (f2 == 0 && f1 == 0);
    }
    /* the fraction (binary point above f0) to 128-bit floating point */
    int32_t sh = 0;
    while (!f0 && sh < 128) { f0 = f1; f1 = f2; f2 = 0; sh += 64; }
    if (!f0) { *r = qi(0); return quad; }
    int lz = __builtin_clzll(f0);
    if (lz) {
        f0 = (f0 << lz) | (f1 >> (64 - lz));
        f1 = (f1 << lz) | (f2 >> (64 - lz));
        f2 <<= lz;
    }
    /* f = f0.f1f2 * 2^-(sh + lz + 1) with f0's top bit the leading one */
    uint64_t hi = (f0 << 1) >> 16, lo = (f0 << 49) | (f1 >> 15);
    if ((f1 >> 14) & 1) { if (!++lo) hi++; }       /* nearest; the bits below are far past what matters */
    Q f = q_make(((uint64_t)(0x3fff - 1 - sh - lz) << 48) + hi, lo);   /* + : a carry out of hi is the exponent's */
    f = qm(f, q_c(C_PI_2));
    *r = neg ? qneg(f) : f;
    return quad;
}

static Q sin_q(Q r)
{
    Q r2 = qm(r, r), acc = ONE;
    for (int k = 15; k >= 1; k--)
        acc = qs(ONE, qm(qm(r2, acc), qm(RECIP[2 * k], RECIP[2 * k + 1])));
    return qm(r, acc);
}

static Q cos_q(Q r)
{
    Q r2 = qm(r, r), acc = ONE;
    for (int k = 15; k >= 1; k--)
        acc = qs(ONE, qm(qm(r2, acc), qm(RECIP[2 * k - 1], RECIP[2 * k])));
    return acc;
}

/* atan(a) for a >= 0 */
static Q atan_q(Q a)
{
    bool inv = f128_lt(ONE, a);
    if (inv) a = qd(ONE, a);
    for (int i = 0; i < 3; i++)
        a = qd(a, qa(ONE, f128_sqrt(qa(ONE, qm(a, a)))));
    /* acc = 1 - a2/3 + a2^2/5 - ... (to a2^17/35), by Horner */
    Q a2 = qm(a, a), acc = RECIP[35];
    for (int k = 16; k >= 0; k--)
        acc = qs(RECIP[2 * k + 1], qm(a2, acc));
    Q res = q_scale(qm(a, acc), 3);
    if (inv) res = qs(q_c(C_PI_2), res);
    return res;
}

/* ------------------------------------------------------------------ the functions */

static bool x_isint(extFloat80_t x, int32_t *n, int32_t limit)
{
    extFloat80_t r = extF80_roundToInt(x, softfloat_round_minMag, false);
    if (!extF80_eq(r, x)) return false;
    int32_t ex = (x.signExp & 0x7fff) - 0x3fff;
    if (ex > 30) return false;
    *n = extF80_to_i32(x, softfloat_round_minMag, false);
    return *n <= limit && *n >= -limit;
}

/* x for a 128-bit value */
static extFloat80_t fin(Q v, int32_t scale, uint8_t prec, uint_fast8_t mode, uint_fast8_t flags)
{
    softfloat_roundingMode = mode;
    softfloat_exceptionFlags = flags;
    return q_round(v, scale, prec);
}

extFloat80_t psmac_fpu_elementary(int op, extFloat80_t x, uint8_t prec, extFloat80_t *cosr)
{
    if (!ready) init();
    bool neg = x.signExp >> 15;
    bool zero = !(x.signExp & 0x7fff) && !x.signif;
    bool inf = (x.signExp & 0x7fff) == 0x7fff;
    uint_fast8_t mode = softfloat_roundingMode, flags = softfloat_exceptionFlags;
    int32_t n, e;
    Q v, q, E;

    /* the special operands first: exact results take no flags */
    switch (op) {
    case FOP_SIN: case FOP_TAN: case FOP_SINCOS: case FOP_COS:
        if (inf) { if (cosr) *cosr = x_nan(); return x_nan(); }
        if (zero) { if (cosr) *cosr = x_one(false); return op == FOP_COS ? x_one(false) : x; }
        break;
    case FOP_ATAN:
        if (zero) return x;
        if (inf) return fin(neg ? qneg(q_c(C_PI_2)) : q_c(C_PI_2), 0, prec, mode, flags);
        break;
    case FOP_ASIN: case FOP_ACOS: case FOP_ATANH: {
        if (zero) return op == FOP_ACOS ? fin(q_c(C_PI_2), 0, prec, mode, flags) : x;
        if (inf) return x_nan();
        int c = extF80_lt(x_one(false), x_make(x.signExp & 0x7fff, x.signif)) ? 1
              : extF80_eq(x_one(false), x_make(x.signExp & 0x7fff, x.signif)) ? 0 : -1;
        if (c > 0) return x_nan();
        if (c == 0) {
            if (op == FOP_ATANH) { softfloat_raiseFlags(softfloat_flag_infinite); return x_inf(neg); }
            if (op == FOP_ASIN) return fin(neg ? qneg(q_c(C_PI_2)) : q_c(C_PI_2), 0, prec, mode, flags);
            return neg ? fin(q_c(C_PI), 0, prec, mode, flags) : x_zero(false);
        }
        break;
    }
    case FOP_SINH: case FOP_TANH:
        if (zero) return x;
        if (inf) return op == FOP_SINH ? x : x_one(neg);
        break;
    case FOP_COSH:
        if (zero) return x_one(false);
        if (inf) return x_inf(false);
        break;
    case FOP_ETOX: case FOP_TWOTOX: case FOP_TENTOX:
        if (zero) return x_one(false);
        if (inf) return neg ? x_zero(false) : x;
        break;
    case FOP_ETOXM1:
        if (zero) return x;
        if (inf) return neg ? x_one(true) : x;
        break;
    case FOP_LOGN: case FOP_LOG10: case FOP_LOG2:
        if (zero) { softfloat_raiseFlags(softfloat_flag_infinite); return x_inf(true); }
        if (neg) return x_nan();
        if (inf) return x;
        if (x.signExp == 0x3fff && x.signif == BIT63) return x_zero(false);
        break;
    case FOP_LOGNP1:
        if (zero) return x;
        if (neg && (x.signExp & 0x7fff) >= 0x3fff) {
            if (x.signExp == 0xbfff && x.signif == BIT63) { softfloat_raiseFlags(softfloat_flag_infinite); return x_inf(true); }
            return x_nan();
        }
        if (inf) return x;
        break;
    default:
        return x_nan();
    }

    softfloat_roundingMode = softfloat_round_near_even;
    Q xq = extF80_to_f128(x), ax = qabs(xq);

    switch (op) {
    case FOP_SIN: case FOP_COS: case FOP_TAN: case FOP_SINCOS: {
        Q r, s, c;
        int quad = reduce(x, &r);
        s = sin_q(r);
        c = cos_q(r);
        Q sn, cs;
        switch (quad) {
        case 0: sn = s; cs = c; break;
        case 1: sn = c; cs = qneg(s); break;
        case 2: sn = qneg(s); cs = qneg(c); break;
        default: sn = qneg(c); cs = s; break;
        }
        if (neg) sn = qneg(sn);
        if (op == FOP_TAN) return fin(qd(sn, cs), 0, prec, mode, flags);
        if (op == FOP_COS) return fin(cs, 0, prec, mode, flags);
        if (op == FOP_SINCOS) {
            *cosr = fin(cs, 0, prec, mode, flags);
            uint_fast8_t f2 = softfloat_exceptionFlags;
            extFloat80_t sres = fin(sn, 0, prec, mode, flags);
            softfloat_exceptionFlags |= f2;
            return sres;
        }
        return fin(sn, 0, prec, mode, flags);
    }
    case FOP_ATAN:
        v = atan_q(ax);
        return fin(neg ? qneg(v) : v, 0, prec, mode, flags);
    case FOP_ASIN:
        v = atan_q(qd(ax, f128_sqrt(qm(qs(ONE, ax), qa(ONE, ax)))));
        return fin(neg ? qneg(v) : v, 0, prec, mode, flags);
    case FOP_ACOS:
        /* 2 atan(sqrt((1-x)/(1+x))): accurate at both ends */
        v = q_scale(atan_q(f128_sqrt(qd(qs(ONE, xq), qa(ONE, xq)))), 1);
        return fin(v, 0, prec, mode, flags);
    case FOP_ATANH:
        if (f128_lt(ax, q_scale(ONE, -3))) {      /* the series in x itself: atanh(a) = a + a^3/3 + ... */
            Q a2 = qm(ax, ax), acc = RECIP[41];
            for (int k = 19; k >= 0; k--)
                acc = qa(RECIP[2 * k + 1], qm(a2, acc));
            v = qm(ax, acc);
        } else {
            int32_t ee;
            Q u = qd(qa(ONE, ax), qs(ONE, ax));
            struct qe un = qe_norm(u, 0);
            ee = un.e;
            v = q_scale(ln_me(un.m, ee), -1);
        }
        return fin(neg ? qneg(v) : v, 0, prec, mode, flags);
    case FOP_SINH: case FOP_COSH: case FOP_TANH:
        if (op == FOP_TANH && f128_lt(qi(40), ax)) {    /* 1 - 2e^-2|x|, below the 113 bits: 1 less a little */
            v = qs(ONE, q_scale(ONE, -120));
            return fin(neg ? qneg(v) : v, 0, prec, mode, flags);
        }
        if (f128_lt(qi(20000), ax))
            return fin(neg && op == FOP_SINH ? qneg(ONE) : ONE, 30000, prec, mode, flags);
        if (f128_lt(qi(40), ax)) {
            E = exp_split(ax, &n);                /* e^|x| / 2: e^-|x| is beyond the 113 bits */
            v = qa(ONE, E);
            return fin(neg && op == FOP_SINH ? qneg(v) : v, n - 1, prec, mode, flags);
        }
        if (op == FOP_TANH) {
            /* tanh a = E / (E + 2) with E = e^(2a) - 1 */
            Q a2x = q_scale(ax, 1);
            E = exp_split(a2x, &n);
            if (n) E = qs(q_scale(qa(ONE, E), n), ONE);
            v = qd(E, qa(E, TWO));
            return fin(neg ? qneg(v) : v, 0, prec, mode, flags);
        }
        E = exp_split(ax, &n);
        if (n) E = qs(q_scale(qa(ONE, E), n), ONE);
        if (op == FOP_SINH)
            v = q_scale(qa(E, qd(E, qa(E, ONE))), -1);          /* (E + E/(E+1)) / 2 */
        else {
            Q ea = qa(ONE, E);
            v = q_scale(qa(ea, qd(ONE, ea)), -1);
        }
        return fin(neg && op == FOP_SINH ? qneg(v) : v, 0, prec, mode, flags);
    case FOP_ETOX:
        if (f128_lt(qi(20000), ax)) return fin(ONE, neg ? -30000 : 30000, prec, mode, flags);
        E = exp_split(xq, &n);
        return fin(qa(ONE, E), n, prec, mode, flags);
    case FOP_ETOXM1:
        if (f128_lt(qi(20000), ax)) return neg ? fin(qs(q_scale(ONE, -200), ONE), 0, prec, mode, flags)
                                               : fin(ONE, 30000, prec, mode, flags);
        E = exp_split(xq, &n);
        if (n == 0) return fin(E, 0, prec, mode, flags);
        if (n > 120) return fin(qa(ONE, E), n, prec, mode, flags);
        if (n < -200) n = -200;
        return fin(qs(q_scale(qa(ONE, E), n), ONE), 0, prec, mode, flags);
    case FOP_TWOTOX:
        if (f128_lt(qi(40000), ax)) return fin(ONE, neg ? -60000 : 60000, prec, mode, flags);
        if (x_isint(x, &n, 40000)) return fin(ONE, n, prec, mode, flags);
        n = f128_to_i32(xq, softfloat_round_near_even, false);
        E = expm1_small(qm(qs(xq, qi(n)), q_c(C_LN2)));
        return fin(qa(ONE, E), n, prec, mode, flags);
    case FOP_TENTOX: {
        if (f128_lt(qi(12000), ax)) return fin(ONE, neg ? -60000 : 60000, prec, mode, flags);
        int32_t xi = f128_to_i32(xq, softfloat_round_near_even, false);
        struct qe p = pow10_qe(xi < 0 ? -xi : xi);
        if (xi < 0) { struct qe one = { ONE, 0 }; p = qe_div(one, p); }
        Q xf = qs(xq, qi(xi));
        if (q_zero(xf)) return fin(p.m, p.e, prec, mode, flags);
        E = exp_split(qm(xf, q_c(C_LN10)), &n);
        struct qe ex = { qa(ONE, E), n };
        p = qe_mul(p, ex);
        return fin(p.m, p.e, prec, mode, flags);
    }
    case FOP_LOGN: case FOP_LOG2: case FOP_LOG10:
        q = x_split(x, &e);
        if (op == FOP_LOG2) {
            if (f128_eq(q, ONE)) return fin(qi(e), 0, prec, mode, flags);
            if (f128_lt(SQRT2, q)) { q = q_scale(q, -1); e++; }
            v = qa(qi(e), qm(atanh2(qd(qs(q, ONE), qa(q, ONE))), q_c(C_LOG2E)));
            return fin(v, 0, prec, mode, flags);
        }
        v = ln_me(q, e);
        if (op == FOP_LOG10) {
            v = qm(v, q_c(C_LOG10E));
            /* exact powers of ten give their exponent exactly */
            int32_t k = f128_to_i32(v, softfloat_round_near_even, false);
            if (k >= 0 && k <= 27) {
                struct qe p = pow10_qe(k);
                if (f128_eq(q_scale(p.m, p.e), xq)) return fin(qi(k), 0, prec, mode, flags);
            }
        }
        return fin(v, 0, prec, mode, flags);
    case FOP_LOGNP1:
        /* near zero, ln(1+x) = 2 atanh(x / (2 + x)) with no rounding of 1 + x */
        if (f128_lt(qi(-1), xq) && f128_lt(xq, qs(SQRT2, ONE)) && f128_lt(qs(qd(ONE, SQRT2), ONE), xq))
            return fin(atanh2(qd(xq, qa(TWO, xq))), 0, prec, mode, flags);
        {
            struct qe un = qe_norm(qa(ONE, xq), 0);
            return fin(ln_me(un.m, un.e), 0, prec, mode, flags);
        }
    }
    softfloat_roundingMode = mode;
    softfloat_exceptionFlags = flags;
    return x_nan();
}

/* ------------------------------------------------------------------ FMOVECR */

extFloat80_t psmac_fpu_constant(int offset, uint8_t prec)
{
    for (unsigned k = 0; k < sizeof psmac_fpu_rom / sizeof psmac_fpu_rom[0]; k++)
        if (psmac_fpu_rom[k].offset == offset)
            return softfloat_roundPackToExtF80(false, psmac_fpu_rom[k].exp, psmac_fpu_rom[k].sig, psmac_fpu_rom[k].extra, prec);
    return x_zero(false);
}

/* ------------------------------------------------------------------ packed decimal */

extFloat80_t psmac_fpu_from_packed(const uint32_t p[3], uint8_t prec)
{
    if (!ready) init();
    bool sign = p[0] >> 31, esign = (p[0] >> 30) & 1;
    if ((p[0] & 0x7fff0000u) == 0x7fff0000u) {
        uint64_t m = (uint64_t)p[1] << 32 | p[2];
        if (!m) return x_inf(sign);
        return x_make((sign ? 0x8000 : 0) | 0x7fff, m | BIT63);
    }
    int32_t ex = (int32_t)(((p[0] >> 24) & 15) * 100 + ((p[0] >> 20) & 15) * 10 + ((p[0] >> 16) & 15));
    uint64_t m = p[0] & 15;
    for (int i = 1; i < 3; i++)
        for (int d = 7; d >= 0; d--)
            m = m * 10 + ((p[i] >> (4 * d)) & 15);
    if (!m) return x_zero(sign);
    int32_t k = (esign ? -ex : ex) - 16;
    uint_fast8_t mode = softfloat_roundingMode;
    softfloat_roundingMode = softfloat_round_near_even;
    struct qe mq = qe_norm(ui64_to_f128(m), 0), pw = pow10_qe(k < 0 ? -k : k);
    struct qe v = k < 0 ? qe_div(mq, pw) : qe_mul(mq, pw);
    softfloat_roundingMode = mode;
    extFloat80_t r = q_round(sign ? qneg(v.m) : v.m, v.e, prec);
    return r;
}

static const uint64_t TEN[19] = {
    UINT64_C(1), UINT64_C(10), UINT64_C(100), UINT64_C(1000), UINT64_C(10000), UINT64_C(100000),
    UINT64_C(1000000), UINT64_C(10000000), UINT64_C(100000000), UINT64_C(1000000000),
    UINT64_C(10000000000), UINT64_C(100000000000), UINT64_C(1000000000000), UINT64_C(10000000000000),
    UINT64_C(100000000000000), UINT64_C(1000000000000000), UINT64_C(10000000000000000),
    UINT64_C(100000000000000000), UINT64_C(1000000000000000000)
};

int psmac_fpu_to_packed(extFloat80_t x, int k, uint32_t p[3])
{
    if (!ready) init();
    bool sign = x.signExp >> 15;
    int operr = 0;
    p[0] = sign ? 0x80000000u : 0;
    p[1] = p[2] = 0;
    if ((x.signExp & 0x7fff) == 0x7fff) {         /* infinity and NaN: the exponent field all ones */
        p[0] |= 0x7fff0000u;
        if (x.signif << 1) { p[1] = (uint32_t)(x.signif >> 32); p[2] = (uint32_t)x.signif; }
        return 0;
    }
    if (!(x.signExp & 0x7fff) && !x.signif) return 0;
    if (k > 17) { k = 17; operr = 1; }

    uint_fast8_t mode = softfloat_roundingMode;
    /* the magnitude is rounded: toward minus infinity for a negative number is away from zero */
    uint_fast8_t mmode = mode;
    if (sign && mode == softfloat_round_min) mmode = softfloat_round_max;
    else if (sign && mode == softfloat_round_max) mmode = softfloat_round_min;
    softfloat_roundingMode = softfloat_round_near_even;

    int32_t e2;
    Q m = x_split(x, &e2);
    struct qe a = { m, e2 };
    /* the decimal exponent: floor(log10 |x|), estimated from the binary one and corrected */
    int32_t ilog = (int32_t)(((int64_t)e2 * 30103 - (e2 < 0 ? 99999 : 0)) / 100000);
    for (int i = 0; i < 3; i++) {
        struct qe lo = pow10_qe(ilog < 0 ? -ilog : ilog), hi;
        if (ilog < 0) { struct qe one = { ONE, 0 }; lo = qe_div(one, lo); }
        hi = qe_mul(lo, pow10_qe(1));
        bool below = a.e < lo.e || (a.e == lo.e && f128_lt(a.m, lo.m));
        bool above = a.e > hi.e || (a.e == hi.e && f128_le(hi.m, a.m));
        if (below) ilog--;
        else if (above) ilog++;
        else break;
    }
    int digits = 1;
    uint64_t N = 0;
    for (int pass = 0; pass < 2; pass++) {
        digits = k > 0 ? k : ilog + 1 - k;
        if (digits > 17) digits = 17;
        if (digits < 1) digits = 1;
        int32_t sc = digits - 1 - ilog;
        struct qe p10 = pow10_qe(sc < 0 ? -sc : sc);
        struct qe s = sc < 0 ? qe_div(a, p10) : qe_mul(a, p10);
        Q sq = q_scale(s.m, s.e);
        uint_fast8_t f = softfloat_exceptionFlags;
        N = f128_to_ui64(sq, mmode, true);
        if (N >= TEN[digits] && pass == 0) { ilog++; softfloat_exceptionFlags = f; continue; }   /* 9.99.. rounded up */
        break;
    }
    softfloat_roundingMode = mode;
    /* the digits: the first in the low nibble of the first long, then sixteen after the point */
    uint8_t dig[17] = { 0 };
    for (int i = digits - 1; i >= 0; i--) { dig[i] = (uint8_t)(N % 10); N /= 10; }
    p[0] |= dig[0];
    for (int i = 0; i < 8; i++) {
        p[1] |= (uint32_t)dig[1 + i] << (28 - 4 * i);
        p[2] |= (uint32_t)dig[9 + i] << (28 - 4 * i);
    }
    int32_t ae = ilog < 0 ? -ilog : ilog;
    if (ilog < 0) p[0] |= 0x40000000u;
    if (ae > 999) { operr = 1; p[0] |= (uint32_t)((ae / 1000) % 10) << 12; }
    p[0] |= (uint32_t)((ae / 100) % 10) << 24 | (uint32_t)((ae / 10) % 10) << 20 | (uint32_t)(ae % 10) << 16;
    return operr;
}
