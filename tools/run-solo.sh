#!/bin/bash
# The solo board in QEMU: no MAME, the kernel is the logic board (mac.board=solo). The disk image and
# the declaration ROM come from host files through semihosting; a screenshot of the HDMI screen is
# taken through the QEMU monitor every SHOT seconds and at the end.
# usage: tools/run-solo.sh [seconds] [extra Emu68 cmdline tokens]
#   env: HD (default images/sys710pi.image), ROM, DECLROM, LOGDIR, PSMAC_RAM (MiB, default 16; empty = the kernel's default),
#        SHOT (s, default 0), CONTAINER (a cpio with ROM, declrom and image: the hardware's way, no semihosting)
set -e
cd "$(dirname "$0")/.."
SECS=${1:-240}; shift || true
LOGDIR=${LOGDIR:-/tmp/psmac-solo}; mkdir -p "$LOGDIR"
IMG=${IMG:-psmac-build/Emu68.img}
DTB=${DTB:-firmware/bcm2710-rpi-3-b.dtb}
ROM=${ROM:-rom/original/MacIIci_368CADFE.rom}
HD=${HD:-images/sys710pi.image}
DECLROM=${DECLROM:-mac/build/psmac.declrom}
RAM=${PSMAC_RAM-16}
SHOT=${SHOT:-0}
# CONTAINER=file: boot from a container (ROM, declrom, disk image inside: the hardware's way) instead of host files
if [ -n "$CONTAINER" ]; then INITRD="$CONTAINER"; FILES=""; else INITRD="$ROM"; FILES="mac.semihost=1 mac.declrom=$DECLROM mac.scsi0=$HD"; fi
[ -n "$RAM" ] && RAMTOK="mac.ram=$RAM" || RAMTOK=""
rm -f /tmp/qemu-mon.sock
qemu-system-aarch64 -M raspi3b -smp 4 -m 1G -display none -monitor unix:/tmp/qemu-mon.sock,server,nowait \
  -kernel "$IMG" -dtb "$DTB" -initrd "$INITRD" \
  -append "$RAMTOK enable_cache debug mac.board=solo $FILES $*" \
  -serial stdio -semihosting-config enable=on,target=native -accel tcg,tb-size=256 > "$LOGDIR/emu68.log" 2>&1 &
QP=$!
n=0; t=0
while kill -0 $QP 2>/dev/null && [ $t -lt $SECS ]; do
  sleep 5; t=$((t+5))
  if [ "$SHOT" -gt 0 ] && [ $((t % SHOT)) -eq 0 ]; then n=$((n+1)); python3 tools/qemu-shot.py "$LOGDIR/shot-$(printf %03d $n).png" >/dev/null 2>&1 || true; fi
done
python3 tools/qemu-shot.py "$LOGDIR/final.png" 2>/dev/null || true
kill $QP 2>/dev/null; wait $QP 2>/dev/null || true
echo "log: $LOGDIR/emu68.log; screen: $LOGDIR/final.png"
