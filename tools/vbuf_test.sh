#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# tools/vbuf_test.sh BUILD - the video card's stream channel on the host: emu68/src/psmac/psmac_video.c itself (the
# ring, the Pi's buffer behind it, the start by seconds held, rebuffering) fed a real MPEG-1 film through a
# simulated network with bursts and gaps (tools/vbuf_test.c, docs/08 item 125). FFmpeg makes the film at test time
# with the music server's own arguments for /vmpeg (320 x 240, here 1,200 kbit/s of video so that the ring holds only 3 s, as at the sizes Play asks for HDMI; MP2 at 128 kbit/s); nothing of
# it is kept. Every scenario must decode and show every picture, the same pictures (one hash for all), start only
# with the prefill held, rebuffer exactly as often as it says, and the sound must have no gaps; the card as it was
# (mac.vbuffer=0 mac.vprefill=0) is run on the same networks and must be the worse. Skipped when ffmpeg is not
# installed. Called by `make -C mac check`.
set -e
B=${1:-/tmp/vbuf}
T="$B/vbuf"
mkdir -p "$T"
if ! command -v ffmpeg >/dev/null 2>&1; then echo "vbuf_test: no ffmpeg, skipped"; exit 0; fi
SRC=../emu68/src/psmac
cc -std=c99 -O1 -g -Wall -Wextra -Werror ${SANITIZE:--fsanitize=address,undefined} -DPSMAC_VIDEO_HOST -I../tools/vbuf_host \
   -I../tools/mpg1_host -I$SRC -o "$T/vbuf_test" ../tools/vbuf_test.c $SRC/psmac_video.c $SRC/psmac_wclip.c $SRC/psmac_plmpeg.c \
   $SRC/psmac_vcodec.c $SRC/psmac_cinepak.c $SRC/psmac_rpza.c $SRC/psmac_qtrle.c $SRC/psmac_smc.c
ffmpeg -v error -y -f lavfi -i "testsrc2=size=320x240:rate=25:duration=20" -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=20" \
    -vf fps=25,format=yuv420p -c:v mpeg1video -b:v 1200k -maxrate 2400k -bufsize 1200k -bf 0 -g 25 \
    -c:a mp2 -b:a 128k -ar 44100 -ac 2 -f mpeg -muxdelay 0.5 "$T/film.mpg"
N=$(ffprobe -v error -select_streams v -count_frames -show_entries stream=nb_read_frames -of csv=p=0 "$T/film.mpg" | tr -dc 0-9)
fail=0
for sc in steady bursty outage outage-timer pause stop bursty-old outage-old cover1 cover2 cover-repair cover-paused cover-old; do
    if "$T/vbuf_test" "$T/film.mpg" "$N" $sc > "$T/$sc.log"; then grep '^vbuf_test' "$T/$sc.log"
    else cat "$T/$sc.log"; echo "vbuf_test: $sc: FAILED"; fail=1; fi
done
# one picture sequence for every scenario that played the film once; and the card as it was the worse
python3 - "$T" <<'EOF' || fail=1
import re, sys
d = sys.argv[1]
def line(sc):
    return [l for l in open(f'{d}/{sc}.log') if l.startswith(f'vbuf_test: {sc}: ')][0]
hashes = {sc: re.search(r'pictures ([0-9a-f]{16})', line(sc)).group(1) for sc in ('steady', 'bursty', 'outage', 'outage-timer', 'pause')}
if len(set(hashes.values())) != 1:
    print('vbuf_test: the pictures differ between scenarios:', hashes); sys.exit(1)
def dry(sc):
    l = line(sc)
    return int(re.search(r'rebuffered (\d+) times', l).group(1)) + int(re.search(r'sound gaps (\d+)', l).group(1))
for new, old in (('bursty', 'bursty-old'), ('outage', 'outage-old')):
    if dry(new) >= max(dry(old), 1):
        print(f'vbuf_test: {new} stopped {dry(new)} times, the card as it was {dry(old)}: no better'); sys.exit(1)
    print(f'vbuf_test: {new}: {dry(new)} stops against {dry(old)} with the card as it was')
EOF
rm -f "$T/film.mpg"
[ $fail = 0 ] && echo "vbuf_test: every scenario played every picture, started on time and rebuffered as it should" || { echo "vbuf_test: FAILED"; exit 1; }
