#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# tools/pccard_test.sh BUILD - the PC card on the host (tools/pccard_test.c, docs/19-pc-card.md): emu68/src/psmac/psmac_pc.c
# itself with Aurora86's host library, drawing into a pretend Mac screen: Libreboot's banner on the SE's 512 x 342 one-bit
# screen, and with FreeDOS 1.4 (Aurora86's tests fetch it) its installer left for DOS and "ver" typed through the card's
# input ring, on one bit and on an 8-bit screen; PNGs of each in BUILD/pccard. Needs an Aurora86 checkout after its
# `make firmware p2` (and `make test-freedos` for FreeDOS): AURORA86_DIR=/path/to/aurora86. Skipped without it, so that
# `make -C mac check` needs nothing from Aurora86. Called by `make -C mac check`.
set -e
B=${1:-/tmp/pccard}
T="$B/pccard"
A=${AURORA86_DIR:-}
if [ -z "$A" ] || [ ! -f "$A/build/p2/x86_64/libaurora86.a" ] || [ ! -f "$A/build/firmware/pc.rom" ]; then
    echo "pccard_test: no Aurora86 build (AURORA86_DIR=), skipped"; exit 0
fi
mkdir -p "$T"
SRC=../emu68/src/psmac
cc -std=gnu11 -O2 -g -Wall -Wextra -Werror -DPSMAC_PC_HOST -I../tools/pccard_host -I$SRC -I"$A/src/rt" -o "$T/pccard_test" \
   ../tools/pccard_test.c $SRC/psmac_pc.c $SRC/psmac_wclip.c "$A/build/p2/x86_64/libaurora86.a" -no-pie
ROM="$A/build/firmware/pc.rom"; VGA="$A/build/firmware/vgabios-bochs-stdvga.bin"; FD="$A/build/downloads/FD14LITE.img"
fail=0
run() { name=$1; shift
    if "$T/pccard_test" "$@" > "$T/$name.log" 2>&1; then grep '^pccard_test' "$T/$name.log"; else grep '^pccard_test' "$T/$name.log"; echo "pccard_test: $name FAILED ($T/$name.log)"; fail=1; fi
    python3 ../tools/pnm2png.py "$T/$name.pnm" "$T/$name.png"; }
run banner-se "$ROM" "$VGA" - 512x342x1 "$T/banner-se.pnm" 90 "Libreboot 26.11rev4 Rubber Duck (SeaBIOS)" "" "No bootable device"
if [ -f "$FD" ]; then
    run freedos-se "$ROM" "$VGA" "$FD" 512x342x1 "$T/freedos-se.pnm" 300 "What is your preferred language" '\r' "Return to DOS" '\d\r' ':\>' 'ver\r' "FreeCom version"
    run freedos-8bit "$ROM" "$VGA" "$FD" 800x600x8 "$T/freedos-8bit.pnm" 300 "What is your preferred language"
else
    echo "pccard_test: no FreeDOS image in $A/build/downloads (Aurora86's make test-freedos): FreeDOS skipped"
fi
[ $fail = 0 ] && echo "pccard_test: the card drew the PC's screen and passed keys to it" || { echo "pccard_test: FAILED"; exit 1; }
