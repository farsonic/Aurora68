#!/bin/bash
# SPDX-License-Identifier: MPL-2.0
#
# boot.sh: boot a disk image on the solo board in QEMU (the Pi alone, mac.board=solo), with a USB
# keyboard and mouse to drive it (keys.py, mouse.py), screenshots (shot.sh) and optionally the
# host network peers (pinet).
#
# usage: dev/tools/testbed/boot.sh [options] IMAGE [kernel options...]
#   --secs N         how long the run lasts (default 600); stop it sooner with stop.sh
#   --name NAME      the run's folder, $SCRATCH/runs/NAME (default: the image's name and the time)
#   --screen WxH     the Pi's screen and the matching declaration ROM, mac/build/psmac-WxH.declrom
#                    (default 1024x768; 640x480 takes the plain psmac.declrom)
#   --declrom FILE   another declaration ROM (e.g. a test build's)
#   --ram MB         the Mac's memory (default 32)
#   --kernel FILE    default pistorm-se/psmac-build/Emu68.img (dev/tools/buildall.sh kernel)
#   --rom FILE       default pistorm-se/rom/original/MacIIci_368CADFE.rom (yours to supply)
#   --net            the wired card on tools/pinet.py (a peer at 192.168.68.1 answering ARP, ping,
#                    UDP echo, DHCP; PINET_* variables pass through, e.g. PINET_INTERNET=1,
#                    PINET_MDNS=macse, PINET_ATALK="Lagoon Test")
#   --wifi           the simulated WiFi radio on a second pinet peer (192.168.69.1), joined to
#                    the network "Aurora68" (mac.wifisim=, wifi.ssid=, wifi.psk= for the test
#                    network only; never a real password here)
#   --card FILE      an SD card image for the card code (mkcard.sh), e.g. for mac.image= runs
#   --container FILE boot from a container (cpio) instead of host files, as the hardware does
#   --shot N         a screenshot every N seconds into the run's folder (default 0: only the last)
#   --no-input       no USB keyboard and mouse
#   --bg             return once QEMU is up (the run goes on for --secs; the log is
#                    RUN/emu68.log); without it boot.sh waits for the end
# Kernel options go after the image: e.g. mac.inputtest=320 mac.peek=16a,4 (docs/09).
# One QEMU at a time: the monitor socket /tmp/qemu-mon.sock and pinet's /tmp/psmac-*.rx/tx are
# shared; boot.sh takes the lock $SCRATCH/qemu.lock and refuses if a QEMU is running.
set -uo pipefail
. "$(dirname "$0")/../lib.sh"

SECS=600 NAME= SCREEN=1024x768 DECLROM= RAM=32 KERNEL=$AURORA68/psmac-build/Emu68.img
ROM=$AURORA68/rom/original/MacIIci_368CADFE.rom NET=0 WIFI=0 CARD= CONTAINER= SHOT=0 INPUT=1 BG=0 IMAGE=
while [ $# -gt 0 ]; do
	case "$1" in
	--secs) SECS=$2; shift ;;
	--name) NAME=$2; shift ;;
	--screen) SCREEN=$2; shift ;;
	--declrom) DECLROM=$2; shift ;;
	--ram) RAM=$2; shift ;;
	--kernel) KERNEL=$2; shift ;;
	--rom) ROM=$2; shift ;;
	--net) NET=1 ;;
	--wifi) WIFI=1 ;;
	--card) CARD=$2; shift ;;
	--container) CONTAINER=$2; shift ;;
	--shot) SHOT=$2; shift ;;
	--no-input) INPUT=0 ;;
	--bg) BG=1 ;;
	-h|--help) sed -n '4,36p' "$0"; exit 0 ;;
	--*) die "unknown option $1 (--help)" ;;
	*) IMAGE=$1; shift; break ;;
	esac
	shift
done
[ -n "$IMAGE" ] || [ -n "$CONTAINER" ] || die "which disk image? (--help)"
abs() { [ -n "$1" ] && echo "$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"; }
[ -n "$IMAGE" ] && { [ -f "$IMAGE" ] || die "no image $IMAGE"; IMAGE=$(abs "$IMAGE"); }
[ -f "$KERNEL" ] || die "no kernel $KERNEL (dev/tools/buildall.sh kernel)"
[ -n "$CONTAINER" ] || [ -f "$ROM" ] || die "no ROM $ROM: put your IIci ROM there (it is not in any repository)"
if [ -z "$DECLROM" ]; then
	[ "$SCREEN" = 640x480 ] && DECLROM=$AURORA68/mac/build/psmac.declrom || DECLROM=$AURORA68/mac/build/psmac-$SCREEN.declrom
fi
[ -f "$DECLROM" ] || die "no declaration ROM $DECLROM (sizes: ls $AURORA68/mac/build/*.declrom)"
NAME=${NAME:-$(basename "${IMAGE:-$CONTAINER}" | sed 's/\.[^.]*$//')-$(date +%H%M%S)}
RUN=$SCRATCH/runs/$NAME; mkdir -p "$RUN"

if [ $BG = 1 ]; then
	# again in the background, its output in RUN/boot.out; back once the monitor answers
	args=(); for a in --secs "$SECS" --name "$NAME" --screen "$SCREEN" --declrom "$DECLROM" --ram "$RAM" --kernel "$KERNEL" --rom "$ROM" --shot "$SHOT"; do args+=("$a"); done
	[ $NET = 1 ] && args+=(--net); [ $WIFI = 1 ] && args+=(--wifi); [ $INPUT = 0 ] && args+=(--no-input)
	[ -n "$CARD" ] && args+=(--card "$(abs "$CARD")"); [ -n "$CONTAINER" ] && args+=(--container "$(abs "$CONTAINER")")
	nohup "$0" "${args[@]}" ${IMAGE:+"$IMAGE"} "$@" > "$RUN/boot.out" 2>&1 &
	for _ in $(seq 1 60); do
		[ -S /tmp/qemu-mon.sock ] && [ -s "$RUN/emu68.log" ] && { echo "running: $RUN (log $RUN/emu68.log; stop: $(dirname "$0")/stop.sh $NAME)"; exit 0; }
		kill -0 $! 2>/dev/null || { cat "$RUN/boot.out"; exit 1; }
		sleep 1
	done
	die "QEMU did not come up: $RUN/boot.out"
fi

qemu_lock
PIDS=
cleanup() { for p in $PIDS; do kill "$p" 2>/dev/null; done; qemu_unlock; }
trap cleanup EXIT
trap 'exit 130' INT TERM

OPTS=()
if [ $NET = 1 ]; then
	rm -f /tmp/psmac-net.tx /tmp/psmac-net.rx
	python3 "$AURORA68/tools/pinet.py" /tmp/psmac-net peer > "$RUN/pinet-wired.log" 2>&1 & PIDS="$PIDS $!"
	OPTS+=(mac.net=/tmp/psmac-net)
fi
if [ $WIFI = 1 ]; then
	rm -f /tmp/psmac-wifi.tx /tmp/psmac-wifi.rx
	PINET_NET=69 python3 "$AURORA68/tools/pinet.py" /tmp/psmac-wifi peer > "$RUN/pinet-wifi.log" 2>&1 & PIDS="$PIDS $!"
	# the testbed's own radio and network: the name and password exist only here
	OPTS+=(mac.wifisim=Aurora68:pistorm68 wifi.ssid=Aurora68 wifi.psk=pistorm68 mac.wifinet=/tmp/psmac-wifi)
else
	OPTS+=(wifi=off)     # QEMU has no WiFi chip: keep the driver from looking for one
fi
[ "$SCREEN" != 640x480 ] && OPTS+=(mac.screen=$SCREEN)
QX=; [ $INPUT = 1 ] && QX="-device usb-kbd -device usb-mouse"

echo "boot: ${IMAGE:-$CONTAINER} on the solo board, ${SCREEN}, ${RAM} MB, ${SECS} s; run folder $RUN"
export LOGDIR=$RUN IMG=$KERNEL ROM DECLROM HD=$IMAGE PSMAC_RAM=$RAM SHOT QEMU_EXTRA="${QEMU_EXTRA:-} $QX"
[ -n "$CARD" ] && export CARD
[ -n "$CONTAINER" ] && export CONTAINER
echo "$$" > "$RUN/boot.pid"
"$AURORA68/tools/run-solo.sh" "$SECS" ${OPTS[@]+"${OPTS[@]}"} "$@"
echo "done: $RUN/emu68.log, $RUN/final.png"
