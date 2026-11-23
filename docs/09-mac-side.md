# Mac-side software: drivers, extension, test application, declaration ROM

Everything the Macintosh runs to talk to the Pi lives in `mac/` (68k assembly, GNU `as` MIT
syntax, `binutils-m68k-linux-gnu`) and is packaged by small Python tools in `tools/mac/`
(no Apple tools needed). `make -C mac` builds all of it into `mac/build/`.

The programs and extensions have been called "Aurora68 Xxxx" since 21 November 2026 (below, "The programs
and their folders"); the table gives each one's name before that once, the sources keep the old file names.

| Piece | Source | Output | What it is |
|---|---|---|---|
| `.PiDisk` | `mac/pidisk.s` | `pidisk.bin.raw` (DRVR image) | Block device driver over the mailbox BLOCK service: Open adds a drive queue entry per Pi block device and posts a disk-inserted event, Prime reads/writes 512-byte blocks synchronously, Control answers KillIO/verify/format/eject/icon/drive-info, Status answers drive status. Since 20 November 2026 also removable units (USB sticks): a VBL task adds a drive and posts a disk-inserted event when a disk is put in, marks the drive empty when it is taken out (Prime then answers offLinErr), and eject sends `PSMB_BLK_EJECT` (below, "USB flash drives"). |
| Aurora68 Disk extension (until 21 November 2026 PiDisk) | `mac/pidisk_init.s` + the DRVR | `Aurora68-Disk.bin` (MacBinary, type `INIT`) | System 6/7 extension: installs the DRVR from its own resource fork (`_DrvrInstall`), opens it, mounts every drive it added (`_MountVol`). Used until the declaration ROM path loads the driver from ROM. |
| Aurora68 Machine extension (until 21 November 2026 PiMach) | `mac/pimach.s` | `Aurora68-Machine.bin` (MacBinary, type `INIT`) | Tells the Mac it is a Macintosh SE. The IIci ROM's Gestalt Manager answers the `'mach'` selector with 11 (gestaltMacIIci), so About This Macintosh, the Finder and every program that asks see a IIci. This INIT copies a four-instruction selector function into the system heap and installs it with `_ReplaceGestalt` ($A5AD) for `'mach'`; it answers 5, gestaltMacSE. Nothing else changes: the ROM's own view of its hardware is left alone (changing the ROM's box table would change its decisions about the IIci hardware it drives), the processor still reads 68030. Goes in the System Folder (System 6) or Extensions (System 7); `docs/img/system71-about-se.png`. |
| `.Network_EtherNet_PiStorm` | `mac/enet.s` | `enet.bin.raw` | Apple `.ENET`-interface Ethernet driver over the NET service: EAttachPH/EDetachPH, ERead (default handler: queued parameter blocks completed from a VBL task), EWrite (WDS gather, source address inserted, 60-byte padding), EGetInfo (address + 3 counters), EAddMulti/EDelMulti, ESetGeneral, ERdCancel; real protocol handlers are called with the A3/A4/D1 convention and ReadPacket/ReadRest. Receives from the slot-9 interrupt (`SIntInstall`, docs/07) with the VBL task as a safety net. |
| `.PiInput` | `mac/piinput.s` | `piinput.bin.raw` | Keyboards and mice attached to the Pi (USB on a Pi 3, Bluetooth) become Macintosh input. A VBL task drains the input ring the service core fills (`alice_psmac.h`), translates USB HID usages into Apple virtual key codes and, through `_KeyTranslate` and the Script Manager's current KCHR, into characters, keeps `KeyMap` and the modifier bits current, moves the cursor through `RawMouse`/`MTemp`/`CrsrNew`, and posts keyDown/keyUp/autoKey and mouseDown/mouseUp. Right and middle buttons become Command- and Option-clicks. Runs from the slot-9 interrupt (`SIntInstall`) with the VBL task as a safety net. Opened at start from the declaration ROM. Since 23 November 2026 it also puts a cursor task in front of the ROM's (`JCrsrTask`, $8EE) for the edge jump: a pointer pushed against the side of a screen where another screen lies goes onto it at the same proportion along the edge, for every mouse, the SE's own included (docs/10, "The edge jump"; `mac.edgejump=off`). |
| `.Network_WiFi_PiStorm` | `mac/enet.s` assembled with `WIFI` | `enet_wifi.bin.raw` | The same driver for unit 0 of the network board, the WiFi chip, as a second network sResource (0x83) in slot 9: MacTCP and EtherTalk list two Ethernet cards and the user picks. In the testbed the unit is a simulated radio when `mac.wifisim=` is given (below); Aurora68 Wi-Fi joins a network on hardware. |
| `.Display_Video_PiStorm` | `mac/pivideo.s` | `pivideo.bin.raw` | The second display (docs/10): Apple's video driver interface over the display block in the slot window (psmb.h). Control: cscReset, cscSetMode (modes 128-133: 1/2/4/8 bits indexed, 16/32 bits direct, at the screen size the Pi publishes), cscSetEntries and cscDirectSetEntries (the colour table), cscGrayPage, cscSetGray, cscSetInterrupt; Status: cscGetMode, cscGetEntries, cscGetPageCnt, cscGetPageBase, cscGetGray, cscGetInterrupt, cscGetDefaultMode. The frame buffer is the window's top 512 KiB ($980000 in 24-bit mode, $F9080000 in 32-bit); the Pi converts it to the VideoCore frame buffer sixty times a second and raises the slot interrupt as the card's VBL, which the driver's handler turns into `_DoVBLTask` for slot 9. Opened at boot by the Start Manager like any video card; `tools/vcshot.py` photographs the Pi's screen in the testbed. |
| PiDisk boot record | `mac/pidisk_boot.s` | `pidisk_boot.bin.raw` | The sBootRecord (ID 6) of PiDisk's sResource, an sExec block the Slot Manager copies into the heap and runs: opens `.PiDisk` with OpenSlot (fMulti, an SEBlock naming slot 9 and sResource $80, as the ROM opens a slot driver itself), and when the Start Manager is looking for a startup disk (seBootState 0) asks the Pi which disk goes first (`PSMB_BLK_BOOT`) and returns that driver's reference number in seRefNum: how a disk image on the card becomes the startup disk (docs/08 item 67, `mac.boot=`). Logs `PiDisk boot: ...`. |
| Declaration ROM | `tools/mac/mkdeclrom.py` | `psmac.declrom` | Slot $9 card: board sResource (BoardId 0x5053, vendor info), `PiDisk` functional sResource with the DRVR in its sDriverDir and the boot record above (the ROM opens it once the System file is open, through the boot record), `Network_EtherNet_PiStorm` sResource (catNetwork/typeEtherNet, not opened at start: the `.ENET` shell opens it through OpenSlot when MacTCP or EtherTalk ask), `PiInput` sResource (opened at start). Served by Emu68 at 0xF9FF0000 (`mac.declrom=` in the testbed). |
| Aurora68 Agent extension (21 November 2026) | `mac/piagent/agent.c`, `entry.s` | `Aurora68-Agent.bin` (MacBinary, type `INIT`, creator `PIAG`) | Files and programs for the Pi's remote control (docs/13, "Files and applications"): lists folders, reads and writes files a 24 KiB chunk at a time through the card window, makes, deletes and renames, launches applications and opens documents, lists and quits programs, all from the GetNextEvent filter at application time. In C (the `mac/c` kit, `elf2init.py`), System 6.0.8 and 7, 24- and 32-bit; a 'sysz' resource asks System 6 for 64 KB of system heap. Does nothing until the remote asks. Goes in the System Folder (System 6) or Extensions (System 7). |
| Aurora68 Wi-Fi (until 21 November 2026 PiWiFi) | `mac/piwifi.s` | `Aurora68-WiFi.bin` (application) | The Pi's WiFi from the Mac: shows unit 0 of the network board (state, the driver's message, network, signal, channel, address), scans and lists networks (name, signal, channel, security), joins a network (name and password typed on the status line, sent as a `struct an_join` through the command box), disconnects, switches the radio on or off. Single-letter commands in a text window, so it runs under System 6 without resources; the Macintosh counterpart of Alice68-Card. |
| Aurora68 Bluetooth (until 21 November 2026 PiBlue) | `mac/piblue.s` | `Aurora68-Bluetooth.bin` (application) | The Pi's Bluetooth from the Mac: the host's state and message, its address, the paired devices (name, address, keyboard/mouse/device, connected), the devices found by the last scan (name, address, signal), the passkey while pairing; scan, pair with a found device, connect or forget a paired one, through the `struct an_bt` command box at board + 0x3800. |
| Aurora68 Status (until 21 November 2026 PiStatus) | `mac/pistatus/status.c`, `model.c` (until 23 November 2026 `mac/pistatus.s`) | `Aurora68-Status.bin` (application) | The Pi's status page (Alice68's `alice_status.h` at window + 0x50000) as gauges since 2.0 (23 November 2026, below, "Aurora68 Status 2.0"): dials for the SoC temperature, the ARM clock against its nominal one and the 68k's MIPS; the firmware's under-voltage, frequency-capped, throttled and soft-limit lights, now and since boot; strip charts of the temperature, clock and MIPS over the last minutes; each ARM core's load and what it runs; the Pi's memory, the network's throughput, the video and sound cards. A compact view of the temperature, clock, MIPS and one light. In C (`mac/c` kit). Version 1 (assembly, text) showed the build, uptime, cores, temperature, JIT and network counters (`docs/img/system71-pistatus.png`). |
| Aurora68 Ping (until 21 November 2026 PiPing) | `mac/piping.s` | `Aurora68-Ping.bin` (application) | MacTCP over the Pi: opens Apple's `.IPP` driver, shows the configured address and mask (ipctlGetAddr), sends three ICMP echo requests to the testbed peer (ipctlEchoICMP, completion routine) and prints the round trips in ticks. Needs MacTCP installed and pointed at the card. |
| Aurora68 Log (until 21 November 2026 PiLog) | `mac/pilog.c` | `Aurora68-Log.bin` (application) | The Pi kernel's log in a window that moves, resizes, zooms and scrolls (`docs/img/system71-pilog.png`). The kernel copies its log into a 64 KiB ring at window + 0x60000 (`psmb.h`, struct psmb_log) from boot; Aurora68 Log costs one memory read a quarter-second. Check boxes filter by part: Kernel, Mac, WiFi, Bluetooth, Network, Card; Option-click shows one part alone (`docs/img/system71-pilog-wifi.png`). Since 19 November a second row: **Filter:** shows only lines containing the text, in any case, together with the check boxes (several words must all match, `-word` hides lines with it; typing anywhere in the window goes to the field, the view is redone a fifth of a second after the last key; Esc or the field's small close box empties it) and says how many lines show, "5 of 688 lines" for WIFI in the testbed (`docs/img/system71-pilog-filter.png`); **Clear** (Edit > Clear Log, Command-K) empties the view without touching the kernel's ring, which other readers and the serial copy use: Aurora68 Log remembers where the ring stood and shows only what came after; Edit > Show Earlier Lines brings the rest back (`docs/img/system71-pilog-clear.png`); **Follow** (View > Follow New Lines, Command-T), checked at the start, keeps the view at the newest line; scrolling up unchecks it, scrolling back to the bottom or End checks it again (`docs/img/system71-pilog-follow.png`). Text can be selected and copied, and pasted into the filter. Since 20 November the log can be handed on as text instead of a photograph: **File > Save Log…** (⌘S) puts the lines shown (the parts, the filter, from Clear's mark, under a first line saying which) into a TeachText document (TEXT, 'ttxt') named "Aurora68 Log 2026-11-19 14.05"; with Option held (⌘⌥S) the whole ring. TeachText opens 32 KB at most, a whole ring is 64 KB: copy it to a modern machine (Lagoon's share) and read it there. **Save to Card** (the button at the right of the second row, or File > Save Log to SD Card) asks the kernel (`PSMB_INFO_SAVELOG`, progress through `PSMB_INFO_LOGSTATE`) to write the whole ring to `aurora68.log` in the SD card's root, overwritten each time; the kernel also does it by itself after a `[CRASH]` record (docs/08 item 55). Secrets never show: the values of `wifi.psk` and of any option named psk, pass, key or secret, the password in `mac.wifisim=` and the link key in `bt.dev=` become `***` in the window, the Scrap, a saved file and `aurora68.log` (the kernel echoes its bootargs) (`docs/img/system71-pilog-save.png`, `docs/img/system71-pilog-card.png`). Menus File, Edit (Undo to Clear in the standard order for desk accessories, then Select All, Filter Lines Command-F, Clear Log, Show Earlier Lines) and View; items that cannot be used are dimmed (`docs/img/system71-pilog-menu.png`). The 496-pixel window fits the SE's screen. In C, built with the kit in `mac/c` (`mac/c/README.md`); the image starts it from Startup Items. |
| Aurora68 Play (until 21 November 2026 PiPlay) | `mac/piplay/piplay.c` | `Aurora68-Play.bin` (application) | MP3, AIFF and WAV files on the sound card, decoded on the Pi (below, "Aurora68 Play"): File > Open… (Standard File, every file shown; the kind is found from the content, so a file from a PC with no type plays), Play/Pause and Stop, the title and artist from the ID3 tag, the time played and the length, a volume slider. Hands the file's bytes to the card's stream channel (`psmb.h` struct psmb_stream); the Pi's core 1 decodes with minimp3 and mixes with the Mac's own sound. In C, with the kit in `mac/c`. Since 21 November 2026. |
| Aurora68 Screens (until 21 November 2026 PiScreens) | `mac/piscreens.s` | `Aurora68-Screens.bin` (application) | QuickDraw's GDevice list: each screen's rectangle, flags, depth, frame buffer base, row bytes and driver, then MMU32Bit. Written for the second display's bring-up (docs/10): it is how the Pi's screen was found created but not activated. |
| Aurora68 Slots (until 21 November 2026 PiSlots) | `mac/pislots/pislots.c` | `Aurora68-Slots.bin` (application) | The NuBus cards the Slot Manager knows about, in plain words (System 7.1 has no Apple System Profiler): for the logic board's own declaration ROM (slot 0) and each slot $9-$E, whether a card is there and started, its name, board ID, maker, revision, part number and date as far as its declaration ROM gives them, and its start-up code's answer; then each function (sResource): id, name, kind in words (display, network, processor, disk, input...) with the four numbers, its driver, whether the driver is open and its reference number (from the unit table), and for a screen its size and depth. A resizable, zoomable window with two scroll bars that comes back where it was left; File > Save As… (a TeachText document), Edit > Copy, File > Look Again (⌘R), About Aurora68 Slots…. A summary also goes to the Pi's log (`[MAC] A68 Slots: slot 9 "Aurora68", ...`). On a Mac without a Slot Manager it says so. In C (`mac/c` kit), 68020, System 6.0.8 and 7; below, "Aurora68 Slots" (`docs/img/system71-pislots-se.png`). |
| Aurora68 Colour (until 21 November 2026 PiColor) | `mac/picolor.s` | `Aurora68-Colour.bin` (application) | Colour on the Pi's screen: a colour window placed on the second display paints twelve hues at full and half strength, the primaries and a sixteen-step grey ramp with RGBForeColor and PaintRect (`docs/img/system71-two-screens.png`, both screens side by side). |
| Aurora68 Network (until 21 November 2026 PiNet) | `mac/pinet.s` | `Aurora68-Network.bin` (application) | Test application: shows the mailbox header, PiDisk 0, finds the Ethernet card through `SGetTypeSRsrc`, opens `.ENET` with `OpenSlot`, prints the address from EGetInfo, sends an ARP request for 192.168.68.1 and waits for the reply through ERead. Pair with `tools/pinet.py` on the host. |
| `.A68Video` (22 November 2026) | `mac/a68video.s` | `a68video.bin.raw`, in the kernel's `psmac_video_declrom.h` | The driver of the Aurora68 Video Decoder card in slot $C, carried in the card's own declaration ROM and opened by the Slot Manager at start-up: frame requests for QuickTime's formats, frame decoding on or off, the MPEG stream's commands, bytes, place and volume (Control 100-114); the card's address, formats, stream state and counts (Status 100-103). `docs/video-decoder-api.md` is its reference |
| Aurora68 Video (23 November 2026) | `mac/a68player/video.c`, `glue.s` | `Aurora68-Video.bin` (application, creator `A68V`) | MPEG-1 films from the Mac's disks on the video decoder card (below, "Aurora68 Video"); its bundle opens 'MPEG' and 'MPG ' files from the Finder |
| Aurora68 Video Decoder extension (22 November 2026) | `mac/vdec/vdec.c`, `import.c`, `mpgindex.c`, `entry.s`, `traps.txt` | `Aurora68-VideoDecoder.bin` (MacBinary, type `thng`, creator `A68v`) | QuickTime decompressors for Cinepak, Apple Video, Animation and Graphics that hand each picture to the card through `.A68Video` (below, "The Aurora68 Video Decoder in QuickTime"); QuickTime's own decompressors take over when the card is off or absent. Since 23 November 2026 also MPEG-1: a movie import for 'MPEG' and 'MPG ' files and the decompressor 'mpg1' (below, "MPEG-1 films in MoviePlayer") |

More of them, each with a section below: **Aurora68 Display** (PiDisplay, `mac/pidisplay/`), **Aurora68 Depth**
(PiDepth, `mac/pidepth.c`), **Aurora68 Screen Setup** (the extension PiScrn, `mac/piscrn/`), **Aurora68 Sound** (the
extension Sound PiStorm, `mac/pisound.s`) and **Aurora68 Wi-Fi & Bluetooth** (the menus' extension, `mac/wifimenu/`,
"Wi-Fi & Bluetooth" before).

## The programs and their folders (21 November 2026)
The owner asked on 21 November 2026 that every program be called "Aurora68 Xxxx", that they go into folders
and that they get icons of their own (docs/08 item 84 is the record). Names in the Finder, the window titles,
the About boxes, the Apple menu's About item, Balloon Help, alerts and the log:

| Before | Since 21 November 2026 | File in `mac/build/` | Creator | In the log |
|---|---|---|---|---|
| PiPlay | Aurora68 Play | `Aurora68-Play.bin` | PIPY | `[MAC] A68 Play:` |
| PiDisplay | Aurora68 Display | `Aurora68-Display.bin` | PIDY | `[MAC] A68 Display:` |
| PiSlots | Aurora68 Slots | `Aurora68-Slots.bin` | PISL | `[MAC] A68 Slots:` |
| PiLog | Aurora68 Log | `Aurora68-Log.bin` | PILG | |
| PiStatus | Aurora68 Status | `Aurora68-Status.bin` | PIST | |
| PiNet | Aurora68 Network | `Aurora68-Network.bin` | PINT | `[MAC] A68 Network:` |
| PiWiFi | Aurora68 Wi-Fi | `Aurora68-WiFi.bin` | PIWF | `[MAC] A68 Wi-Fi:` |
| PiBlue | Aurora68 Bluetooth | `Aurora68-Bluetooth.bin` | PIBL | |
| PiPing | Aurora68 Ping | `Aurora68-Ping.bin` | PIPG | `[MAC] A68 Ping:` |
| PiColor | Aurora68 Colour | `Aurora68-Colour.bin` | PICO | |
| PiScreens | Aurora68 Screens | `Aurora68-Screens.bin` | PISC | |
| PiDepth 8 | Aurora68 Depth 8 | `Aurora68-Depth.bin` | PIDP | |
| PiDisk (extension) | Aurora68 Disk | `Aurora68-Disk.bin` | PSMC | `[MAC] A68 Disk:` |
| PiMach (extension) | Aurora68 Machine | `Aurora68-Machine.bin` | PSMC | `[MAC] A68 Machine:` |
| PiScrn (extension) | Aurora68 Screen Setup | `Aurora68-ScreenSetup.bin` | PISR | `[MAC] A68 Screen Setup:` |
| Sound PiStorm (extension) | Aurora68 Sound | `Aurora68-Sound.bin` | PSMC | `[MAC] A68 Sound:` |
| Wi-Fi & Bluetooth (extension) | Aurora68 Wi-Fi & Bluetooth | `Aurora68-WiFi-Bluetooth.bin` | PSWM | |
| (new, 21 November 2026) | Aurora68 Agent (extension) | `Aurora68-Agent.bin` | PIAG | `[MAC] A68 Agent:` |

What did not change: creator codes and file types (documents, preferences and the Finder's associations keep
working), the drivers' names (`.PiDisk`, `.ENET`, `.PiInput`, `.Display_Video_PiStorm`...), the declaration
ROM's sResource names, the mailbox, the sources' file names (`mac/piplay/`, `mac/pislots/pislots.c`...), the
drivers' own log lines (`[MAC] PiDisk: ...`, `[MAC] PiDisk boot: ...`, `[MAC] ENET: ...`) and the kernel's tags
(`[PSMAC]`, `[AUDIO]`...). The programs' log lines start `[MAC] A68 ` and the short name: "Aurora68" in every line
would take seven of the 127 characters a line can have. Aurora68 Log's filter finds them with `A68`.

**Preferences carry over.** "Aurora68 Play Prefs", "Aurora68 Slots Prefs" and "Aurora68 Display Prefs" are read
if they are there, else "PiPlay Prefs", "PiSlots Prefs", "PiDisplay Prefs" (same type 'pref', same contents);
the new name is written at Quit, the old file stays. The radio's list "PiPlay Stations", which the user edits,
is renamed "Aurora68 Play Stations" when Aurora68 Play first starts and finds no list of the new name, so that
there is one list (log: `radio: "PiPlay Stations" renamed "Aurora68 Play Stations"`). **Aurora68 Depth** reads
its depth from its own name as before, after the "68" of "Aurora68": "Aurora68 Depth 16", "Aurora68 Depth HDMI
32", and still "PiDepth 16" for a copy renamed in the old way.

**The folders.** On the release (its `Mac/` folder and the disk image Aurora68-Utilities.img) and on a disk the
helper installs (`docs/img/system71-aurora68-folders.png`: the Display, Network, System and Music folders in the
Finder on a colour screen; `docs/img/se-aurora68-folders.png`: the same on the SE's one-bit screen):

    Aurora68/
      Aurora68 Play/            Aurora68 Play, Music/
      Display/                  Aurora68 Display, Aurora68 Screens, Aurora68 Colour, Aurora68 Depth 8
      Network/                  Aurora68 Network, Aurora68 Wi-Fi, Aurora68 Bluetooth, Aurora68 Ping
      System/                   Aurora68 Slots, Aurora68 Log, Aurora68 Status
      Extensions (to install)/  Aurora68 Disk, Aurora68 Machine, Aurora68 Screen Setup, Aurora68 Sound,
                                Aurora68 Wi-Fi & Bluetooth, Aurora68 Agent, Read Me

`tools/mac/aurora68-layout.py` makes it: `stage DIR` as folders of MacBinary files (`tools/mkrelease.sh` uses it
for `Mac/`), `disk IMAGE` onto an HFS volume (the Utilities image), `install IMAGE [--part N]` onto a startup
disk, the extensions into the System Folder's Extensions folder and the Aurora68 folder at the top of the disk
(`make -C mac install IMG=...` runs it). `install` takes the old extensions out (two copies of an extension
would both run), `--remove-old` the old programs too (from the System Folder, Startup Items, the top of the disk,
Utilities and Apps; a folder named after one, like Apps:PiPlay with its Music, stays, only the program goes),
`--music FILE...` copies sound files into Aurora68 Play's Music folder with the type 'MPG3', 'AIFF' or 'WAVE'
and the creator 'PIPY', so that a double-click opens them in Aurora68 Play, `--startup-log` puts Aurora68 Log
into Startup Items, and `--rebuild-desktop` deletes the Finder's Desktop DB and Desktop DF so that the next
start builds them again with the new icons (holding Command-Option at startup does the same on a Mac). Every
hfsutils call runs with a HOME of its own made for the run, because hfsutils keeps its current volume in
`$HOME/.hcwd`, shared by all of a user's hfsutils processes.

**Extensions run in the order of their names**, and the new names moved them. "Sound PiStorm" sorted after
Apple's "Sound Manager" (Sound Manager 3.0, an extension on System 7.1), which it needs; "Aurora68 Sound" sorts
before it. So when Aurora68 Sound finds a Sound Manager older than 3.0 it copies itself into the system heap,
says `A68 Sound: no Sound Manager 3.0 yet; waiting for the Sound Manager extension, which starts after this
one`, and runs once more from the GetNextEvent filter (jGNEFilter) as soon as the Sound Manager says 3.0, which
is when the first program takes an event after the extensions: then the usual `A68 Sound: Sound Manager 3.0;
the sound card ... registered and chosen as the output device`. With no Sound Manager 3.0 after twenty minutes
it says it is too old, as before. The other extensions have no order among themselves or with Apple's that
matters; Aurora68 Screen Setup and Aurora68 Wi-Fi & Bluetooth now run before Riptide and the Sound Manager
instead of after them.

**The icons** (`tools/mac/mkicons.py`, standard library; `docs/img/aurora68-icons.png` is every one at one bit
and in colour, at 1x and 4x). One motif: a rounded tile with a one-pixel shadow and across its top a night sky
with an aurora, a wave of light whose rays hang down like a curtain, the sky's lower edge a soft wave; below,
on white, a glyph for the program: a pair of quavers (Play), a monitor with arrows (Display), three slots on a
board (Slots), a page of log lines (Log), a gauge (Status), a network plug (Network), arcs rising from a dot
(Wi-Fi), the Bluetooth rune in an oval (Bluetooth), a sonar screen with an echo (Ping), a colour wheel
(Colour), two screens (Screens), three layers stacked (Depth). Extensions are the same tile cut as a puzzle
piece: an SD card (Disk), a compact computer (Machine), two screens with a double arrow (Screen Setup), a
loudspeaker (Sound), the menu bar with the Wi-Fi and Bluetooth marks (Wi-Fi & Bluetooth). Documents are a page
with a turned corner, a narrow aurora and the glyph: Aurora68 Play's MP3 ("MP3" and a quaver), AIFF and WAV (a
waveform and the kind), and the preferences of Play, Slots and Display (sliders). Each family is complete:
ICN# (32 by 32 and its mask), ics# (16 by 16 and mask), icl4, icl8, ics4 and ics8. The one-bit icons were drawn
first, since the SE's screen shows nothing else; the colour ones are the same pixels with aurora greens, blues
and violets on a navy sky, in the system's 16-colour and 256-colour palettes. Every program carries a bundle
(signature resource, BNDL, an FREF per kind of file, the families) and the has-bundle flag (`--icons KEY` to
`mkapp.py`, `elf2app.py`, `mkinit.py`, `elf2init.py`). The Finder keeps one icon per creator and type, and Aurora68
Disk, Machine and Sound share the creator 'PSMC' (kept, like every creator); so their bundles carry the
family's common extension icon (a puzzle piece with "68"), and each its own glyph as a custom icon (the family
at ID -16455 and the Finder's hasCustomIcon flag), which System 7 shows and System 6 does not. The Stations file
stays TeachText's ('ttxt'), so that a double-click still opens it for editing; Aurora68 Log's and Aurora68 Slots'
saved reports are TeachText documents as before.

## Tools
- `tools/mac/rsrc.py`: resource fork writer (type list, reference lists, name list; 28-byte map header) and MacBinary II writer. `hcopy -m` understands MacBinary, so files land on HFS images with their resource forks.
- `tools/mac/mkinit.py`: DRVR + INIT code image → extension file. `tools/mac/mkapp.py`: flat code image → application (CODE 0 jump table, CODE 1, SIZE). `tools/mac/hfsput.py`: copy into a folder of an image (hfsutils). `tools/mac/bootblock.py`: show or set the boot blocks' startup names; `--startup "Aurora68 Network"` makes System 6 launch the application at boot (bbHelloName, what "Set Startup..." writes) with the Finder as the shell it returns to.
- `tools/mac/mkicons.py`: the Aurora68 icon family and each program's bundle (`--icons KEY` to the four packagers), `--preview PNG` the sheet of every icon (21 November 2026). `tools/mac/aurora68-layout.py`: the programs in their folders, as MacBinary files (`stage`), on an HFS volume (`disk`) or on a startup disk (`install`).
- `tools/mac/declrom.py`: declaration ROM builder (format block at the top, sResource directory, entries with self-relative 24-bit offsets, sBlocks, sExec blocks, CRC = rotate-left-and-add verified against the IIci ROM's own built-in declaration ROM). `tools/declrom.py` parses the result.
- `tools/vcshot.py`: a picture of the Pi's VideoCore display in the testbed (QEMU's emulated frame buffer, through the monitor socket): the second display (docs/10).
- `tools/pinet.py`: host peer for the testbed net service (answers ARP, ICMP echo and UDP echo on port 7, or bridges to a TAP).

## Aurora68 IO Test: the floppy and the modem port (22 November 2026)

`mac/iotest/` (`make -C mac iotest`, `build/Aurora68-IOTest.bin`; a test program, not in the release's
layout), written for docs/08 item 93. Put it in Startup Items. It waits for a volume named **Floppy**,
reads HOSTFILE from it if there is one, writes MACFILE (64 KiB, byte i = ((i·37 + 11) xor (i >> 8)) & 255),
reads it back and compares, unmounts and ejects the disk, waits for it to come back and compares again;
if no Floppy mounts within a minute it reads blocks 0 and 2 from the .Sony driver and logs the result
code. Then it opens .AOut/.AIn at the speed in its own name (rename it "IO Test 57600"; 9600 by default),
sends a line `A68SER <speed> BEGIN`, 4096 bytes of the same pattern and `A68SER END`, and reads 4096 bytes
for up to a minute, expecting byte i = ((i·13 + 5) xor (i >> 7)) & 255. "NoFloppy" or "NoSerial" in its
name leaves a part out; "Sound" plays a tone through the Sound Manager while it works. Every result is a
`[MAC] IO Test:` line in the kernel's log (byte counts, CRC-32s, result codes, the serial driver's error
flags). The far end of the line in the testbed is `tools/iotest_host.py` with MAME's null modem in the slot
called `printer`, which is SCC channel A, the Mac's modem port. Its own glue (`iotest/glue.s`) carries the
register-based File Manager, Device Manager and Sound Manager calls.

## Workflow in the testbed
```
make -C mac                                        # drivers, extension, app, declaration ROM
cp images/incoming/sys710_patched.image images/sys710pi.image
make -C mac install IMG=../images/sys710pi.image   # extensions into System Folder:Extensions, programs into Aurora68
chdman createhd -i images/sys710pi.image -o images/chd/sys710pi.chd -c none -f   # -c none: the testbed MAME serves compressed CHDs as raw files
dd if=/dev/zero of=images/pidisk/pidisk.img bs=1M count=20 && hformat -l PiDisk images/pidisk/pidisk.img
ROM=rom/patched/MacIIci_psmac.rom HD=images/chd/sys710pi.chd tools/run-fullstack.sh 480 \
    mac.disk0=$PWD/images/pidisk/pidisk.img mac.declrom=$PWD/mac/build/psmac.declrom mac.net=/tmp/psmac-net \
    bt=off wifi=off mac.inputtest=320
python3 tools/pinet.py /tmp/psmac-net peer        # in another shell
```
System 6 (`images/hd20pi.hda`, the user's System 6.0.8 volume): Aurora68 Network goes into the System Folder
and the boot blocks name it as the startup application; the applications go in the root:
```
make -C mac install IMG=../images/hd20pi.hda PART=1          # or hfsput.py for each file
python3 tools/mac/hfsput.py images/hd20pi.hda mac/build/Aurora68-Network.bin "System Folder" 1
python3 tools/mac/bootblock.py images/hd20pi.hda --startup "Aurora68 Network"
chdman createhd -i images/hd20pi.hda -o images/chd/hd20pi.chd -c none -f
ROM=rom/patched/MacIIci_psmac.rom HD=images/chd/hd20pi.chd tools/run-fullstack.sh 380 \
    mac.disk0=$PWD/images/pidisk/pidisk.img mac.declrom=$PWD/mac/build/psmac.declrom mac.net=/tmp/psmac-net \
    bt=off wifi=off mac.inputtest=250
```
The System 6 boot reaches Aurora68 Network in about three minutes; `docs/img/system6-pinet-arp.png` is what
it shows when everything works (the ARP reply arrives through the slot interrupt).
`mac.inputabout=SECONDS` plays a second script that opens the Apple menu and chooses the first
item, "About This Macintosh" (`docs/img/system71-about.png`: Macintosh IIci, System Software 7.1,
8,192K, which is the testbed's `mac.ram=8`; with Aurora68 Machine installed it says Macintosh SE,
`docs/img/system71-about-se.png`, and with 128 MiB of Mac RAM `docs/img/system71-about-128mb.png`).

The input path has no real keyboard in QEMU; `mac.inputtest=SECONDS` makes the service core play a
scripted session that many seconds after the 68k starts (the mouse walks to the Apple menu, opens
and closes it, then five letters are typed) so the screenshots show whether `.PiInput` works.

68k code can log to the Pi console through the INFO service (`log "text"` macro in the sources);
the lines appear as `[MAC] ...` in `emu68.log`.

## Saying "Macintosh SE": what it changes and what it might break
Aurora68 Machine changes two answers and nothing else: Gestalt's `'mach'` (5, gestaltMacSE, instead of
11) and SysEnvirons' `machineType` (3, envSE, instead of 9). Every other selector keeps the
ROM's truth: the processor (68030 as the kernel presents it), the FPU, QuickDraw's version,
the MMU, 32-bit addressing, the Slot Manager, the sound and video drivers. Well-written
software of the System 7 era asks those specific questions and never the machine type, so it
does not notice. What can notice:
- **Installers and System Enablers** that gate on the machine type ("this software cannot be
  installed on this Macintosh"): Apple's installer scripts of the period check the box. Keep
  Aurora68 Machine out of the System Folder while installing system software, and put it back after.
- **Programs that infer hardware from the box.** A game or utility that reads "SE" and assumes
  a 68000, a 512x342 black-and-white screen, no FPU, no slots or 24-bit addressing may turn off
  features it could use, or, if it then pokes SE hardware addresses, misbehave. The IIci ROM's
  own code does not consult Gestalt for this; the ROM keeps thinking it is a IIci either way.
- **The reverse case**: software that refuses to run on an SE at all (anything that needs
  Color QuickDraw and checks the machine instead of the QuickDraw version) will refuse here.
The remedy is always the same: Aurora68 Machine is one file, and without it the machine is a IIci
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
lands on the Pi's; Aurora68 Screens (startup item or double-click) lists what QuickDraw made of the
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
5. **The Mac**: copy **Aurora68 Screen Setup** and **Aurora68 Sound** (Mac/Aurora68/Extensions (to install)/Aurora68-ScreenSetup.bin and Mac/Aurora68/Extensions (to install)/Aurora68-Sound.bin, or the
   Aurora68-Utilities disk) into the System Folder; System 7 puts both into Extensions. On a PC with
   hfsutils, into the card's image directly:

       hmount sys710pi.image
       hcopy -m Aurora68-ScreenSetup.bin ":System Folder:Extensions:"
       hcopy -m Aurora68-Sound.bin ":System Folder:Extensions:"
       humount

   Aurora68 Sound needs Sound Manager 3.0 or later (the "Sound Manager" extension in the Extensions
   folder on System 7.1, built into 7.5); without it the Mac's sound still reaches HDMI through the
   ASC, in 8 bits.
6. **The first start**: Aurora68 Screen Setup finds two screens with no arrangement, writes one (the SE's screen main,
   HDMI to its right, top-aligned) and restarts the Mac once. From the second start both screens come up.
7. **Monitors** (control panel): drag the two monitors where they stand on the desk, move the menu bar to
   either, and choose the HDMI screen's depth (Millions needs 32-bit addressing, which the kernel switches
   on with more than 8 MB). The **Sound** control panel's volume sets the card's volume. The two monitors
   need not touch along their whole edge: pushing the pointer against the side where the other screen lies
   takes it across at the same proportion along the edge (`.PiInput`'s edge jump, 23 November 2026, docs/10
   "The edge jump"; `mac.edgejump=off` for the ROM's way, across only where they touch).

Lines in the log that say it worked: `declaration ROM: two cards, the SE's screen in slot 9 (main) and the
HDMI screen in slot $A`, `[PSMAC:DISP] VideoCore read back: physical 1024x768, ... : as asked`, `[PSMAC:DISP]
two screens: ... the firmware's own buffer, kept`, `[AUDIO] HDMI: 48000 Hz stereo through DMA channel N; HSM
clock ..., pixel clock ..., N 6144 CTS ...`, `[MAC] A68 Screen Setup: ...`, `[MAC] A68 Sound: Sound Manager 3.0; the sound
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

Aurora68 Screen Setup sees the size change at the next start, writes the arrangement again (the SE's screen at
(0,0)-(512,342), HDMI at (512,0)-(3072,1440)) and restarts once. Monitors offers every depth; millions
need 32-bit addressing, as at every size. **Aurora68 Depth HDMI 16** (a copy of Aurora68 Depth renamed, double-clicked
or in Startup Items) sets the HDMI screen's depth without Monitors: any of 1, 2, 4, 8, 16, 32 at the end
of the name; plain "Aurora68 Depth N" sets the main screen's, which on the SE is the one-bit screen.

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
   has the new size; Aurora68 Screen Setup rearranges the two screens and restarts once.

**A new declaration ROM is a file on the card.** A `.declrom` file in the card's root (beside
kernel8.img) is taken in preference to the container's of the same rank, so a new PiDisk, network, input
or video driver needs no new container: copy the new `aurora68-se.declrom` to the card. The log's
`[PSMAC] card: declaration ROM ...` lines list what the card holds, and `[PSMAC] declaration ROM: NAME from
the card's root taken (...)` says which one was used. Take old ones off the card when you update: a file on
the card beats the container's. `mac.cardrom=off` in cmdline.txt ignores the card's files.

### Aurora68 Display: choosing the HDMI resolution and rotation on the Mac (21 November 2026)
**Aurora68 Display** (`mac/pidisplay/pidisplay.c`, `Aurora68-Display.bin`, an application) shows the HDMI monitor's own
description and every resolution it lists, judged for a Pi 3 (usable, too fast with a slower alternative,
the wrong shape), suggests one with its reason, and sets the HDMI resolution and the picture's rotation.
docs/10, "Aurora68 Display", is the record of how; these are the steps on the user's SE.

1. **kernel8.img**, the new one, and the new **Aurora68 Screen Setup** in the System Folder (it asks "Keep this
   resolution?" after a change; Mac/Aurora68/Extensions (to install)/Aurora68-ScreenSetup.bin). **Aurora68 Display** anywhere on the Mac.
2. **config.txt, once**: the line

       include aurora68-display.txt

   at the very end, after every `hdmi_` line and in an `[all]` section (the release's config.txt ends so).
   The firmware takes the last value of a setting it reads, so an `hdmi_mode=` or `hdmi_cvt=` after the
   include would win over Aurora68 Display's choice; Aurora68 Display says so under Current. The release's
   `aurora68-display.txt` (comments only) goes on the card beside config.txt; the kernel makes the file
   itself if it is not there, but a firmware that is strict about a missing include would be the first
   thing to suspect if a card without it does not start. config.txt is never written by the kernel.
3. **In Aurora68 Display**: click a resolution (the bullet marks the suggested one; dimmed ones cannot be
   chosen), or a rotation, then **Restart to Apply...** and **Restart** in the dialog. A new resolution:
   the kernel writes `aurora68-display.txt` (the old one becomes `aurora68-display.bak`), the Mac shuts
   down its programs and restarts, and the Pi restarts with it, so that the firmware reads config.txt
   again. A rotation alone restarts only the Mac.
4. **After a new resolution**: on the SE's own screen Aurora68 Screen Setup asks **Keep this resolution?** with a
   countdown. **Keep** (Return) keeps it. **Revert** (Escape), or 15 seconds without an answer, puts the
   previous file back and restarts the Pi once more. Aurora68 Screen Setup also rearranges the two screens for the new
   size, which costs one restart of the Mac, as after any change of monitor.

For 2560 by 1440 on the UP2716D Aurora68 Display should suggest 2560 x 1440 at 30 Hz (60 Hz needs 241.5 MHz, the
Pi 3 sends about 162), which writes `hdmi_group=2`, `hdmi_mode=87`, `hdmi_cvt=2560 1440 30 3 0 0 1`, the two
`max_framebuffer_` lines and `hdmi_drive=2`: the same as the five lines of "2560 by 1440 on the SE" above,
which can then come out of config.txt.

Lines in the log to send back: every `[PSMAC:EDID]` line (the first, `the firmware's EDID: N blocks in U
us`, says whether the firmware gave the EDID at all; then the monitor, its sound, a line per mode with
its judgement, the suggestion and the mode in use), `[CARD] aurora68-display.txt: N bytes written and read
back`, `[PSMAC:EDID] the whole Pi restarts now (its watchdog)`, after the restart `[PSMAC:EDID] the card:
config.txt read (it includes aurora68-display.txt), ...; a new mode waits to be confirmed`, `[MAC] A68 Screen Setup: a
new HDMI mode waits to be kept`, `[MAC] A68 Display: ...`, and from the heartbeat `display 1 (HDMI): ...
VideoCore N frames in 5 s, longest U us in B bands, turned 90` for a rotated screen.

## Aurora68 Play: MP3, AIFF and WAV on the sound card (21 November 2026)
The user's SE plays the Mac's sound in stereo on HDMI (above); they wanted to play MP3 files. System 7.1
on a 68k has no MP3 player, the test image has no sound-file player at all, and third-party Mac software
is not to be downloaded; a 68000-class Mac could not decode MP3 in time anyway. So the card decodes, as
some real cards did with a DSP: **Aurora68 Play** (`mac/piplay/piplay.c`, C, the kit of `mac/c`) reads the file
with the File Manager and hands its bytes, as they are, to the card's **stream channel**; the Pi's core 1
decodes MPEG audio with minimp3 (`emu68/src/psmac/third_party/minimp3/`, CC0, unmodified; compiled in
`psmac_mp3.c`), converts the PCM of AIFF and WAV files, resamples to the output's rate with the card's
filter and mixes the result with the ASC and the card's Sound Manager ring, so beeps sound over the music.
The SE's speaker plays the stream only with `mac.sestream=on` (in MAME's SE a beep during music hung the Mac
while the stream fed the speaker; docs/08 item 82 is the record).

**The stream channel** (`psmb.h`, struct psmb_stream): a 384-byte block at window + $11100, beside the
sound card's, and a 44 KiB byte ring at window + $71000 (between the log's ring and slot $A's declaration
ROM; $971000 in 24-bit mode). The Mac writes `head`, the Pi `tail`, both counting bytes for ever. A new
file: STOP (the Pi empties the ring), wait for `done_seq`, fill in `format` (MPEG or PCM, and for PCM the
rate, channels, bits, byte order and signedness) and `data_bytes`, write the first bytes, START; more bytes
whenever the ring has room, `eof` after the last; PAUSE, RESUME, STOP. The Pi answers with the state
(playing, paused, ended, error and why), what it found (rate, channels, layer, the bit rate), the position
and the length in ms, how much it holds, MPEG frames decoded, bytes skipped, underruns, and its decoder's
load on core 1 in per mille. The Mac also writes `title` (128 bytes, Mac Roman, what is playing) and then
bumps `title_seq`; the kernel logs it (`[AUDIO] stream: now playing "..."`). Aurora68 Play writes the ID3 title or
the file's name there. Made ready for a radio stream (the next step: an MP3 stream over MacTCP, its ICY
metadata stripped on the Mac): a stream may be open-ended (`data_bytes` 0, no `eof`: the length shows as
unknown and the stream plays as long as bytes come), minimp3 looks for the next frame header after a gap
or damage and the decoder never stops on bad bytes once a first frame was found (the bytes are counted as
skipped, a gap as underruns, and playback starts again once enough is in hand), the station's "now
playing" goes into `title`, and STOP followed by START restarts everything without a reset of the Pi. Core 1 empties the ring into 2 MiB of its own at once, so the Mac fills that
much ahead: about 90 s of a 192 kbit/s MP3, 12 s of CD-quality PCM; a held menu, a dialog, another program
in front or a slow SCSI disk cost nothing until that runs out. Decoding is one MPEG frame (1,152 samples)
per poll of core 1's loop and only while fewer than 8,192 decoded frames wait, so the output's DMA blocks
and the USB work on the same core are never held up by more than one frame's decoding. The length comes
from the Xing/Info frame LAME and ffmpeg write (its frame count; the LAME tag's encoder delay and padding
are taken off, so a file starts with its first sample), else from the bit rate and `data_bytes`.

**Aurora68 Play** finds what a file is from its content, never its type or name (a file copied from a PC has
none): an ID3v2 tag ("ID3") or an MPEG frame header confirmed by the next one is MPEG audio (layers I-III,
all MPEG-1, 2 and 2.5 rates); "FORM" ... "AIFF"/"AIFC" is AIFF (uncompressed: 'NONE', 'twos', 'sowt'); "RIFF"
... "WAVE" is WAV (PCM, 8, 16 or 24 bits; format 0x55, MP3 inside a WAV, goes to the decoder). Compressed
AIFF-C (IMA, MACE, µ-law) and floating-point WAV are refused in plain words. The ID3v2 tag gives the title
and artist (TIT2, TPE1; Latin-1, UTF-16 and UTF-8 into Mac Roman), else the ID3v1 tag at the end, else the
file's name; neither tag is sent to the card.

The window follows the Human Interface Guidelines of 1992: a document window with a grow box and a zoom
box (340 by 106 at the first start and at the smallest, which fits the SE's screen; the zoom box's other
state is as wide as the main screen allows, 140 high); the title in Chicago, the artist and the format
("Aurora68 test — MP3, 44.1 kHz stereo, 192 kbit/s") and the progress line with "0:42 of 1:44" and the
state run the window's width, the Play/Pause and Stop buttons and the volume slider sit along its bottom,
clear of the grow box (drawn alone, without the scroll-bar lines a window without scroll bars should not
have). Menus: Apple (About Aurora68 Play…), File (Open… ⌘O, Close ⌘W, Quit ⌘Q), Edit (Undo, Cut, Copy, Paste,
Clear: for desk accessories, dimmed while Aurora68 Play is in front), Controls (Play/Pause ⌘P, Stop ⌘., Volume
Up ⌘], Volume Down ⌘[); the space bar plays and pauses while the window is in front, Esc stops, the
arrows change the volume, Return opens or plays; a ⌘-key that is no command beeps. Closing the window
(⌘W or its close box) closes the sound and leaves Aurora68 Play running; Open… brings the window back. The
window's place and size and the volume go into "Aurora68 Play Prefs" (type 'pref', in the Preferences folder
from FindFolder on System 7, in the System Folder on System 6) at Quit, and come back at the next start
when the place is still on a screen (both ends of the title bar inside the desktop region), else the
window goes on the main screen and the log says so. Update, activate, suspend and resume events are
handled, the SIZE resource says canBackground, so the music goes on with another program in front. The
volume is the slider times the Sound control panel's level (SdVolume). Errors and the About box come in a
small window of their own, centred a third of the way down the main screen, with OK outlined as the
default button (Return, Enter or Esc); Standard File's dialog is centred the same way. Everything is drawn
with patterns, in black and white, so the SE's one-bit screen and a colour screen show the same. Not done:
Balloon Help (the project has no resource compiler, and a hand-built 'hmnu' that the Help Manager reads
wrongly is worse than none), a playlist, and seeking.

From the Finder: Aurora68 Play carries a bundle (`tools/mac/mkicons.py`, since 21 November 2026; below, "The
programs and their folders": signature 'PIPY', BNDL 128, an FREF for the application, for MP3 ('MPG3'), AIFF and
WAV ('WAVE') files and the preferences ('pref'), each with its icon family, and '****' for every other kind
of file) and the has-bundle flag, so the Finder registers it; files dropped on its icon, or opened while
their creator is 'PIPY' (a double-click on a file of the Music folder that `aurora68-layout.py --music` typed),
reach it as the application's files (AppParmHandle), the way
System 6 does it and System 7 does for a program that is not high-level-event aware. (An Apple-event version,
'odoc', stopped with "bad F-Line instruction" in the testbed when the Finder launched it with a document; it
was taken out, docs/08 item 82.)
Every step is in the Pi's log as `[MAC] A68 Play: ...`, the kernel's as `[AUDIO] stream: ...`, and each
heartbeat has a `[PSMAC]   stream:` line while a stream has been used. mac.audio=off (or no output found):
Aurora68 Play says the Pi plays no sound and quits; an older kernel without the channel: it says so.

Test sounds: `tools/mksounds.py OUTDIR` (standard library; the MP3 step runs `lame`, else ffmpeg) makes a
left-then-right check (440 Hz left, 660 Hz right, 880 Hz both, 2 s each), a sweep from 100 Hz to 10 kHz
and 1 kHz, as AIFF, WAV, AIFF-C 'sowt', 8-bit mono WAV and MP3, and three pieces of public-domain music
played by a small synthesizer, each with an ID3v2 title and the artist "Aurora68 test": the Minuet in G
of the Anna Magdalena Notebook (44.1 kHz, 192 kbit/s, and as AIFF), Beethoven's Ode to Joy (32 kHz,
160 kbit/s) and Joplin's The Entertainer (22.05 kHz, 128 kbit/s). Nothing is downloaded; the files are
not in the repository (about 35 MB): run the script. In the testbed (`tools/run-solo.sh` with
`mac.audiodump=PATH` and `mac.remotefile=BASE`, the files copied raw, with no type, by `hcopy -r`):
- LR check (MP3 at 192 kbit/s, AIFF, WAV, AIFF-C 'sowt'): 440.0 Hz on the left alone, 660.0 Hz on the right
  alone, 880.0 Hz on both, each 2.05 s with 0.45 s between (tools/audiocheck.py on `mac.audiodump=`), the
  other channel silent; the MP3 starts with its first sample (the encoder's delay dropped).
- Sweep (MP3, and the 8-bit mono WAV at 22,050 Hz): 10.05 s of glide, then 1000.0 Hz for 3.05 s; 50 ms
  windows found 310, 978 and 3095 Hz where the glide should be at 312, 986 and 3112 Hz.
- Minuet in G (MP3, 104 s): played to the end, no underruns, length 1:44 from the Info frame; paused for
  4.8 s (silent within 0.3 s of the key, a 6 ms fade) and resumed; two System beeps through Aurora68 Sound
  sounded over it (440 Hz with 1320 and 2200 Hz, a square wave, against the music), nothing clipped. Ode to
  Joy (32 kHz MPEG-1) and The Entertainer (22.05 kHz MPEG-2) found, played and stopped.
- The decoder on core 1, under QEMU's emulation: 30 to 75 per mille of the core, the longest slice 13 to
  28 ms; on the x86 host the same code decodes 104 s in 0.14 s, so a Pi 3 should be near 1 or 2 per cent
  (to be read from the heartbeat's `stream:` line on hardware).
- CD-quality PCM needs 176 KB/s from the Mac's disk. The short AIFF and WAV files play without a gap; the
  18 MB AIFF of the minuet had gaps adding up to 9 s in 104 (the testbed's System 7.1 reads its disk image
  through semihosting at about 160 KB/s). MP3s need 16 to 24 KB/s.
- The window: grown, zoomed (the standard state across the screen) and back, the contents following
  (`docs/img/system71-piplay-resize.png`); its place and size came back at the next start, and on MAME's SE
  (512 by 342, one bit) a place remembered from the 640 by 480 screen was found off the screen and the
  window put on the main screen (`docs/img/se-piplay.png`); the About box and the error windows centred
  with OK outlined.

### Internet radio in Aurora68 Play (21 November 2026)
Aurora68 Play 1.1 plays internet radio: SomaFM (https://somafm.com/), the listener-supported, commercial-free
stations of San Francisco, and any other Shoutcast or Icecast station that sends MP3 over plain
`http://`. The Mac fetches the stream with MacTCP's own programming interface (Riptide or Apple's
MacTCP), takes the station's titles out of it and pours the audio into the card's stream channel; the
Pi decodes it as it decodes an MP3 file. A 68k could not do the encryption of `https://` and does not
try. SomaFM is supported by its listeners; if you enjoy it, support it at https://somafm.com/support/.
Aurora68 carries no recording of theirs: Aurora68 Play's list holds only the addresses of their public streams.

**Listening, step by step.**
1. Network first: Riptide (or MacTCP) set up and working, as for Internet Explorer: Riptide in the
   Extensions folder and "Riptide DNR" in the System Folder (docs/09 "MacTCP"; Riptide's docs/MAC.md).
2. Copy Aurora68 Play (Mac/Aurora68/Aurora68 Play/Aurora68-Play.bin, `hcopy -m` or any MacBinary tool) onto the Mac, replacing the old one. The
   kernel needs no change for the radio (the stream channel of item 82 already takes open-ended streams).
3. Start Aurora68 Play. The **Radio** menu lists the stations, in submenus (SomaFM ▸, Listener-supported ▸,
   By country ▸ Netherlands ▸ ...); choose one. The window shows the station's name,
   "Connecting", then "Buffering" with a percentage, then "Playing", the station's name for itself, "Now
   playing: ..." with the title of what is on, the format and bit rate, and a meter of what the card holds
   (full at 30 seconds). Play/Pause (space, Command-P) pauses: the station goes on into the card's buffer
   (up to two minutes at 128 kbit/s) and plays on from there; Stop (Esc, Command-.) closes the connection.
4. **Radio > Open URL… (Command-U)** plays an address of your own: `http://host[:port]/path`, the
   `http://` may be left out; a `.pls` or `.m3u` playlist's address works too (its first `http://` stream
   is taken). Command-V pastes into the field.
   **Your own music as stations:** `tools/music-server` (a Docker container for a NAS, or one Python file)
   serves a folder of MP3s as stations, one per folder, over IPv4 and IPv6, and writes a stations file for
   the Radio menu (`http://NAS:8000/stations.txt`); its README says how (22 November 2026).
   **The Library (Aurora68 Play 1.2, 22 November 2026, docs/08 item 99).** The same server's music as a
   library, in a window of its own: **Library > Show Library** (Command-L). It finds the server by itself
   (a UDP probe) or takes an address (**Library > Connect to Server...**, such as `192.168.1.20:8423`, or
   empty to look again), and shows "Library — the server's name". The views, one at a time with **Back**
   (or Delete, or the left arrow): **Artists**; an artist's **albums**, each with its cover; an album's
   **tracks** under its cover, with their lengths. Double-click (or Return, or the right arrow) opens a row;
   on a track it plays the album from that track, gaplessly, the next track's stream following the last
   one's into the card. **Play** plays the album, or all of an artist's albums; **Shuffle** shuffles what is
   queued. Typing selects by name ("c" finds The Cure: a "The " in front is left out, as record shops file
   them). **Find...** (Command-F) searches artists, albums and tracks. The bar along the window's bottom
   and the player window both show what plays: its cover, the track, the artist and album, the time; in
   the player window, **<<** and **>>** (or the left and right arrows) are Previous and Next, and a click
   on the progress line moves in the track. Covers are dithered to one bit on the SE's screen (Bill
   Atkinson's dithering, done by the server) and come in the Mac's 256 colours on a colour screen.
   **Listen Live** tunes to the server's Live channel (what its web control panel plays) as a station.
   **Connect By > IPv6 First / IPv4 First** (Library and Radio menus) chooses the family tried first when
   a server's or a station's name has both, the other tried when it fails; it is kept in "Aurora68 Play
   Prefs" with the server, Shuffle and the Library window's place. The window starts at 500 by 292 on the
   SE's screen, 540 by 360 on larger ones, and takes any size from 360 by 200.
5. **Your own stations:** the list is the text file "Aurora68 Play Stations" in the Preferences folder (System
   6: the System Folder), written at Aurora68 Play's first start from the built-in list. Open it with TeachText
   or SimpleText (double-click it), add a line `Name = http://host[:port]/path` for each station (a line holding only an
   address is named after its host; lines starting with # are notes), save, and come back to Aurora68 Play: it
   reads the file again when it comes to the front or when you open a menu. A line `[Group]` puts the
   stations below it into a submenu of the Radio menu, `[Group/Sub]` into a submenu of that one (below,
   "Groups of stations"). Throw the file away and the built-in list comes back at the next start;
   **Radio > Add Default Stations** adds the built-in stations your file lacks without touching the rest. At
   most 128 stations and 48 groups.

The built-in list (since 21 November 2026, Aurora68 Play's first start writes it; a file written earlier
keeps its flat list until Add Default Stations or the file is thrown away), as the Radio menu shows it:
- **SomaFM ▸** Groove Salad, Drone Zone, Secret Agent, Lush, Space Station Soma, Indie Pop Rocks!, Beat
  Blender, Deep Space One, DEF CON Radio, Left Coast 70s, Underground 80s, Sonic Universe: each SomaFM
  channel `http://ice2.somafm.com/<channel>-128-mp3`, the 128 kbit/s MP3 streams that SomaFM's playlists
  (somafm.com/<channel>.pls) name; their playlists now point at the same streams over https, which the
  ice servers still also serve over plain http on port 80 (seen 21 November 2026).
- **Listener-supported ▸** Radio Paradise Main, Mellow, Rock and Global (`http://stream.radioparadise.com/mp3-192`,
  `mellow-192`, `rock-192`, `global-192`), KEXP Seattle (`http://kexp-mp3-128.streamguys1.com/kexp128.mp3`), WFMU
  Freeform (`http://stream0.wfmu.org/freeform-128k.mp3`), WWOZ New Orleans
  (`http://wwoz-sc.streamguys1.com/wwoz-hi.mp3`), Radio Swiss Jazz, Classic and Pop
  (`http://stream.srg-ssr.ch/m/rsj/mp3_128`, `.../m/rsc_de/mp3_128`, `.../m/rsp/mp3_128`; each sends Aurora68 Play
  on twice, to `/srgssr/rsj/mp3/128` and then to a `livestreaming-node-N.srg-ssr.ch`, within the three
  redirections it follows).
- **By country ▸** Netherlands ▸ NPO Radio 2, NPO 3FM, NPO Klassiek (`http://icecast.omroep.nl/radio2-bb-mp3`,
  `3fm-bb-mp3`, `radio4-bb-mp3`); Switzerland ▸ the three Radio Swiss channels; Austria ▸ Ö1
  (`http://ors-sn05.ors-shoutcast.at/oe1-q2a`); United Kingdom ▸ Classic FM
  (`http://media-ice.musicradio.com/ClassicFMMP3`), BBC World Service
  (`http://stream.live.vc.bbcmedia.co.uk/bbc_world_service`); France ▸ Radio Nova
  (`http://novazz.ice.infomaniak.ch/novazz-128.mp3`); Germany ▸ 90s90s Pop (`http://streams.90s90s.de/pop/mp3-128/`);
  Greece ▸ ERT Kosmos (`http://radiostreaming.ert.gr/ert-kosmos`); New Zealand ▸ RNZ Concert
  (`http://radionz-ice.streamguys.com/concert.mp3`); United States ▸ KEXP, WFMU, WWOZ, 181.fm The Beatles
  (`http://listen.181fm.com/181-beatles_128k.mp3`) and 181.fm Classic Hits (`http://listen.181fm.com/181-greatoldies_128k.mp3`).

Forty stations, each address answered from a test machine on 21 November 2026 with HTTP 200 or 206 and
`audio/mpeg` over plain http (the Radio Swiss ones after their two redirections). Asked for too and left
out, because they did not answer over plain http or refused: Australia's ABC, FIP, Deutschlandfunk, RAI,
Jazz24, Radio Caroline and Linn (they may need https, which a 68k cannot do). If a station moves, change
its line. **IPv6:** of the forty, only NPO's server (`icecast.omroep.nl`, eight AAAA records) has IPv6
addresses (looked up the same day through a public resolver; none of the other hosts, nor the Radio Swiss
servers the redirections lead to, has any), so NPO Radio 2, 3FM and Klassiek are the stations to try
Riptide's `mactcp ipv6 prefer` with (below, "IPv4 or IPv6").

#### Groups of stations: the Radio menu's submenus (21 November 2026)
The owner asked for many more stations, grouped. Forty stations in one menu would not fit the SE's 342
lines, so the station list got headings and the Radio menu got hierarchical submenus, as Inside Macintosh
volume V describes them (the Menu Manager of the Macintosh II and SE and of System 7):
```
# notes start with #
My Station = http://radio.example.net:8000/stream     <- above the first heading: in the Radio menu itself
[Testbed]                                             <- Radio > Testbed >
Test tone = http://radio.testbed:8000/stream
[Testbed/Over IPv6]                                   <- Radio > Testbed > Over IPv6 >
Test tone over IPv6 = http://v6only.testbed/stream
[]                                                    <- back to the Radio menu itself
```
A heading seen again adds to the group it named first (letters of either case alike), a heading with no
station under it makes no submenu, one level of nesting only (in `[A/B/C]` the submenu of A is called
"B/C"), and a line that only begins with `[` (`[Live] = http://...`) is a station. Items stand in the order
the file first names them, the Radio menu's commands below a line: **Open URL… ⌘U** and **Add Default
Stations**. A file without headings, as every file written before 21 November 2026 is, gives the flat menu
it gave before. The station that plays is checked in its submenu, and in every other submenu that has the
same address (KEXP is in Listener-supported and in United States).

**Add Default Stations** compares the file with the built-in list and appends, at the end of the file,
every built-in station that the file does not have in the same group (a heading compared without regard
to case) or in the Radio menu itself, under its heading and that heading's notes, after a note
"# Added by Aurora68 Play's Radio > Add Default Stations:". Nothing already in the file is changed, moved
or removed; a second time it adds nothing and says so. A file of the first kind, with SomaFM's twelve and
NPO Radio 2 at the top, gets Listener-supported and By country (27 stations; not SomaFM, which it has, and
not NPO Radio 2 again under Netherlands); its own stations stay where they were. A window says how many
were added; if the file cannot be written (open in an editor that keeps it open for writing, a locked
disk) it says so and adds nothing.

How it is made: `mac/piplay/stations.c` (`play/core/stations.c` since 22 November 2026, below, "The shared core") (portable C, also run on the host by `make -C mac check` through
`tools/stations_test.c`) holds the built-in list (Mac Roman: the Ö of Ö1 is $85) and takes the file apart
into stations, groups and their order; `radio.c` reads and writes the file; `piplay.c`'s `fill_menu` builds a
menu per group with NewMenu, puts it into the menu list with `InsertMenu(menu, -1)` (the hierarchical
part) and marks its parent item with the command character $1B (SetItemCmd) and the submenu's menu ID as
the item's mark (SetItemMark). Submenus take IDs 140 to 187 (group 0 to 47): an application's submenus
must have IDs from 1 to 235, as the IDs above are the desk accessories', and the menu bar's own are 128 to
132. When the file changes the submenus are taken out (DeleteMenu) and disposed of (DisposeMenu) and the
menu is built again. MenuSelect answers with the submenu's ID and the item, which `choose_station` turns
back into a station. A Radio > Stations File… command that would open the file in TeachText (an 'odoc'
Apple event to the creator 'ttxt', or LaunchApplication with the event) was considered and left out: it
needs the Apple Event Manager, the Alias Manager and a search of the desktop database for the editor,
none of which this program uses yet, and Aurora68 Play's own Apple-event handling was taken out after a
"bad F-Line instruction" (item 82); a double-click on the file in the Finder does the same.

**How it works.** `mac/piplay/radio.c` opens `.IPP` by name, as every MacTCP program does (or Riptide's
`.IPP6` when it is there: below, "Riptide's .IPP6"), and finds the
name resolver the way Apple's DNR glue does (the Control Panels folder for a `'cdev'` of creator
`'ztcp'`, then the System Folder and the Control Panels folder for `'mtcp'`; the first file whose
resource fork holds `'dnrp'` 1, which is "Riptide DNR" with Riptide and "MacTCP DNR" with Apple's MacTCP).
It sends `GET /path HTTP/1.0` with `Host:`, `User-Agent: Aurora68-Play/1.1 (Macintosh)` and
`Icy-MetaData: 1`. `mac/piplay/icy.c` (now `play/core/icy.c`) takes the answer apart: an `HTTP/1.x` or `ICY 200 OK` status line,
the headers in any case (`icy-metaint`, `icy-name`, `icy-br`, `Content-Type`, `Location`,
`Transfer-Encoding`), redirections (301, 302, 303, 307, 308; three at most; to plain http only), a
playlist's first stream, HTTP/1.1's chunks (a server or proxy that answers 426 "Upgrade Required" is
asked again with HTTP/1.1), and the metadata block after every `icy-metaint` bytes of audio, from which
`StreamTitle='...';` is taken (UTF-8 or Latin-1 into Mac Roman). The title goes into the window and into
the stream block's `title` field (`title_seq` bumped), which the Pi logs. The receive call is
asynchronous and its completion routine (`mac/piplay/net.s`, `c_rcv_done`) runs the parser and puts the
audio into the card's ring at interrupt time, then makes the next receive, so the stream goes on while a
menu is held, a dialog is up or another program is in front; the event loop only connects, reconnects
and redraws. The card is started paused and resumed when three seconds of audio are in hand. If the
connection ends, or nothing comes for ten seconds (TCPRcv's command timeout), Aurora68 Play connects again
after 1 s, then 2, 4, 8, 16 and 30 s, while the card plays what it still holds; the decoder finds the next
frame in what the new connection brings. Every step is in the log as `[MAC] A68 Play: radio: ...`.

**IPv4 or IPv6** (21 November 2026, the owner's request). The window's counts end with the IP version the
stream travels over, "0 reconnects, 0 underruns — IPv6" (or "— IPv4"; an em dash, as Geneva 9 has no middle dot), and the log's connection line says
it too: `[MAC] A68 Play: radio: connected to 198.18.0.5:80 (IPv6 through Riptide's stand-in); asking for
/radio2-bb-mp3` (`docs/img/system71-aurora68-play-ipv6.png`: the testbed's station reached over IPv6). Aurora68 Play asks nothing of Riptide for it: MacTCP's interface has IPv4 addresses only,
and Riptide gives a MacTCP program a stand-in from 198.18.0.0/15 (RFC 2544's benchmarking block, which no
real host has) for a host it reaches over IPv6, carrying the connection over IPv6 (RFC 6535,
"Bump-in-the-Host"; Riptide's docs/MAC.md, "IPv6 for MacTCP programs"). So an address in that block is
IPv6 and every other one IPv4 (`icy_ip_version` in `mac/piplay/icy.c`, checked by `make -C mac check`); with
Apple's MacTCP it always says IPv4. A station with only IPv6 addresses gets a stand-in by itself; one with
both (dual stack) is reached over IPv4 unless riptide.conf says `mactcp ipv6 prefer`. The built-in list
ended with such a station, NPO Radio 2 (Dutch public radio, `http://icecast.omroep.nl/radio2-bb-mp3`, MP3
at 192 kbit/s, with AAAA records), as the test of it; since the groups (above) it is in By country ▸
Netherlands with NPO 3FM and NPO Klassiek, the built-in list's only stations with IPv6; SomaFM has none.

**Riptide's .IPP6** (21 November 2026, the owner's request). Riptide (branch `mac-port` from commit
f592402) installs a second driver beside `.IPP`: `.IPP6`, MacTCP's calls and parameter blocks with
20-byte addresses that hold IPv6 (IPv4 as `::ffff:a.b.c.d`) and a resolver of its own (Riptide's
`docs/IPP6.md` is the specification, `sdk/mac/IPP6.h` its header, copied into `mac/piplay/IPP6.h`).
Aurora68 Play asks Gestalt for `'ipp6'` (version 1 or later) and then opens `.IPP6`; if either fails it
uses `.IPP` and the DNR exactly as before. The log says which at every station: `radio: via .IPP6
(Riptide: IPv6 and IPv4 addresses, names by the driver)` or `radio: via .IPP (MacTCP's interface: IPv4,
names through the DNR)`. Over `.IPP6`:
- a name goes to `ip6Resolve` (csCode 72), asynchronously, with the port filled in and the flag for IPv6
  addresses first (RFC 6724's default order would put a unique local address such as the testbed's
  `fd68::` after IPv4): `radio: peer.testbed is [fd68::1], 192.168.68.1`. No `'dnrp'` is needed;
- `TCPActiveOpen` gets the addresses in turn, the 20-byte address at offset 36 (where MacTCP has
  remoteHost and remotePort; the fields after it are 28 bytes further on, `TCP6iopb` is 130 bytes):
  `no connection to [fd68::1]:8000, error -23015: trying the next address`, then `connected to
  192.168.68.1:8000 (IPv4)`. With more than one address each gets 15 s instead of 30; an IPv6 address that
  nothing answers fails with ipDestDeadErr after about 3 s;
- an IPv6 address in the URL, in brackets as RFC 3986 writes it (`http://[2001:db8::1]:8000/stream`), goes
  through `ip6StrToAddr`, a dotted one too. Over `.IPP` such a URL is refused: "An IPv6 address needs
  Riptide's .IPP6, which this Macintosh does not have. Use the station's name instead." The `Host:` header and a redirection to a path on the same server keep the
  brackets;
- the IPv4/IPv6 after the counts is the family of the address actually connected to (an IPv4-mapped one is
  IPv4), and the log writes it `[fd68::80]:80` or `192.168.68.1:8000`;
- TCPCreate, TCPSend, the receive chain at interrupt time, TCPAbort and TCPRelease are MacTCP's calls byte
  for byte and the same code serves both drivers. A lookup still out when a station stops is withdrawn
  (`ip6ResolveCancel`), as `docs/IPP6.md` asks of a program before it quits.

The testbed's Riptide with `.IPP6` was built from that commit for the test image
(`Riptide-aurora68.bin`, the "Riptide" extension); the image's older Riptide (no `.IPP6`) is what the
fallback was tested with. Seen in the testbed (System 7.1, a station list with a Testbed group and
an Over IPv6 submenu): over `.IPP6` `http://radio.testbed:8000/stream` (IPv4 only) played over IPv4,
`http://v6only.testbed/stream` (IPv6 only) and `http://[fd68::80]/stream` over IPv6 (window: "— IPv6",
`docs/img/system71-aurora68-play-ipp6.png`), `http://peer.testbed:8000/stream` (both kinds) tried
`[fd68::1]:8000` first, was refused there (the testbed's peer relays port 8000 over IPv4 only) and played
over IPv4; with the older Riptide the same list played over `.IPP` (docs/08 item 88 has the record).

**Plain words when it fails:** "The name could not be found: is the network up?" (the resolver knows no
such name, or no name server answered), "The station did not answer." (no connection), "The station has
no stream at that address (404 Not Found).", "This station sends AAC (audio/aacp), and Aurora68 Play plays MP3
streams only.", "Aurora68 Play plays plain http:// streams only: a Macintosh cannot do the encryption of
https.", "The station sent Aurora68 Play from one address to another more than three times.", "MacTCP is not
there: Aurora68 Play needs MacTCP (or Riptide) to play internet radio." Once a station has played, a lost
connection is not an error: the window says "Lost: ..." and "Retrying in N s".

**The log lines to send back** when a station does not play: every `[MAC] A68 Play: radio: ...` line from
the station's name to the end, the `[AUDIO] stream: ...` lines and one `[PSMAC]   stream:` heartbeat line
(Aurora68 Log's Mac tab shows them, or the Pi's log through the remote control).

**In the testbed** (no Internet there, so the station is our own): `tools/radiotest.py` is a small
Icecast-like station in Python's standard library. It plays an MP3 as a live stream with one clock for
all listeners, a 64 KB burst at connection (Icecast's default), a metadata block every 16,000 bytes and a
new StreamTitle every 20 s (with an apostrophe, an em dash and an e acute, in UTF-8); it answers `/stream`
(HTTP/1.0 200), `/icy` (ICY 200 OK), `/redirect`, `/moved`, `/loop`, `/secure`, `/pls` and `/aac`, and on a
control port of its own it drops or stalls its listeners on command. `tools/mksounds.py --radio` makes its
stream, "Radio tone.mp3": five minutes of 1 kHz on the left and 1.5 kHz on the right at 128 kbit/s, in
which `tools/audiocheck.py --breaks 1000,1500` finds every lost, doubled or damaged frame and every gap.
```
python3 tools/mksounds.py /tmp/snd --radio
python3 tools/radiotest.py "/tmp/snd/Radio tone.mp3" &                       # 127.0.0.1:18000, control :18001
PINET_FORWARD="8000=127.0.0.1:18000" python3 tools/pinet.py /tmp/net peer &
HD=test.image tools/run-solo.sh 600 mac.net=/tmp/net mac.audiodump=/tmp/dump.pcm
curl http://127.0.0.1:18001/drop          # the server closes the connection; ?how=reset, /stall?s=15
python3 tools/audiocheck.py /tmp/dump.pcm --breaks 1000,1500
```
The Mac reaches the station as `http://radio.testbed:8000/stream` (pinet's DNS names every `*.testbed`
with the peer's address) or `http://192.168.68.1:8000/stream`.

**Seen in the testbed** (21 November 2026; the solo board under QEMU, System 7.1, Aurora68 Play driven through the
remote control's semihosting channel, the output recorded with `mac.audiodump=`):
- *With Riptide* (Riptide and Riptide DNR installed, MacTCP taken out; a "Aurora68 Play Stations" of the user's
  kind already in the Preferences folder: a test station with a redirect, one with Shoutcast's answer, a
  playlist, a bare address and two SomaFM lines): the Radio menu listed the six (`docs/img/system71-piplay-radio-menu.png`);
  choosing the first, Aurora68 Play opened the resolver in "Riptide DNR", looked up `radio.testbed`, followed the
  302 to `/stream`, got `icy-metaint: 16000`, showed the station's name, "Now playing: Aurora68 test signal
  - part 5 (Don't panic) — Café" with its em dash and accent, and started at 5.1 s in hand (the station's
  64 KB burst arrives at once); each new title reached the window and the Pi's log (`[AUDIO] stream: now
  playing ...`). The tone ran 33 s through 33 metadata blocks and two title changes with no phase step, no
  dip and no damaged sample on either channel. The server dropped the connection: Aurora68 Play was back two
  seconds later ("reconnected: reconnect 1"), no underrun, and the junction, where the old connection's
  last frame is cut short and the first new one lacks its bit reservoir, is 40 ms of a dip five seconds
  later in the sound; then the tone again without a break. The Radio menu held down for 15 s: no break at
  all. The server stalled for 16 s without closing: the card's 7 s ran out (underrun 1 counted), the
  receive timed out at 10 s ("nothing came for ten seconds"), Aurora68 Play reconnected, and after 5.1 s of
  silence the sound went on. Pause (9 s) and Resume went on from where it paused (8.6 s in the buffer
  meanwhile, `docs/img/system71-piplay-radio.png` is the window after the stall). Shoutcast's "ICY 200 OK"
  from a dotted address and the playlist (an https stream first, then the plain one, which was taken)
  played the same way. Every failure gave its own words (`docs/img/system71-piplay-radio-error.png`): a
  redirect loop, a name that does not exist, a port with nothing behind it ("The station did not
  answer."), AAC, an https address, a redirect to https, a 404; the SomaFM entries, with no Internet in the
  testbed, "The name could not be found: is the network up?". Close and Quit while playing released the
  stream.
- *With Apple's MacTCP 2.0.6* (the user's image as it is, no station list yet): the first start wrote
  "Aurora68 Play Stations" ('TEXT', 'ttxt', the twelve SomaFM channels); `http://192.168.68.1:8000/stream` from Open
  URL… played, the server's drop brought a reconnection two seconds later, and the tone was unbroken but for
  the 40 ms junction. A named host found the resolver in the MacTCP control panel (its `'dnrp'`, as the
  glue finds it) and was answered with "no name server" (-23044), since this MacTCP has none set: Aurora68 Play
  now says that in those words.
- *The list edited while Aurora68 Play ran:* from the Finder (Find, then Open), the file opened in SimpleText
  (the image's TeachText-class editor; a 'ttxt' file opens in whichever is there), a line `My Own Station =
  http://radio.testbed:8000/icy` typed at the top, saved, quit; one click in Aurora68 Play's window brought it to
  the front, the resume event read the file again ("read: 8 stations"), the Radio menu had the new station
  first (`docs/img/system71-piplay-stations-edited.png`), and it played.
- *Two things seen on the way, not Aurora68 Play's:* the error beep through the ASC (an image without Sound
  PiStorm) went on for one to four minutes with the Mac waiting inside it, the solo board's known ASC
  beep (item 74's "Seen on the way"); with Aurora68 Sound the same errors beeped and went on at once. And
  after the stall the card played 1.04 s of the old connection's last audio after the silence: the
  decoder waits for its 16 KB window to fill (or for the end to be said) before it decodes, so an
  underrun can begin with up to a second of 128 kbit/s still undecoded. Nothing is lost, but the buffer could be drained to the last
  frame; for later.

### FLAC, Ogg Vorbis and Apple Lossless; seeking; a device of the music server (Aurora68 Play 1.3, 22 November 2026)
The owner's music is mostly FLAC and .m4a, and Aurora68 Play 1.2 played MP3, AIFF and WAV; they also asked for
new albums to show without a restart and for the players to know of each other (docs/08 item 105 is the
record). What Aurora68 Play 1.3 adds:

- **The files.** File > Open... lists every file and Aurora68 Play reads each file's first bytes to tell what it
  is, so a file copied from a modern computer, which arrives as type `????`, plays as well as one with its
  Finder type (the bundle now has `FLAC`, `OggS` and `M4A ` documents with icons of their own, and `****`
  for the rest). FLAC (16 or 24 bits, any rate the card plays, an ID3v2 tag in front allowed), Ogg Vorbis,
  and Apple Lossless in .m4a. The title and artist come from FLAC's and Ogg's Vorbis comments ("TITLE=",
  "ARTIST=") and the .m4a's `ilst` ("©nam", "©ART"), in UTF-8, turned into Mac Roman (an e acute stays one).
  Refused in words: AAC in .m4a ("the music server converts it: play it from the Library"; no AAC decoder
  suits the card: the good free ones are GPL or LGPL and large, and AAC needs a patent licence in some
  places), Opus, FLAC in Ogg, and an MP4 file without sound; a file whose contents say nothing is judged by
  its name (`.wma`, `.opus`, `.aac`...).
- **The card decodes** (`emu68/src/psmac/psmac_codec.h`): FLAC with dr_flac 0.13.4 and Ogg Vorbis with
  stb_vorbis 1.25 (both vendored unmodified, public domain or MIT, under `third_party/`), Apple Lossless with
  a decoder written for the project (`psmac_alac.c`): Apple's own (Apache-2.0) cannot share a binary with
  the GPL-2.0-only Alice68 modules. The Mac sends what each wants (`mac/piplay/media.c`): FLAC's STREAMINFO
  alone and then the frames (a cover picture in the metadata can be megabytes); Ogg's first pages up to the
  first audio page; for .m4a the decoder's configuration and then each packet with its length, taken from
  the MP4 sample tables with the File Manager's random access (iTunes puts them at the end of the file, where
  a stream could not look). The stream block is version 2 (formats 3 to 5 and `start_ms`); on a card with an
  older kernel Aurora68 Play says the card's software is too old for these files.
- **Seeking** in the Mac's own files: a click on the progress line plays from there. FLAC goes to the last
  SEEKTABLE point before the time when the file has one, else to the byte in proportion; Ogg to the next page
  after the byte in proportion; .m4a to the packet; MP3 and PCM to the byte in proportion (a whole PCM
  frame). The card starts at the next whole FLAC frame or Ogg page and finds the real time from the stream
  (the frame's number, the page's granule position), so the position shown is right even where the guess
  was not (seen: a guess of 4.0 s landed at 4.39 s, and 4.39 s is shown).
- **New albums without a restart.** The library's generation comes with each announce answer (below) and
  in `/mac/info`'s sixth field; when it moves, the list in view is fetched again and the selected row
  selected again.
- **A device of the music server** (`mac/piplay/devices.c`, now `play/core/devices.c`; the server's side is `tools/music-server`'s
  README, "Devices"). Once a server is known Aurora68 Play announces itself every 20 seconds
  (`/mac/announce`, with an id made once and kept in its preferences and the Mac's name from Sharing
  Setup), reports what it plays when that changes (`/mac/state`: a library track by its id, a station by
  its title, a file by its tags) and waits for commands (`/mac/commands`, the server holding the request
  for up to 15 seconds: MacTCP's receive gives up after 20). The web panel's Devices list (or another player)
  then sends it play, add, next (after the playing song), pause, resume, skip, previous, seek, volume, stop,
  or an internet station; tracks play through the server's `/t/ID` as the Library's do, one after the other
  without a gap. All of it goes through the Library's one connection, one request at a time: the waiting
  request is given up when the Library window is clicked or needs a list, but only while no byte of its
  answer has come (a command the server sent in the same instant would be lost; it answers at once when one
  is queued, so the window is milliseconds wide), and the lists come first. A command with more tracks than fit in the
  answer buffer (48 KB, some 600) keeps the first ones; the queue holds 250.

**Seen in the testbed** (22 November 2026; the solo board under QEMU, System 7.1 with Riptide, Aurora68 Play driven
through the remote control's semihosting channel, the music server of `tools/music-server` on the host reached
through pinet, test files made by ffmpeg: tones, never music):
- "LR check.flac" (16-bit, 44.1 kHz) opened from the Finder: played to the end, 7.5 s, no underrun, the decoder
  at most 10 ms a slice and 6 per mille of core 1 under QEMU. "LR check.ogg", of type `????`, chosen in File >
  Open...: known by its contents, played to the end (37 per mille). "LR check.m4a", Apple Lossless at 24 bits
  and 48 kHz: played to the end with its title and artist (`docs/img/system71-play13-alac.png`), 18 per mille.
  MP3 as before (the LR check, and Ode to Joy with its LAME tag, paused and resumed).
- "Tone.flac" (40 s), a click in the middle of the progress line: "seek to 0:20", the card "starts at 20.062 s
  (the Mac asked for 20.000)", the window counted on from there and played to 0:40
  (`docs/img/system71-play13-seek.png`).
- The device: the server listed it ("Macintosh", the image's Sharing Setup having no name; classic, 1.3, online)
  with "LR check ALAC" playing. From the server's command interface: play `f:` (the three tracks of the test
  library, one after the other through `/t/ID` without a gap, the server's state following the track), pause,
  resume, skip, seek to 12 s (`/t/ID?start=12`), volume 50 (the state said 50), stop; each in the log as "a
  command from the server", within a second or two.
- A new album copied into the music folder while the Library window was open: at the next announce Aurora68
  Play logged "the library changed: its lists again" and listed "Second Artist" with "Test Artist" still
  selected (`docs/img/system71-play13-library-refresh.png`).

Not done: a "Play On" submenu to send music from the SE to another player. The server's commands name music
by folder paths (`f:`, `t:FOLDER/SONG`), which the classic Play's track ids do not give, and a 512 by 342
menu of devices is of little use while the panel has one; the SE is a player others send to.

### The shared core: one code base for both Aurora68 Plays (22 November 2026)
The owner asked for "a common code base between the two": the classic Aurora68 Play on the SE (`mac/piplay`, C
for the 68000) and the macOS one (`macos/Aurora68Play`, SwiftUI). Until then each had its own copy of the same
ideas: the classic Play's `icy.c`, `stations.c`, `devices.c` and `libparse.c` and what `library.c`, `radio.c` and
`fetch.c` knew; the macOS Play's `Address.swift`, `TrackName.swift`, `PlayQueue.swift`, `Devices.swift`,
`Models.swift`. Now everything that is not interface is one portable C core, **`play/core`** (its README has the
table of parts), compiled by both: the classic Play's `mac/Makefile` with `m68k-linux-gnu-gcc` and the kit's
flags, the macOS Play as its Swift package's C target `PlayCore`, the host tests with cc (docs/08 item 107 is the
record).

- **What is in the core:** `text.c` (bounded text, numbers with thousandths, times, %XX), `names.c` (a song's
  number and title from its file's name), `address.c` (typed addresses, a station's URL, IPv6 in brackets, hosts
  in the order Connect By names, the probe's answer), `proto.c` (every request's path and every answer of the
  server's taken apart), `catalog.c` (a page of a list as the Library window's rows), `queue.c` (the play queue,
  with the stream sent ahead), `devices.c`, `icy.c`, `stations.c`. What each Play keeps: the classic Play its
  windows and menus, MacTCP and Riptide's `.IPP6`, the card's stream channel and `media.c` (only it reads music
  files itself); the macOS Play SwiftUI, AVFoundation, URLSession, the probe's sockets and the library filter's
  folding of accents.
- **One wire format, the server's `/mac/` text.** The macOS Play used the server's JSON; it now asks for the same
  text as the SE with two parameters the SE does not send: `enc=utf8` (UTF-8 both ways; without it Mac Roman) and
  `full=1` (an album's folder and artist id, a track's path, album and folder, a device's whole state, a command's
  tracks with their paths). Two requests were added for it: `/mac/now` (the Live channel) and `/mac/command`
  (play this on a player or on Live, and Live's queue: jump, remove, clear, shuffle, repeat). The JSON is still
  served, unchanged, for the web panel. tools/music-server/README.md has the interface.
- **The rules the core keeps:** no header and no C library call (the kit has `memcpy`, `memset`, `memmove`,
  `memcmp`, `strlen` and nothing else); no allocation and no shared state (the caller's struct and buffers);
  answers taken apart in place (fields are pointers into the answer, so a long path costs a 68000 nothing it does
  not copy); arithmetic that wraps masked to 32 bits, so the shuffle and the device's id are the same on the SE
  and on a Mac; no floating point (positions in thousandths). The classic Play compiles it at `-Os` with a section
  per function and `--gc-sections`.
- **What changed for the user.** Classic Play 1.4: **Library > Repeat** (the queue from its first song again when
  it ends, gaplessly; kept in the preferences, in the byte that was padding); Previous on the first song starts it
  again (it beeped); a stream of the second or later song of a gapless run that is lost and made again resumes
  where its audio stopped (the place it started from was not reset between songs); an address typed without a
  port means the server's own, 8000 (it was 8423, the owner's server's). macOS Play 1.2: a station whose address
  is a `.pls` or `.m3u` plays its first stream; a server without radio stations still offers the built-in list;
  after the probe the address the answer came from is tried before the one the server names for itself; songs
  added after the queue has played to its end are the ones that play (it started the old queue again).

**Tested** (22 November 2026). On the host (`make -C mac check`): `tools/play_core_test.c`, 136 checks of every
part on answers written as the server writes them, and 24 more on the real server's answers, which
`tools/play_core_fixtures.py` fetches from `tools/music-server` on a library of ffmpeg-made tones, both ways (Mac
Roman with the short records, UTF-8 with the full ones; the artists in pages of two give the same list as in one
page; a track's file name tidied by the core agrees with its tag); the same 136 as a 68020 program, big-endian with
32-bit longs, compiled by the classic Play's compiler and run under qemu-m68k (`tools/play_core_m68k.sh`: a seed
gives the same id and the same shuffle as on the host); the older tests of the parts that moved (icy 92, stations
3934, devices 33 checks) and the server's own test (141 checks, 14 of them new: UTF-8, the full records, a folder
of 160 characters whole, `/mac/now`, `/mac/command` to Live and to a player). On GitHub's macOS runner: the XCTest
suite, 19 tests of the Swift wrappers and of the core's reading of the server's text (runs 45070804702 and
45071242421, both green), and the app built for Apple silicon and Intel. In the testbed (solo board, System 7.1
with Riptide, the server on the host through pinet): a FLAC file played as before; the Library listed the artist,
its album and its tracks; a track played and the next followed gaplessly; Library > Repeat on, and after the last
song the first came next in the stream; Next on the last went round to the first; from the server, play the
library, pause and resume (by `/mac/command`, the macOS Play's request), skip, seek to 12 s (`/t/ID?start=12`),
volume 50 (reported back) and stop; Radio > Open URL with the server's Live channel; an album copied in,
"the library changed: its lists again", two artists listed; Connect to Server with the field emptied: the probe
(multicast, Riptide refusing the broadcasts as before), "found the music server at 192.168.68.1", the address the
answer came from tried first; Quit kept Repeat in "Aurora68 Play Prefs".

Not verified: the macOS app on a real Mac (only built and unit-tested on the runner: the catalogue by pages, Play on
Live and on another player, a playlist station, the long poll against a live server); the classic Play on the SE;
Find and covers in the testbed (the test library has none), a library of the owner's size through the paging.

### Music videos in both Aurora68 Plays (22 November 2026)
The owner's library holds music videos beside the albums. The music server now finds them, attaches each to its
album (when it sits in an album's folder or a `Videos` folder inside one) or its artist, and sends each the cheapest
way a Mac plays it: the file as it is, a remux into MP4 with the picture copied, or a transcode to H.264 and AAC (the
last limited by `VIDEO_TRANSCODES` and refused with `VIDEO_TRANSCODE=off`), with HLS for AVFoundation, posters, and
the sound alone as MP3 (tools/music-server/README.md, "Music videos"; docs/08 item 109 is the record). Both Plays ask
for them with a third parameter on the `/mac/` text, `videos=1`, and read the `V` records with the shared core.

- **Aurora68 Play for macOS 1.3.** An album's videos follow its songs, an artist's its albums (an artist with videos
  alone is listed), each as its poster, title and length. A click plays one; right-click for Play Next and Add to
  Queue: videos and songs share the queue, and a video plays as a video, then the queue goes on. The Video window
  (AVKit: its own controls, full screen) opens by itself when a video starts, and from the film button in the bar;
  Now Playing says video and shows the poster. On Live or another player a video is `v:ID`: another Mac shows it,
  the SE and Live play its sound.
- **Aurora68 Play 1.5 (classic).** A video is a row marked "Video:" (no number) after the album's tracks, and in
  Find; an artist's row counts its videos, and an artist with videos and no album opens on them. Playing a video
  plays its sound, `/vaudio/ID`, exactly as a track plays `/t/ID` (into the card, gaplessly into what follows); an
  album's Play is still its songs. A video sent from another player or the panel plays the same way. The Play tells
  the server it shows no video.
- **Next: the picture on the SE.** The kernel's video card (docs/16) decodes MPEG-1; the server already serves a
  library video in the card's format (`/vmpeg/ID?w=&h=`, the video helper's own conversion), and the classic Play
  will use it once the card's Macintosh program (issue #24) exists. Not done here.

**Tested** (22 November 2026): the server's test, 203 checks (62 of them the videos'); the core's, 158 units on the host and as a 68020 program and 34 on the real server's answers; XCTest on GitHub's runner, 21 tests (runs 45084364767 and 45085274385); the Docker image's ffmpeg checked by the workflow; the classic Play in the testbed: not run in this session. The emulator lock was held by another session from 23:39 to 01:09, and when it came free the build machine's disk was full (1.7 MB left; the scratch copy of the test image had to be deleted) and another QEMU was already running, so no run was started. The classic Play 1.5 builds cleanly (CODE 1 170152 bytes), and its new paths go through the core that the host and 68020 tests check against the real server's answers. The session is ready to run: a copy of b-play.image with the new Play installed, the server on a library with an MP4, an MKV and a WebM video, and a script that opens an album with a video, plays the video's sound, opens an artist that has only videos, and sends a video to the SE from the server. (Run on 23 November 2026: all four passed; docs/08 item 114.)

### Music videos on the video decoder card: Aurora68 Play 1.6's video window (23 November 2026)
The classic Play 1.5 played a music video's sound only. With the Aurora68 Video Decoder card (docs/16) in the Mac,
Aurora68 Play 1.6 shows the video itself, in a window of its own, and the SE becomes a player the web panel and the
macOS Play can send videos to (docs/08 item 114 is the record).

- **Finding the card.** At start the Play opens the card's driver, ".A68Video", as Aurora68 Video does (an Open by
  name, Status 100 for the card's window, the block's magic). The log says "video: the video decoder card is there
  ... videos are shown", or that videos play their sound. Without the card everything is as in 1.5.
- **Playing a video** (a "Video:" row, an artist's videos, a `v:ID` from the panel or another player) asks the music
  server for `/vmpeg/ID?w=W&h=H` (MPEG-1 video and MP2 sound in an MPEG program stream) at a size of the video's own
  shape that suits the screen the player window is on: 320 x 240 at most on the SE's 512 x 342 screen and on any
  screen up to 640 wide (a 16:9 video is 320 x 180), 512 x 384 below 1280, 640 x 480 on a larger HDMI screen
  (`a68_vmpeg_size` in the shared core), and at a rate the network can carry: 400 kbit/s of video at 320 x 240
  (`a68_vmpeg_rate`, the server's `rate=`; its own rate, 2,500 kbit/s there, came too slowly in the testbed). The
  stream comes over the same connection as songs (MacTCP, or Riptide's `.IPP6`), received at interrupt time as radio
  and songs are, but poured into the video card's 512 KB ring; the card decodes it on a Pi core, draws every picture into the screen at its depth (dithered to one bit on the SE's own
  screen, in colour on HDMI) and mixes its sound with the Mac's.
- **The window** (Human Interface Guidelines, 1992): a document window with the video's title and a close box,
  exactly the size of the picture, black until the first picture comes. It opens below the player window when the
  screen has room (a 640 x 480 screen does), otherwise at the top of the screen, and where the last one was if that
  is still on a screen. A click in it plays or pauses; so does the space bar while it is in front. Its close box
  hides the picture and the sound goes on; Controls > Show Video brings it back, as does the next video. The card
  is told not to draw while the window is covered by another window (a corner covered hides the whole picture: the
  card draws rectangles, not regions), while it or another window is dragged or sized, while a menu is down, while a dialog is up and while
  another program is in front; the card draws the last picture again when the window is uncovered or moved while the
  video is paused.
- **The controls** are the player's: Play/Pause, Stop, Previous and Next, the volume (the card's volume follows Play's
  slider and the Sound control panel), and a click on the progress line seeks: a new `/vmpeg` request with
  `start=S`, the card stopped and a new stream started. A seek or a volume from the server works the same way. Now
  Playing shows the video's title, "Video N of M" and "MPEG-1 320x180".
- **The queue goes on** when a video has played (the card says "ended"): the next song or video starts as a stream
  of its own. A video is never sent gaplessly after a song or another video, since each is a stream of its own for
  the card.
- **A device that shows video.** The announce says `video=1` when the card is there and the server makes MPEG-1
  (`mpeg1` among its capabilities; an older server's videos play their sound), so the server sends a video
  as a video (the panel's devices list marks the SE as showing video, and the server no longer answers
  `audio_only`).

**Tested** (23 November 2026, the solo board in QEMU, System 7.1 with Riptide, the music server on the host through
pinet, ffmpeg-made test videos of 20 and 40 seconds): a video row played in its window, dithered at one bit on a
640 x 480 screen and in 256 colours at 512 x 384 on a 1024 x 768 one; pause, a seek from the progress line and one
from the server, the window dragged, covered by the Library and uncovered, a menu held over it, closed and shown
again; the end of a video followed by the album's next video, and by a song added from the server after a video it
had sent (`v:ID`). The testbed's network carried 420-750 kbit/s of the stream (QEMU and pinet's ring files, not the
Play's receive chain: a TCP buffer twice as large carried no more); the 40-second film at 300 kbit/s kept time (43
s), every picture shown, 1 to 2 ms a picture to decode under QEMU. The numbers and what was found on the way are
docs/08 item 114; the screenshots stay in the scratchpad.

### Aurora68 Play: classic and macOS (Aurora68 Play 1.7, 23 November 2026)
The owner asked: "lets get the classic mac version up to parity. Also have it automatically discover server hosts".
Below is every feature that a user sees in the macOS Aurora68 Play 1.5 (macos/Aurora68Play/README.md), with what the
classic Play 1.7 does: whether it already had the feature ("had"), gained it in 1.7 ("1.7") or leaves it out on purpose
("left out", with the reason). The reference machine is the SE: 512 x 342 pixels at one bit, a 68000-class Mac,
MacTCP or Riptide, the card decoding. docs/08 item 120 is the record.

| macOS Play 1.5 | Classic Play 1.7 |
|---|---|
| Library: artists, albums as a grid of covers, an album with its cover and songs | had: Artists, an artist's albums with 32-pixel covers, an album's songs under its 64-pixel cover (a list: a grid of covers would show eight on the SE's screen) |
| The search field filters artists and albums as you type (Command-F) | had in its own form: Find... (Command-F; the server searches artists, albums, songs and videos) and typing in a list selects by name. Left out: filtering at each letter, which would be one request over MacTCP for every key |
| The original files played by AVFoundation (MP3, AAC, Apple Lossless, FLAC), real seeking, no gap between songs | had: the server sends MP3 (`/t/ID`), the card decodes it; no gap between songs; a seek is a new request from that second |
| Play, Shuffle | had |
| Play Next, Add to Queue (right-click a song or an album) | 1.7: Library > Play Next and Add to Queue, for the selected song, video, album or artist; with nothing of the library playing, it plays |
| The queue (Up Next in the bar's menu) | 1.7: Library > Up Next, the queue as a list, the one playing marked; Return or a double-click plays from there |
| Previous, Next, the position bar (click or drag), volume | had (Previous and Next moved to the Controls menu in 1.7, the arrows as before); dragging left out: every place is a new stream |
| Repeat (all, one) | Repeat had (1.4); 1.7: Repeat One |
| The video in the window: the bar's picture expands it to the whole window and shrinks it back; Escape shrinks it | 1.7: Controls > Normal Size, Double Size, Fill Screen (Command-1, 2, 3) and the video window's zoom box (Normal and Fill Screen); the stream stays the size asked, the card scales it. Escape stays Stop, as in every Aurora68 Play |
| The Video window of its own, AVKit's controls, full screen | had (1.6: the card's window, the player's controls); Fill Screen is the nearest to full screen (the menu bar stays) |
| Videos after an album's songs and an artist's albums, each as a poster | had after an album's songs (1.5); 1.7: an artist's view begins with a line "Videos" (with their count) that opens the artist's videos. Posters left out: each is a picture to fetch over MacTCP, and at 32 pixels and one bit a frame of a video says little; the film mark stands for them |
| Videos and songs share the queue; a video plays as a video, then the queue goes on | had (1.6) |
| Now Playing says it is a video and shows its poster | had: "Video 2 of 3 - MPEG-1 320x180"; the album's cover stays (no poster, as above) |
| A song with a video has a film button; Play Video in its menu | 1.7: a film mark in the song's row; Library > Play Video (Command-M) plays the selected song's video and the album's videos after it |
| Play Videos Instead of Songs (Settings, Controls menu) | 1.7: Controls > Play Videos Instead of Songs, kept in the preferences; with the video decoder card (without it, or with a server that makes no MPEG-1, the songs play) |
| An artist's page: its videos as a row of posters, See All | 1.7: the "Videos" line above the albums, which opens them all |
| A film mark after the subtitle of artists and albums with videos | 1.7: the film mark beside "2 videos" in the artist's and the album's row |
| Videos in the sidebar: every video by artist, filtered by the search field | 1.7: Library > Videos, every video grouped by artist (the artist's name once, at the head of its group); typing selects by title; Find finds videos |
| Albums in disc and track order | had: the classic Play shows the server's order and never sorts (the server's fix of item 115) |
| New albums without a restart | had (1.3) |
| Now Playing in Control Center, the keyboard's media keys | left out: System 6 and 7 have neither; the player window is Now Playing |
| Space plays and pauses; Command-Left and Command-Right are Previous and Next | had: space, and the left and right arrows (without Command: the Menu Manager of System 7 takes no arrow as a command key) |
| Play on Live (the SE), Listen here | Listen Live had (Library > Listen Live). Sending the queue to Live left out, as below |
| Play on another Mac or on the SE; the bar shows what the other player plays | left out: the SE is the player the others send to (it has been one since 1.3, and shows videos since 1.6); sending from it needs the songs' paths of the long records and a bar kept current by asking the server about the other player, over the one MacTCP connection that the long poll for commands already uses. The web panel and the macOS Play do the sending |
| Stations: Everything shuffled, each artist, the internet radio stations | the radio stations had (the Radio menu, the same list as the server's); an artist shuffled had (Shuffle, then Play in an artist's view). Everything left out: a library is more than the queue's 250 songs, and Live plays Everything already |
| Live's queue and its listeners; the players on the network | left out: the web panel shows them; the SE would have to poll for them |
| Another player or the web panel plays music on this one | had (1.3) |
| Finding the server: the probe at the start, the first that answers; an address typed in Settings | 1.7: the probe at the first start and whenever the remembered server does not answer, with the Library window shown or not; every server that answers within a second of the first is kept; the remembered one (by name, so a new address does not matter) or the only one is taken; with several and none remembered the Library window lists them and Library > Servers offers them. Servers > Search Again; Servers > Connect to Server... takes an address as before |
| IPv6 first or IPv4 first, the other after four seconds | had (Connect By) |

**What 1.7 does, in more words.**
- **Finding the server.** At the start, if the preferences name no server, Aurora68 Play sends the probe at once, with or
  without the Library window (the device announces itself as soon as one answers). The probe goes three ways as
  before (255.255.255.255, the network's broadcast address, the group 239.255.68.70); every answer within a second of
  the first is kept (four seconds when none comes), each server (the address it answered from and its port) once. One
  server: it is taken. Several: the one chosen before, by its name, is taken; if none of them is, the Library window
  says "Music servers on this network: Test Library (192.168.68.1:18523), Second Server (192.168.68.1:18524). Choose
  one in Library > Servers." The remembered server that does not answer (after its other addresses) brings the probe
  too: the list area says "The music server at 192.168.1.20:8423 does not answer. Looking for music servers on the
  network..." and the server found is taken, or the words say that none answered (Reload tries the remembered one
  again, Search Again looks again). The address stays in the preferences while it does not answer.
- **Library > Servers** (a submenu): the servers the last probe found, as "Name (address:port)", the one in use
  checked (listed first if the probe did not find it); Search Again; Connect to Server... (moved here from the
  Library menu: the menu had grown past the SE's 342 lines). Choosing a server connects to it, the views start again at
  Artists, and its name is kept in the preferences.
- **Videos linked to songs.** An artist's albums and an album's tracks are asked for with `full=1&videos=1`, still in Mac
  Roman: the longer records end with an album's count of videos and a song's video (the server's tenth field of item
  115). The core reads records by their fields, so an older server's records read as "no video", and the server
  answers the classic Play's other requests as before; the older classic Play sends neither parameter and gets what it
  always got. An answer too long for the 48 KB buffer with the longer records (a box set of a few hundred songs) is
  asked again without them.
- **The video window's sizes.** Normal Size is the picture as the server sends it (320 x 240 at most on the SE's
  screen); Double Size twice that, or Fill Screen where twice would not fit; Fill Screen the largest of the picture's
  shape on the screen the window is on, below the menu bar (on the SE's screen a 16:9 picture is 508 x 285, a 4:3 one
  400 x 300). The zoom box toggles Normal and Fill Screen; the size is kept in the preferences and used for the next
  video. Back to Normal Size the window goes where it was.
- **The menus.** Library: Show Library, Videos, Up Next, Find..., Play All, Play Video, Play Next, Add to Queue, Shuffle,
  Repeat, Repeat One, Listen Live, Reload, Servers, Connect By (nineteen lines: 304 pixels below the SE's menu bar).
  Controls: Play, Stop, Previous Track, Next Track, Volume Up and Down, Show Video, Normal Size, Double Size, Fill
  Screen, Play Videos Instead of Songs.
- **The preferences** keep, after the record of 1.3 to 1.6, a tail with the server's name, Play Videos Instead of Songs
  and the video window's size. The file is still version 3, and an older Play reads the part it knows and leaves the
  tail out when it writes the file again.
- **Memory.** The program asks for 640 KB (it asked for 512): the code is 18 KB larger, and the rows and the queue's
  items hold a song's video.
- The About box and the version the Play announces to the server say 1.7.

**Tested** (23 November 2026, the solo board in QEMU, System 7.1 with Riptide, the music server on the host through
pinet, two test libraries shaped like the owner's: `Albums/<Artist>/<Album>/<Title>.flac` with the track numbers only in
the tags, and `Albums/videos/Artist - Title [123456789].mp4`, tones and FFmpeg's test pictures; docs/08 item 120 has the
details):
- **Discovery.** No preferences, one server: at the start, before the Library window was shown, "found the music server
  "Test Library" at 192.168.68.1:18523" and the server's answer; Riptide refused both broadcasts (error -23033, as in
  item 107) and the multicast probe was answered. Two servers (pinet relaying the probe to both): Search Again found both
  ("2 music servers answered"), the one in use stayed checked, choosing the other connected to it and Quit kept it
  (`docs/img/system71-512x342-play17-servers.png`). That one stopped and the Play started again: "the server does not
  answer", the probe, "the remembered server did not answer: taking the one found, Test Library". No preferences and two
  servers: the Library window opened by itself naming both (`docs/img/system71-play17-two-servers.png`), one probe only;
  choosing one from Library > Servers connected to it.
- **Links and videos.** An album named by title: its songs in track order, the film mark on the two songs with a video,
  "4 tracks, 2 videos" with the mark on the album's row, "1 album, 2 videos" on the artist's, the artist's view opening
  on its "Videos" line (`docs/img/system71-512x342-play17-film-marks.png`). Play Video on "One More Time" played its video
  (`/vmpeg/1739616b`), then the album's other video. Play Videos Instead of Songs: the album played as video, song,
  video, song, Up Next showing that queue. Library > Videos grouped by artist, with the two downloads of one video listed
  once (the server's doing, item 117).
- **The video window.** Normal, Double (Fill Screen for a 320 x 240 picture on 640 x 480, where twice would not fit) and
  Fill Screen, by the keys and the zoom box, on 640 x 480 and on 512 x 342 (`mac.screen=512x342` with the 512 x 342
  declaration ROM: 508 x 285 for 16:9, `docs/img/system71-512x342-play17-fill-screen.png`); the size kept for the next
  video and across a Quit.
- **Queue.** Add to Queue, Play Next, Up Next, Repeat One (the song again, gaplessly).
- **An older server** (the one from before item 115): no film marks, no album counts, the album in that server's own
  (title) order, a video played; nothing failed.
- **Found and fixed on the way:** a song put in with Play Next or Add to Queue after the last song's stream had come
  whole was never played (the queue's end moved on only into videos); the Library window came back at a place kept from
  a 640 x 480 screen with only its left end on the 512 x 342 one; the card drew the video over Standard File's dialog
  (since 1.6; File > Open... now holds the picture, not tried in the testbed).
- **Seen once, not again:** in the first session the Mac stopped 4 seconds into the second video of a queue, in the
  network's interrupt path (the ROM's level-2 handler and system-heap code, "slot-9 interrupt not serviced in 500 ms"),
  code 1.7 did not change; after a restart nine more video streams, with five hand-overs from a video to what
  followed, ran without it. Not explained.

Not verified: the SE itself (MAME's SE board was not run; the 512 x 342 screen was the solo board's); a real MacTCP
(Apple's) sending the broadcast probe; two servers on two machines (the testbed's two were on one host, both answering
from the peer's address); a box set too long for the links (the fallback without them has not run); Standard File's
hold.

### Aurora68 Play 1.8: windows that resize, the artist in the titles, IPv6 first, videos in the background (23 November 2026)
The owner's feedback from the real SE (Play 1.7, a 2560 x 1440 HDMI screen in colour, a music video playing through
the card): "Resizable windows"; "Add the artist name into the window also"; "I need to be able to connect to server and
have it prefer its IPv6 address then its IPv4"; "Video in the window stops playing if it isn't focused. This should work
in the background. Audio keeps working though"; and "it also locked up when I was moving through the list of videos
using the scroll bar". docs/08 item 124 is the record.

- **The video window has a size box.** It sits in a strip of 15 pixels below the picture (the card would draw over a
  size box inside the picture), with the time ("0:46 of 3:49") at its left. Dragging it shows the usual outline, the card
  not drawing meanwhile; let go, and the window's content snaps to the picture of the video's own shape nearest to the
  outline (by the side dragged further, for its length), 160 pixels wide at least and the screen's Fill Screen at most;
  the card is told the new place (Control 112) and draws the next picture there (a paused one at once). That size is a
  fourth one beside Normal, Double and Fill Screen: the next video opens at the same width, and it is kept in the
  preferences. The zoom box toggles Fill Screen and whichever size the window had (Normal, Double or the one the size
  box gave), and back from Fill Screen the window goes where it was. Normal Size, Double Size and Fill Screen stay in the
  Controls menu (Command-1, 2, 3); none is checked at a size of the size box.
- **The player window and the Library window** were resizable already (since 1.1 and 1.2: a size box and a zoom box,
  the smallest 340 x 106 and 360 x 200, both well inside the SE's 512 x 342; the player's title, its second line and its
  progress line widen with it, the Library's list gives the extra width to the titles and the time stays at the right).
  New in 1.8: their places and sizes are kept for each arrangement of screens they are used on (the desktop's bounding
  box: the SE's screen alone, the SE's with an HDMI screen beside it, at whatever HDMI size), the four latest, so a
  window put on the HDMI screen comes back there when the HDMI screen is, and on the SE's screen when it is not; a
  place on no screen now is still not taken (both ends of the title bar on a screen, as 1.7 checked).
- **The artist in the titles.** The video window's title is "Artist — Title" ("The Warning — Kerosene"). Now Playing's
  second line is the artist, then the album; the Library's Now Playing strip "Artist — 0:46 of 3:49". An artist with no
  letter or digit ("_", a downloader's "nobody", which the server now replaces where it can, docs/08 item 121) is shown
  as no artist there (the album alone, or the time alone), and as "Unknown artist" where a name has to stand: the head
  of a group in Library > Videos, an artist's row.
- **IPv6 first, with a fallback.** Why the SE said "IPv4": 1.7 kept only the address that had last answered, so a
  server first reached at its IPv4 address (typed in Connect to Server..., or found by a probe whose answer named no
  IPv6 address) was asked at that address for good; and the log never said which addresses the probe's answer named.
  1.8 keeps the server's addresses by family (from the probe's answer, in the preferences), tries them in the order
  Connect By names (IPv6 First, the default; IPv4 First; IPv4 Only, new), five seconds each while another remains (an
  IPv6 address Riptide has no neighbour for fails in about three), and keeps the one that answered for the session:
  the lists, the covers, the device's requests and every stream go there without waiting again. A list or a cover whose
  connection fails moves to the other address and is asked again. A remembered server reached over IPv4 while IPv6 is
  wanted and its IPv6 address is not known, or its remembered IPv6 address did not answer: the probe is sent once,
  quietly, and if the server's answer names another IPv6 address the Play moves to it (and back to IPv4 if it does not
  answer). Changing Connect By moves to the family named
  first. Connect to Server... takes an IPv6 address with or without brackets and a port, an IPv4 address or a name
  (Riptide's driver looks a name up, AAAA and A, in Connect By's order, and each address is tried in turn). Without
  Riptide's .IPP6 (Apple's MacTCP) everything is IPv4, as before. The log says the addresses the probe's answer names,
  the order they are tried in, each one that does not answer and why, and the one used from then on; Now Playing still
  ends with the family of the stream.
- **Videos in the background.** 1.6 and 1.7 told the card not to draw whenever another program was in front, and
  whenever any window covered a corner of the picture. 1.8 hands the card the window's visible region (its `visRgn`,
  which the Window Manager keeps current for every window, another program's in front or not); the card reads it at
  every picture and draws only where the window shows (docs/video-decoder-api.md, "The window's visible region"). So
  the picture goes on with the Finder in front, behind a window of another program, partly under the Library window.
  It still stops while a menu of Play's is down, while a window of Play's is dragged or sized, while a dialog of Play's
  is up (each drawn over the screen, outside any window's region), and while any program's menu is down (the card looks
  at the menu bar's saved bits itself, MBSaveLoc: a program in the background gets no time while another tracks a menu).
  Aurora68 Video 1.1 does the same with its film window. This needs the kernel8.img of the same release (the card's
  block version 3); with an older one the Play keeps 1.7's rule.
- **Buffering.** With the card's buffer in the Pi (docs/08 item 125, block version 2 and later) Now Playing says
  "Buffering 45%, 1.3 s" while the card buffers, from the card's own figures.
- **The lock-up while scrolling.** Not seen in the testbed (below); weak points were made safe on the way. While the
  Library's scroll arrow is held, the Control Manager keeps the event loop from running, and a video's stream that had
  stopped for room in the card's ring stayed stopped; the scroll bar's action procedure now starts it again. And the
  event loop's "a receive completed without its completion routine" check, which calls the completion itself, could
  run it a second time at once if the real one came between its test and its start, and a late real completion read the
  next receive's "in progress" as an error: the test and set now happen with interrupts off, and a call for a receive
  still in progress does nothing. The scroll bar also no longer draws itself again at every step while it is tracked.
  A pass of the event loop that took three seconds or more is logged with the stream's state ("the event loop was away
  12.4 s; video stream, waiting for room, ..."), so a lock-up on the SE leaves what went before it in Aurora68 Log and
  the kernel's log.
- The About box, the version announced to the server and the User-Agent say 1.8; Aurora68 Video says 1.1.

**Tested** (23 November 2026, the solo board in QEMU, System 7.1 with Riptide, the music server on the host through pinet,
over IPv4 and IPv6 (pinet's fd68::1), a test library of 46 videos by ten artists, one of them in `Albums/_/_/` with "_"
as its artist):
- **IPv6.** No preferences: the probe's answer logged as "it names IPv4 192.168.68.1, IPv6 fd68::1", the order "fd68::1
  first, then 192.168.68.1", the server answering at fd68::1 and the videos' streams coming from `[fd68::1]:18523`. A
  server naming an IPv6 address that does not answer (fd68::99, Library > Servers > Search Again): "no answer from
  fd68::99 ...: trying 192.168.68.1", the lists and streams over IPv4 from then on. The next start with those
  preferences tried the IPv6 address first again, and with the server back at fd68::1: fd68::99 did not answer, IPv4,
  the quiet probe ("the server also has the IPv6 address fd68::1"), and the server over IPv6 again. In both long sessions Riptide itself dropped its fd68:: address after
  four or five minutes (its mDNS announcement then named only the link-local address, and no router solicitation
  followed); the library moved to IPv4 by itself, the stream that was playing reconnected (docs/08 item 124 has what
  this may mean for the SE).
- **The video window.** Its size box dragged at 640 x 480 (a 468 x 263 picture, `docs/img/system71-play18-size-box.png`)
  and at 512 x 342 (`mac.screen=512x342` with the 512 x 342 declaration ROM: 294 x 165,
  `docs/img/system71-512x342-play18-size-box.png`), the shape kept, the size kept for the next video and across a Quit.
- **The background.** With the Finder in front the video went on (`docs/img/system71-play18-background-finder.png`: a
  Finder window over part of it, the card drawing around it, the heartbeat saying "region parts 3"); with the Library
  window over part of it, the parts that show (`docs/img/system71-play18-library-over-video.png`); with the Finder's
  Apple menu held down over the picture for eight seconds, the menu left alone and the picture held (`docs/img/system71-play18-menu-over-video.png`; "192 under a menu" in the heartbeat), and going on when the menu closed.
- **The titles.** "Long Tones — Ninety Seconds" on the video window; "Unknown artist" at the head of the "_" group in
  Library > Videos and in the Artists list (`docs/img/system71-play18-videos-unknown-artist.png`), and that video's
  window titled "Kerosene", the Library's strip "Kerosene" over "0:06 of 0:40".
- **Places.** The windows put back at 640 x 480 from the arrangement kept for it after a session at 512 x 342 in between
  (the Library window at 49,25-592,406 again, after 5,25-508,338 on the small screen; "the windows' places kept for this arrangement of screens").
- **Scrolling.** With "Ninety Seconds" playing and its window partly under the Library window: the down arrow held 12
  seconds, the up arrow 12, 24 clicks in the page areas, three drags of the thumb, the arrow held 8 more: nothing locked
  up, the picture went on, and the log said "the event loop was away 12.2 s; video stream, a receive out, 512 KB free in
  the ring, last bytes 0 s ago".
- **Found and fixed on the way:** a new video window opened at Double Size (1.7) or at the size box's size went to the
  screen's top left corner (the window's region is empty until it is shown, and the size was worked out from it); and
  the card's first way of reading the Mac's memory, through the 68k's MMU tables, stopped the kernel during a start-up
  (reading a garbage handle while the tables were rebuilt): it now reads RAM directly, inside its size, aligned.

### Aurora68 Play 1.8.1: no network call that waits for ever, the intake bounded, videos sized for the HDMI screen (23 November 2026)
Two lock-ups on the owner's SE after minutes of video (Play 1.8, Riptide over WiFi, the video window on the HDMI screen):
the pointer moved, windows and keys did nothing, the kernel's heartbeat had the 68k in the ROM's wait for a synchronous
Device Manager call. docs/08 item 127 is the record: the call was the Library connection's TCPAbort, which Riptide had
lost (fixed in Riptide the same day: update both).

- **No synchronous network call.** Every call Play makes to `.IPP6` or `.IPP` (TCPCreate, TCPAbort, TCPRelease, the
  probe's UDPCreate, UDPWrite, ipctlGetAddr, UDPBfrReturn and UDPRelease, ip6StrToAddr, ip6ResolveCancel) is made
  asynchronously and waited for five seconds at most, the stream's receive chain kept going meanwhile (`net_call`,
  radio.c). One that has not come back is logged ("net: library: TCPAbort did not complete in 5 s: the stream left to
  the driver") and its parameter block, with the buffers the driver may still write into, is left to the driver
  (`net_leave`): new ones take their place, the old ones are given back once the driver has finished with them, and a
  stream left so is released then ("net: a stream left to the driver is released now"). An open or a send the driver
  never answers ends after 45 seconds (the radio) or 30 (the Library), a receive 30 seconds past its own timeout, and
  the stream is asked again from where it was: a lost call no longer stands a video still for ever. The parameter
  blocks come from the heap; the receive chain's completion routine is handed its block and passes over one that is
  no longer its own. ip6AddrToStr, which the log's addresses used, is not called any more: Play writes an IPv6
  address itself (RFC 5952). The video card's driver, which the kernel answers at once, is still called synchronously.
- **The video's intake rests.** Since the card keeps up to 8 MB in the Pi (docs/08 item 125) a video's stream came at
  the network's full rate until 8 MB were held (4.7 MB/s on the owner's WiFi for a stream of 40 KB/s), with Riptide's
  socket buffers full ("memory pressure") and its deferred task running at every frame; both lock-ups came in such a
  burst. Now the receive chain rests while the card holds more than 20 seconds of the stream and goes on once it holds
  less than 10 (the first three times logged: "the card holds 20 s of the video: the intake rests until it holds 10
  s"), never while the card is buffering and never with an older card. After the first 20 seconds the intake follows
  the film's own rate, in bursts of about 10 seconds' worth, and 10 to 20 seconds stay in hand for the network's gaps.
- **Videos sized for the HDMI screen.** Until 1.8 a video was asked for at a size for the screen the player window was
  on (320 x 240 on any screen up to 640 wide), so with the player on the SE's screen a 720p video came at 320 x 212
  even in a large video window on the 2560 x 1440 HDMI screen. Now, when the video window is on the HDMI screen (slot
  $A's), the stream fits the window's picture, in the video's shape, at most 960 x 540 (what the card decodes and
  draws at 25 pictures a second on a Pi 3B: 640 x 426 already took about 6 ms to decode and 10 to draw) and at least
  320 wide. When the window grows (its size box, Double Size, Fill Screen, or dragged onto the HDMI screen) so that it
  wants a stream at least half as wide again, the stream is asked again at the larger size from the picture shown, as
  a seek is: the window stays, the sound starts again there; the same size must be wanted a second apart (not in the
  middle of a drag), and four seconds pass between two. Smaller, or a little larger, and the card scales what it has.
  The window's own sizes (Normal, Double, the size box's) stay those of the size it opened at. On the SE's screen
  nothing changes. **Controls > Video Quality** (a submenu): Automatic (the above), 320, 480, 640 or 960 wide, which
  apply on the HDMI screen (the SE's keeps 320); a choice that changes the size asks again at once. Kept in the
  preferences, in a third tail marked PL81 after 1.8's (1.8 and 1.7 still read their parts). The rate stays w x h /
  192 kbit/s (960 x 540: 2,700, which the server holds at 2,500), and Now Playing says the stream's size ("MPEG-1
  960x540"). The larger the stream, the more the intake's rest above matters.
- The About box, the version announced to the server and the User-Agent say 1.8.1.

**Tested** (23 November 2026, the solo board in QEMU, System 7.1, the music server on the host through pinet, a queue
of two 4-minute videos sent by the server's device commands, an "add" to the queue every four seconds for the Library's
traffic; docs/08 item 127 has the runs):
- **The freeze, before and after.** With Riptide as on the SE (mac-port) and the card's buffer at 1 MB
  (`mac.vbuffer=1`, so that the receive chain stops for room as the SE's did with its 8 MB full), Play 1.8 froze in both
  tries, after 112 and 57 s of video, the heartbeat at 4080bb8c with the owner's registers and "the Mac waits for a
  driver: .IPP6 (ioRefNum -56) csCode 39". With Riptide's fix, Play 1.8 played the first video to its end with the same
  buffer, its ring full the whole time, and went on into the second (stopped after a minute of it). With the old Riptide and a scratch program that makes the driver lose the next call, Play 1.8 froze;
  Play 1.8.1 did not, and when the call lost was the video's receive: "a receive did not come back in 30 s: the
  connection made again", "a call was still out after TCPRelease: its block left to the driver", the video again from
  182 s ("reconnected"). The five-second wait of a call that never comes back did not come up in these runs (the lost
  calls were receives).
- **The intake.** Play 1.8, 8 MB buffer: 7.4 MB in the first 50 s (1,194 kbit/s, two and a half times the stream's
  rate, as fast as the testbed carries; Riptide said "memory pressure, 171 KB" 27 times in a video). Play 1.8.1: the
  card held 20 s after about 40 s, then the intake rested and went on in turn ("rest 1", "rest 2" ...), 10 to 20 s in
  hand, and the whole video came as 15,730 KB in 241 s, 533 kbit/s, the stream's own rate.
- **The HDMI sizes** were tested on the host (tools/play_core_test.c: a68_vmpeg_hdmi and a68_vmpeg_bigger, nine
  cases); the solo board has no slot $A screen, so there the video window keeps today's 320 x 240, as on the SE's
  screen. The size box, Double Size and Fill Screen on the owner's HDMI screen, and Video Quality's choices, are for
  the SE.

## The WiFi in the testbed: a radio that exists only in software
QEMU's raspi3b has no WiFi chip (no SDIO function, no firmware to load), so the real driver in
`alice_net.c` can only report "off" there, and until 16 November the WiFi unit, Aurora68 Wi-Fi and the
`.Network_WiFi_PiStorm` driver had never run against anything that answered. Everything above the
chip is worth testing all the same, and the Alice68 board protocol makes that easy: the Mac only
ever sees unit 0's state word, message, command box, scan table and frame rings. So the service
core got a simulated radio (`psmac_wifi_testbed_poll` in `emu68/src/alice/alice_psmac.c`), switched
on by `mac.wifisim=SSID:PASSWORD` (or `mac.wifisim=1` for the defaults, Aurora68 / pistorm68):
- it starts, a second later it is idle, exactly the states the real driver walks through;
- a scan (AN_CMD_SCAN) takes two seconds and finds four networks: the one named on the command
  line (WPA2, channel 6, -47 dBm), "Macintosh Garden" (WPA2, whose password nobody knows),
  "LINKSYS" (WEP) and "Free Hotspot" (open), with BSSIDs, so Aurora68 Wi-Fi's list has something to show;
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
`mac.inputtest=`): with Aurora68 Wi-Fi as the startup application the script presses S, waits for the scan
list, presses J and types the network's name and password at Aurora68 Wi-Fi's prompts
(`docs/img/system71-piwifi-scan.png`, `docs/img/system71-piwifi-joined.png`). For MacTCP over the
WiFi the card is built with the WiFi as its only network sResource (`mac/build/psmac_wifi.declrom`,
`mkdeclrom.py --wifi` without `--enet`): MacTCP's Ethernet link layer takes the first Ethernet-type
sResource it finds in the slot, so with the Ethernet sResource absent it opens
`.Network_WiFi_PiStorm`, and Aurora68 Ping's echoes go through unit 0, the simulated radio and the
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

**What the stick can hold.** System 7 reads HFS ("Mac OS Standard") only. The stick can be:
- formatted HFS on a Mac (Mac OS 9 or earlier, or Disk Utility up to 10.5): the Mac uses it as it is;
- a disk image written to it (`dd`), bare HFS or with an Apple partition map, like the card's images;
- initialized on the Mac itself (below);
- **FAT32 or exFAT, as Windows, Linux or a modern Mac format it (since 21 November 2026, below).**

NTFS, HFS Plus ("Mac OS Extended"), APFS and blank sticks stay on the Pi, and the log says why. That way
the Finder never offers to erase a PC's stick.

### FAT32 and exFAT sticks, and the card's exchange folder (21 November 2026)
A stick formatted FAT32 ("MS-DOS (FAT)") or exFAT, with a Master Boot Record or a GUID partition map, or
none, is used two ways at once (docs/08 item 97):
- **Its disk images** (`.image`, `.dsk`, `.hda`, `.img`, `.hfv` in the stick's root, bare HFS or
  Apple-partitioned) appear on the desktop as disks of their own, writable, exactly like the card's.
  The Mac writes into the image file where it lies; the stick's FAT is never changed.
- **Its files** appear as one more disk, named after the stick ("USB STICK"), **locked**: a real HFS volume
  the Pi makes of the stick's folders, so the Finder, Standard File and every program open them like any
  disk's. A Mac file copied to the stick on a modern Mac keeps its resource fork, type, creator and Finder
  flags in the `._name` file macOS writes beside it, so a classic application copied there runs; the
  `._` files, `.DS_Store` and other dot files are not shown. Names are converted to Mac Roman; a
  character Mac Roman does not have is shown as `?`, a name longer than 31 characters keeps its
  extension behind an ellipsis ("Quarterly report for the y….doc"), and two names that would then be
  the same get " #2", " #3". Disk images shown on their own are not repeated in it.
  The disk is locked because the Pi does not write FAT yet (the issue's stage 3): copy files *from* it.
  At most 32,000 files and folders, and 2 GB (System 7.1's largest HFS volume): with more data than that,
  the largest files are left out and the log says how many.

Making it takes a moment after the stick is plugged in (core 1 does it between its other work; in the
testbed a quarter of a second for ten thousand files, longer from a real stick). The first time the Finder
sees it, it builds a desktop database in memory ("Rebuilding the desktop file", a minute for ten thousand
files in the testbed), as for a CD-ROM. The log says when it is ready (the testbed's stick, with 10,000 files in one folder):
```
[USB] disk 0: a GPT partition table, first partition EFI system (FAT), second Microsoft basic data (FAT, exFAT or NTFS)
[USB] disk 0: exFAT in a GPT partition at block 43008, 27088 clusters of 4 KB
[USB] disk 0: disk image Floppy.image (1 MB, 1 piece) holds the HFS volume "Stick Floppy": offered to the Mac, writable
[USB] disk 0: its files are being read, to be shown to the Mac as a locked HFS volume
[USB] disk 0: ready as the HFS volume "USB STICK": 10002 files, 1 folders, 1 of them with Mac information from AppleDouble files; 12834 blocks (6 MB), allocation blocks of 512 bytes; locked; 2582 KB of memory; made in 241 ms (238 reads)
```
Eject each of the stick's disks (drag to the Trash); "safe to unplug" comes when the last one is out.
`mac.usbview=off` shows only the disk images; `mac.usbdisk=all` offers the whole stick raw, as before.

**The exchange folder on the SD card.** A folder named **Mac Exchange** in the card's root appears on
the desktop the same way, as a locked disk of that name (`docs/img/system71-exchange-window.png`; the
names of its International folder: `docs/img/system71-exchange-names.png`). Copy files into it on any computer (with their
`._` files from a modern Mac) and start the Pi. `mac.exchange=NAME` uses another folder, `mac.exchange=off`
none. It takes one of PiDisk's four units, which the card's disk images, the exchange folder and the
sticks' disks share.

**Formatting a stick on a modern Mac.** Disk Utility, the stick, Erase: Format "ExFAT" (or "MS-DOS (FAT)"),
Scheme "Master Boot Record" or "GUID Partition Map". Copy files and applications to it in the Finder;
macOS adds the `._` files by itself.

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

**Log lines to send back** (Aurora68 Log with the filter `disk`, or the serial log). In the testbed, with
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

## The Aurora68 Video Decoder in QuickTime (22 November 2026)
The video decoder card (slot $C, docs/16) decodes QuickTime's movie formats of the 1990s on a Pi core, for
MoviePlayer and every other QuickTime program, with nothing outside the Mac and the Pi. Two pieces on the
Mac (docs/08 item 110 is the record):

- **The card's driver, `.A68Video`** (`mac/a68video.s`), in the card's own declaration ROM: the Slot
  Manager installs and opens it at start-up (`[MAC] A68Video: the Aurora68 Video Decoder card's driver is
  open`). Its Control and Status calls are the card's programming interface for any program
  (`docs/video-decoder-api.md`); Aurora68 Slots lists the card as "Aurora68 Video Decoder".
- **The "Aurora68 Video Decoder" extension** (`mac/vdec`, made by `tools/mac/elf2thng.py`): a file of type
  'thng' in the Extensions folder, which QuickTime 2.0 and later registers by itself at start-up. It holds
  four decompressor components, for Cinepak ('cvid'), Apple Video ('rpza'), Animation ('rle ') and Graphics
  ('smc '), manufacturer 'A68v' (and since 23 November 2026 a fifth, 'mpg1', with the MPEG-1 import: below,
  "MPEG-1 films in MoviePlayer"). QuickTime picks them before its own (they are registered after its built-in
  ones). For each movie a component opens a session on the card, then hands it every picture with the
  destination (the window's screen, or QuickTime's offscreen buffer when the window is covered or the movie
  is scaled); the card decodes and draws it at the destination's depth, only the part that changed. The
  component's log lines: `[MAC] A68 Video Decoder: Cinepak 320x240: decoded by the card into a 1-bit pixel
  map` when a movie starts, and when it is closed how many pictures came, in how long, and the time a picture
  took inside the decompressor.
- **Off, and without the card:** `mac.vdecode=off` on the Pi's command line (or the driver's Control 101)
  switches frame decoding off; each component then hands everything to QuickTime's own decompressor of the
  same type, which it opened beside itself (`... frame decoding is switched off: QuickTime's own decompressor
  takes it`). Where `.A68Video` does not open (no card, `mac.video=off`, another kind of Mac) the components
  decline registration and QuickTime never sees them. The same happens for a picture the card cannot take:
  a destination depth it does not draw, a picture larger than 1024 x 768, one loaded in pieces.
- **Installing:** `tools/mac/aurora68-layout.py install` puts it into the Extensions folder (System 6:
  the System Folder) with the other extensions; it is in the release's "Extensions (to install)". QuickTime
  2.5 itself is the owner's (docs/08 item 91).

**Seen in the testbed** (solo board, System 7.1 with QuickTime 2.5.1 and MoviePlayer 2.5, the components
from the Extensions folder; test movies made by FFmpeg, 320 x 240, a test pattern or moving boxes):
QuickTime chose the card's components for every format (`... Cinepak 320x240: decoded by the card into a
1-bit pixel map`); Cinepak and Apple Video played to the end at one bit and Cinepak at eight bits (after
Aurora68 Depth 8), each picture drawn by the card into the window on the screen, the colour test pattern
in the System's 256 colours; with `mac.vdecode=off` the same movies played through QuickTime's own
decompressors, handed over by the components. The times a picture took, card against QuickTime's own, are in
docs/16 ("Measured in the testbed"): at one bit QuickTime's own decompressors, which Emu68 runs as ARM code,
were faster in the testbed; Cinepak at eight bits was 1.7 times faster through the card. On MAME's SE the
card drew MoviePlayer's pictures into the SE's own screen, dithered. What to try on the SE and with HDMI:
READ-ME-FIRST, "QuickTime movies on the video decoder card".

## MPEG-1 films in MoviePlayer (23 November 2026)
With the Aurora68 Video Decoder extension (from 23 November 2026) MoviePlayer and every QuickTime program open
MPEG-1 films (.mpg, MPEG-1 video with MP2 sound; types 'MPEG' and 'MPG '): in MoviePlayer, File > Open..., the
film, then the button that says "Convert" (it opens the film where it is, with nothing copied and no question
where to save). The film is then an ordinary QuickTime movie: play, pause, the slider, Play All Frames, copy and
paste, Save As (a small movie that refers to the film, or self-contained). The card decodes the pictures, drawn
at the screen's depth, and decoded the sound once, the first time the film was opened or selected in the Open
dialog (a few seconds), into a file in the System Folder's Preferences: "Aurora68 Video Sounds", 10 MB a minute
of film, the latest nine films kept (delete the folder to make room; it is made again). A double-click in the
Finder plays the film in Aurora68 Video (MoviePlayer 2.5 refuses files that are not movies when the Finder hands
them over). Without the card, or with its decoding switched off, the film opens with a line in place of the
picture saying so, and no sound. Its log lines: `[MAC] A68 Video Decoder: MPEG-1 import: film.mpg (program
stream): 320x240 at 25.00 fps, 200 pictures (17 I, 51 P, 132 B), 200 samples (17 sync)` and a line for the
sound. How it works: docs/16, "MPEG-1 in QuickTime"; the record: docs/08 item 113.

## Aurora68 Video: MPEG-1 films on the card (23 November 2026)
In `Aurora68:Aurora68 Video`. File > Open... an MPEG-1 program stream (.mpg: MPEG-1 video with MP2 sound, what
the video helper and `ffmpeg -c:v mpeg1video -c:a mp2 -f mpeg` make), or double-click one in the Finder (type
'MPEG' or 'MPG '; a file from a modern computer that arrives as '????' opens through File > Open...). The card
decodes it into the window; the program reads the file into the card's ring and draws nothing in the picture.
Space or the button plays and pauses, a click in the bar seeks, Movie > Double Size doubles the picture (the
card scales it), Movie's volumes, and Card > Decode QuickTime Movies on the Card switches the QuickTime
components between the card and QuickTime's own decompressors (docs/08 items 110 and 112; docs/16).


The user asked how to see which NuBus cards the Mac has. On the SE the Pi presents two cards that
exist only in software (slot 9, the Aurora68 card; slot $A, the HDMI screen), and the IIci ROM's Slot
Manager treats them like real ones. System 7.1 has no Apple System Profiler and the project gives away
nothing it did not write, so Aurora68 Slots (`mac/pislots/pislots.c`, `Aurora68-Slots.bin`) is the project's own.
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
as tall as the report. The window's place and size are kept in "Aurora68 Slots Prefs" (type 'pref', the
Preferences folder through `FindFolder` on System 7, the System Folder through `SysEnvirons` on System
6) and put back only while a screen still shows its title bar. Menus: Apple (About Aurora68 Slots…), File
(Close ⌘W, Save As…, Look Again ⌘R, Quit ⌘Q), Edit (Undo, Cut, Copy, Paste, Clear, Select All, with the
usual keys; only Copy and Select All are Aurora68 Slots' own, the rest are there for desk accessories and dimmed
otherwise). Copy copies the selection, or the whole report when nothing is selected. Save As… writes a
TeachText document, "Aurora68 Slots 2026-11-21" by default. The About box and the alert for a failed save are
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
Aurora68 Log show what the Mac saw (the kernel takes 127 characters of a line, so a long card takes two):
```
[MAC] A68 Slots: slot 9: PiDisk open, Network_EtherNet_PiStorm closed, PiInput open, Network_WiFi_PiStorm closed
[MAC] A68 Slots: slot 9, more: Display_Video_PiStorm open
[MAC] A68 Slots: slot 9 "Aurora68"; empty: $A $B $C $D $E
```

**On the SE board** (MAME's `macse_ps`, 32 MiB, the System 7.1 image as the startup disk,
`psmac-se-1024x768.declrom` with `mac.screen=1024x768`, Aurora68 Screen Setup in Extensions; 21 November 2026), saved
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
  Card:        Aurora68
  Board ID:    $5053 'PS'
  Made by:     Aurora68 project
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
  Card:        Aurora68 HDMI
  Board ID:    $5048 'PH'
  Made by:     Aurora68 project
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
System 6.0.8 on the solo board (the user's System 6 volume, Aurora68 Slots as the startup application, 8 MiB and
`mac.xpram=8a:00` so that the ROM starts in 24-bit mode instead of asking, which `.PiInput` is not yet
open to answer): the same list, resize, About, the File menu and Quit, "Aurora68 Slots Prefs" in the System
Folder.
*Not verified:* real NuBus cards (on a IIci or SE/30: other vendors' declaration ROMs, a card whose start
failed), a real SE, System 6 on the SE board, the Balloon Help that is not there.

## Aurora68 PC: the PC card's PC in a window (23 November 2026)
In `Aurora68:Aurora68 PC` (`mac/a68pc/pc.c`, C with the `mac/c` kit; the card's driver `.A68PC` is `mac/a68pc.s`, in
its declaration ROM). It needs a kernel built with the PC card (`-DPSMAC_PC=ON`) and `pc=on`; without the card its
window says so in two lines. The window shows the PC (Aurora86: Bochs with Libreboot's firmware) on the Aurora68 PC
Card in slot $D: the card draws the PC's screen into the Mac's frame buffer itself, only where the window shows, and
the program draws nothing in it but the grow box. Text is black on white in a 6 x 12 font on the SE's screen (80 x 25
in 480 x 300, which fits with the menu bar and the window's title), 6 x 13 on a taller one-bit screen, the PC's own font
and colours on a deeper one; graphics are scaled to the window and dithered (View > Actual Size: 1:1).
- **PC**: Power On, Power Off, Reset, Send Control-Alt-Delete, Pause. The program powers the PC on when it starts.
- **Disks**: the images the card has, attached ones checked (`pc.disk0=` chooses one; attaching from the menu comes
  later).
- **View**: Scaled to the Window, Actual Size, Large Text (8 x 16), White on Black (the PC's own sense).
- **Keyboard**: Capture Keyboard and Mouse (Control-Option-Command-P). A click in the window captures too; the same key,
  a Command-click or another window in front gives the keyboard and mouse back to the Mac. The title says which
  ("Aurora68 PC: keyboard and mouse to the PC").
The zoom box sizes the window to the PC's picture (the card's natural size); the grow box resizes it, and graphics
follow. Key releases reach the program because it adds keyUpMask to SysEvtMask while it runs; the mouse's movement is
read from the window's middle with the pointer hidden. docs/19-pc-card.md has the card, the block and the options;
docs/08 item 132 the history.

## Aurora68 Status 2.0: the Pi's heat and speed as gauges (23 November 2026)

**Why.** The owner's Pi 3B, in the SE's 68000 socket, played music videos for long stretches and got hot: the
firmware capped the ARM's clock and the 68k fell from 169 to 31 MIPS (the heartbeat's `JIT: ... MIPS` line). Only
the kernel's log showed it. He asked: "might be good to get this on the Mac Screen, CPU util, temp etc. Gauges in a
specific app window". Aurora68 Status was a text window in assembly (`mac/pistatus.s`, PiStatus before 21
November); 2.0 is a new program in C with the `mac/c` kit (`mac/pistatus/status.c` for the window, `model.c` for
the numbers), under the same name, creator `PIST` and icon, in the same folder (Aurora68:System). docs/08 item 129
is the record.

**What the window shows** (`docs/img/system71-status-gauges.png` in colour at 640 x 480,
`docs/img/system71-512x342-status-gauges.png` on a one-bit 512 x 342 screen), from the status page the kernel's core
3 fills once a second (`alice_status.h`, below):

- **Three dials** in the style of the 1984-92 Mac: a round face, a scale over 240 degrees, a needle, the value in
  the gap at the bottom. *Temperature*, 30 to 90 °C, the band from 60 °C (the firmware's soft limit on a 3B+,
  `temp_soft_limit`) warm and from 80 °C (where a Pi 3 caps its clock) hot: amber and red in colour, grey and black
  on one bit. *ARM clock*, in MHz, a short thick tick at the nominal clock (the firmware's maximum); the measured
  clock where the firmware gives it, else the one it set. *68k speed*, in MIPS, from the JIT's instruction counter
  over the last second; the scale is 50, 100, 200, 300... above the highest seen.
- **Power and heat**: four lights for the firmware's GET_THROTTLED bits, *now* (red, or black) and *since boot*
  (amber, or grey): under-voltage, frequency capped, throttled, soft temperature limit, and a line in words
  ("Normal: full speed.", "Slowed down by the firmware.", "Under-voltage: check the power.", "Normal now; slowed
  down earlier."). The firmware's word comes from the kernel's power watch (psmac_power.c, every two seconds).
- **Strip charts** of the temperature (the 60 and 80 °C lines dotted), the clock (the nominal clock dotted) and the
  MIPS: a point every two seconds, five minutes across in the normal size, more when the window is made larger
  (the history holds 17 minutes). The newest value is at the top right of each.
- **ARM cores**: each core's load over the last second as a bar, with what it runs (from the page): 0 the 68k's JIT
  (it never idles, so its bar is its speed against the MIPS dial's scale, "always running: 169.3 MIPS"), 1 USB,
  the sound output and the video decoder card (the time not spent waiting in `wfe`), 2 the Mac's bus (the time of
  the turns that served the 68k a bus cycle), 3 the services (Alice68's own figure: the loop's time beyond its
  shortest turn).
- **Four lines**: the Pi's memory (the kernel's heap free of its size, and the JIT's translation cache); the network
  (each unit's link and its bytes in and out a second, from the counters the page always had); the video card
  while it plays (its share of core 1, a picture's decoding and drawing, the seconds buffered, the late pictures,
  the times it ran dry); the sound card (the output, whether the Mac's own sound is playing, frames short; the
  music stream's state, its decoder's share of core 1).

0xFFFFFFFF on the page is "not measured", and the window says so: the dial has no needle and reads "not measured",
the lights are hollow and the line says "The firmware does not report it." (`docs/img/system71-status-not-measured.png`,
the testbed with `mac.sensors=off`). QEMU 8.2's Pi answers the clock rates (700 MHz, the maximum the same) and a
temperature that never moves (25.0 °C, limit 99), but neither the measured clock nor GET_THROTTLED; `mac.sensorsim=on`
(testbed only) puts made-up values on the page instead, a Pi 3B heating and being capped, and the window says
"simulated".

**The compact view** (`docs/img/system71-512x342-status-compact.png`): 232 by 50 pixels, the temperature, the clock and the
MIPS in bold with a bar each (the temperature's with the 60 and 80 °C marks), and one light with a few words
("normal, full speed", "frequency capped now", "normal now, slowed earlier"). View > Compact View (⌘2) and Normal
View (⌘1), or the zoom box, which switches between the two; each view keeps its own place.

**The window and the menus** (the Human Interface Guidelines of 1992, as the other programs follow them): a
document window with a close box, the zoom box and, in the normal view, a grow box (the charts take the extra
room); its place, size and view kept in "Aurora68 Status Prefs" (type 'pref', creator 'PIST'; the Preferences
folder on System 7, the System Folder on System 6), put back only while a screen still shows its title bar. Apple
(About Aurora68 Status…), File (Close ⌘W, Quit ⌘Q), Edit (Undo to Clear for desk accessories; Copy puts every
number on the Clipboard as text, the build string and uptime first), View (Normal View ⌘1, Compact View ⌘2,
Redraw ⌘R). As in 1.0, R redraws and Q quits without Command. The normal view is 488 by 292 and fits the SE's
screen under the menu bar. Drawn on a colour screen (four bits or more, found with GetMaxDevice on the window's
place) in colour in a colour window; on one bit with patterns. Moved from one screen to the other, it draws for the
new one.

**Low cost on the 68k.** It looks at the page twice a second and copies it (512 bytes) only when `seq` has moved and
is even; then only what changed is drawn again: a dial whose needle moved a degree or whose text changed, the lights
when the word changed, the cores' bars when a load changed by a percent, a line of text when it differs, and the
charts by one pixel to the left with ScrollRect when the window is unobscured on a one-bit screen (on a colour
screen, and when another window covers part of it, the plot is drawn whole: 150 short lines). No offscreen GWorld.

**The status page's new fields** (`emu68/src/alice/alice_status.h`, shared with the Amiga's readers). Its rule is
that new fields go at the end and `size` says how far the page goes, the version changing only if a field's
meaning does: so the page grew from 0x1a0 to 0x200 bytes, version 1 still. A reader looks at `size` before reading
past 0x1a0 (`model.c` does; an older kernel's page shows the new numbers as not measured). Added: `arm_max_hz`
(GET_MAX_CLOCK_RATE), `arm_measured_hz` (GET_CLOCK_RATE_MEASURED), `temp_max_mc` (GET_MAX_TEMPERATURE),
`insn_rate` (68k instructions in the last second), `heap_free`, `heap_total`, the video card's `vcard_state`,
`vcard_load`, `vcard_decode_us`, `vcard_draw_us`, `vcard_buffer_ms`, `vcard_underruns`, `vcard_shown`,
`vcard_dropped`, the sound card's `snd_out`, `snd_playing`, `snd_underruns`, its stream's `str_state`, `str_load`,
`str_underruns`; roles 5-7 (media, bus, service) for the Macintosh's cores and the flag `ALICE68_STF_SIMULATED`.
The Amiga's build fills them with 0xFFFFFFFF. `alice_psmac_status.c` is the Macintosh half: the cores' names
("68k JIT", "USB, sound, video card", "Mac bus", "services") and loads, the 68k's rate, the heap, the cards, and
the firmware's sensors through the mailbox, read before the page is opened for writing so that a slow answer never
leaves `seq` odd. On a Pi 3 the temperature comes from the mailbox's GET_TEMPERATURE (the BCM2711 register Alice68
reads is not there); a tag the firmware does not answer is asked once and logged (`[PSMAC] status: the firmware does
not give the measured ARM clock (tag 30047: not answered)`), and the first full reading is logged too. Three
mailbox calls a second, through `alice_mbox_once`: under the mailbox's lock, never while the WiFi has it.
`mac.sensors=off` stops them.

**The remote control.** `aurora68.py state` (GET /state, docs/13) has a `pi` object with the same numbers:
`temperature_c`, `temperature_limit_c`, `arm_mhz`, `arm_measured_mhz`, `arm_max_mhz`, `mips_68k`, `core_load`
(four percentages, null where not measured), `heap_free_kb`, `simulated`; `null` for a value the firmware does
not give.

**Tested.** The numbers on the host (`tools/pistatus_test.c`, in `make -C mac check`): every offset of `model.h`
against the kernel's header; a QEMU page (not measured everywhere it should be), a hot Pi 3B's (81.2 °C of 85,
600 of 1200 MHz, 31 MIPS, frequency capped and throttled and the soft limit now, capping since boot), an older
kernel's page of 0x1a0 bytes, pages that are not pages; the rates from two readings, the history every two
seconds, its wrap and a Pi that restarts; the dials' angles and the scales.
The same tests as a 68020 program under qemu-m68k, compiled as the Mac program is (`tools/pistatus_m68k.sh`, also
in `make -C mac check`), with the needle's arithmetic at a negative angle. In the testbed (solo board, System 7.1,
Aurora68 Status in Startup Items, driven through `mac.remotefile=` with `aurora68.py --semihost`): at 640 x 480 one
bit and, after Aurora68 Depth 8, eight bits (`docs/img/system71-status-gauges.png`, `mac.sensorsim=on`: 69 °C on
its way down, the soft-limit light on now, capping and throttling since boot amber, the clock chart's step from
1200 to 600 MHz and back); at 512 x 342 one bit with `mac.screen=512x342` and the 512 x 342 declaration ROM
(`docs/img/system71-512x342-status-gauges.png`: 83.5 °C, 600 MHz, capped and throttled now, the window exactly
the screen's width under the menu bar); the zoom box to the compact view and back, ⌘1 and ⌘2, the View menu with
its check mark, the About box and the window drawn again after it, ⌘Q with "Aurora68 Status Prefs" written (24
bytes) and the window back at its place at the next start; without `mac.sensorsim` QEMU's 25.0 °C and 700 MHz with
"The firmware does not report it." and the measured clock's tag logged as not answered; `mac.sensors=off`, not
measured everywhere (`docs/img/system71-status-not-measured.png`). The 68k's MIPS on the page agree with the
heartbeat's (6 to 9 under QEMU). `aurora68.py state`'s `pi` object in each case.
*Not verified:* everything the firmware of a real Pi says (the temperature moving, the measured clock, the
throttled bits, under-voltage), so the owner's SE during a long video is the test; the core loads on hardware (under
QEMU `wfe` returns at once, so core 1 reads busy, 87%, and the solo board has no bus, so core 2 reads 0%); the video
and sound card lines while they work (QEMU had no sound output and no video played); System 6; a Mac in 24-bit mode
(the page is then read at $950000); the grow box dragged (the window's size kept across a Quit is the same code as
its place).

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
the same numbers.) MacTCP's Ethernet link layer then finds the card the way Aurora68 Network does and opens
`.ENET` through OpenSlot. `Aurora68 Ping` proves the stack: it opens `.IPP`, prints the address MacTCP
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
  clipped to `CrsrPin` ($834): the ROM's cursor task (`JCrsrTask`, $8EE; IIci ROM $4082DF94) moves
  `RawMouse` by the accelerated difference from `MTemp`, pins it, and moves the cursor onto another
  active screen of the device list ($8A8) that holds a point past the edge, setting `CrsrDevice`
  ($89C) and `CrsrPin` for it (docs/10, "The edge jump"). The keyboard layout is the `'KCHR'` 0 resource fetched, locked
  and address-stripped at Open (the driver opens in 24-bit mode and uses the pointer in 32-bit
  mode), read directly by the driver: the low-memory long at $1B40, which MPW's SysEqu.a calls
  `KCHRPtr`, holds the address of a code patch under 7.1 on this ROM (docs/08, item 22).
