<!-- SPDX-License-Identifier: MPL-2.0 -->
# 22. An Aurora68 card for the SE's processor direct slot: a design note

*24 November 2026.* SE owners running Aurora68 have been talking about a card for the SE's processor direct
slot (PDS) in place of a PiStorm in the 68000's socket. This note is for whoever designs that card. It says what
the Aurora68 kernel needs from the bus, what the card has to do with each of the slot's signals, what the
software would change for a card built around a Pi 3-class board and for one built around a Compute Module 4,
and what is still to be checked. Nothing here has been built or measured. Where a fact comes from a source it
is named; where it is our reading or a guess it says so.

The original hardware plan already had a CM4 carrier in the PDS slot as the second route (docs/00, "Hardware
plan"). What changed since is that the socket route works (release 0.16 runs on a real SE, a Plus and a 512Ke),
and it showed what the socket costs: a Pi hanging upside down over the logic board, its main chip facing the
PiStorm board, at 80 °C and frequency-capped on the owner's bench even out of the case (the status window,
24 November 2026).

## 1. Why a slot card

- **Nothing in the CPU socket.** The SE's slot carries the 68000's bus, so the card plugs in and the original
  68000 stays where it is. No socket adapter, no strain on the socket, nothing to lift on the logic board.
- **Heat.** A card standing in the slot can carry its computer the right way up, with a heatsink and the SE's
  fan blowing past it. In the socket the Pi's main chip faces the PiStorm board across a few millimetres.
- **A port at the back.** The SE's case has a cut-out at the rear for a slot card: Ethernet and USB can come
  out of the case instead of through the case's gaps.
- **A way back to a stock SE.** With the 68000 still fitted, the card can let go of the bus and let the SE start
  its own ROM on its own processor (section 4, "Stock mode").

Which machines: the **SE** only. The 128K, 512K, 512Ke and Plus have no slot (they keep the socket route). The
**SE/30's** slot is a 68030 bus, 32 bits wide, with a different ROM and memory map: an Aurora68 card for it would
be a different project, closer to the IIci testbed than to the SE.

## 2. What the kernel needs from the bus

The kernel does not run the SE's 68000 code on the board: the Mac's memory is the Pi's, the IIci ROM runs in
Emu68, and the board is reached only for its chips and its screen and sound buffers (docs/12, "The transport";
docs/18, section 4). Every access goes through two functions, `psmac_wire_read(addr, size)` and
`psmac_wire_write(addr, size, data, posted)` in `emu68/src/psmac/psmac_bus.c`, on the core that owns the
transport. So the contract with the card is small:

1. **Byte and word cycles at a 24-bit address**, read and write, with /UDS and /LDS as a 68000 drives them
   (a byte at an even address on D8-D15, at an odd one on D0-D7). Long accesses are split into two words by the
   kernel. Only the low 24 address bits exist on the SE's bus.
2. **/DTACK honoured as the board gives it.** The SE's BBU delays /DTACK for RAM while the video and sound
   fetches take their slots; the card must wait for it, as a 68000 would, and must sample the data when a 68000
   would. docs/18 sections 1 and 4 describe how the classic PiStorm's CPLD samples about one 68000 clock early
   and why the SE's BBU hides that while the Plus-era boards do not: a new design should sample at the 68000's
   own point (S6/S7), not early.
3. **6800-style cycles for the VIA.** The SE's VIA is a 6522 clocked by E. An access in its range gets /VPA from
   the board; the card then asserts /VMA in step with E and ends the cycle on E's falling edge, as the 68000 does
   (MC68000 user's manual, "Interface with 6800 peripherals"). See section 4 on where E comes from on a slot card.
4. **The interrupt level.** The kernel reads /IPL0-2 (today through the CPLD's status register, bits 13-15) and
   makes the IIci ROM's interrupts from them. It never runs an interrupt acknowledge cycle on the board: the SE's
   interrupts are autovectored and the kernel answers them itself. The card never needs to drive FC2-0 = 111.
5. **The board's reset.** The IIci ROM's RESET instruction becomes a pulse on /RESET (about 124 68000 clocks, as
   the instruction makes), which resets the board's chips and not the processor. The kernel also pulses it once
   at its own start (`psmac_gpio.c`; the backstory of `psmac_hdmi.c`).
6. **Bounded waits.** Every wait on the card must end: the kernel's transport already gives up on a cycle that
   never completes and logs it (`txn_wait` in `psmac_gpio.c`), so that a Pi on the bench with no SE still starts.
   A card that asserts /BERR after a timeout, and reports it, would let the kernel tell a missing chip from a
   slow one.
7. **Throughput.** The SE's screen is 10,944 words; the kernel rewrites changed rows only, plus the speaker's
   370-word buffer twice a frame while a sound plays (`psmac_board_se.c`). On the socket today a word cycle
   costs about a microsecond (docs/18, K3, an estimate). Anything near that is enough; the 68000 bus itself is
   the limit (7.8336 MHz, four clocks a cycle without wait states).

The card does not need to support: the 68000 running the SE's ROM while the card watches (section 4 has stock
mode as a separate state), bus snooping, DMA by the card into the SE's RAM beyond ordinary cycles, or the
68000's TAS read-modify-write (the kernel never issues one to the board).

## 3. The slot's signals

**Pin numbers are not given here.** The authoritative pinout is Apple's figure "Macintosh SE 68000 Direct Slot"
in *Designing Cards and Drivers for the Macintosh Family*; Apple's technote list notes that the figure was
corrected in the third edition (page 284 there). Take the pin numbers from that corrected figure and check them
against the SE's schematic and a meter on a real board. One warning from looking: a 96-pin table that circulates
on pinout sites under "PPDS" is the **Macintosh Portable's** slot, not the SE's, although the two share the
Euro-DIN connector and most signal names. Do not lay out a board from it.

What the card needs, by group (directions are the card's when it holds the bus):

| Group | Signals | Card | Notes |
|---|---|---|---|
| Address | A1-A23 | drives | three-stated until the bus is granted |
| Data | D0-D15 | both | driven only on writes, after the bus is granted |
| Strobes | /AS, /UDS, /LDS, R/W | drives | three-stated until granted; R/W high when idle |
| Cycle end | /DTACK | reads | from the BBU; sample data at the 68000's point (section 2, item 2) |
| 6800 cycles | /VPA in, /VMA out, E in (see section 4) | both | the VIA |
| Function codes | FC0-FC2 | drives | supervisor data (101) for everything; never 111 |
| Interrupts | /IPL0-/IPL2 | reads | encoded, active low |
| Arbitration | /BR out, /BG in, /BGACK out | both | section 4; whether all three are on the SE's slot is the first thing to confirm |
| Control | /RESET, /HALT | both, open drain | section 4; never drive them push-pull |
| Error | /BERR | reads (and may drive) | the SE's glue may assert it; a card time-out may too |
| Clocks | the 68000's clock (C8M, 7.8336 MHz) and C16M, if present | reads | the card's state machine runs from the 68000 clock as the CPLD does today |
| Power | +5 V, ground, +12 V (and whatever else the figure shows) | takes | section 6 |

Signals the classic PiStorm does not use today and a slot card must add: /BR, /BG, /BGACK (docs/18, section 4:
"/BR and /BGACK are inputs never read, /BG is never asserted"), and E as an input instead of an output.
Frimples' `MacSE-PiStorm-DirectSocket` RTL (`reference/MacSE-PiStorm-DirectSocket`, with `tb_arbitration.v`)
already synchronises /BR and /BGACK and tracks the grant; its README is explicit that it is "deliberately
separate from the PiStorm PDS-card work" and does not implement a slot takeover, but its arbitration and
tri-state logic and testbenches are the nearest starting point.

## 4. Taking the bus, and giving it back

**Taking it (our reading of the MC68000 user's manual, "Bus arbitration"; to be checked on a board).**
1. Hold /BR asserted from power-on.
2. The 68000 answers with /BG at the end of its current bus cycle.
3. When /AS, /DTACK and /BGACK are all negated, the card asserts /BGACK and keeps it asserted for as long as
   Aurora68 runs, then releases /BR. While /BGACK is held the 68000 stays off the bus: its address, data, /AS,
   /UDS, /LDS and R/W are three-stated.

What to check: whether the 68000 grants the bus while it is held in reset at power-on, or only once it has left
reset (and so may fetch its first vectors from the SE's ROM before it sees the request). If the latter, the
card holds /HALT with /RESET at power-on, keeps /HALT asserted until it has the grant, then lets /HALT go. A
halted 68000 also three-states its bus (the manual's description of the halt state), so /HALT alone is the
fallback if /BR and /BGACK turn out not to be on the slot, at the price of a processor that is halted rather
than parked.

**E.** On the socket the CPLD makes E itself, because there is no 68000. On a slot card the SE's 68000 is still
there, still clocked, and, as we read the manual, **E stays a free-running output of the 68000 even while it
has given the bus away** (E is not among the lines it three-states). The SE's VIA is clocked by that E. So the
card must follow the board's E for its /VMA cycles, not make its own; a second E source on the same line would
fight the 68000's. Confirm on a scope before committing the CPLD to either.

**Resets.**
- The kernel's board reset (the RESET instruction, and once at start): /RESET alone, about 124 clocks, open
  drain. A 68000 is reset only by /RESET and /HALT together, so the parked 68000 stays parked.
- The SE's own reset (power-on, the programmer's switch): /RESET and /HALT together from the board. The card
  sees it, resets its own state, and starts the bus request again; the kernel restarts the Mac as it does today
  when the SE is reset under it.
- The card must never hold /RESET or /HALT in a way the SE's power-on circuit cannot override.

**Stock mode.** A switch or a kernel request (a `mac.` option, or a menu in Aurora68's setup) makes the card
release /BGACK and /BR and pulse /RESET with /HALT. The SE's 68000 then starts the SE's own ROM, and the card
keeps every line three-stated until the next power-on (or the next request). Nothing on the Mac side is needed.
It is worth having from the first prototype: it tells a card fault from a board fault in one restart.

## 5. Two ways to build it, and what changes in the software

### A. A Pi 3-class board (3A+, Zero 2 W, 3B) with the classic PiStorm protocol

The card is the classic PiStorm moved onto a slot card: an EPM240 or EPM570 CPLD, the same latches, the same
GPIO protocol (registers DATA, ADDR_LO, ADDR_HI, STATUS; GPIO 0 transaction in progress, GPCLK0 on GPIO 4;
`emu68/src/pistorm/ps_protocol.h`), plus the arbitration of section 4, E as an input, and the sampling point of
section 2.

Kernel changes:
- `psmac_gpio.c`: a start-up step that waits for the card to report the grant (a status bit), logs it, and
  times out with a clear message; and a `mac.slot=pds` (or similar) option so a socket card and a slot card can
  share one kernel. If the card's protocol is otherwise unchanged, nothing else in the transport moves.
- Stock mode: one more register write and a log line.
- Nothing in the board layer (`psmac_board_se.c`), the Mac side, or the drivers.

This is the quickest route and the one that can reuse the most, but it keeps the Pi 3's limits: 1 GB of memory,
four Cortex-A53 cores at 1.2 GHz, WiFi on the 3A+/3B/Zero 2 W only, Ethernet only on the 3B (through its USB
hub).

### B. A Compute Module 4

A CM4 on the card, with either the same CPLD protocol over its GPIO or an FPGA protocol like the PiStorm32-lite's.
What it brings: four Cortex-A72 cores at 1.5 GHz (roughly twice the work per clock of the A53), up to 8 GB of
memory (256 MB or more for the Mac stops being a question), Gigabit Ethernet on the module, WiFi and Bluetooth on
the modules that have them, eMMC instead of a loose SD card, and room for the card's own USB and Ethernet at the
SE's rear cut-out.

What already works on that family (to be confirmed, not assumed):
- Emu68 runs on the CM4: the PiStorm32-lite uses one, and Emu68 carries Pi 4 GPIO settings in
  `ps_protocol.h` (`GPFSEL*_PI4`, `CLEAR_BITS_PI4`) and Efinix FPGA images (`efinix_firmware_ps16.h`,
  `_ps32.h`; which boards the "ps16" image is for has not been checked here).
- The network drivers came from Alice68, written for the BCM2711 in the first place: GENET for Ethernet, the
  CYW43455 for WiFi (docs/07).
- The CM4's own USB 2.0 port is a DWC2 controller, the same family as the Pi 3's, which `src/alice/usb` drives.

What the kernel would need for a CM4 (each a piece of work, none done):
- **The card's storage.** `psmac_card.c` drives the Pi 3's SDHOST controller directly (`SDH` at peripheral base
  + 0x202000). The CM4's SD and eMMC are on its EMMC2 (SDHCI) controller: a new low-level driver under the same
  FAT and image code (the large-image work of docs/08 item 142 sits above that layer and would not change).
- **HDMI and its sound.** The second screen comes from the firmware's frame buffer (mailbox), which the Pi 4
  family keeps; the HDMI sound and the mode switching (`psmac_hdmi.c`, `psmac_audio.c`) need checking line by line
  against the Pi 4's HDMI blocks, which are not the Pi 3's.
- **Bluetooth.** The UART and its pins differ (the Pi 3's arrangement is in docs/21); `docs/21` has the Pi 3 details the CM4 port would redo.
- **Peripheral base and GPIO pulls.** Emu68's upstream code handles the BCM2711's base address; the project's
  own files that poke registers directly need a review (`grep -n PERI emu68/src/psmac/*.c`).
- **The bus transport.** With the CPLD protocol over GPIO, `psmac_gpio.c` with Emu68's Pi 4 GPIO settings. With
  an FPGA protocol, a new back end beside `psmac_gpio.c` behind the same `xfer` interface.
- **Power and temperature.** `psmac_power.c` reads GET_THROTTLED through the mailbox, which the CM4's firmware
  answers the same way.

### Which first

A is a few days of kernel work once a card exists; B is weeks, most of it storage, HDMI sound and Bluetooth, and
gives a much better machine. A sensible order: a Pi 3-class slot card to prove the arbitration, E and stock mode
on real SEs, then a CM4 card with the same bus half.

## 6. Physical and electrical

- **Power.** A Pi 3B draws 0.5 to 0.8 A at 5 V (docs/12, "Power through the socket"); a CM4 with Ethernet and
  WiFi in use draws more, several watts under load. How much the SE's slot and its analog board can supply has to
  be taken from Apple's figures for the SE and checked against the owner's supply (several SEs now run on
  replacement supplies; the Plus tester's PicoPSU-style supply had a noisy -12 V rail, docs/18 section 7). Put a
  current measurement on the first prototype's 5 V.
- **Heat.** Leave room for a heatsink on the SoC and a path for the SE's fan's air. The reason for the card is
  partly this (section 1).
- **Clearances.** The slot sits near the analog board and the CRT's neck: keep conductive parts, and anything a
  person might touch while the machine runs, away from the high-voltage side. Mechanical limits (card height,
  where the rear cut-out is) from Apple's card dimensions for the SE.
- **Levels.** The SE's bus is 5 V TTL; the Pi and CM4 are 3.3 V. The PiStorm's 74LVC parts are 5 V tolerant on
  their inputs; any new part on the bus needs the same, and its outputs must meet TTL highs.
- **Licences.** If the card reuses the PiStorm's hardware design or the DirectSocket RTL, check their licences
  before publishing a board; the kernel side is MPL-2.0 and GPL-2.0-only as the rest of Aurora68 (CLAUDE.md).

## 7. Testing without a card

The MAME testbed's `macse_ps` machine already stands in for the SE board behind a bridge (docs/06): the kernel's
board layer runs against it unchanged. For a slot card, the bridge can answer the new arbitration step at once
and accept the stock-mode request as a no-op, so the kernel's side is tested before hardware. MAME could also be
taught to keep its own 68000 on the bus until it sees /BR, /BG and /BGACK, which would exercise section 4's
sequence; that is optional.

## 8. Open questions (in the order they block)

1. Are /BR, /BG and /BGACK all on the SE's slot, and on which pins (Apple's corrected figure; the schematic; a
   meter)?
2. Does the 68000 grant the bus before its first vector fetch after power-on, or must the card hold /HALT?
3. Does E keep running from the 68000 while the bus is granted away, as we read the manual?
4. Which clock does the slot carry for the card's state machine (C8M, C16M)?
5. The slot's power budget, on an original analog board and on replacement supplies.
6. Physical: card size limits and the rear cut-out's position.
7. For a CM4 card: the FPGA or the CPLD protocol, and which Emu68 back end it matches.

## Sources

- Apple Computer, *Designing Cards and Drivers for the Macintosh Family*, 2nd and 3rd editions: the SE's 68000
  direct slot pinout (corrected in the 3rd edition, page 284) and card dimensions. Scanned copy of the 2nd
  edition: https://www.vintageapple.org/inside_o/pdf/Designing_Cards_and_Drivers_for_the_Macintosh_Family_2nd_Edition_1990.pdf
- Apple, *Guide to the Macintosh Family Hardware*, 2nd edition: the SE's bus, BBU and VIA (docs/18 cites the
  pages it used).
- Motorola, *MC68000 User's Manual*: bus arbitration, the halt state, 6800 peripheral cycles.
- Apple technote HW 12, "Macintosh Portable PDS Development": the Portable's slot, which shares the connector and
  most names with the SE's and is not the SE's.
- `reference/MacSE-PiStorm-DirectSocket` (Frimples): arbitration RTL and testbenches for the socket.
- The PiStorm (captain-amygdala/pistorm) and Emu68's `src/pistorm/` (the classic protocol, Pi 4 GPIO settings,
  the Efinix images).
- Forum discussion of a slot card for the SE: TinkerDifferent, "PiStorm on the Macintosh SE"
  (https://tinkerdifferent.com/threads/pistorm-on-the-macintosh-se.2085/page-2).
- In this repository: docs/00 (the original plan), docs/07 (the Alice68 drivers), docs/12 (transport, power),
  docs/18 (the compact Macs' bus and the PiStorm's cycle), docs/21 (Bluetooth on the Pi 3).
