#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# tools/mpg1_test.sh BUILD - MPEG-1 in QuickTime on the host: the import's indexer (mac/vdec/mpgindex.c) and
# the card's 'mpg1' and MP2 decoders (emu68/src/psmac/psmac_plmpeg.c, pl_mpeg) against FFmpeg (docs/08 item 113).
# FFmpeg makes small films the way the period's encoders and today's helper make them: MPEG-1 program streams
# with MP2 sound, with and without B-pictures, at 25 and at 29.97 frames a second, and a bare video stream;
# tools/mpg1_test.c indexes each, decodes QuickTime's samples in display order, seeks, and decodes the sound;
# tools/mpg1_compare.py compares the pictures and the sound with FFmpeg's decoding of the same file. FFmpeg's
# code is not used by the card or the Mac: only its encoder and decoder on the host, as a reference. Skipped
# when ffmpeg is not installed. Called by `make -C mac check`.
set -e
B=${1:-/tmp/mpg1}
T="$B/mpg1"
mkdir -p "$T"
if ! command -v ffmpeg >/dev/null 2>&1; then echo "mpg1_test: no ffmpeg, skipped"; exit 0; fi
SRC=../emu68/src/psmac
cc -std=c99 -O1 -g -Wall -Wextra -Werror ${SANITIZE:--fsanitize=address,undefined} -I../tools/mpg1_host -I$SRC -o "$T/mpg1_test" \
   ../tools/mpg1_test.c vdec/mpgindex.c $SRC/psmac_plmpeg.c $SRC/psmac_vcodec.c $SRC/psmac_cinepak.c $SRC/psmac_rpza.c \
   $SRC/psmac_qtrle.c $SRC/psmac_smc.c
PIC="testsrc2=size=320x240:rate=25:duration=3"
TONE="sine=frequency=440:sample_rate=44100:duration=3"
fail=0
one() {   # name, extension, ffmpeg's options
    name=$1; shift; ext=$1; shift
    ffmpeg -v error -y "$@" "$T/$name.$ext"
    ffmpeg -v error -y -i "$T/$name.$ext" -f rawvideo -pix_fmt yuv420p "$T/$name.ref.yuv"
    if ffprobe -v error -select_streams a -show_entries stream=index -of csv=p=0 "$T/$name.$ext" | grep -q .; then
        ffmpeg -v error -y -i "$T/$name.$ext" -vn -f s16be -ac 2 "$T/$name.ref.pcm"
    else : > "$T/$name.ref.pcm"; fi
    if "$T/mpg1_test" "$T/$name.$ext" "$T/$name.yuv" "$T/$name.pcm" > "$T/$name.log"; then
        echo "mpg1_test: $name: $(cat "$T/$name.log")"
    else cat "$T/$name.log"; echo "mpg1_test: $name: FAILED"; fail=1; return; fi
    python3 ../tools/mpg1_compare.py "$name" 320 240 "$T/$name.yuv" "$T/$name.ref.yuv" "$T/$name.pcm" "$T/$name.ref.pcm" || fail=1
    rm -f "$T/$name.yuv" "$T/$name.ref.yuv" "$T/$name.pcm" "$T/$name.ref.pcm"
}
one no-b mpg -f lavfi -i "$PIC" -f lavfi -i "$TONE" -c:v mpeg1video -b:v 800k -bf 0 -g 25 -c:a mp2 -b:a 128k -ac 2 -f mpeg
one b-frames mpg -f lavfi -i "$PIC" -f lavfi -i "$TONE" -c:v mpeg1video -b:v 800k -bf 2 -g 12 -c:a mp2 -b:a 128k -ac 2 -f mpeg
one ntsc mpg -f lavfi -i "testsrc2=size=320x240:rate=30000/1001:duration=3" -f lavfi -i "$TONE" -c:v mpeg1video -b:v 800k -bf 2 -g 15 \
    -c:a mp2 -b:a 192k -ac 2 -f mpeg
one video-only m1v -f lavfi -i "$PIC" -c:v mpeg1video -b:v 600k -bf 1 -g 10 -f mpeg1video
[ $fail = 0 ] && echo "mpg1_test: every film indexed and decoded as FFmpeg decodes it" || { echo "mpg1_test: FAILED"; exit 1; }
