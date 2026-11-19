#!/bin/bash
# SPDX-License-Identifier: MPL-2.0
#
# bootstrap.sh: make (or bring up to date) the Aurora68 workspace and its build container.
#
# usage: bootstrap.sh [WORKSPACE] [--no-container] [--extras "SETUP OPTIONS"] [--build]
#   WORKSPACE        default ~/aurora68-work
#   --no-container   only the repositories (a Linux machine set up with dev/setup-linux.sh)
#   --extras "..."   dev/setup-linux.sh options baked into the image, e.g. "--claude --servers"
#   --build          then build everything and run the native tests in the container
#                    (dev/tools/buildall.sh --test)
#
# Runs on macOS (its bash 3.2) and Linux. Needs git; for the container, Docker Desktop or OrbStack.
# It can be run from anywhere, before anything is cloned:
#   curl -fsSLO https://raw.githubusercontent.com/farsonic/Aurora68/main/dev/bootstrap.sh && bash bootstrap.sh
#
# What it does:
#   1. clones the five repositories over HTTPS, each on the branch the work is on, or, where a
#      checkout is there already, fetches it and fast-forwards it if it is on that branch with no
#      local changes (anything else is reported and left alone);
#   2. fetches Emu68's three submodules (capstone, libdeflate, tiny-stl) at their pinned commits;
#   3. links WORKSPACE/CLAUDE.md to pistorm-se/dev/workspace/CLAUDE.md (the instructions a Claude
#      Code session started in the workspace reads first);
#   4. builds the image aurora68-dev from pistorm-se/dev/Dockerfile and says how to start it
#      (pistorm-se/dev/shell.sh).
set -eu

WORK= CONTAINER=1 EXTRAS= BUILD=0
while [ $# -gt 0 ]; do
	case "$1" in
	--no-container) CONTAINER=0 ;;
	--extras) EXTRAS=$2; shift ;;
	--build) BUILD=1 ;;
	-h|--help) sed -n '4,26p' "$0"; exit 0 ;;
	-*) echo "bootstrap.sh: unknown option $1" >&2; exit 2 ;;
	*) WORK=$1 ;;
	esac
	shift
done
WORK=${WORK:-$HOME/aurora68-work}
mkdir -p "$WORK"; WORK=$(cd "$WORK" && pwd)

# folder  url  branch  (Lagoon moved: farsonic/Lagoon.git is its new location)
REPOS="pistorm-se https://github.com/farsonic/Aurora68.git main
riptide https://github.com/farsonic/Riptide.git mac-port
lagoon https://github.com/farsonic/Lagoon.git mac-port
quill https://github.com/farsonic/Quill.git mac-port
cornerstone https://github.com/farsonic/cornerstone.git main"

echo "== the workspace: $WORK"
echo "$REPOS" | while read -r dir url branch; do
	d=$WORK/$dir
	if [ ! -d "$d/.git" ]; then
		echo "-- $dir: cloning $url ($branch)"
		git clone --branch "$branch" "$url" "$d"
		continue
	fi
	git -C "$d" remote set-url origin "$url"
	git -C "$d" fetch --quiet origin
	cur=$(git -C "$d" rev-parse --abbrev-ref HEAD)
	if [ "$cur" != "$branch" ]; then
		echo "-- $dir: on $cur, not $branch: left alone (git -C $d checkout $branch)"
	elif ! git -C "$d" diff --quiet || ! git -C "$d" diff --cached --quiet; then
		echo "-- $dir: local changes: left alone (fetched; git -C $d status)"
	else
		git -C "$d" merge --ff-only --quiet "origin/$branch" 2>/dev/null \
			&& echo "-- $dir: $branch at $(git -C "$d" rev-parse --short HEAD)" \
			|| echo "-- $dir: $branch has commits origin/$branch lacks: left alone"
	fi
done

A=$WORK/pistorm-se
echo "-- Emu68's submodules (capstone, libdeflate, tiny-stl)"
for s in capstone libdeflate tiny-stl; do
	if [ -z "$(ls -A "$A/emu68/external/$s" 2>/dev/null)" ]; then
		git -C "$A" submodule update --init --depth 1 "emu68/external/$s"
	fi
done

if [ ! -e "$WORK/CLAUDE.md" ] || [ -L "$WORK/CLAUDE.md" ]; then
	ln -sfn pistorm-se/dev/workspace/CLAUDE.md "$WORK/CLAUDE.md"
	echo "-- $WORK/CLAUDE.md -> pistorm-se/dev/workspace/CLAUDE.md"
else
	echo "-- $WORK/CLAUDE.md is a file of its own: left alone (compare it with pistorm-se/dev/workspace/CLAUDE.md)"
fi
mkdir -p "$WORK/scratch" "$WORK/inputs" "$A/rom/original"   # the ROMs go there; git ignores them
[ -f "$WORK/inputs/README.txt" ] || cat > "$WORK/inputs/README.txt" <<'EOF'
Files the repositories may not hold, for this machine only (never commit them anywhere):
  MacIIci_368CADFE.rom   your Macintosh IIci ROM (512 KB); copy it to pistorm-se/rom/original/ too
  aurora-test3.image     a System 7.1 disk image (the base of dev/tools/mkfullimage.sh)
  wifi-firmware/         what pistorm-se/tools/get-firmware.sh fetches for the container
EOF

if [ $CONTAINER = 1 ]; then
	if ! command -v docker >/dev/null || ! docker info >/dev/null 2>&1; then
		echo "== no Docker running: start Docker Desktop or OrbStack and run this again (or --no-container)"
		exit 1
	fi
	echo "== building the image aurora68-dev (a few minutes the first time)"
	docker build -t aurora68-dev ${EXTRAS:+--build-arg EXTRAS="$EXTRAS"} "$A/dev"
	echo "== ready. A shell in the container, the workspace at /work:"
	echo "     $A/dev/shell.sh"
	if [ $BUILD = 1 ]; then
		WORKSPACE=$WORK "$A/dev/shell.sh" /work/pistorm-se/dev/tools/buildall.sh --test
	fi
elif [ $BUILD = 1 ]; then
	"$A/dev/tools/buildall.sh" --test
fi
