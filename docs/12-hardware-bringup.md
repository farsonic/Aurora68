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

| Piece | Pi 3B | Pi 3B+ / 3A+ | Notes |
|---|---|---|---|
| WiFi chip | BCM43438 (id 43430), 2.4 GHz | BCM43455, dual band | same Arasan SDIO host as the Pi 4; the kernel picks the firmware by chip id (`alice_net.c`: 43455, 43456 and now 43430 name tables). **The 43438 is a different family inside**: an ARM Cortex-M3 with SOCRAM where the 43455 has a Cortex-R4 with tightly-coupled memory banks, and `wifi/chip.c` only knows the R4 (RAM at 0x198000, bank enumeration, CR4 halt and release). The CM3 path from Linux's brcmfmac (`brcmf_chip_cm3_*`, SOCRAM at 0, bank sizes from the SOCRAM core) is a day's work and is item 4a below |
| WiFi firmware files | `brcmfmac43430-sdio.bin`, `brcmfmac43430-sdio.txt` | `cyfmac43455-sdio-standard.bin` (or `brcmfmac43455-sdio.bin`), `brcmfmac43455-sdio.txt`, `.clm_blob` | from RPi-Distro/firmware-nonfree, as the README's curl lines |
| Bluetooth | BCM43438 over UART, `BCM43430A1.hcd` | BCM4345C0 over UART, `BCM4345C0.hcd` | the module now looks for either file by name (`bt_pi.c`). The Pi 3 has one PL011; Bluetooth sits on it unless `dtoverlay=miniuart-bt`. The kernel's log and the Bluetooth module must agree on which UART is whose (open) |
| Ethernet | LAN9514 over USB (100 Mbit) | LAN7515 over USB on the 3B+, none on the 3A+ | no GENET; the Alice68 GENET module does not apply. The WiFi is the network until a DWC2 USB stack exists |
| USB host | DWC2 (OTG) | DWC2 | not the Pi 4's VL805 xHCI; keyboards and mice through the Pi need a DWC2 driver (later; the SE's own ADB keyboard and mouse work regardless) |
| Memory | 1 GB | 1 GB / 512 MB | the Macintosh gets its full 128 MB on all three |
| Second screen | HDMI, VideoCore IV | same | as tested |

So, for WiFi first: the Alice68 WiFi module over SDIO is expected to work on a 3B+/3A+ with the
files already in the release notes, and on a 3B with the 43430 pair, which the kernel now looks
for. Bluetooth needs the UART question settled; USB input needs a new driver.

## The transport: from the bridge to the GPIO

In the testbed every motherboard access leaves `psmac_bus.c` as a bridge transfer to MAME over a
UART or semihosting (`bridge_xfer`). Upstream Emu68 carries the classic PiStorm transport in
`emu68/src/pistorm/ps_classic_protocol.c`: `ps_setup_protocol`, `ps_read_8/16/32`,
`ps_write_8/16/32`, `ps_read_status_reg`, `ps_pulse_reset`, `ps_get_ipl_zero`, and the
housekeeper that watches the interrupt lines. The step is a second back end behind the same
calls: when the kernel finds no bridge (no `mac.bridge=`), it drives the GPIO protocol instead,
with the same reply structure (bus error, data) so the MMU shim and the service core do not care
which it is. The 24-bit address, the function codes and the interrupt-acknowledge cycle are what
the classic firmware already does for the Amiga.

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
   4a. The user's Pi is a **3B v1.2**: BCM43438. Before step 4 the chip layer needs the Cortex-M3
   and SOCRAM path (above), the 43430 firmware trio in the container (README) and
   `BCM43430A1.hcd` for Bluetooth. 2.4 GHz only. Everything else in the WiFi module (SDIO
   host, the BCDC/firmware protocol, scanning, joining, the Mac-side driver) is chip-independent.
5. **PiDisk on the SD card**: a driver for the Pi 3's SD card (the SDHOST controller, not the
   Arasan one the WiFi uses) so disk images live on the card.
6. **Bluetooth**, then **USB input** (DWC2).

## What to have ready

- The release zip's SD card layout as it is (0.2): `kernel8.img`, `config.txt`, `cmdline.txt`,
  the container with the IIci ROM and declaration ROM, `WIFI/` with the firmware for the Pi
  model in hand.
- A serial cable on the Pi's GPIO 14/15: the kernel's log is the only window into the first
  boots. On a Pi 3 with Bluetooth on the PL011 this is the mini UART; `enable_uart=1` is in
  `config.txt` already.
- A known-good CPLD SVF (the shipped firmware) before any reflash.
- An HDMI monitor for the second screen, which doubles as "is the kernel alive" before the SE
  side works.
