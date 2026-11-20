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
# MAME_RAM=2560K (1M, 2M, 4M) gives the MAME SE that much of its own RAM, with mac.seram=2.5 (1, 2, 4)
# on the kernel side; unset, MAME's default (4M on macse_ps)
reference/mame/mamepsmac $MACHINE ${MAME_RAM:+-ramsize $MAME_RAM} -rompath mame/roms -cfg_directory mame/cfgFS -nvram_directory mame/nvramFS \
  -video none -sound none -nothrottle -frameskip 8 -hard1 ${HD:-images/chd/hd20.chd} -snapshot_directory mame/snapFS \
  -verbose -seconds_to_run $((SECS+20)) -autoboot_script ${MAME_SCRIPT:-mame/shots.lua} -skip_gameinfo > "$LOGDIR/mame.log" 2>&1 &
MP=$!
for i in $(seq 1 60); do [ -p /tmp/psmac.req ] && break; sleep 0.5; done
timeout "$SECS" tools/run-psmac-qemu.sh "$@" > "$LOGDIR/emu68.log" 2>&1
wait $MP 2>/dev/null
echo "logs: $LOGDIR/mame.log $LOGDIR/emu68.log; snapshots: mame/snapFS/$MACHINE/"
