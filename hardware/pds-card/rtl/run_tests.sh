#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Card P's CPLD in simulation: the testbench, then two deliberately broken copies of the design that the testbench
# must catch (a read latch closed one clock early, as the classic PiStorm does; /AS on a C8M falling edge, in S3).
# Needs Icarus Verilog (IVERILOG and VVP name the binaries if they are not on PATH). Exit status 0 when all is well.
set -e
cd "$(dirname "$0")"
IVERILOG=${IVERILOG:-iverilog}
VVP=${VVP:-vvp}
OUT=${TMPDIR:-/tmp}/aurora68_pds_tb.$$
"$IVERILOG" -g2005 -o "$OUT.vvp" aurora68_pds.v tb/tb_card.v
"$VVP" -n "$OUT.vvp" | tee "$OUT.log" | grep -E '^(FAIL|PASS)|checks'
grep -q '^PASS$' "$OUT.log"
status=0
mutant() {
    python3 - "$1" "$2" "$OUT.mut.v" <<'PY'
import sys
s = open('aurora68_pds.v').read()
a, b = sys.argv[1], sys.argv[2]
assert a in s, 'mutation anchor not found'
open(sys.argv[3], 'w').write(s.replace(a, b, 1))
PY
    "$IVERILOG" -g2005 -o "$OUT.mut.vvp" "$OUT.mut.v" tb/tb_card.v
    if "$VVP" -n "$OUT.mut.vvp" | grep -q '^PASS$'; then echo "MUTANT NOT CAUGHT: $3"; status=1; else echo "mutant caught: $3"; fi
}
mutant "      S_S6: begin
        if (c8m_falling) begin" "      S_S6: begin
        if (1) begin" "read latch closed at once after /DTACK (the classic early sample)"
mutant "if (c8m_rising && (!pmcyc_align" "if (c8m_falling && (!pmcyc_align" "/AS asserted on a falling edge (S3)"
rm -f "$OUT.vvp" "$OUT.log" "$OUT.mut.v" "$OUT.mut.vvp"
exit $status
