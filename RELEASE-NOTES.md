# Aurora68 release notes

## Unreleased

- **Fixed: Speedometer, and anything busy with interrupts, freezing or bombing on the Pi alone**
  (`mac.board=solo`). Run ALL Tests ended in "bad F-Line instruction" or a freeze with the pointer
  still moving. An interrupt the Mac had already answered stayed raised for up to a millisecond,
  and System 7 took it again and again, each time deeper into the stack, until the stack ran over
  the program's memory. The kernel now lowers the interrupt the moment the Mac answers it, as the
  real chip does. Run ALL Tests finishes, with every extension of the test image loaded.
- **Speedometer's zeros are its own clock.** Speedometer times each test in sixtieths of a second.
  On the Pi many tests, the FPU ones above all, finish within one sixtieth, and Speedometer shows
  0.000 for them, or 2147483.65 where it divides by that time: read those as "faster than
  Speedometer can measure". More iterations do not help, since it divides the time by them in
  whole sixtieths. The FPU itself runs every 68882 instruction natively.
- **The WiFi messages say where the firmware goes**: in the container, with make-container.py (they
  said a WIFI folder on the SD card, which is the Amiga's way). A container with no WiFi firmware
  is said once at boot, with the three file names a Pi 3B needs, and PiLog no longer shows the
  WiFi's state line every five seconds, only when it changes.
- **1920 by 1200 in millions of colours.** The Pi's screen can now be a 1920 by 1200 monitor at
  32 bits: `hdmi_group=2` and `hdmi_mode=69` in config.txt, `mac.screen=1920x1200` in
  cmdline.txt, and the new `aurora68-1920x1200.declrom` in the container. Every screen size the
  kernel accepts, up to 1920 by 1200, now offers millions of colours (1080p only just did).
  Replace every declaration ROM along with `kernel8.img`: the frame buffer for thousands and
  millions of colours has moved, and an old declaration ROM with the new kernel (or the other way
  round) shows a grey screen in those modes.
- **Fixed: the network not working at bigger screen sizes on a Pi 3.** The declaration ROMs for
  the larger screens list the wired Ethernet card first, which a Pi 3 does not drive, and MacTCP
  and Riptide take the first card they find: the Mac sent its packets into a dead card. The kernel
  now leaves the wired card out of whatever declaration ROM it serves when the Pi has no working
  wired Ethernet (no GENET, which only the Pi 4 has, or `eth=off`), so the WiFi is the Mac's card
  at every size. The log says "the wired Ethernet card ($81) left out". Lagoon EtherTalk set to the
  wired card falls back to the first card it finds, now the WiFi.
- **WiFi-only declaration ROMs for every size**, `aurora68-wifi-WxH.declrom` (like
  `aurora68-wifi.declrom` at 640 by 480), for anyone who wants the file to say it. Put only one
  declaration ROM of a size in the container: the kernel takes whichever comes last.
- **PiDepth** is in the kit: renamed "PiDepth 32" and put in Startup Items, the Mac starts in
  millions of colours on a board whose parameter RAM forgets the Monitors setting.

## 0.10 — The Bluetooth menu, PC keyboards (18 November 2026)

- **The Bluetooth menu.** A Bluetooth icon beside the WiFi one in the menu bar. Its menu shows the
  keyboards and mice the Pi knows, searches for new ones and pairs them (showing the passkey to
  type when a keyboard asks for one), and forgets a device from its submenu after asking. The two
  menus are now one extension, `Wi-Fi & Bluetooth`: remove the old `WiFi Menu` from the Extensions
  folder when you add it.
- **The WiFi menu hangs from the menu bar** with no gap, and its password dialog follows Apple's
  1992 Human Interface Guidelines (OK outlined, a note icon, bullets for the password). The check
  box says "Remember this network".
- **PC keyboards: `mac.altcmd=1`** in cmdline.txt swaps Alt and the Windows key, so the key beside
  the space bar is Command, as on a Mac keyboard.
- **The Mac answers on the network by name** (Riptide: mDNS is on). `ping macintosh.local` from
  another computer reaches it; the name is the one in the Sharing Setup control panel.
- **The FPU is found.** Programs saw no FPU (Speedometer: "No FPU"); they now see a 68882 beside
  the 68030, and floating-point programs can use it.

## 0.9 — The WiFi menu, AppleTalk (18 November 2026)

- **The WiFi menu.** A WiFi icon at the right of the menu bar, as on today's Macs: it shows the
  signal, and its menu lists the networks around, joins one (asking for its password), switches
  the WiFi off and on. "Remember this network" saves the network in cmdline.txt on the card, so the
  Pi joins it at every power-on. It is the `WiFi Menu` extension in the kit's disk image: drop it
  on the System Folder and restart.
- **AppleTalk on the Pi alone.** Switching AppleTalk on in the Chooser no longer stops the next
  start at "Welcome to Macintosh", and the Network control panel opens. `mac.appletalk=1` in
  cmdline.txt starts with AppleTalk on.

## 0.8 — Restart, sound, and the CPU (18 November 2026)

- **Restart from the Special menu works.** It ended in a Sad Mac (0000000F 00000033) when the
  mouse or keyboard was used during the restart.
- **Programs that play sound no longer freeze the Mac** (the end of Speedometer's tests,
  Lemmings): the stand-in sound chip now fills and empties at the real chip's rate.
- **The Mac reports a 68030** (it said 68020): About This Macintosh, Gestalt and Speedometer.
- **USB keyboards and mice plugged in at power-on** are found without unplugging them: the Pi
  tries again for half a minute.
- **Microsoft Word** starts (4.0 stopped with "bad F-Line instruction"; 5.1 showed the spinning
  watch): the emulator now handles programs that patch their own instructions.
- **Lemmings** plays, with its music.

## 0.7 — USB keyboards and mice (18 November 2026)

- **USB keyboards and mice on the Pi 3.** Plug a USB keyboard and mouse into the Pi's USB ports and
  they work on the Mac, as Bluetooth ones do: the cursor, the buttons (right is Command-click,
  middle Option-click), typing, Command-key shortcuts. The Pi takes the first keyboard and the
  first mouse it finds and leaves every other USB device alone. Unplugging and plugging back in
  works (within two seconds). The kernel log shows `[USB] start rc=0: 1 keyboard, 1 mouse`.
  Tested in the emulator; on a real Pi 3, if the devices do not appear, try `usb=pio` in
  cmdline.txt and send the `[USB]` lines from the log. `usb=off` turns USB off.

## 0.6 — PiLog, and the modifier keys (18 November 2026)

- **PiLog**, the Pi's log on the Mac: a window that moves, resizes, zooms and scrolls, follows
  new lines, and filters by part (Kernel, Mac, WiFi, Bluetooth, Network, Card; Option-click a box
  for that part alone, All for everything). Text can be selected and copied. It starts at boot from
  the image's Startup Items (drag it out of `System Folder:Startup Items` to stop that) and sleeps
  between looks at the log. The kernel keeps the last 64 KB of its log in the card window from
  boot, so PiLog shows the boot too.
- **Modifier keys from USB and Bluetooth keyboards.** `.PiInput` set the wrong KeyMap bits, so
  Command, Shift, Option and Control never reached the Mac's events: Command-key shortcuts and
  Shift-click did nothing, and programs reading the keyboard with GetKeys (games) saw the
  wrong keys. Fixed; this is in the declaration ROM (`aurora68-wifi.declrom`).
- For developers: `mac/c`, a kit for writing Mac programs in C with the Debian m68k cross
  compiler (`mac/c/README.md`); `mac.inputclick=` for scripted clicks in the testbed.

## 0.5 — Bluetooth mice and keyboards on the Pi 3 (17 November 2026, 23:00)

- **Bluetooth on the Pi 3 family.** The Alice68 Bluetooth host (Classic and LE keyboards and mice,
  from the Pi 4 work) now runs on a Pi 3B, 3B+, 3A+, Zero W and Zero 2 W: BT_ON by board type
  (the firmware's GPIO expander, or GPIO 45/42 on the Zeros), the BCM2837's pull registers,
  BCM43430A1.hcd for the 3B's chip. A Pi 3 has one PL011 and Bluetooth needs it, so the kernel
  moves its log to the mini UART on the same header pins when Bluetooth starts; config.txt no
  longer carries dtoverlay=disable-bt.
- **Pairing without a pointer.** `bt.pair=1` pairs with the first keyboard or mouse a scan finds
  in pairing mode (`bt.pair=NAME` for one by name), and the log prints the `bt.dev=` word that
  makes the pairing permanent. The shipped cmdline.txt has it on.
- **The card writer** (psmac_card.c): the kernel drives the Pi 3's SD card through the sdhost
  controller (the Arasan stays the WiFi's) and keeps two things across a power cycle:
  Bluetooth pairings in `pairings.txt` (created by the kernel), and the disk image's changes,
  written back into `aurora68.cpio` at the image's own offset a second after the Mac stops
  writing. The container is written only after its size and first sector match the copy in
  memory; cmdline.txt and config.txt are never touched. `mac.card=ro` / `mac.card=off`.
  Tested in QEMU (a FAT32 card image: the written-back container parses, its changes lie only
  inside the image, fsck.vfat finds nothing, and the next boot reaches the Finder from it);
  not yet on the Pi.
- Also: a container larger than 8 MB is kept below the kernel's block at the top of RAM (the
  firmware's followkernel placement corrupted a 100 MB one); config.txt loads it at a fixed
  address. Bluetooth on the 3B/3B+/3A+ without CTS/RTS flow control (their chips have none).
- The Pi 3B's Bluetooth path ran on the hardware: an LE mouse paired, reconnected and drove
  the Mac's input (18 November).

## 0.4 — a Pi that boots System 7 on its own (17 November 2026, 12:30)

- **The solo board** (`mac.board=solo`): no Macintosh behind the Pi at all. The kernel answers the
  VIA with the clock/PRAM chip and the ADB transceiver behind it, the SCC, the IWM and an NCR
  5380 with a SCSI disk for each disk image in the container, next to the RBV, ASC and VDAC it
  already answered on the SE board. Put a System 7 disk image (`*.img`, `*.hda`, `*.image`) into
  the container with make-container.py, set `mac.board=solo` in cmdline.txt, and the Mac boots
  from it to the Finder on the HDMI screen. The image lives in RAM: what the Mac writes lasts
  until the power goes (a 1 GB Pi takes images up to a couple of hundred megabytes). No keyboard
  or mouse yet: the ADB bus is empty until the Pi has input devices to put on it. AppleTalk is
  off in the kernel's PRAM (LocalTalk would wait for SCC interrupts the model does not raise).
- On the other boards a disk image in the container becomes a PiDisk volume (RAM-backed).
- The testbed grew `tools/run-solo.sh`: QEMU alone, no MAME, screenshots through the monitor.

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
