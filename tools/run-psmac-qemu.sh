#!/bin/bash
# Run the psmac Emu68 build under QEMU's Raspberry Pi 3 model, with the IIci ROM as initrd and the
# bridge transport (mini UART = second serial) connected to the MAME bridge CPU socket.
# usage: tools/run-psmac-qemu.sh [extra cmdline tokens]   env: PSMAC_BRIDGE_SOCK, PSMAC_RAM (MiB), ROM
set -e
cd "$(dirname "$0")/.."
IMG=${IMG:-psmac-build/Emu68.img}
DTB=${DTB:-firmware/bcm2710-rpi-3-b.dtb}
ROM=${ROM:-rom/original/MacIIci_368CADFE.rom}
SOCK=${PSMAC_BRIDGE_SOCK:-/tmp/psmac-bridge.sock}
RAM=${PSMAC_RAM:-8}
[ -f "$IMG" ] || { echo "missing $IMG (build the psmac variant first)"; exit 1; }
[ -f "$DTB" ] || { echo "missing device tree blob"; exit 1; }
if [ "${PSMAC_TRANSPORT:-semihost}" = semihost ]; then
  # semihosting FIFO transport: MAME must run with PSMAC_BRIDGE_FIFO=/tmp/psmac.req:/tmp/psmac.rep
  [ -p /tmp/psmac.req ] || echo "note: /tmp/psmac.req missing (start the MAME bridge with PSMAC_BRIDGE_FIFO first)"
  BRIDGE="-serial null -semihosting-config enable=on,target=native"; EXTRA="mac.bridge=semihost"
elif [ -S "$SOCK" ]; then BRIDGE="-chardev socket,id=bridge,path=$SOCK -serial chardev:bridge"; EXTRA=""
else echo "note: $SOCK not present, bridge transport disconnected"; BRIDGE="-serial null"; EXTRA=""; fi
exec qemu-system-aarch64 -M raspi3b -smp 4 -m 1G -display none -monitor unix:/tmp/qemu-mon.sock,server,nowait \
  -kernel "$IMG" -dtb "$DTB" -initrd "$ROM" \
  -append "mac.ram=$RAM enable_cache debug $EXTRA $*" \
  -serial stdio $BRIDGE -accel tcg,tb-size=256
