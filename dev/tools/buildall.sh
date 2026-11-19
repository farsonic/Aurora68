#!/bin/bash
# SPDX-License-Identifier: MPL-2.0
#
# buildall.sh: build every artefact of the workspace, and with --test run the native test suites.
#
# usage: dev/tools/buildall.sh [--test] [--jobs N] [PART...]
#   PART: kernel mac riptide lagoon quill (default: all five, in that order)
#   --test   afterwards: make -C mac check, Riptide's host tests, Lagoon's make test, Quill's
#            native tests (the cc line in Quill's CLAUDE.md)
#   logs in $SCRATCH/logs/build-PART.log (SCRATCH defaults to WORK/scratch); the last lines of a
#   failing step are printed
#
# What each part makes (the paths dev/tools/mkfullimage.sh and mktestset.sh take them from):
#   kernel   pistorm-se/psmac-build/Emu68.img and Emu68.img.gz (cmake, Emu68's toolchain file)
#   mac      pistorm-se/mac/build/*.bin and the declaration ROMs (make -C mac)
#   riptide  riptide/build/mac (Riptide.bin, RiptideDNR.bin, RiptidePrefs.bin, RiptideTerminal.bin,
#            MacTCPTest.bin, BrowserTest.bin) and riptide/build/mac-a68 (the same with AURORA68=1:
#            their log also goes to the Pi kernel's log)
#   lagoon   lagoon/build/mac-a68 (Lagoon.bin, LagoonEtherTalk.bin; AURORA68=1)
#   quill    quill/build/mac-a68/Octavo.bin (AURORA68=1)
# Riptide's tree is the Mac application kit Lagoon and Quill build against (RIPTIDE=../riptide,
# its mac-port branch), so it goes first when they are built together.
set -uo pipefail
. "$(dirname "$0")/lib.sh"

TEST=0 JOBS=$(nproc 2>/dev/null || echo 4) PARTS=
while [ $# -gt 0 ]; do
	case "$1" in
	--test) TEST=1 ;;
	--jobs|-j) JOBS=$2; shift ;;
	-h|--help) sed -n '4,24p' "$0"; exit 0 ;;
	kernel|mac|riptide|lagoon|quill) PARTS="$PARTS $1" ;;
	*) die "unknown argument $1 (--help)" ;;
	esac
	shift
done
PARTS=${PARTS:-kernel mac riptide lagoon quill}
LOGS=$SCRATCH/logs; mkdir -p "$LOGS" || die "cannot write $LOGS (on Linux, does the container run as the workspace's owner?)"
FAILED=

step() {   # step NAME DIR COMMAND...: run, log, report
	local name=$1 dir=$2; shift 2
	local log=$LOGS/build-$name.log t0=$SECONDS
	printf '%-22s ' "$name"
	if (cd "$dir" && "$@") >>"$log" 2>&1; then
		echo "ok ($((SECONDS - t0)) s)"
	else
		echo "FAILED ($((SECONDS - t0)) s; $log)"; tail -15 "$log" | sed 's/^/    /'
		FAILED="$FAILED $name"
	fi
}
fresh() { : > "$LOGS/build-$1.log"; }

for p in $PARTS; do
	case $p in
	kernel)
		fresh kernel
		[ -d "$AURORA68/emu68/external/capstone/include" ] || die "emu68/external/capstone is empty: git -C $AURORA68 submodule update --init --depth 1"
		[ -f "$AURORA68/psmac-build/CMakeCache.txt" ] || step kernel-cmake "$AURORA68" cmake -S "$AURORA68/emu68" -B "$AURORA68/psmac-build" \
			-DCMAKE_TOOLCHAIN_FILE="$AURORA68/emu68/toolchains/aarch64-linux-gnu-13.cmake" \
			-DTARGET=raspi64 -DVARIANT=psmac -DCMAKE_BUILD_TYPE=Release
		step kernel "$AURORA68" bash -c "cmake --build psmac-build -j$JOBS && gunzip -kf psmac-build/Emu68.img.gz"
		;;
	mac)
		fresh mac
		step mac "$AURORA68" make -C mac -j"$JOBS"
		;;
	riptide)
		fresh riptide; fresh riptide-a68
		step riptide "$RIPTIDE" make -f Makefile.mac -j"$JOBS" all test
		step riptide-a68 "$RIPTIDE" make -f Makefile.mac -j"$JOBS" AURORA68=1 BUILD=build/mac-a68 all test
		;;
	lagoon)
		fresh lagoon
		step lagoon "$LAGOON" make -f Makefile.mac -j"$JOBS" AURORA68=1 B=build/mac-a68 RIPTIDE="$RIPTIDE"
		;;
	quill)
		fresh quill
		step quill "$QUILL" make -f Makefile.mac -j"$JOBS" AURORA68=1 B=build/mac-a68 RIPTIDE="$RIPTIDE"
		;;
	esac
done

if [ $TEST = 1 ]; then
	note "native tests"
	for n in mac-check riptide-check lagoon-test quill-test; do fresh $n; done
	step mac-check "$AURORA68" make -C mac check
	step riptide-check "$RIPTIDE" make -j"$JOBS" check
	step lagoon-test "$LAGOON" make -j"$JOBS" test
	# Quill's CLAUDE.md: run from the repository root; expect "N tests, 0 failed"
	step quill-test "$QUILL" bash -c 'cc -std=c99 -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined \
		core/*.c tests/coretest.c tests/test_wp*.c -o "$0/quill_coretest" && "$0/quill_coretest" | tail -3' "$SCRATCH"
	grep -h -E 'tests?, [0-9]+ fail|checks?, [0-9]+ fail|PASS|FAIL|skipped' "$LOGS"/build-*-test.log "$LOGS"/build-*-check.log 2>/dev/null | tail -12 | sed 's/^/    /'
fi

[ -z "$FAILED" ] && { note "all built${TEST:+}"; exit 0; } || { note "failed:$FAILED"; exit 1; }
