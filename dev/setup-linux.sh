#!/bin/bash
# SPDX-License-Identifier: MPL-2.0
#
# setup-linux.sh - the build environment of the Aurora68 workspace (Aurora68, Riptide, Lagoon,
# Quill/Octavo) on Ubuntu 24.04: in the container (dev/Dockerfile runs this), in a VM, or on a
# plain Ubuntu machine.
#
# usage: dev/setup-linux.sh [options]          (as root, or with sudo available)
#   (no options)        the base: every package the builds and the QEMU testbed need, and the
#                       big-endian stubs header Emu68 needs
#   --check             install nothing; say what is there and what is missing
#   --servers           Samba too, for Lagoon's SMB integration tests (they skip without it)
#   --mame-deps         the libraries a MAME build needs, and chdman (mame-tools)
#   --mame DIR          build MAME 0.264 with Aurora68's patch in DIR/reference/mame, where DIR is
#                       the Aurora68 checkout (about 25 minutes on four cores; implies --mame-deps)
#   --amiga [PREFIX]    vbcc, vasm, vlink and the NDK 3.2 headers for the Amiga builds, in PREFIX
#                       (default /opt/amiga). The MUI SDK is not fetched: see dev/README.md
#   --claude            Claude Code itself, with Anthropic's installer (in the container)
#
# Every package named below is one installed on the machine the work was done on (Ubuntu 24.04,
# 19 November 2026: `dpkg -l`), not a list from memory. Running it again is harmless: apt skips
# what is there, and the header is only made when it is missing.
set -euo pipefail

CHECK=0 SERVERS=0 MAMEDEPS=0 MAMEDIR= AMIGA= CLAUDE=0
while [ $# -gt 0 ]; do
	case "$1" in
	--check) CHECK=1 ;;
	--servers) SERVERS=1 ;;
	--mame-deps) MAMEDEPS=1 ;;
	--mame) MAMEDEPS=1; MAMEDIR=${2:?--mame needs the Aurora68 checkout}; shift ;;
	--amiga) AMIGA=/opt/amiga; case "${2:-}" in /*) AMIGA=$2; shift ;; esac ;;
	--claude) CLAUDE=1 ;;
	-h|--help) sed -n '4,22p' "$0"; exit 0 ;;
	*) echo "setup-linux.sh: unknown option $1 (--help)" >&2; exit 2 ;;
	esac
	shift
done

SUDO=; [ "$(id -u)" -eq 0 ] || SUDO=sudo
ARCH=$(dpkg --print-architecture)

# ---- the packages ---------------------------------------------------------------------------
# Native compiler and make: Quill's native tests, Lagoon's host tests, `make -C mac check`.
BASE="build-essential make cmake git gh curl ca-certificates file gzip xz-utils bzip2 zip unzip
      python3 python3-pil xxd device-tree-compiler procps netcat-openbsd less sudo"
# Not on the build machine, added for the bench: reaching the Pi on the LAN from the container.
BASE="$BASE iproute2 iputils-ping openssh-client"
# Emu68 is big-endian AArch64 (the kernel), with a PowerPC ROM sub-build. On an x86 machine both
# are cross compilers; on an ARM64 one (a container on Apple Silicon) the native gcc is already
# aarch64-linux-gnu-gcc and takes -mbig-endian. The PowerPC sub-build is a C and C++ CMake
# project, so it wants a C++ compiler too: g++-powerpc-linux-gnu (the build machine had none and
# CMake quietly took clang++; a container without either stops at "No CMAKE_CXX_COMPILER").
if [ "$ARCH" = amd64 ]; then
	KERNEL="gcc-aarch64-linux-gnu g++-aarch64-linux-gnu gcc-powerpc-linux-gnu g++-powerpc-linux-gnu"
else
	KERNEL="gcc g++ gcc-powerpc-linux-gnu g++-powerpc-linux-gnu"
fi
# The Macintosh side: GNU as for Aurora68's mac/, gcc for mac/c (PiLog, PiDepth) and for Riptide's,
# Lagoon's and Octavo's Mac builds, which link /usr/lib/gcc-cross/m68k-linux-gnu/13/libgcc.a.
MAC="binutils-m68k-linux-gnu gcc-m68k-linux-gnu"
# The testbed and the disk images: QEMU's raspi3b, hfsutils, FAT tools for SD card images.
TESTBED="qemu-system-arm qemu-utils hfsutils mtools dosfstools"
PKGS="$BASE $KERNEL $MAC $TESTBED"
[ $SERVERS = 1 ] && PKGS="$PKGS samba"
# docs/06-mame-bridge.md's list, plus chdman from Ubuntu's mame-tools (the same 0.264)
[ $MAMEDEPS = 1 ] && PKGS="$PKGS g++ libsdl2-dev libsdl2-ttf-dev libfontconfig-dev qtbase5-dev
	libxinerama-dev libxi-dev libxrandr-dev libxcursor-dev libasound2-dev libpulse-dev libgl1-mesa-dev
	libutf8proc-dev libflac-dev libjpeg-dev libportmidi-dev libpugixml-dev rapidjson-dev mame-tools"
[ -n "$AMIGA" ] && PKGS="$PKGS lhasa"

missing() { local p out=; for p in $PKGS; do dpkg -s "$p" >/dev/null 2>&1 || out="$out $p"; done; echo $out; }

# where the aarch64 compiler looks for glibc's headers: Debian's cross toolchain has no big-endian
# multilib, and <gnu/stubs.h> asks for stubs-lp64_be.h when __AARCH64EB__ is set (Aurora68's
# CLAUDE.md, "Prerequisites"). Only headers are needed: the kernel links with -nostdlib.
if [ "$ARCH" = amd64 ]; then STUBS=/usr/aarch64-linux-gnu/include/gnu; else STUBS=/usr/include/aarch64-linux-gnu/gnu; fi

if [ $CHECK = 1 ]; then
	m=$(missing)
	echo "architecture: $ARCH"
	[ -z "$m" ] && echo "packages: all there" || echo "packages missing:$m"
	[ -f $STUBS/stubs-lp64_be.h ] && echo "stubs-lp64_be.h: there" || echo "stubs-lp64_be.h: missing ($STUBS)"
	for t in aarch64-linux-gnu-gcc powerpc-linux-gnu-gcc m68k-linux-gnu-as m68k-linux-gnu-gcc cmake dtc xxd \
	         python3 hmount hcopy mcopy qemu-system-aarch64 gh; do
		printf '  %-22s %s\n' $t "$(command -v $t || echo MISSING)"
	done
	command -v m68k-linux-gnu-gcc >/dev/null && echo "  libgcc (m68k)          $(m68k-linux-gnu-gcc -m68020 -print-libgcc-file-name)"
	command -v qemu-system-aarch64 >/dev/null && echo "  $(qemu-system-aarch64 --version | head -1)"
	[ -n "$AMIGA" ] && for t in vc vbccm68k vasmm68k_mot vlink; do printf '  %-22s %s\n' $t "$(ls $AMIGA/vbcc/bin/$t 2>/dev/null || echo MISSING)"; done
	exit 0
fi

m=$(missing)
if [ -n "$m" ]; then
	echo "== installing:$m"
	$SUDO apt-get update
	DEBIAN_FRONTEND=noninteractive $SUDO apt-get install -y --no-install-recommends $m
else
	echo "== packages: all there"
fi

if [ ! -f $STUBS/stubs-lp64_be.h ]; then
	echo "== $STUBS/stubs-lp64_be.h (big-endian AArch64 headers)"
	$SUDO mkdir -p $STUBS
	if [ -f $STUBS/stubs-lp64.h ]; then $SUDO cp $STUBS/stubs-lp64.h $STUBS/stubs-lp64_be.h; else $SUDO touch $STUBS/stubs-lp64_be.h; fi
fi

# a bind-mounted workspace belongs to another user id: let git work there all the same
git config --global --get-all safe.directory 2>/dev/null | grep -qx '\*' || git config --global --add safe.directory '*'

# ---- MAME 0.264 with the bridge patch (Aurora68's CLAUDE.md, "The MAME testbed") ----------
if [ -n "$MAMEDIR" ]; then
	R=$MAMEDIR/reference/mame
	[ -f "$MAMEDIR/reference/mame-psmac.patch" ] || { echo "no reference/mame-psmac.patch in $MAMEDIR" >&2; exit 1; }
	if [ ! -d "$R/.git" ]; then
		git clone --branch mame0264 --depth 1 https://github.com/mamedev/mame "$R"
	fi
	if git -C "$R" apply --check "$MAMEDIR/reference/mame-psmac.patch" 2>/dev/null; then
		git -C "$R" apply "$MAMEDIR/reference/mame-psmac.patch"
	else
		echo "== the patch is applied already (or no longer applies: git -C $R status)"
	fi
	echo "== building MAME (psmac subtarget): about 25 minutes on four cores"
	( cd "$R" && make SUBTARGET=psmac \
		SOURCES=src/mame/apple/maciici.cpp,src/mame/apple/maciici_ps.cpp,src/mame/apple/macse_ps.cpp \
		REGENIE=1 NOWERROR=1 TOOLS=1 -j"$(nproc)" && ln -sf psmac mamepsmac )
	echo "== built $R/mamepsmac"
fi

# ---- vbcc and the NDK for the Amiga builds (Lagoon, Quill, Riptide, Cornerstone) -------------
# The repositories name vbcc 0.9h with vasm, vlink and the m68k-amigaos target in /opt/amiga/vbcc,
# NDK 3.2 in /opt/amiga/ndk. vbcc's site (http://www.compilers.de/vbcc.html) offers statically
# linked Linux x64 binaries of vbcc, vasm and vlink with every target's libraries (May 2022, the
# compiler says "V0.9i pre"); this takes that archive on x86. There is no ARM64 build (the "arm"
# one is 32-bit and Apple Silicon cannot run 32-bit ARM code): on ARM64 use an amd64 container
# (docker run --platform linux/amd64) for Amiga builds, or build vbcc from its sources by hand.
if [ -n "$AMIGA" ]; then
	$SUDO mkdir -p "$AMIGA"; $SUDO chown "$(id -u):$(id -g)" "$AMIGA"
	T=$(mktemp -d)
	if [ ! -x "$AMIGA/vbcc/bin/vc" ]; then
		[ "$ARCH" = amd64 ] || { echo "--amiga: no ARM64 vbcc binaries; use an amd64 container for Amiga builds" >&2; exit 1; }
		echo "== vbcc, vasm, vlink (vbcc_linux_x64.tar.gz) into $AMIGA/vbcc"
		curl -fsSLo "$T/vbcc.tar.gz" http://www.ibaug.de/vbcc/vbcc_linux_x64.tar.gz
		tar xzf "$T/vbcc.tar.gz" -C "$AMIGA"
	fi
	if [ ! -d "$AMIGA/ndk/Include_H" ]; then
		echo "== NDK 3.2 (Aminet dev/misc/NDK3.2.lha) into $AMIGA/ndk"
		curl -fsSLo "$T/NDK3.2.lha" https://aminet.net/dev/misc/NDK3.2.lha
		mkdir -p "$AMIGA/ndk"; (cd "$AMIGA/ndk" && lha -xq "$T/NDK3.2.lha")
		# NDK3.2.lha unpacks into a folder of its own (NDK3.2/ or NDK_3.2/) on some versions
		[ -d "$AMIGA/ndk/Include_H" ] || for d in "$AMIGA"/ndk/NDK*; do [ -d "$d/Include_H" ] && for e in "$d"/*; do mv "$e" "$AMIGA/ndk/"; done; done
	fi
	rm -f "$T/vbcc.tar.gz" "$T/NDK3.2.lha"; rmdir "$T"
	# the archive's own modes (some NDK headers are rw------- for their owner) keep other users out
	$SUDO chmod -R a+rX "$AMIGA"
	echo "== Amiga toolchain: export VBCC=$AMIGA/vbcc NDK=$AMIGA/ndk/Include_H PATH=\$VBCC/bin:\$PATH"
	echo "   bsdsocket headers (Lagoon, Riptide): NETINC=$AMIGA/ndk/SANA+RoadshowTCP-IP/netinclude"
	echo "   MUI 3.8 headers (Quill, Cornerstone): not redistributable; put Aminet dev/mui/mui38dev.lha's C/Include in $AMIGA/mui/include yourself"
fi

# ---- Claude Code ------------------------------------------------------------------------------
if [ $CLAUDE = 1 ] && ! command -v claude >/dev/null; then
	echo "== Claude Code (https://claude.ai/install.sh)"
	curl -fsSL https://claude.ai/install.sh | bash
	echo "   then: export PATH=\$HOME/.local/bin:\$PATH; claude"
fi

echo "== done. Check with: dev/setup-linux.sh --check"
