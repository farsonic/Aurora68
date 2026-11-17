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
2. Copy the **Raspberry Pi firmware** onto it from
   [raspberrypi/firmware](https://github.com/raspberrypi/firmware/tree/master/boot):
   `bootcode.bin`, `start.elf`, `fixup.dat`, `bcm2710-rpi-3-b.dtb` for a Pi 3 or Zero 2 W;
   `start4.elf`, `fixup4.dat`, `bcm2711-rpi-4-b.dtb` for a Pi 4 or CM4.
3. Unzip the release and copy **everything in it** onto the card's root: `kernel8.img`,
   `config.txt`, `cmdline.txt`, `aurora68.declrom`, `make-container.py`, the `Mac` folder, the
   notes and licences.
4. Copy **your own** Macintosh IIci ROM (version 368CADFE, 512 KB) next to them and build the
   container the kernel loads as `initramfs`:
   ```bash
   python3 make-container.py -o aurora68.cpio MacIIci_368CADFE.rom aurora68.declrom
   ```
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
   python3 make-container.py -o aurora68.cpio MacIIci_368CADFE.rom aurora68.declrom \
     cyfmac43455-sdio-standard.bin brcmfmac43455-sdio.txt cyfmac43455-sdio.clm_blob BCM4345C0.hcd
   ```
   Those are the files for a Pi 3B+, 3A+ or 4 (BCM43455). A **Pi 3B** carries the older BCM43438
   and wants these instead (the kernel picks by the chip's id):
   ```bash
   curl -fL --remote-name-all \
     https://raw.githubusercontent.com/RPi-Distro/firmware-nonfree/bullseye/debian/config/brcm80211/brcm/brcmfmac43430-sdio.bin \
     https://raw.githubusercontent.com/RPi-Distro/firmware-nonfree/bullseye/debian/config/brcm80211/brcm/brcmfmac43430-sdio.txt \
     https://raw.githubusercontent.com/RPi-Distro/firmware-nonfree/bullseye/debian/config/brcm80211/brcm/brcmfmac43430-sdio.clm_blob \
     https://raw.githubusercontent.com/RPi-Distro/bluez-firmware/master/broadcom/BCM43430A1.hcd
   python3 make-container.py -o aurora68.cpio MacIIci_368CADFE.rom aurora68.declrom \
     brcmfmac43430-sdio.bin brcmfmac43430-sdio.txt brcmfmac43430-sdio.clm_blob BCM43430A1.hcd
   ```
   (On Windows, `curl.exe` and the `curl` part as one line.) The kernel finds the files in the
   container by name (`cyfmac43455-sdio-standard` and `brcmfmac43455-sdio` are both accepted). Apple's ROM and Broadcom's firmware are not included in the release.
5. Put the Pi in the PiStorm, the PiStorm in the SE's 68000 socket, HDMI to a monitor if you
   want the second screen, and power on.

The Macintosh side needs no installation: the Slot Manager loads the Pi's drivers from the
declaration ROM. The `Mac` folder holds the extensions and programs (PiMach, PiDisk, PiWiFi,
PiBlue, PiStatus and the test programs) as MacBinary files and on an HFS image.

### config.txt
```ini
[all]
kernel=kernel8.img
arm_64bit=1
initramfs aurora68.cpio followkernel   # the ROM, the declaration ROM and firmware in one file
enable_uart=1                          # the kernel's log on the serial pins, 115200 8N1
hdmi_group=2
hdmi_mode=4                            # 640 x 480: the second screen, one pixel to one
```
The Pi's HDMI output is the Macintosh's second screen. By default it is 640 by 480 and the
firmware scales it to whatever HDMI mode it chose. For a larger Macintosh screen, one pixel to one,
set the HDMI mode and tell the kernel the same size with `mac.screen=` (below) and put the matching
declaration ROM (`aurora68-1024x768.declrom` and so on) in the container in place of
`aurora68.declrom`. `hdmi_mode=16` is 1024 by 768, `35` is 1280 by 1024, `82` is 1080p. The screen
offers black and white, 4, 16, 256, thousands and millions of colours in the Monitors control
panel; the ROM sizes the Macintosh's memory and the frame buffer is the Pi's, so a 1080p screen in
millions of colours costs the Macintosh nothing. `docs/img/system71-pi-1024x768-millions.png`
is the Pi's screen at 1024 by 768 in millions of colours.

### cmdline.txt
One line, options separated by spaces, no comments:

| Option | Meaning |
|---|---|
| `mac.ram=128` | Macintosh memory in MB. Default: what the Pi can spare after the kernel's 192 MB, up to 128, which is all the IIci ROM sizes (two banks of 64). A 512 MB Pi Zero 2 W and an 8 GB Pi 4 both get 128 |
| `enable_cache` | Emu68's instruction cache; leave it on |
| `wifi.ssid=NAME wifi.psk=PASSWORD wifi.country=GB` | join a WiFi network at boot |
| `wifi=off`, `bt=off`, `eth=off` | leave a radio or the Ethernet off |
| `net.oui=off` | keep the Pi's own MAC addresses (default: Commodore's prefix, as Alice68) |
| `mac.screen=1024x768` | the second screen's size in pixels (default 640x480; 512 to 1920 by 342 to 1200). Set `hdmi_mode` to match and use the declaration ROM of that size |
| `mac.vcswap=1` | swap red and blue on the second screen, should a board's firmware disagree with the kernel about the pixel byte order (the firmware is asked; this overrides its answer) |

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
mouse work regardless.

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

## Licences
The Emu68 fork is MPL-2.0 like Emu68 itself. The Alice68 service modules under `emu68/src/alice/`
are GPL-2.0-only, as in Alice68. The Macintosh-side drivers, tools and documents of this project
are MPL-2.0. Apple ROM images and disk images are not distributed.
