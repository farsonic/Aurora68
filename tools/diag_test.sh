#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# diag_test.sh [BUILD_DIR] - Setup and Recovery's options table and validator, the one-shot debug start's
# words and the report bundle's scrubbing (emu68/src/psmac/psmac_diag.c, docs/08 item 122) on the host; then
# the page and its header: tools/recovery/mkpage.py must make exactly the psmac_recovery_page.h in the tree.
# Run by `make -C mac check`.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
B=${1:-$ROOT/mac/build}
mkdir -p "$B"
cc -std=c99 -O1 -Wall -Wextra -Werror -I "$ROOT/emu68/src/psmac" -o "$B/diag_test" "$ROOT/tools/diag_test.c" \
   "$ROOT/emu68/src/psmac/psmac_diag.c" "$ROOT/emu68/src/psmac/psmac_cmdline.c"
"$B/diag_test" > "$B/diag_test.log" || { cat "$B/diag_test.log"; exit 1; }
tail -1 "$B/diag_test.log"
python3 "$ROOT/tools/recovery/mkpage.py" --check
