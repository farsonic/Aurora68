#!/bin/bash
# SPDX-License-Identifier: MPL-2.0
#
# mkcard.sh: a raw SD card image for the testbed, like the user's card: an MBR, one FAT32
# partition at 1 MiB, the given files in its root.
#
# usage: dev/tools/testbed/mkcard.sh OUT.img FILE...
#   e.g. mkcard.sh card.img cmdline.txt aurora-test3.image riptide-update.img
# Then boot.sh --card OUT.img --container small.cpio ... mac.image=aurora-test3.image,riptide-update.img
# runs the card code (psmac_card.c) as on the Pi: images read from the card in the background,
# pram.bin, pairings.txt and aurora68.log written back. QEMU's SD model wants a power-of-two
# size; the image is the files plus 48 MB of room, rounded up. Keep a pristine copy: a run
# writes to the card. Needs mtools and dosfstools.
set -euo pipefail
[ $# -ge 2 ] || { sed -n '4,13p' "$0"; exit 2; }
OUT=$1; shift
TOTAL=0; for f in "$@"; do TOTAL=$((TOTAL + $(stat -c %s "$f"))); done
MB=$(( TOTAL / 1048576 + 48 )); P=1; while [ $P -lt $MB ]; do P=$((P*2)); done; MB=$P
: > "$OUT"; truncate -s ${MB}M "$OUT"
python3 - "$OUT" $MB <<'PY'
import sys, struct
out, mb = sys.argv[1], int(sys.argv[2])
mbr = bytearray(512)
mbr[446:462] = bytes([0x80, 0, 0, 0, 0x0c, 0, 0, 0]) + struct.pack('<II', 2048, mb * 2048 - 2048)  # FAT32 LBA
mbr[510:512] = b'\x55\xaa'
with open(out, 'r+b') as f:
    f.write(mbr)
PY
mkfs.vfat -F 32 -s 1 --offset 2048 -n AURORA68 "$OUT" $(( MB * 1024 - 1024 )) > /dev/null
for f in "$@"; do mcopy -i "$OUT@@1M" "$f" ::; done
echo "$OUT: $MB MB"; mdir -i "$OUT@@1M" -b ::
