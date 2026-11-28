#!/bin/bash
# SPDX-License-Identifier: MPL-2.0
#
# build-in-container.sh - the steps of tools/linux/build-debian-image.sh that run inside its privileged Debian
# container (28 November 2026, docs/27-linux.md). Not meant to be run by hand on a Mac: the host script starts it
# with this folder mounted at /aurora68, the output folder at /out, a cache volume at /cache, and the root
# password on standard input (one line). It can also be run on any Debian or Ubuntu machine as root, with the
# packages of the Dockerfile installed, as: printf '%s\n' "$PW" | sudo AURORA68_OUT=... ./build-in-container.sh
#
# 1. the Linux kernel: a pinned kernel.org release (checked against its SHA-256), the patches in patches/,
#    mac_defconfig with every module switched off, aurora68.config over it, built for m68k -> vmlinux
# 2. qemu-m68k registered with binfmt_misc (fix-binary flag, so the chroot needs no qemu of its own)
# 3. debootstrap --arch=m68k from Debian Ports, unstable: a small sysvinit system with openssh-server, ifupdown and
#    dhcpcd (systemd-standalone-sysusers instead of systemd for their "systemd | systemd-sysusers")
# 4. the system configured (configure-rootfs.sh): host name, fstab, getty on tty1 and on the serial console, eth0
#    by DHCP, apt's source, the root password (hashed here, never written in clear or logged)
# 5. debian.img: a sparse file of IMAGE_SIZE with ext4 on the whole disk, filled with mkfs.ext4 -d (no loop device)
set -euo pipefail

KERNEL_VERSION=${KERNEL_VERSION:-6.18.65}
KERNEL_SHA256=${KERNEL_SHA256:-c92606f956f09dc50b5a0e2af9e86763fce27ada1647de97f8d1ac8cfc95801d}
MIRROR=${MIRROR:-https://deb.debian.org/debian-ports}
SUITE=${SUITE:-unstable}
IMAGE_SIZE=${IMAGE_SIZE:-2G}
AURORA68_HOSTNAME=${AURORA68_HOSTNAME:-aurora68}
SRC=${AURORA68_SRC:-/aurora68}
OUT=${AURORA68_OUT:-/out}
CACHE=${AURORA68_CACHE:-/cache}
WORK=${AURORA68_WORK:-/work}
JOBS=${JOBS:-$(nproc)}
KEYRING=/usr/share/keyrings/debian-ports-archive-keyring.gpg
ROOTFS=$WORK/rootfs
PACKAGES=sysvinit-core,systemd-standalone-sysusers,debian-ports-archive-keyring,ca-certificates,openssh-server,ifupdown,dhcpcd-base,iproute2,netbase,procps,nano,less

say() { printf '\n==> %s\n' "$*"; }
die() { printf '\nERROR: %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "run as root (the container does)"
mkdir -p "$OUT" "$CACHE" "$WORK"

# the root password: one line on standard input, kept in a variable and never echoed
IFS= read -r ROOT_PW || true
[ -n "${ROOT_PW:-}" ] || die "no root password was given on standard input"
PW_HASH=$(printf '%s' "$ROOT_PW" | openssl passwd -6 -stdin)
unset ROOT_PW

# ---- 1. the kernel ----------------------------------------------------------------------------------------
say "Linux $KERNEL_VERSION for m68k (Macintosh IIci, 68030), with $SRC/aurora68.config"
TARBALL=$CACHE/linux-$KERNEL_VERSION.tar.xz
MAJOR=${KERNEL_VERSION%%.*}
if [ ! -s "$TARBALL" ]; then
    curl -fL --retry 3 -o "$TARBALL.part" "https://cdn.kernel.org/pub/linux/kernel/v$MAJOR.x/linux-$KERNEL_VERSION.tar.xz"
    mv "$TARBALL.part" "$TARBALL"
fi
echo "$KERNEL_SHA256  $TARBALL" | sha256sum -c - || die "linux-$KERNEL_VERSION.tar.xz does not have the SHA-256 this script pins"
KSRC=$WORK/linux-$KERNEL_VERSION
KOUT=$WORK/linux-build
if [ ! -d "$KSRC" ]; then
    tar -C "$WORK" -xf "$TARBALL"
    for p in "$SRC"/patches/*.patch; do
        [ -e "$p" ] || continue
        echo "patch: $(basename "$p")"
        patch -d "$KSRC" -p1 --forward < "$p"
    done
fi
mkdir -p "$KOUT"
KMAKE=(make -C "$KSRC" O="$KOUT" ARCH=m68k CROSS_COMPILE=m68k-linux-gnu-)
"${KMAKE[@]}" -s mac_defconfig
# everything mac_defconfig builds as a module would become built in once modules are off: off instead
sed -i 's/^\(CONFIG_[A-Za-z0-9_]*\)=m$/# \1 is not set/' "$KOUT/.config"
KCONFIG_CONFIG="$KOUT/.config" "$KSRC/scripts/kconfig/merge_config.sh" -m -O "$KOUT" "$KOUT/.config" "$SRC/aurora68.config" > /dev/null
"${KMAKE[@]}" -s olddefconfig
missing=""
for opt in M68030 MAC SCSI BLK_DEV_SD MAC_SCSI EXT4_FS MAC8390 ADB ADB_MACII INPUT_ADBHID FB_MAC FRAMEBUFFER_CONSOLE \
           SERIAL_PMACZILOG SERIAL_PMACZILOG_CONSOLE DEVTMPFS DEVTMPFS_MOUNT UNIX INET PACKET; do
    grep -q "^CONFIG_$opt=y" "$KOUT/.config" || missing="$missing $opt"
done
[ -z "$missing" ] || die "the kernel's configuration lacks:$missing"
"${KMAKE[@]}" -s -j"$JOBS" vmlinux
cp "$KOUT/vmlinux" "$OUT/vmlinux"
cp "$KOUT/.config" "$OUT/vmlinux.config"
cp "$KOUT/System.map" "$OUT/System.map"
echo "vmlinux: $(stat -c %s "$OUT/vmlinux") bytes"

# ---- 2. qemu-m68k through binfmt_misc -----------------------------------------------------------------------
say "qemu-m68k for the m68k programs of debootstrap's second stage"
QEMU=$(command -v qemu-m68k-static || command -v qemu-m68k || true)
[ -n "$QEMU" ] || die "no qemu-m68k-static in the container"
file -L "$QEMU" | grep -qE "statically linked|static-pie linked" || die "$QEMU is not statically linked: it could not run inside the chroot"
BFM=/proc/sys/fs/binfmt_misc
[ -e "$BFM/register" ] || mount -t binfmt_misc binfmt_misc "$BFM" || die "binfmt_misc cannot be mounted: is the container privileged (--privileged)?"
if [ -e "$BFM/qemu-m68k" ]; then
    if grep -q '^enabled' "$BFM/qemu-m68k" && grep -q '^flags:.*F' "$BFM/qemu-m68k"; then
        echo "qemu-m68k is registered already, with the fix-binary flag: $(grep '^interpreter' "$BFM/qemu-m68k")"
    else
        echo "qemu-m68k is registered without the fix-binary flag: registered again with this container's"
        echo -1 > "$BFM/qemu-m68k"
    fi
fi
if [ ! -e "$BFM/qemu-m68k" ]; then
    printf '%s' ':qemu-m68k:M::\x7fELF\x01\x02\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x04:\xff\xff\xff\xff\xff\xff\xfe\x00\xff\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff:'"$QEMU"':F' > "$BFM/register"
    echo "registered: $QEMU (flags F)"
fi

# ---- 3. debootstrap ---------------------------------------------------------------------------------------
say "Debian $SUITE for m68k from $MIRROR"
if [ -e "$ROOTFS" ]; then
    mv "$ROOTFS" "$WORK/rootfs.old.$$"            # a run that stopped half way: start again beside it
fi
mkdir -p "$ROOTFS" "$CACHE/debs"
# systemd-standalone-sysusers satisfies the "systemd | systemd-sysusers" of openssh-server, ifupdown and dhcpcd
# without systemd. debootstrap takes the first alternative of a dependency and adds systemd to its list, which
# the standalone sysusers conflicts with, so systemd (and its private library) leave the list between the stages.
debootstrap --foreign --arch=m68k --variant=minbase --keyring="$KEYRING" --cache-dir="$CACHE/debs" \
    --include="$PACKAGES" "$SUITE" "$ROOTFS" "$MIRROR"
for f in required base; do
    tr ' ' '\n' < "$ROOTFS/debootstrap/$f" | grep -v -x -e systemd -e libsystemd-shared -e systemd-sysv | tr '\n' ' ' \
        > "$ROOTFS/debootstrap/$f.new"
    mv "$ROOTFS/debootstrap/$f.new" "$ROOTFS/debootstrap/$f"
done
chroot "$ROOTFS" /bin/true || die "m68k programs do not run in the chroot (binfmt_misc and qemu-m68k)"
say "debootstrap's second stage, under qemu-m68k (slow: tens of minutes)"
chroot "$ROOTFS" /debootstrap/debootstrap --second-stage
chroot "$ROOTFS" sh -c 'if dpkg -s systemd > /dev/null 2>&1; then echo "systemd was installed after all" >&2; exit 1; fi'
echo "deb $MIRROR $SUITE main" > "$ROOTFS/etc/apt/sources.list"

# ---- 4. the configuration ---------------------------------------------------------------------------------
say "configuration: $AURORA68_HOSTNAME, fstab, getty, eth0 by DHCP, the root password"
ROOT_UUID=$(cat /proc/sys/kernel/random/uuid)
HASHFILE=$(mktemp)
printf '%s' "$PW_HASH" > "$HASHFILE"
unset PW_HASH
sh "$SRC/configure-rootfs.sh" "$ROOTFS" "$AURORA68_HOSTNAME" "$ROOT_UUID" "$HASHFILE" "${AURORA68_KEYS:-}"
rm -f "$HASHFILE"
echo "built with Linux $KERNEL_VERSION" >> "$ROOTFS/etc/aurora68-release"
# the SSH host keys made during the second stage are this image's own; a copy of debian.img shares them

# ---- 5. the disk image ------------------------------------------------------------------------------------
say "debian.img: $IMAGE_SIZE, ext4 on the whole disk"
rm -f "$OUT/debian.img"
truncate -s "$IMAGE_SIZE" "$OUT/debian.img"
mkfs.ext4 -q -F -L aurora68-root -U "$ROOT_UUID" -d "$ROOTFS" "$OUT/debian.img"
echo "debian.img: $(du -h "$OUT/debian.img" | cut -f1) used of $(du -h --apparent-size "$OUT/debian.img" | cut -f1)"
say "done: $OUT/vmlinux, $OUT/debian.img"
