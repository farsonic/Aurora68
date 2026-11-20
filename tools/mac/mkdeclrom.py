#!/usr/bin/env python3
"""Build the PiStorm-SE slot-$9 declaration ROM.

    mkdeclrom.py --pidisk mac/build/pidisk.bin.raw [--enet mac/build/enet.bin.raw] -o psmac.declrom

sResources (ids ascending):
    1    board:  type {catBoard 1, 0, 0, 0}, name "PiStorm-SE", BoardId (32) word 0x5053 ('PS'), VendorInfo
    0x80 block:  type {0x7050 'pP' (vendor category), 1, 1, 1}, name "PiDisk", sRsrcDrvrDir {sMacOS68020 -> DRVR},
                 flags omitted => opened at start, once the System file is open (the ROM at 0x40802240), Open adds
                 the drives; with --boot also sRsrcBootRec (6), an sExec block (mac/pidisk_boot.s) that opens the
                 driver itself, both when the Start Manager looks for a startup disk on this card (the default
                 startup device in parameter RAM names slot 9, sResource $80) and in place of that later open:
                 how a disk image on the card becomes the startup disk (docs/08 item 67)
    0x81 ethernet (optional): type {catNetwork 4, typeEtherNet 1, drSwApple 1, drHw 0x5053}, name
                 "Network_EtherNet_PiStorm" (driver ".Network_EtherNet_PiStorm"), sRsrcDrvrDir, sRsrcFlags 0 (not
                 opened at start: the .ENET shell opens it through OpenSlot when MacTCP/EtherTalk ask)
    0x83 wifi (optional): the same driver assembled for the board's WiFi unit, type {4, 1, 1, 0x5054},
                 name "Network_WiFi_PiStorm", not opened at start
    0x82 input (optional): type {0x7050 vendor category, 2, 1, 1}, name "PiInput" (driver ".PiInput"),
                 opened at start: keyboards and mice on the Pi (USB, Bluetooth) become Mac events
    0x84 video (optional): type {catDisplay 3, typVideo 1, drSwApple 1, drHw 0x5056}, name
                 "Display_Video_PiStorm" (driver ".Display_Video_PiStorm"), modes 128-133 (1/2/4/8 bits indexed,
                 16/32 bits direct) at the size --res gives (640x480 by default; one file per size, the
                 kernel takes the one named for the screen in use): the second display (docs/10)
--se with --video2: the SE board's two cards (docs/10, "Two screens on the SE"), in one file. Slot 9's ROM
    as above with 0x84 the SE's own 512 x 342 screen, mode 128 (one bit) only, its frame the first 32 KiB
    of the window's top 512 KiB; before it, slot $A's ROM, the HDMI screen's card: board sResource 1
    ("PiStorm-SE HDMI", BoardId 0x5048) and 0x80, type {3, 1, 1, drHw 0x5048 'PH'}, name
    "Display_Video_PiHDMI", every mode at the size --res gives, its indexed modes in the window's other
    480 KiB where they fit, the rest in the big region (vpBaseOffsets reach from slot $A's dCtlDevBase,
    $FAA80000, into slot 9's space). The kernel serves slot $A's ROM on the SE board with mac.hdmi=screen.
Facts: Inside Macintosh: Devices (Slot Manager), Designing Cards and Drivers 3rd ed., ROMDefs.h.
"""
import argparse, os, struct, sys
sys.path.insert(0, os.path.dirname(__file__))
from declrom import DeclROM

ap = argparse.ArgumentParser()
ap.add_argument('--pidisk', required=True); ap.add_argument('--boot', help='PiDisk\'s boot record (mac/pidisk_boot.s assembled)'); ap.add_argument('--enet'); ap.add_argument('--wifi'); ap.add_argument('--input'); ap.add_argument('--video'); ap.add_argument('--res', default='640x480', help='the second display\'s size, WxH (the kernel picks the matching file from the container)'); ap.add_argument('-o', required=True)
ap.add_argument('--se', action='store_true', help='the SE board\'s card: --video is the SE\'s 512x342 one-bit screen, --video2 the HDMI screen at --res')
ap.add_argument('--video2', help='the HDMI screen\'s driver (mac/pivideo.s assembled with SCREEN2), with --se')
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
if a.boot:
    blk.sexec(6, open(a.boot, 'rb').read())     # sRsrcBootRec: sExec block, revision 2, sCPU68020 (the ROM takes CPU ids up to 2)
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

GAMMA = bytes.fromhex("20004d6163204869526573205374642047616d6d61000000000000000001010000080005090b0e10131517191b1d1e2022242527282a2c2d2f303133343637383a3b3c3e3f40424344454748494a4b4d4e4f5051525455565758595a5b5c5e5f606162636465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f80818182838485868788898a8b8c8c8d8e8f90919293949595969798999a9b9b9c9d9e9fa0a1a1a2a3a4a5a6a6a7a8a9aaababacadaeafb0b0b1b2b3b4b4b5b6b7b8b8b9babbbcbcbdbebfc0c0c1c2c3c3c4c5c6c7c7c8c9cacacbcccdcdcecfd0d0d1d2d3d3d4d5d6d6d7d8d9d9dadbdcdcdddedfdfe0e1e1e2e3e4e4e5e6e7e7e8e9e9eaebececedeeeeeff0f1f1f2f3f3f4f5f5f6f7f8f8f9fafafbfcfcfdfeff")

def add_video(rom, rid, video, drhw, hwdev, W, H, win, win_size, one_bit_only=False, devbase=None):
    """One video sResource: the driver, its frame buffer (win, win_size: in the window, as the Mac
    addresses it) and a mode sResource per depth. devbase is the card's dCtlDevBase, slot space plus
    MinorBaseOS, which every mode's vpBaseOffset counts from: the frame buffer itself by default."""
    devbase = win if devbase is None else devbase
    vs = rom.sresource(rid)
    vs.type(3, 1, 1, drhw)
    vs.name(drvr_name(video)[1:])
    drv5 = type(vs)(0); drv5.longblock(2, video)
    vs.sublist(4, drv5)
    vs.word(8, hwdev)                           # sRsrcHWDevId
    # MinorBaseOS: NuBus geometry puts the megabyte a 24-bit system can reach ($900000-$9FFFFF for
    # slot 9) at $F9900000-$F99FFFFF of the slot's 32-bit space; the kernel maps the window there
    # as well as at $F9000000 (psmac_mmu.c), and the frame buffer is its top 512 KiB.
    vs.long(10, devbase & 0xFFFFFF)             # MinorBaseOS: from the slot's space, $Fs000000
    vs.long(11, win_size)                       # MinorLength
    # sGammaDir: the IIci ROM's own "HiRes Std Gamma" table, copied from its RBV sResource; the ROM's
    # screen set-up reads a gamma table from the card and gives up on the screen without one.
    gd = type(vs)(0); gd.longblock(128, GAMMA)
    vs.sublist(64, gd)
    # modes 128-133: 1, 2, 4, 8 bits indexed (vpPixelType 0, one component), 16 and 32 bits direct
    # (vpPixelType 16 RGBDirect, three components of 5 or 8 bits). The ROM's InitGDevice never asks
    # the driver where a mode's pixels are: it adds the mode's vpBaseOffset to dCtlDevBase
    # ($F9980000, MinorBaseOS in 32-bit form). So the offset must say what the kernel decided
    # (psmac_display.c, the same rule): a mode of at most 8 bits that fits the window's 512 KiB
    # stays there (offset 0, reachable in 24-bit mode too); anything larger lives in the big
    # region at $90000000, 9 MiB of Pi RAM in the slot's super slot space, offset $90000000 -
    # $F9980000 (the sum wraps round to the right address in 32 bits). Every size mac.screen
    # accepts (up to 1920 x 1200) fits there at 32 bits; a larger one is refused. Without the offset
    # the first 32-bit boot drew into the window and past it while the Pi showed the big region:
    # a grey screen (docs/08, item 27). Until 18 November the big region was 8 MiB at $F9100000,
    # too small for 1920 x 1200 at 32 bits (docs/10, "1920 by 1200"). The SE card's two screens
    # share the window's 512 KiB (win, win_size: psmb.h).
    BIG, BIG_SIZE = 0x90000000, 0x900000        # psmb.h: PSMB_FB_BIG_PHYS, PSMB_FB_BIG_SIZE
    for n, depth in enumerate((1, 2, 4, 8, 16, 32)):
        if one_bit_only and depth > 1:
            break
        m = type(vs)(0)
        direct = depth >= 16
        rowbytes = (W * depth + 7) // 8
        in_window = depth <= 8 and rowbytes * H <= win_size
        if not in_window and (one_bit_only or rowbytes * H > BIG_SIZE):
            sys.exit(f"mkdeclrom: {W} x {H} at {depth} bits needs {rowbytes * H} bytes, the big region has {BIG_SIZE}")
        base_off = ((win if in_window else BIG) - devbase) & 0xffffffff
        vp = struct.pack('>IHhhhhHHIIIHHHHI', base_off, rowbytes, 0, 0, H, W, 0, 0, 0, 0x00480000, 0x00480000,
                         16 if direct else 0, depth, 3 if direct else 1, (5 if depth == 16 else 8) if direct else depth, 0)
        m.longblock(1, vp)                      # mVidParams
        m.word(3, 1)                            # mPageCnt
        m.word(4, 2 if direct else 0)           # mDevType: 0 CLUT, 2 direct
        vs.sublist(128 + n, m)

# The second display (docs/10): a video sResource shaped exactly like the IIci ROM's own
# 'Display_Video_Apple_RBV1' for 640 x 480 (tools/declrom.py lists it from the ROM): sRsrcType
# {catDisplay 3, typVideo 1, drSwApple 1, drHw 'PV'}, the driver, HWDevId, MinorBaseOS/Length
# (the frame buffer: the window's top 512 KiB, reachable in 24-bit mode too), and one mode
# sResource per depth (ids 128-133) each with its VPBlock (Designing Cards and Drivers: baseOffset,
# rowBytes, bounds, version, packType, packSize, hRes, vRes, pixelType, pixelSize, cmpCount,
# cmpSize, planeBytes), mPageCnt 1 and mDevType 0 (a CLUT device) or 2 (direct).
W, H = (int(x) for x in a.res.lower().split('x'))
WIN, WIN_SIZE = 0xF9980000, 0x80000             # psmb.h: the window's top 512 KiB in its $F99 form
SE_FRAME = 0x8000                               # psmb.h: PSMB_SE_FRAME_SIZE, the SE's screen's share of it
rom2 = None
if a.se:
    if not (a.video and a.video2):
        sys.exit("mkdeclrom: --se needs --video (the SE's screen) and --video2 (the HDMI screen)")
    add_video(rom, 0x84, open(a.video, 'rb').read(), 0x5056, 5, 512, 342, WIN, SE_FRAME, one_bit_only=True)
    # slot $A: the HDMI screen's card, a board sResource and the video sResource, nothing else
    video = open(a.video2, 'rb').read()
    rom2 = DeclROM(byte_lanes=0x0F)
    b2 = rom2.sresource(1)
    b2.type(1, 0, 0, 0)
    b2.name('PiStorm-SE HDMI')
    b2.word(32, 0x5048)                         # BoardId 'PH'
    v2 = type(b2)(0)
    v2.cstring(1, 'PiStorm-SE project'); v2.cstring(3, '0.1'); v2.cstring(4, 'PSMAC-HDMI')
    b2.sublist(36, v2)
    add_video(rom2, 0x80, video, 0x5048, 1, W, H, WIN + SE_FRAME, WIN_SIZE - SE_FRAME, devbase=0xFAA80000)
elif a.video:
    video = open(a.video, 'rb').read()
    add_video(rom, 0x84, video, 0x5056, 5, W, H, WIN, WIN_SIZE)

img = rom.build()
if rom2:
    # slot $A's ROM first, slot 9's at the end, where the kernel expects the file's top (psmb.h): the
    # kernel finds slot $A's by slot 9's length and serves it from 16 KiB of the window
    img2 = rom2.build()
    if len(img2) > 0x4000:
        sys.exit(f"mkdeclrom: slot $A's declaration ROM is {len(img2)} bytes, the kernel has 16 KiB for it")
    img = img2 + img
open(a.o, 'wb').write(img)
print(f"{a.o}: {len(img)} bytes, PiDisk driver {len(pidisk)} bytes" + (" with its boot record" if a.boot else "") + (f", ENET driver {len(enet)} bytes" if a.enet else "") + (f", input driver {len(inp)} bytes" if a.input else "") + (f", WiFi driver {len(wifi)} bytes" if a.wifi else "") + (f", video driver {len(video)} bytes" if a.video else "") + (f", and slot $A's ROM ({len(img2)} bytes): the SE's screen and HDMI" if a.se else ""))
