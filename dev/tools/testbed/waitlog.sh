#!/bin/bash
# SPDX-License-Identifier: MPL-2.0
#
# waitlog.sh: wait until a testbed run's kernel log holds a line, then return.
#
# usage: dev/tools/testbed/waitlog.sh RUN|LOG PATTERN [SECONDS] [COUNT]
#   RUN      a run's name (its folder in $SCRATCH/runs) or the path of an emu68.log
#   PATTERN  an extended regular expression (grep -E), e.g. '\[BOOT\] .*Finder'
#   SECONDS  give up after this long (default 600): exit status 1
#   COUNT    wait for the COUNT-th matching line (default 1)
# Useful marks: "[BOOT] t=... the Finder" (the desktop is up), "[MAC] " lines from the Mac's
# programs, "[CRASH]" (a system error's record), "[NET] echo" (a ping's timing).
set -uo pipefail
. "$(dirname "$0")/../lib.sh"
[ $# -ge 2 ] || die "usage: waitlog.sh RUN|LOG PATTERN [SECONDS] [COUNT]"
L=$1; [ -f "$L" ] || L=$SCRATCH/runs/$1/emu68.log
T=${3:-600} N=${4:-1} t0=$SECONDS
until [ "$(grep -a -c -E -- "$2" "$L" 2>/dev/null || true)" -ge "$N" ] 2>/dev/null; do
	[ $((SECONDS - t0)) -ge "$T" ] && { echo "waitlog: no \"$2\" in $L after $T s" >&2; exit 1; }
	sleep 2
done
grep -a -E -- "$2" "$L" | sed -n "${N}p"
