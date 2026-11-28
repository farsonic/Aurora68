#!/bin/bash
# SPDX-License-Identifier: MPL-2.0
#
# build-debian-image.sh - Debian m68k for Aurora68, built on a Mac (28 November 2026, docs/27-linux.md).
#
# Needs only a Docker-compatible engine: OrbStack (its docker command) or Docker Desktop, on Apple Silicon or Intel;
# written for the bash 3.2 that macOS has, and nothing else from the Mac (no Python, no Homebrew).
# Everything else happens in a privileged Debian container (tools/linux/Dockerfile): the Linux kernel cross-built
# from a pinned kernel.org release with tools/linux/aurora68.config, Debian unstable for m68k from Debian Ports by
# debootstrap (its second stage under qemu-m68k through binfmt_misc), the system configured, and an ext4 disk image.
#
#   tools/linux/build-debian-image.sh [OUTPUT_FOLDER]          (default: ./aurora68-debian)
#
# Environment (all optional):
#   IMAGE_SIZE          the disk image's size, e.g. 2G (default), 3500M; at most 4095M (the SD card's FAT32 limit)
#   AURORA68_HOSTNAME   the system's host name (default aurora68)
#   ROOT_PASSWORD       root's password; when unset it is asked for (twice, not shown). It is passed to the
#                       container on standard input, hashed there, and never written in clear or logged
#   AUTHORIZED_KEYS     a file of SSH public keys for root (then root logs in over SSH with a key only)
#   JOBS                parallel jobs for the kernel build (default: the container's processors)
#
# Output: vmlinux (the kernel), debian.img (sparse), README.txt (what to copy where and the cmdline.txt line),
# and vmlinux.config / System.map for reference. A cache volume (aurora68-linux-cache) keeps the kernel's source
# tarball and Debian's packages between runs.
#
# Without Docker, the same steps run in an OrbStack Linux machine (orb create debian aurora68-build): as root there,
# install the packages the Dockerfile lists, then
#   printf '%s\n' "$ROOT_PASSWORD" | AURORA68_SRC=$PWD/tools/linux AURORA68_OUT=$HOME/aurora68-debian \
#       AURORA68_CACHE=/var/cache/aurora68 AURORA68_WORK=/var/tmp/aurora68 bash tools/linux/build-in-container.sh
# (the work folder must be on the machine's own disk, not on a folder shared with macOS: the root file system
# needs root's ownership and device nodes). The Docker way is the one tested and meant.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$PWD/aurora68-debian}
IMAGE_SIZE=${IMAGE_SIZE:-2G}
AURORA68_HOSTNAME=${AURORA68_HOSTNAME:-aurora68}
BUILDER=aurora68-linux-builder
CACHE_VOLUME=aurora68-linux-cache

die() { printf '\nERROR: %s\n' "$*" >&2; exit 1; }

# ---- the engine ----------------------------------------------------------------------------------------------
command -v docker > /dev/null 2>&1 || die "no 'docker' command. Install OrbStack (https://orbstack.dev) or Docker Desktop, start it, and run this again."
if ! docker info > /dev/null 2>&1; then
    die "docker is installed but its engine does not answer. Start OrbStack (or Docker Desktop) first, wait until it says it is running, and run this again."
fi
echo "engine: $(docker version --format '{{.Server.Os}}/{{.Server.Arch}} {{.Server.Version}}' 2>/dev/null || echo unknown)"

# ---- the options ---------------------------------------------------------------------------------------------
size_num=${IMAGE_SIZE%[GgMm]}
case "$size_num:$IMAGE_SIZE" in
    :*|*[!0-9]*:*) size_mb=0 ;;
    *[Gg]) size_mb=$(( size_num * 1024 )) ;;
    *[Mm]) size_mb=$size_num ;;
    *) size_mb=0 ;;
esac
[ "$size_mb" -ge 512 ] || die "IMAGE_SIZE=$IMAGE_SIZE: a whole number of M or G, at least 512M (2G is the default)"
[ "$size_mb" -le 4095 ] || die "IMAGE_SIZE=$IMAGE_SIZE: a file on the SD card's FAT32 partition can be at most 4095M"
case "$AURORA68_HOSTNAME" in
    ''|*[!A-Za-z0-9-]*|-*) die "AURORA68_HOSTNAME=$AURORA68_HOSTNAME: letters, digits and '-' only" ;;
esac
KEYS_ARGS=()
if [ -n "${AUTHORIZED_KEYS:-}" ]; then
    [ -s "$AUTHORIZED_KEYS" ] || die "AUTHORIZED_KEYS=$AUTHORIZED_KEYS is not a file with keys in it"
    KEYS_ARGS=(-v "$(cd "$(dirname "$AUTHORIZED_KEYS")" && pwd)/$(basename "$AUTHORIZED_KEYS"):/keys/authorized_keys:ro"
               -e AURORA68_KEYS=/keys/authorized_keys)
fi
if [ -z "${ROOT_PASSWORD:-}" ]; then
    [ -t 0 ] || die "no ROOT_PASSWORD in the environment and no terminal to ask on"
    while :; do
        read -r -s -p "Root password for the Debian system: " pw1; echo
        read -r -s -p "The same again: " pw2; echo
        [ -n "$pw1" ] && [ "$pw1" = "$pw2" ] && break
        echo "They differ, or are empty: again."
    done
    ROOT_PASSWORD=$pw1
    unset pw1 pw2
fi
case "$ROOT_PASSWORD" in *$'\n'*) die "the root password cannot contain a line break" ;; esac

mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
echo "output: $OUT (image $IMAGE_SIZE, host name $AURORA68_HOSTNAME)"

# ---- the container -------------------------------------------------------------------------------------------
echo "building the container image $BUILDER (the first time a few minutes, then from Docker's cache)"
docker build -t "$BUILDER" "$HERE"
docker volume create "$CACHE_VOLUME" > /dev/null

echo "starting the build: the kernel (minutes), then Debian's second stage under emulation (tens of minutes)"
printf '%s\n' "$ROOT_PASSWORD" | docker run --rm -i --privileged \
    -v "$HERE":/aurora68:ro -v "$OUT":/out -v "$CACHE_VOLUME":/cache ${KEYS_ARGS[@]+"${KEYS_ARGS[@]}"} \
    -e IMAGE_SIZE="$IMAGE_SIZE" -e AURORA68_HOSTNAME="$AURORA68_HOSTNAME" ${JOBS:+-e JOBS="$JOBS"} \
    "$BUILDER" bash /aurora68/build-in-container.sh
unset ROOT_PASSWORD

# ---- what to do with it --------------------------------------------------------------------------------------
cat > "$OUT/README.txt" <<EOF
Aurora68: Debian m68k (unstable, Debian Ports) with Linux, built $(date +%Y-%m-%d) by tools/linux/build-debian-image.sh
=============================================================================================================

The files
  vmlinux        the Linux kernel (an m68k ELF file), started by Aurora68's kernel instead of the Macintosh ROM
  debian.img     the root disk: ext4 on the whole disk, $IMAGE_SIZE (a sparse file: it takes less space here than that)
  vmlinux.config, System.map   for reference only; they do not go on the card

1. Copy vmlinux and debian.img to the root of the SD card's FAT partition, beside kernel8.img and cmdline.txt.
   (debian.img is a sparse file here; on the card it takes its full size.)

2. In cmdline.txt (one line), keep your board, network and other options and add:

     mac.mmu=full mac.linux=vmlinux scsi.disk=debian.img:1 mac.auxnet=on mac.linuxargs="root=/dev/sdb rw console=tty0 console=ttyS0"

   For an SE whose own hard disk is at SCSI ID 0 the Debian disk is ID 1, and Linux names the disks by ID: the
   SE's disk is sda and Debian's sdb. Without an internal disk use scsi.disk=debian.img:0 and root=/dev/sda.
   A whole line for the SE, with your own values in place of the capitals:

     enable_cache mac.board=se mac.mmu=full mac.linux=vmlinux scsi.disk=debian.img:1 mac.auxnet=on mac.linuxargs="root=/dev/sdb rw console=tty0 console=ttyS0" eth=on bt=on wifi.country=XX wifi.ssid="YOURSSID" wifi.psk=YOURPASSWORD mac.logsave=10

   mac.linuxscreen=hdmi puts Linux's console on the HDMI screen (with mac.hdmi=screen) instead of the SE's.
   Take mac.linux= out again and the Macintosh starts as before.

3. Log in as root on the SE's screen (an ADB keyboard, or a USB or Bluetooth one through Aurora68), on the
   serial console (the SE's modem port, 9600 baud), or over SSH: the system asks DHCP for its address, which
   'ip addr' shows. Host name: $AURORA68_HOSTNAME.

What is in it: sysvinit (not systemd), openssh-server, ifupdown with dhcpcd, ca-certificates, nano, iproute2.
More with 'apt install' (slowly: a 68030 at work). The kernel has everything built in and uses no modules.
To build again with other settings: tools/linux/build-debian-image.sh again (the cache keeps the downloads).
EOF
echo
echo "done. In $OUT:"
ls -lh "$OUT"
echo "README.txt says what to copy to the SD card and the cmdline.txt line."
