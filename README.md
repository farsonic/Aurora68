# Aurora68 — PiStorm-SE

An Emu68-based accelerator and expansion for the Macintosh SE: a Raspberry Pi in the 68000
socket presents a 68040, runs Apple's Macintosh IIci ROM, keeps the SE's own screen, keyboard,
mouse, floppy and SCSI chip working, and adds memory, disk images from the SD card, Ethernet,
WiFi and Bluetooth through a NuBus card that exists only in software.

**Building it:** `CLAUDE.md` lists every tool and version, the build command for each artifact (kernel, Macintosh side, ROM patch, MAME testbed, disk images, containers), how the testbed is run and how each dependency is updated.

Start with `docs/00-overview.md`: it tells the story of the project and points to the working
documents for each part. The day-by-day record of what was found and fixed is `docs/08`.

## Quick start (release 0.1)

Release 0.1 is the testbed build packaged for the SD card: everything in it runs under
emulation, and the PiStorm GPIO transport that connects the kernel to a real SE motherboard is
the next piece of work, so on hardware this build boots the Pi and waits (see
`RELEASE-NOTES.md`). The card layout is final, and 0.2 will be a drop-in `kernel8.img`.

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
   For WiFi and Bluetooth on a Pi with the Broadcom CYW43455 (Pi 3B+, 3A+, 4, Zero 2 W, CM4),
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
declaration ROM. The `Mac` folder holds the extensions and programs (PiMach, PiDisk, PiWiFi,
PiBlue, PiStatus and the test programs) as MacBinary files and on an HFS image.

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
```
The Pi's HDMI output is the Macintosh's second screen. By default it is 640 by 480 and the
firmware scales it to whatever HDMI mode it chose. For a larger Macintosh screen, one pixel to one,
set the HDMI mode and tell the kernel the same size with `mac.screen=` (below) and put the matching
declaration ROM (`aurora68-1024x768.declrom` and so on) in the container in place of
`aurora68.declrom`. `hdmi_mode=16` is 1024 by 768, `35` is 1280 by 1024, `82` is 1080p, `69` is
1920 by 1200 (60 Hz, reduced blanking: the Pi 3's HDMI cannot drive the full-blanking `68`). The screen
offers black and white, 4, 16, 256, thousands and millions of colours in the Monitors control
panel; the ROM sizes the Macintosh's memory and the frame buffer is the Pi's, so a 1080p screen in
millions of colours costs the Macintosh nothing. `docs/img/system71-pi-1024x768-millions.png`
is the Pi's screen at 1024 by 768 in millions of colours; `docs/img/system71-about-128mb.png`
is About This Macintosh with 128 MB.

### cmdline.txt
One line, options separated by spaces, no comments:

| Option | Meaning |
|---|---|
| `mac.ram=128` | Macintosh memory in MB. Default: what the Pi can spare after the kernel's 192 MB, up to 128, which is all the IIci ROM sizes (two banks of 64). A 512 MB Pi Zero 2 W and an 8 GB Pi 4 both get 128 |
| `enable_cache` | Emu68's instruction cache; leave it on |
| `wifi.ssid=NAME wifi.psk=PASSWORD wifi.country=GB` | join a WiFi network at boot |
| `wifi=off`, `bt=off`, `eth=off` | leave a radio or the Ethernet off |
| `net.oui=off` | keep the Pi's own MAC addresses (default: Commodore's prefix, as Alice68) |
| `mac.screen=1024x768` | the second screen's size in pixels (default 640x480; 512 to 1920 by 342 to 1200, every size up to 1920x1200 in millions of colours). Set `hdmi_mode` to match and use the declaration ROM of that size (`aurora68-1920x1200.declrom` for `mac.screen=1920x1200`, `hdmi_mode=69`) |
| `mac.console=hdmi` | keep the kernel's log on the HDMI screen for the whole run (the same text as the serial line); without it the log shows until the Macintosh's video driver takes the screen |
| `mac.board=se` | the logic board the Pi sits in: `se` (the Macintosh SE: its chips behind the IIci ROM's addresses, docs/12), `solo` (no Macintosh: the kernel answers every chip and boots a disk image from the container, docs/12) or the default, the testbed's IIci |
| `aurora68-wifi.declrom` (a file, not an option) | the declaration ROM with the screen, PiDisk, input and the WiFi network card but no wired Ethernet: MacTCP and Riptide take the first network card they find, so this was the one for a Pi whose wired Ethernet is not driven (every Pi 3, the Zero 2 W, the bench). The kernel now leaves the wired card out of any declaration ROM by itself when the Pi has no GENET (the Pi 4's Ethernet) or `eth=off` is given, so the ordinary ones work on a Pi 3 too. `aurora68-wifi-WxH.declrom` is the WiFi-only one for a larger screen. Put only one declaration ROM of a size in the container: with both `aurora68-1920x1080` and `aurora68-wifi-1920x1080` the kernel takes whichever comes last |
| `bt=off` | leave the Bluetooth chip off. Without it the kernel starts Bluetooth: on a Pi 3 the log then moves from the PL011 to the mini UART on the same header pins |
| `bt.pair=1` | pair with the first keyboard or mouse a scan finds in pairing mode, and look again every 20 s until one is connected (`bt.pair=NAME` takes only a device whose name contains NAME). The log then prints the `bt.dev=` word that makes the pairing permanent |
| `bt.dev=ADDR:KEY:KIND...` | a paired device, as the log printed it after `bt.pair=`, so it reconnects at every boot without pairing again |
| `mac.card=off` / `mac.card=ro` | the kernel's use of the Pi's own SD card (Pi 3 family, through the sdhost controller): by default it reads and writes `pairings.txt` (Bluetooth pairings) and writes the disk image's changes back into the container on the card. `ro` reads pairings only, `off` leaves the card alone. cmdline.txt and config.txt are never written |
| `mac.container=NAME` | the container's file name on the card, when it is not `aurora68.cpio` (the card writer looks for it in the root directory and writes to it only if its size and first sector match what was loaded) |
| `mac.image=NAME` | the disk image on the card, a file of its own beside the container (default: the first file in the card's root ending in `.hda`, `.img`, `.image`, `.hfv` or `.dsk`, not `kernel*.img`); `mac.image=off` takes none. Used only when the container has no disk image (below, "Booting fast") |
| `mac.synclog=1` | send every log character to the serial line before going on, as the kernel used to (by default the service core sends the log from a ring, so the 115200 baud line no longer holds the kernel up) |
| `mac.scsi0=PATH` | testbed only: a host disk image for the solo board's SCSI ID 0 (`mac.scsi1=` ... `mac.scsi6=`), with `mac.semihost=1` for host file access without the MAME bridge. On hardware the images come from the container |
| `mac.solotrace=N` | the solo board's first N SCSI commands and ADB commands in the log |
| `mac.bridge=gpio` | how the motherboard is reached: `gpio` (the classic PiStorm's CPLD, the default on hardware), `semihost` or `uart` (the testbed's bridge to MAME) |
| `mac.vcswap=1` | swap red and blue on the second screen, should a board's firmware disagree with the kernel about the pixel byte order (the firmware is asked; this overrides its answer) |

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
  the startup disk and what the Mac asks for is read first, in the SE it is the PiDisk volume.
  The Mac's writes go back into the file.
- **config.txt:** `boot_delay=0`; `kernel8.img` as the release ships it, gzip-compressed (the
  firmware inflates it); `initramfs aurora68.cpio 0x10000000`. Optional: `disable_splash=1` (a
  little faster, without the rainbow square that shows the firmware found its files) and, in the
  SE with HDMI at 640 by 480 or none, `gpu_mem=16` with `start_cd.elf` and `fixup_cd.dat` on the
  card (the cut-down firmware; without them the Pi does not start).
- **cmdline.txt for the SE:** `enable_cache mac.board=se` and the `wifi.`/`bt.` words you use. For
  the bench: `enable_cache mac.board=solo mac.screen=WxH` with the declaration ROM of that size.
- **The ROM:** the patch (rom/patches/iici.json, applied by the kernel and by make-container.py)
  skips the IIci ROM's memory test (it tests the Pi's memory, which cannot fail as SIMMs do),
  shortens the startup chime nobody hears, and waits 10 ms instead of 250 at each empty SCSI ID.

### Upgrading
Replace `kernel8.img` and `aurora68.declrom`, rebuild `aurora68.cpio`, keep your `config.txt`
and `cmdline.txt`. The release notes say when a Macintosh-side file changed.

## The hardware
The target is a classic PiStorm (the DIP-64 board with the EPM240 CPLD made for the Amiga 500
and 2000) in a socket in the SE's 68000 position, carrying a Raspberry Pi 3. The classic board
takes a Zero 2 W, 3A+, 3B or 3B+; a Pi 4 needs a different board. `docs/12-hardware-bringup.md`
is the plan and what is known about the fit, the CPLD firmware (stock first; Frimples'
DirectSocket E-clock build if the VIA needs it), and what a Pi 3 changes against the Pi 4 the
network modules were written for: WiFi works the same way (the firmware differs between a 3B
and a 3B+), there is no Gigabit Ethernet, Bluetooth shares the Pi 3's one PL011 with the
console, and USB keyboards need a DWC2 driver that does not exist yet. The SE's own keyboard and
mouse work regardless. The kernel's **board layer** (`mac.board=se`) already runs the IIci ROM
against a Macintosh SE's logic board in the testbed, MAME's `macse` with the bridge CPU:
`docs/img/system71-finder-on-macse.png` is System 7.1's Finder on the SE's own 512 by 342
screen, booted from the SE's SCSI disk, with PiDisk mounted beside it (docs/08 item 28).

## Layout
| Directory | Contents |
|---|---|
| `emu68/` | the Emu68 fork (vendored; its own history is the branch `emu68-psmac`): the kernel. `src/psmac/` is the Macintosh host layer (bus bridge, 68030 MMU shim, mailbox window); `src/alice/` are the Alice68 service modules (network, Bluetooth, status, USB) with their Macintosh glue |
| `mac/` | the Macintosh side in 68k assembly: the `.PiDisk` block driver, the `.ENET`-interface Ethernet driver, the `.PiInput` keyboard-and-mouse driver, the `.Display_Video_PiStorm` second-display driver, the PiDisk and PiMach extensions, the PiNet, PiPing, PiScreens and PiColor test applications, the PiWiFi, PiBlue and PiStatus applications, a Makefile (`make check` compares the drivers' structure offsets with the kernel's header) |
| `tools/` | ROM patching, the initramfs container builder (`mkcontainer.py`: ROM, declaration ROM and firmware in one cpio for the Pi), the full-stack testbed launcher, the host network peer, debugging helpers |
| `tools/mac/` | resource fork, MacBinary, application, extension and declaration ROM builders (pure Python) |
| `rom/` | ROM patch descriptions and the patch tool's Makefile (ROM images themselves are not in the repository) |
| `mame/` | Lua scripts for the MAME testbed and the testbed's configuration |
| `reference/` | the MAME testbed patch and instructions to recreate the MAME checkout; third-party checkouts live here but are not committed |
| `docs/` | the documents |
| `release/` | the SD card's `config.txt`, `cmdline.txt` and read-me; `tools/mkrelease.sh VERSION` builds the release zip |

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
