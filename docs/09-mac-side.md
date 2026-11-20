# Mac-side software: drivers, extension, test application, declaration ROM

Everything the Macintosh runs to talk to the Pi lives in `mac/` (68k assembly, GNU `as` MIT
syntax, `binutils-m68k-linux-gnu`) and is packaged by small Python tools in `tools/mac/`
(no Apple tools needed). `make -C mac` builds all of it into `mac/build/`.

| Piece | Source | Output | What it is |
|---|---|---|---|
| `.PiDisk` | `mac/pidisk.s` | `pidisk.bin.raw` (DRVR image) | Block device driver over the mailbox BLOCK service: Open adds a drive queue entry per Pi block device and posts a disk-inserted event, Prime reads/writes 512-byte blocks synchronously, Control answers KillIO/verify/format/eject/icon/drive-info, Status answers drive status. |
| PiDisk extension | `mac/pidisk_init.s` + the DRVR | `PiDisk.bin` (MacBinary, type `INIT`) | System 6/7 extension: installs the DRVR from its own resource fork (`_DrvrInstall`), opens it, mounts every drive it added (`_MountVol`). Used until the declaration ROM path loads the driver from ROM. |
| PiMach extension | `mac/pimach.s` | `PiMach.bin` (MacBinary, type `INIT`) | Tells the Mac it is a Macintosh SE. The IIci ROM's Gestalt Manager answers the `'mach'` selector with 11 (gestaltMacIIci), so About This Macintosh, the Finder and every program that asks see a IIci. This INIT copies a four-instruction selector function into the system heap and installs it with `_ReplaceGestalt` ($A5AD) for `'mach'`; it answers 5, gestaltMacSE. Nothing else changes: the ROM's own view of its hardware is left alone (changing the ROM's box table would change its decisions about the IIci hardware it drives), the processor still reads 68030. Goes in the System Folder (System 6) or Extensions (System 7); `docs/img/system71-about-se.png`. |
| `.Network_EtherNet_PiStorm` | `mac/enet.s` | `enet.bin.raw` | Apple `.ENET`-interface Ethernet driver over the NET service: EAttachPH/EDetachPH, ERead (default handler: queued parameter blocks completed from a VBL task), EWrite (WDS gather, source address inserted, 60-byte padding), EGetInfo (address + 3 counters), EAddMulti/EDelMulti, ESetGeneral, ERdCancel; real protocol handlers are called with the A3/A4/D1 convention and ReadPacket/ReadRest. Receives from the slot-9 interrupt (`SIntInstall`, docs/07) with the VBL task as a safety net. |
| `.PiInput` | `mac/piinput.s` | `piinput.bin.raw` | Keyboards and mice attached to the Pi (USB on a Pi 3, Bluetooth) become Macintosh input. A VBL task drains the input ring the service core fills (`alice_psmac.h`), translates USB HID usages into Apple virtual key codes and, through `_KeyTranslate` and the Script Manager's current KCHR, into characters, keeps `KeyMap` and the modifier bits current, moves the cursor through `RawMouse`/`MTemp`/`CrsrNew`, and posts keyDown/keyUp/autoKey and mouseDown/mouseUp. Right and middle buttons become Command- and Option-clicks. Runs from the slot-9 interrupt (`SIntInstall`) with the VBL task as a safety net. Opened at start from the declaration ROM. |
| `.Network_WiFi_PiStorm` | `mac/enet.s` assembled with `WIFI` | `enet_wifi.bin.raw` | The same driver for unit 0 of the network board, the WiFi chip, as a second network sResource (0x83) in slot 9: MacTCP and EtherTalk list two Ethernet cards and the user picks. In the testbed the unit is a simulated radio when `mac.wifisim=` is given (below); PiWiFi joins a network on hardware. |
| `.Display_Video_PiStorm` | `mac/pivideo.s` | `pivideo.bin.raw` | The second display (docs/10): Apple's video driver interface over the display block in the slot window (psmb.h). Control: cscReset, cscSetMode (modes 128-133: 1/2/4/8 bits indexed, 16/32 bits direct, at the screen size the Pi publishes), cscSetEntries and cscDirectSetEntries (the colour table), cscGrayPage, cscSetGray, cscSetInterrupt; Status: cscGetMode, cscGetEntries, cscGetPageCnt, cscGetPageBase, cscGetGray, cscGetInterrupt, cscGetDefaultMode. The frame buffer is the window's top 512 KiB ($980000 in 24-bit mode, $F9080000 in 32-bit); the Pi converts it to the VideoCore frame buffer sixty times a second and raises the slot interrupt as the card's VBL, which the driver's handler turns into `_DoVBLTask` for slot 9. Opened at boot by the Start Manager like any video card; `tools/vcshot.py` photographs the Pi's screen in the testbed. |
| Declaration ROM | `tools/mac/mkdeclrom.py` | `psmac.declrom` | Slot $9 card: board sResource (BoardId 0x5053, vendor info), `PiDisk` functional sResource with the DRVR in its sDriverDir (opened at start by the Start Manager), `Network_EtherNet_PiStorm` sResource (catNetwork/typeEtherNet, not opened at start: the `.ENET` shell opens it through OpenSlot when MacTCP or EtherTalk ask), `PiInput` sResource (opened at start). Served by Emu68 at 0xF9FF0000 (`mac.declrom=` in the testbed). |
| PiWiFi | `mac/piwifi.s` | `PiWiFi.bin` (application) | The Pi's WiFi from the Mac: shows unit 0 of the network board (state, the driver's message, network, signal, channel, address), scans and lists networks (name, signal, channel, security), joins a network (name and password typed on the status line, sent as a `struct an_join` through the command box), disconnects, switches the radio on or off. Single-letter commands in a text window, so it runs under System 6 without resources; the Macintosh counterpart of Alice68-Card. |
| PiBlue | `mac/piblue.s` | `PiBlue.bin` (application) | The Pi's Bluetooth from the Mac: the host's state and message, its address, the paired devices (name, address, keyboard/mouse/device, connected), the devices found by the last scan (name, address, signal), the passkey while pairing; scan, pair with a found device, connect or forget a paired one, through the `struct an_bt` command box at board + 0x3800. |
| PiStatus | `mac/pistatus.s` | `PiStatus.bin` (application) | The Pi's status page (Alice68's `alice_status.h` at window + 0x50000): build string, uptime, the four ARM cores' names and load, SoC temperature, the JIT's cache and translation counts, the network units' link and counters; redraws when the kernel's seq moves, at most every five seconds (`docs/img/system71-pistatus.png`: under System 7.1 after PiNet's ARP exchange, the Ethernet unit shows one frame each way and core 3, the service core, its load). |
| PiPing | `mac/piping.s` | `PiPing.bin` (application) | MacTCP over the Pi: opens Apple's `.IPP` driver, shows the configured address and mask (ipctlGetAddr), sends three ICMP echo requests to the testbed peer (ipctlEchoICMP, completion routine) and prints the round trips in ticks. Needs MacTCP installed and pointed at the card. |
| PiLog | `mac/pilog.c` | `PiLog.bin` (application) | The Pi kernel's log in a window that moves, resizes, zooms and scrolls (`docs/img/system71-pilog.png`). The kernel copies its log into a 64 KiB ring at window + 0x60000 (`psmb.h`, struct psmb_log) from boot; PiLog costs one memory read a quarter-second. Check boxes filter by part: Kernel, Mac, WiFi, Bluetooth, Network, Card; Option-click shows one part alone (`docs/img/system71-pilog-wifi.png`). Since 19 November a second row: **Filter:** shows only lines containing the text, in any case, together with the check boxes (several words must all match, `-word` hides lines with it; typing anywhere in the window goes to the field, the view is redone a fifth of a second after the last key; Esc or the field's small close box empties it) and says how many lines show, "5 of 688 lines" for WIFI in the testbed (`docs/img/system71-pilog-filter.png`); **Clear** (Edit > Clear Log, Command-K) empties the view without touching the kernel's ring, which other readers and the serial copy use: PiLog remembers where the ring stood and shows only what came after; Edit > Show Earlier Lines brings the rest back (`docs/img/system71-pilog-clear.png`); **Follow** (View > Follow New Lines, Command-T), checked at the start, keeps the view at the newest line; scrolling up unchecks it, scrolling back to the bottom or End checks it again (`docs/img/system71-pilog-follow.png`). Text can be selected and copied, and pasted into the filter. Since 20 November the log can be handed on as text instead of a photograph: **File > Save Log…** (⌘S) puts the lines shown (the parts, the filter, from Clear's mark, under a first line saying which) into a TeachText document (TEXT, 'ttxt') named "PiLog 2026-11-19 14.05"; with Option held (⌘⌥S) the whole ring. TeachText opens 32 KB at most, a whole ring is 64 KB: copy it to a modern machine (Lagoon's share) and read it there. **Save to Card** (the button at the right of the second row, or File > Save Log to SD Card) asks the kernel (`PSMB_INFO_SAVELOG`, progress through `PSMB_INFO_LOGSTATE`) to write the whole ring to `aurora68.log` in the SD card's root, overwritten each time; the kernel also does it by itself after a `[CRASH]` record (docs/08 item 55). Secrets never show: the values of `wifi.psk` and of any option named psk, pass, key or secret, the password in `mac.wifisim=` and the link key in `bt.dev=` become `***` in the window, the Scrap, a saved file and `aurora68.log` (the kernel echoes its bootargs) (`docs/img/system71-pilog-save.png`, `docs/img/system71-pilog-card.png`). Menus File, Edit (Undo to Clear in the standard order for desk accessories, then Select All, Filter Lines Command-F, Clear Log, Show Earlier Lines) and View; items that cannot be used are dimmed (`docs/img/system71-pilog-menu.png`). The 496-pixel window fits the SE's screen. In C, built with the kit in `mac/c` (`mac/c/README.md`); the image starts it from Startup Items. |
| PiScreens | `mac/piscreens.s` | `PiScreens.bin` (application) | QuickDraw's GDevice list: each screen's rectangle, flags, depth, frame buffer base, row bytes and driver, then MMU32Bit. Written for the second display's bring-up (docs/10): it is how the Pi's screen was found created but not activated. |
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
