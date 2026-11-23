#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# tools/vcodec_test.sh BUILD - the video decoder card's frame decoders against FFmpeg (docs/08 item 110).
# FFmpeg makes small test movies (its test pattern, a moving square) in each format the card decodes; each
# movie's samples are decoded by the card's code (tools/vcodec_test.c) and by FFmpeg, and the pictures
# compared byte for byte (Cinepak, Animation, Graphics) or within one step of 5-bit colour (Apple Video, which
# FFmpeg widens to 8 bits its own way). FFmpeg's code is not used by the card: only its encoders and decoders
# on the host, as a reference. Skipped when ffmpeg is not installed. Called by `make -C mac check`.
set -e
B=${1:-/tmp/vcodec}
T="$B/vcodec"
mkdir -p "$T"
if ! command -v ffmpeg >/dev/null 2>&1; then echo "vcodec_test: no ffmpeg, skipped"; exit 0; fi
SRC=../emu68/src/psmac
cc -std=c99 -O1 -g -Wall -Wextra -Werror ${SANITIZE:--fsanitize=address,undefined} -I$SRC -o "$T/vcodec_test" ../tools/vcodec_test.c \
   $SRC/psmac_vcodec.c $SRC/psmac_cinepak.c $SRC/psmac_rpza.c $SRC/psmac_qtrle.c $SRC/psmac_smc.c
IN="testsrc=size=160x120:rate=10:duration=3"
MOV="color=c=navy:size=160x120:rate=10:duration=3[b];[b]drawbox=x=t*40:y=20:w=40:h=40:color=yellow:t=fill"
fail=0
one() {   # name, filter, ffmpeg's encoder options, tolerance
    name=$1; shift; filt=$1; shift; tol=$1; shift
    ffmpeg -v error -y -f lavfi -i "$filt" "$@" "$T/$name.mov"
    python3 ../tools/movsamples.py "$T/$name.mov" "$T/$name.desc" "$T/$name.frames" >/dev/null
    ffmpeg -v error -y -i "$T/$name.mov" -f rawvideo -pix_fmt rgb24 "$T/$name.ref"
    "$T/vcodec_test" "$T/$name.desc" "$T/$name.frames" "$T/$name.out" > "$T/$name.log" || { cat "$T/$name.log"; echo "vcodec_test: $name: FAILED to decode"; fail=1; return; }
    if python3 - "$T/$name.out" "$T/$name.ref" "$tol" "$name" "$(cat "$T/$name.log")" <<'EOF'
import sys
a, b, tol, name, log = open(sys.argv[1], 'rb').read(), open(sys.argv[2], 'rb').read(), int(sys.argv[3]), sys.argv[4], sys.argv[5]
if len(a) != len(b):
    print(f'vcodec_test: {name}: {len(a)} bytes against FFmpeg\'s {len(b)}'); sys.exit(1)
worst = max((abs(x - y) for x, y in zip(a, b)), default=0)
diff = sum(1 for x, y in zip(a, b) if x != y)
print(f'vcodec_test: {name}: {log.strip()}; against FFmpeg: {diff} of {len(a)} bytes differ, at most by {worst}' + (' (allowed)' if worst <= tol else ''))
sys.exit(0 if worst <= tol else 1)
EOF
    then :; else fail=1; fi
    rm -f "$T/$name.out" "$T/$name.ref"
}
one cinepak "$IN" 0 -c:v cinepak
one cinepak-moving "$MOV" 0 -c:v cinepak -g 10
one rpza "$IN" 4 -c:v rpza
one rpza-moving "$MOV" 4 -c:v rpza
one rle24 "$IN" 0 -c:v qtrle -pix_fmt rgb24
one rle16 "$IN" 4 -c:v qtrle -pix_fmt rgb555be
one rle32 "$MOV" 0 -c:v qtrle -pix_fmt argb
one smc "$IN" 0 -c:v smc -pix_fmt pal8
one smc-moving "$MOV" 0 -c:v smc -pix_fmt pal8
[ $fail = 0 ] && echo "vcodec_test: all formats decoded as FFmpeg decodes them" || { echo "vcodec_test: FAILED"; exit 1; }
