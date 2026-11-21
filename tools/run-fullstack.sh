#!/bin/bash
# Start the MAME IIci bridge motherboard (FIFO transport) and Emu68 under QEMU; logs under $LOGDIR.
# usage: tools/run-fullstack.sh [seconds] [extra Emu68 cmdline tokens]
cd "$(dirname "$0")/.."
SECS=${1:-300}; shift
LOGDIR=${LOGDIR:-/tmp/psmac-logs}; mkdir -p "$LOGDIR"
export PSMAC_BRIDGE_FIFO=/tmp/psmac.req:/tmp/psmac.rep
rm -f /tmp/psmac.req /tmp/psmac.rep
rm -rf mame/snapFS
MACHINE=${MACHINE:-maciici_ps}                       # macse_ps: the Macintosh SE board (pass mac.board=se to the kernel)
# MAME_EXTRA: more MAME arguments, e.g. "-flop1 floppy.img" or "-printer null_modem -bitb socket.127.0.0.1:6809"
# (the SE's modem port, SCC channel A, is MAME's "printer" slot: docs/08 item 93)
reference/mame/mamepsmac $MACHINE -rompath mame/roms -cfg_directory mame/cfgFS -nvram_directory mame/nvramFS \
  -video none -sound none -nothrottle -frameskip 8 -hard1 ${HD:-images/chd/hd20.chd} -snapshot_directory mame/snapFS $MAME_EXTRA \
  -verbose -seconds_to_run $((SECS+20)) -autoboot_script ${MAME_SCRIPT:-mame/shots.lua} -skip_gameinfo > "$LOGDIR/mame.log" 2>&1 &
MP=$!
for i in $(seq 1 60); do [ -p /tmp/psmac.req ] && break; sleep 0.5; done
timeout "$SECS" tools/run-psmac-qemu.sh "$@" > "$LOGDIR/emu68.log" 2>&1
wait $MP 2>/dev/null
echo "logs: $LOGDIR/mame.log $LOGDIR/emu68.log; snapshots: mame/snapFS/$MACHINE/"
