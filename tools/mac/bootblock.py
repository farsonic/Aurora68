#!/usr/bin/env python3
"""Show or set the startup names in an HFS volume's boot blocks (the first two sectors of the
partition): the System file, the shell, the startup application, the debuggers, the startup screen,
the clipboard file.

    bootblock.py image.hda                      show the names (partition 1, found through the APM)
    bootblock.py image.hda --startup PiNet      launch "PiNet" at boot, the Finder when it quits
    bootblock.py image.hda --startup Finder     back to normal

History. The System 6 Finder's "Set Startup..." writes the chosen application into bbHelloName
(offset $5A): the boot code launches that program first and runs the shell named in bbShellName
($1A, "Finder") when it quits. The first attempt at an unattended PiNet launch put the name into
bbShellName instead, and the boot code dutifully ran the Finder named in bbHelloName. Inside
Macintosh: Files, "Boot Blocks"; the record is bbID 'LK', bbEntry, bbVersion, bbPageFlags, then
seven 16-byte Pascal strings: System, shell, debugger, disassembler, startup screen, startup
application ("hello"), clipboard.
"""
import argparse, struct

NAMES = [('bbSysName', 0x0A), ('bbShellName', 0x1A), ('bbDbg1Name', 0x2A), ('bbDbg2Name', 0x3A),
         ('bbScreenName', 0x4A), ('bbHelloName', 0x5A), ('bbScrapName', 0x6A)]

def hfs_partition_start(f):
    """Byte offset of the first Apple_HFS partition, or 0 for a bare volume."""
    f.seek(0); dd = f.read(2)
    if dd != b'ER':
        return 0
    i = 1
    while True:
        f.seek(512 * i); e = f.read(512)
        if e[:2] != b'PM': break
        cnt, start = struct.unpack('>II', e[4:12])
        if e[48:80].rstrip(b'\0') == b'Apple_HFS':
            return start * 512
        i += 1
        if i > cnt: break
    raise SystemExit("no Apple_HFS partition in the partition map")

ap = argparse.ArgumentParser()
ap.add_argument('image'); ap.add_argument('--startup', help='the application launched at boot (bbHelloName)')
ap.add_argument('--shell', help='the shell (bbShellName), normally Finder')
a = ap.parse_args()
with open(a.image, 'r+b') as f:
    base = hfs_partition_start(f)
    f.seek(base); bb = bytearray(f.read(1024))
    if bb[:2] != b'LK':
        raise SystemExit("no boot blocks (bbID is not 'LK') at offset %d" % base)
    def setname(off, text):
        s = text.encode('mac_roman')[:15]
        bb[off:off + 16] = bytes([len(s)]) + s + b'\0' * (15 - len(s))
    if a.startup: setname(0x5A, a.startup)
    if a.shell: setname(0x1A, a.shell)
    if a.startup or a.shell:
        f.seek(base); f.write(bb)
    print("boot blocks at %d: version %04x" % (base, struct.unpack('>H', bb[6:8])[0]))
    for name, off in NAMES:
        n = bb[off]
        print("  %-13s %s" % (name, bb[off + 1:off + 1 + n].decode('mac_roman', 'replace')))
