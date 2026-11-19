#!/bin/bash
# SPDX-License-Identifier: MPL-2.0
#
# mktestset.sh: a test set for the user's SD card, the way test sets 3 and 4 were made on
# 19 November 2026: the kernel built from a clean worktree at a pushed commit, the declaration ROMs
# of that commit, config.txt and cmdline.txt for the bench, optionally a small update disk, a
# TEST-BUILD.txt, checked for key material and secrets, and zipped.
#
# usage: dev/tools/mktestset.sh --out DIR [options]
#   --out DIR          the set's folder (DIR.zip beside it); must not exist yet
#   --commit REF       the Aurora68 commit (default HEAD of the checkout); it must be on origin
#                      (pushed) so the set can be traced, unless --unpushed
#   --title TEXT       the first line of TEST-BUILD.txt (default "Aurora68 test set, DATE")
#   --notes FILE       appended to TEST-BUILD.txt: what is new, what to try, what to photograph
#   --screen WxH       config.txt's HDMI mode and cmdline.txt's mac.screen (default 1920x1200,
#                      hdmi_mode=69, the bench's Dell)
#   --images A[,B..]   cmdline.txt's mac.image= (default aurora-test3.image); the first is the
#                      startup disk
#   --board solo|se    mac.board= (default solo: the Pi alone on the bench)
#   --opt KEY=VALUE    another cmdline option (repeatable); each key may appear only once
#   --update-disk NAME.img  a small partitioned HFS disk made with mkpartimg.py from
#                      --update-base IMAGE (needed), labelled --update-label TEXT, holding every
#                      --update-put FILE.bin and --update-text FILE[:NAME]
#   --add FILE         any other file into the set (repeatable)
#   --kernel-only      only kernel8.img (and the update disk, notes, --add files): the card keeps
#                      its declaration ROMs, config.txt, cmdline.txt and container
#   --no-tools         without make-container.py, get-firmware.sh, rompatch.py and patches/
#   --raw-kernel       kernel8.img uncompressed (default gzip, 1 MB instead of 5, as the releases)
#   --no-zip
# The kernel is built in $SCRATCH/kernel/COMMIT (worktree src/, build/), reused when it is there.
# Nothing of Apple's and no firmware goes in: the card already has the ROM in its container and
# the firmware files; the set is refused if one turns up, or key material, or a WiFi password.
set -euo pipefail
. "$(dirname "$0")/lib.sh"

OUT= REF=HEAD UNPUSHED=0 TITLE= NOTES= SCREEN=1920x1200 IMAGES=aurora-test3.image BOARD=solo OPTS=()
UPD= UPDBASE= UPDLABEL=Update UPDPUT=() ADD=() KONLY=0 TOOLS=1 RAW=0 ZIP=1
while [ $# -gt 0 ]; do
	case "$1" in
	--out) OUT=$2; shift ;;
	--commit) REF=$2; shift ;;
	--unpushed) UNPUSHED=1 ;;
	--title) TITLE=$2; shift ;;
	--notes) NOTES=$2; shift ;;
	--screen) SCREEN=$2; shift ;;
	--images) IMAGES=$2; shift ;;
	--board) BOARD=$2; shift ;;
	--opt) OPTS+=("$2"); shift ;;
	--update-disk) UPD=$2; shift ;;
	--update-base) UPDBASE=$2; shift ;;
	--update-label) UPDLABEL=$2; shift ;;
	--update-put) UPDPUT+=(--put "$2"); shift ;;
	--update-text) UPDPUT+=(--text "$2"); shift ;;
	--add) ADD+=("$2"); shift ;;
	--kernel-only) KONLY=1 ;;
	--no-tools) TOOLS=0 ;;
	--raw-kernel) RAW=1 ;;
	--no-zip) ZIP=0 ;;
	-h|--help) sed -n '4,36p' "$0"; exit 0 ;;
	*) die "unknown argument $1 (--help)" ;;
	esac
	shift
done
[ -n "$OUT" ] || die "--out DIR is needed (--help)"
[ ! -e "$OUT" ] || die "$OUT exists: choose a new name (sets are not overwritten)"
[ -z "$UPD" ] || [ -f "$UPDBASE" ] || die "--update-disk needs --update-base IMAGE (a partitioned image with a driver, e.g. aurora-test3.image)"
case "$SCREEN" in
640x480) MODE=4 ;; 800x600) MODE=9 ;; 1024x768) MODE=16 ;; 1280x1024) MODE=35 ;; 1280x720) MODE=85 ;;
1920x1080) MODE=82 ;; 1920x1200) MODE=69 ;; 512x342) MODE=4 ;;
*) die "no DMT mode known for $SCREEN (the declaration ROMs come in 512x342 640x480 800x600 1024x768 1280x720 1280x1024 1920x1080 1920x1200)" ;;
esac

# ---- the kernel, from a clean worktree at a pushed commit --------------------------------------
SHA=$(git -C "$AURORA68" rev-parse --verify "$REF^{commit}") || die "no commit $REF"
SHORT=${SHA:0:7}
git -C "$AURORA68" fetch --quiet origin || echo "(could not fetch origin; checking against what is known of it)"
if [ -z "$(git -C "$AURORA68" branch -r --contains "$SHA" 2>/dev/null)" ]; then
	[ $UNPUSHED = 1 ] || die "$SHORT is not on origin: push it first (a set must name a commit anyone can find), or --unpushed"
	echo "warning: $SHORT is not pushed"
fi
K=$SCRATCH/kernel/$SHORT
if [ ! -d "$K/src" ]; then
	note "worktree $K/src at $SHORT"
	mkdir -p "$K"
	git -C "$AURORA68" worktree add --detach "$K/src" "$SHA" >/dev/null
fi
# A fresh worktree has Emu68's three submodules empty: take them from the main checkout when it
# has them (the same pinned commits), else fetch them.
for s in capstone libdeflate tiny-stl; do
	if [ -z "$(ls -A "$K/src/emu68/external/$s" 2>/dev/null)" ]; then
		if [ -n "$(ls -A "$AURORA68/emu68/external/$s" 2>/dev/null)" ]; then
			cp -a "$AURORA68/emu68/external/$s/." "$K/src/emu68/external/$s/"
		else
			git -C "$K/src" submodule update --init --depth 1 "emu68/external/$s"
		fi
	fi
done
if [ ! -f "$K/build/Emu68.img.gz" ] || [ ! -f "$K/build/Emu68.img" ]; then
	note "building the kernel at $SHORT (log $K/build.log)"
	# absolute paths: cmake resolves a relative toolchain file against the build folder
	{ cmake -S "$K/src/emu68" -B "$K/build" -DCMAKE_TOOLCHAIN_FILE="$K/src/emu68/toolchains/aarch64-linux-gnu-13.cmake" \
		-DTARGET=raspi64 -DVARIANT=psmac -DCMAKE_BUILD_TYPE=Release \
	  && cmake --build "$K/build" -j"$(nproc)" && gunzip -kf "$K/build/Emu68.img.gz"; } > "$K/build.log" 2>&1 \
	  || { tail -20 "$K/build.log"; die "the kernel did not build: $K/build.log"; }
fi

# ---- the set ------------------------------------------------------------------------------------
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
note "the set: $OUT"
if [ $RAW = 1 ]; then cp "$K/build/Emu68.img" "$OUT/kernel8.img"; else cp "$K/build/Emu68.img.gz" "$OUT/kernel8.img"; fi
MB=$K/src/mac/build
if [ $KONLY = 0 ]; then
	# the release's names (tools/mkrelease.sh): psmac*.declrom become aurora68*.declrom
	cp "$MB/psmac.declrom" "$OUT/aurora68.declrom"
	cp "$MB/psmac_wifi.declrom" "$OUT/aurora68-wifionly.declrom"
	cp "$MB/psmac_wifi_video.declrom" "$OUT/aurora68-wifi.declrom"
	for f in "$MB"/psmac-*x*.declrom; do b=$(basename "$f"); cp "$f" "$OUT/aurora68-${b#psmac-}"; done
	# config.txt: the release's, with the HDMI mode of the bench's screen
	sed -E "s/^hdmi_mode=[0-9]+ +#.*/hdmi_mode=$MODE                  # $SCREEN (this test set). 4 = 640 x 480, 9 = 800 x 600, 16 = 1024 x 768, 82 = 1080p,/" \
		"$K/src/release/config.txt" > "$OUT/config.txt"
	grep -q "^hdmi_mode=$MODE " "$OUT/config.txt" || die "config.txt's hdmi_mode line has changed shape; set it by hand"
	# cmdline.txt: one line, each option once (the kernel takes the first of a repeated
	# mac.image=, which cost the bench an evening), the network's name and password as placeholders
	LINE=(enable_cache "mac.board=$BOARD")
	[ "$SCREEN" != 640x480 ] && LINE+=("mac.screen=$SCREEN")
	[ -n "$IMAGES" ] && LINE+=("mac.image=$IMAGES")
	LINE+=(wifi.ssid=YOURNETWORK wifi.psk=YOURPASSWORD wifi.country=AU)
	for o in ${OPTS[@]+"${OPTS[@]}"}; do
		k=${o%%=*}; i=0
		for e in "${LINE[@]}"; do [ "${e%%=*}" = "$k" ] && LINE[$i]=$o && k=; i=$((i + 1)); done
		[ -n "$k" ] && LINE+=("$o")
	done
	printf '%s\n' "${LINE[*]}" > "$OUT/cmdline.txt"
	dup=$(tr ' ' '\n' < "$OUT/cmdline.txt" | sed 's/=.*//' | sort | uniq -d)
	[ -z "$dup" ] || die "cmdline.txt names $dup twice"
fi
if [ $TOOLS = 1 ] && [ $KONLY = 0 ]; then
	cp "$K/src/tools/mkcontainer.py" "$OUT/make-container.py"
	cp "$K/src/tools/get-firmware.sh" "$OUT/get-firmware.sh"
	cp "$K/src/tools/rompatch.py" "$OUT/rompatch.py"
	mkdir -p "$OUT/patches"; cp "$K/src/rom/patches/iici-368CADFE.delta.json" "$K/src/rom/patches/iici.json" "$OUT/patches/"
fi
if [ -n "$UPD" ]; then
	note "update disk $UPD"
	python3 "$DEVTOOLS/mkpartimg.py" "$UPDBASE" "$OUT/$(basename "$UPD")" --label "$UPDLABEL" ${UPDPUT[@]+"${UPDPUT[@]}"}
fi
for f in ${ADD[@]+"${ADD[@]}"}; do cp "$f" "$OUT/"; done

# ---- TEST-BUILD.txt ------------------------------------------------------------------------------
{
	T=${TITLE:-"Aurora68 test set, $(date '+%-d %B %Y')"}
	echo "$T"; echo "$T" | sed 's/./=/g'; echo
	echo "Kernel and declaration ROMs from Aurora68 $SHORT; the Mac programs of this period: Riptide $(rev "$RIPTIDE"),"
	echo "Lagoon $(rev "$LAGOON"), Quill (Octavo) $(rev "$QUILL"). Not a release: tested in the QEMU testbed."
	echo
	echo "In this set"
	echo "- kernel8.img$( [ $RAW = 1 ] || echo ' (gzip-compressed; the Pi firmware inflates it itself)')"
	if [ $KONLY = 0 ]; then
		echo "- aurora68*.declrom: the declaration ROMs. The container needs the one whose size matches"
		echo "  mac.screen= (aurora68-$SCREEN.declrom here); replace them together with kernel8.img."
		echo "- config.txt (hdmi_mode=$MODE for $SCREEN) and cmdline.txt (one line):"
		echo "    $(cat "$OUT/cmdline.txt")"
		echo "  Put your network's name and password in place of YOURNETWORK and YOURPASSWORD on the card."
	fi
	[ -n "$UPD" ] && echo "- $(basename "$UPD"): a small disk, \"$UPDLABEL\". Add it to mac.image= after the startup disk."
	[ $TOOLS = 1 ] && [ $KONLY = 0 ] && echo "- make-container.py, get-firmware.sh, rompatch.py, patches/: to make the container"
	echo
	if [ $KONLY = 0 ]; then cat <<-'EOF'
	Setting up the card
	1. Copy kernel8.img, every aurora68*.declrom, config.txt and cmdline.txt to the card's root.
	   Put your network's name and password into cmdline.txt (only there: never in a message).
	2. Disk images go in the card's root as files of their own; cmdline.txt's mac.image= lists them,
	   the startup disk first. Move images you are not using into a folder: every image file in the
	   root is mounted otherwise.
	3. The container, about 1 MB, with no disk image in it (a Pi 3B):
	     python3 make-container.py -o aurora68.cpio MacIIci_368CADFE.rom aurora68-SIZE.declrom \
	       cyfmac43430-sdio.bin brcmfmac43430-sdio.txt cyfmac43430-sdio.clm_blob BCM43430A1.hcd
	   (sh get-firmware.sh --container MacIIci_368CADFE.rom --declrom aurora68-SIZE.declrom fetches the
	   firmware, checks it and makes the container in one go.)
	4. Boot. If the Mac will not start: hold Shift (no extensions); the previous set still works with
	   its own kernel.

	What to send back
	- PiLog's Save to Card where the kernel has it (aurora68.log on the card), or photographs of PiLog with Kernel ticked
	  (type a word such as BOOT, CRASH, ETH, WIFI, echo in its Filter field).
	EOF
	fi
	[ -n "$NOTES" ] && { echo; cat "$NOTES"; }
} > "$OUT/TEST-BUILD.txt"

# ---- the checks: nothing secret, nothing of Apple's, no firmware ---------------------------------
note "checking the set"
bad=0
while IFS= read -r -d '' f; do
	b=$(basename "$f")
	case "$b" in
	*.pem|*.crt|*.cer|*.key|*.p12|*.pfx|id_rsa*|id_ed25519*) echo "REFUSED: $b is key material"; bad=1 ;;
	*.rom|bootcode.bin|start*.elf|fixup*.dat|*.hcd|*.clm_blob|brcmfmac*|cyfmac*|*.cpio) echo "REFUSED: $b is a ROM, firmware or a container"; bad=1 ;;
	esac
	if LC_ALL=C grep -a -l -E 'BEGIN ([A-Z]+ )?PRIVATE KEY|BEGIN CERTIFICATE|BEGIN OPENSSH|ssh-(rsa|ed25519) AAAA' "$f" >/dev/null 2>&1; then
		echo "REFUSED: $b holds key or certificate material"; bad=1
	fi
	head -c 4 "$f" | xxd -p | grep -qiE '^(368cadfe|b2e362a8)$' && { echo "REFUSED: $b is a Macintosh ROM"; bad=1; }
	case "$b" in *.txt|*.conf|*.md|*.sh|*.py|*.json)
		if grep -a -o -E '(wifi\.psk|psk|password|passphrase)=[^ ]+' "$f" | grep -v -E '=(YOURPASSWORD|\$|"\$)' | grep -q .; then
			echo "REFUSED: $b has a password in it"; bad=1
		fi ;;
	esac
done < <(find "$OUT" -type f -print0)
[ $bad = 0 ] || die "the set has things that must not be sent: remove them ($OUT)"
(cd "$OUT" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS)
echo "  no key material, ROMs, firmware or passwords"
if [ $ZIP = 1 ]; then
	(cd "$(dirname "$OUT")" && zip -qr "$(basename "$OUT").zip" "$(basename "$OUT")")
	note "$OUT.zip ($(du -h "$OUT.zip" | cut -f1))"
fi
ls -l "$OUT"
