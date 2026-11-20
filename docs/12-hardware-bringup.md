# Hardware bring-up: a classic PiStorm, a Pi 3 and the Macintosh SE

*17 November 2026.* The user has a classic PiStorm board on order (the DIP-64 board with the
Altera EPM240 CPLD, the one made for the Amiga 500 and 2000: Melbourne Console Reproductions'
"PiStorm to suit Amiga 500, 2000", assembled, tested, "pre-programmed" with the stock CPLD
firmware, with a 3D-printed cover, and specified for a Raspberry Pi 3A+), a Raspberry Pi 3B v1.2 and
a 64-pin DIP socket for the SE's 68000 position. The 3B is a full-size board where the shop
expects a 3A+: same header, same signals, the Pi simply overhangs the PiStorm, which inside an
SE means finding it a place to lie; 1 GB of memory, so the Macintosh gets its 128 MB with room
over. WiFi comes first among the Pi-side peripherals. This document
is the plan for taking Aurora68 from the testbed to that hardware, and the record of what was
found about it.

## The board and the socket

The classic PiStorm is a 68000 in a DIP-64 footprint: the CPLD drives the 68000's bus signals
under the Pi's direction through the Pi's GPIO header. A 68000 socket is a 68000 socket, so the
board fits the SE's electrically. The SE's VIA is a 6522, a 6800-family part, addressed with the
68000's E clock, /VPA and /VMA; the Amiga's two CIAs are addressed the same way, so the classic
CPLD already generates those cycles. The SE has no second bus master on the 68000 bus (its video
and sound fetches are the BBU's own cycles, interleaved with the CPU's, not bus requests), so the
bus-arbitration additions in the DirectSocket firmware (below) do not come into play.

The questions that remain are mechanical and documented by nobody: whether the board, on a
socket, on an SE logic board, clears the chassis; which way the Pi header faces relative to the
analog board; and whether a Pi 3 fits: the board is laid out for the 3A+'s footprint (the shop
specifies a 3A+), a full-size 3B overhangs it, and the 3D-printed cover adds height that may have
to come off inside the SE. The 2022 Tinker
Different thread has one builder closing the case and another finding an extended socket too tall.
The user's SE will answer this. Most SEs have the 68000 soldered, hence the socket.

## CPLD firmware

Three choices exist, all for the same EPM240:

1. **The stock classic PiStorm firmware** (what this board ships with, already flashed). AlexCL's 2022
   port ran a Musashi 68000 on an SE with firmware close to this: internal video, sound and
   floppy reads worked; writes and SCSI did not, for reasons on the software side. This is the
   firmware Emu68's classic transport (`emu68/src/pistorm/ps_classic_protocol.c`) was written
   against, and it is where bring-up starts.
2. **Frimples' MacSE-PiStorm-DirectSocket** (github.com/Frimples/MacSE-PiStorm-DirectSocket):
   RTL that keeps the stock transaction and peripheral paths and adds /BR-/BG-/BGACK handling with
   the control lines released during another master's ownership. Simulated, built, never flashed
   or booted by its author. Its separate "experimental E-clock timing" release changes when a
   peripheral (VIA) cycle is considered complete, and one user booted an SE from floppy with it
   under Frimples' emulator. If the VIA misbehaves on stock firmware (keyboard, mouse, clock,
   interrupts all go through it), that build is the candidate, and it speaks the same GPIO
   protocol, so the kernel need not change.
3. **A firmware of our own**, only if the SE's /DTACK or interrupt-acknowledge timing turns out
   to need it. Not planned.

Flashing is JTAG from the Pi's GPIO with `pistormflash` and an SVF; keep the stock SVF to recover.

## The Pi 3, and what it changes

The testbed has been a Pi 3 all along: QEMU's `raspi3b`, peripherals at 0x3F000000, the Arasan
SDHCI at 0x3F300000. A real Pi 3 is therefore closer to what has been tested than a Pi 4 would
be. What differs from the Pi 4 the Alice68 modules were written for:

| Piece | Pi 3B | Pi 3B+ / 3A+ (Zero 2 W: see below) | Notes |
|---|---|---|---|
| WiFi chip | BCM43438 (id 43430), 2.4 GHz | BCM43455, dual band | same Arasan SDIO host as the Pi 4; the kernel picks the firmware by chip id (`alice_net.c`: 43455, 43456 and now 43430 name tables). **The 43438 is a different family inside**: an ARM Cortex-M3 with SOCRAM where the 43455 has a Cortex-R4 with tightly-coupled memory banks, and `wifi/chip.c` only knows the R4 (RAM at 0x198000, bank enumeration, CR4 halt and release). The CM3 path from Linux's brcmfmac (`brcmf_chip_cm3_*`, SOCRAM at 0, bank sizes from the SOCRAM core) is a day's work and is item 4a below |
| WiFi firmware files | `brcmfmac43430-sdio.bin`, `brcmfmac43430-sdio.txt` | `cyfmac43455-sdio-standard.bin` (or `brcmfmac43455-sdio.bin`), `brcmfmac43455-sdio.txt`, `.clm_blob` | from RPi-Distro/firmware-nonfree, as the README's curl lines |
| Bluetooth | BCM43438 over UART, `BCM43430A1.hcd` | BCM4345C0 over UART, `BCM4345C0.hcd` | the module now looks for either file by name (`bt_pi.c`). The Pi 3 has one PL011; Bluetooth sits on it unless `dtoverlay=miniuart-bt`. The kernel's log and the Bluetooth module must agree on which UART is whose (open) |
| Ethernet | LAN9514 over USB (100 Mbit) | LAN7515 over USB on the 3B+, none on the 3A+ | no GENET; the Alice68 GENET module does not apply. The WiFi is the network until a DWC2 USB stack exists |
| USB host | DWC2 (OTG) | DWC2 | not the Pi 4's VL805 xHCI. Keyboards and mice use Alice68's native DWC2 HID host (docs/08 item 40; tested in QEMU; split transactions behind the LAN9514 still to be tested on the board). The SE's own ADB keyboard and mouse work regardless |
| Memory | 1 GB | 1 GB / 512 MB | the Macintosh gets its full 128 MB on all three (on 512 MB since 20 November: docs/08 item 60) |
| Second screen | HDMI, VideoCore IV | same | as tested |

One more row the table above hides: **the radio's power switch**. On the 3B, 3B+, 3A+, 4 and
CM4, WL_ON is line 1 of the VideoCore's GPIO expander, driven through the mailbox. On the Zero W
and Zero 2 W it is the SoC's own GPIO 41 (the 3B's GPIO 41 is the audio PWM). The chip's
32.768 kHz sleep clock comes from GPCLK2 on GPIO 43 on all of them, set up by start.elf. The
kernel reads the board type from the revision code and takes the right path (`alice_net.c`,
`wl_on()`). The pin pulls for the SD host differ between the BCM2837 (GPPUD/GPPUDCLK) and the
BCM2711 (`sdio.c`, `sdh_pins()`): on the 3B the old code left DAT3 pulled down, which turns
CMD0 into an SPI-mode entry and silences the SDIO card. The first 3B run found both, and a
mailbox race between the display and the WiFi that blacked the screen (docs/08, items 30-31).

**The 3A+ and the Zero 2 W (20 November).** The user's 3B does not fit the classic board, which is
laid out for the 3A+. Both 512 MB boards were gone through for the SE (docs/08 item 60): the 3A+
has no device tree of its own (the firmware passes it the 3B+'s, so the kernel now reads the
board type from the revision code), one USB-A port on the DWC2 root port, the BCM43455 and
BCM4345C0, no Ethernet; the Zero 2 W (RP3A0: a BCM2837 at 1 GHz) has the BCM43436 or 43436s and
BCM43430B0, WL_ON on GPIO 41 and BT_ON on GPIO 42, CTS/RTS to the chip on GPIO 30/31, one micro-USB
OTG port, mini-HDMI. Both run the core clock at 400 MHz like the 3B, so the GPIO transport's Pi 3
timings and GPCLK0's PLLC/6 apply to both. The 3A+ boots to the Finder in QEMU's `raspi3ap` with
USB input behind a hub (`docs/img/solo-pi3aplus-usb-apple-menu.png`); QEMU has no Zero 2 W.

**Status, 17 November:** on a 3B v1.2 the chip is found, loads its firmware and reports its MAC
address (docs/08, items 30-33). Three things stood between the Pi 4 code and the 3B: the pin
pulls go through different registers, the firmware leaves the SD card slot on the same Arasan
host (GPIO 48-53 must be detached), and the mailbox needed a lock between the display and the
WiFi. A 3B+ or 3A+ has not been tried yet.

So, for WiFi first: the Alice68 WiFi module over SDIO is expected to work on a 3B+/3A+ with the
files already in the release notes, and on a 3B with the 43430 pair, which the kernel now looks
for. Bluetooth needs the UART question settled; USB input needs a new driver.

## The transport: from the bridge to the GPIO

In the testbed every motherboard access leaves `psmac_bus.c` as a bridge transfer to MAME over a
UART or semihosting (`bridge_xfer`). The same call now has a second back end,
`emu68/src/psmac/psmac_gpio.c` (written 17 November, 03:40, untested on hardware): the classic
PiStorm's CPLD protocol as Emu68's `ps_classic_protocol.c` does it, with the bus cycle sequences
kept word for word, the peripherals reached through the kernel's high alias instead of
0xF2000000 (the low 4 GiB belong to the Macintosh here), and every wait on the CPLD bounded, so
a Pi on the bench without an SE still boots to its second screen and its log instead of spinning
for ever. `mac.bridge=gpio` is the default; the testbed passes `semihost` (or `uart`). The
service core on core 2 owns it, as it owned the bridge: a read is a 68000 bus cycle, the
interrupt level is the status register's top three bits, a RESET instruction pulses the
motherboard's reset line, and the 200 MHz clock the CPLD needs comes from the Pi's GPCLK0 on
GPIO 4. Only the low 24 bits of an address travel: the 68000 socket has no more.

What the wire does not do is make the IIci ROM's addresses mean anything to an SE. The ROM
talks to a VIA at 0x50F00000, an SCC at 0x50F04000, a 5380 at 0x50F10000, an RBV, a VDAC, an
ASC; the SE has its VIA at $EFE1FE, the SCC at $9FFFF8/$BFFFF9, the 5380 at $5FF000, an IWM at
$DFE1FF, and no RBV, VDAC or ASC at all (docs/01 and docs/03 have both tables). That translation,
with the missing chips emulated by the kernel and the SE's video buffer fed from the Pi, is the
**board layer**, the piece between the transport and the first picture on the SE's screen. It is
best built against MAME's own `macse` driver with the bridge CPU attached to it (the same
`bridge68k` device the IIci testbed uses, on a 16-bit, 24-bit bus), so it can be debugged here
before the hardware arrives. That is the next piece after the 43438 path.

*04:00, the same night.* Both exist now. `reference/mame-psmac.patch` carries `macse_ps`: the SE
parts of MAME's mac128.cpp with a `bridge68000_device` (the bridge on a 16-bit, 24-bit bus;
the SE's SCSI window is the one whose replies wait a scheduler slice) and the overlay cleared,
as the SE's BBU does, by an access to the ROM's own space. The kernel's board layer is
`emu68/src/psmac/psmac_board_se.c`, `mac.board=se`: VIA, SCC, 5380 and IWM reached at the SE's
addresses (the VIA with port A bit 6 and port B bit 6 forced, so the SE scans out its main
buffer and keeps the 5380's interrupt to itself; the pseudo-DMA window read a byte at a time,
since the SE's bus is 16 bits and its 5380 sits on the upper byte), the RBV, ASC and VDAC
answered by the kernel (monitor sense 7, no built-in monitor, so the ROM takes the slot-9 card
as its main screen; the ASC's FIFOs always half empty so the chime loop ends), the SE's level
2 (SCC) reported to the ROM as level 4 and the RBV's level 2 made here, and the SE's 512 by
342 screen fed from the card's frame buffer, changed rows only, by the service core between the
Mac's own cycles. The card's declaration ROM is the 512x342 one, so Monitors sees the SE's
screen as the card's screen; the Pi's HDMI shows the same picture for now. What the first boots
against `macse_ps` said is in docs/08, item 28: six findings between the first bus cycle and
the Finder (the machine identity on VIA port A, the lost RAM writes, the stale slot line, the
byte lanes of the 68000 bus, the pseudo-DMA data register), and at 05:04 **System 7.1's Finder
on the SE's own screen** (`docs/img/system71-finder-on-macse.png`), booted from the SE's SCSI
disk with PiDisk mounted beside it. Step 1 of the bench order above is therefore done in the
testbed; on the hardware it comes down to the GPIO wire and the SE's real timing.

Then the order on the bench:

1. **The ROM runs, the SE's screen shows something.** The IIci ROM from the Pi, the SE's VIA,
   RAM (for its video buffer) and ROM socket reached through the GPIO. The calibration table
   (docs/08 item 14) gets the real bus latency instead of the bridge's. First milestone: the
   SE's own screen draws the happy Mac or the Sad Mac code; either tells what the bus is doing.
2. **Floppy and SCSI**: the IWM and the NCR 5380 through the real bus; pseudo-DMA timing, which
   the testbed only approximated (docs/08 item 13).
3. **The second screen** on the Pi 3's HDMI, which needs no motherboard at all and can be
   checked first of everything with the SE unplugged.
4. **WiFi**: the service core on the real chip, PiWiFi, MacTCP over it. The simulated radio's
   command sequence is what the real one must answer.
   4a. The user's Pi is a **3B v1.2**: BCM43438. The chip layer now has the Cortex-M3 and SOCRAM
   path (`wifi/chip.c`, 17 November, 03:45, from Linux's brcmf_chip_cm3_*: the M3 disabled, the
   radio cores reset, SOCRAM reset with bank 3 powered up, the size from SOCRAM's bank registers,
   firmware at 0, the M3 reset to run it; save/restore from the PMU's extended capabilities);
   it is untested until the board is here. The 43430 firmware trio goes in the container (README)
   and `BCM43430A1.hcd` for Bluetooth. 2.4 GHz only. Everything else in the WiFi module (SDIO
   host, the BCDC/firmware protocol, scanning, joining, the Mac-side driver) is chip-independent.
5. **PiDisk on the SD card**: a driver for the Pi 3's SD card (the SDHOST controller, not the
   Arasan one the WiFi uses) so disk images live on the card.
6. **Bluetooth**, then **USB input** (DWC2; written and tested in QEMU, docs/08 item 40: on the
   board, check `[USB] start rc=0: 1 keyboard, 1 mouse` with both plugged in, then `usb=pio`).

## The Pi alone: `mac.board=solo`

Before the PiStorm board arrives, and for anyone who wants a Pi that boots System 7 on an HDMI
monitor, the kernel can be the logic board itself: `mac.board=solo` answers the VIA (with the
clock/PRAM chip and the ADB transceiver behind it), the SCC, the IWM and an NCR 5380 with a SCSI
disk per image in the container, next to the RBV, ASC and VDAC the SE layer already answers
(`emu68/src/psmac/psmac_board_solo.c`; docs/08, item 35). The second screen is the main screen.
A disk image goes into the container like any other file (`*.img`, `*.hda`, `*.image`, `*.hfv`;
the first is SCSI ID 0, the startup disk; further ones are PiDisk volumes, docs/08 item 52): it is loaded into RAM with the rest of the initramfs, so a 1 GB Pi takes
images up to a couple of hundred megabytes, and what the Mac writes lasts until the power goes.
The PRAM is the kernel's (AppleTalk off, 32-bit addressing on, then whatever the Mac saved in
`pram.bin` on the card, docs/08 item 51), the clock starts on 17 November 2026 (or where the last
save of `pram.bin` left it), and the ADB bus is empty: the Finder comes up but has no mouse yet. The testbed for it is
`tools/run-solo.sh` (QEMU alone, no MAME, host files through semihosting, screenshots through the
QEMU monitor).

## Power through the socket

*20 November 2026.* In the SE the Pi takes its 5 V through the 68000 socket and the PiStorm, from
an analog board some thirty years old, while a Pi 3B draws 0.5 to 0.8 A. Under-voltage is the
first suspect for anything odd on the first boots, and the Pi's firmware knows when it happens:
the mailbox tag GET_THROTTLED (0x00030046). The kernel reads it every two seconds on the service
core, on every board (`psmac_power.c`; `mac.power=off` stops it), and logs only changes:

    [PSMAC] power: normal (0x0), watched every 2 s (mac.power=off to stop)
    [PSMAC] power: under-voltage now; ARM frequency capped (0x30003)
    [BOOT] t=41.207s power: under-voltage seen (GET_THROTTLED 0x30003)
    [PSMAC] power: normal again; under-voltage, frequency capping occurred since boot (0x30000)

Bits 0-3 are "now" (under-voltage, ARM frequency capped, throttled, soft temperature limit), bits
16-19 the same "has occurred since boot". A first reading with the high bits already set means
the dip came before the kernel (during the firmware's own boot, when the Pi draws most). The
first under-voltage is also a `[BOOT]` line, which the remote control keeps with the boot's
record (`aurora68.py log --records`), and `/state` carries the bits as `power` (docs/13). QEMU's
mailbox does not know the tag; the kernel says so once and stops asking.

What to do with a "power: under-voltage" line: shorter, thicker wiring is not available inside a
socket, so the remedies are the SE's 5 V rail itself (the analog board's adjustment, recapping) or
a Pi that draws less (the 3A+ and the Zero 2 W, on order, have no USB hub or wired Ethernet chip
to feed).

## Two screens: the SE's and HDMI

*20 November 2026* (docs/10, "Two screens on the SE", has the design and the testbed's record). On
`mac.board=se` the Pi can present the HDMI output as a second screen of its own beside the SE's
512 by 342: any size up to 1920 by 1200, any depth from one bit to millions, the SE's screen the
main one with the menu bar. It is two virtual cards, slot 9 (the SE's screen, as before) and slot
$A (the HDMI screen), in one declaration ROM file, `aurora68-se-WxH.declrom`. For the first boots
the log on HDMI is more useful than a second desktop, which is why the release's cmdline.txt has
`mac.console=hdmi`; once the SE's screen shows the Finder:

1. **Container**: put `aurora68-se-WxH.declrom` for the HDMI size in it (instead of
   `aurora68-512x342.declrom`; one `-se-` file only), e.g. `aurora68-se-1024x768.declrom`.
   Sizes built: 640x480, 800x600, 1024x768, 1280x720, 1280x1024, 1600x1200, 1920x1080, 1920x1200.
2. **config.txt**: the HDMI mode of that size, `hdmi_group=2` with `hdmi_mode=16` (1024 x 768),
   `9` (800 x 600), `82` (1920 x 1080) or `69` (1920 x 1200, reduced blanking); keep `gpu_mem=64`
   for anything over 640 x 480.
3. **cmdline.txt**: take out `mac.console=hdmi` (it means `mac.hdmi=log`, one screen) and add the
   size, e.g. `enable_cache debug mac.board=se bt.pair=1 mac.screen=1024x768`. `mac.hdmi=screen`
   is the default; `mac.hdmi=mirror` gives back the SE's picture on HDMI, `mac.hdmi=log` the log.
4. **The Mac**: copy **PiScrn** (on the Aurora68-Utilities disk) onto the System Folder, which
   puts it into Extensions. At the next start it finds the two screens, writes their arrangement
   (the SE's main at the left, HDMI to its right) into the System file and restarts the Mac once;
   from then on both screens come up at every start. The log says
   `[MAC] PiScrn: two screens: their arrangement written into the System file; restarting once`.
   Without PiScrn the HDMI screen stays grey until it is arranged in Monitors.
5. **Millions of colours** need 32-bit addressing (Memory control panel, On, restart). PiScrn
   chooses millions when 32-bit addressing is on and otherwise the deepest mode a 24-bit system
   reaches (256 colours at 640 x 480 and 800 x 600, 16 at 1024 x 768, fewer above); it keeps an
   arrangement that still fits, so after switching 32-bit addressing on choose Millions for
   monitor 2 in Monitors. Monitors also moves the screens and the menu bar as on any Mac II.

What the kernel's log says at start: `declaration ROM: two cards, the SE's screen in slot 9 (main)
and the HDMI screen in slot $A`, then `[PSMAC:DISP] two screens: ...` with the HDMI size and the
VideoCore buffer the firmware granted. The remote control photographs either screen
(`aurora68.py --display 1 screenshot hdmi.png`). Not seen on hardware yet: everything above ran
on the MAME SE and QEMU's model of the VideoCore.

## The first boot on the SE

The photographs of these boots, in the order they happened, are in `docs/14-the-se-in-pictures.md`.

*20 November 2026* (docs/08 item 63 has the record). The first boot of `mac.board=se` in the user's
SE died within a second, in the IIci ROM's MMU table builder, before anything reached the SE's
screen; a second boot with `mac.ram=8` got further and stalled with no video. Neither was the
memory size. The SE's own clock chip, which is the PRAM on this board, holds extended byte $8A
with bit 4 set, and the IIci ROM reads that byte to choose its memory layout: bit 4 puts RAM at
logical $38000000, which with 128 MiB overruns the table builder's frame on the stack (the MAME SE
crashes register for register like the hardware with $8A = $15) and at 8 MiB leads elsewhere. The
board layer now mends the byte on its way to the ROM, whoever reads it: bit 4 cleared, 32-bit
addressing on when the Mac has more than 8 MiB, the rest as the chip has it. The kernel says so in
one line:

    [PSMAC:SE] parameter RAM $8A: the SE's clock chip says $15, the ROM is given $05: bit 4 cleared (...)

System 7.1 writes back the mode it started in, so after the first good boot the chip holds $05 and
the line no longer appears. `mac.pram8a=keep` turns the mending off; `pram.bin` on the card was the
solo board's alone then (since docs/08 item 72 the SE's stand-in, below, uses it too). The same change keeps the RBV stand-in's SCSI interrupt
flag at 0 on the SE (the 5380's interrupt never reaches the Pi there), which is the likeliest cause
of the stall at 8 MiB, and classes the heartbeat's bus cycles by the SE's address map.

For the next boot: the default memory (no `mac.ram=`), the rest of the cmdline as before; the
lines to look for are the `parameter RAM $8A` one, `switch #3: TC=80f06b00 ...` with
"log 00000000-07ffffff -> phys 00000000", and the heartbeats' `scsi` and `via` counts.

### The second boot: a clock chip that does not answer

*20 November 2026* (docs/08 items 64-67). With the $8A mending the ROM got past its MMU tables, and
then waited for ever, with no video, on its open of the floppy driver. The SE's clock chip read $FF
every time, also after the ROM had rewritten it: it does not answer at all. The IIci ROM cannot
start with parameter RAM like that (one byte, $AE, read as $FF sends it the wrong way; the MAME SE
with such a chip stops at the same instruction with the same registers). The kernel now asks the
chip before the Mac starts and, if it reads $FF for the signature and the clock, answers in its
place with default parameter RAM and a clock of its own, kept until the Pi is switched off (since
docs/08 item 72 kept in `pram.bin` on the card instead, below). One
line says which case it found:

    [PSMAC:SE] clock chip: answers; parameter RAM valid (signature 4e 75 4d 63), clock ...
    [PSMAC:SE] clock chip: reads $FF for the signature and the clock: it does not answer (...). The board layer answers in its place (...)

and every report line ends in `clock chip: answering|not answering, N bytes read (M of them $FF
from the chip), K written`. `mac.sepram=chip` turns the stand-in off. A clock chip that reads $FF
while the SE is switched on is worth a look: a leaked PRAM battery corrodes the board around the
chip and the VIA, which is the usual reason on an SE; the SE's own ROM shrugs it off, the IIci's
does not.

Two more fixes from that boot: with `mac.console=hdmi` the log stays in the buffer the Pi's
firmware made at boot (the kernel's 32-bit buffer at the same size stayed black on the user's
monitor); and the USB keyboard retries, which held the service core for 360 ms each with nothing
plugged in, now run on another core and only when a device is there to retry.

For the next boot, with nothing else attached as on the second (no SCSI disk, no floppy), the SE's
screen should show the grey desktop and the floppy with the question mark. A card image cannot be
the startup disk yet (docs/08 item 67: the declaration ROM has no boot record for PiDisk), so the
SE needs a startup disk of its own, a SCSI disk (or a BlueSCSI) with a System, or a System floppy;
the card's images (`mac.image=NAME`) then mount beside it. Lines to look for: the `clock chip:` one,
`[MAC] PiVideo: open`, and the heartbeats' `display:` line with `flags 3`. (Superseded the same day:
see "A disk image as the startup disk", below.)

### Stripes over the top hundred rows: a solder bridge

*20 November 2026* (docs/08 item 68). The next boots reached the question-mark disk, then, with a
BlueSCSI attached, the happy Mac and the Finder, the mouse working; but the top hundred or so rows
of the SE's screen stayed eight vertical bands of noise that never changed, whatever the Mac drew
there (the menu bar never appeared), across screens and reboots. The photographs said where to look:
the bands ended at row 100, which starts at $3FC000. Every row above it has address line A14 = 0
and A15 = 1; every row below, and every address of the SE's chips the kernel uses (VIA, SCC, SCSI,
IWM), has A14 = A15 = 1. Two lines that can only agree would break those rows and nothing else. The
user found a sliver of solder joining pins 22 and 23 of U2 (A14 and A15) on the hand-built PiStorm
(Rev B, 74LVC16373A latches); with it gone the screen is clean over its full height.

The kernel now tests the address lines into the SE's memory before the Mac starts, each line alone
(as 0 and as 1) and each pair of lines (all four combinations), and names a fault with the socket
pins to look at:

    [PSMAC:SE] address lines A1-A21 good: each line read back as 0 and as 1 ...
    [PSMAC:SE] address line A14 does not reach the SE as both 0 and 1: ... Check the PiStorm's A14: the 68000 socket's pin 42 ...
    [PSMAC:SE] address lines A14 and A15 are joined: ... Look for a solder bridge or a short between them: the 68000 socket's pins 42 and 43 ...

Data lines show themselves sooner (the VIA, the SCC and the SCSI chip would not work); a bad address
line can hide behind a working Mac, as this one did.

### Speedometer on the real SE

*20 November 2026.* With System 7.1 started from the BlueSCSI and 128 MiB, Speedometer 4 on the SE
gave a performance rating of 90.2 (a Macintosh Classic is 1.0): CPU 41.2, graphics 83.3, math 486.2,
disk 0.82, and an average of 276 for its benchmark mix. The disk figure is the SE's own 5380 and the
BlueSCSI behind it, which the Pi drives through the 68000 socket a byte at a time.

### "Parity has been disabled"

*20 November 2026* (docs/08 item 69). Booting from the BlueSCSI the Mac once greeted the user with
"Parity has been disabled because the parity circuitry is not functioning". The IIci ROM tells a
plain IIci from an IIci with parity by one bit of the VIA's port A, which on an SE is the floppy
drive select and reads whatever its pin is left at; when it read 1 the System took the SE for an
IIci with parity and complained. The board layer now always reads that bit as the plain IIci's (the
Pi's memory has no parity), and the stand-in parameter RAM no longer carries the testbed IIci's old
record of a failed parity test. Click Continue if it still appears once with an older kernel.

### Nothing on the SCSI bus: no terminator

*20 November 2026* (docs/08 item 70). With the BlueSCSI taken off and no internal disk, the se7 kernel with
`mac.boot=image` found the image on the card ("startup disk: parameter RAM $78-$7B a5 a5 a5 a5, shown to
the Mac as 00 00 09 80") and then sat at the question-mark disk, the 68k at PC $408076D8 from about five
seconds on, 640,000 accesses a second to the 5380, the last at $5FF010: the SCSI Manager's arbitration,
waiting for a bus that never became free. With the BlueSCSI attached SCSI had worked, because the BlueSCSI
terminates the bus; with nothing on it the lines float and read asserted. The kernel now looks at the bus
before the Mac starts (our own 5380 driving nothing, BSY and SEL read eight times) and, when they read
asserted, answers the 5380 itself as an empty, terminated bus, so the ROM's scan ends in a moment and the
image starts the Mac:

    [PSMAC:SE] SCSI: the SE's SCSI bus has nothing on it, or no terminator: BSY and SEL read asserted with nobody arbitrating (bus status $fe, ...). The board layer answers the 5380 as an empty bus ...
    [PSMAC:SE] SCSI: the SE's SCSI bus is terminated and free (bus status $00, ...): the SE's 5380 is used

`mac.sescsi=real` uses the chip whatever the probe says, `mac.sescsi=empty` never uses it. With the
BlueSCSI attached and `mac.boot=image` the image is the startup disk and the BlueSCSI mounts beside it
(MAME). For the next boot: the `SCSI:` line, with and without the BlueSCSI; its bus status value tells
which lines the SE's floating bus reads asserted.

### A disk image as the startup disk

*20 November 2026* (docs/08 item 67). A disk image on the card (`mac.image=NAME`, a PiDisk volume on
the SE) is now a startup disk: when no SCSI disk with a System answers, the SE starts from the image.
The declaration ROM's PiDisk entry has gained a boot record, the code the IIci ROM runs to start from
a NuBus card; the kernel shows the Mac slot 9 as its default startup device until that code has run,
and tells it which disk to try first (`mac.boot=`):

- `auto` (the default): the SCSI disk the parameter RAM names (ID 0, the BlueSCSI's usual ID, unless
  the Startup Disk control panel chose another), then the card's image, then the other SCSI disks;
- `image`: the card's image first, then the SCSI disks;
- `scsi`: the parameter RAM as it is; the image starts the Mac only if Startup Disk chose it.

A floppy with a System always comes first, as on any Mac. The image must be a startup disk in its own
right: a System Folder the Mac has started from (with its boot blocks); an Apple-partitioned `.hda`
is served as its HFS partition. It needs the new declaration ROM in the container (rebuild it) and the
new kernel8.img. Lines to look for:

    [PSMAC:MBOX] startup disk: parameter RAM $78-$7B ..., shown to the Mac as 00 00 09 80, slot 9's PiDisk, until its boot record asks (mac.boot=auto)
    [PSMAC:MBOX] startup disk: the SCSI disk the parameter RAM names first, then the card's image (mac.boot=auto): SCSI ID 0, refNum -33; 1 image(s) on the card
    [MAC] PiDisk boot: the SCSI disk the parameter RAM names first, the card's image after it
    [BOOT] t=...s PiDisk: first block read (device 0, block 0)

### Sound from the SE's speaker, and parameter RAM on the card

*20 November 2026* (docs/08 items 71 and 72). The SE has no sound chip: its video hardware reads one word
of a 370-word buffer $300 below the top of its RAM ($3FFD00 with 4 MB) in every line, the visible ones
and the vertical blanking, and plays the high byte, 22,255 samples a second; VIA port A bits 0-2 are the
volume, port B bit 7 switches the sound off. The kernel now writes what the IIci ROM and the Sound
Manager put into the ASC into that buffer, in halves of 185 words in time with the SE's own vertical
blanking, sets the volume from the ASC's (the Sound control panel's), and switches the speaker off a
quarter second after each sound. The IIci ROM's startup chime is back at its full length (about a second
on the SE). `mac.sesound=off` leaves the speaker alone. In MAME the chime and System 7's beep come out
recognisably (docs/08 item 71 has the frequencies); on the hardware it has not been heard yet. Lines to
look for: `[PSMAC:SE] sound: the ASC's samples go to the SE's speaker ...` at the start, and in the
report lines `sound: N starts, ... halves written (L late, H held ...) ... a half written in U us at
most`: L and H should stay small next to the halves written, U well under 4,000.

The stand-in parameter RAM (the SE's clock chip reads $FF, docs/08 item 64) is now kept in `pram.bin` in
the card's root, as on the solo board: read before the ROM's first parameter RAM access, saved a second
after the Mac changes something once the boot is over (at most every ten seconds), and every ten minutes
for the clock, which goes on from the last save. The log says `stand-in parameter RAM from pram.bin on the
card (... bytes; signature 4e 75 4d 63 ...), clock ...` or `the defaults ...`, and `[CARD] pram.bin:
parameter RAM saved` when it writes. A clock chip that answers is used as it is and the file is left
alone. To try it: set the time in Date & Time, change the startup disk or AppleTalk, wait ten seconds,
switch off, switch on.

### Two screens and sound on HDMI, for the next boots

*20 November 2026* (docs/08 items 73 and 74; the steps are in docs/09, "Two screens and sound through
HDMI on the real SE"). The HDMI screen of the two screens is drawn into the firmware's own boot buffer when
it has the screen's size (16 bits a pixel on the monitor), since that buffer is the one known to be visible;
every frame-buffer request now reaches the firmware through the VideoCore's uncached alias, and the firmware's
view of the buffer is read back into the log. The Mac's sound plays on the HDMI monitor in stereo, mixed on
core 1, with the SE's speaker playing too: the ASC's two channels always, and a sound card of 16-bit stereo
at 44.1 kHz when the **Sound PiStorm** extension finds Sound Manager 3.0. config.txt needs `hdmi_drive=2`
(the release's has it): in DVI mode the transmitter sends no sound. `mac.audio=jack` uses the 3B's
headphone socket instead. Lines to send back from the first boot with all of it:

    [PSMAC:DISP] VideoCore read back: physical ..., depth ..., pitch ..., ...: as asked | NOT what was asked
    [PSMAC:DISP] two screens: ... VideoCore 1024x768 x16 pitch 2048 at ...: the firmware's own buffer, kept
    [AUDIO] HDMI: 48000 Hz stereo through DMA channel N; HSM clock ... Hz (MAI divider n/m), pixel clock ... Hz, N 6144 CTS ...
    [AUDIO] HDMI: the transmitter sends no packets (packet RAM off): ... DVI ...      (if silent)
    [MAC] PiScrn: ...
    [MAC] PiSound: Sound Manager 3.0; the sound card (16-bit stereo, 44.1 kHz, played by the Pi) registered ...
    [PSMAC]   audio: HDMI 48000 Hz, B blocks | ASC ... | card F frames, U underruns, queued Q, volume V | C clipped
    [PSMAC:SE]   sound: ...
    [AUDIO] DMA channel N error, debug ...                                           (should not appear)

In the heartbeat's audio line, the blocks should grow by 625 every five seconds at 48 kHz (the DMA is
running at the HDMI's pace), the card's underruns stay small next to its frames, and "(some filled late)"
should not appear. Nothing of the HDMI audio, the DMA or the jack has run on hardware yet.

## What to have ready

- The release zip's SD card layout as it is (0.2): `kernel8.img`, `config.txt`, `cmdline.txt`,
  the container with the IIci ROM and declaration ROM, `WIFI/` with the firmware for the Pi
  model in hand.
- No serial cable: on the classic PiStorm GPIO 8-23 are the 68000's data bus (`psmac_gpio.c`), so
  GPIO 14 and 15 are D6 and D7, not a UART. (Until 20 November 2026 this list asked for a cable
  there, and the kernel moved its log onto those pins in ALT5 when Bluetooth took the PL011; with
  the GPIO transport up it now stops using a serial line instead, `psmac_console_miniuart`.) The
  windows into the first boots are the HDMI screen (`mac.console=hdmi` keeps the log there for the
  whole run), PiLog once the Mac is up, and `aurora68.log` on the card (PiLog's Save to Card, and
  written by itself after every crash record).
- A known-good CPLD SVF (the shipped firmware) before any reflash.
- An HDMI monitor for the second screen, which doubles as "is the kernel alive" before the SE
  side works.
