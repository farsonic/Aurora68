#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# splash_test.sh [BUILD_DIR] - the HDMI splash (emu68/src/psmac/psmac_splash.c, docs/08 item 128) drawn on the host
# at the screen sizes a Pi gives, in 16 and 32 bits, with checks (tools/splash_test.c); then the logo's header:
# tools/mklogo.py must make the psmac_splash_logo.h in the tree from docs/img/aurora68-logo.svg.
# Run by `make -C mac check`. `splash_test DIR` (the binary) also writes DIR/splash-WxH.ppm to look at.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
B=${1:-$ROOT/mac/build}
mkdir -p "$B"
cc -std=c99 -O1 -Wall -Wextra -Werror -I "$ROOT/emu68/src/psmac" -o "$B/splash_test" "$ROOT/tools/splash_test.c" \
   "$ROOT/emu68/src/psmac/psmac_splash.c" "$ROOT/emu68/src/raspi/topaz.c"
"$B/splash_test" > "$B/splash_test.log" || { cat "$B/splash_test.log"; exit 1; }
tail -1 "$B/splash_test.log"
python3 "$ROOT/tools/mklogo.py" --check
