#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# tools/pcplace_test.sh BUILD - Aurora68 PC's window on the host (tools/pcplace_test.c, docs/19-pc-card.md): the program's
# own mapping of its window onto the SE's screen and the HDMI screen (mac/a68pc/place.h) in both arrangements, and the
# kernel's live-region check and pointer box (emu68/src/psmac/psmac_wclip.c). Needs only cc. Called by `make -C mac check`.
set -e
B=${1:-/tmp/pcplace}
mkdir -p "$B"
cc -std=c99 -O1 -g -Wall -Wextra -Werror ${SANITIZE:--fsanitize=address,undefined} -I../emu68/src/psmac -o "$B/pcplace_test" \
   ../tools/pcplace_test.c ../emu68/src/psmac/psmac_wclip.c
"$B/pcplace_test"
