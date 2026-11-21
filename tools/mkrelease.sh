#!/bin/bash
# Build the Aurora68 release zip: tools/mkrelease.sh VERSION   -> dist/Aurora68-VERSION.zip
#
# What goes in (see release/READ-ME-FIRST.txt for the user's view): the kernel from the psmac
# build, the boot files from release/, both declaration ROMs and every Macintosh binary from
# mac/build (built fresh), the container tool, an HFS image with the Macintosh files already on
# it, the release notes, the licences and a checksum list. Nothing Apple's: no ROM, no system
# software; the user adds the ROM with make-container.py.
set -e
cd "$(dirname "$0")/.."
V=${1:?version, e.g. 0.1}
OUT=dist/Aurora68-$V; rm -rf "$OUT" "$OUT.zip"; mkdir -p "$OUT/Mac" "$OUT/LICENSES"

echo "== kernel"
[ -d psmac-build ] || cmake -S emu68 -B psmac-build -DCMAKE_TOOLCHAIN_FILE=emu68/toolchains/aarch64-linux-gnu-13.cmake \
      -DTARGET=raspi64 -DVARIANT=psmac -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build psmac-build -j4 > /dev/null
gunzip -kf psmac-build/Emu68.img.gz
# kernel8.img goes on the card gzip-compressed, 1 MB instead of 5: the Pi firmware recognises a
# gzip archive by its first bytes and inflates a 64-bit kernel itself (config.txt documentation,
# arm_64bit), as upstream Emu68's own Emu68-pistorm.gz on every Pi 3 PiStorm (docs/08 item 50)
cp psmac-build/Emu68.img.gz "$OUT/kernel8.img"

echo "== Macintosh side"
make -C mac > /dev/null
cp mac/build/psmac.declrom "$OUT/aurora68.declrom"
cp mac/build/psmac_wifi.declrom "$OUT/aurora68-wifionly.declrom"
cp mac/build/psmac_wifi_video.declrom "$OUT/aurora68-wifi.declrom"       # screen + WiFi network, no wired Ethernet: the bench and Pis without Ethernet
cp mac/build/psmac-se.declrom "$OUT/aurora68-se.declrom"           # the SE's two screens at any HDMI size (the kernel rewrites it)
for f in mac/build/psmac-*x*.declrom; do cp "$f" "$OUT/aurora68-${f#mac/build/psmac-}"; done   # one per screen size (aurora68-1024x768.declrom ...),
                                                         # and each with the WiFi as the only network card (aurora68-wifi-1024x768.declrom ...)
for f in PiDisk PiMach PiWiFi PiBlue PiStatus PiNet PiPing PiScreens PiColor PiLog PiDepth WiFiMenu PiScrn SoundPiStorm; do cp "mac/build/$f.bin" "$OUT/Mac/"; done
# an HFS disk image (2 MB) with the Macintosh files on it, for emulators and Disk Copy
IMG="$OUT/Mac/Aurora68-Utilities.img"
dd if=/dev/zero of="$IMG" bs=1k count=2048 status=none
hformat -l "Aurora68 $V" "$IMG" > /dev/null
hmount "$IMG" > /dev/null
for f in "$OUT"/Mac/*.bin; do hcopy -m "$f" :; done
hcopy -t release/READ-ME-FIRST.txt ":Read Me"
humount

echo "== boot files, notes, licences"
cp release/config.txt release/cmdline.txt release/READ-ME-FIRST.txt "$OUT/"
cp tools/mkcontainer.py "$OUT/make-container.py"
cp tools/get-firmware.sh "$OUT/get-firmware.sh"      # the Pi's boot files and every board's WiFi/Bluetooth firmware, fetched and checked
mkdir -p "$OUT/patches"; cp rom/patches/iici-368CADFE.delta.json "$OUT/patches/"   # make-container.py patches a stock ROM with this (no assembler needed)
cp tools/rompatch.py "$OUT/rompatch.py"; cp rom/patches/iici.json "$OUT/patches/"     # the source of that delta, for the record
cp RELEASE-NOTES.md "$OUT/"
cp emu68/LICENSE "$OUT/LICENSES/Emu68-and-Aurora68-MPL-2.0.txt"
cp emu68/src/alice/../../COPYING "$OUT/LICENSES/Alice68-modules-GPL-2.0.txt" 2>/dev/null || \
  curl -sSf https://www.gnu.org/licenses/old-licenses/gpl-2.0.txt -o "$OUT/LICENSES/Alice68-modules-GPL-2.0.txt" 2>/dev/null || \
  echo "The Alice68 service modules (emu68/src/alice) are GPL-2.0-only; see https://www.gnu.org/licenses/old-licenses/gpl-2.0.html" > "$OUT/LICENSES/Alice68-modules-GPL-2.0.txt"
(cd "$OUT" && sha256sum kernel8.img *.declrom *.txt *.py *.sh Mac/* > SHA256SUMS)
(cd dist && zip -qr "Aurora68-$V.zip" "Aurora68-$V")
ls -la "dist/Aurora68-$V.zip"; unzip -l "dist/Aurora68-$V.zip" | tail -1
