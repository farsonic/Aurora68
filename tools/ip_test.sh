#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# ip_test.sh [BUILD_DIR] - the kernel's own IPv4 (emu68/src/psmac/psmac_ip.c) on the host: the shared WiFi address,
# its ARP answers and probes, the card's remembered address and the pings left to the Mac (docs/08 item 149).
# Run by `make -C mac check`.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
B=${1:-$ROOT/mac/build}
mkdir -p "$B"
cc -std=gnu99 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -DPSMAC_IP_HOST -I "$ROOT/tools" -I "$ROOT/emu68/src/psmac" -I "$ROOT/emu68/src/alice" \
   -o "$B/ip_test" "$ROOT/tools/ip_test.c" "$ROOT/emu68/src/psmac/psmac_ip.c" "$ROOT/emu68/src/psmac/psmac_apnet.c"
"$B/ip_test"
