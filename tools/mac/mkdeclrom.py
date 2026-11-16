#!/usr/bin/env python3
"""Build the PiStorm-SE slot-$9 declaration ROM.

    mkdeclrom.py --pidisk mac/build/pidisk.bin.raw [--enet mac/build/enet.bin.raw] -o psmac.declrom

sResources (ids ascending):
    1    board:  type {catBoard 1, 0, 0, 0}, name "PiStorm-SE", BoardId (32) word 0x5053 ('PS'), VendorInfo
    0x80 block:  type {0x7050 'pP' (vendor category), 1, 1, 1}, name "PiDisk", sRsrcDrvrDir {sMacOS68020 -> DRVR},
                 flags omitted => opened at start by the Start Manager (OpenSlots), Open adds the drives
    0x81 ethernet (optional): type {catNetwork 4, typeEtherNet 1, drSwApple 1, drHw 0x5053}, name
                 "Network_EtherNet_PiStorm" (driver ".Network_EtherNet_PiStorm"), sRsrcDrvrDir, sRsrcFlags 0 (not
                 opened at start: the .ENET shell opens it through OpenSlot when MacTCP/EtherTalk ask)
    0x83 wifi (optional): the same driver assembled for the board's WiFi unit, type {4, 1, 1, 0x5054},
                 name "Network_WiFi_PiStorm", not opened at start
    0x82 input (optional): type {0x7050 vendor category, 2, 1, 1}, name "PiInput" (driver ".PiInput"),
                 opened at start: keyboards and mice on the Pi (USB, Bluetooth) become Mac events
    0x84 video (optional): type {catDisplay 3, typVideo 1, drSwApple 1, drHw 0x5056}, name
                 "Display_Video_PiStorm" (driver ".Display_Video_PiStorm"), the frame buffer at the window's
                 top 512 KiB, modes 128-131 (1/2/4/8 bits, 640 x 480): the second display (docs/10)
Facts: Inside Macintosh: Devices (Slot Manager), Designing Cards and Drivers 3rd ed., ROMDefs.h.
"""
import argparse, os, struct, sys
sys.path.insert(0, os.path.dirname(__file__))
from declrom import DeclROM

ap = argparse.ArgumentParser()
ap.add_argument('--pidisk', required=True); ap.add_argument('--enet'); ap.add_argument('--wifi'); ap.add_argument('--input'); ap.add_argument('--video'); ap.add_argument('-o', required=True)
a = ap.parse_args()

def drvr_name(img):
    return img[19:19 + img[18]].decode('mac_roman')

rom = DeclROM(byte_lanes=0x0F)
board = rom.sresource(1)
board.type(1, 0, 0, 0)
board.name('PiStorm-SE')
board.word(32, 0x5053)                          # BoardId
vendor = type(board)(0)                         # VendorInfo sub-list
vendor.cstring(1, 'PiStorm-SE project')         # VendorId
vendor.cstring(3, '0.1')                        # RevLevel
vendor.cstring(4, 'PSMAC')                      # PartNum
board.sublist(36, vendor)

pidisk = open(a.pidisk, 'rb').read()
blk = rom.sresource(0x80)
blk.type(0x7050, 1, 1, 1)
blk.name(drvr_name(pidisk)[1:])                 # ".PiDisk" -> "PiDisk"
drv = type(blk)(0); drv.longblock(2, pidisk)    # sMacOS68020
blk.sublist(4, drv)                             # sRsrcDrvrDir
blk.word(8, 1)                                  # sRsrcHWDevId

if a.enet:
    enet = open(a.enet, 'rb').read()
    net = rom.sresource(0x81)
    net.type(4, 1, 1, 0x5053)
    net.name(drvr_name(enet)[1:])
    drv2 = type(net)(0); drv2.longblock(2, enet)
    net.sublist(4, drv2)
    net.word(7, 0)                              # sRsrcFlags: fOpenAtStart clear
    net.word(8, 2)

if a.wifi:
    wifi = open(a.wifi, 'rb').read()
    wn = rom.sresource(0x83)
    wn.type(4, 1, 1, 0x5054)                    # catNetwork, typeEtherNet, drSwApple, drHw 'PT'
    wn.name(drvr_name(wifi)[1:])
    drv4 = type(wn)(0); drv4.longblock(2, wifi)
    wn.sublist(4, drv4)
    wn.word(7, 0)                               # not opened at start
    wn.word(8, 4)

if a.input:
    inp = open(a.input, 'rb').read()
    ir = rom.sresource(0x82)
    ir.type(0x7050, 2, 1, 1)
    ir.name(drvr_name(inp)[1:])
    drv3 = type(ir)(0); drv3.longblock(2, inp)
    ir.sublist(4, drv3)
    ir.word(8, 3)

if a.video:
    # The second display (docs/10): a video sResource shaped exactly like the IIci ROM's own
    # 'Display_Video_Apple_RBV1' for 640 x 480 (tools/declrom.py lists it from the ROM): sRsrcType
    # {catDisplay 3, typVideo 1, drSwApple 1, drHw 'PV'}, the driver, HWDevId, MinorBaseOS/Length
    # (the frame buffer: the window's top 512 KiB, reachable in 24-bit mode too), and one mode
    # sResource per depth (ids 128-131: 1, 2, 4, 8 bits) each with its VPBlock (Designing Cards and
    # Drivers: baseOffset, rowBytes, bounds, version, packType, packSize, hRes, vRes, pixelType,
    # pixelSize, cmpCount, cmpSize, planeBytes), mPageCnt 1 and mDevType 0 (a CLUT device).
    video = open(a.video, 'rb').read()
    vs = rom.sresource(0x84)
    vs.type(3, 1, 1, 0x5056)
    vs.name(drvr_name(video)[1:])
    drv5 = type(vs)(0); drv5.longblock(2, video)
    vs.sublist(4, drv5)
    vs.word(8, 5)                               # sRsrcHWDevId
    vs.long(10, 0x80000)                        # MinorBaseOS: the frame buffer's offset in the slot space
    vs.long(11, 0x80000)                        # MinorLength
    for n, depth in enumerate((1, 2, 4, 8)):
        m = type(vs)(0)
        vp = struct.pack('>IHhhhhHHIIIHHHHI', 0, 80 * depth, 0, 0, 480, 640, 0, 0, 0, 0x00480000, 0x00480000, 0, depth, 1, depth, 0)
        m.longblock(1, vp)                      # mVidParams
        m.word(3, 1)                            # mPageCnt
        m.word(4, 0)                            # mDevType: CLUT
        vs.sublist(128 + n, m)

img = rom.build()
open(a.o, 'wb').write(img)
print(f"{a.o}: {len(img)} bytes, PiDisk driver {len(pidisk)} bytes" + (f", ENET driver {len(enet)} bytes" if a.enet else "") + (f", input driver {len(inp)} bytes" if a.input else "") + (f", WiFi driver {len(wifi)} bytes" if a.wifi else "") + (f", video driver {len(video)} bytes" if a.video else ""))
