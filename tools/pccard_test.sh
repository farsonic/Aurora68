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
# the program's half (23 November 2026): the place and the window's visRgn as Aurora68 PC gives them; its loop stopping, its
# heap going, it quitting: nothing drawn on either screen after each; a program again: the window drawn, nothing outside it.
# Then two screens as on the owner's SE (its own 512 x 342 screen 0, HDMI screen 1): the window on HDMI, and on the SE's.
export PCCARD_PROG=1
run prog-se "$ROM" "$VGA" - 512x342x1 "$T/prog-se.pnm" 120 "Libreboot" '@stall' "Libreboot" '@live' "Libreboot" '@crash' "Libreboot" '@live' \
    "Libreboot" '@free' "Libreboot" '@live' "Libreboot" '@quit' "Libreboot" '@live' "No bootable device"
export PCCARD_TWO=1 PCCARD_WIN=1,88,60
run two-hdmi "$ROM" "$VGA" - 512x342x1 "$T/two-hdmi.pnm" 90 "Libreboot" '@quit' "Libreboot" '@live' "No bootable device"
export PCCARD_WIN=0,15,40
run two-se "$ROM" "$VGA" - 512x342x1 "$T/two-se.pnm" 90 "Libreboot" '@stall' "Libreboot" '@live' "No bootable device"
unset PCCARD_TWO PCCARD_WIN
# drawn over another window (24 November 2026, docs/20 "Drawn over another window"): the window's region made smaller in
# the middle of a picture on a 32-bit screen, before a band's look (given up) and just after (drawn over, then painted
# again at the card's request); with pc.guard=off, as before that day: the leftover
run cover-32 "$ROM" "$VGA" - 1280x800x32 "$T/cover-32.pnm" 90 "Libreboot" '@cover:1' "Libreboot" '@live' "Libreboot" '@cover:2' "Libreboot"
PCCARD_OPTS="pc.guard=off" PCCARD_BUG=1 run cover-old-32 "$ROM" "$VGA" - 1280x800x32 "$T/cover-old-32.pnm" 90 "Libreboot" '@cover:2' "Libreboot"
unset PCCARD_PROG
if [ -f "$FD" ]; then
    # a copy: the card writes what the PC writes back into the image (docs/19-pc-card.md "Disks"), and FreeDOS's own
    # download stays as it came
    cp "$FD" "$T/freedos.img"
    run freedos-se "$ROM" "$VGA" "$T/freedos.img" 512x342x1 "$T/freedos-se.pnm" 300 "What is your preferred language" '\r' "Return to DOS" '\d\r' ':\>' 'ver\r' "FreeCom version"
    run freedos-8bit "$ROM" "$VGA" "$T/freedos.img" 800x600x8 "$T/freedos-8bit.pnm" 300 "What is your preferred language"
    # writing back: a file made at DOS's prompt is in the image when the PC starts again (a new process: the kernel
    # restarted), and not with pc.disk0ro=on
    run write-1 "$ROM" "$VGA" "$T/freedos.img" 512x342x1 "$T/write-1.pnm" 300 "What is your preferred language" '\r' "Return to DOS" '\d\r' ':\>' 'echo WRITTEN BY THE PC>a68test.txt\r' ':\>'
    run write-2 "$ROM" "$VGA" "$T/freedos.img" 512x342x1 "$T/write-2.pnm" 300 "What is your preferred language" '\r' "Return to DOS" '\d\r' ':\>' 'type a68test.txt\r' "WRITTEN BY THE PC"
    export PCCARD_OPTS="pc.disk0ro=on"; run write-ro-1 "$ROM" "$VGA" "$T/freedos.img" 512x342x1 "$T/write-ro-1.pnm" 300 "What is your preferred language" '\r' "Return to DOS" '\d\r' ':\>' 'del a68test.txt\r' ':\>'
    unset PCCARD_OPTS; run write-ro-2 "$ROM" "$VGA" "$T/freedos.img" 512x342x1 "$T/write-ro-2.pnm" 300 "What is your preferred language" '\r' "Return to DOS" '\d\r' ':\>' 'type a68test.txt\r' "WRITTEN BY THE PC"
    # a CD from the Disks menu: put into the CD-ROM drive while DOS runs (FreeDOS's UDVD2 and SHSUCDX found the empty
    # drive at the start: D:), read, ejected; then a second one in its place
    printf 'FROM THE FIRST CD\r\n' > "$T/a68cd.txt"; python3 ../tools/mkiso.py "$T/one.iso" ONE "$T/a68cd.txt"
    printf 'FROM THE SECOND CD\r\n' > "$T/a68cd.txt"; python3 ../tools/mkiso.py "$T/two.iso" TWO "$T/a68cd.txt"
    export PCCARD_OPTS="pc.img1=$T/one.iso pc.img2=$T/two.iso"
    run cd "$ROM" "$VGA" "$T/freedos.img" 512x342x1 "$T/cd.pnm" 300 "What is your preferred language" '\r' "Return to DOS" '\d\r' \
        ':\>' '@attach:1' ':\>' 'type d:a68cd.txt\r' "FROM THE FIRST CD" '@eject:1' ':\>' '@attach:2' ':\>' 'type d:a68cd.txt\r' "FROM THE SECOND CD"
    unset PCCARD_OPTS
    rm -f "$T/one.iso" "$T/two.iso" "$T/a68cd.txt"
    rm -f "$T/freedos.img"
else
    echo "pccard_test: no FreeDOS image in $A/build/downloads (Aurora86's make test-freedos): FreeDOS skipped"
fi
[ $fail = 0 ] && echo "pccard_test: the card drew the PC's screen and passed keys to it" || { echo "pccard_test: FAILED"; exit 1; }
