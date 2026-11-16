# The service cores and the mailbox: how the Pi's devices reach the Mac

## Where the design comes from
Emu68 runs the 68k JIT on ARM core 0 and leaves the other three cores nearly idle: core 1 writes
the serial log, core 2 does the PiStorm's housekeeping (sampling the interrupt lines), core 3 is
reserved for a PowerPC emulation this project does not use. The idea of using those cores to drive
the Pi's own hardware and hand the results to the 68k through shared memory is Alice68's, the
user's Amiga kernel: there, "boards" in memory carry USB transfers, Ethernet and WiFi frames,
Bluetooth state and a read-only status page, and AmigaOS drivers find them like real Zorro cards.
The Macintosh version keeps that model and changes only the plumbing, so that the hardware drivers
(thousands of lines that already work on a Pi 4) port over almost unchanged.

## The four cores on the Macintosh build
| core | upstream Emu68 | Aurora68 (psmac) |
|---|---|---|
| 0 | 68k JIT | 68k JIT, unchanged |
| 1 | serial log writer | the same |
| 2 | PiStorm housekeeping | the **bus-transport owner**: it talks to the motherboard (the MAME bridge in the testbed, the CPLD on hardware) and samples the interrupt level at 1 kHz; nothing else, so no Mac bus cycle ever waits for a WiFi transaction |
| 3 | PowerPC | the **service core**: the mailbox services (information, block devices, the plain net service) and the Alice68 modules (network board with WiFi and Ethernet, Bluetooth, the status page), `alice_psmac.c` |
The split between cores 2 and 3 was made on the second day, when the Alice68 modules were brought
in: their WiFi transactions and Bluetooth UART waits can hold a core for milliseconds, which the
bus transport cannot afford.

## The window: a NuBus card that exists only in memory
The first design put the mailbox at a free address in the IIci's I/O space (0x51000000) and
expected drivers to switch the MMU to 32-bit mode to reach it, as Apple's own 32-bit-clean NuBus
drivers do. It was replaced on the second day by something simpler and more Macintosh-like: the Pi
is a **card in NuBus slot $9**. The bottom 1 MiB of the slot's super slot space (0xF9000000) is
ordinary Pi RAM shared with the 68k, and the IIci ROM's own tables map it in both modes (24-bit
System 6 reaches it at 0x900000), so no driver ever switches modes. The top 64 KiB (0xF9FF0000)
hold a **declaration ROM** the Slot Manager reads at boot; it carries the Mac-side drivers, and the
Start Manager loads and opens them exactly as it would for a 1989 card (docs/09). The Pi serves
both regions from its own memory; the MMU shim maps them for the JIT (window read-write,
declaration ROM read-only).

Layout of the window (`emu68/src/psmac/psmb.h` is authoritative):
```
+0x00000  header: magic 'PSMB', version, service count, features, Pi/Mac RAM, uptime, firmware string
+0x00100  service table, 8 x 32 bytes: type (1 info, 2 block, 3 net, 6 Alice68 boards), command block offset, parameters
+0x01000  one 64-byte command block per service: req_seq, done_seq, cmd, args, buf (68k logical), len, status, result, xfer
+0x02000  block devices, 4 x 64 bytes: blocks, block size, flags, name
+0x04000  the plain net service's receive ring (superseded by the Alice68 network board; kept for the first tests)
+0x20000  Alice68 network board, 128 KiB (alice_net.h: units 0 WiFi and 1 Ethernet, command boxes, 2 KiB frame slots)
+0x40000  Alice68 USB pass-through board, 64 KiB (alice68_usbpt.h; not yet served on the Mac)
+0x50000  Alice68 status page, 4 KiB (alice_status.h: cores, temperature, JIT counters, network counters)
+0x51000  input event ring, 4 KiB (alice_psmac.h: keyboard and mouse reports from USB and Bluetooth)
+0x80000  bounce area, 512 KiB, free for drivers
```
Rules, the same on both sides and the same as Alice68's: every field is a 32-bit word or an
array of a multiple of 4; a magic and version come first; the payload is written before the
sequence or head word that publishes it; counters only count up and positions are `index % size`;
a full ring drops rather than waits. Emu68 is big-endian AArch64 like the 68k, so nothing is
byte-swapped. Buffer addresses the Mac hands over are 68k logical addresses; the service core
translates them page by page with the MMU shim, which also makes the flag bits in the high byte of
24-bit pointers harmless.

## Services
| service | Pi side | Mac side |
|---|---|---|
| info | ping, a log line into the Pi console (`[MAC] ...`), time | the `log "text"` macro in every 68k source |
| block | disk images: host files through QEMU semihosting in the testbed (`mac.disk0=`), SD-card images on hardware (to come) | `.PiDisk` DRVR, loaded from the declaration ROM at boot; the volume mounts on the desktop |
| network board | Alice68's `alice_net.c`: unit 0 WiFi (SDIO, firmware preloaded by the kernel), unit 1 Ethernet (GENET on a Pi 4; in the testbed a bridge to host frame files and `tools/pinet.py`) | `.Network_EtherNet_PiStorm` DRVR on the `.ENET` interface, found by MacTCP/EtherTalk/Open Transport through the Slot Manager |
| Bluetooth | Alice68's `bt/`: HCI over the PL011 UART, HID keyboards and mice, LE | keyboard and mouse reports go to the input ring; a Mac input driver will turn them into events (to come) |
| status page | Alice68's `alice_status.c`: seq odd while writing | a monitor application (to come) |
| USB | Alice68's xHCI driver (Pi 4 only): compiled but not started; needs the DMA pool below 1 GiB | keyboards and mice through the input ring; no Mac USB stack exists |

## Interrupts
Version 1 has none: the service core sets the board's `irq` flag and the Mac drivers poll from
VBL tasks (60 Hz), which is what a real card's receive interrupt would give at the latency of a
System 6 machine. The proper version, planned, is a **virtual slot-9 interrupt**: the IIci routes
NuBus interrupts through the RBV's slot-interrupt register at level 2; the service core can merge
"slot 9 pending" into the interrupt level the JIT sees and answer the RBV register read with the
slot-9 bit, after which the Mac drivers use the Slot Manager's standard `SIntInstall` and nothing
in the ROM changes.

## What was learned bringing the Alice68 modules over (second day)
- The modules compiled for the psmac build with almost no change: a peripheral base macro (the
  Mac build maps the Pi's peripherals at 0x3F000000, Alice68 at 0xF2000000), an attach function
  instead of autoconfig, a firmware-preload entry, and the Amiga hooks (virtual CIA interrupt,
  keyboard/mouse sinks, splash strip, reboot) provided by `alice_psmac.c`.
- Bluetooth uses the PL011 UART, which on the testbed is the console. The first run with the
  modules on reprogrammed the console to 3 Mbit/s and the log went silent; `bt=off` in the testbed
  boot arguments keeps the console. On hardware the console moves to the mini-UART, as on a
  Raspberry Pi with Bluetooth enabled.
- The WiFi code waits for firmware files before touching the chip; the Mac build will carry them in
  the Emu68 initramfs next to the ROM (the Pi firmware loads one file), so no 68k cooperation is
  needed and WiFi can come up before the Mac boots.
- Licence: the Alice68 modules are GPL-2.0-only; the psmac fork is MPL-2.0, which allows the
  combination, and the resulting kernel binary is distributed under the GPL.

## The hardware side, for the record
The Alice68 drivers are written for the BCM2711 (Pi 4, CM4, Pi 400): GENET for Ethernet, the VL805
xHCI over PCIe for USB, the Pi 4's GPIO pull registers for the SDIO pins. On a Pi 3A+ or Zero 2 W
there is no GENET and no xHCI; the WiFi chip of the Zero 2 W (CYW43436, an ARM Cortex-M3 part) is
a different family from the Pi 4's CYW43455 and would need the "sonics" bring-up path the driver
does not have. The choice of board for the PiStorm decides which of these matter (docs/00).
