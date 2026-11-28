#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# adb_test.sh [BUILD_DIR] - the SE board's virtual ADB keyboard and mouse (emu68/src/psmac/psmac_adb.c, docs/08 item
# 151) on the host, against a model of the SE's VIA and transceiver and of the IIci ROM's ADB Manager
# (tools/adb_test.c). Run by `make -C mac check`. `adb_test v` (the binary) prints the file's own log lines too.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
B=${1:-$ROOT/mac/build}
mkdir -p "$B"
cc -std=gnu11 -O1 -Wall -Wextra -Werror -DPSMAC_ADB_HOST -I "$ROOT/tools/pccard_host" -o "$B/adb_test" "$ROOT/tools/adb_test.c"
"$B/adb_test" > "$B/adb_test.log" || { cat "$B/adb_test.log"; exit 1; }
tail -1 "$B/adb_test.log"
