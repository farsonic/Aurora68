/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_fpu_int.h - what the exact FPU's two halves share (psmac_fpu.c, the instructions; psmac_fpu_math.c,
 * the elementary functions, FMOVECR's constants and the packed decimal format). Both work on SoftFloat's
 * extFloat80_t and leave the IEEE flags of what they did in softfloat_exceptionFlags, rounding with
 * softfloat_roundingMode to the precision they are given (32, 64 or 80 bits of significand). Written 22
 * November 2026 (issue #27).
 */
#ifndef PSMAC_FPU_INT_H
#define PSMAC_FPU_INT_H
#include <stdbool.h>
#include <stdint.h>
#include "platform.h"
#include "softfloat.h"

/* the opmodes of the elementary functions (the command word's low 7 bits) */
#define FOP_SINH    0x02
#define FOP_LOGNP1  0x06
#define FOP_ETOXM1  0x08
#define FOP_TANH    0x09
#define FOP_ATAN    0x0a
#define FOP_ASIN    0x0c
#define FOP_ATANH   0x0d
#define FOP_SIN     0x0e
#define FOP_TAN     0x0f
#define FOP_ETOX    0x10
#define FOP_TWOTOX  0x11
#define FOP_TENTOX  0x12
#define FOP_LOGN    0x14
#define FOP_LOG10   0x15
#define FOP_LOG2    0x16
#define FOP_COSH    0x19
#define FOP_ACOS    0x1c
#define FOP_COS     0x1d
#define FOP_SINCOS  0x30

/* f(x) for one of the opmodes above, rounded to prec; for FOP_SINCOS the sine, and the cosine in *cos.
   x is never a NaN (the caller propagates those). A domain error raises softfloat_flag_invalid and
   returns the default NaN; a pole raises softfloat_flag_infinite. */
extFloat80_t psmac_fpu_elementary(int op, extFloat80_t x, uint8_t prec, extFloat80_t *cos);

/* FMOVECR #offset: the constant rounded to prec (inexact flagged as the 68881 does); 0 for offsets
   the 68881's ROM has nothing documented at */
extFloat80_t psmac_fpu_constant(int offset, uint8_t prec);

/* the packed decimal format (three longs, the first the signs, exponent and integer digit) */
extFloat80_t psmac_fpu_from_packed(const uint32_t p[3], uint8_t prec);
/* returns 1 when the 68881 would set OPERR (k-factor above 17, or a decimal exponent of four digits) */
int psmac_fpu_to_packed(extFloat80_t x, int k, uint32_t p[3]);

/* a finite value unpacked with an exponent of any size, rounded to an extended value at last:
   value = sig.extra * 2^(exp - 16383 - 63), sig normalized (bit 63 set) */
extFloat80_t psmac_fpu_round(bool sign, int32_t exp, uint64_t sig, uint64_t extra, uint8_t prec);

#endif
