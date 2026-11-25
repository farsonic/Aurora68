#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# tools/wifi_pick_test.sh [BUILD] - the WiFi's choice of access point among several BSSIDs of one SSID on the PC
# (tools/wifi_pick_test.c, emu68/src/alice/wifi/fmac.c's fm_pick_*; docs/08 item 147). Run by `make -C mac check`.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
B=${1:-$ROOT/mac/build}
mkdir -p "$B"
cc -std=c99 -O1 -Wall -Wextra -Werror -I "$ROOT/emu68/src/alice/wifi" -o "$B/wifi_pick_test" \
   "$ROOT/tools/wifi_pick_test.c" "$ROOT/emu68/src/alice/wifi/fmac.c"
"$B/wifi_pick_test"
