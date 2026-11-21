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
declaration ROM. The `Mac` folder holds the extensions and programs (PiMach, PiDisk, PiWiFi,
PiBlue, PiStatus, PiLog, PiSlots and the test programs) as MacBinary files and on an HFS image.
PiSlots lists the NuBus cards the Mac sees (the Pi's card in slot 9, the SE's HDMI screen in
slot $A, real cards on a IIci or SE/30): each card's maker and what its functions are and do.

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
| `enable_cache` | Emu68's instruction cache; leave it on |
| `wifi.ssid=NAME wifi.psk=PASSWORD wifi.country=GB` | join a WiFi network at boot; the country is your own two-letter code (GB, DE, US ...), not the placeholder `XX`, which the kernel ignores. A WiFi chip that does not answer is given up (`the WiFi chip does not answer (SDIO): WiFi off, the Mac carries on`) and the Mac starts as with `wifi=off` |
| `wifi=off`, `bt=off`, `eth=off` | leave a radio or the Ethernet off (`eth=off` also takes the wired card out of the Mac's slot 9) |
| `eth=usb` | on a Pi without a wired port (3A+, Zero 2 W): a USB Ethernet adapter as the Mac's wired card, a LAN9500, LAN7800 or CDC-ECM device on the USB port or behind a high-speed hub there (tested in QEMU with a CDC-ECM device only) |
| `net.oui=off` | keep the Pi's own MAC addresses (default: Commodore's prefix, as Alice68) |
| `mac.screen=1024x768` | the HDMI screen's size in pixels (default: the HDMI mode the firmware set, 640x480 in QEMU; 512 to 4096 by 342 to 2560, millions of colours up to 3840x2160). Since 20 November 2026 the kernel rewrites the declaration ROM's screen for the size in use, so `mac.screen=` is needed only to differ from the firmware's mode, and one declaration ROM serves every size (`aurora68-se.declrom` on the SE board). Older notes: set `hdmi_mode` to match and use the declaration ROM of that size (`aurora68-1920x1200.declrom` for `mac.screen=1920x1200`, `hdmi_mode=69`; on `mac.board=se`, `aurora68-se-1920x1200.declrom`; `mac.screen=2560x1440` with the custom 30 Hz mode above and `aurora68-se-2560x1440.declrom`). On the SE board the SE's own screen is always 512x342 and this is the second screen's size |
| `*.declrom` in the card's root (files, not an option) | declaration ROMs on the card beside `kernel8.img` are offered with the container's and win a tie, so a new Mac-side driver (PiDisk, the network cards, input, video) is a file copied to the card; the log names the file taken and where from (`[PSMAC] declaration ROM: ... from the card's root taken`). Keep only the ones you mean on the card (on the SE: `aurora68-se.declrom` and `aurora68-512x342.declrom`): an old file there beats the container's. A file whose PiDisk has no boot record (an older release's) is taken only when nothing else will do, since the Mac cannot start from a disk image with it, and the line `chosen over: ...` says why each other file was passed over. `mac.cardrom=off` ignores them |
| `include aurora68-display.txt` (a line in **config.txt**, not an option) | lets **PiDisplay** (the Mac program) choose the HDMI resolution: the kernel writes the chosen mode into `aurora68-display.txt` in the card's root (the one before it as `aurora68-display.bak`) and never edits config.txt. Put the line once, **at the end of config.txt**, after every `hdmi_` line and in an `[all]` section: the firmware takes the last value of a setting, so an `hdmi_mode=` after the include would win over PiDisplay's (PiDisplay says so). After a new mode the Pi restarts and PiScrn asks "Keep this resolution?" on the SE's screen; no answer within 15 seconds puts the previous file back and restarts again. docs/10, "PiDisplay" |
| `mac.displaywait=300` | seconds after the Pi's start within which a new HDMI mode must be kept (PiScrn's question): without an answer by then the kernel puts `aurora68-display.bak` back and restarts the Pi. `0` waits for ever |
| `mac.hdmiclock=162` | the pixel clock in MHz PiDisplay judges the monitor's modes against: 162 by default (a Pi 0-3; Raspberry Pi's documentation names 1920x1200 at 60 Hz with reduced blanking, 154 MHz, as the highest mode before the Pi 4, and the firmware lists 1600x1200 at 60 Hz, 162 MHz). The kernel's log lists every mode of the monitor with this judgement (`[PSMAC:EDID]`) |
| `mac.edidtest=sample`, `mac.hdmisim=1`, `mac.cardearly=1` | testbed only: a made-up 27-inch 2560x1440 monitor's EDID (or `mac.edidtest=FILE`, an EDID file through semihosting), since QEMU has no monitor; the firmware's frame buffer made at the mode config.txt and `aurora68-display.txt` set, as the firmware does on a Pi; the card read before the 68k starts after a boot from a bare ROM |
| `mac.hdmi=screen` | on `mac.board=se`, what HDMI is: `screen` (the default) a second screen of its own beside the SE's, any size and depth, with an `aurora68-se-WxH.declrom` in the container and PiScrn in the Extensions folder to arrange the two at the first boot (docs/10, "Two screens on the SE"); `mirror` the SE's picture (what HDMI showed until now); `log` the kernel's log for the whole run (the same as `mac.console=hdmi`). Without an `-se-` declaration ROM HDMI mirrors the SE |
| `mac.console=hdmi` | keep the kernel's log on the HDMI screen for the whole run (the same text as the serial line); without it the log shows until the Macintosh's video driver takes the screen. On the SE board the Mac then has one screen, the SE's |
| `mac.board=se` | the logic board the Pi sits in: `se` (the Macintosh SE: its chips behind the IIci ROM's addresses, docs/12), `solo` (no Macintosh: the kernel answers every chip and boots a disk image from the container, docs/12) or the default, the testbed's IIci |
| `aurora68-wifi.declrom` (a file, not an option) | the declaration ROM with the screen, PiDisk, input and the WiFi network card but no wired Ethernet: MacTCP and Riptide take the first network card they find, so this was the one for a Pi whose wired Ethernet is not driven (every Pi 3, the Zero 2 W, the bench). The kernel drives the Pi 3B's and 3B+'s wired ports now (both cards on the Mac), and leaves the wired card out of any declaration ROM by itself on a board with no wired port (3A+, Zero 2 W) or with `eth=off`, so the ordinary ones work everywhere. `aurora68-wifi-WxH.declrom` is the WiFi-only one for a larger screen. Put only one declaration ROM of a size in the container: with both `aurora68-1920x1080` and `aurora68-wifi-1920x1080` the kernel takes whichever comes last |
| `bt=off` | leave the Bluetooth chip off. Without it the kernel starts Bluetooth: on a Pi 3 the log then moves from the PL011 to the mini UART on the same header pins |
| `bt.pair=1` | pair with the first keyboard or mouse a scan finds in pairing mode, and look again every 20 s until one is connected (`bt.pair=NAME` takes only a device whose name contains NAME). The log then prints the `bt.dev=` word that makes the pairing permanent |
| `bt.dev=ADDR:KEY:KIND...` | a paired device, as the log printed it after `bt.pair=`, so it reconnects at every boot without pairing again |
| `mac.card=off` / `mac.card=ro` | the kernel's use of the Pi's own SD card (Pi 3 family, through the sdhost controller): by default it reads and writes `pairings.txt` (Bluetooth pairings) and writes the disk image's changes back into the container on the card, and keeps the solo board's parameter RAM in `pram.bin` (AppleTalk, the Network control panel's connection, Monitors' depth, the startup disk; written a second after the Mac changes it, once the Finder is up; `mac.appletalk=` and `mac.xpram=` apply on top of it). `ro` reads pairings only, `off` leaves the card alone. cmdline.txt and config.txt are never written |
| `mac.container=NAME` | the container's file name on the card, when it is not `aurora68.cpio` (the card writer looks for it in the root directory and writes to it only if its size and first sector match what was loaded) |
| `mac.image=NAME` | the disk images on the card, files of their own beside the container: by default every file in the card's root ending in `.hda`, `.img`, `.image`, `.hfv` or `.dsk` (not `kernel*.img`), in the order they were copied there, up to four; `mac.image=A.hda,B.img` names them, `mac.image=off` takes none. Used only when the container has no disk image (below, "Booting fast") |
| `mac.boot=auto` | the startup disk when a disk image on the card (`mac.image=`) holds a System: `auto` (the default) the SCSI disk the parameter RAM names (ID 0 unless the Startup Disk control panel chose another) first, then the card's image, then the other SCSI disks, so the image starts the Mac when there is no SCSI disk with a System, as in an SE with nothing attached; `image` the card's image first, the SCSI disks after it; `scsi` the parameter RAM as it is, so the image starts the Mac only if Startup Disk chose it. A floppy with a System is always taken first. The image must be bootable as a disk would be (a System Folder the Mac has started from, with its boot blocks; an Apple-partitioned `.hda` is served as its HFS partition). The kernel's log says which (`startup disk: ...`, `[MAC] PiDisk boot: ...`). docs/08 item 67 |
| `mac.usbdisk=on` | USB flash drives plugged into the Pi: `on` (the default) a stick holding HFS (bare, or an Apple-partitioned one's HFS partition) appears on the Mac's desktop while the Mac runs and ejects from the Finder; sticks formatted FAT, exFAT or HFS Plus are left on the Pi and the log says what they hold; `all` offers every stick (the Finder will offer to initialize one it cannot read: how to make an HFS stick on the Mac); `off` leaves sticks alone. Needs the new declaration ROM in the container. docs/09 "USB flash drives", docs/08 item 75 |
| `mac.sescsi=auto` | on `mac.board=se`, the SE's SCSI bus: `auto` (the default) looks at the bus before the Mac starts and, when nothing on it terminates it (BSY or SEL read asserted, as with no internal disk and nothing on the external port), answers the SCSI chip as an empty bus so the Mac does not wait for ever and starts from a disk image on the card; `real` always uses the SE's chip; `empty` never does. The log says which (`[PSMAC:SE] SCSI: ...`). docs/08 item 70 |
| `mac.sesound=off` | on `mac.board=se`, leave the SE's speaker alone. By default what the Mac plays (the startup chime, alert sounds, everything the Sound Manager plays) comes out of the SE's own speaker: the kernel writes the samples into the SE's sound buffer, 370 a frame, after the SE's own vertical blanking, sets the volume from the Sound control panel and switches the speaker off between sounds. With `off` the samples are thrown away as before and the startup chime is cut short again. docs/08 item 71 |
| `mac.audio=hdmi` | where the Pi plays the Mac's sound: `hdmi` (the default) the HDMI monitor's loudspeakers, in stereo; `jack` the 3.5 mm socket of a Pi 3B, 3B+ or 3A+; `off` nowhere (the SE's own speaker still plays on `mac.board=se`). The Mac's ASC (alert sounds, the startup chime, every program that plays through the Sound Manager) and the sound card below are mixed on the Pi's core 1 and resampled to the output's rate. HDMI needs the firmware to drive the monitor as HDMI, not DVI: `hdmi_drive=2` in config.txt. The log says what it found (`[AUDIO] ...`). docs/08 item 74 |
| `mac.audiorate=48000` | the output's sample rate, `48000` (the default) or `44100` |
| `mac.sndcard=off` | leave out the sound card: 16-bit stereo at 44.1 kHz, a ring in slot 9's window that the **Sound PiStorm** extension (Sound Manager 3.0 or later) makes the Sound Manager's output device. By default it is there whenever `mac.audio` has an output; with `off` the Sound Manager stays on the ASC, whose sound still reaches HDMI (8 bits, 22 kHz). On the SE board the card's sound is played on the SE's speaker too |
| `mac.audiotest=3` | play a test at start: `N` seconds of 440 Hz on the left and 660 Hz on the right from the kernel; `card` three seconds of 441 Hz left and 882 Hz right written by the Sound PiStorm extension into the card's ring. For checking the channels on a new monitor or cable |
| `mac.audiodump=PATH` | testbed only: the output written into a host file through semihosting (16-bit little-endian stereo at the output's rate) instead of HDMI; `tools/audiocheck.py PATH` lists its sounds and their spectra |
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
| `remote=on remote.token=TOKEN` | the remote control (docs/13): the Mac's screen, mouse, keyboard, the kernel's log and a restart over HTTP from the local network, for a browser (`http://aurora68-pi.local/`) or an agent (`tools/aurora68-mcp.py`, an MCP server for Claude Code and Claude Desktop; `tools/aurora68.py`, a command line). The kernel gets an address of its own by DHCP (its own MAC address on the wired port; on the WiFi the Mac's, with a DHCP client identifier of its own). Off by default; the token (8 characters or more) is required. `remote.name=`, `remote.ip=A.B.C.D/24,ROUTER`, `remote.link=wired\|wifi`, `remote.wifi=shared`, `remote.any=1`: docs/13 |

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
- **cmdline.txt for the SE:** `enable_cache mac.board=se` and the `wifi.`/`bt.` words you use. For
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
shown and reproduced without a photograph. It needs nothing on the Mac and works while the Mac is
hung or shows a system error. Setting it up, Claude Code's `claude mcp add` line and Claude
Desktop's configuration: docs/13.

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
screen, booted from the SE's SCSI disk, with PiDisk mounted beside it (docs/08 item 28).

## Layout
| Directory | Contents |
|---|---|
| `emu68/` | the Emu68 fork (vendored; its own history is the branch `emu68-psmac`): the kernel. `src/psmac/` is the Macintosh host layer (bus bridge, 68030 MMU shim, mailbox window); `src/alice/` are the Alice68 service modules (network, Bluetooth, status, USB) with their Macintosh glue |
| `mac/` | the Macintosh side in 68k assembly: the `.PiDisk` block driver, the `.ENET`-interface Ethernet driver, the `.PiInput` keyboard-and-mouse driver, the `.Display_Video_PiStorm` second-display driver, the PiDisk and PiMach extensions, the PiNet, PiPing, PiScreens and PiColor test applications, the PiWiFi, PiBlue and PiStatus applications, and in C (with the kit in `mac/c`) PiLog, PiSlots, PiDepth, the WiFi menu, PiScrn and PiPlay (MP3, AIFF and WAV on the sound card), a Makefile (`make check` compares the drivers' structure offsets with the kernel's header) |
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
(https://github.com/lieff/minimp3), the MPEG audio decoder behind PiPlay, dedicated to the public domain
under CC0 1.0 and vendored unmodified in `emu68/src/psmac/third_party/minimp3/` with its licence text
and a note of the commit it was taken from.
