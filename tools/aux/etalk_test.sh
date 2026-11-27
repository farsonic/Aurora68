#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# etalk_test.sh [BUILD_DIR] - the A/UX network card's chip model (emu68/src/psmac/psmac_etalk.c) on the host, driven
# as A/UX's ae6 driver drives it (docs/24-aux-network.md section 3): tools/aux/etalk_test.c.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
B=${1:-$ROOT/mac/build}
mkdir -p "$B"
cc -std=gnu99 -O1 -Wall -Wextra -Werror -I "$ROOT/tools/aux/etalk_stub" -I "$ROOT/emu68/src/psmac" -o "$B/etalk_test" \
   "$ROOT/tools/aux/etalk_test.c" "$ROOT/emu68/src/psmac/psmac_etalk.c"
"$B/etalk_test" > "$B/etalk_test.log" || { cat "$B/etalk_test.log"; exit 1; }
tail -1 "$B/etalk_test.log"
