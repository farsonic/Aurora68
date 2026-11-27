#!/bin/bash
# SPDX-License-Identifier: MPL-2.0
# runse.sh SECS LOGDIR KERNEL [kernel options]: A/UX in the MAME SE full stack, the A/UX disk image as SCSI ID 0.
# Environment: AUXDSK (the disk image, a copy: A/UX writes to it; default $AUXWORK/AUX.dsk), AUXWORK (a scratch
# directory, default /tmp/aux-work), DECLROM (default mac/build/psmac-512x342.declrom), PSMAC_RAM (use 64 for A/UX),
# MAME_SCRIPT / ACT_PLAN / ACT_WAIT (mame/act.lua, a scripted mouse). One emulator at a time: the lock is the
# directory $AUXWORK/qemu.lock, taken here and given back at the end; a second caller waits up to an hour.
# After the run the snapshots are in LOGDIR/snaps and MAME's saved PRAM is put back from $AUXWORK/macse_ps.rtc.orig
# if that file exists (keep a copy of mame/nvramFS/macse_ps/rtc from a good boot there).
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
W=${AUXWORK:-/tmp/aux-work}; mkdir -p "$W"
SECS=$1; LOG=$2; K=$3; shift 3
for i in $(seq 1 3600); do mkdir "$W/qemu.lock" 2>/dev/null && break; sleep 1; done
[ "$i" -lt 3600 ] || { echo "lock held"; exit 1; }
mkdir -p "$LOG"; cd "$ROOT"
IMG=$K LOGDIR=$LOG MACHINE=macse_ps HD=${HD:-none} PSMAC_RAM=${PSMAC_RAM:-64} ROM=${ROM:-rom/original/MacIIci_368CADFE.rom} \
  tools/run-fullstack.sh "$SECS" mac.board=se mac.declrom=${DECLROM:-$ROOT/mac/build/psmac-512x342.declrom} bt=off wifi=off \
  scsi.disk=${AUXDSK:-$W/AUX.dsk}:0 mac.boot=scsi "$@"
cp -r mame/snapFS/macse_ps "$LOG/snaps" 2>/dev/null
[ -f "$W/macse_ps.rtc.orig" ] && cp -p "$W/macse_ps.rtc.orig" mame/nvramFS/macse_ps/rtc
rmdir "$W/qemu.lock"
