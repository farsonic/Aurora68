#!/bin/bash
# SPDX-License-Identifier: MPL-2.0
# run-aux-net.sh SECS LOGDIR KERNEL DISK [kernel options]: A/UX in the MAME SE full stack (tools/run-fullstack.sh) with
# the A/UX disk image at SCSI ID 0, mac.mmu=full, 64 MiB, and the host's network peer (tools/pinet.py) on the
# testbed's file bridge, so the A/UX network card (mac.auxnet=on, docs/24-aux-network.md) has somebody to talk to.
#   env: DECLROM (the SE's declaration ROM, default mac/build/psmac-512x342.declrom), ROM (the IIci ROM),
#        LOCK (a directory used as a lock, made before the run and removed after it: one emulator at a time, and
#        the bridge's pipes, MAME's nvram and its snapshots are shared; the run waits for it), RTC (a saved copy of
#        mame/nvramFS/macse_ps/rtc to put back afterwards, so each run starts from the same parameter RAM)
# The disk image is written to: give it a copy. Logs: LOGDIR/emu68.log, mame.log, pinet.log; snapshots LOGDIR/snaps.
cd "$(dirname "$0")/../.."
SECS=$1; LOG=$2; K=$3; D=$4; shift 4
DECLROM=${DECLROM:-$PWD/mac/build/psmac-512x342.declrom}
if [ -n "$LOCK" ]; then until mkdir "$LOCK" 2>/dev/null; do sleep 10; done; echo "lock taken $(date +%T)"; fi
mkdir -p "$LOG"
rm -f /tmp/psmac-net.tx /tmp/psmac-net.rx
python3 tools/pinet.py /tmp/psmac-net peer > "$LOG/pinet.log" 2>&1 &
PP=$!
IMG=$K LOGDIR=$LOG MACHINE=macse_ps HD=none PSMAC_RAM=64 ROM=${ROM:-rom/original/MacIIci_368CADFE.rom} \
  tools/run-fullstack.sh "$SECS" mac.board=se mac.declrom="$DECLROM" bt=off wifi=off \
  scsi.disk="$D":0 mac.boot=scsi mac.mmu=full mac.net=/tmp/psmac-net "$@"
kill $PP
cp -r mame/snapFS/macse_ps "$LOG/snaps" 2>/dev/null
[ -n "$RTC" ] && cp -p "$RTC" mame/nvramFS/macse_ps/rtc
if [ -n "$LOCK" ]; then rmdir "$LOCK"; echo "lock released $(date +%T)"; fi
