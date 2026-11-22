#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# cmdline_test.sh [BUILD_DIR] - the word editor Aurora68 Recovery changes cmdline.txt with
# (emu68/src/psmac/psmac_cmdline.c, docs/08 item 106) on the host: unknown options and spacing kept,
# quoting, one line. Run by `make -C mac check`.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
B=${1:-$ROOT/mac/build}
mkdir -p "$B"
cc -std=c99 -O1 -Wall -Wextra -Werror -o "$B/cmdline_test" "$ROOT/tools/cmdline_test.c" "$ROOT/emu68/src/psmac/psmac_cmdline.c"
"$B/cmdline_test"
