# Aurora68 release notes

## Unreleased

- **Sound from the SE's own speaker.** What the Mac plays now comes out of the SE: the IIci ROM's
  startup chime (back at its full length, about a second), System 7's alert sounds and everything else
  the Sound Manager plays. The SE has no sound chip; the kernel writes the samples into the SE's own
  sound buffer, 370 a frame, in time with the SE's vertical blanking, sets the SE's volume from the
  Sound control panel, and switches the speaker off between sounds so it does not hum.
  `mac.sesound=off` leaves the speaker alone as before. Replace kernel8.img.
- **Parameter RAM and the clock kept on the card (SE board).** When the SE's clock chip does not
  answer (a leaked battery is the usual reason), the kernel answers in its place; what the Mac sets
  there (AppleTalk, the startup disk, Monitors, the sound volume, the time) used to be lost at
  power-off. It is now kept in `pram.bin` in the card's root, the file the solo board already uses,
  and read back at the next start. The clock goes on from the last save (the Pi has no clock of its
  own); set it in the Date & Time control panel and it is saved too. A clock chip that answers is
  used as it is. Replace kernel8.img.
- **Fixed: an SE with nothing on its SCSI bus waited for ever at the question-mark disk.** With no
  internal disk and nothing on the external port (the BlueSCSI taken off), the bus has no terminator,
  its lines float, and the Mac's SCSI Manager waited for ever for the bus to become free, so a disk image
  on the card never started the Mac. The kernel now looks at the bus before the Mac starts and, when it
  floats, answers the SCSI chip as an empty bus: the scan ends at once and the image starts the Mac. The
  log says which (`[PSMAC:SE] SCSI: ...`); `mac.sescsi=real` uses the chip all the same,
  `mac.sescsi=empty` never. With the BlueSCSI attached nothing changes, and `mac.boot=image` starts from
  the image with the BlueSCSI beside it. Replace kernel8.img.
- **A disk image on the card as the SE's startup disk.** With no SCSI disk holding a System (or none
  at all), the SE now starts from a disk image on the card (`mac.image=NAME`), which until now only
  mounted beside a startup disk. The declaration ROM's PiDisk entry has a boot record, the code the
  IIci ROM runs to start from a NuBus card, and the kernel points the Mac's default startup device at
  it for as long as the start needs. `mac.boot=auto` (the default) tries the SCSI disk the parameter
  RAM names first (ID 0 unless Startup Disk chose another), then the image; `mac.boot=image` the image
  first; `mac.boot=scsi` leaves it to the parameter RAM. Works on the solo board too. The image must
  be a startup disk (a System Folder with its boot blocks; a partitioned `.hda` is fine). Replace
  kernel8.img and rebuild the container with the new declaration ROM.
- **Address lines tested at every start (SE board).** The kernel writes and reads the SE's memory
  with each address line alone and each pair of lines before the Mac starts, and names a stuck,
  missing or bridged line with the 68000 socket's pins: the stripes over the top hundred rows of the
  user's SE were A14 and A15 bridged by solder at the PiStorm's address latch.
- **Fixed: "Parity has been disabled ..." at startup on the SE.** One bit of the VIA, the floppy drive
  select on an SE, could make the IIci ROM take the SE for an IIci with parity. Also: after every
  RESET (each Restart) the SE's RAM is put back at address 0, and the trap-table debugging watch,
  which put forty lines on the HDMI log at every start and a trap on every low-memory write, is off
  unless `mac.watchtraps=1`.
- **Two screens on the SE: its own and HDMI.** On `mac.board=se` the Pi's HDMI output can now be a
  second screen beside the SE's 512 x 342, any size up to 1920 x 1200 and any depth up to millions of
  colours (with 32-bit addressing), the SE's screen the main one: move windows across, set the HDMI
  screen's depth in Monitors. The Mac sees two video cards, slot 9 (the SE's screen) and slot $A
  (HDMI), from one declaration ROM file per HDMI size, `aurora68-se-1024x768.declrom` and so on. Put
  it in the container, set the HDMI mode in config.txt, take `mac.console=hdmi` out of cmdline.txt
  and add `mac.screen=WxH`, and copy the new extension **PiScrn** onto the System Folder: at the
  first start it arranges the two screens and restarts the Mac once (READ-ME-FIRST.txt, docs/12).
  `mac.hdmi=mirror` gives back the SE's picture on HDMI, `mac.hdmi=log` (or `mac.console=hdmi`, as
  in the release's cmdline.txt) the log, at the HDMI size now. Also: USB and Bluetooth mice and the
  remote control reach a second screen (they stopped at the first one's edge, on every board);
  PiColor opens on the second screen wherever it is; with `mac.console=hdmi` the SE's own screen
  no longer freezes; the remote control photographs either screen (`?display=1`). Replace
  kernel8.img, the declaration ROM, PiColor; add PiScrn.
- **The log says when the Pi's power is short.** In the SE the Pi is powered through the 68000
  socket, and a tired analog board or a Pi 3B's appetite can pull it below the firmware's
  threshold (about 4.63 V). The kernel now asks the Pi's firmware every two seconds and logs each
  change: `power: under-voltage now`, `ARM frequency capped`, `throttled`, `soft temperature
  limit`, and `power: normal again` with what has happened since boot. The first under-voltage is
  also a `[BOOT]` line, and the remote control's `/state` has the bits under `power`. On by
  default; `mac.power=off` stops it. Replace kernel8.img.
- **A Pi that fits the SE: the 3A+ and the Zero 2 W.** The classic PiStorm board is laid out for a
  3A+; a 3A+ or a Zero 2 W fits it, a 3B or 3B+ overhangs it, the original Zero W cannot run this.
  READ-ME-FIRST's new "Which Raspberry Pi" table says what each board has. Fixed for them:
  - a 3A+ was taken for a 3B+ (its firmware passes the 3B+'s device tree), kept the wired card and
    looked for an Ethernet chip it does not have; the kernel now reads the board's revision code
    and logs "[PSMAC] board: ...". The wired card is left out on both, so the WiFi is the Mac's
    network card; `eth=usb` takes a USB Ethernet adapter instead;
  - a 512 MB Pi gave the Mac 72 MB, and 8 MB with a 96 MB disk image in the container: it now gets
    128 MB either way, with room for a disk image on the card of up to about 99 MB (the log says
    how much, and says so in a MEMORY line when something does not fit);
  - the one-card container (every board's firmware, as get-firmware.sh prints it) brought no WiFi
    firmware to a 3A+, 3B+ or Pi 4: the kernel kept only the first eight files;
  - the Zero 2 W's later chip, the BCM43436s, gets its own firmware (get-firmware.sh fetches it);
  - a low-speed USB keyboard or mouse plugged straight into the one port (no hub) is now spoken
    to at low speed (not testable in QEMU), and a hub pulled out of that port is noticed;
  - on the SE board the CPLD's start ran twice at once from two cores; once now. The 200 MHz
    CPLD clock is checked against the Pi's PLL and logged ("CPLD clock on GPIO 4: ...").
  Tested in QEMU's 3A+ model (512 MB, USB keyboard and mouse behind a hub); the Zero 2 W by reading
  only. Replace kernel8.img, config.txt (a `[pi02]` section) and get-firmware.sh, and run
  get-firmware.sh again for the two new Zero 2 W files.
- **Booting 15 seconds faster on a Pi 3 with Bluetooth.** The kernel waited at every character of
  its log for a serial port that does not send (the Pi 3's PL011 belongs to Bluetooth), and that
  was most of the time the firmware's rainbow screen stayed up: the 68k started at 19 s. It now
  stops waiting after the first try; expect the 68k about half a second after the kernel starts.
- **PiLog saves the log as text, and the kernel keeps it on the card.** Instead of photographing
  the window: File > Save Log (Command-S) writes the lines shown, with the parts and the filter, to
  a TeachText document named "PiLog <date> <time>" (Option for the whole log; TeachText opens up
  to 32 KB, so read a whole log on another computer, copied there through Lagoon). Save to Card
  (the button, or File > Save Log to SD Card) writes the whole log to aurora68.log in the SD card's
  root, overwritten each time, to read on another computer; the kernel also does it by itself
  right after a crash record, so the log of a crash survives switching off. Passwords never show:
  wifi.psk and any option named psk, pass, key or secret, the password in mac.wifisim and the key
  in bt.dev become *** in PiLog, in saved files and in aurora68.log, and in the kernel's own lines
  that repeat cmdline.txt (on the serial line and the HDMI screen). Replace kernel8.img and PiLog.
- **Fixed: the Pi stopping for good with two disk images on the card** (`mac.image=A,B`, both
  files on the card): the log ended with "the Mac waits for block 0 from the card". One image was
  never affected. A block the Mac waits for still comes before anything else is loaded.
- **An option given twice in cmdline.txt is reported**: the log names the option, the value used
  (the first) and the ones ignored. Passwords are not shown.
- **Fixed: "Error type 41" (the Finder could not be loaded) booting from a disk image on the card.**
  When the Pi was busy elsewhere (joining the WiFi) the Mac could be given blocks of the image that
  had not yet come in from the card. Every read now waits for its blocks, and the Mac's own core
  reads them from the card itself when the rest of the Pi is busy. An image inside the container
  was never affected.
- **More than one disk on the Pi alone (`mac.board=solo`).** A second disk image, in the container or
  as a file on the card beside the first, never appeared on the desktop: as a SCSI disk it needed
  its own driver to mount it, and the Iomega driver on most images mounts only the startup disk.
  The first image is now SCSI ID 0, the startup disk, and every further one a PiDisk volume, which
  mounts itself a few seconds after the Finder starts; an Apple-partitioned image is served as its
  HFS partition. Several images on the card work too (every image file in the card's root, or
  `mac.image=A.hda,B.img`). `mac.disks=scsi` brings back the old way. **Fixed:** the first write to
  a PiDisk volume held in memory (an image in the container or on the card) stopped the Pi's service
  core, and with it the disk, the network and the screen's VBL. Replace the declaration ROMs: PiDisk
  now waits long enough for a block still coming in from the card.
- **A crash record in the log.** When the Mac bombs ("Sorry, a system error occurred"), the Pi's
  log now holds what the processor was doing: the program, where in memory it was, its registers,
  the instructions and the stack. After pressing Restart, open PiLog and filter for `CRASH`, or
  copy those lines from the serial log, and send them with a report.
- **Fixed: the Finder's "bad F-Line instruction" a few minutes after opening a window**, seen with
  Lagoon installed on the Pi alone (`mac.board=solo`). It was the interrupt fault fixed for
  Speedometer: with the fix, the same test runs clean.
- **PiLog: Clear, a text filter and Follow.** A second row of controls under the part check boxes.
  *Filter:* shows only the lines that contain the text, in any case, together with the check boxes;
  several words must all be there and `-word` hides the lines with it. Typing anywhere in the window
  goes to the field, Esc or its small close box empties it, and the count beside it says how many
  lines show ("5 of 688 lines"). *Clear* (Edit > Clear Log, Command-K) empties the window but
  not the kernel's log, which the serial copy and other readers still have; Edit > Show Earlier
  Lines brings the earlier lines back. *Follow* (View > Follow New Lines, Command-T) keeps the view at
  the newest line; scrolling up unchecks it and scrolling back to the bottom, or End, checks it
  again. Copy keeps working, Paste goes into the filter, and the window still fits the SE's screen.
- **Parameter RAM kept on the card (the Pi alone, `mac.board=solo`).** AppleTalk active in the
  Chooser, the Network control panel's connection (Lagoon EtherTalk), Monitors' depth, the startup
  disk and the sound used to go back to the defaults at every power-on. The kernel now keeps them
  in `pram.bin` on the SD card, as a real Mac keeps them in a chip with a battery: written a
  second after the Mac changes them, once the Finder is up, and read before the Mac's first look at
  the next power-on; the clock goes on from where the last save left it. `mac.appletalk=1` and
  `mac.xpram=` still apply on top; `mac.card=ro` or `off` stop the writing; delete `pram.bin` for
  the defaults.
- **Both network cards on every board that has two: the Pi 3's wired port is driven.** The Pi 3B's
  RJ45 socket is a LAN9514 on the Pi's own USB bus and the 3B+'s a LAN7800 behind two USB hubs;
  the kernel now drives both (beside the USB keyboard and mouse, on the same bus), as the wired
  card of the slot-9 card, so the Mac has its wired card ($81) and its WiFi ($83) side by side
  on a Pi 3B, a 3B+ and a Pi 4. Only a board with no wired port (3A+, Zero 2 W) or `eth=off`
  loses the wired card. Riptide uses both, en0 and en1, with DHCP on each and the default route
  through the wired one while its link is up, moving to the WiFi when the cable is pulled; MacTCP
  uses the first card, the wired one; the Network control panel shows one Lagoon EtherTalk icon
  per card. **Not yet tested on a real Pi 3B or 3B+**: the USB network driver was proved in the
  emulator against QEMU's USB network device, not against the LAN9514 or LAN7800 themselves. The
  log lines to send: `[ETH] LAN9514 found at USB address 2`, `[ETH] link up, ...`, and the
  `[ETH]   LAN9514: running ...` line every 15 seconds. If the chip is not found, the Mac's wired
  card refuses to open and the WiFi carries on; `eth=off` hides the card as before.
- **Two drivers, one interrupt.** With both network cards open the wired driver used to take the
  WiFi's interrupts, and the WiFi's frames waited a quarter of a second for its timer: replace
  every declaration ROM along with `kernel8.img`.
- **One SD card for a Pi 3 and a Pi 4.** `get-firmware.sh` (new in the zip) fetches the Pi's boot
  files for every board and every board's WiFi and Bluetooth firmware, checks each against its
  SHA-256, and prints the `make-container.py` line; the kernel takes the WiFi firmware by the
  chip's id and the Bluetooth firmware by the board, so one container serves a 3B, a 3B+ and a 4.
  Bluetooth pairings do not move between boards; the card writer is the Pi 3's only (on a Pi 4
  nothing is written back).
- **Where a ping's time goes.** Every ICMP echo through either card is timed in the kernel log
  (`[NET] echo wifi seq N: took->out ..., out->back ..., back->mac ...`), and the heartbeat says
  which part of the service loop took longest. The big screen's picture is now converted a band at
  a time, a millisecond or so per turn of the loop, instead of 20 ms in one go at 1920 by 1200,
  which held up the WiFi, the disk and the network board's interrupt (the 80 ms pings to the
  router on the user's Pi 3B at 1920 by 1200). The WiFi's power saving is now checked after every
  join and switched off if the firmware took it up (`[WIFI] power save: PM ...` in the log): with it
  on, the access point holds each reply until the next beacon. With Riptide's own fix (a waiting
  program is woken at once, not at the next tick) the wired port's 20 ms should drop to a few.

- **Booting faster.** From power-on to the Mac's first instruction, and from there to the Finder,
  is now much shorter (docs/08 item 50 has the timeline):
  - A disk image can sit on the card as a file of its own beside `aurora68.cpio` (`MyMac.hda`, or
    `mac.image=NAME`) instead of inside it. The Pi firmware reads the whole container before the
    kernel even starts, five to ten seconds for 100 MB on a Pi 3; the kernel now reads the image
    in the background while the Mac boots, the blocks the Mac asks for first, and writes the Mac's
    changes back into the same file. On the bench it is the startup disk, in the SE the PiDisk
    volume. An image inside the container still works and wins if there is one.
  - The IIci ROM patch skips the memory test (the Pi's memory is not a SIMM), cuts the startup
    chime nobody hears to nothing, and waits 10 ms instead of 250 at every empty SCSI ID (twice
    per boot, six IDs). Make the container again, or let the kernel patch a stock ROM.
  - The kernel's log no longer waits for the 115200 baud serial line (`mac.synclog=1` brings the
    old behaviour back), the USB start runs on its own core, and the ROM's first RESET no longer
    remakes the HDMI frame buffer or holds the SE's reset line for 40 ms.
  - `kernel8.img` ships gzip-compressed (1 MB instead of 5) and `config.txt` has `boot_delay=0`.
  - Every step is timed in the log, `[BOOT] t=N.NNNs what`, counted from power-on; the first line is
    the firmware's part. Please send those lines from a real Pi 3B.
  - PiDisk's volume appears a few seconds after the Finder instead of a minute after power-on.
- **Fixed: no screen when the declaration ROM's size did not match `mac.screen`.** A container whose
  only declaration ROM was for another size (aurora68-1920x1200.declrom with mac.screen=1920x1080)
  left slot 9 empty: no Pi screen, no PiDisk, no input, the log on HDMI for ever. The kernel now
  takes the plain declaration ROM, else the sized one, and gives the screen that ROM's size, saying
  so in the log; a container with no declaration ROM at all is reported at every heartbeat.
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
