#!/bin/bash
# SPDX-License-Identifier: MPL-2.0
#
# shot.sh: a picture of the testbed's screen (the Pi's HDMI screen, which on the solo board is the
# Mac's main screen), as PNG, through QEMU's monitor.
#
# usage: dev/tools/testbed/shot.sh OUT.png                one picture now
#        dev/tools/testbed/shot.sh OUT.png EVERY COUNT    COUNT pictures, EVERY seconds apart (OUT-NN.png)
# It is Aurora68's tools/vcshot.py (no Python libraries needed). The pixels are the Pi screen's,
# the coordinates mouse.py takes. Claude Code can look at the PNG with its Read tool.
set -euo pipefail
. "$(dirname "$0")/../lib.sh"
[ $# -ge 1 ] || die "usage: shot.sh OUT.png [EVERY COUNT]"
[ -S "${QEMU_MON:-/tmp/qemu-mon.sock}" ] || die "no QEMU monitor (is a testbed run up? boot.sh --bg)"
mkdir -p "$(dirname "$1")"
if [ $# -ge 3 ]; then
	exec python3 "$AURORA68/tools/vcshot.py" "$1" --every "$2" --count "$3" --sock "${QEMU_MON:-/tmp/qemu-mon.sock}"
fi
exec python3 "$AURORA68/tools/vcshot.py" "$1" --sock "${QEMU_MON:-/tmp/qemu-mon.sock}"
