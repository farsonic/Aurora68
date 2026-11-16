#!/usr/bin/env python3
"""romlanes.py - split a Macintosh IIci ROM image into the four byte-lane files MAME's
`maciici` set expects (lane k = bytes k, k+4, k+8, ...), or join them back.

usage: romlanes.py ROM.rom OUTDIR          split (writes 341-0736.um12, 341-0735.um11,
                                                  342-0734.um10, 342-0733.um9 to OUTDIR)
       romlanes.py --join LANEDIR OUT.rom  join the four lane files into one image

MAME's region is 0x80000 bytes (4 x 0x20000); a ROM that has grown past 512 KiB is
split in full but only the first 128 KiB per lane are used by MAME (warning printed).
"""
import os
import sys

LANES = ["341-0736.um12", "341-0735.um11", "342-0734.um10", "342-0733.um9"]
MAME_LANE = 0x20000


def main():
    if len(sys.argv) == 4 and sys.argv[1] == "--join":
        lanes = [open(os.path.join(sys.argv[2], n), "rb").read() for n in LANES]
        if len({len(l) for l in lanes}) != 1:
            sys.exit("lane files differ in size")
        out = bytearray(len(lanes[0]) * 4)
        for k, l in enumerate(lanes):
            out[k::4] = l
        with open(sys.argv[3], "wb") as f:
            f.write(out)
        print("joined %d bytes -> %s" % (len(out), sys.argv[3]))
        return
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    rom = open(sys.argv[1], "rb").read()
    if len(rom) % 4:
        sys.exit("ROM size is not a multiple of 4")
    os.makedirs(sys.argv[2], exist_ok=True)
    for k, name in enumerate(LANES):
        with open(os.path.join(sys.argv[2], name), "wb") as f:
            f.write(rom[k::4])
    print("split %s (%d bytes) -> %s/{%s} (%d bytes each)" % (
        sys.argv[1], len(rom), sys.argv[2], ",".join(LANES), len(rom) // 4))
    if len(rom) // 4 != MAME_LANE:
        print("WARNING: MAME expects %d bytes per lane; it will load only the first "
              "0x%X bytes of the ROM" % (MAME_LANE, MAME_LANE * 4))


if __name__ == "__main__":
    main()
