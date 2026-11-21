#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# fatfs_test.sh [BUILD_DIR] - the kernel's FAT/exFAT reader (emu68/src/psmac/psmac_fatfs.c, issue #20) on
# the host: one test tree (tools/hfsview_check.py tree: long, accented and non-Latin names, AppleDouble
# files, 20 folders deep, a few hundred files in one folder, a fragmented file) as FAT16 and FAT32 made
# by mkfs.vfat and mcopy, and as exFAT made by tools/mkexfat.py (with and without a partition table,
# contiguous and FAT-chained files, names composed and decomposed, in a GPT after an EFI partition); every file found by the reader with
# the bytes it holds (tools/fatfs_test.c). Run by `make -C mac check`; skipped when a tool is missing.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
B=${1:-$ROOT/mac/build}/hfsview
for t in cc python3 mkfs.vfat mcopy mmd; do
    command -v $t > /dev/null || { echo "fatfs_test: skipped, no $t (dosfstools, mtools)"; exit 0; }
done
mkdir -p "$B"
LC_ALL=C.UTF-8; export LC_ALL
cc -std=c99 -Wall -Wextra -Werror -O1 -g -I "$ROOT/emu68/src/psmac" -o "$B/fatfs_test" "$ROOT/tools/fatfs_test.c" "$ROOT/emu68/src/psmac/psmac_fatfs.c"
T=$B/ftree.$$
python3 "$ROOT/tools/hfsview_check.py" tree "$T" --many 300
# what the tree holds, as fatfs_test prints it (names NFC on both sides: macOS writes them decomposed)
python3 - "$T" > "$B/ftree.want" <<'EOF'
import os, sys, unicodedata
root = sys.argv[1]
for d, ds, fs in os.walk(root):
    for f in fs:
        p = os.path.join(d, f)
        h = 1469598103934665603
        for b in open(p, 'rb').read():
            h = ((h ^ b) * 1099511628211) & 0xffffffffffffffff
        print('%s\t%d\t%016x' % (unicodedata.normalize('NFC', os.path.relpath(p, root)), os.path.getsize(p), h))
EOF
sort -o "$B/ftree.want" "$B/ftree.want"
# mtools 4.0 stores a character beyond Unicode's first plane (the test's emoji, U+1F3B5) as its low 16
# bits (U+F3B5) in a long name: for the images it made, the expected names say so too
python3 -c "
import sys
t = open(sys.argv[1], encoding='utf-8').read()
open(sys.argv[2], 'w', encoding='utf-8').write(''.join(chr(ord(c) & 0xffff) if ord(c) > 0xffff else c for c in t))" "$B/ftree.want" "$B/ftree.want16"
check() {   # IMAGE WHAT [16]
    "$B/fatfs_test" "$1" > "$B/ftree.got"
    python3 -c "
import sys, unicodedata
lines = [unicodedata.normalize('NFC', l) for l in open(sys.argv[1], encoding='utf-8')]
open(sys.argv[1], 'w', encoding='utf-8').writelines(sorted(lines))" "$B/ftree.got"
    if cmp -s "$B/ftree.want$3" "$B/ftree.got"; then echo "fatfs_test: $2: all $(wc -l < "$B/ftree.want") files found, bytes as written"
    else echo "fatfs_test: $2: FAIL"; diff "$B/ftree.want$3" "$B/ftree.got" | head -10; exit 1; fi
}
fat() {     # FATBITS IMAGE: an MBR, the volume at 1 MiB, every folder copied in sorted order
    : > "$2"; truncate -s 64M "$2"
    python3 -c "
import struct,sys
m=bytearray(512); m[446:462]=bytes([0,0,0,0,0x0c if sys.argv[2]=='32' else 0x0e,0,0,0])+struct.pack('<II',2048,64*2048-2048); m[510:512]=b'\x55\xaa'
open(sys.argv[1],'r+b').write(m)" "$2" "$1"
    mkfs.vfat -F "$1" -s ${3:-1} --offset 2048 -n EXCHANGE "$2" $((63 * 1024)) > /dev/null
    python3 - "$2@@1M" "$T" <<'EOF'
import os, subprocess, sys
img, src = sys.argv[1], sys.argv[2]
def put(d, rel):
    names = sorted(os.listdir(d))
    files = [os.path.join(d, n) for n in names if os.path.isfile(os.path.join(d, n))]
    for n in names:
        if os.path.isdir(os.path.join(d, n)):
            subprocess.run(['mmd', '-i', img, '::' + (rel + '/' if rel else '') + n], check=True)
    if files:
        subprocess.run(['mcopy', '-i', img] + files + ['::' + rel + ('/' if rel else '')], check=True)
    for n in names:
        if os.path.isdir(os.path.join(d, n)):
            put(os.path.join(d, n), (rel + '/' if rel else '') + n)
put(src, '')
EOF
}
fat 32 "$B/f32.img"; check "$B/f32.img" "FAT32 (mkfs.vfat, mcopy)" 16
fat 16 "$B/f16.img" 4; check "$B/f16.img" "FAT16 (mkfs.vfat, mcopy)" 16
python3 "$ROOT/tools/mkexfat.py" "$B/fx.img" 64 "$T" > /dev/null
if command -v fsck.exfat > /dev/null; then fsck.exfat -n "$B/fx.img" | tail -1; fi
check "$B/fx.img" "exFAT, no partition table"
python3 "$ROOT/tools/mkexfat.py" "$B/fx.img" 64 "$T" --mbr --nfd --cluster 65536 > /dev/null
check "$B/fx.img" "exFAT in an MBR partition, decomposed names, 64 KB clusters"
python3 "$ROOT/tools/mkexfat.py" "$B/fx.img" 96 "$T" --gpt --nfd > /dev/null
mkfs.vfat -F 16 --offset 2048 -n EFI "$B/fx.img" 20480 > /dev/null 2>&1
check "$B/fx.img" "exFAT in a GPT partition after an EFI partition (FAT16), as Disk Utility makes a stick"
[ -d "$B/ftree.last" ] && mv "$B/ftree.last" "$B/ftree.old.$$"
mv "$T" "$B/ftree.last"
echo "fatfs_test: all passed"
