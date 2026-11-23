#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# tools/bt_test.sh BUILD - the kernel's Bluetooth host on the PC (tools/bt_test.c): AES and address resolution against the
# specifications' examples, a bonded LE mouse with a private address on a controller without the resolving list (the
# Pi 3B's), the firmware download's speed, the page scan's settings. Called by `make -C mac check`.
set -e
B=${1:-/tmp/bt_test}
mkdir -p "$B"
BT=../emu68/src/alice/bt
cc -std=c99 -O1 -g -Wall -Wextra -Werror -I$BT -o "$B/bt_test" ../tools/bt_test.c $BT/bthost.c
"$B/bt_test"
