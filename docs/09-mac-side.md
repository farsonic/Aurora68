# Mac-side software: drivers, extension, test application, declaration ROM

Everything the Macintosh runs to talk to the Pi lives in `mac/` (68k assembly, GNU `as` MIT
syntax, `binutils-m68k-linux-gnu`) and is packaged by small Python tools in `tools/mac/`
(no Apple tools needed). `make -C mac` builds all of it into `mac/build/`.

| Piece | Source | Output | What it is |
|---|---|---|---|
| `.PiDisk` | `mac/pidisk.s` | `pidisk.bin.raw` (DRVR image) | Block device driver over the mailbox BLOCK service: Open adds a drive queue entry per Pi block device and posts a disk-inserted event, Prime reads/writes 512-byte blocks synchronously, Control answers KillIO/verify/format/eject/icon/drive-info, Status answers drive status. |
| PiDisk extension | `mac/pidisk_init.s` + the DRVR | `PiDisk.bin` (MacBinary, type `INIT`) | System 6/7 extension: installs the DRVR from its own resource fork (`_DrvrInstall`), opens it, mounts every drive it added (`_MountVol`). Used until the declaration ROM path loads the driver from ROM. |
| `.Network_EtherNet_PiStorm` | `mac/enet.s` | `enet.bin.raw` | Apple `.ENET`-interface Ethernet driver over the NET service: EAttachPH/EDetachPH, ERead (default handler: queued parameter blocks completed from a VBL task), EWrite (WDS gather, source address inserted, 60-byte padding), EGetInfo (address + 3 counters), EAddMulti/EDelMulti, ESetGeneral, ERdCancel; real protocol handlers are called with the A3/A4/D1 convention and ReadPacket/ReadRest. Receives from the slot-9 interrupt (`SIntInstall`, docs/07) with the VBL task as a safety net. |
| `.PiInput` | `mac/piinput.s` | `piinput.bin.raw` | Keyboards and mice attached to the Pi (USB on a Pi 4, Bluetooth) become Macintosh input. A VBL task drains the input ring the service core fills (`alice_psmac.h`), translates USB HID usages into Apple virtual key codes and, through `_KeyTranslate` and the Script Manager's current KCHR, into characters, keeps `KeyMap` and the modifier bits current, moves the cursor through `RawMouse`/`MTemp`/`CrsrNew`, and posts keyDown/keyUp/autoKey and mouseDown/mouseUp. Right and middle buttons become Command- and Option-clicks. Runs from the slot-9 interrupt (`SIntInstall`) with the VBL task as a safety net. Opened at start from the declaration ROM. |
| `.Network_WiFi_PiStorm` | `mac/enet.s` assembled with `WIFI` | `enet_wifi.bin.raw` | The same driver for unit 0 of the network board, the WiFi chip, as a second network sResource (0x83) in slot 9: MacTCP and EtherTalk list two Ethernet cards and the user picks. In the testbed the unit is off (no WiFi in QEMU); PiWiFi joins a network on hardware. |
| Declaration ROM | `tools/mac/mkdeclrom.py` | `psmac.declrom` | Slot $9 card: board sResource (BoardId 0x5053, vendor info), `PiDisk` functional sResource with the DRVR in its sDriverDir (opened at start by the Start Manager), `Network_EtherNet_PiStorm` sResource (catNetwork/typeEtherNet, not opened at start: the `.ENET` shell opens it through OpenSlot when MacTCP or EtherTalk ask), `PiInput` sResource (opened at start). Served by Emu68 at 0xF9FF0000 (`mac.declrom=` in the testbed). |
| PiWiFi | `mac/piwifi.s` | `PiWiFi.bin` (application) | The Pi's WiFi from the Mac: shows unit 0 of the network board (state, the driver's message, network, signal, channel, address), scans and lists networks (name, signal, channel, security), joins a network (name and password typed on the status line, sent as a `struct an_join` through the command box), disconnects, switches the radio on or off. Single-letter commands in a text window, so it runs under System 6 without resources; the Macintosh counterpart of Alice68-Card. |
| PiBlue | `mac/piblue.s` | `PiBlue.bin` (application) | The Pi's Bluetooth from the Mac: the host's state and message, its address, the paired devices (name, address, keyboard/mouse/device, connected), the devices found by the last scan (name, address, signal), the passkey while pairing; scan, pair with a found device, connect or forget a paired one, through the `struct an_bt` command box at board + 0x3800. |
| PiStatus | `mac/pistatus.s` | `PiStatus.bin` (application) | The Pi's status page (Alice68's `alice_status.h` at window + 0x50000): build string, uptime, the four ARM cores' names and load, SoC temperature, the JIT's cache and translation counts, the network units' link and counters; redraws when the kernel's seq moves, at most every five seconds (`docs/img/system71-pistatus.png`: under System 7.1 after PiNet's ARP exchange, the Ethernet unit shows one frame each way and core 3, the service core, its load). |
| PiPing | `mac/piping.s` | `PiPing.bin` (application) | MacTCP over the Pi: opens Apple's `.IPP` driver, shows the configured address and mask (ipctlGetAddr), sends three ICMP echo requests to the testbed peer (ipctlEchoICMP, completion routine) and prints the round trips in ticks. Needs MacTCP installed and pointed at the card. |
| PiNet | `mac/pinet.s` | `PiNet.bin` (application) | Test application: shows the mailbox header, PiDisk 0, finds the Ethernet card through `SGetTypeSRsrc`, opens `.ENET` with `OpenSlot`, prints the address from EGetInfo, sends an ARP request for 192.168.68.1 and waits for the reply through ERead. Pair with `tools/pinet.py` on the host. |

## Tools
- `tools/mac/rsrc.py`: resource fork writer (type list, reference lists, name list; 28-byte map header) and MacBinary II writer. `hcopy -m` understands MacBinary, so files land on HFS images with their resource forks.
- `tools/mac/mkinit.py`: DRVR + INIT code image → extension file. `tools/mac/mkapp.py`: flat code image → application (CODE 0 jump table, CODE 1, SIZE). `tools/mac/hfsput.py`: copy into a folder of an image (hfsutils). `tools/mac/bootblock.py`: show or set the boot blocks' startup names; `--startup PiNet` makes System 6 launch the application at boot (bbHelloName, what "Set Startup..." writes) with the Finder as the shell it returns to.
- `tools/mac/declrom.py`: declaration ROM builder (format block at the top, sResource directory, entries with self-relative 24-bit offsets, sBlocks, sExec blocks, CRC = rotate-left-and-add verified against the IIci ROM's own built-in declaration ROM). `tools/declrom.py` parses the result.
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
The input path has no real keyboard in QEMU; `mac.inputtest=SECONDS` makes the service core play a
scripted session that many seconds after the 68k starts (the mouse walks to the Apple menu, opens
and closes it, then five letters are typed) so the screenshots show whether `.PiInput` works.

68k code can log to the Pi console through the INFO service (`log "text"` macro in the sources);
the lines appear as `[MAC] ...` in `emu68.log`.

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
MacTCP finds the card the way PiNet does (SGetTypeSRsrc for catNetwork/typeEtherNet, then
OpenSlot ".ENET"); its control panel lists one entry per network sResource, so the Pi shows two,
Ethernet (sRsrcID 0x81) and WiFi (0x83). The address can be set in the control panel ("Manually",
192.168.68.2, mask 255.255.255.0, gateway 192.168.68.1 for the testbed peer) or obtained from a
server: `tools/pinet.py` answers BOOTP, so a MacTCP left on "Server" gets 192.168.68.2 from the
peer. `PiPing` then proves the stack: it opens `.IPP`, prints the address MacTCP has, and pings
the peer three times through MacTCP's own ICMP echo call.

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
  clipped to `CrsrPin` ($834); `KCHRPtr` ($1B40) points at the current layout.
