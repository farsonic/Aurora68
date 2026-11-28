#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
#
# configure-rootfs.sh ROOT HOSTNAME UUID HASHFILE [KEYFILE] - the Debian system's settings for Aurora68 (28 November
# 2026, docs/27-linux.md), written into the root file system at ROOT once debootstrap's second stage is done.
# Plain sh with no programs beyond those of Debian's minbase, so that it runs in the build container on the tree
# (build-in-container.sh) and also on the m68k system itself with ROOT=/ (how it was tried in the testbed).
#   HOSTNAME  the host name; UUID the ext4 file system's UUID (mkfs.ext4 -U); HASHFILE a file holding root's
#   password hash (openssl passwd -6), read and not shown; KEYFILE SSH public keys for root, optional.
set -eu
R=${1%/}
HOST=$2
UUID=$3
HASHFILE=$4
KEYS=${5:-}
[ -d "$R/etc" ] && [ -f "$R/etc/inittab" ] && [ -f "$R/etc/shadow" ] || { echo "configure-rootfs: $R/ is not a finished Debian tree" >&2; exit 1; }

echo "$HOST" > "$R/etc/hostname"
cat > "$R/etc/hosts" <<EOF
127.0.0.1	localhost
127.0.1.1	$HOST
::1		localhost ip6-localhost ip6-loopback
EOF

cat > "$R/etc/fstab" <<EOF
# Aurora68: the whole virtual SCSI disk is the root file system (no partition table). The kernel mounts it
# read-write (rw on its command line), so it is not checked at boot: fsck it from another system now and then.
UUID=$UUID  /     ext4   defaults,noatime,errors=remount-ro  0  0
proc  /proc  proc   defaults                        0  0
tmpfs /tmp   tmpfs  defaults,nosuid,nodev,size=16m  0  0
EOF

# getty on the screen (tty1) and on the serial console (the SCC's channel A, the SE's modem port); the other
# virtual consoles left out: each getty is memory a 68030 notices
sed -i -e 's/^\([2-6]:23:respawn:.*\)/#\1/' "$R/etc/inittab"
grep -q '^T0:' "$R/etc/inittab" || echo 'T0:23:respawn:/sbin/getty -L ttyS0 9600 vt100' >> "$R/etc/inittab"

mkdir -p "$R/etc/network"
cat > "$R/etc/network/interfaces" <<'EOF'
# Aurora68: the NuBus Ethernet card (mac.auxnet=on, Linux's mac8390) by DHCP
auto lo
iface lo inet loopback

auto eth0
iface eth0 inet dhcp
EOF
rm -f "$R/etc/resolv.conf"
: > "$R/etc/resolv.conf"                        # dhcpcd writes the name servers it is given

mkdir -p "$R/etc/apt/apt.conf.d"
cat > "$R/etc/apt/apt.conf.d/90aurora68" <<'EOF'
// a small system: what a package recommends is not installed with it
APT::Install-Recommends "false";
APT::Install-Suggests "false";
EOF

# root's password: the hash only, put in place with sed; the file is not printed
HASH=$(cat "$HASHFILE")
case "$HASH" in '$'*'$'*) ;; *) echo "configure-rootfs: $HASHFILE does not hold a password hash" >&2; exit 1 ;; esac
sed -i "s|^root:[^:]*:|root:$HASH:|" "$R/etc/shadow"
HASH=

mkdir -p "$R/etc/ssh/sshd_config.d"
if [ -n "$KEYS" ] && [ -s "$KEYS" ]; then
    mkdir -p "$R/root/.ssh"
    chmod 700 "$R/root/.ssh"
    cp "$KEYS" "$R/root/.ssh/authorized_keys"
    chmod 600 "$R/root/.ssh/authorized_keys"
    echo "# Aurora68: root logs in with the keys given at build time (PermitRootLogin's Debian default)" \
        > "$R/etc/ssh/sshd_config.d/aurora68.conf"
else
    cat > "$R/etc/ssh/sshd_config.d/aurora68.conf" <<'EOF'
# Aurora68: no other account exists, so root may log in with its password over SSH. Put a key in
# /root/.ssh/authorized_keys and delete this file (or set PermitRootLogin prohibit-password) to stop that.
PermitRootLogin yes
EOF
fi
echo "Aurora68 Debian m68k, configured $(date -u +%Y-%m-%d)" > "$R/etc/aurora68-release"
echo "configure-rootfs: $HOST, root on UUID=$UUID, getty on tty1 and ttyS0, eth0 by DHCP"
