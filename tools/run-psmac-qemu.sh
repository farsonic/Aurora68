#!/bin/bash
# Run the psmac Emu68 build under QEMU's Raspberry Pi 3 model, with the IIci ROM as initrd and the
# bridge transport (mini UART = second serial) connected to the MAME bridge CPU socket.
# usage: tools/run-psmac-qemu.sh [extra cmdline tokens]   env: PSMAC_BRIDGE_SOCK, PSMAC_RAM (MiB), ROM
set -e
cd "$(dirname "$0")/.."
IMG=${IMG:-psmac-build/Emu68.img}
DTB=${DTB:-$(find psmac-build emu68-build emu68 -name 'bcm2710-rpi-3-b.dtb' 2>/dev/null | head -1)}
ROM=${ROM:-rom/original/MacIIci_368CADFE.rom}
SOCK=${PSMAC_BRIDGE_SOCK:-/tmp/psmac-bridge.sock}
RAM=${PSMAC_RAM:-8}
[ -f "$IMG" ] || { echo "missing $IMG (build the psmac variant first)"; exit 1; }
[ -f "$DTB" ] || { echo "missing device tree blob"; exit 1; }
if [ -S "$SOCK" ]; then BRIDGE="-chardev socket,id=bridge,path=$SOCK -serial chardev:bridge"; else echo "note: $SOCK not present, bridge transport disconnected"; BRIDGE="-serial null"; fi
exec qemu-system-aarch64 -M raspi3b -smp 4 -m 1G -display none \
  -kernel "$IMG" -dtb "$DTB" -initrd "$ROM" \
  -append "mac.ram=$RAM enable_cache debug $*" \
  -serial mon:stdio $BRIDGE -accel tcg,tb-size=256
