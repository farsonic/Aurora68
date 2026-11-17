#!/usr/bin/env python3
"""Build a 'scrn' resource: the screen configuration the Monitors control panel saves in the
System file, which is the only way QuickDraw activates a second screen at boot.

    mkscrn.py -o scrn.bin \\
        --screen hw=0x18,slot=11,base=0xFBB08000,mode=128,flags=0x0800,rect=0,0,480,640 \\
        --screen hw=0x5056,slot=9,base=0xF9980000,mode=131,flags=0,rect=0,640,480,1280

Format and rules, read out of the Macintosh IIci ROM's loader (0x40827BA0 in ROM 368CADFE, the
routine InitGraf runs after the Start Manager; docs/10). Apple documented the resource's existence
and nothing else ("its structure is not described here", Inside Macintosh: Imaging With QuickDraw).
    count.w
    per screen, in the order sNextTypeSRsrc (category 3, type 1, software 1) returns the video
    sResources:
        spDrvrHW.w        must equal the sResource's hardware id
        spSlot.w          must equal its slot (compared as a byte)
        dCtlDevBase.l     must equal the driver's DCE dCtlDevBase
        mode.w            the mode sResource id to set (128 one bit ... 131 eight bits)
        flagMask.w        gdFlags = (gdFlags & flagMask) | flags | ramInit | screenDevice
        flags.w           bit 15 screenActive, bit 11 mainScreen
        colorTable.w      'clut' resource id, -1 for the default of the depth
        gammaTable.w      'gama' resource id, -1 for none, -2 for the driver's own
        rect              8 bytes: top, left, bottom, right on the desktop; height and width
                          must equal the device's own (the ROM compares them before anything)
        ctlCount.w        then per control: csCode.w, length.w, data: Control calls to the driver
The loader insists on the count matching the number of video sResources and on every entry
matching; otherwise it places the screens side by side and leaves all but the startup one
inactive, which is what the Pi's screen looked like before this tool existed.
Two things the first attempts taught: the loader runs from InitGraf, which every program calls,
and skips itself only while bit 12 (allInit) is set on the first device; a mask that clears that
bit makes the next program's InitGraf run the loader again, where the "width must equal
gdRect.right" test fails for any screen not at the origin and the mismatch path shifts it
sideways once more (our screen went from 640 to 1280). So keep bit 12 in the mask (0x1000),
and bit 0 (gdDevType, 1 = colour) with it: mask=0x1001, and flags 0x8001 for an active colour
screen, 0x8800 for the active main one.
"""
import argparse, struct

ap = argparse.ArgumentParser()
ap.add_argument('--screen', action='append', required=True, help='hw=,slot=,base=,mode=,flags=,[mask=],[clut=],[gamma=],rect=t,l,b,r')
ap.add_argument('-o', required=True)
a = ap.parse_args()

out = struct.pack('>H', len(a.screen))
for spec in a.screen:
    kv = {}
    for part in spec.split(','):
        if '=' in part: k, v = part.split('=', 1); kv[k] = v; last = k
        else: kv[last] += ',' + part                   # rect=0,0,480,640 keeps its commas
    rect = [int(x, 0) for x in kv['rect'].split(',')]
    out += struct.pack('>HHIHHHhh4hH', int(kv['hw'], 0), int(kv['slot'], 0), int(kv['base'], 0), int(kv.get('mode', '128'), 0),
                       int(kv.get('mask', '0x0000'), 0), int(kv['flags'], 0), int(kv.get('clut', '-1'), 0), int(kv.get('gamma', '-1'), 0),
                       *rect, 0)
open(a.o, 'wb').write(out)
print(f"{a.o}: {len(a.screen)} screens, {len(out)} bytes")
