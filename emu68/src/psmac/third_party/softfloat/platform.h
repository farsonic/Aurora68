/* SPDX-License-Identifier: MPL-2.0
 *
 * platform.h - SoftFloat 3e's build configuration for Aurora68 (README.md). SoftFloat's own packages
 * carry one per build directory (build/Linux-x86_64-GCC/platform.h and the others); this one serves
 * the big-endian AArch64 kernel and the little-endian host the tests run on alike, so the byte order
 * comes from the compiler. The options are those of SoftFloat's 64-bit GCC builds.
 */
#ifndef PSMAC_SOFTFLOAT_PLATFORM_H
#define PSMAC_SOFTFLOAT_PLATFORM_H

#define SOFTFLOAT_FAST_INT64
#define SOFTFLOAT_FAST_DIV32TO16
#define SOFTFLOAT_FAST_DIV64TO32
#define INLINE_LEVEL 5

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define LITTLEENDIAN 1
#endif

#ifdef __GNUC_STDC_INLINE__
#define INLINE inline
#else
#define INLINE extern inline
#endif

#define SOFTFLOAT_BUILTIN_CLZ 1
#define SOFTFLOAT_INTRINSIC_INT128 1
#include "opts-GCC.h"

#endif
