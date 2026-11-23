#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# tools/a2card_test.sh BUILD - the Apple II card on the host (tools/a2card_test.c, docs/20-apple-ii-card.md):
# emu68/src/psmac/psmac_a2.c itself with Aurora65's host library, drawing into a pretend Mac screen, through the guest
# scheduler (psmac_guest.c). Only Aurora65's own firmware and images (its make p2: build/rom, build/img), and two boot
# sectors of our own (tools/a2card_hires.s, tools/a2card_80col.s) assembled with Aurora65's assembler: the enhanced IIe
# booting a 5.25" image and typed into on the SE's 512 x 342 one-bit screen; hires at 1:1, every pixel checked; 80
# columns from the cells; the HDMI screen at 32 bits, hires in colour scaled by 2; the II+; a block image written by the
# Apple and written back into its file; the Disks menu's attach and eject; a change of model; no ROM refused clearly.
# With AURORA86_DIR too, the PC card and the Apple through the scheduler, each in front in turn (guest.floor=1 and 0).
# PNGs in BUILD/a2card. Needs AURORA65_DIR=/path/to/aurora65 after its `make p2`; skipped without it, so that
# `make -C mac check` needs nothing from Aurora65. Called by `make -C mac check`.
set -e
B=${1:-/tmp/a2card}
T="$B/a2card"
A=${AURORA65_DIR:-}
if [ -z "$A" ] || [ ! -f "$A/build/p2/x86_64/libaurora65.a" ] || [ ! -f "$A/build/rom/a65test-iie.rom" ] || [ ! -f "$A/build/img/test.dsk" ]; then
    echo "a2card_test: no Aurora65 build (AURORA65_DIR=), skipped"; exit 0
fi
mkdir -p "$T"
SRC=../emu68/src/psmac
cc -std=gnu11 -O2 -g -Wall -Wextra -Werror -DPSMAC_A2_HOST -DPSMAC_GUEST_HOST -DPSMAC_A2 -I../tools/pccard_host -I$SRC -I"$A/src/core" \
   -o "$T/a2card_test" ../tools/a2card_test.c $SRC/psmac_a2.c $SRC/psmac_guest.c $SRC/psmac_wclip.c "$A/build/p2/x86_64/libaurora65.a" -no-pie
ROM2E="$A/build/rom/a65test-iie.rom"; ROM2="$A/build/rom/a65test-ii.rom"; IMG="$A/build/img"
mkdsk() { python3 -I "$A/tools/a65asm.py" "$1" -o "$T/boot.bin" && python3 -I -c "
import sys; b=open(sys.argv[1],'rb').read(); d=bytearray(143360); d[:len(b)]=b; open(sys.argv[2],'wb').write(d)" "$T/boot.bin" "$2"; rm -f "$T/boot.bin"; }
mkdsk ../tools/a2card_hires.s "$T/hires.dsk"
mkdsk ../tools/a2card_80col.s "$T/c80.dsk"
fail=0
run() { name=$1; shift
    if (cd "$T" && ./a2card_test "$@" > "$T/$name.log" 2>&1); then grep '^a2card_test' "$T/$name.log"; else grep '^a2card_test' "$T/$name.log"; echo "a2card_test: $name FAILED ($T/$name.log)"; fail=1; fi
    python3 ../tools/pnm2png.py "$T/$name.pnm" "$T/$name.png"; }
# copies: the card writes what the Apple writes back into the image, and Aurora65's own images stay as they came
cp "$IMG/test.dsk" "$T/test.dsk"; cp "$IMG/test.po" "$T/test.po"
export A2CARD_OPTS="a2.rom=$ROM2E a2.d1=$T/test.dsk"
run boot-se 512x342x1 "$T/boot-se.pnm" 30 "5.25 DONE" 'HELLO APPLE\r' "HELLO APPLE"
export A2CARD_OPTS="a2.rom=$ROM2E a2.d1=$T/hires.dsk"
run hires-se 512x342x1 "$T/hires-se.pnm" 30 "HIRES 1:1" 'K' "K"
# every pixel of the hires picture at 1:1: four lines of every other column lit, four lines all lit (black on white)
python3 -I - "$T/hires-se.pnm" <<'EOF' || fail=1
import sys
d = open(sys.argv[1], 'rb').read().split(b'\n', 3)
w, h = map(int, d[1].split()); px = d[3]
bad = sum(1 for y in range(160) for i in range(280)
          if (px[(40 + y) * w + 18 + i] == 0) != ((i % 2 == 0) if y % 8 < 4 else True))
print("a2card_test: hires at 1:1: %d of 44,800 pixels not as the Apple's memory says" % bad)
sys.exit(1 if bad else 0)
EOF
A2CARD_WIN="420 288" run hires-se-scaled 512x342x1 "$T/hires-se-scaled.pnm" 30 "HIRES 1:1" '@scale' "HIRES 1:1"
A2CARD_WIN="1120 768" run hires-32 1280x800x32 "$T/hires-32.pnm" 30 "HIRES 1:1" '@scale' "HIRES 1:1"
export A2CARD_OPTS="a2.rom=$ROM2E a2.d1=$T/c80.dsk"
A2CARD_WIN="484 192" run c80-se 512x342x1 "$T/c80-se.pnm" 30 "80 COLUMNS FROM THE CELLS"
# the II+ (Aurora65's 12K test ROM): uppercase only
export A2CARD_OPTS="a2.model=iiplus a2.rom=$ROM2 a2.d1=$T/test.dsk"
run iiplus-se 512x342x1 "$T/iiplus-se.pnm" 30 "5.25 DONE" 'lower\r' "LOWER"
# a block image: the boot block writes its last block but one ("A65W" and the block number), the card writes it back
export A2CARD_OPTS="a2.rom=$ROM2E a2.hd1=$T/test.po"
run block-32 1280x800x32 "$T/block-32.pnm" 30 "WRITE+READ OK"
python3 -I -c "
import sys; d = open(sys.argv[1], 'rb').read(); n = len(d) // 512; b = d[(n - 2) * 512:(n - 2) * 512 + 6]
ok = b[:4] == b'A65W' and b[4] | b[5] << 8 == n - 2
print('a2card_test: test.po block %d in the file: %r: %s' % (n - 2, b, 'written back' if ok else 'NOT written back')); sys.exit(0 if ok else 1)" "$T/test.po" || fail=1
# the Disks menu: test.po listed (a2.img1=), put into HD1 (a 140K image goes to a 5.25" drive unless a unit is named), a
# cold start boots it from slot 7; then out again
cp "$IMG/test.po" "$T/test.po"
export A2CARD_OPTS="a2.rom=$ROM2E a2.img1=$T/test.po"
# (no disk in the Disk II: its boot waits for one for ever, as a real one does)
run disks-se 512x342x1 "$T/disks-se.pnm" 40 "BOOT SLOT 06" '@attach:0:2' "#unit2=0" '@cold' "BLOCK DONE" '@eject:2' "#unit2=-1"
# the program's half (23 November 2026): the place and the window's visRgn as Aurora68 Apple II gives them; its loop
# stopping, its heap going, the block freed, it quitting: nothing drawn after each; a program again: drawn, nothing outside
export A2CARD_OPTS="a2.rom=$ROM2E a2.d1=$T/test.dsk"
A2CARD_PROG=1 run prog-se 512x342x1 "$T/prog-se.pnm" 60 "5.25 DONE" '@stall' "5.25 DONE" '@live' "5.25 DONE" '@crash' "5.25 DONE" '@live' \
    "5.25 DONE" '@free' "5.25 DONE" '@live' "5.25 DONE" '@quit' "5.25 DONE" '@live' "5.25 DONE"
# a change of model: the II+ from apple2plus.rom in the run's folder (a host path relative to it, as the card's root)
cp "$ROM2" "$T/apple2plus.rom"
export A2CARD_OPTS="a2.rom=$ROM2E a2.d1=$T/test.dsk"
run model-se 512x342x1 "$T/model-se.pnm" 40 "5.25 DONE" '@model:1' "#model=1" '' "5.25 DONE" '@model:4' "#model=1"
rm -f "$T/apple2plus.rom"
# no ROM: refused, and why
export A2CARD_OPTS="a2.rom=$T/no-such.rom"
A2CARD_FAIL=1 run norom-se 512x342x1 "$T/norom-se.pnm" 10 "#state=5"
grep -q "No ROM for the Apple //e (enhanced): your own machine.s dump goes on the card as" "$T/norom-se.log" && echo "a2card_test: no ROM: refused with the reason" || { echo "a2card_test: no ROM: no reason given"; fail=1; }
unset A2CARD_OPTS
# both guests (the PC card too, with Aurora86's host library): each in front in turn, guest.floor=1 and 0
P=${AURORA86_DIR:-}
if [ -n "$P" ] && [ -f "$P/build/p2/x86_64/libaurora86.a" ] && [ -f "$P/build/firmware/pc.rom" ]; then
    cc -std=gnu11 -O2 -g -Wall -Wextra -Werror -DPSMAC_A2_HOST -DPSMAC_PC_HOST -DPSMAC_GUEST_HOST -DPSMAC_A2 -DPSMAC_PC -DWITH_PC \
       -I../tools/pccard_host -I$SRC -I"$A/src/core" -I"$P/src/rt" -o "$T/a2card_sched" ../tools/a2card_test.c $SRC/psmac_a2.c $SRC/psmac_pc.c \
       $SRC/psmac_guest.c $SRC/psmac_wclip.c "$A/build/p2/x86_64/libaurora65.a" "$P/build/p2/x86_64/libaurora86.a" -no-pie
    FD="$P/build/downloads/FD14LITE.img"
    for fl in 1 0; do for front in pc a2; do
        PCO="pc.rom=$P/build/firmware/pc.rom pc.vgarom=$P/build/firmware/vgabios-bochs-stdvga.bin pc.disk0ro=on guest.floor=$fl"
        [ -f "$FD" ] && PCO="$PCO pc.disk0=$FD"
        if (cd "$T" && A2CARD_OPTS="a2.rom=$ROM2E a2.d1=$T/hires.dsk" PC_OPTS="$PCO" ./a2card_sched sched 12 $front > "$T/sched-$fl-$front.log" 2>&1); then
            grep '^a2card_test' "$T/sched-$fl-$front.log"
        else echo "a2card_test: sched-$fl-$front FAILED ($T/sched-$fl-$front.log)"; fail=1; fi
    done; done
else
    echo "a2card_test: no Aurora86 build (AURORA86_DIR=): the two guests together skipped"
fi
rm -f "$T"/*.pnm
[ $fail = 0 ] && echo "a2card_test: the card drew the Apple's screen, passed keys to it and wrote its disks back" || { echo "a2card_test: FAILED"; exit 1; }
