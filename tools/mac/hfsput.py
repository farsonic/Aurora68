#!/usr/bin/env python3
"""Copy a MacBinary file into a folder of an HFS volume image (or a partition of a .hda/.img)
using hfsutils (hmount/hcopy/humount).   hfsput.py image file.bin "System Folder" [partition]"""
import subprocess, sys, os
img, src, folder = sys.argv[1], sys.argv[2], sys.argv[3]
part = sys.argv[4] if len(sys.argv) > 4 and sys.argv[4] else None
def run(*cmd): return subprocess.run(cmd, check=True, capture_output=True, text=True)
run('hmount', img, *([part] if part else []))
try:
    dest = ':' + folder.replace('/', ':') + ':' if folder else ':'
    run('hcopy', '-m', src, dest)
    print(run('hls', '-l', dest).stdout)
finally:
    run('humount')
