#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# hfsview_test.sh [BUILD_DIR] - the kernel's FAT/exFAT reader and HFS view (issue #20) on the host:
# FAT32 and exFAT images made from the same test tree (tools/hfsview_check.py tree) by mkfs.vfat + mcopy
# and by tools/mkexfat.py (checked by fsck.exfat when exfatprogs is there), each turned into the HFS
# volume the Mac would see by tools/hfsview_test.c (the kernel's own sources), and that volume read back
# with hfsutils: every name, every file's data fork, and the Mac files' resource forks, types and
# creators. Run by `make -C mac check`; skipped, with a line saying so, when a tool is missing.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
B=${1:-$ROOT/mac/build}/hfsview
for t in cc python3 mkfs.vfat mcopy mmd mattrib hmount hls hcopy humount; do
    command -v $t > /dev/null || { echo "hfsview_test: skipped, no $t (dosfstools, mtools, hfsutils)"; exit 0; }
done
mkdir -p "$B"
# hfsutils remembers the mounted volume in $HOME/.hcwd: a HOME of the test's own, so nothing else is touched
HOME=$(mktemp -d "$B/home.XXXXXX"); export HOME
LC_ALL=C.UTF-8; export LC_ALL
cc -std=c99 -Wall -Wextra -Werror -O1 -g -I "$ROOT/emu68/src/psmac" -o "$B/hfsview_test" "$ROOT/tools/hfsview_test.c" \
   "$ROOT/emu68/src/psmac/psmac_fatfs.c" "$ROOT/emu68/src/psmac/psmac_hfsview.c"
# two trees: a few hundred files in one folder for FAT (mcopy takes minutes for thousands), ten thousand for exFAT
T=$B/tree.$$; TX=$B/treex.$$
python3 "$ROOT/tools/hfsview_check.py" tree "$T" --many ${MANY_FAT:-300}
python3 "$ROOT/tools/hfsview_check.py" tree "$TX" --many ${MANY_EXFAT:-10000}

# FAT32 with an MBR, as a PC or a Mac formats a stick ("MS-DOS (FAT)", Master Boot Record): every folder's
# files copied in sorted order, so that the directory order is the one the check expects
F=$B/fat32.img
: > "$F"; truncate -s 64M "$F"
python3 -c "
import struct,sys
m=bytearray(512); m[446:462]=bytes([0,0,0,0,0x0c,0,0,0])+struct.pack('<II',2048,64*2048-2048); m[510:512]=b'\x55\xaa'
open(sys.argv[1],'r+b').write(m)" "$F"
mkfs.vfat -F 32 -s 1 --offset 2048 -n EXCHANGE "$F" $((63 * 1024)) > /dev/null
python3 - "$F@@1M" "$T" <<'EOF'
import os, subprocess, sys
img, src = sys.argv[1], sys.argv[2]
def put(d, rel):
    names = sorted(os.listdir(d))
    files = [os.path.join(d, n) for n in names if os.path.isfile(os.path.join(d, n))]
    dest = '::' + rel
    for n in names:
        p = os.path.join(d, n)
        if os.path.isdir(p):
            subprocess.run(['mmd', '-i', img, '::' + (rel + '/' if rel else '') + n], check=True)
    # each file in order (one mcopy per directory keeps the order of its arguments)
    if files:
        subprocess.run(['mcopy', '-i', img] + files + [dest + ('/' if rel else '')], check=True)
    for n in names:
        p = os.path.join(d, n)
        if os.path.isdir(p):
            put(p, (rel + '/' if rel else '') + n)
put(src, '')
EOF
echo "fat32: image made"
"$B/hfsview_test" "$F" "$B/fat32.hfs"
python3 "$ROOT/tools/hfsview_check.py" check "$B/fat32.hfs" "$T"

# exFAT, a volume with no partition table and one in an MBR partition, names as macOS writes them
# (decomposed, NFD) in the second
E=$B/exfat.img
python3 "$ROOT/tools/mkexfat.py" "$E" 400 "$TX" --label "Stick" --cluster 32768
if command -v fsck.exfat > /dev/null; then fsck.exfat -n "$E" | tail -1; fi
"$B/hfsview_test" "$E" "$B/exfat.hfs"
python3 "$ROOT/tools/hfsview_check.py" check "$B/exfat.hfs" "$TX"
python3 "$ROOT/tools/mkexfat.py" "$E" 96 "$T" --label "Mac Stick" --mbr --nfd
"$B/hfsview_test" "$E" "$B/exfat-mbr.hfs"
python3 "$ROOT/tools/hfsview_check.py" check "$B/exfat-mbr.hfs" "$T"

# more than one HFS volume holds (2 GB on System 7.1): three files of 900 MB (sparse on the host: data at
# their start and end only), a small one between and after them. The largest file found last is left out,
# the smallest allocation block that holds the rest is used, and the files that stay are read back there
G=$B/big.$$; mkdir -p "$G/Big"
for f in a b c; do
    truncate -s 900M "$G/Big/$f.bin"
    printf 'start of %s' $f | dd of="$G/Big/$f.bin" conv=notrunc status=none
    printf 'end of %s' $f | dd of="$G/Big/$f.bin" bs=1 seek=$((900 * 1048576 - 8)) conv=notrunc status=none
done
head -c 3000000 /dev/urandom > "$G/Big/m.bin"; echo small > "$G/Big/zz small.txt"
python3 "$ROOT/tools/mkexfat.py" "$E" 3000 "$G" --label "Big Stick" --cluster 131072
"$B/hfsview_test" "$E" "$B/big.hfs" | tail -2
hmount "$B/big.hfs" > /dev/null
hls -1 :Big > "$B/big.ls"
printf 'a.bin\nb.bin\nm.bin\nzz small.txt\n' | cmp - "$B/big.ls"
hcopy -r ":Big:m.bin" "$B/big.m" && cmp "$B/big.m" "$G/Big/m.bin"
[ "$(hcopy -r ':Big:zz small.txt' -)" = small ]
# b.bin through a pipe: its 900 MB never land on the disk
[ "$(hcopy -r ":Big:b.bin" - | head -c 10)" = "start of b" ] && [ "$(hcopy -r ":Big:b.bin" - | tail -c 8)" = "end of b" ]
humount
echo "big: c.bin (the largest, found last) left out, a.bin, b.bin, m.bin and zz small.txt read back as written"
mv "$G" "$B/big.old.$$"

# a folder of the volume as the volume: the SD card's exchange folder
mmd -i "$F@@1M" "::Mac Exchange"
mcopy -i "$F@@1M" "$T/ReadMe.txt" "::Mac Exchange/"
"$B/hfsview_test" "$F" "$B/folder.hfs" --folder "Mac Exchange" | tail -2
hmount "$B/folder.hfs" | grep -q 'Mac Exchange' && [ "$(hls -1)" = ReadMe.txt ] && humount && echo "folder: the volume is \"Mac Exchange\" with ReadMe.txt in it"
# the trees are kept for a look (never deleted here: moved aside, the newest as tree.last)
for d in tree treex; do
    [ -d "$B/$d.last" ] && mv "$B/$d.last" "$B/$d.old.$$"
    mv "$B/$d.$$" "$B/$d.last"
done
echo "hfsview_test: all passed"
