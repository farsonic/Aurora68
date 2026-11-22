#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""softfloat-68881.py DIR - turn a copy of SoftFloat 3e's 8086-SSE specialization (DIR) into the 68881's.

22 November 2026 (issue #27). The changes emu68/src/psmac/third_party/softfloat/README.md lists, made the
same way again on a later SoftFloat: the default NaNs, tininess before rounding, the integer overflow
values, and the 68881's NaN propagation (the destination's NaN first). Each replaced line must be found
exactly once, or the script stops. Standard library only.

    cp -r SoftFloat-3e/<the package's code>/8086-SSE DIR && python3 tools/softfloat-68881.py DIR
"""
import sys, os
d = sys.argv[1]
p = os.path.join(d, 'specialize.h')
s = open(p).read()
rep = [
    ('#define init_detectTininess softfloat_tininess_afterRounding',
     '#define init_detectTininess softfloat_tininess_beforeRounding'),
    ('#define i32_fromPosOverflow  (-0x7FFFFFFF - 1)', '#define i32_fromPosOverflow  0x7FFFFFFF'),
    ('#define i32_fromNaN          (-0x7FFFFFFF - 1)', '#define i32_fromNaN          0x7FFFFFFF'),
    ('#define i64_fromPosOverflow  (-INT64_C( 0x7FFFFFFFFFFFFFFF ) - 1)', '#define i64_fromPosOverflow  INT64_C( 0x7FFFFFFFFFFFFFFF )'),
    ('#define i64_fromNaN          (-INT64_C( 0x7FFFFFFFFFFFFFFF ) - 1)', '#define i64_fromNaN          INT64_C( 0x7FFFFFFFFFFFFFFF )'),
    ('#define defaultNaNF32UI 0xFFC00000', '#define defaultNaNF32UI 0x7FFFFFFF'),
    ('#define defaultNaNF64UI UINT64_C( 0xFFF8000000000000 )', '#define defaultNaNF64UI UINT64_C( 0x7FFFFFFFFFFFFFFF )'),
    ('#define defaultNaNExtF80UI64 0xFFFF', '#define defaultNaNExtF80UI64 0x7FFF'),
    ('#define defaultNaNExtF80UI0  UINT64_C( 0xC000000000000000 )', '#define defaultNaNExtF80UI0  UINT64_C( 0xFFFFFFFFFFFFFFFF )'),
    ('#define defaultNaNF128UI64 UINT64_C( 0xFFFF800000000000 )', '#define defaultNaNF128UI64 UINT64_C( 0x7FFFFFFFFFFFFFFF )'),
    ('#define defaultNaNF128UI0  UINT64_C( 0 )', '#define defaultNaNF128UI0  UINT64_C( 0xFFFFFFFFFFFFFFFF )'),
]
for a, b in rep:
    assert s.count(a) == 1, a
    s = s.replace(a, b)
note = '''/*----------------------------------------------------------------------------
| Motorola 68881/68882 specialization, made for Aurora68 (PiStorm-SE) on
| 22 November 2026 from the 8086-SSE one (README.md in the directory above):
| default NaNs have every fraction bit set and a clear sign (the 68881's),
| tininess is detected before rounding, a positive overflow to an integer
| gives the largest positive integer, and of two NaN operands the first
| (the 68k instruction's destination) is the one propagated.
*----------------------------------------------------------------------------*/

#ifndef specialize_h'''
s = s.replace('#ifndef specialize_h', note, 1)
open(p, 'w').write(s)

body = {
 'ExtF80': ('''struct uint128
 softfloat_propagateNaNExtF80UI(
     uint_fast16_t uiA64,
     uint_fast64_t uiA0,
     uint_fast16_t uiB64,
     uint_fast64_t uiB0
 )
{
    struct uint128 uiZ;

    if (
        softfloat_isSigNaNExtF80UI( uiA64, uiA0 )
            || softfloat_isSigNaNExtF80UI( uiB64, uiB0 )
    ) {
        softfloat_raiseFlags( softfloat_flag_invalid );
    }
    /* 68881: the destination (first) operand if it is a NaN, else the other */
    if ( isNaNExtF80UI( uiA64, uiA0 ) ) {
        uiZ.v64 = uiA64;
        uiZ.v0  = uiA0 | UINT64_C( 0xC000000000000000 );
    } else {
        uiZ.v64 = uiB64;
        uiZ.v0  = uiB0 | UINT64_C( 0xC000000000000000 );
    }
    return uiZ;

}
'''),
 'F128': ('''struct uint128
 softfloat_propagateNaNF128UI(
     uint_fast64_t uiA64,
     uint_fast64_t uiA0,
     uint_fast64_t uiB64,
     uint_fast64_t uiB0
 )
{
    struct uint128 uiZ;

    if (
        softfloat_isSigNaNF128UI( uiA64, uiA0 )
            || softfloat_isSigNaNF128UI( uiB64, uiB0 )
    ) {
        softfloat_raiseFlags( softfloat_flag_invalid );
    }
    /* 68881 rule: the first operand if it is a NaN, else the other */
    if ( isNaNF128UI( uiA64, uiA0 ) ) {
        uiZ.v64 = uiA64 | UINT64_C( 0x0000800000000000 );
        uiZ.v0  = uiA0;
    } else {
        uiZ.v64 = uiB64 | UINT64_C( 0x0000800000000000 );
        uiZ.v0  = uiB0;
    }
    return uiZ;

}
'''),
 'F64': ('''uint_fast64_t
 softfloat_propagateNaNF64UI( uint_fast64_t uiA, uint_fast64_t uiB )
{
    if ( softfloat_isSigNaNF64UI( uiA ) || softfloat_isSigNaNF64UI( uiB ) ) {
        softfloat_raiseFlags( softfloat_flag_invalid );
    }
    /* 68881 rule: the first operand if it is a NaN, else the other */
    return (isNaNF64UI( uiA ) ? uiA : uiB) | UINT64_C( 0x0008000000000000 );

}
'''),
 'F32': ('''uint_fast32_t
 softfloat_propagateNaNF32UI( uint_fast32_t uiA, uint_fast32_t uiB )
{
    if ( softfloat_isSigNaNF32UI( uiA ) || softfloat_isSigNaNF32UI( uiB ) ) {
        softfloat_raiseFlags( softfloat_flag_invalid );
    }
    /* 68881 rule: the first operand if it is a NaN, else the other */
    return (isNaNF32UI( uiA ) ? uiA : uiB) | 0x00400000;

}
'''),
}
for k, new in body.items():
    p = os.path.join(d, 's_propagateNaN%sUI.c' % k)
    s = open(p).read()
    # keep the licence and the includes; the function after the last comment block is replaced
    i = s.index('/*----------------------------------------------------------------------------\n| Interpreting')
    j = s.index('*/', i) + 2
    s = s[:j] + '\n' + new
    open(p, 'w').write(s)
print('ok')
