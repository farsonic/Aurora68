#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
#
# tools/codec_test.sh - Aurora68 Play's FLAC, Ogg Vorbis and Apple Lossless path on the host (tools/codec_test.c):
# makes test files with ffmpeg (tones and noise, never music: the project's rule), decodes each with ffmpeg as the
# reference and with media.c and the card's decoders, and compares. Skipped when ffmpeg is not installed.
#     sh tools/codec_test.sh [BUILD_DIR]          (make -C mac check runs it with mac/build)
# Written on 22 November 2026 (docs/08 item 103).
set -e
here=$(cd "$(dirname "$0")" && pwd)
top=$(dirname "$here")
B=${1:-$top/mac/build}
if ! command -v ffmpeg >/dev/null 2>&1; then echo "codec_test: ffmpeg not installed, skipped"; exit 0; fi
mkdir -p "$B/codec"
D=$B/codec
P=$top/emu68/src/psmac
cc -std=c99 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-sign-compare -I "$P" -o "$D/codec_test" \
   "$here/codec_test.c" "$top/mac/piplay/media.c" "$top/mac/piplay/icy.c" \
   "$P/psmac_codec.c" "$P/psmac_flac.c" "$P/psmac_ogg.c" "$P/psmac_alac.c" -lm
ff() { ffmpeg -nostdin -hide_banner -loglevel error -y "$@"; }
# twelve seconds: a chord on each side, a sweep, and noise under it (noise makes the residuals work)
SRC="aevalsrc=0.30*sin(2*PI*440*t)+0.15*sin(2*PI*(200+400*t)*t)+0.05*(random(0)-0.5)|0.25*sin(2*PI*660*t)+0.10*sin(2*PI*3000*t)*sin(PI*t)+0.05*(random(1)-0.5):s=44100:d=12"
MONO="aevalsrc=0.4*sin(2*PI*523*t)*(0.6+0.4*sin(2*PI*0.5*t))+0.03*(random(0)-0.5):s=48000:d=9"
CAFE=$(printf 'Caf\303\251 Tone')
ff -f lavfi -i "$SRC" -c:a flac -sample_fmt s16 -metadata "title=Test Tone" -metadata artist=Aurora68 "$D/t16.flac"
ff -f lavfi -i "$SRC" -c:a flac -sample_fmt s32 -bits_per_raw_sample 24 "$D/t24.flac"
ff -f lavfi -i "$MONO" -c:a flac -sample_fmt s32 -bits_per_raw_sample 24 "$D/t24m.flac"
ff -f lavfi -i "$SRC" -c:a flac -sample_fmt s16 -compression_level 0 -metadata "title=$CAFE" -metadata artist=Aurora68 "$D/t16fast.flac"
ff -f lavfi -i "$SRC" -c:a libvorbis -q:a 4 -metadata "title=Test Tone" -metadata artist=Aurora68 "$D/t.ogg"
ff -f lavfi -i "$MONO" -c:a libvorbis -q:a 2 "$D/tm.ogg"
ff -f lavfi -i "$SRC" -c:a alac -sample_fmt s16p -metadata "title=Test Tone" -metadata artist=Aurora68 "$D/t16.m4a"
ff -f lavfi -i "$SRC" -c:a alac -sample_fmt s32p "$D/t24.m4a"
ff -f lavfi -i "$MONO" -c:a alac -sample_fmt s16p "$D/tm.m4a"
ff -f lavfi -i "$SRC" -c:a aac -b:a 128k "$D/aac.m4a"
ff -f lavfi -i "$SRC" -c:a libopus -b:a 96k "$D/t.opus"
# the reference as the card plays it: stereo as it is, mono on both sides at full level (ffmpeg's own upmix
# would put it 3 dB down), deeper samples cut to 16 bits
ref() {
	if [ "$(ffprobe -v error -select_streams a:0 -show_entries stream=channels -of csv=p=0 "$1")" = 1 ]; then
		ff -i "$1" -af "pan=stereo|c0=c0|c1=c0" -f s16le -sample_fmt s16 "$2"
	else
		ff -i "$1" -f s16le -ac 2 -sample_fmt s16 "$2"
	fi
}
fail=0
for f in t16.flac t24.flac t24m.flac t16fast.flac t16.m4a t24.m4a tm.m4a; do
	ref "$D/$f" "$D/$f.s16"
	"$D/codec_test" "$D/$f" "$D/$f.s16" exact || fail=1
done
for f in t.ogg tm.ogg; do
	ref "$D/$f" "$D/$f.s16"
	"$D/codec_test" "$D/$f" "$D/$f.s16" lossy || fail=1
done
"$D/codec_test" "$D/t16.flac" - tags "Test Tone" Aurora68 || fail=1
"$D/codec_test" "$D/t16fast.flac" - tags "$(printf 'Caf\216 Tone')" Aurora68 || fail=1
"$D/codec_test" "$D/t.ogg" - tags "Test Tone" Aurora68 || fail=1
"$D/codec_test" "$D/t16.m4a" - tags "Test Tone" Aurora68 || fail=1
"$D/codec_test" "$D/aac.m4a" - refuse || fail=1
"$D/codec_test" "$D/t.opus" - refuse || fail=1
if [ $fail = 0 ]; then echo "codec_test: all passed"; else echo "codec_test: FAILED"; exit 1; fi
