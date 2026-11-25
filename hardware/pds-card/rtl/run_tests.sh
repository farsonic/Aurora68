#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Card P's CPLD in simulation: the testbench, then two deliberately broken copies of the design that the testbench
# must catch (a read latch closed one clock early, as the classic PiStorm does; /AS on a C8M falling edge, in S3;
# /BR released once the card has the bus; a stock-mode request that letting go of Interrupt does not cancel).
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
mutant "      S_S1: begin
        if (c8m_rising) begin" "      S_S1: begin
        if (c8m_falling) begin" "/AS asserted on a falling edge (S3)"
mutant "      A_OWN: begin                        // /BR and /BGACK both held for as long as the card owns the bus
        br_assert <= 1'b1;" "      A_OWN: begin
        br_assert <= 1'b0;" "/BR released once /BGACK is asserted (the earlier arbitration)"
mutant "    end else if (ipl2_c8[1]) begin        // Interrupt not held: nothing armed
      sw_hold <= 1'b0;" "    end else if (ipl2_c8[1]) begin
      sw_hold <= sw_hold;" "Interrupt let go before the reset ends does not cancel"
rm -f "$OUT.vvp" "$OUT.log" "$OUT.mut.v" "$OUT.mut.vvp"
exit $status
