#!/bin/bash
# SPDX-License-Identifier: MPL-2.0
#
# stop.sh: end a testbed run started by boot.sh before its time is up.
#
# usage: dev/tools/testbed/stop.sh [RUN]     RUN: its name in $SCRATCH/runs (default: the newest)
# It stops that run's QEMU only (found by its log folder on QEMU's command line, never by name:
# another run may be someone else's); boot.sh then takes the last screenshot if it can, stops
# the pinet peers and releases the lock.
set -uo pipefail
. "$(dirname "$0")/../lib.sh"
R=${1:-$(ls -t "$SCRATCH/runs" 2>/dev/null | head -1)}
[ -n "$R" ] || die "no runs in $SCRATCH/runs"
D=$SCRATCH/runs/$R
P=$(pgrep -f -- "file:$D/miniuart.log" || true)
[ -n "$P" ] || die "no QEMU of run $R is running"
python3 "$AURORA68/tools/vcshot.py" "$D/stopped.png" >/dev/null 2>&1 || true
kill $P && echo "stopped run $R (QEMU $P); last picture $D/stopped.png"
