# SPDX-License-Identifier: MPL-2.0
# lib.sh: sourced by the dev/tools scripts. Finds the workspace and its five repositories.
#
# The workspace is the folder that holds the checkouts side by side (dev/bootstrap.sh makes it):
#   WORK/pistorm-se  Aurora68    WORK/riptide  Riptide    WORK/lagoon  Lagoon
#   WORK/quill       Quill       WORK/cornerstone  the style guide
# By default WORK is the folder above the Aurora68 checkout these scripts live in; AURORA68_WORK
# overrides it, and AURORA68, RIPTIDE, LAGOON, QUILL override single checkouts.
# Scratch output (logs, testbed runs, built images) goes under WORK/scratch unless SCRATCH says.

_here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
AURORA68=${AURORA68:-$(cd "$_here/../.." && pwd)}
WORK=${AURORA68_WORK:-$(cd "$AURORA68/.." && pwd)}
RIPTIDE=${RIPTIDE:-$WORK/riptide}
LAGOON=${LAGOON:-$WORK/lagoon}
QUILL=${QUILL:-$WORK/quill}
CORNERSTONE=${CORNERSTONE:-$WORK/cornerstone}
SCRATCH=${SCRATCH:-$WORK/scratch}
DEVTOOLS=$_here

die() { echo "$(basename "$0"): $*" >&2; exit 1; }
note() { echo "== $*"; }

# One QEMU at a time: the testbed's monitor socket (/tmp/qemu-mon.sock), the semihosting files
# and pinet's /tmp/psmac-net.* are shared by every run on the machine. The lock is a directory
# (mkdir is atomic); a lock left by a run that was killed is removed by hand (rmdir) once
# `pgrep -a qemu-system` shows no QEMU.
QEMU_LOCK=${QEMU_LOCK:-$SCRATCH/qemu.lock}
qemu_lock() {
	mkdir -p "$(dirname "$QEMU_LOCK")"
	local waited=0
	until mkdir "$QEMU_LOCK" 2>/dev/null; do
		[ "${QEMU_LOCK_WAIT:-0}" = 1 ] || die "the testbed is busy ($QEMU_LOCK exists: another run, or a stale lock after a crash)"
		[ $waited = 0 ] && echo "waiting for $QEMU_LOCK ..."
		waited=1; sleep 2
	done
	echo "$$ $(date +%T) $0" > "$QEMU_LOCK/owner"
	if pgrep -f '(^|/)qemu-system-aarch64 ' >/dev/null; then
		rm -f "$QEMU_LOCK/owner"; rmdir "$QEMU_LOCK"
		die "a qemu-system-aarch64 is running without the lock (pgrep -af qemu-system-aarch64)"
	fi
}
qemu_unlock() { [ -d "$QEMU_LOCK" ] && grep -q "^$$ " "$QEMU_LOCK/owner" 2>/dev/null && { rm -f "$QEMU_LOCK/owner"; rmdir "$QEMU_LOCK"; }; true; }

# the name inside a MacBinary file (what hcopy -m calls it on the HFS volume)
macbin_name() { python3 -c "import sys;d=open(sys.argv[1],'rb').read(128);print(d[2:2+d[1]].decode('mac_roman'))" "$1"; }

# a commit id with a word on whether it is on the remote
rev() { local d=$1 c; c=$(git -C "$d" rev-parse --short HEAD 2>/dev/null) || { echo "?"; return; }
	git -C "$d" diff --quiet HEAD 2>/dev/null || c="$c+changes"; echo "$c"; }
