#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# fpu_test.sh [BUILD_DIR] [ROUNDS] - the exact FPU (issue #27) on the host: tools/fpu_test.c with the kernel's
# own psmac_fpu.c, psmac_fpu_math.c and SoftFloat 3e, against the host's x87 and C library. Needs an x86-64
# host (its long double is the 68881's extended format); skipped elsewhere, with a line saying so.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
B=${1:-$ROOT/mac/build}/fpu
[ "$(uname -m)" = x86_64 ] || { echo "fpu_test: skipped, the oracle is an x86-64's long double"; exit 0; }
command -v cc > /dev/null || { echo "fpu_test: skipped, no cc"; exit 0; }
mkdir -p "$B"
SF=$ROOT/emu68/src/psmac/third_party/softfloat
SRC=$(sed -n 's|^    src/psmac/third_party/softfloat/|'"$SF"'/|p' "$SF/files.cmake")
cc -std=gnu11 -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-label -Wno-implicit-fallthrough -Wno-sign-compare -DPSMAC_FPU_HOST \
   -I "$ROOT/emu68/src/psmac" -I "$SF" -I "$SF/include" -I "$SF/68881" \
   -o "$B/fpu_test" "$ROOT/tools/fpu_test.c" "$ROOT/emu68/src/psmac/psmac_fpu.c" "$ROOT/emu68/src/psmac/psmac_fpu_math.c" \
   $SRC -lm
"$B/fpu_test" --dump "$B/elementary.txt" ${2:-20000}
# the elementary functions against 80-digit arithmetic (the C library is the looser oracle above)
if command -v python3 > /dev/null; then python3 "$ROOT/tools/fpu_refcheck.py" "$B/elementary.txt"; fi
