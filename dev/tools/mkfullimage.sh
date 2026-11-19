#!/bin/bash
# SPDX-License-Identifier: MPL-2.0
#
# mkfullimage.sh: the full test disk, a System 7.1 image with every current build on it, as sent
# to the bench (aurora-test3.image on 19 November 2026 was made this way).
#
# usage: dev/tools/mkfullimage.sh --base BASE.image --out OUT.image [options]
#   --base FILE      your System 7.1 image to start from (it is copied, never changed). Apple's
#                    software: not in any repository. A previous test image (aurora-test3.image)
#                    is a good base: every file below replaces the one of the same name.
#   --out FILE       the new image
#   --conf FILE      riptide.conf to install (default dev/tools/disk/riptide-macse.conf: both
#                    cards, DHCP, host name MacSE; riptide-onecard.conf has the wired card only)
#   --readme FILE    the "Test Disk Read Me" (default dev/tools/disk/testdisk-readme.txt, with
#                    @DATE@ and @AURORA68@ @RIPTIDE@ @LAGOON@ @QUILL@ filled in with the commits)
#   --part N         the HFS partition of BASE to use (default: hfsutils' choice, the first)
#   --plain          the non-logging Riptide, Lagoon and Octavo builds (build/mac) instead of the
#                    AURORA68=1 ones (build/mac-a68), whose log goes to the Pi's kernel log too
# The builds come from the workspace (dev/tools/buildall.sh first): Aurora68's mac/build, Riptide's
# build/mac and build/mac-a68, Lagoon's and Quill's build/mac-a68 (AURORA68, RIPTIDE, LAGOON,
# QUILL override the checkouts). A build that is missing stops it, except the optional ones.
#
# Where things go (the layout the bench's notes and docs/09 refer to):
#   System Folder:Extensions   PiDisk, PiMach, Wi-Fi & Bluetooth, Riptide, Lagoon EtherTalk
#   System Folder              Riptide DNR; Preferences:Riptide:riptide.conf
#   System Folder:Startup Items PiLog
#   the top level              PiBlue, PiNet, PiPing, PiStatus, PiWiFi; "Test Disk Read Me"
#   Utilities                  PiLog, PiPing, PiColor, PiScreens, PiDepth, Riptide Command
#                              Terminal, RiptidePrefs, MacTCP test, Browser test
#   Apps:Lagoon, Apps:Octavo   Lagoon; Octavo with its Fonts folder and five test documents
set -euo pipefail
. "$(dirname "$0")/lib.sh"

BASE= OUT= CONF=$DEVTOOLS/disk/riptide-macse.conf README=$DEVTOOLS/disk/testdisk-readme.txt PART= VAR=mac-a68
while [ $# -gt 0 ]; do
	case "$1" in
	--base) BASE=$2; shift ;;
	--out) OUT=$2; shift ;;
	--conf) CONF=$2; shift ;;
	--readme) README=$2; shift ;;
	--part) PART=$2; shift ;;
	--plain) VAR=mac ;;
	-h|--help) sed -n '4,33p' "$0"; exit 0 ;;
	*) die "unknown argument $1 (--help)" ;;
	esac
	shift
done
[ -n "$BASE" ] && [ -n "$OUT" ] || die "usage: mkfullimage.sh --base BASE.image --out OUT.image (--help)"
[ -f "$BASE" ] || die "no base image $BASE"
[ "$(cd "$(dirname "$BASE")" && pwd)/$(basename "$BASE")" != "$(cd "$(dirname "$OUT")" 2>/dev/null && pwd)/$(basename "$OUT")" ] || die "--out must not be the base"

A=$AURORA68/mac/build R=$RIPTIDE/build L=$LAGOON/build/$VAR Q=$QUILL
need() { [ -f "$1" ] || die "missing $1 (dev/tools/buildall.sh)"; }
for f in PiDisk PiMach WiFiMenu PiLog PiBlue PiNet PiPing PiStatus PiWiFi PiColor PiScreens PiDepth; do need $A/$f.bin; done
need $R/$VAR/Riptide.bin; need $R/mac/RiptideDNR.bin; need $R/mac/RiptidePrefs.bin; need $R/mac/MacTCPTest.bin
need $L/LagoonEtherTalk.bin; need $L/Lagoon.bin
QB=$Q/build/$VAR/Octavo.bin; need $QB

cp "$BASE" "$OUT.part"
hmount "$OUT.part" $PART >/dev/null
trap 'humount >/dev/null 2>&1 || true' EXIT
SF=":System Folder"
put() {   # put FILE.bin FOLDER: replaces the file of the same name
	local n; n=$(macbin_name "$1")
	hdel "$2:$n" 2>/dev/null || true
	hcopy -m "$1" "$2:"
	echo "  $2:$n"
}
mkd() { hmkdir "$1" 2>/dev/null || true; }
note "System Folder"
put $A/PiDisk.bin "$SF:Extensions"
put $A/PiMach.bin "$SF:Extensions"
put $A/WiFiMenu.bin "$SF:Extensions"
put $R/$VAR/Riptide.bin "$SF:Extensions"
put $L/LagoonEtherTalk.bin "$SF:Extensions"
put $R/mac/RiptideDNR.bin "$SF"
put $A/PiLog.bin "$SF:Startup Items"
note "Aurora68's programs (the disk's top level)"
for p in PiBlue PiNet PiPing PiStatus PiWiFi; do put $A/$p.bin ""; done
note "Utilities"
mkd ":Utilities"
for p in PiLog PiPing PiColor PiScreens PiDepth; do put $A/$p.bin ":Utilities"; done
for t in $R/$VAR/RiptideTerminal.bin $R/mac/RiptideTerminal.bin; do [ -f $t ] && { put $t ":Utilities"; break; }; done
put $R/mac/RiptidePrefs.bin ":Utilities"
put $R/mac/MacTCPTest.bin ":Utilities"
[ -f $R/mac/BrowserTest.bin ] && put $R/mac/BrowserTest.bin ":Utilities"
note "Apps"
mkd ":Apps"; mkd ":Apps:Lagoon"
put $L/Lagoon.bin ":Apps:Lagoon"
# the Mac edition of Quill is called Octavo (a classic Mac program called Quill exists already);
# an older image may still have Apps:Quill
hrename ":Apps:Quill" ":Apps:Octavo" 2>/dev/null || true
mkd ":Apps:Octavo"; mkd ":Apps:Octavo:Fonts"
hdel ":Apps:Octavo:Quill" 2>/dev/null || true
put $QB ":Apps:Octavo"
for f in word:Sample charts:Charts pictures:Pictures tables:Tables headers:Headers; do
	hdel ":Apps:Octavo:${f##*:}.docx" 2>/dev/null || true
	hcopy -r $Q/tests/data/${f%%:*}.docx ":Apps:Octavo:${f##*:}.docx"
	hattrib -t DOCX -c OCTV ":Apps:Octavo:${f##*:}.docx"
done
for f in $Q/fonts/*.ttf $Q/fonts/fonts.list; do
	hdel ":Apps:Octavo:Fonts:$(basename $f)" 2>/dev/null || true
	hcopy -r "$f" ":Apps:Octavo:Fonts:"
done
note "riptide.conf ($(basename "$CONF"))"
mkd "$SF:Preferences"; mkd "$SF:Preferences:Riptide"
hdel "$SF:Preferences:Riptide:riptide.conf" 2>/dev/null || true
hcopy -t "$CONF" "$SF:Preferences:Riptide:riptide.conf"
hattrib -t TEXT -c ttxt "$SF:Preferences:Riptide:riptide.conf"
note "Test Disk Read Me"
T=$(mktemp)
sed -e "s/@DATE@/$(date '+%-d %B %Y')/" -e "s/@AURORA68@/$(rev "$AURORA68")/" -e "s/@RIPTIDE@/$(rev "$RIPTIDE")/" \
    -e "s/@LAGOON@/$(rev "$LAGOON")/" -e "s/@QUILL@/$(rev "$QUILL")/" "$README" > "$T"
hdel ":Test Disk Read Me" 2>/dev/null || true
hcopy -t "$T" ":Test Disk Read Me"
hattrib -t TEXT -c ttxt ":Test Disk Read Me"
rm -f "$T"
hvol | tail -1
humount; trap - EXIT
mv "$OUT.part" "$OUT"
note "$OUT ($(du -h "$OUT" | cut -f1)); it mounts with: hmount $OUT"
