# Virtual cards: what else the Pi could be for the Mac

21 November 2026. A survey written for the owner's question "what virtual cards could or should be
worked on?". Each proposal has an issue on GitHub (farsonic/Aurora68) with its findings, how it would
be tested and what "done" means (issues #11 to #18); this document is the overview and the reasoning between them.

## What exists

The Pi already appears to the Mac as hardware that was never built:

| Card | Slot | What it does | Where |
|---|---|---|---|
| The Aurora68 card | $9 | mailbox, PiDisk (disk images, USB sticks), Ethernet and WiFi (`.ENET`), input, the kernel's log, the Aurora68 Agent's block | `psmb.h`, docs/07 |
| The second screen | $9 (main board) or $A (SE board) | a colour display on HDMI at any size up to 2560 x 1440 | docs/10 |
| The sound card | $9 | 16-bit stereo at 44.1 kHz through the Sound Manager, MP3/AIFF/WAV decoding for Aurora68 Play | docs/08 items 74, 26 |

Slot $9's 1 MiB window is nearly full: the second screen's frame buffer takes the top half, and the log,
sound rings, network ring, agent buffer and the second declaration ROM share the rest. **Slots $B to $E
are free.** The IIci ROM's Slot Manager scans every slot from $9 to $E, and a declaration ROM served from
Pi memory costs nothing, so a new card can have a slot and a window of its own rather than squeezing
into $9. That also keeps each card's driver, declaration ROM and kernel code separate, so a card can be
switched off (`mac.<card>=off`) without touching the others.

The Pi 3B has four cores. Core 0 runs the 68k (the JIT), core 1 USB and sound, core 2 the bus service,
core 3 the card, network and PiDisk. A card that does real computing (decoding video, synthesising
music) needs time on core 1 or 3 between their duties, the way MP3 decoding already shares core 1.

There is no operating system under the kernel. Anything a card needs has to be written for it or brought
in as plain portable C with a compatible licence, as minimp3 was. That rules out Ghostscript, ffmpeg or a
Linux network stack, and it is the main thing that decides how big each proposal is.

## The proposals, in the order I would do them

### 1. Network time (small) — issue #11
**What:** the Pi asks a time server (SNTP over its own network stack, `psmac_ip.c`) and sets the Mac's
clock at start-up and once a day; the time zone comes from `cmdline.txt` (`mac.tz=`) or the Date & Time
control panel's Map setting.
**Why first:** the SE's clock battery is usually dead, the SE board already has a stand-in clock
(`xp_secs` in `psmac_board_se.c`), and **TLS needs the right date** to check certificates, so the
Alice68 TLS card depends on it.
**How:** kernel-side only. The clock chip, or its stand-in, answers the network time. No Mac software
is needed.

### 2. TLS offload (the Alice68 worker, in progress) — issue #12
Being built elsewhere. What Aurora68 has to settle with it: the Mac-side interface (a MacTCP-style
driver like Riptide's `.IPP6`, so that one API serves both Riptide and Apple's MacTCP), where the
certificate store lives (the SD card), and network time (above). See its issue.

### 3. Video offload: a QuickTime codec card (large, the most visible) — issue #13
**What:** QuickTime's own extension point. QuickTime finds decompressors through the Component Manager
by the compression type of each video track (`'imdc'` components: `ImageCodecPreDecompress`,
`ImageCodecBandDecompress`). Hardware video cards of the time added decoders exactly this way, and every
QuickTime program (MoviePlayer, Director, games) then uses them without being changed. Its flags allow
decoding asynchronously (`codecCanAsync`) and on hardware with its own timing (scheduled decompression).
**On Aurora68:** a small 68k `'imdc'` component that hands each compressed frame through the card's
window to a Pi core, which decodes it. The decoded picture goes either into the destination (the Mac's
screen or an offscreen buffer, at the Mac's depth) or, on the HDMI screen, straight to the frame buffer.
**Formats:** Cinepak and Apple Video first (the 68k era's own movies, which a 68000 at 8 MHz cannot play
smoothly but the JIT and the Pi can), then JPEG (QuickTime's Photo codec). MPEG-1 second: QuickTime 2.5
on a 68k has no MPEG at all (Apple's MPEG extension needs a PowerPC), so MPEG would need its own codec
component and a media path for its audio. The decoders must be written or ported as plain C (Cinepak's
format is documented; public-domain MPEG-1 decoders such as pl_mpeg exist).
**Later:** a helper on the LAN (a PC or a Linux Pi) that turns modern video into MPEG-1 or Cinepak for
the Mac. That is task #34's "YouTube" idea, and it belongs outside the kernel.

### 4. A virtual modem (medium) — issue #14
**What:** a "Hayes modem" that dials telnet addresses: `ATDT bbs.example.org:23` from ZTerm or any terminal
program connects over the Pi's network. It is the most-asked-for retro Mac feature, for BBSes.
**How:** a serial port that is not the SCC. The Communications Resource Manager is the System's own way
for a card to add serial ports (a `CRMSerialRecord` with input and output driver names). NuBus serial
cards registered their ports that way, and terminal programs list them beside the modem and printer
ports. The driver passes bytes through a ring in the card's window, and the kernel speaks telnet (or raw
TCP) and the AT command set.
**Bonus:** the same driver with a TCP listener lets a PC's terminal talk to the Mac's serial-port
software.

### 5. CD-ROM images (small) — issue #15
**What:** mount Mac CD-ROM images (`.iso`, `.toast`, `.cdr`, `.img`) read-only from the SD card or a USB
stick. Most Mac CD-ROMs are HFS, or hybrids with an Apple partition map, and PiDisk already finds the
`Apple_HFS` partition in an image (`hfs_partition` in `psmac_mbox.c`). So this may largely work already;
it needs the file types recognised, the volume marked locked, and a test with real hybrid discs.
Audio tracks and ISO 9660-only discs are out of scope (the Mac would need Foreign File Access for those).

### 6. A General MIDI synthesiser (medium) — issue #16
**What:** QuickTime's music (Musical Instruments, MIDI files, game music) played by a SoundFont
synthesiser on the Pi instead of the 68k's small built-in synthesiser. The music comes out through the
existing sound card.
**How:** a QuickTime music component on the Mac (the note allocator's synthesiser interface) that sends
note events through the window. On the Pi, a portable SoundFont synthesiser (TinySoundFont is MIT
licensed and plain C) runs beside the MP3 decoder on core 1. A General MIDI bank on the SD card (needs a
redistributable one).

### 7. Printing to a modern printer (large) — issue #17
**What:** "Print" on the Mac reaches a printer on today's network.
**Findings:** the LaserWriter driver produces PostScript and sends it with AppleTalk's PAP; modern
printers speak IPP, raw TCP (port 9100) or LPR, and many still accept PostScript. The Pi is the Mac's
network card, so it sees the Mac's AppleTalk frames. It could answer as a LaserWriter (the AppleTalk
NBP name lookup, then the PAP session) and pass the PostScript to a printer over TCP. Turning PostScript
into PDF on the Pi itself is out of reach without an operating system, so the target is a printer that
takes PostScript, or a PC that does the conversion.
**Size:** AppleTalk's NBP, ATP and PAP on the kernel side; nothing new on the Mac.

### 8. QuickDraw acceleration (measure first) — issue #18
Accelerated video cards of the time patched QuickDraw (CopyBits, fills, text) to run on their own
processor. Here the 68k is already a JIT on a 1.2 GHz ARM, so the gain is far from certain; the
expensive case is large second screens (2560 x 1440 at 8 bits). **Measure before building:**
Speedometer's graphics tests and window dragging on the second screen at its largest size. Only if
QuickDraw is the bottleneck would a card that blits on a Pi core be worth it.

## Smaller things that are not cards

- **Clipboard sharing** through the Aurora68 Agent: `aurora68.py clip get|put`, text first, then
  pictures (PICT to PNG). It needs no new hardware, only the agent and the remote.
- **A Pi-side RAM disk** is not needed: the Mac can already have up to 1 GiB of RAM.

## What I would not do

A floating-point coprocessor (the JIT already gives the 68040's FPU), a second CPU card (the Mac OS of
this era runs on one processor), and general compression offload such as StuffIt (no standard interface
for the programs to use).
