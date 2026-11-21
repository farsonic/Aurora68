# Mac-side software: drivers, extension, test application, declaration ROM

Everything the Macintosh runs to talk to the Pi lives in `mac/` (68k assembly, GNU `as` MIT
syntax, `binutils-m68k-linux-gnu`) and is packaged by small Python tools in `tools/mac/`
(no Apple tools needed). `make -C mac` builds all of it into `mac/build/`.

| Piece | Source | Output | What it is |
|---|---|---|---|
| `.PiDisk` | `mac/pidisk.s` | `pidisk.bin.raw` (DRVR image) | Block device driver over the mailbox BLOCK service: Open adds a drive queue entry per Pi block device and posts a disk-inserted event, Prime reads/writes 512-byte blocks synchronously, Control answers KillIO/verify/format/eject/icon/drive-info, Status answers drive status. Since 20 November 2026 also removable units (USB sticks): a VBL task adds a drive and posts a disk-inserted event when a disk is put in, marks the drive empty when it is taken out (Prime then answers offLinErr), and eject sends `PSMB_BLK_EJECT` (below, "USB flash drives"). |
| PiDisk extension | `mac/pidisk_init.s` + the DRVR | `PiDisk.bin` (MacBinary, type `INIT`) | System 6/7 extension: installs the DRVR from its own resource fork (`_DrvrInstall`), opens it, mounts every drive it added (`_MountVol`). Used until the declaration ROM path loads the driver from ROM. |
| PiMach extension | `mac/pimach.s` | `PiMach.bin` (MacBinary, type `INIT`) | Tells the Mac it is a Macintosh SE. The IIci ROM's Gestalt Manager answers the `'mach'` selector with 11 (gestaltMacIIci), so About This Macintosh, the Finder and every program that asks see a IIci. This INIT copies a four-instruction selector function into the system heap and installs it with `_ReplaceGestalt` ($A5AD) for `'mach'`; it answers 5, gestaltMacSE. Nothing else changes: the ROM's own view of its hardware is left alone (changing the ROM's box table would change its decisions about the IIci hardware it drives), the processor still reads 68030. Goes in the System Folder (System 6) or Extensions (System 7); `docs/img/system71-about-se.png`. |
| `.Network_EtherNet_PiStorm` | `mac/enet.s` | `enet.bin.raw` | Apple `.ENET`-interface Ethernet driver over the NET service: EAttachPH/EDetachPH, ERead (default handler: queued parameter blocks completed from a VBL task), EWrite (WDS gather, source address inserted, 60-byte padding), EGetInfo (address + 3 counters), EAddMulti/EDelMulti, ESetGeneral, ERdCancel; real protocol handlers are called with the A3/A4/D1 convention and ReadPacket/ReadRest. Receives from the slot-9 interrupt (`SIntInstall`, docs/07) with the VBL task as a safety net. |
| `.PiInput` | `mac/piinput.s` | `piinput.bin.raw` | Keyboards and mice attached to the Pi (USB on a Pi 3, Bluetooth) become Macintosh input. A VBL task drains the input ring the service core fills (`alice_psmac.h`), translates USB HID usages into Apple virtual key codes and, through `_KeyTranslate` and the Script Manager's current KCHR, into characters, keeps `KeyMap` and the modifier bits current, moves the cursor through `RawMouse`/`MTemp`/`CrsrNew`, and posts keyDown/keyUp/autoKey and mouseDown/mouseUp. Right and middle buttons become Command- and Option-clicks. Runs from the slot-9 interrupt (`SIntInstall`) with the VBL task as a safety net. Opened at start from the declaration ROM. |
| `.Network_WiFi_PiStorm` | `mac/enet.s` assembled with `WIFI` | `enet_wifi.bin.raw` | The same driver for unit 0 of the network board, the WiFi chip, as a second network sResource (0x83) in slot 9: MacTCP and EtherTalk list two Ethernet cards and the user picks. In the testbed the unit is a simulated radio when `mac.wifisim=` is given (below); PiWiFi joins a network on hardware. |
| `.Display_Video_PiStorm` | `mac/pivideo.s` | `pivideo.bin.raw` | The second display (docs/10): Apple's video driver interface over the display block in the slot window (psmb.h). Control: cscReset, cscSetMode (modes 128-133: 1/2/4/8 bits indexed, 16/32 bits direct, at the screen size the Pi publishes), cscSetEntries and cscDirectSetEntries (the colour table), cscGrayPage, cscSetGray, cscSetInterrupt; Status: cscGetMode, cscGetEntries, cscGetPageCnt, cscGetPageBase, cscGetGray, cscGetInterrupt, cscGetDefaultMode. The frame buffer is the window's top 512 KiB ($980000 in 24-bit mode, $F9080000 in 32-bit); the Pi converts it to the VideoCore frame buffer sixty times a second and raises the slot interrupt as the card's VBL, which the driver's handler turns into `_DoVBLTask` for slot 9. Opened at boot by the Start Manager like any video card; `tools/vcshot.py` photographs the Pi's screen in the testbed. |
| PiDisk boot record | `mac/pidisk_boot.s` | `pidisk_boot.bin.raw` | The sBootRecord (ID 6) of PiDisk's sResource, an sExec block the Slot Manager copies into the heap and runs: opens `.PiDisk` with OpenSlot (fMulti, an SEBlock naming slot 9 and sResource $80, as the ROM opens a slot driver itself), and when the Start Manager is looking for a startup disk (seBootState 0) asks the Pi which disk goes first (`PSMB_BLK_BOOT`) and returns that driver's reference number in seRefNum: how a disk image on the card becomes the startup disk (docs/08 item 67, `mac.boot=`). Logs `PiDisk boot: ...`. |
| Declaration ROM | `tools/mac/mkdeclrom.py` | `psmac.declrom` | Slot $9 card: board sResource (BoardId 0x5053, vendor info), `PiDisk` functional sResource with the DRVR in its sDriverDir and the boot record above (the ROM opens it once the System file is open, through the boot record), `Network_EtherNet_PiStorm` sResource (catNetwork/typeEtherNet, not opened at start: the `.ENET` shell opens it through OpenSlot when MacTCP or EtherTalk ask), `PiInput` sResource (opened at start). Served by Emu68 at 0xF9FF0000 (`mac.declrom=` in the testbed). |
| PiWiFi | `mac/piwifi.s` | `PiWiFi.bin` (application) | The Pi's WiFi from the Mac: shows unit 0 of the network board (state, the driver's message, network, signal, channel, address), scans and lists networks (name, signal, channel, security), joins a network (name and password typed on the status line, sent as a `struct an_join` through the command box), disconnects, switches the radio on or off. Single-letter commands in a text window, so it runs under System 6 without resources; the Macintosh counterpart of Alice68-Card. |
| PiBlue | `mac/piblue.s` | `PiBlue.bin` (application) | The Pi's Bluetooth from the Mac: the host's state and message, its address, the paired devices (name, address, keyboard/mouse/device, connected), the devices found by the last scan (name, address, signal), the passkey while pairing; scan, pair with a found device, connect or forget a paired one, through the `struct an_bt` command box at board + 0x3800. |
| PiStatus | `mac/pistatus.s` | `PiStatus.bin` (application) | The Pi's status page (Alice68's `alice_status.h` at window + 0x50000): build string, uptime, the four ARM cores' names and load, SoC temperature, the JIT's cache and translation counts, the network units' link and counters; redraws when the kernel's seq moves, at most every five seconds (`docs/img/system71-pistatus.png`: under System 7.1 after PiNet's ARP exchange, the Ethernet unit shows one frame each way and core 3, the service core, its load). |
| PiPing | `mac/piping.s` | `PiPing.bin` (application) | MacTCP over the Pi: opens Apple's `.IPP` driver, shows the configured address and mask (ipctlGetAddr), sends three ICMP echo requests to the testbed peer (ipctlEchoICMP, completion routine) and prints the round trips in ticks. Needs MacTCP installed and pointed at the card. |
| PiLog | `mac/pilog.c` | `PiLog.bin` (application) | The Pi kernel's log in a window that moves, resizes, zooms and scrolls (`docs/img/system71-pilog.png`). The kernel copies its log into a 64 KiB ring at window + 0x60000 (`psmb.h`, struct psmb_log) from boot; PiLog costs one memory read a quarter-second. Check boxes filter by part: Kernel, Mac, WiFi, Bluetooth, Network, Card; Option-click shows one part alone (`docs/img/system71-pilog-wifi.png`). Since 19 November a second row: **Filter:** shows only lines containing the text, in any case, together with the check boxes (several words must all match, `-word` hides lines with it; typing anywhere in the window goes to the field, the view is redone a fifth of a second after the last key; Esc or the field's small close box empties it) and says how many lines show, "5 of 688 lines" for WIFI in the testbed (`docs/img/system71-pilog-filter.png`); **Clear** (Edit > Clear Log, Command-K) empties the view without touching the kernel's ring, which other readers and the serial copy use: PiLog remembers where the ring stood and shows only what came after; Edit > Show Earlier Lines brings the rest back (`docs/img/system71-pilog-clear.png`); **Follow** (View > Follow New Lines, Command-T), checked at the start, keeps the view at the newest line; scrolling up unchecks it, scrolling back to the bottom or End checks it again (`docs/img/system71-pilog-follow.png`). Text can be selected and copied, and pasted into the filter. Since 20 November the log can be handed on as text instead of a photograph: **File > Save Log…** (⌘S) puts the lines shown (the parts, the filter, from Clear's mark, under a first line saying which) into a TeachText document (TEXT, 'ttxt') named "PiLog 2026-11-19 14.05"; with Option held (⌘⌥S) the whole ring. TeachText opens 32 KB at most, a whole ring is 64 KB: copy it to a modern machine (Lagoon's share) and read it there. **Save to Card** (the button at the right of the second row, or File > Save Log to SD Card) asks the kernel (`PSMB_INFO_SAVELOG`, progress through `PSMB_INFO_LOGSTATE`) to write the whole ring to `aurora68.log` in the SD card's root, overwritten each time; the kernel also does it by itself after a `[CRASH]` record (docs/08 item 55). Secrets never show: the values of `wifi.psk` and of any option named psk, pass, key or secret, the password in `mac.wifisim=` and the link key in `bt.dev=` become `***` in the window, the Scrap, a saved file and `aurora68.log` (the kernel echoes its bootargs) (`docs/img/system71-pilog-save.png`, `docs/img/system71-pilog-card.png`). Menus File, Edit (Undo to Clear in the standard order for desk accessories, then Select All, Filter Lines Command-F, Clear Log, Show Earlier Lines) and View; items that cannot be used are dimmed (`docs/img/system71-pilog-menu.png`). The 496-pixel window fits the SE's screen. In C, built with the kit in `mac/c` (`mac/c/README.md`); the image starts it from Startup Items. |
| PiScreens | `mac/piscreens.s` | `PiScreens.bin` (application) | QuickDraw's GDevice list: each screen's rectangle, flags, depth, frame buffer base, row bytes and driver, then MMU32Bit. Written for the second display's bring-up (docs/10): it is how the Pi's screen was found created but not activated. |
| PiSlots | `mac/pislots/pislots.c` | `PiSlots.bin` (application) | The NuBus cards the Slot Manager knows about, in plain words (System 7.1 has no Apple System Profiler): for the logic board's own declaration ROM (slot 0) and each slot $9-$E, whether a card is there and started, its name, board ID, maker, revision, part number and date as far as its declaration ROM gives them, and its start-up code's answer; then each function (sResource): id, name, kind in words (display, network, processor, disk, input...) with the four numbers, its driver, whether the driver is open and its reference number (from the unit table), and for a screen its size and depth. A resizable, zoomable window with two scroll bars that comes back where it was left; File > Save As… (a TeachText document), Edit > Copy, File > Look Again (⌘R), About PiSlots…. A summary also goes to the Pi's log (`[MAC] PiSlots: slot 9 "PiStorm-SE", ...`). On a Mac without a Slot Manager it says so. In C (`mac/c` kit), 68020, System 6.0.8 and 7; below, "PiSlots" (`docs/img/system71-pislots-se.png`). |
| PiColor | `mac/picolor.s` | `PiColor.bin` (application) | Colour on the Pi's screen: a colour window placed on the second display paints twelve hues at full and half strength, the primaries and a sixteen-step grey ramp with RGBForeColor and PaintRect (`docs/img/system71-two-screens.png`, both screens side by side). |
| PiNet | `mac/pinet.s` | `PiNet.bin` (application) | Test application: shows the mailbox header, PiDisk 0, finds the Ethernet card through `SGetTypeSRsrc`, opens `.ENET` with `OpenSlot`, prints the address from EGetInfo, sends an ARP request for 192.168.68.1 and waits for the reply through ERead. Pair with `tools/pinet.py` on the host. |

## Tools
- `tools/mac/rsrc.py`: resource fork writer (type list, reference lists, name list; 28-byte map header) and MacBinary II writer. `hcopy -m` understands MacBinary, so files land on HFS images with their resource forks.
- `tools/mac/mkinit.py`: DRVR + INIT code image → extension file. `tools/mac/mkapp.py`: flat code image → application (CODE 0 jump table, CODE 1, SIZE). `tools/mac/hfsput.py`: copy into a folder of an image (hfsutils). `tools/mac/bootblock.py`: show or set the boot blocks' startup names; `--startup PiNet` makes System 6 launch the application at boot (bbHelloName, what "Set Startup..." writes) with the Finder as the shell it returns to.
- `tools/mac/declrom.py`: declaration ROM builder (format block at the top, sResource directory, entries with self-relative 24-bit offsets, sBlocks, sExec blocks, CRC = rotate-left-and-add verified against the IIci ROM's own built-in declaration ROM). `tools/declrom.py` parses the result.
- `tools/vcshot.py`: a picture of the Pi's VideoCore display in the testbed (QEMU's emulated frame buffer, through the monitor socket): the second display (docs/10).
- `tools/pinet.py`: host peer for the testbed net service (answers ARP, ICMP echo and UDP echo on port 7, or bridges to a TAP).

## Workflow in the testbed
```
make -C mac                                        # drivers, extension, app, declaration ROM
cp images/incoming/sys710_patched.image images/sys710pi.image
make -C mac install IMG=../images/sys710pi.image   # PiDisk into System Folder:Extensions, PiNet in the root
chdman createhd -i images/sys710pi.image -o images/chd/sys710pi.chd -c none -f   # -c none: the testbed MAME serves compressed CHDs as raw files
dd if=/dev/zero of=images/pidisk/pidisk.img bs=1M count=20 && hformat -l PiDisk images/pidisk/pidisk.img
ROM=rom/patched/MacIIci_psmac.rom HD=images/chd/sys710pi.chd tools/run-fullstack.sh 480 \
    mac.disk0=$PWD/images/pidisk/pidisk.img mac.declrom=$PWD/mac/build/psmac.declrom mac.net=/tmp/psmac-net \
    bt=off wifi=off mac.inputtest=320
python3 tools/pinet.py /tmp/psmac-net peer        # in another shell
```
System 6 (`images/hd20pi.hda`, the user's System 6.0.8 volume): PiNet goes into the System Folder
and the boot blocks name it as the startup application; the applications go in the root:
```
make -C mac install IMG=../images/hd20pi.hda PART=1          # or hfsput.py for each file
python3 tools/mac/hfsput.py images/hd20pi.hda mac/build/PiNet.bin "System Folder" 1
python3 tools/mac/bootblock.py images/hd20pi.hda --startup PiNet
chdman createhd -i images/hd20pi.hda -o images/chd/hd20pi.chd -c none -f
ROM=rom/patched/MacIIci_psmac.rom HD=images/chd/hd20pi.chd tools/run-fullstack.sh 380 \
    mac.disk0=$PWD/images/pidisk/pidisk.img mac.declrom=$PWD/mac/build/psmac.declrom mac.net=/tmp/psmac-net \
    bt=off wifi=off mac.inputtest=250
```
The System 6 boot reaches PiNet in about three minutes; `docs/img/system6-pinet-arp.png` is what
it shows when everything works (the ARP reply arrives through the slot interrupt).
`mac.inputabout=SECONDS` plays a second script that opens the Apple menu and chooses the first
item, "About This Macintosh" (`docs/img/system71-about.png`: Macintosh IIci, System Software 7.1,
8,192K, which is the testbed's `mac.ram=8`; with PiMach installed it says Macintosh SE,
`docs/img/system71-about-se.png`, and with 128 MiB of Mac RAM `docs/img/system71-about-128mb.png`).

The input path has no real keyboard in QEMU; `mac.inputtest=SECONDS` makes the service core play a
scripted session that many seconds after the 68k starts (the mouse walks to the Apple menu, opens
and closes it, then five letters are typed) so the screenshots show whether `.PiInput` works.

68k code can log to the Pi console through the INFO service (`log "text"` macro in the sources);
the lines appear as `[MAC] ...` in `emu68.log`.

## Saying "Macintosh SE": what it changes and what it might break
PiMach changes two answers and nothing else: Gestalt's `'mach'` (5, gestaltMacSE, instead of
11) and SysEnvirons' `machineType` (3, envSE, instead of 9). Every other selector keeps the
ROM's truth: the processor (68030 as the kernel presents it), the FPU, QuickDraw's version,
the MMU, 32-bit addressing, the Slot Manager, the sound and video drivers. Well-written
software of the System 7 era asks those specific questions and never the machine type, so it
does not notice. What can notice:
- **Installers and System Enablers** that gate on the machine type ("this software cannot be
  installed on this Macintosh"): Apple's installer scripts of the period check the box. Keep
  PiMach out of the System Folder while installing system software, and put it back after.
- **Programs that infer hardware from the box.** A game or utility that reads "SE" and assumes
  a 68000, a 512x342 black-and-white screen, no FPU, no slots or 24-bit addressing may turn off
  features it could use, or, if it then pokes SE hardware addresses, misbehave. The IIci ROM's
  own code does not consult Gestalt for this; the ROM keeps thinking it is a IIci either way.
- **The reverse case**: software that refuses to run on an SE at all (anything that needs
  Color QuickDraw and checks the machine instead of the QuickDraw version) will refuse here.
The remedy is always the same: PiMach is one file, and without it the machine is a IIci
again. `make -C mac pimach-variant MACH=N` builds the extension with another type if a
particular program wants one. The machine name the Finder shows comes from a string list
indexed by this number, so only Apple's own types are available; an "Aurora68" name would need
the Finder's resource edited rather than the answer changed.

## Two screens in the testbed
```
python3 tools/pram32.py                            # 32-bit addressing on (MAME's saved PRAM; once)
python3 tools/mac/mkscrn.py -o /tmp/scrn.bin \
    --screen hw=0x18,slot=0,base=0xFBB08000,mode=128,mask=0x1001,flags=0x8800,rect=0,0,480,640 \
    --screen hw=0x5056,slot=9,base=0xF9980000,mode=131,mask=0x1001,flags=0x8001,rect=0,640,480,1280
hmount images/sys710pi.image; hcopy -m ":System Folder:System" /tmp/System.bin; humount
python3 tools/mac/rsrcadd.py /tmp/System.bin scrn 0 /tmp/scrn.bin -o /tmp/System-scrn.bin
hmount images/sys710pi.image; hdel ":System Folder:System"; hcopy -m /tmp/System-scrn.bin ":System Folder:"; humount
chdman createhd -i images/sys710pi.image -o images/chd/sys710pi.chd -c none -f
PSMAC_RAM=16 ROM=rom/patched/MacIIci_psmac.rom HD=images/chd/sys710pi.chd tools/run-fullstack.sh 340 \
    mac.disk0=$PWD/images/pidisk/pidisk.img mac.declrom=$PWD/mac/build/psmac.declrom bt=off wifi=off mac.inputdrag=285
python3 tools/vcshot.py pi-screen.png              # the Pi's screen, during the run
```
`mac.inputdrag=SECONDS` drags the volume icon off the right edge of the SE's screen so that it
lands on the Pi's; PiScreens (startup item or double-click) lists what QuickDraw made of the
screens and the values a 'scrn' needs on another machine (docs/10).

## Two screens and sound through HDMI on the real SE (20 November 2026)
The SE's own screen plus the HDMI monitor as a second screen, with the Mac's sound in stereo on the
monitor's loudspeakers (and still on the SE's speaker). docs/10 ("Two screens on the SE") and docs/08
items 62, 73 and 74 are the record; these are the steps, starting from a card that already boots the
SE with the log on HDMI (the container `aurora68.cpio` with `aurora68-512x342.declrom`, cmdline
`enable_cache mac.board=se mac.console=hdmi bt=off wifi=off mac.image=sys710pi.image mac.boot=image`,
config.txt with `hdmi_group=2` and `hdmi_mode=16`):

1. **kernel8.img**: the new one.
2. **The container**, rebuilt with the two-screen declaration ROM of the monitor's size in place of the
   512x342 one (only one `-se-` file in it; 1024 by 768 here, as `hdmi_mode=16`):

       python3 make-container.py -o aurora68.cpio MacIIci_368CADFE.rom aurora68-se-1024x768.declrom \
           cyfmac43430-sdio.bin brcmfmac43430-sdio.txt cyfmac43430-sdio.clm_blob BCM43430A1.hcd

   Sizes built: 640x480, 800x600, 1024x768, 1280x720, 1280x1024, 1600x1200, 1920x1080, 1920x1200,
   2560x1440 (below, "2560 by 1440").
3. **config.txt**: `hdmi_group=2`, the `hdmi_mode` of that size (16 = 1024 x 768, 9 = 800 x 600, 82 =
   1920 x 1080, 69 = 1920 x 1200 reduced blanking), and **`hdmi_drive=2`**, which makes the firmware drive
   the monitor as HDMI rather than DVI: DVI carries no sound. Keep `gpu_mem=64` (the release's [pi3]).
4. **cmdline.txt**: take out `mac.console=hdmi` (it means one screen, the log on HDMI) and add the size:

       enable_cache mac.board=se mac.screen=1024x768 bt=off wifi=off mac.image=sys710pi.image mac.boot=image

   `mac.audio=hdmi` is the default; `mac.audio=jack` uses the Pi 3B's headphone socket instead,
   `mac.audio=off` neither (the SE's speaker plays either way unless `mac.sesound=off`).
5. **The Mac**: copy **PiScrn** and **Sound PiStorm** (Mac/PiScrn.bin and Mac/SoundPiStorm.bin, or the
   Aurora68-Utilities disk) into the System Folder; System 7 puts both into Extensions. On a PC with
   hfsutils, into the card's image directly:

       hmount sys710pi.image
       hcopy -m PiScrn.bin ":System Folder:Extensions:"
       hcopy -m SoundPiStorm.bin ":System Folder:Extensions:"
       humount

   Sound PiStorm needs Sound Manager 3.0 or later (the "Sound Manager" extension in the Extensions
   folder on System 7.1, built into 7.5); without it the Mac's sound still reaches HDMI through the
   ASC, in 8 bits.
6. **The first start**: PiScrn finds two screens with no arrangement, writes one (the SE's screen main,
   HDMI to its right, top-aligned) and restarts the Mac once. From the second start both screens come up.
7. **Monitors** (control panel): drag the two monitors where they stand on the desk, move the menu bar to
   either, and choose the HDMI screen's depth (Millions needs 32-bit addressing, which the kernel switches
   on with more than 8 MB). The **Sound** control panel's volume sets the card's volume.

Lines in the log that say it worked: `declaration ROM: two cards, the SE's screen in slot 9 (main) and the
HDMI screen in slot $A`, `[PSMAC:DISP] VideoCore read back: physical 1024x768, ... : as asked`, `[PSMAC:DISP]
two screens: ... the firmware's own buffer, kept`, `[AUDIO] HDMI: 48000 Hz stereo through DMA channel N; HSM
clock ..., pixel clock ..., N 6144 CTS ...`, `[MAC] PiScrn: ...`, `[MAC] PiSound: Sound Manager 3.0; the sound
card ... registered and chosen as the output device`, and in every heartbeat `[PSMAC]   audio: HDMI 48000 Hz,
... | ASC ... | card ... frames, ... underruns ...`. If HDMI is silent and the log says `the transmitter
sends no packets ... DVI`, `hdmi_drive=2` is missing or the monitor declined; `mac.audiotest=card` plays a
test tone on each side at start (441 Hz left, 882 Hz right).

### 2560 by 1440 on the SE (20 November 2026)
A 2560 by 1440 monitor (the user's Dell UP2716D) as the HDMI screen. A Pi 3 cannot send that size at
60 Hz: it needs a 241 MHz pixel clock, and the Pi 3's HDMI tops out below that (1920 by 1200 at 60 Hz
already needs reduced blanking, 154 MHz). At 30 Hz with reduced blanking it is about 121 MHz, a custom
mode the firmware makes from a CVT description. docs/10, "2560 by 1440", is the record. The steps
differ from the ones above in three places:

1. **The container** with `aurora68-se.declrom` (any two-screen file serves; `aurora68-se-2560x1440.declrom`
   is the one of this size, below "One container for every monitor").
2. **config.txt**, in place of the `hdmi_mode=` line (keep `hdmi_drive=2` and `gpu_mem=64`):

       hdmi_group=2
       hdmi_mode=87
       hdmi_cvt=2560 1440 30 3 0 0 1
       max_framebuffer_width=2560
       max_framebuffer_height=1440

   `hdmi_mode=87` is the custom mode, `hdmi_cvt` its width, height, refresh (30), aspect (3 = 16:9),
   no margins, not interlaced, reduced blanking (the last 1). The last two lines let the firmware make
   its frame buffer that large. Whether the monitor takes 30 Hz is the monitor's matter: if it shows
   "no signal" or "out of range", nothing on the Mac side can help.
3. **cmdline.txt**: nothing about the size (the firmware's mode is the screen's); `mac.screen=2560x1440`
   says the same if you prefer it written down:

       enable_cache mac.board=se bt=off wifi=off mac.image=sys710pi.image mac.boot=image

PiScrn sees the size change at the next start, writes the arrangement again (the SE's screen at
(0,0)-(512,342), HDMI at (512,0)-(3072,1440)) and restarts once. Monitors offers every depth; millions
need 32-bit addressing, as at every size. **PiDepth HDMI 16** (a copy of PiDepth renamed, double-clicked
or in Startup Items) sets the HDMI screen's depth without Monitors: any of 1, 2, 4, 8, 16, 32 at the end
of the name; plain "PiDepth N" sets the main screen's, which on the SE is the one-bit screen.

The log lines to look for: `[PSMAC] screen: 2560x1440, the firmware's HDMI mode (mac.screen= not given)`,
`[PSMAC] screen 2560x1440: the big frame buffer region is 15 MiB`, `[PSMAC] declaration ROM: slot $A's screen
rewritten for 2560x1440, 6 depths`, `[PSMAC:DISP]
VideoCore read back: physical 2560x1440, virtual 2560x1440, depth 16, pitch 5120, ...: as asked`, `[PSMAC:DISP]
two screens: ... HDMI 2560x1440 ... (15 MiB); VideoCore 2560x1440 x16 pitch 5120 ...: the firmware's own buffer,
kept`, and in each heartbeat `display 1 (HDMI): 2560x1440 ... depth 32 rowbytes 10240 ...; VideoCore N frames in
5 s, longest U us in B bands`: how long one frame of the picture takes core 3 on the real Pi, which no
testbed can say. If the read-back says `depth 16` with any other size, or `NOT what was asked`, the firmware's
buffer is not 2560 by 1440: check the two `max_framebuffer_` lines.

### One container for every monitor, and declaration ROMs on the card (20 November 2026)
The kernel now rewrites the declaration ROM's HDMI screen for the size the firmware set (docs/10, "Any
size"), so the container no longer names a size and config.txt alone chooses the monitor's:

1. **The container, once**, with the one two-screen file (it holds no size that matters):

       python3 make-container.py -o aurora68.cpio MacIIci_368CADFE.rom aurora68-se.declrom \
           cyfmac43430-sdio.bin brcmfmac43430-sdio.txt cyfmac43430-sdio.clm_blob BCM43430A1.hcd

2. **cmdline.txt without `mac.screen=`**:

       enable_cache mac.board=se bt=off wifi=off mac.image=sys710pi.image mac.boot=image

3. **config.txt** chooses: `hdmi_group=2` and `hdmi_mode=16` (1024 x 768), `82` (1080p), `69` (1920 x
   1200), `81` (1366 x 768), or the five 2560 by 1440 lines above. Change the mode, restart, and the Mac
   has the new size; PiScrn rearranges the two screens and restarts once.

**A new declaration ROM is a file on the card.** A `.declrom` file in the card's root (beside
kernel8.img) is taken in preference to the container's of the same rank, so a new PiDisk, network, input
or video driver needs no new container: copy the new `aurora68-se.declrom` to the card. The log's
`[PSMAC] card: declaration ROM ...` lines list what the card holds, and `[PSMAC] declaration ROM: NAME from
the card's root taken (...)` says which one was used. Take old ones off the card when you update: a file on
the card beats the container's. `mac.cardrom=off` in cmdline.txt ignores the card's files.

### PiDisplay: choosing the HDMI resolution and rotation on the Mac (21 November 2026)
**PiDisplay** (`mac/pidisplay/pidisplay.c`, `PiDisplay.bin`, an application) shows the HDMI monitor's own
description and every resolution it lists, judged for a Pi 3 (usable, too fast with a slower alternative,
the wrong shape), suggests one with its reason, and sets the HDMI resolution and the picture's rotation.
docs/10, "PiDisplay", is the record of how; these are the steps on the user's SE.

1. **kernel8.img**, the new one, and the new **PiScrn** in the System Folder (it asks "Keep this
   resolution?" after a change; Mac/PiScrn.bin). **PiDisplay** anywhere on the Mac.
2. **config.txt, once**: the line

       include aurora68-display.txt

   at the very end, after every `hdmi_` line and in an `[all]` section (the release's config.txt ends so).
   The firmware takes the last value of a setting it reads, so an `hdmi_mode=` or `hdmi_cvt=` after the
   include would win over PiDisplay's choice; PiDisplay says so under Current. The release's
   `aurora68-display.txt` (comments only) goes on the card beside config.txt; the kernel makes the file
   itself if it is not there, but a firmware that is strict about a missing include would be the first
   thing to suspect if a card without it does not start. config.txt is never written by the kernel.
3. **In PiDisplay**: click a resolution (the bullet marks the suggested one; dimmed ones cannot be
   chosen), or a rotation, then **Restart to Apply...** and **Restart** in the dialog. A new resolution:
   the kernel writes `aurora68-display.txt` (the old one becomes `aurora68-display.bak`), the Mac shuts
   down its programs and restarts, and the Pi restarts with it, so that the firmware reads config.txt
   again. A rotation alone restarts only the Mac.
4. **After a new resolution**: on the SE's own screen PiScrn asks **Keep this resolution?** with a
   countdown. **Keep** (Return) keeps it. **Revert** (Escape), or 15 seconds without an answer, puts the
   previous file back and restarts the Pi once more. PiScrn also rearranges the two screens for the new
   size, which costs one restart of the Mac, as after any change of monitor.

For 2560 by 1440 on the UP2716D PiDisplay should suggest 2560 x 1440 at 30 Hz (60 Hz needs 241.5 MHz, the
Pi 3 sends about 162), which writes `hdmi_group=2`, `hdmi_mode=87`, `hdmi_cvt=2560 1440 30 3 0 0 1`, the two
`max_framebuffer_` lines and `hdmi_drive=2`: the same as the five lines of "2560 by 1440 on the SE" above,
which can then come out of config.txt.

Lines in the log to send back: every `[PSMAC:EDID]` line (the first, `the firmware's EDID: N blocks in U
us`, says whether the firmware gave the EDID at all; then the monitor, its sound, a line per mode with
its judgement, the suggestion and the mode in use), `[CARD] aurora68-display.txt: N bytes written and read
back`, `[PSMAC:EDID] the whole Pi restarts now (its watchdog)`, after the restart `[PSMAC:EDID] the card:
config.txt read (it includes aurora68-display.txt), ...; a new mode waits to be confirmed`, `[MAC] PiScrn: a
new HDMI mode waits to be kept`, `[MAC] PiDisplay: ...`, and from the heartbeat `display 1 (HDMI): ...
VideoCore N frames in 5 s, longest U us in B bands, turned 90` for a rotated screen.

## The WiFi in the testbed: a radio that exists only in software
QEMU's raspi3b has no WiFi chip (no SDIO function, no firmware to load), so the real driver in
`alice_net.c` can only report "off" there, and until 16 November the WiFi unit, PiWiFi and the
`.Network_WiFi_PiStorm` driver had never run against anything that answered. Everything above the
chip is worth testing all the same, and the Alice68 board protocol makes that easy: the Mac only
ever sees unit 0's state word, message, command box, scan table and frame rings. So the service
core got a simulated radio (`psmac_wifi_testbed_poll` in `emu68/src/alice/alice_psmac.c`), switched
on by `mac.wifisim=SSID:PASSWORD` (or `mac.wifisim=1` for the defaults, Aurora68 / pistorm68):
- it starts, a second later it is idle, exactly the states the real driver walks through;
- a scan (AN_CMD_SCAN) takes two seconds and finds four networks: the one named on the command
  line (WPA2, channel 6, -47 dBm), "Macintosh Garden" (WPA2, whose password nobody knows),
  "LINKSYS" (WEP) and "Free Hotspot" (open), with BSSIDs, so PiWiFi's list has something to show;
- a join (AN_CMD_JOIN, `struct an_join` in the command box) checks the name against the scan list
  and the password against the network's, the way the firmware's association and four-way handshake
  would: "no such network in range" and "the password was not accepted" come back as FAILED
  states after a believable delay, the right pair goes JOINING then CONNECTED with the SSID,
  BSSID, signal and channel filled in and the link word up;
- `wifi.ssid=` / `wifi.psk=` on the command line join at boot, as on hardware;
- disconnect, off, enable, the receive switch, the filter and the address change behave as the
  real driver's;
- while connected, frames in unit 0's transmit ring go to a second host file pair,
  `mac.wifinet=BASE` (BASE.tx/.rx, served by `tools/pinet.py BASE peer` like the Ethernet's
  `mac.net=`), and frames from it fill the receive ring; without a connection transmitted frames
  are lost, as in the air.
The real driver is never started when the simulation owns the unit, and without `mac.wifisim=` the
simulation does not exist: `alice_net.c` asks once, before it would start the WiFi thread.

Two scripted sessions exercise it without a keyboard (`mac.inputwifi=SECONDS`, like
`mac.inputtest=`): with PiWiFi as the startup application the script presses S, waits for the scan
list, presses J and types the network's name and password at PiWiFi's prompts
(`docs/img/system71-piwifi-scan.png`, `docs/img/system71-piwifi-joined.png`). For MacTCP over the
WiFi the card is built with the WiFi as its only network sResource (`mac/build/psmac_wifi.declrom`,
`mkdeclrom.py --wifi` without `--enet`): MacTCP's Ethernet link layer takes the first Ethernet-type
sResource it finds in the slot, so with the Ethernet sResource absent it opens
`.Network_WiFi_PiStorm`, and PiPing's echoes go through unit 0, the simulated radio and the
`mac.wifinet` files to the host peer (`docs/img/system71-mactcp-wifi-ping.png`):
```
ROM=rom/patched/MacIIci_psmac.rom HD=images/chd/sys710pi.chd tools/run-fullstack.sh 400 \
    mac.disk0=$PWD/images/pidisk/pidisk.img mac.declrom=$PWD/mac/build/psmac_wifi.declrom \
    mac.wifisim=Aurora68:pistorm68 wifi.ssid=Aurora68 wifi.psk=pistorm68 mac.wifinet=/tmp/psmac-wifi bt=off
python3 tools/pinet.py /tmp/psmac-wifi peer        # in another shell
```
With both sResources present MacTCP takes the Ethernet; choosing the WiFi from the control panel's
list (its interface-name string, `+48` in `'ipln'`) is not decoded yet. On 19 November the
kernel settled it on a Pi without a working wired port by taking sResource $81 out of the
declaration ROM in RAM before the 68k starts (`declrom_drop` in psmac_bus.c). Later the same day
the Pi 3B's LAN9514 and the 3B+'s LAN7800 are driven (docs/08 item 49), and hiding the card is the
exception again: only a board with no wired port at all (3A+, Zero 2 W) or `eth=off` loses $81.

### Two network cards on one NuBus card (19 November)

The slot-9 card carries both network functions side by side, sResource $81 (the wired port,
`.Network_EtherNet_PiStorm`, Alice68 unit 1) and $83 (the WiFi, `.Network_WiFi_PiStorm`, unit 0),
beside PiDisk, input and video. Two drivers with two names, so two unit-table entries and two
refnums; each has its own protocol table and its own rings on the network board. What each board
gives unit 1, decided from the board's revision code (else the device tree) before the 68k starts:

| Board | Wired port (unit 1, $81) | WiFi (unit 0, $83) | Bluetooth firmware |
|---|---|---|---|
| Pi 3B (and 2B v1.2) | LAN9514 on USB, `psmac_usbnet.c` (smsc95xx) | BCM43438, files `*43430*` | BCM43430A1.hcd |
| Pi 3B+ | LAN7800 on USB behind two hubs, `psmac_usbnet.c` (lan78xx) | BCM43455 | BCM4345C0.hcd |
| Pi 3A+ (boots with the 3B+'s device tree) | none: $81 left out; `eth=usb`: a USB adapter | BCM43455 | BCM4345C0.hcd |
| Zero 2 W | none: $81 left out; `eth=usb`: a USB adapter | BCM43436 or 43436s (both answer as a 43430; revision 1 is the 43436s: `brcmfmac43436s-sdio.*`, else `brcmfmac43436-sdio.*`) | BCM43430B0.hcd |
| Pi 4B | GENET, Alice68's `alice_net.c` | BCM43455 | BCM4345C0.hcd |
| Pi 400, CM4 | GENET | BCM43455/43456 | BCM4345C5.hcd |
| testbed (QEMU) | the file bridge with `mac.net=`, else QEMU's `usb-net` (CDC-ECM) through `psmac_usbnet.c` | the simulated radio with `mac.wifisim=` | |

A Pi 4 does not fit the classic PiStorm in the SE: Pi 4 means the solo bench, or a different
adapter.

**Interrupts.** Both drivers share slot 9's interrupt with PiDisk, input and video, and the
network board has one irq flag for both units. Until then each driver's slot handler
cleared the flag and answered "mine" whenever it had been set, so with both cards open the wired
driver took the WiFi's interrupts and the WiFi's frames waited for its VBL task (a quarter of a
second). Now (mac/enet.s, `slot_int` and the end of `rx_body`) a handler claims the interrupt only
for frames it took from its own ring, or when nothing waits in the other unit's ring (a command
finished: somebody must clear the flag); with frames waiting for the other driver it puts the flag
back and answers 0, and the ROM goes on to the other handler. Frames left behind by a driver that
closed do not count (the other unit's `rx_on` is checked).

**Link state.** The .ENET interface has no link state, so the drivers answer a Control call of
their own, csLinkStatus (230): csParam = link (word), speed in Mbit/s (word), 'A68L'. Riptide asks
it once a second and moves its default route to the WiFi while the wired link is down
(Riptide's docs/MAC.md, "Several cards").

**Who uses which card.**
- Riptide: both, as en0 and en1 (its docs/MAC.md); the default route prefers the wired card while
  its link is up.
- MacTCP: one card, the first Ethernet-type sResource of the slot, so the wired port; the control
  panel's choice of the second is not decoded (above). On a board without the wired port it is the
  WiFi.
- AppleTalk (Lagoon EtherTalk): one card, chosen in the Network control panel, which shows one icon
  per card, "Lagoon (9)" and "Lagoon Wi-Fi (9)"; parameter RAM ($E0) holds the slot and sResource
  id. If the card in parameter RAM does not open, Lagoon takes the first card that does (since 20
  November it walks the list instead of retrying the first card).
- If a Pi 3B's network chip is not found on the USB bus, unit 1 reports no unit (kind 0) and
  the wired driver's Open fails, as with `eth=off`: Riptide carries on with the WiFi, Lagoon
  falls back to it. MacTCP, which takes only the first card, then has no network: `eth=off` in
  cmdline.txt hides the card altogether and gives back the behaviour before (the WiFi as the only card).

## The WiFi menu
`WiFi Menu` (mac/wifimenu, an extension: drop it on the System Folder) puts a WiFi icon at the right
of the menu bar, left of the Help menu, on System 7. The icon shows the signal (a fan of three
arcs), the dot alone while not connected, faint arcs when the radio is off. A press on it lists
the networks around with their signal and "(open)" for unsecured ones, ticks the one in use,
switches the radio on or off, looks again, disconnects, and forgets the network saved on the Pi.
Choosing a secured network asks for its password (shown as bullets; Return joins, Escape cancels). With "Remember this network" ticked, the kernel
writes `wifi.ssid=` and `wifi.psk=` into cmdline.txt on the Pi's card once the join has worked, so
the next power-on joins by itself. The rest of cmdline.txt is kept (docs/08 items 43 and 44).

## AppleTalk on the solo board
`mac.appletalk=1` starts with AppleTalk active (PRAM's SPConfig), on top of whatever `pram.bin` on
the card holds: the solo board keeps its parameter RAM there across power cycles (docs/08 item 51),
so the Chooser's choice lasts by itself. LocalTalk runs over the solo
board's SCC stand-in, whose transmitter finishes every frame into an empty network (docs/08
item 43), so the boot and the Network control panel work. There is nobody to talk to there.
For AppleTalk over the Pi's Ethernet (or WiFi) card, Lagoon brings its own connection file,
"Lagoon EtherTalk" (Lagoon's `src/mac/ethertalk`, its docs/mac.md): put it in the Extensions
folder and choose "Lagoon / Slot #9" in the Network control panel. In the testbed, preset the
choice and give the peer an AppleShare server to find (docs/08 item 47):
```
PINET_ATALK="Lagoon Test" python3 tools/pinet.py /tmp/psmac-net peer
tools/run-solo.sh 600 mac.net=/tmp/psmac-net mac.appletalk=1 mac.xpram=e0:098100a8
```
The peer's log shows the Mac's AARP probes, NBP lookups and Echo replies. The Chooser's
AppleShare panel lists "Lagoon Test" and gets as far as the login dialog.

## USB keyboards and mice in the testbed
QEMU's raspi3b has the Pi 3's DWC2 controller; give it devices and type or move through the monitor:
```
QEMU_EXTRA="-device usb-kbd -device usb-mouse" tools/run-solo.sh 340 wifi=off
python3 tools/qemu-mon.py "mouse_move 20 12"                 # once the Finder is up
python3 tools/qemu-mon.py "sendkey meta_l-n;sendkey u;sendkey ret"
```
The log shows `[USB] start rc=0: 1 keyboard, 1 mouse`, and a heartbeat line every thirty seconds
with the transport, the number of enumerations and each device's completed, NAKed and failed
polls. Kernel options: `usb=off` (leave the controller alone), `usb=pio` (Alice68's FIFO transport
instead of DMA). docs/08 item 40 has the design.

PC keyboards: by default the Windows key is Command and Alt is Option (Apple's own mapping for PC
keyboards). `mac.altcmd=1` swaps them, so the key beside the space bar is Command as on a Mac
keyboard. Both apply to USB and Bluetooth keyboards.

## USB flash drives (20 November 2026)
A USB stick plugged into the Pi appears on the Mac's desktop as a disk, while the Mac runs, and is
ejected from the Finder like a floppy (docs/08 item 75). It is a PiDisk unit, so nothing has to be
installed on the Mac: the kernel and the declaration ROM do it.

**What you need.** The new `kernel8.img` and a container rebuilt with the new declaration ROM
(`aurora68.declrom` or `aurora68-se-WxH.declrom`). An older declaration ROM's PiDisk sees a stick only if it
was plugged in before the Mac started, and may give up on a slow one. Nothing in cmdline.txt:
`mac.usbdisk=on` is the default.

**What the stick must hold.** System 7 reads HFS ("Mac OS Standard"), not FAT, exFAT or HFS Plus
("Mac OS Extended"). The stick can be:
- formatted HFS on a Mac (Mac OS 9 or earlier, or Disk Utility up to 10.5);
- a disk image written to it (`dd`), bare HFS or with an Apple partition map, like the card's images;
- initialized on the Mac itself (below).

Other sticks stay on the Pi, and the log says why: "[USB] disk 0: an MBR partition table, first
partition FAT32 ... not offered to the Mac". That way the Finder never offers to erase a PC's stick.

**Using it.**
1. Plug the stick into any of the Pi's USB sockets, before or after the Mac starts. A few seconds
   later its icon (the PiDisk icon) is on the desktop.
2. Use it as any disk. Every write the Mac makes has reached the stick when the Finder says it is
   done; the kernel caches nothing.
3. To take it out, drag its icon to the Trash (Put Away) or choose Eject Disk (Command-E). Wait for
   the icon to go (or turn grey), then unplug. The log says "[USB] disk 0 (...): ejected by the Mac
   ...: safe to unplug".
4. Pulled out without ejecting, the Mac is told the drive is empty, as for a floppy pulled out with a
   paper clip. Using the volume then gives "error of type -65", and a volume ejected with Command-E
   asks "Please insert the disk". Plug the stick back in and the volume carries on. Drag it to the
   Trash if it is not coming back. A file being written when the stick was pulled is lost, as on any
   computer.

**To make a stick for the Mac on the Mac:** add `mac.usbdisk=all` to cmdline.txt, plug in a stick
you can erase, and click Initialize when the Finder says "This is not a Macintosh disk"
(`docs/img/system71-usb-stick-initialize.png`). It becomes an HFS stick (2 GB at most, HFS's limit on
System 7). Take `mac.usbdisk=all` out again afterwards. With it, every stick, a PC's too, is offered,
and the Finder offers to erase it. PC Exchange users may want it for FAT12/16 sticks.
`mac.usbdisk=off` leaves sticks alone. Up to four disks in all: the card's images and sticks share
PiDisk's four units.

**Log lines to send back** (PiLog with the filter `disk`, or the serial log). In the testbed, with
QEMU's stick, they read:
```
[USB] disk 0 on hub port 3: QEMU QEMU USB HARDDRIVE (46f4:0001), full speed, LUN 0 of 1
[USB] disk 0: "QEMU QEMU HARDDISK 2.5+", 32832 blocks of 512 bytes (16 MB), writable
[USB] disk 0: an Apple partition map, 2 entries: Apple_partition_map, Apple_HFS
[USB] disk 0: its Apple_HFS partition (blocks 64-32831) holds the HFS volume "APM Stick"
[USB] disk 0: offered to the Mac: 32768 blocks (16 MB) from block 64, writable
[PSMAC:DISK] PiDisk unit 1 is USB disk 0, USB QEMU USB HARDDRIVE: 32768 blocks (16 MB), writable; the Mac's driver is told a disk was inserted
[MAC] PiDisk: a disk was put in: disk-inserted event
[PSMAC:DISK] PiDisk unit 1 (USB disk 0): the Mac's first read, block 2: it is mounting the disk
[USB]   disk 0 (QEMU USB HARDDRIVE): on the Mac, 40 reads (804 blocks), 1 writes (1 blocks), 0 errors, 0 resets
```
On the Pi 3B the first line should say "high speed" for a USB 2 stick.
Any line with "reset recovery", "failed", "not ready" or "did not answer", and the
`[USB] start` / `plug or unplug` lines around it, say what went wrong.

**In the testbed** (QEMU's raspi3b has the Pi's DWC2; its hub is full speed). Make an HFS image
(`dd if=/dev/zero of=stick.img bs=1M count=32; hformat -l "USB Stick" stick.img`) and boot with a hub
(`QEMU_EXTRA="-device usb-hub,id=h -device usb-kbd,bus=usb-bus.0,port=1.1"`, plus `mac.remotefile=BASE`
to drive the Finder with `tools/aurora68.py --semihost BASE`). Once the Finder is up, plug the stick in
with the virtual CPUs stopped: QEMU 8.2.2 hung on a `device_add` while they ran.
```
python3 tools/qemu-mon.py stop
python3 tools/qemu-mon.py "drive_add 0 if=none,id=d1,file=$PWD/stick.img,format=raw"
python3 tools/qemu-mon.py "device_add usb-storage,id=u1,drive=d1,bus=usb-bus.0,port=1.3"
python3 tools/qemu-mon.py cont
```
and `stop`, `device_del u1`, `cont` to pull it out (each plug needs a new id). A stick alone on the root
port (`-device usb-storage,bus=usb-bus.0,port=1,drive=...` at start) runs at high speed. hfsutils keeps
its current volume in `$HOME/.hcwd`: give it a HOME of its own when another job uses hfsutils too.

## PiSlots: the NuBus cards (21 November 2026)
The user asked how to see which NuBus cards the Mac has. On the SE the Pi presents two cards that
exist only in software (slot 9, the Aurora68 card; slot $A, the HDMI screen), and the IIci ROM's Slot
Manager treats them like real ones. System 7.1 has no Apple System Profiler and the project gives away
nothing it did not write, so PiSlots (`mac/pislots/pislots.c`, `PiSlots.bin`) is the project's own.
Double-click it, or put it in Startup Items. It needs nothing else and changes nothing.

**What it asks.** For the logic board's own declaration ROM (slot 0) and for slots $9 to $E, the Slot
Manager (`_SlotManager`, $A06E): `SReadInfo` for whether a card is there and started (`siInitStatusA`)
and what its start-up code answered (`siInitStatusV`); `SNextSRsrc` slot by slot for every sResource,
the board sResource (category 1) first; `SGetCString`, `SReadWord` and `SFindStruct` for the name,
board ID, vendor information (maker, serial number, revision, part number, date: whatever the card
gives) and whether there is a primary init; `SReadDrvrName` for each function's driver name. Whether the
driver is open comes from the unit table (`UTableBase`, `UnitNtryCnt`): a DCE whose `dCtlSlot`,
`dCtlSlotId` and `dCtlExtDev` are the sResource's, `dOpened` in its flags, the reference number
`~unit`. A screen's size and depth come from the GDevice whose `gdRefNum` is that driver. Kinds are
put in words where the numbers are Apple's (ROMDefs: display/video, network/Ethernet, processor 68000
to 68040, scanner, test, internal bus, dock) or Aurora68's own (category $7050 'pP': type 1 disk, type
2 keyboard and mouse); the four numbers follow on a line of their own. A function with no driver in
the card's ROM and none in the unit table says "driver: none" (`SReadDrvrName` makes a name either
way). An empty slot answers `smEmptySlot` (-300) on a real Mac; the IIci ROM in the testbed answered
-309 (`smBLFieldBad`, no byte lane holds a format block) for every empty slot, and both count as
empty. Empty slots are listed once, at the end.

**The window**, by the Human Interface Guidelines (1992), as the user asked of the project's Mac
programs: a document window with close, zoom and grow boxes and two scroll bars that follow every
resize; the report is not wrapped, so its columns stay in line in a narrow window and the horizontal bar
moves it. The zoom box zooms to the screen that holds most of the window, as wide as the widest line and
as tall as the report. The window's place and size are kept in "PiSlots Prefs" (type 'pref', the
Preferences folder through `FindFolder` on System 7, the System Folder through `SysEnvirons` on System
6) and put back only while a screen still shows its title bar. Menus: Apple (About PiSlots…), File
(Close ⌘W, Save As…, Look Again ⌘R, Quit ⌘Q), Edit (Undo, Cut, Copy, Paste, Clear, Select All, with the
usual keys; only Copy and Select All are PiSlots' own, the rest are there for desk accessories and dimmed
otherwise). Copy copies the selection, or the whole report when nothing is selected. Save As… writes a
TeachText document, "PiSlots 2026-11-21" by default. The About box and the alert for a failed save are
centred on the main screen with the default button outlined; Return, Enter, Escape and Command-period
answer them. Update, activate, suspend and resume events are handled; an inactive window's scroll bars
are blank, as the Finder's are (System 7.1's control definition still draws the arrows of a bar dimmed
with `HiliteControl` 255, so they are hidden instead); the 'SIZE' resource says MultiFinder-aware and
can run in the background. Look Again redoes the list: a network driver is opened only when MacTCP or
AppleTalk asks, so "closed until a program asks for it" changes to "open" after that. No Balloon Help:
the C kit has no resource compiler and no 'hmnu' writer. It runs on the SE's one-bit screen and on
colour screens, on System 6.0.8 and 7.

On an Aurora68 Mac (slot 9 holding board ID $5053) it also writes a summary into the Pi's log through
the mailbox's INFO service, one line per card's functions and one for the cards, so the kernel log and
PiLog show what the Mac saw (the kernel takes 127 characters of a line, so a long card takes two):
```
[MAC] PiSlots: slot 9: PiDisk open, Network_EtherNet_PiStorm closed, PiInput open, Network_WiFi_PiStorm closed
[MAC] PiSlots: slot 9, more: Display_Video_PiStorm open
[MAC] PiSlots: slot 9 "PiStorm-SE"; empty: $A $B $C $D $E
```

**On the SE board** (MAME's `macse_ps`, 32 MiB, the System 7.1 image as the startup disk,
`psmac-se-1024x768.declrom` with `mac.screen=1024x768`, PiScrn in Extensions; 21 November 2026), saved
with File > Save As…:
```
The cards in this Macintosh, as the Slot Manager sees them.

The logic board (slot 0, the built-in declaration ROM)
  Card:        Macintosh II Built-In Video
  Board ID:    $001F
  Made by:     Apple Computer, Inc
  Revision:    Macintosh II Family 1.0
  Part number: 342-0733/342-0734/342-0735/342-0736
  Date:        18-Jul-89
  Start-up:    its own code (primary init) ran, answer 1
  Functions:
  $B5 CPU_68030_\_MacIIci
      kind:    processor, 68030
      numbers: category 10, type 4, software 0, hardware $0018
      driver:  none

Slot 9: a card
  Card:        PiStorm-SE
  Board ID:    $5053 'PS'
  Made by:     PiStorm-SE project
  Revision:    0.1
  Part number: PSMAC
  Start-up:    no start-up code of its own
  Functions:
  $80 PiDisk
      kind:    disk (Aurora68's own kind)
      numbers: category $7050 'pP', type 1, software 1, hardware $0001
      driver:  .PiDisk, open, reference number -50
  $81 Network_EtherNet_PiStorm
      kind:    network, Ethernet
      numbers: category 4, type 1, software 1, hardware $5053 'PS'
      driver:  .Network_EtherNet_PiStorm, closed until a program asks for it
  $82 PiInput
      kind:    keyboard and mouse (Aurora68's own kind)
      numbers: category $7050 'pP', type 2, software 1, hardware $0001
      driver:  .PiInput, open, reference number -51
  $83 Network_WiFi_PiStorm
      kind:    network, Ethernet
      numbers: category 4, type 1, software 1, hardware $5054 'PT'
      driver:  .Network_WiFi_PiStorm, closed until a program asks for it
  $84 Display_Video_PiStorm
      kind:    display, video
      numbers: category 3, type 1, software 1, hardware $5056 'PV'
      driver:  .Display_Video_PiStorm, open, reference number -49
      screen:  512 x 342, 1 bit (black and white), the main screen

Slot $A: a card
  Card:        PiStorm-SE HDMI
  Board ID:    $5048 'PH'
  Made by:     PiStorm-SE project
  Revision:    0.1
  Part number: PSMAC-HDMI
  Start-up:    no start-up code of its own
  Functions:
  $80 Display_Video_PiHDMI
      kind:    display, video
      numbers: category 3, type 1, software 1, hardware $5048 'PH'
      driver:  .Display_Video_PiHDMI, open, reference number -52
      screen:  1024 x 768, 32 bits (millions of colours)

Empty slots: $B, $C, $D, $E
```
(`docs/img/system71-pislots-se.png` on the SE's screen; `docs/img/system71-pislots-hdmi-zoom.png` after
dragging the window onto the HDMI screen and clicking its zoom box: it zooms on that screen, the whole
report in view; `docs/img/system71-pislots-se-resized.png` resized and scrolled on the SE's screen). The
solo board with `psmac.declrom` shows the same slot 0 and slot 9 (its screen 640 x 480) and "Empty slots:
$A, $B, $C, $D, $E"; with `psmac_wifi_video.declrom` the wired card $81 is absent
(`docs/img/system71-pislots-solo-resized.png`, narrowed, with the horizontal bar).

*Tested* in the testbed, driven through the remote's semihosting channel (`mac.remotefile=`,
`tools/aurora68.py --semihost`): the list on the solo board (System 7.1, both declaration ROMs) and on
the SE board (two screens, then one); resize, zoom (on the main screen and on the HDMI screen), both
scroll bars (arrows, page, thumb); About and Command-period; the File and Edit menus; Save As… (the file
opens in SimpleText); Copy; Quit and Close; suspend and resume behind the Finder; the window back where it
was left, and back on the SE's screen at the default place when the HDMI screen it was saved on is gone.
System 6.0.8 on the solo board (the user's System 6 volume, PiSlots as the startup application, 8 MiB and
`mac.xpram=8a:00` so that the ROM starts in 24-bit mode instead of asking, which `.PiInput` is not yet
open to answer): the same list, resize, About, the File menu and Quit, "PiSlots Prefs" in the System
Folder.
*Not verified:* real NuBus cards (on a IIci or SE/30: other vendors' declaration ROMs, a card whose start
failed), a real SE, System 6 on the SE board, the Balloon Help that is not there.

## MacTCP
MacTCP is Apple's TCP/IP of 1989-1996: the `.IPP` driver and a control panel, later folded into
Open Transport. It is not in the repository (Apple's), and the user's images do not carry it; the
full product shipped with System 7.5 and as "TCP/IP Connection for Macintosh", the 2.0.6 files
on the CD-ROM archives are updaters for 2.0.4. To test it here, put MacTCP (the control panel,
and "MacTCP DNR" if the version has one) into `images/incoming/`, then:
```
python3 tools/mac/hfsput.py images/sys710pi.image images/incoming/MacTCP.bin "System Folder/Control Panels" 1
python3 tools/mac/rsrcdump.py images/incoming/MacTCP.bin        # what its configuration resources look like
```
The user supplied MacTCP 2.0.6 (a floppy image with the control panel; `images/incoming/`).
Out of the box it is set to LocalTalk and "Server", and its configuration lives in a resource
Apple never documented, 'ipln' 128 in the control panel. Rather than drive the control panel's
dialogs inside the emulator, the layout was read out of the control panel's and the `.IPP`
driver's code (the driver takes the first 'ipln' of version 1 and copies 130 bytes into its
globals; the link layer is the 'iplp' resource whose ID is the word at +90; the Ethernet link
layer searches NuBus slots from the long at +92 with sNextTypeSRsrc and takes the first Ethernet
card when it is 0) and `tools/mac/mactcpcfg.py` writes it:
```
python3 tools/mac/mactcpcfg.py images/incoming/MacTCP.bin --ip 192.168.68.2 --gateway 192.168.68.1 -o images/incoming/MacTCP-manual.bin
python3 tools/mac/hfsput.py images/sys710pi.image images/incoming/MacTCP-manual.bin "System Folder/Control Panels" 1
```
(`--server` instead of `--ip` leaves the address to BOOTP, which `tools/pinet.py` answers with
the same numbers.) MacTCP's Ethernet link layer then finds the card the way PiNet does and opens
`.ENET` through OpenSlot. `PiPing` proves the stack: it opens `.IPP`, prints the address MacTCP
has, and pings the peer three times through MacTCP's own ICMP echo call.

## Facts the drivers rely on (from Inside Macintosh and Designing Cards and Drivers)
- Driver header: flags, delay, event mask, menu, five offsets, Pascal name, then a word-aligned
  version word (the `.ENET` shell and the Start Manager compare versions).
- Slot Manager: the format block is probed at $FsFFFFFF (byte lanes $0F for a 32-bit ROM), test
  pattern $5A932BC7, format 1, revision 1, CRC over the whole image; the board sResource is found by
  type (1/0/0/0) and must carry a BoardId; sResources without sRsrcFlags are opened at start.
- Start Manager and slot boot records (the IIci ROM, 20 November 2026, docs/08 item 67): the default
  startup device in parameter RAM $78-$7B is {sdExtDevID, sdPartition, sdSlotNum, sdSRsrcID} for a
  card or {-, -, refNum} for a driver (SCSI ID n is refNum -33-n). For a card the ROM runs the
  sResource's sBootRecord (ID 6) through `_sExec` before it loads the SCSI drivers: sExec checks
  revision 2 and a CPU id of at most 2 (sCPU68020), copies the block into the heap, calls the code
  at offset field + offset with A0 = the SEBlock and all registers saved, and disposes of the copy.
  The SEBlock's seRefNum (offset 20) names the driver whose drives the first pass of the drive
  queue tries (with a floppy); later passes take any drive. Once the System file is open the ROM
  runs the record again with seBootState 1 (offset 22) instead of opening the sResource's driver.
- Ethernet clients find cards with `SGetTypeSRsrc` (category 4, type 1, mask 3) and `OpenSlot
  ".ENET"`; protocol type 0 means 802.3 (length field ≤ 1500) frames and goes to the LAP Manager;
  `ERead` returns the whole frame including the 14-byte header; the driver inserts the source address
  and pads to 60 bytes.
- Buffer addresses handed to the Pi are logical; in 24-bit mode the driver strips the high byte.
- An application in front must ask `GetNextEvent` for every event: the Event Manager mounts a
  volume when the disk-inserted event is *taken*, so a test application that asked only for
  mouse and key events kept the Pi disk off the desktop until it quit. The four applications
  take everything and ignore what is not theirs.
- Input (Inside Macintosh I "Toolbox Event Manager", Inside Macintosh: Text "Virtual Key Codes"
  and `KeyTranslate`, MPW `SysEqu.a`): `_PostEvent` fills an event's modifiers from the modifier
  bits of `KeyMap` ($174, four longwords, bit 0 of each the lowest code) and the button from
  `MBState` ($172, $80 = up); a key event's message is ADB address << 16 | virtual code << 8 |
  character; the cursor follows `RawMouse` ($82C) and `MTemp` ($828) when `CrsrNew` ($8CE) is set,
  clipped to `CrsrPin` ($834). The keyboard layout is the `'KCHR'` 0 resource fetched, locked
  and address-stripped at Open (the driver opens in 24-bit mode and uses the pointer in 32-bit
  mode), read directly by the driver: the low-memory long at $1B40, which MPW's SysEqu.a calls
  `KCHRPtr`, holds the address of a code patch under 7.1 on this ROM (docs/08, item 22).
