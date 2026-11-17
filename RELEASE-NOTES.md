# Aurora68 release notes

## 0.3.1 — the first hardware fixes (17 November 2026, 10:00)

The kernel from the first morning on a real Pi 3B v1.2, standalone with an HDMI monitor. The
WiFi joins a WPA2 network on the 3B; the rest of 0.3 is unchanged. Only `kernel8.img` differs.

- **Black screen after the rainbow** (0.3): the VideoCore's memory map overlapped the peripheral
  window the kernel keeps at 0x3F000000 on a Pi 3 with gpu_mem=64; clipped.
- **Black screen a minute into the boot**: the VideoCore mailbox had no lock between the
  display (CPU0, the 68k's RESET) and the WiFi (CPU1), and Emu68's receive loop matched replies
  by channel only; one lock, replies matched by buffer address.
- **The Pi 3B's WiFi chip did not answer**: the firmware boots from the SD card through the
  same Arasan host the WiFi uses, and the card slot was still routed to it; the slot's pins are
  detached first on a Pi 3. The host's pin pulls go through the BCM2837's registers now (the
  BCM2711's do not exist there). WL_ON is selected by board type: the expander line on the 3B,
  3B+, 3A+, 4 and CM4, GPIO 41 on the Zero W and Zero 2 W.
- The heartbeat carries a `[WIFI]` line; the kernel's log draws each glyph completely (the
  HDMI console smeared on the hardware); the GPIO transport's reset wait is bounded to two
  seconds.
- Known: the 68k ROM loops in decoder detection with "TRANSPORT DEAD" until a PiStorm board
  and an SE are behind the Pi, as expected.

## 0.3 — the first build for the hardware (17 November 2026, 05:30)

For a classic PiStorm (EPM240, stock firmware) in a Macintosh SE's 68000 socket with a Pi 3B,
3B+, 3A+ or Zero 2 W. **Untested on hardware**: this is the build that tests it. Everything in
it ran in the testbed, where MAME's Macintosh SE stands in for the logic board.

- **The classic PiStorm's GPIO protocol** as the transport (`mac.bridge=gpio`, the default
  outside the testbed): the CPLD driven with Emu68's own bus-cycle sequences, every wait
  bounded, so a Pi with no SE behind it still reaches its HDMI screen and its log.
- **The Macintosh SE board layer** (`mac.board=se`, in the shipped `cmdline.txt`): the IIci ROM's
  VIA, SCC, 5380 and IWM reached where the SE has them; the RBV, ASC and VDAC answered by the
  kernel; the SE's screen fed from the Pi card's frame buffer. In the testbed it boots System
  7.1 from the SE's SCSI disk to the Finder on the SE's own 512 by 342 screen, with PiDisk
  mounted beside it (`docs/img/system71-finder-on-macse.png`).
- **A Pi 3B's WiFi chip** (BCM43438) has its own path in the WiFi module now, next to the
  3B+/3A+/4's BCM43455; the firmware files for each are in the read-me, with the download
  commands.
- **The container builder patches a stock IIci ROM** (0.1 and 0.2 packed it as given and left
  System 7.1 to an occasional zero divide at boot), with a precomputed byte delta
  (`patches/iici-368CADFE.delta.json`), so plain Python is enough; the first attempt wanted the
  m68k assembler, which a Mac does not have. `rompatch.py` and `patches/iici.json` remain the
  delta's source. The kernel applies the same delta in memory when the container holds a stock
  ROM, so nothing on the card has to be patched by hand at all.
- `config.txt` carries `dtoverlay=disable-bt` so the kernel's log is on the header's GPIO 14/15;
  Bluetooth stays off in this build (`bt=off`).
- **The kernel's log on the HDMI screen** (`mac.console=hdmi`, in the shipped `cmdline.txt`): the
  same text as the serial line, drawn with Emu68's font into the VideoCore buffer, so a bench
  without a serial cable still has a log. Without the option the log shows until the Macintosh's
  video driver turns the second screen on.

### Known limits of 0.3
- Nothing has met a real CPLD or a real SE yet; the SE's timing may differ from MAME's.
- On the SE board: keyboard and mouse through the SE's own ADB are wired but untried (the
  testbed had no ADB input); the floppy (IWM) and sound are untried; the SE's speaker is silent
  (the ASC is a stand-in). Keyboards and mice through the Pi's USB need a driver the Pi 3
  does not have yet (DWC2).
- The Pi's HDMI shows the same 512 by 342 picture as the SE's screen; two different screens
  on that board come later.
- Bluetooth and the serial log share the Pi 3's one PL011; the log won this build.

## 0.2 — colour, bigger screens, more memory (17 November 2026)

Still a testbed build (see 0.1 below for what that means); the same SD card layout. What changed:

- **Thousands and millions of colours** on the Pi's screen: the second display offers six modes
  (black and white, 4, 16, 256, thousands, millions) in the Monitors control panel. The direct
  modes and all larger screens live in 8 MB of Pi memory mapped into the slot, so the Macintosh
  pays nothing for them; the Pi's VideoCore scans out 32 bits.
- **Screen sizes**: the second screen can be 800 by 600, 1024 by 768, 1280 by 720, 1280 by 1024
  or 1920 by 1080 as well as 640 by 480. Set `hdmi_mode` in `config.txt`, add
  `mac.screen=WxH` to `cmdline.txt` and build the container with the declaration ROM of that
  size (`aurora68-1024x768.declrom` and the others are in the zip). The `'scrn'` resource's
  rectangle must be the screen's size (docs/10).
- **Up to 128 MB of Macintosh memory**, the most the IIci ROM sizes on its own (two banks of
  64), and the default is now "what the Pi can spare, up to 128": a 512 MB Pi Zero 2 W and an
  8 GB Pi 4 both give the Macintosh 128 MB, a smaller board gives what is left after the
  kernel's 192 MB. `cmdline.txt` no longer pins `mac.ram=`. Memory sizes that fill bank A and
  part of bank B (72, 80, 96 MB) work too.
- **Two boot-time faults fixed** on the way: the JIT re-verifies code loaded into the second
  megabyte of RAM in 32-bit mode (it used to treat that megabyte as ROM, which it is only in
  24-bit mode), and self-looping `dbf` delay loops take real time again, which is what System
  7.1's speed probe divides by (the intermittent zero divide at boot that 0.1 listed).
- New options: `mac.screen=`, `mac.vcswap=` (red/blue order if a board's firmware disagrees),
  `mac.dbfslow=` (reads per turn of a delay loop; 1 by default, 0 for none).
- **Hardware target named**: a classic PiStorm (EPM240, DIP-64) with a Raspberry Pi 3 in a
  socketed Macintosh SE; `docs/12-hardware-bringup.md` is the plan. The kernel now also looks
  for the Pi 3B's WiFi and Bluetooth firmware files (BCM43438: `brcmfmac43430-sdio.*`,
  `BCM43430A1.hcd`), but the WiFi module's chip layer still knows only the BCM43455 of the
  3A+/3B+/4; the 43438's Cortex-M3 path is on the list before the first WiFi on a 3B.

Macintosh-side files are unchanged except the declaration ROMs; replace `kernel8.img`, rebuild
`aurora68.cpio` with the new `aurora68.declrom` (or the size you want) and keep your
`config.txt` and `cmdline.txt` (drop `mac.ram=64` from it to get the new default).

## 0.1 — the testbed build, packaged (17 November 2026)

Download: `Aurora68-0.1.zip` from the [v0.1 release](https://github.com/farsonic/Aurora68/releases).
Unzip it and copy everything in it to the root of the SD card's FAT partition, next to the
Raspberry Pi firmware; `READ-ME-FIRST.txt` in the zip walks through it, including the one step
that cannot be done for you: making `aurora68.cpio` from your own Macintosh IIci ROM with
`make-container.py`.

**Read this first.** 0.1 is the state of the project at the end of its second day, packaged so
that the SD card layout, the container format and the Macintosh-side files are fixed before the
hardware work. Everything below was verified in the testbed (Emu68 under QEMU, the Macintosh
IIci motherboard under MAME with a bridged CPU). **It does not yet run on a real PiStorm**: the
kernel's transport to the motherboard is the testbed's bridge; the PiStorm GPIO transport is
the next piece of work. On hardware, this build starts the Pi, shows its boot splash on HDMI and
waits for a motherboard. 0.2 will be a drop-in `kernel8.img`.

### What works (in the testbed)
- **System 6.0.8 and System 7.1 boot** on the Macintosh IIci ROM with the Pi as a NuBus card in
  slot 9; the Slot Manager loads the Pi's drivers from the declaration ROM at boot.
- **PiDisk**: a disk image on the Pi mounts on the desktop.
- **Ethernet** (`.ENET` driver): ARP round trips; **MacTCP 2.0.6 pings** through it
  (`tools/mac/mactcpcfg.py` configures MacTCP without the control panel).
- **WiFi**: the same driver as a second network card; PiWiFi scans, joins with a name and
  password, disconnects, switches the radio; MacTCP pings over it. (In the testbed against a
  simulated radio; the chip is driven by the Alice68 module on hardware.)
- **Keyboard and mouse** through the Pi (`.PiInput`): USB and Bluetooth devices become Mac
  events, with the card's slot interrupt.
- **Macintosh SE by name**: PiMach makes Gestalt and SysEnvirons answer "Macintosh SE"; About
  This Macintosh shows it with the compact-Mac picture.
- **A second screen** on the Pi's HDMI: 640 by 480, 1 to 8 bits (256 colours), the Finder's
  desktop spanning both screens, Color QuickDraw drawing on it (PiColor). System 7 activates
  a second screen only with a `'scrn'` resource in the System file; `tools/mac/mkscrn.py` and
  `rsrcadd.py` make and install one, with the values PiScreens prints (docs/10).
- **32-bit addressing** and up to 64 MB of Macintosh memory (`mac.ram=`).
- Status (PiStatus), Bluetooth (PiBlue) and the test programs.

### Known limits
- No PiStorm GPIO transport yet (above). No SD-card disk images for PiDisk on hardware yet
  (the testbed serves host files); no real WiFi chip in the testbed; 128 MB of Macintosh
  memory bombs during the boot (64 works); the second screen has one resolution and no gamma;
  Open Transport, Fetch and friends are untested; a System 7.1 intermittent zero-divide at
  boot (a speed probe) is still being chased.
- The Macintosh-side programs are plain text windows with single-letter commands; the
  Alice68-style settings program is planned (docs/11).

### Installing on the Macintosh
The drivers need no installation: they are in the declaration ROM. From `Mac/`, put `PiMach`
and `PiDisk` in the System Folder (System 6) or in Extensions (System 7) if you want them, and
the applications anywhere. `Aurora68-Utilities.img` is an HFS image with all of them.

### Versions
Emu68 1.1 line (upstream commit 61a61f7) with the psmac variant; Alice68 modules of
16 November 2026; MAME 0.264 for the testbed; QEMU 8.2.2. Built with GCC 13.3 (aarch64),
binutils 2.42 (m68k), cmake 3.28, Python 3.13, hfsutils 3.2.6. `CLAUDE.md` in the repository
has the full build.
