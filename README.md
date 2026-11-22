# Aurora68

An Emu68-based accelerator and expansion for the Macintosh SE (and, in the testbed so far, the
Macintosh Plus, 512Ke, 512K and 128K): a Raspberry Pi in the 68000 socket presents a 68040, runs Apple's Macintosh IIci ROM, keeps the SE's own screen, keyboard,
mouse, floppy and SCSI chip working, and adds memory, disk images from the SD card, Ethernet,
WiFi and Bluetooth through a NuBus card that exists only in software.

**Building it:** `CLAUDE.md` lists every tool and version, the build command for each artifact (kernel, Macintosh side, ROM patch, MAME testbed, disk images, containers), how the testbed is run and how each dependency is updated.

Start with `docs/00-overview.md`: it tells the story of the project and points to the working
documents for each part. The day-by-day record of what was found and fixed is `docs/08`.

## Quick start (release 0.13)

Release 0.13 runs on a real Macintosh SE: System 7.1 and System 6.0.8 start from the SE's own SCSI
disk (or a BlueSCSI) or from a disk image on the SD card, with the SE's screen, keyboard, mouse and
speaker, a second screen and stereo sound on HDMI, Ethernet and WiFi, USB and Bluetooth keyboards and
mice, USB sticks, and a remote control. What has been checked on the hardware and what only in the
emulator is listed in `RELEASE-NOTES.md`; the day-by-day record is `docs/08`. The steps below set up
the card; "Disks and disk images: what works" (below) says which disks the Mac can use.

**The Plus, 512Ke, 512K and 128K (after 0.13, not in a release yet).** The same kernel has boards for
the other compact Macs: `mac.board=plus`, `512ke`, `512k` or `128k` in `cmdline.txt` in place of
`mac.board=se`; everything else on the card is as for the SE. All four reach System 7.1's Finder in
the testbed (docs/08 item 96); none has been tried on a real machine yet. What differs from the SE:
- **Keyboard and mouse:** the Plus's own (and the 128K's and 512K's) are not ADB; the kernel turns
  them into ADB devices for the IIci ROM, beside USB and Bluetooth ones.
- **Disks:** the Plus has SCSI (a BlueSCSI works as on the SE). The 512Ke, 512K and 128K have none:
  their System comes from a disk image in the card's root, as on an SE with no SCSI disk (below).
  Their floppy drives (400K on the 128K and 512K) have not been tried yet.
- **Memory:** the Pi's, as on the SE (the machine's own 128 or 512 KB is used for its screen and
  sound buffers only). Give the Mac 32 MiB or more with System 7.1: at 8 MiB in 24-bit mode QuickTime
  crashes at start-up on every board.
- **Parameter RAM:** the 128K and 512K's clock chip has 20 bytes and none of the extended parameter
  RAM the IIci ROM wants: the kernel keeps all of it in `pram.bin` on the card (as `mac.sepram` does
  for an SE whose chip does not answer). A Mac whose parameter RAM is blank or not trusted (a Plus's,
  at its first start with Aurora68) still starts from the card's image.
- **SANE:** exact SANE reads PACK 4 and 5 from the machine's own ROM where it holds them (the log says
  `[PSMAC] SANE: ...`); the 64 KB ROM of the 128K and 512K has none, so there the IIci ROM's SANE
  stays (64-bit doubles) unless `mac.serom=` names an SE ROM image on the card.
- **Fitting the PiStorm:** on these boards the 68000 is usually soldered in, not socketed: fitting a
  socket (or a clip-on adapter) is the owner's job and the board's risk. Check the board first.
- `mac.fpu=none` makes the Mac FPU-less, as all of these machines really are (docs/08 item 103).

1. Take a microSD card with a **FAT32** first partition.
2. Copy the **Raspberry Pi firmware** onto it, from the card's root folder (the device tree
   for your board and the `disable-bt` overlay are needed: the kernel reads the device tree, and
   the overlay puts the serial port the kernel logs on at GPIO 14/15):
   ```bash
   curl -fL --remote-name-all \
     https://raw.githubusercontent.com/raspberrypi/firmware/master/boot/bootcode.bin \
     https://raw.githubusercontent.com/raspberrypi/firmware/master/boot/start.elf \
     https://raw.githubusercontent.com/raspberrypi/firmware/master/boot/fixup.dat \
     https://raw.githubusercontent.com/raspberrypi/firmware/master/boot/bcm2710-rpi-3-b.dtb \
     https://raw.githubusercontent.com/raspberrypi/firmware/master/boot/bcm2710-rpi-3-b-plus.dtb \
     https://raw.githubusercontent.com/raspberrypi/firmware/master/boot/bcm2710-rpi-zero-2-w.dtb
   mkdir -p overlays
   curl -fL -o overlays/disable-bt.dtbo \
     https://raw.githubusercontent.com/raspberrypi/firmware/master/boot/overlays/disable-bt.dtbo
   ```
   (A Pi 4 or CM4 wants `start4.elf`, `fixup4.dat` and `bcm2711-rpi-4-b.dtb` instead; a Pi 4
   does not fit a classic PiStorm.)
3. Unzip the release and copy **everything in it** onto the card's root: `kernel8.img`,
   `config.txt`, `cmdline.txt`, the declaration ROMs, `make-container.py` with `rompatch.py`
   and `patches/`, the `Mac` folder, the notes and licences.
4. Copy **your own** Macintosh IIci ROM (version 368CADFE, 512 KB) next to them and build the
   container the kernel loads as `initramfs`. A stock ROM is patched on the way (the kernel's
   timing calibration; `make-container.py` says so):
   ```bash
   python3 make-container.py -o aurora68.cpio MacIIci_368CADFE.rom aurora68-512x342.declrom aurora68.declrom
   ```
   `aurora68-512x342.declrom` is the card's declaration ROM for the SE's screen; the kernel takes
   the one whose name ends in the screen size in use, so both can travel.
   The quickest way to the rest: `sh get-firmware.sh` (in the zip) from the card's root fetches the
   Pi's boot files for a 3B, 3B+, 3A+, Zero 2 W and 4 alike and every board's WiFi and Bluetooth
   firmware into `wifi-firmware/`, checks each file's SHA-256, and prints the `make-container.py`
   line with all of them: one card and one container then boot a Pi 3 and a Pi 4. By hand:
   For WiFi and Bluetooth on a Pi with the Broadcom CYW43455 (Pi 3B+, 3A+, 4, CM4; the Zero 2 W's
   chip is a BCM43436, whose files get-firmware.sh fetches),
   fetch Broadcom's firmware (the same files and versions Alice68 uses: WiFi firmware
   7.45.241 from Raspberry Pi's `bullseye` branch, the Bluetooth patch from `bluez-firmware`)
   and add them to the command:
   ```bash
   curl -fL --remote-name-all \
     https://raw.githubusercontent.com/RPi-Distro/firmware-nonfree/bullseye/debian/config/brcm80211/cypress/cyfmac43455-sdio-standard.bin \
     https://raw.githubusercontent.com/RPi-Distro/firmware-nonfree/bullseye/debian/config/brcm80211/cypress/cyfmac43455-sdio.clm_blob \
     https://raw.githubusercontent.com/RPi-Distro/firmware-nonfree/bullseye/debian/config/brcm80211/brcm/brcmfmac43455-sdio.txt \
     https://raw.githubusercontent.com/RPi-Distro/bluez-firmware/master/broadcom/BCM4345C0.hcd
   python3 make-container.py -o aurora68.cpio MacIIci_368CADFE.rom aurora68-512x342.declrom aurora68.declrom \
     cyfmac43455-sdio-standard.bin brcmfmac43455-sdio.txt cyfmac43455-sdio.clm_blob BCM4345C0.hcd
   ```
   Those are the files for a Pi 3B+, 3A+ or 4 (BCM43455). A **Pi 3B** carries the older BCM43438
   and wants these instead (the kernel picks by the chip's id):
   ```bash
   curl -fL --remote-name-all \
     https://raw.githubusercontent.com/RPi-Distro/firmware-nonfree/bullseye/debian/config/brcm80211/cypress/cyfmac43430-sdio.bin \
     https://raw.githubusercontent.com/RPi-Distro/firmware-nonfree/bullseye/debian/config/brcm80211/brcm/brcmfmac43430-sdio.txt \
     https://raw.githubusercontent.com/RPi-Distro/firmware-nonfree/bullseye/debian/config/brcm80211/cypress/cyfmac43430-sdio.clm_blob \
     https://raw.githubusercontent.com/RPi-Distro/bluez-firmware/master/broadcom/BCM43430A1.hcd
   python3 make-container.py -o aurora68.cpio MacIIci_368CADFE.rom aurora68-512x342.declrom aurora68.declrom \
     cyfmac43430-sdio.bin brcmfmac43430-sdio.txt cyfmac43430-sdio.clm_blob BCM43430A1.hcd
   ```
   (On Windows, `curl.exe` and the `curl` part as one line. The `brcm/` copies of the 43430 `.bin`
   and `.clm_blob` in that repository are links, and a raw download of a link is a 31-byte path:
   expect about 400 KB and 5 KB.) The kernel finds the files in the container by name
   (`cyfmac43455-sdio-standard` and `brcmfmac43455-sdio` are both accepted, as are `cyfmac43430`
   and `brcmfmac43430`). Apple's ROM and Broadcom's firmware are not included in the release.
5. `cmdline.txt` as shipped reads `enable_cache debug mac.board=se bt=off`: `mac.board=se` is
   what tells the kernel it sits in a Macintosh SE rather than the testbed's IIci.
6. Put the Pi in the PiStorm, the PiStorm in the SE's 68000 socket (pin 1 at the notch end), a
   serial cable on GPIO 14/15 for the kernel's log, HDMI to a monitor if you have one, and
   power on. `READ-ME-FIRST.txt` in the zip says what the first boot should look like.

The Macintosh side needs no installation: the Slot Manager loads the Pi's drivers from the
declaration ROM. The `Mac` folder holds the programs in the folder **Aurora68**, as MacBinary
files and on an HFS image (Aurora68-Utilities.img): **Aurora68 Play** (MP3, FLAC, Ogg Vorbis and Apple Lossless files, seeking in them, the music server's library and its commands from other players, and internet radio:
forty stations in submenus, plus your own; over MacTCP, or over Riptide's `.IPP6` with IPv6 stations
reached directly) with its Music folder; **Display** (Aurora68 Display, Screens, Colour, Depth); **Network**
(Aurora68 Network, Wi-Fi, Bluetooth, Ping); **System** (Aurora68 Slots, Log, Status); and
**Extensions (to install)** (Aurora68 Disk, Machine, Screen Setup, Sound, Wi-Fi & Bluetooth), each
with an icon of the family (docs/img/aurora68-icons.png). Until 21 November 2026 the programs were
PiPlay, PiDisplay, PiSlots and so on (docs/09 has the table). Aurora68 Slots lists the NuBus cards
the Mac sees (the Pi's card in slot 9, the SE's HDMI screen in slot $A, real cards on a IIci or
SE/30): each card's maker and what its functions are and do.

### Disks and disk images: what works
The Macintosh sees disks in five ways. Every one of them is HFS ("Mac OS Standard") as far as the Mac
is concerned: System 6 and 7 read nothing else.

| Where | What works | Notes |
|---|---|---|
| **The SE's own SCSI bus**: its internal disk, a BlueSCSI, SCSI2SD or ZuluSCSI | A disk with an **Apple partition map and a driver partition** (Apple's `Apple_Driver43` from HD SC Setup, or another formatter's driver) and an HFS volume. It can be the startup disk. | For a BlueSCSI, image files named `HDxy_512.hda` (x the SCSI ID, y the LUN). A bare HFS image with no driver does not mount on SCSI. **The card images made before 22 November 2026 carry Iomega's Zip driver**, which works on the Pi's own disk path but not with a BlueSCSI (the Mac offers to initialize the disk): `python3 tools/mac/swap_driver.py YOUR-BLUESCSI-DISK.hda IMAGE HD00_512.hda` gives an image Apple's driver from one of your own disks (`--show IMAGE` says which driver an image has). Nothing on the bus at all, or no terminator, is fine: the kernel notices (`mac.sescsi=auto`, docs/08 item 70). |
| **Disk images on the SD card**: files in the card's root ending in `.img`, `.hda`, `.image`, `.hfv` or `.dsk`, 1 MB or more, up to four (or the ones `mac.image=` names) | **Bare HFS** (`hformat`, Basilisk II and Mini vMac disks) **or Apple-partitioned** (the first `Apple_HFS` partition is used). They appear as disks (PiDisk volumes); one holding a System can be the startup disk (`mac.boot=`, below). | Read into the Pi's memory, so the size is limited by the memory the Pi has left after the Mac's: a few hundred MB on a 1 GB Pi, about 100 MB on a 512 MB one (the log says when one does not fit). What the Mac writes goes back into the file. Keep them out of the container: the firmware reads the container before anything starts. |
| **USB sticks** in the Pi's sockets, plugged in any time | A stick formatted **FAT32 or exFAT** on a modern Mac or a PC: its files appear as a **locked** disk; Mac files keep their resource forks, type and creator from the `._` files macOS writes (new in 0.12, docs/09 "USB flash drives"). HFS disk images in such a stick's root mount as disks of their own, writable. A stick holding **HFS** itself mounts writable as it is. | Up to 2 GB and 32,000 files per view. Writing from the Mac to a FAT/exFAT stick's own files is not done yet (issue #20). Eject with Put Away, then wait for "safe to unplug" in Aurora68 Log. |
| **The "Mac Exchange" folder** on the SD card | Its files as a locked disk, the same way as a FAT/exFAT stick (new in 0.12). | Put files there from any computer. `mac.exchange=off` turns it off, `mac.exchange=NAME` uses another folder. |
| **The SE's floppy drive** | The SE's own 800K drive, reached through the IWM; a floppy with a System is the startup disk, as on any Mac. | 0.12 selects the internal drive (before, the kernel left it unselected: docs/08 item 93). Not yet verified on a real SE; Aurora68 IO Test and docs/12 have the steps (issue #9). |

**System versions.** Tested: **System 7.1** (also 7.1.2 from the owner's BlueSCSI) and **System 6.0.8**.
Others are untested. More than 8 MB of memory needs 32-bit addressing (System 7: Memory control panel,
On); System 6 is 24-bit and sees 8 MB at most.

**What does not work:** HFS Plus ("Mac OS Extended") and APFS disks (System 7 cannot read them; use FAT32 or
exFAT to move files between a modern Mac and the SE); Disk Copy 4.2 floppy images (they carry an 84-byte
header: convert them to a raw image first); compressed or packaged images (`.dmg`, `.sit`, `.toast`); CD-ROM
images (issue #15).

**The startup disk.** A floppy with a System is taken first, as on any Mac. Then, with `mac.boot=auto` (the
default), the SCSI disk the parameter RAM names (the Startup Disk control panel), then a disk image on the
card, then the other SCSI disks: the card's image starts the Mac when the usual startup disk is not there.
`mac.boot=image` puts the card's image first; `mac.boot=scsi` leaves the parameter RAM's choice alone.

### config.txt
```ini
[all]
kernel=kernel8.img
arm_64bit=1
initramfs aurora68.cpio 0x10000000     # the ROM, the declaration ROM and firmware in one file
boot_delay=0                           # start the kernel at once ("Booting fast", below)
enable_uart=1                          # the kernel's log on the serial pins, 115200 8N1
hdmi_group=2
hdmi_mode=4                            # 640 x 480: the second screen, one pixel to one
hdmi_drive=2                           # HDMI with sound (not DVI): the Mac's sound on the monitor
```
The Pi's HDMI output is the Macintosh's second screen, at the size of the HDMI mode config.txt sets,
one pixel to one: the kernel fits the declaration ROM's screen to that size at every start (since 20
November 2026; before, a declaration ROM per size had to be in the container, `aurora68-1024x768.declrom`
and so on, with `mac.screen=` to match, and those still work). `mac.screen=` (below) gives the Mac
another size than the mode's. `hdmi_mode=16` is 1024 by 768, `35` is 1280 by 1024, `82` is 1080p, `69` is
1920 by 1200 (60 Hz, reduced blanking: the Pi 3's HDMI cannot drive the full-blanking `68`). 2560 by
1440 has no mode a Pi 3 can drive at 60 Hz (it needs a 241 MHz pixel clock); at 30 Hz with reduced
blanking (about 121 MHz) it is a custom mode, `hdmi_mode=87` with `hdmi_cvt=2560 1440 30 3 0 0 1`,
`max_framebuffer_width=2560` and `max_framebuffer_height=1440` (docs/10, "2560 by 1440"). The screen
offers black and white, 4, 16, 256, thousands and millions of colours in the Monitors control
panel; the ROM sizes the Macintosh's memory and the frame buffer is the Pi's, so a 1080p screen in
millions of colours costs the Macintosh nothing. `docs/img/system71-pi-1024x768-millions.png`
is the Pi's screen at 1024 by 768 in millions of colours; `docs/img/system71-about-128mb.png`
is About This Macintosh with 128 MB.

### cmdline.txt
One line, options separated by spaces, no comments:

| Option | Meaning |
|---|---|
| `mac.ram=128` | Macintosh memory in MB. Default: up to 128, which is all the IIci ROM sizes (two banks of 64), keeping back room for a 96 MB disk image on the card (none when the container brings the image). A 512 MB 3A+ or Zero 2 W and an 8 GB Pi 4 all get 128; on a 512 MB Pi that leaves room for a card image of up to about 99 MB, and the log says how much (`[PSMAC] memory: ...`). A larger image needs a smaller `mac.ram=` |
| `mac.serom=PATH` | an SE ROM image (B2E362A8) to take exact SANE from, in the testbeds or on a Pi without an SE: a host file through semihosting in QEMU, else a file in the card's root. On the SE board it is not needed: the kernel reads PACK 4 and PACK 5 from the SE's own ROM. A68 Machine installs them in place of the IIci ROM's SANE, whose extended arithmetic Emu68's FPU rounds to 64-bit doubles (docs/08 item 101: Photoshop waited for ever at its splash screen) |
| `mac.sane=rom`, `mac.sane=se` | which SANE (the Mac's floating-point packages). With the exact FPU (the default, below) the IIci ROM's own SANE is kept: it is the FPU version, and on the exact FPU its extended results are exact (A68 Machine's check: (1 + 2^-60) - 1 = 2^-60) and it is several times faster than the SE ROM's software SANE (A68 Machine in QEMU: 94,000 to 258,000 additions a second against 15,000 to 26,000). `mac.sane=se` installs the SE ROM's software SANE all the same (from the SE's own ROM on `mac.board=se`, from `mac.serom=` elsewhere). With `mac.fpu=double` the SE ROM's SANE is the default when an SE ROM is at hand, because the IIci ROM's would round extended results to double (`mac.sane=rom` keeps it: 20 to 60 times faster). With `mac.fpu=none` the SE ROM's SANE is always used. docs/08 items 101 and 104 |
| `mac.fpu=exact`, `mac.fpu=double`, `mac.fpu=none` | no FPU for the Mac, as on a real SE or Plus (a 68000 has none): every FPU instruction takes the line-F exception, the ROM's hardware flags say no FPU (HWCfgFlags' hwCbFPU cleared in the ROM's ProductInfo record), Gestalt `'fpu '` answers 0, and programs take their software paths, so no extended result is rounded to a double by Emu68's FPU. SANE is then the SE ROM's software SANE (from the SE's own ROM on `mac.board=se`, from `mac.serom=` elsewhere; without either, a program's first SANE call stops with system error 90, "no FPU"). The price is speed: floating-point programs run at software speed. `exact` (the default, also `on`): a 68882 to the Mac that computes every instruction in 80-bit extended precision with the 68881's rounding, exceptions and transcendental functions correctly rounded (SoftFloat behind a JIT call, docs/17; Speedometer's FPU average 8.5 against a Mac II in QEMU). `double`: Emu68's own FPU, faster, but in 64-bit doubles. The log says which (`[PSMAC] FPU: ...`). docs/08 items 103 and 104 |
| `enable_cache` | Emu68's instruction cache; leave it on |
| `wifi.ssid=NAME wifi.psk=PASSWORD wifi.country=GB` | join a WiFi network at boot; the country is your own two-letter code (GB, DE, US ...), not the placeholder `XX`, which the kernel ignores. A WiFi chip that does not answer is given up (`the WiFi chip does not answer (SDIO): WiFi off, the Mac carries on`) and the Mac starts as with `wifi=off` |
| `wifi=off`, `bt=off`, `eth=off` | leave a radio or the Ethernet off (`eth=off` also takes the wired card out of the Mac's slot 9) |
| `eth=usb` | on a Pi without a wired port (3A+, Zero 2 W): a USB Ethernet adapter as the Mac's wired card, a LAN9500, LAN7800 or CDC-ECM device on the USB port or behind a high-speed hub there (tested in QEMU with a CDC-ECM device only) |
| `net.oui=off` | keep the Pi's own MAC addresses (default: Commodore's prefix, as Alice68) |
| `mac.screen=1024x768` | the HDMI screen's size in pixels (default: the HDMI mode the firmware set, 640x480 in QEMU; 512 to 4096 by 342 to 2560, millions of colours up to 3840x2160). Since 20 November 2026 the kernel rewrites the declaration ROM's screen for the size in use, so `mac.screen=` is needed only to differ from the firmware's mode, and one declaration ROM serves every size (`aurora68-se.declrom` on the SE board). Older notes: set `hdmi_mode` to match and use the declaration ROM of that size (`aurora68-1920x1200.declrom` for `mac.screen=1920x1200`, `hdmi_mode=69`; on `mac.board=se`, `aurora68-se-1920x1200.declrom`; `mac.screen=2560x1440` with the custom 30 Hz mode above and `aurora68-se-2560x1440.declrom`). On the SE board the SE's own screen is always 512x342 and this is the second screen's size |
| `*.declrom` in the card's root (files, not an option) | declaration ROMs on the card beside `kernel8.img` are offered with the container's and win a tie, so a new Mac-side driver (PiDisk, the network cards, input, video) is a file copied to the card; the log names the file taken and where from (`[PSMAC] declaration ROM: ... from the card's root taken`). Keep only the ones you mean on the card (on the SE: `aurora68-se.declrom` and `aurora68-512x342.declrom`): an old file there beats the container's. A file whose PiDisk has no boot record (an older release's) is taken only when nothing else will do, since the Mac cannot start from a disk image with it, and the line `chosen over: ...` says why each other file was passed over. `mac.cardrom=off` ignores them |
| `include aurora68-display.txt` (a line in **config.txt**, not an option) | lets **Aurora68 Display** (the Mac program) choose the HDMI resolution: the kernel writes the chosen mode into `aurora68-display.txt` in the card's root (the one before it as `aurora68-display.bak`) and never edits config.txt. Put the line once, **at the end of config.txt**, after every `hdmi_` line and in an `[all]` section: the firmware takes the last value of a setting, so an `hdmi_mode=` after the include would win over PiDisplay's (PiDisplay says so). After a new mode the Pi restarts and PiScrn asks "Keep this resolution?" on the SE's screen; no answer within 15 seconds puts the previous file back and restarts again. docs/10, "PiDisplay" |
| `mac.displaywait=300` | seconds after the Pi's start within which a new HDMI mode must be kept (Aurora68 Screen Setup's question): without an answer by then the kernel puts `aurora68-display.bak` back and restarts the Pi. `0` waits for ever |
| `mac.hdmiclock=162` | the pixel clock in MHz Aurora68 Display judges the monitor's modes against: 162 by default (a Pi 0-3; Raspberry Pi's documentation names 1920x1200 at 60 Hz with reduced blanking, 154 MHz, as the highest mode before the Pi 4, and the firmware lists 1600x1200 at 60 Hz, 162 MHz). The kernel's log lists every mode of the monitor with this judgement (`[PSMAC:EDID]`) |
| `mac.edidtest=sample`, `mac.hdmisim=1`, `mac.cardearly=1` | testbed only: a made-up 27-inch 2560x1440 monitor's EDID (or `mac.edidtest=FILE`, an EDID file through semihosting), since QEMU has no monitor; the firmware's frame buffer made at the mode config.txt and `aurora68-display.txt` set, as the firmware does on a Pi; the card read before the 68k starts after a boot from a bare ROM |
| `mac.hdmi=screen` | on `mac.board=se`, what HDMI is: `screen` (the default) a second screen of its own beside the SE's, any size and depth, with an `aurora68-se-WxH.declrom` in the container and Aurora68 Screen Setup in the Extensions folder to arrange the two at the first boot (docs/10, "Two screens on the SE"); `mirror` the SE's picture (what HDMI showed until now); `log` the kernel's log for the whole run (the same as `mac.console=hdmi`). Without an `-se-` declaration ROM HDMI mirrors the SE |
| `mac.console=hdmi` | keep the kernel's log on the HDMI screen for the whole run (the same text as the serial line); without it the log shows until the Macintosh's video driver takes the screen. On the SE board the Mac then has one screen, the SE's |
| `mac.board=se` | the logic board the Pi sits in: `se` (the Macintosh SE: its chips behind the IIci ROM's addresses, docs/12), `plus` (the Macintosh Plus: the same layer, with the overlay on VIA PA4, its non-ADB keyboard and mouse as ADB devices, SCSI pseudo-DMA by polling DRQ), `512ke`, `512k` and `128k` (the Plus without SCSI; the 512K and 128K with their 20-byte clock chip, parameter RAM in `pram.bin`), docs/08 item 96 (testbed only so far), `solo` (no Macintosh: the kernel answers every chip and boots a disk image from the container, docs/12) or the default, the testbed's IIci |
| `aurora68-wifi.declrom` (a file, not an option) | the declaration ROM with the screen, PiDisk, input and the WiFi network card but no wired Ethernet: MacTCP and Riptide take the first network card they find, so this was the one for a Pi whose wired Ethernet is not driven (every Pi 3, the Zero 2 W, the bench). The kernel drives the Pi 3B's and 3B+'s wired ports now (both cards on the Mac), and leaves the wired card out of any declaration ROM by itself on a board with no wired port (3A+, Zero 2 W) or with `eth=off`, so the ordinary ones work everywhere. `aurora68-wifi-WxH.declrom` is the WiFi-only one for a larger screen. Put only one declaration ROM of a size in the container: with both `aurora68-1920x1080` and `aurora68-wifi-1920x1080` the kernel takes whichever comes last |
| `bt=off` | leave the Bluetooth chip off. Without it the kernel starts Bluetooth: on a Pi 3 the log then moves from the PL011 to the mini UART on the same header pins |
| `bt.pair=1` | pair with the first keyboard or mouse a scan finds in pairing mode, and look again every 20 s until one is connected (`bt.pair=NAME` takes only a device whose name contains NAME). The log then prints the `bt.dev=` word that makes the pairing permanent |
| `bt.dev=ADDR:KEY:KIND...` | a paired device, as the log printed it after `bt.pair=`, so it reconnects at every boot without pairing again |
| `mac.card=off` / `mac.card=ro` | the kernel's use of the Pi's own SD card (Pi 3 family, through the sdhost controller): by default it reads and writes `pairings.txt` (Bluetooth pairings) and writes the disk image's changes back into the container on the card, and keeps the solo board's parameter RAM in `pram.bin` (AppleTalk, the Network control panel's connection, Monitors' depth, the startup disk; written a second after the Mac changes it, once the Finder is up; `mac.appletalk=` and `mac.xpram=` apply on top of it). `ro` reads pairings only, `off` leaves the card alone. cmdline.txt and config.txt are never written |
| `mac.container=NAME` | the container's file name on the card, when it is not `aurora68.cpio` (the card writer looks for it in the root directory and writes to it only if its size and first sector match what was loaded) |
| `mac.image=NAME` | the disk images on the card, files of their own beside the container: by default every file in the card's root ending in `.hda`, `.img`, `.image`, `.hfv` or `.dsk` (not `kernel*.img`), in the order they were copied there, up to four; `mac.image=A.hda,B.img` names them, `mac.image=off` takes none. Used only when the container has no disk image (below, "Booting fast") |
| `mac.boot=auto` | the startup disk when a disk image on the card (`mac.image=`) holds a System: `auto` (the default) the SCSI disk the parameter RAM names (ID 0 unless the Startup Disk control panel chose another) first, then the card's image, then the other SCSI disks, so the image starts the Mac when there is no SCSI disk with a System, as in an SE with nothing attached; `image` the card's image first, the SCSI disks after it; `scsi` the parameter RAM as it is, so the image starts the Mac only if Startup Disk chose it. A floppy with a System is always taken first. The image must be bootable as a disk would be (a System Folder the Mac has started from, with its boot blocks; an Apple-partitioned `.hda` is served as its HFS partition). The kernel's log says which (`startup disk: ...`, `[MAC] PiDisk boot: ...`). docs/08 item 67 |
| `mac.usbdisk=on` | USB flash drives plugged into the Pi: `on` (the default) a stick holding HFS (bare, or an Apple-partitioned one's HFS partition) appears on the Mac's desktop while the Mac runs and ejects from the Finder; sticks formatted FAT, exFAT or HFS Plus are left on the Pi and the log says what they hold; `all` offers every stick (the Finder will offer to initialize one it cannot read: how to make an HFS stick on the Mac); `off` leaves sticks alone. Needs the new declaration ROM in the container. docs/09 "USB flash drives", docs/08 item 75 |
| `mac.usbview=off` | for a FAT32 or exFAT stick, offer only the HFS disk images in its root, not its own files as a locked disk (new in 0.12) |
| `mac.exchange=NAME` / `mac.exchange=off` | the folder in the SD card's root shown as a locked disk: `Mac Exchange` by default (new in 0.12) |
| `mac.sescsi=auto` | on `mac.board=se`, the SE's SCSI bus: `auto` (the default) looks at the bus before the Mac starts and, when nothing on it terminates it (BSY or SEL read asserted, as with no internal disk and nothing on the external port), answers the SCSI chip as an empty bus so the Mac does not wait for ever and starts from a disk image on the card; `real` always uses the SE's chip; `empty` never does. The log says which (`[PSMAC:SE] SCSI: ...`). docs/08 item 70 |
| `mac.sesound=off` | on `mac.board=se`, leave the SE's speaker alone. By default what the Mac plays (the startup chime, alert sounds, everything the Sound Manager plays) comes out of the SE's own speaker: the kernel writes the samples into the SE's sound buffer, 370 a frame, after the SE's own vertical blanking, sets the volume from the Sound control panel and switches the speaker off between sounds. With `off` the samples are thrown away as before and the startup chime is cut short again. docs/08 item 71 |
| `mac.audio=hdmi` | where the Pi plays the Mac's sound: `hdmi` (the default) the HDMI monitor's loudspeakers, in stereo; `jack` the 3.5 mm socket of a Pi 3B, 3B+ or 3A+; `off` nowhere (the SE's own speaker still plays on `mac.board=se`). The Mac's ASC (alert sounds, the startup chime, every program that plays through the Sound Manager) and the sound card below are mixed on the Pi's core 1 and resampled to the output's rate. HDMI needs the firmware to drive the monitor as HDMI, not DVI: `hdmi_drive=2` in config.txt. The log says what it found (`[AUDIO] ...`). docs/08 item 74 |
| `mac.audiorate=48000` | the output's sample rate, `48000` (the default) or `44100` |
| `mac.sndcard=off` | leave out the sound card: 16-bit stereo at 44.1 kHz, a ring in slot 9's window that the **Aurora68 Sound** extension (Sound Manager 3.0 or later) makes the Sound Manager's output device. By default it is there whenever `mac.audio` has an output; with `off` the Sound Manager stays on the ASC, whose sound still reaches HDMI (8 bits, 22 kHz). On the SE board the card's sound is played on the SE's speaker too |
| `mac.audiotest=3` | play a test at start: `N` seconds of 440 Hz on the left and 660 Hz on the right from the kernel; `card` three seconds of 441 Hz left and 882 Hz right written by the Aurora68 Sound extension into the card's ring. For checking the channels on a new monitor or cable |
| `mac.audiodump=PATH` | testbed only: the output written into a host file through semihosting (16-bit little-endian stereo at the output's rate) instead of HDMI; `tools/audiocheck.py PATH` lists its sounds and their spectra |
| `mac.video=off` | leave out the video card, a NuBus card in slot $C that decodes MPEG-1 video with MP2 sound (what the video helper sends, docs/16) on the Pi's core 1 and draws it into the Mac's own screen, the SE's included; the Mac's program Aurora68 Video (issue #24) feeds it. By default it is there (1 MiB of the Pi's memory). docs/08 item 98 |
| `mac.videotest=PATH,X,Y,W,H,SCREEN[,DELAY[,PAUSE_AT[,SEEK_AT]]]` | testbed only: play an MPEG program stream from a host file through the video card, at X,Y, W x H on screen SCREEN (0: the first, the SE's own on the SE board; 1: the SE board's HDMI screen), DELAY seconds after start; PAUSE_AT pauses for two seconds at that many seconds into the film, SEEK_AT goes back to the start there. Feeds the card's ring as the Mac's program will. docs/08 item 98 |
| `mac.vcdepth=32` | on `mac.board=se` with two screens: ask the firmware for a 32-bit HDMI frame buffer. By default the HDMI screen is drawn into the buffer the firmware made at power-on when it has the screen's size (16 bits a pixel on the monitor whatever the Mac's depth, millions included: the buffer known to be on the screen, docs/08 item 73); a `mac.screen=` of another size gets a new buffer either way |
| `mac.sepram=chip` | on `mac.board=se`, when the SE's clock chip does not answer: leave it so (the IIci ROM then cannot start). By default the kernel answers in its place with parameter RAM and a clock kept in `pram.bin` in the card's root, saved a second after the Mac changes something (at most every ten seconds, and every ten minutes for the clock); the clock goes on from the last save, since the Pi has no clock of its own. A clock chip that answers is used as it is and `pram.bin` is not touched. docs/08 items 64 and 72 |
| `mac.disks=scsi` | on the solo board every disk image is a SCSI disk. By default the first is SCSI ID 0, the startup disk, and every further one a PiDisk volume, which mounts by itself; an Apple-partitioned image is served as its first HFS partition (docs/08 item 52) |
| `mac.synclog=1` | send every log character to the serial line before going on, as the kernel used to (by default the service core sends the log from a ring, so the 115200 baud line no longer holds the kernel up) |
| `mac.deaduart=1` | testbed only: the serial port looks full to the kernel, as the Pi 3's PL011 does when Bluetooth has it (it still prints); `=2` also waits for every character as the kernel used to (docs/08 item 58) |
| `mac.cardslow=N`, `mac.cardstall=S` | testbed only: N microseconds more per card sector read (a slow card), and core 3 kept away from the card for S seconds once a disk image starts coming in (docs/08 item 54) |
| `mac.scsi0=PATH` | testbed only: a host disk image for the solo board's SCSI ID 0 (`mac.scsi1=` ... `mac.scsi6=`), with `mac.semihost=1` for host file access without the MAME bridge. On hardware the images come from the container |
| `mac.selinefault=N`, `mac.selinefault=N,M`, `mac.sepa4=1`, `mac.sefloat=1` | testbed only, `mac.board=se`: address line N stuck high, or N joined to M, on the wire to the SE's memory (the kernel's address-line test at the start names such faults: `address lines A1-A21 good`, or which line and which socket pins, docs/08 item 68); VIA port A bit 4 read as 1, which made the IIci ROM take the SE for an IIci with parity and the System say "Parity has been disabled" (docs/08 item 69); the SE's SCSI chip read as on an unterminated bus (`mac.sefloat=1`, docs/08 item 70) |
| `mac.watchtraps=1` | debugging: report odd writes into the OS trap tables (`[PSMAC:WATCH]`, docs/08 item 12). Off by default since 20 November: it costs a trap on every write to low memory's first page |
| `mac.solotrace=N` | the solo board's first N SCSI commands and ADB commands in the log |
| `mac.bridge=gpio` | how the motherboard is reached: `gpio` (the classic PiStorm's CPLD, the default on hardware), `semihost` or `uart` (the testbed's bridge to MAME) |
| `mac.power=off` | stop watching the Pi's power. By default the kernel reads the firmware's under-voltage and throttling bits every two seconds and logs every change (`[PSMAC] power: under-voltage now ...`): in the SE the Pi is powered through the 68000 socket (docs/12, "Power through the socket") |
| `mac.vcswap=1` | swap red and blue on the second screen, should a board's firmware disagree with the kernel about the pixel byte order (the firmware is asked; this overrides its answer) |
| `mac.recovery=1` | start in Aurora68 Recovery: the 68k is not started, the Pi serves its SD card on the network (README "Recovery", docs/08 item 106). "Restart normally" on the recovery page takes the option out again |
| `mac.bootcount=3` | the starts in a row without the Finder after which the next start is a recovery start (counted in `boot.txt` in the card's root); `off` for none, for a Mac whose startup program is not the Finder |
| `mac.switchsim=1`, `mac.recoverypin=NNNN` | testbed only: the programmer's interrupt switch held at power-on (MAME's SE has none to press), and a fixed recovery PIN for scripts (never use it on a real card) |
| `remote=on remote.token=TOKEN` | the remote control (docs/13): the Mac's screen, mouse, keyboard, the kernel's log and a restart over HTTP from the local network, for a browser (`http://aurora68-pi.local/`) or an agent (`tools/aurora68-mcp.py`, an MCP server for Claude Code and Claude Desktop; `tools/aurora68.py`, a command line). On the wired port the kernel gets an address of its own by DHCP (its own MAC address); on the WiFi it takes port 8068 on the Mac's address (`http://MAC-ADDRESS:8068/`). Off by default; the token (8 characters or more) is required. `remote.name=`, `remote.ip=A.B.C.D/24,ROUTER`, `remote.link=wired\|wifi`, `remote.wifi=own`, `remote.any=1`: docs/13 |

### Booting fast
In the SE the Pi is the processor, so the SE stays dark until the Pi's firmware has read its files,
the kernel has set up and the 68k starts. The kernel's log times every step, `[BOOT] t=N.NNNs what`,
with t counted from power-on (the Pi's system timer starts at reset; the first line, "kernel
entry", is the firmware's part). docs/08 item 50 has the measurements.
- **The container holds no disk image.** The firmware reads the whole initramfs before the kernel
  starts, 10 to 20 MB a second on a Pi 3: a 100 MB container is five to ten seconds. Keep the ROM,
  the declaration ROM and the WiFi/Bluetooth firmware in it (about 1 MB) and put the disk image on
  the card as a file of its own (`MyMac.hda`, or `mac.image=NAME`). The kernel finds it a moment
  after the 68k has started and reads it in the background; on the bench (`mac.board=solo`) it is
  the startup disk and what the Mac asks for is read first, in the SE it is the PiDisk volume, and
  the startup disk too when no SCSI disk holds a System (`mac.boot=`, above).
  The Mac's writes go back into the file.
- **config.txt:** `boot_delay=0`; `kernel8.img` as the release ships it, gzip-compressed (the
  firmware inflates it); `initramfs aurora68.cpio 0x10000000`. Optional: `disable_splash=1` (a
  little faster, without the rainbow square that shows the firmware found its files) and, in the
  SE with HDMI at 640 by 480 or none, `gpu_mem=16` with `start_cd.elf` and `fixup_cd.dat` on the
  card (the cut-down firmware; without them the Pi does not start).
- **cmdline.txt for the SE:** `enable_cache mac.board=se` and the `wifi.`/`bt.` words you use
  (`mac.board=plus`, `512ke`, `512k` or `128k` for the other compact Macs, above). For
  the bench: `enable_cache mac.board=solo mac.screen=WxH` with the declaration ROM of that size.
- **The serial port:** the kernel no longer waits for a serial port that does not send. On a Pi 3
  with Bluetooth on the PL011 that wait was 15 seconds of the firmware's rainbow screen.
- **The ROM:** the patch (rom/patches/iici.json, applied by the kernel and by make-container.py)
  skips the IIci ROM's memory test (it tests the Pi's memory, which cannot fail as SIMMs do),
  shortens the startup chime nobody hears, and waits 10 ms instead of 250 at each empty SCSI ID.

### Remote control
With `remote=on remote.token=TOKEN` in cmdline.txt the kernel answers on the local network at
`http://aurora68-pi.local/` (or the address its log prints, `[REMOTE] address ...`): a page with the
Mac's screen that clicks and types, and an HTTP API that `tools/aurora68-mcp.py` turns into tools
for an agent (screenshot, click, move, drag, type, key, log, state, restart), so a problem can be
shown and reproduced without a photograph. It lists the Mac's windows and takes a picture of one
of them (`/windows`, `/screenshot?window=...`; the page's Windows panel, `aurora68.py windows` and
`aurora68.py screenshot --window "title" out.png`, `--front` to bring it forward first), read from the Mac's memory by the kernel. It needs nothing on the Mac and works while the Mac is
hung or shows a system error. With the **Aurora68 Agent** extension on the Mac it also lists, copies
and changes files and starts programs: `aurora68.py ls`, `pull` (MacBinary with `--macbinary`),
`push`, `mkdir`, `rm`, `mv`, `open`, `apps`, `quit`, and the MCP server's `list_files`, `read_file`,
`write_file`, `open`, `list_apps` (docs/13, "Files and applications"). Setting it up, Claude Code's
`claude mcp add` line and Claude Desktop's configuration: docs/13.

### Recovery
For a Mac that will not start, or a Pi whose SD card cannot be reached because it is inside the Mac (docs/08
item 106). In a recovery start the 68k is never started and no Mac code runs: the Pi serves its own SD card on
the network and shows on the Mac's screen (SE, Plus, 512K) and on HDMI what to open: "Aurora68 Recovery", why,
the address and `http://aurora68.local/`, a four-digit PIN, the network and what is being done.

Getting in, any of:
- **three starts in a row that did not reach the Finder**: the next start is a recovery start (`boot.txt` in the
  card's root keeps the count; `mac.bootcount=N` for another number, `off` for none). Switching the Pi off during
  a start before the Finder counts too; a restart from the Finder does not;
- **the programmer's interrupt switch held** while the SE, Plus or 512K is switched on (the switch on the side,
  the one nearer the back), or **Esc held** for a second on a USB keyboard in the first seconds (the Pi then
  restarts into recovery);
- **`recovery.txt`** in the card's root (it is taken away on entry): `aurora68.py restart recovery` through the
  remote control writes it, and so does the recovery page's "restart into recovery";
- **`mac.recovery=1`** in cmdline.txt ("restart normally" takes it out again).

On the network: the wired port or a USB Ethernet adapter (`eth=usb`) by DHCP, or with no DHCP server (a laptop on
a direct cable) an address of its own from 169.254/16 after about ten seconds, found as `aurora68.local`; the WiFi
with the `wifi.ssid/psk/country` options. Without any of those, a USB keyboard on the Pi opens a menu on the
recovery screen with Return: choose a network from a scan, type its password and country, set `mac.fpu`,
`mac.boot` and `mac.console`, save, restart (the Plus's and 512K's own keyboards work too; the SE's own keyboard
does not, as the Mac's ROM reads it).

The page (`http://ADDRESS/`, a phone will do) asks for the PIN shown on the screen once, then lists the card's
files with Get, Rename and Delete, uploads any file of any size (each read back from the card and compared), changes
the WiFi settings (a scan, a test join, then save), the common options as a form, cmdline.txt and config.txt as
text (a `.bak` kept of each), shows the last start's `aurora68.log`, and restarts normally or into recovery.
**A new kernel8.img is checked before it is used** (its gzip data and checksum, the ARM64 header, that it is an
Aurora68 kernel); the one it replaces stays on the card as `kernel8.old`, and "Roll back" puts it back. The same
from a terminal: `aurora68.py recovery --url http://aurora68.local --pin 1234 ls | put kernel8.img | get NAME |
rm NAME | mv A B | cmdline [TEXT] | opt mac.fpu=none -bt.pair | wifi scan | wifi test --ssid S --psk P --country GB |
log | rollback | reboot [normal|recovery]`.

Security: every request but the page needs the PIN, a new one at each recovery start; after ten wrong PINs there
is a new one and a minute's pause; only the local network is answered; the PIN is never written to the log, and
the WiFi password and the remote's token are never sent back (shown as `***` or "set"). Recovery works with the
remote control off.

**What it cannot do:** a kernel that stops before it has read the SD card cannot rescue itself, and as far as we
know a Pi 3's firmware has no fallback kernel of its own. Keep a known good kernel8.img; a microSD extension cable
led out of the Mac's case is the physical way back for that case.

### Upgrading
Replace `kernel8.img` and `aurora68.declrom`, rebuild `aurora68.cpio`, keep your `config.txt`
and `cmdline.txt`. The release notes say when a Macintosh-side file changed.

## The hardware
Photographs of the first boots on a real Macintosh SE (20 November 2026): `docs/14-the-se-in-pictures.md`.

The target is a classic PiStorm (the DIP-64 board with the EPM240 CPLD made for the Amiga 500
and 2000) in a socket in the SE's 68000 position, carrying a Raspberry Pi 3. The classic board
takes a Zero 2 W, 3A+, 3B or 3B+; a Pi 4 needs a different board. The board is laid out for the
3A+'s footprint: a 3A+ or a Zero 2 W fits it, a 3B or 3B+ overhangs it, and the original Zero W
(ARMv6, 32-bit only) cannot run the kernel at all. Which Pi does what (docs/08 item 60):

| Pi | Where | WiFi (files) | Bluetooth | Wired Ethernet | USB keyboard/mouse | Card writer | RAM: Mac default |
|---|---|---|---|---|---|---|---|
| 3A+ | the SE (fits the board) or the bench | BCM43455 (`cyfmac43455-sdio-standard.bin`, `brcmfmac43455-sdio.txt`, `.clm_blob`), 2.4 and 5 GHz | BCM4345C0 | none (`eth=usb`: a USB adapter) | the one USB-A port, a hub on it for both | yes | 512 MB: 128 MB |
| Zero 2 W | the SE (smaller than a 3A+, same header position; it needs a soldered 40-pin header, or buy the Zero 2 WH) or the bench (mini-HDMI) | BCM43436 or 43436s (`brcmfmac43436-sdio.*` or `brcmfmac43436s-sdio.*`), 2.4 GHz | BCM43430B0 | none (`eth=usb`) | the micro-USB OTG port through an adapter, a hub on it for both | yes | 512 MB: 128 MB |
| 3B | the bench; the SE with the Pi overhanging | BCM43438 (`cyfmac43430-sdio.*`), 2.4 GHz | BCM43430A1 | LAN9514 on USB | four ports (behind the LAN9514) | yes | 1 GB: 128 MB |
| 3B+ | the bench; the SE with the Pi overhanging | BCM43455 | BCM4345C0 | LAN7800 on USB (not yet met on hardware) | four ports | yes | 1 GB: 128 MB |
| 4B, 400 | the bench only | BCM43455 | BCM4345C0 (400: C5) | GENET | not yet | no | 2-8 GB: 128 MB |
| Zero W (2017) | nowhere: ARMv6, 32-bit | | | | | | |

The 3A+ has no device tree of its own: its firmware loads `bcm2710-rpi-3-b-plus.dtb`, and the
kernel tells it from a 3B+ by the board's revision code. The 3A+ is the one tested in QEMU
(`raspi3ap`); QEMU has no Zero 2 W, which was checked by reading only. `docs/12-hardware-bringup.md`
is the plan and what is known about the fit, the CPLD firmware (stock first; Frimples'
DirectSocket E-clock build if the VIA needs it), and what a Pi 3 changes against the Pi 4 the
network modules were written for: WiFi works the same way (the firmware differs between a 3B
and a 3B+), the wired port is a USB chip (the 3B's LAN9514, the 3B+'s LAN7800, driven since
19 November by `psmac_usbnet.c` over the DWC2 host that also serves USB keyboards and mice;
docs/08 item 49) instead of the Pi 4's GENET, and Bluetooth shares the Pi 3's one PL011 with the
console. The SE's own keyboard and
mouse work regardless. The kernel's **board layer** (`mac.board=se`) already runs the IIci ROM
against a Macintosh SE's logic board in the testbed, MAME's `macse` with the bridge CPU:
`docs/img/system71-finder-on-macse.png` is System 7.1's Finder on the SE's own 512 by 342
screen, booted from the SE's SCSI disk, with PiDisk mounted beside it (docs/08 item 28). The same
layer serves the Plus, 512Ke, 512K and 128K (`mac.board=plus|512ke|512k|128k`), against MAME's
`macplus_ps`, `mac512ke_ps` and `mac512k_ps` (docs/08 item 96).

## Layout
| Directory | Contents |
|---|---|
| `emu68/` | the Emu68 fork (vendored; its own history is the branch `emu68-psmac`): the kernel. `src/psmac/` is the Macintosh host layer (bus bridge, 68030 MMU shim, mailbox window); `src/alice/` are the Alice68 service modules (network, Bluetooth, status, USB) with their Macintosh glue |
| `mac/` | the Macintosh side in 68k assembly: the `.PiDisk` block driver, the `.ENET`-interface Ethernet driver, the `.PiInput` keyboard-and-mouse driver, the `.Display_Video_PiStorm` second-display driver, the Aurora68 Disk, Machine and Sound extensions, the Aurora68 Network, Ping, Screens and Colour test applications, the Aurora68 Wi-Fi, Bluetooth and Status applications, and in C (with the kit in `mac/c`) Aurora68 Log, Slots, Depth, Display, the Wi-Fi & Bluetooth menus, Screen Setup and Aurora68 Play (MP3, FLAC, Ogg Vorbis, Apple Lossless, AIFF and WAV on the sound card, the music server's library and devices, and internet radio over MacTCP or Riptide's `.IPP6`); their sources keep the old names (`mac/piplay/`, `pislots.c`...), a Makefile (`make check` compares the drivers' structure offsets with the kernel's header) |
| `tools/` | ROM patching, the initramfs container builder (`mkcontainer.py`: ROM, declaration ROM and firmware in one cpio for the Pi), the full-stack testbed launcher, the host network peer, debugging helpers |
| `tools/mac/` | resource fork, MacBinary, application, extension and declaration ROM builders (pure Python) |
| `rom/` | ROM patch descriptions and the patch tool's Makefile (ROM images themselves are not in the repository) |
| `mame/` | Lua scripts for the MAME testbed and the testbed's configuration |
| `reference/` | the MAME testbed patch and instructions to recreate the MAME checkout; third-party checkouts live here but are not committed |
| `docs/` | the documents |
| `release/` | the SD card's `config.txt`, `cmdline.txt` and read-me; `tools/mkrelease.sh VERSION` builds the release zip |
| `dev/` | the development kit: a build container, the workspace of the four repositories and its CLAUDE.md, test-image and test-set tools, testbed helpers (`dev/README.md`) |

## Building and running the testbed
The kernel: see `docs/08` ("What exists"). The Mac side: `make -C mac`. The whole stack:
`tools/run-fullstack.sh SECONDS [bootargs...]` with `ROM=`, `HD=`, `LOGDIR=` in the environment;
`docs/09` gives the complete workflow with a disk image, the declaration ROM and the network peer.
The solo board alone, no MAME: `tools/run-solo.sh SECONDS [bootargs...]` (`HD=` a raw disk image,
`SHOT=20` for a screenshot every 20 s, `CONTAINER=file.cpio` to boot the hardware's way); System
7.1 reaches the Finder on the second screen in about 100 s.

## Licences
The Emu68 fork is MPL-2.0 like Emu68 itself. The Alice68 service modules under `emu68/src/alice/`
are GPL-2.0-only, as in Alice68. The Macintosh-side drivers, tools and documents of this project
are MPL-2.0. Apple ROM images and disk images are not distributed.

Third-party code compiled into the kernel beyond Emu68 and Alice68: **minimp3** by lieff
(https://github.com/lieff/minimp3), the MPEG audio decoder behind Aurora68 Play, dedicated to the public domain
under CC0 1.0 and vendored unmodified in `emu68/src/psmac/third_party/minimp3/` with its licence text
and a note of the commit it was taken from; **dr_flac** by David Reid (https://github.com/mackron/dr_libs,
release 0.13.4, public domain or MIT No Attribution) and **stb_vorbis** by Sean Barrett and contributors
(https://github.com/nothings/stb, version 1.25, MIT or public domain), Aurora68 Play's FLAC and Ogg Vorbis
decoders, vendored unmodified beside it in the same way. Apple Lossless is decoded by the project's own
`psmac_alac.c` (MPL-2.0): Apple's Apache-2.0 decoder cannot be combined with the GPL-2.0-only Alice68
modules in one kernel.

Aurora68 Play's Radio menu lists the public MP3 streams of **SomaFM** (https://somafm.com/), listener-supported,
commercial-free internet radio from San Francisco; if you listen, consider supporting them
(https://somafm.com/support/). Its other submenus list the public streams of other listener-supported
stations (Radio Paradise, KEXP, WFMU, WWOZ) and of broadcasters in nine countries (docs/09, "Groups of
stations"); several of them are paid for by their listeners too. Aurora68 holds only the streams'
addresses: nothing of any station's is recorded or redistributed.
