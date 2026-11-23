#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# apnet_test.sh [BUILD_DIR] - Setup and Recovery's access point on the host (docs/08 item 117): the DHCP server
# and DNS answerer (emu68/src/psmac/psmac_apnet.c) and the firmware's SoftAP command sequence
# (emu68/src/alice/wifi/fmac_ap.c, with fmac.c). Run by `make -C mac check`.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
B=${1:-$ROOT/mac/build}
mkdir -p "$B"
cc -std=c99 -O1 -Wall -Wextra -Werror -I "$ROOT/emu68/src/psmac" -I "$ROOT/emu68/src/alice/wifi" -o "$B/apnet_test" \
   "$ROOT/tools/apnet_test.c" "$ROOT/emu68/src/psmac/psmac_apnet.c" "$ROOT/emu68/src/alice/wifi/fmac_ap.c" "$ROOT/emu68/src/alice/wifi/fmac.c"
"$B/apnet_test"
