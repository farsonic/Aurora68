<!-- SPDX-License-Identifier: MPL-2.0 -->
# 22. An Aurora68 card for the SE's processor direct slot: a design note

*24 November 2026.* SE owners running Aurora68 have been talking about a card for the SE's processor direct
slot (PDS) in place of a PiStorm in the 68000's socket. This note is for whoever designs that card. It says what
the Aurora68 kernel needs from the bus, what the card has to do with each of the slot's signals, what the
software would change for a card carrying an existing Pi 3 board (3B, 3B+, 3A+, Zero 2 W) and for one carrying
a Compute Module (CM3+, CM4), and what is still to be checked. Nothing here has been built or measured. Where a fact comes from a source it
is named; where it is our reading or a guess it says so.

The original hardware plan already had a CM4 carrier in the PDS slot as the second route (docs/00, "Hardware
plan"). What changed since is that the socket route works (release 0.16 runs on a real SE, a Plus and a 512Ke),
and it showed what the socket costs: a Pi hanging upside down over the logic board, its main chip facing the
PiStorm board, at 80 °C and frequency-capped on the owner's bench even out of the case (the status window,
24 November 2026).

## 1. Why a slot card

- **Nothing in the CPU socket.** The SE's slot carries the 68000's bus, so the card plugs in and the original
  68000 stays where it is. No socket adapter, no strain on the socket, nothing to lift on the logic board.
- **Heat.** A slot card lies flat above the logic board, under the SE's chassis (section 6, "Where the card sits"),
  and can carry its computer components-up with a heatsink and a fan of its own. In the socket the Pi's main chip
  faces the PiStorm board across a few millimetres.
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
   the instruction makes), which resets the board's chips. The kernel also pulses it once at its own start
   (`psmac_gpio.c`; the backstory of `psmac_hdmi.c`). On the SE /RESET is wired to /HALT, so on a slot card the
   pulse resets the parked 68000 as well (section 4).
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

**The SE's pinout, confirmed by Apple (24 November 2026).** The owner found it first on Chris Adams's "Mac SE
Support Pages" ("Mac SE port pinouts", May 1993; mirror at https://machut.net/mirrors/www.ccadams.org/se/pinouts.html,
"Macintosh SE Internal System Expansion Slot"), then sent Apple's own book, *Designing Cards and Drivers for the
Macintosh Family*, 3rd edition (1992): Figure 13-1, "Macintosh SE 68000 Direct Slot connector pinout" (page 284), and
Table 13-1, each pin's signal, direction and loading (pages 285 to 287). The two agree pin for pin. Apple names the
rows: **Adams's column 1 is row C, column 2 is row B, column 3 is row A.** Apple's figure is the connector (J13) "as
viewed from above", with row C on the side marked "To I/O ports at rear of machine", row A on the side marked "Card
edge", and pin 1 at the end towards the SIMMs. A meter on a real board is still the last check before a layout.

| Pin | Row C (Adams col. 1) | Row B (col. 2) | Row A (col. 3) |
|---|---|---|---|
| 32 | -12V | -5V | +12V |
| 31 | Spare | +12V | +12V |
| 30 | Ground | +12V | Ground |
| 29 | D15 | Ground | C16M |
| 28 | D14 | Ext.DTK/ | C8M |
| 27 | D13 | Reserved | E |
| 26 | D12 | Reserved | A23 |
| 25 | D11 | Reserved | A22 |
| 24 | D10 | Reserved | A21 |
| 23 | D9 | Reserved | A20 |
| 22 | D8 | Spare | A19 |
| 21 | D7 | BERR/ | A18 |
| 20 | D6 | IPL2/ | A17 |
| 19 | D5 | IPL1/ | A16 |
| 18 | D4 | IPL0/ | A15 |
| 17 | D3 | +5V | A14 |
| 16 | D2 | +5V | A13 |
| 15 | D1 | +5V | A12 |
| 14 | D0 | +5V | A11 |
| 13 | +5V | +5V | A10 |
| 12 | RESET/ | HALT/ | A9 |
| 11 | PMCYC/ | Reserved | A8 |
| 10 | AS/ | Reserved | A7 |
| 9 | UDS/ | Ground | A6 |
| 8 | LDS/ | Ground | A5 |
| 7 | R/W/ | Ground | A4 |
| 6 | DTACK/ | Ground | A3 |
| 5 | BG/ | Ground | A2 |
| 4 | BGACK/ | Ground | A1 |
| 3 | BR/ | Ground | FC0 |
| 2 | VMA/ | Ground | FC1 |
| 1 | VPA/ | Ground | FC2 |

(Adams's table writes the external-DTACK pin "Ext.STK/"; Apple's figure and table say /EXT.DTK.)

What Apple's Table 13-1 adds, pin by pin, that matters for the card:
- **Pull-ups and directions:** /BR (C3) and /BGACK (C4) are inputs with 3.3 kOhm pull-ups on the logic board; /BG (C5)
  is an output; /RESET (C12) is open collector with a 1 kOhm pull-up and "connected to /HALT, pin B-12"; /IPL0-2
  (B18-B20) are open collector, 3.3 kOhm, and Apple names their sources: /IPL0 the VIA and the SCSI chip, /IPL1 the
  SCC, /IPL2 the NMI switch; /BERR (B21) open collector, 3.3 kOhm; /EXT.DTK (B28) an input with a 3.3 kOhm pull-up.
- **Drive:** the logic board's outputs can drive two 74LS inputs and 30 pF (C8M and C16M only one); Apple says cards
  needing more, "or more than about 2 inches of trace length, should buffer these signals". So the card's bus
  buffers sit within about 50 mm of the connector.
- **Loads the card must drive** when it is bus master: A1-A23 and D0-D15 250 uA high / 1 mA low and 100 pF; /AS
  200 uA / 3.2 mA and 50 pF; R/W 200 uA / 2 mA; /RESET 300 uA / 6 mA. 74LVC-class parts drive this easily.
- **Rails** (with the budget in section 6): +5 V on B13-B17 and C13, +12 V on A31, A32, B30, B31, -5 V on B32,
  -12 V on C32.

What Adams's page and Apple's text say about the signals, and what follows from each:
- **/BR, /BG and /BGACK are all on the slot** (column 1, rows 3, 5 and 4). Section 4's route works on the SE.
- **"HALT/ ... Wired directly to RESET/"** (and the same under RESET/). On the SE the two are one net. Every reset
  of the board is therefore also a reset of the 68000, and /HALT cannot be held on its own. Section 4 takes this
  into account.
- **E, C8M (7.8336 MHz, the 68000's clock, C16M divided by 2) and C16M (15.6672 MHz, the gate array's clock)** are on
  the slot (column 3, rows 27 to 29). The card's state machine can run from C8M as the CPLD runs from the
  68000's clock in the socket.
- **DTACK/ "is not supplied for accesses to VPA space, is held off to separate 2 successive accesses to the SCC and
  is held off during RAM access by video."** So the SE's gate array spaces SCC accesses itself (docs/18's K1 is
  done by the hardware on the SE), and RAM cycles wait for the video fetches. The card must wait for DTACK/ on
  every cycle outside VPA space.
- **"For Macintosh SE, VPA space is $E0 0000 to $FF FFFF"**: the VIA and the other 6800-style accesses. There the
  board answers /VPA instead of DTACK/, and the card runs a 6800 cycle (section 2, item 3).
- **Ext.DTK/** (column 2, row 28): pulled low, it puts the gate array's DTACK/ into high impedance and makes the
  card responsible for DTACK/. Aurora68's card should leave it alone (pulled up), so the board's own timing stays
  in charge.
- **PMCYC/** (column 1, row 11): "low when RAM is available for microprocessor accesses and is high during video
  accesses". Not needed (DTACK/ already waits for video), but a card could use it to start RAM cycles at the right
  moment and save wait states.
- **BERR/** is "generated by gate array due to SCSI access timeout". The card must end the cycle on it and report
  it to the kernel.
- **Power on the slot:** +5 V (column 1 row 13, column 2 rows 13 to 17), +12 V (column 2 rows 30 and 31, column 3
  rows 31 and 32), -12 V (column 1 row 32), -5 V (column 2 row 32), and ground. How much current each may supply is
  not on the page (section 6).

Two other tables are **not** the SE's and should not be used for it. The 96-pin table on pinout sites under "PPDS"
is the **Macintosh Portable's** slot: the same connector and most of the same signal names, other pins. The
AllPinouts table "Apple Macintosh Processor-Direct Slot (PDS)" (sent by the owner on 24 November 2026) is the
**120-pin 68030 slot of the SE/30 and the II family**, which that page itself calls "electrically and mechanically
incompatible with the 96-pin 68000 PDS found in the Macintosh SE and Macintosh Portable". For an SE/30 card one day
(section 1), that table shows the same arbitration lines there: /BR (C10), /BG (B10), /BGACK (A10), /RESET (A12),
/HALT (C12), /BERR (B12), the CPU clock (A38), C16M (C38), E (B38), +5 V, +12 V (C40), -12 V (A40) and -5 V (B40),
on a 32-bit bus. It also names where Apple's own tables for both slots are: the *Guide*'s chapter 14 (its Tables
14-1, 14-3 and 14-5 to 14-7).

What the card does with each group (directions are the card's when it holds the bus; positions are Apple's
row and pin):

| Group | Signals (row and pin) | Card | Notes |
|---|---|---|---|
| Address | A1-A23 (A4-26) | drives | three-stated until the bus is granted |
| Data | D0-D15 (C14-29) | both | driven only on writes, after the bus is granted |
| Strobes | /AS (C10), /UDS (C9), /LDS (C8), R/W (C7) | drives | three-stated until granted; R/W high when idle |
| Cycle end | /DTACK (C6) | reads | from the gate array; sample data at the 68000's point (section 2, item 2) |
| External DTACK | Ext.DTK/ (B28) | leaves alone | pulled up: the gate array keeps making DTACK/ |
| RAM availability | PMCYC/ (C11) | may read | optional, to start RAM cycles when video is not using RAM |
| 6800 cycles | /VPA (C1) in, /VMA (C2) out, E (A27) in | both | the VIA, in VPA space $E00000-$FFFFFF; E is the 68000's (section 4) |
| Function codes | FC0-FC2 (A3 to (A1) | drives | supervisor data (101) for everything; never 111 |
| Interrupts | /IPL0-/IPL2 (B18-20) | reads | encoded, active low |
| Arbitration | /BR (C3) out, /BG (C5) in, /BGACK (C4) out | both | section 4 |
| Reset | /RESET (C12) and /HALT (B12), one net on the SE | both, open drain | section 4; never drive push-pull |
| Error | /BERR (B21) | reads | the gate array's SCSI time-out; the card may also time out on its own |
| Clocks | C8M (A28), C16M (A29) | reads | the card runs from C8M |
| Power | +5 V, +12 V, -12 V, -5 V, ground | takes | section 6 |

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

**/HALT and /RESET are one net on the SE** (section 3). That has three consequences:
- **No halting the 68000 on its own.** Holding /HALT would hold /RESET, and with it the whole board (the VIA, the
  SCC, the SCSI chip) in reset. Bus arbitration is the only way to keep the 68000 off the bus, and fortunately
  all three lines are on the slot.
- **Every board reset is also a 68000 reset.** When the kernel resets the board (the IIci ROM's RESET instruction,
  and once at the kernel's start), the card pulls /RESET low, which on the SE is /RESET and /HALT together: the
  68000 resets too. The card must keep /BGACK asserted through the pulse and after it, so that the 68000, coming
  out of reset, finds the bus taken and does not fetch its vectors. **To check:** that a 68000 coming out of reset
  with /BGACK asserted really waits, rather than starting its vector fetch (the manual's reset and arbitration
  sections, then a scope on a real SE). If it does not wait, the card has to keep /BR asserted across the reset as
  well and take the bus back at the first /BG, before the 68000's first cycle completes, which needs care in the
  CPLD. (A precedent, section 10: the MicroMac Performer, a shipped 68030 card for this slot, never asserts /BGACK at
  all; it holds /BR from the start of every reset for as long as it runs, and drives the bus one or two C8M clocks
  after /RESET rises.)
- **Power-on.** The SE's reset circuit holds /RESET and /HALT low at power-on. The card asserts /BR during that
  time; whether the 68000 grants the bus while still in reset, or only after its first cycles, is the same question
  as above.

**E (confirmed by Apple).** On the socket the CPLD makes E itself, because there is no 68000. On a slot card
the SE's 68000 is still there, and Apple's text answers the question directly: "After removing itself from the bus
by tristate control, however, the MC68000 continues to generate its E clock but no longer provides /VMA. This means
an expansion card coprocessor must correctly synchronize its VIA selection (using /VMA) and VIA accesses to the
timing of the MC68000 E clock. The coprocessor can accept /VPA as its /DTACK, or provide its own." (*Designing Cards
and Drivers*, 3rd edition, chapter 13, "Accessing I/O devices from an expansion card".) So the card follows the
board's E (A27) for its /VMA cycles and never drives E. Apple's chapter calls exactly this arrangement, a card
taking the bus with /BR, /BG and /BGACK, a "coprocessor" on the expansion card.

**RAM cycles: the BBU's rules (same chapter, Figures 13-2 and 13-3).** The kernel writes the SE's screen and sound
buffers in the board's RAM, so these apply:
- The BBU runs a 16-state machine from C16M; a RAM cycle is eight states, S0 to S7, and video and sound fetches
  take states S8 to SF of their own cycles. A processor RAM access that starts during a video or sound cycle is held
  off (no /DTACK) until the next RAM slot.
- A card should behave like a 68000: address in S1, /AS in S2, /DTACK received in S4. It can find S1 from /PMCYC
  (C11), which "always falls in S1 of an eight-state processor cycle", with a falling-edge detector clocked by C16M.
- **/AS must never fall between 20 ns into S3 and 20 ns into S4**: the BBU starts /RAS early on a RAM address, and a
  late /AS gives a /RAS spike or a row/column change too soon, "causing RAM errors". The 68000 avoids this because
  it asserts /AS only in even states, clocked by C8M; the card's state machine must keep the same discipline (C8M
  lags C16M by up to 30 ns).
- Address set-up 15 ns before /AS and 45 ns before S3, held through the first 5 ns of S7; write data valid from S6
  through the first 30 ns of S7; read data valid from 15 ns into S7 until /CAS rises at the end of S0 or /AS rises.
- Outside RAM, $000000 to $DFFFFF: /DTACK in S4 after /AS in S2 (in S0 of the next cycle if /AS came after S3).
  $E00000 to $FFFFFF: /VPA right after /AS, then the 6800 cycle on E.
This is also what docs/18 section 4 asks of a new CPLD: sample read data at the 68000's own point. The classic
PiStorm's early sampling worked on the SE because the BBU's /DTACK timing hid it; a slot card should not rely on that.

**Resets, in short.**
- The kernel's board reset: /RESET pulled low for about 124 C8M clocks, open drain, /BGACK held throughout.
- The SE's own reset (power-on, the programmer's switch): the card sees /RESET low, resets its own state, keeps or
  re-asserts /BR, and the kernel restarts the Mac as it does today when the SE is reset under it.
- The card must never hold /RESET in a way the SE's power-on circuit cannot override.

**Stock mode.** A switch or a kernel request (a `mac.` option, or a menu in Aurora68's setup) makes the card
release /BGACK and /BR and pulse /RESET. The SE's 68000 then starts the SE's own ROM, and the card keeps every line
three-stated until the next power-on (or the next request). Nothing on the Mac side is needed. It is worth having
from the first prototype: it tells a card fault from a board fault in one restart.

## 5. The variants: the Pi 3 boards owners already have, and the Compute Modules

One bus half serves every variant: the same CPLD (or FPGA), latches, arbitration, E input and sampling point of
sections 2 to 4, reached through the same GPIO lines (GPIO 0 to 23 and GPCLK0 on GPIO 4, the classic PiStorm's
protocol in `emu68/src/pistorm/ps_protocol.h`). What changes between variants is the computer half: a 40-pin
header for a whole Pi, or a module connector with the card supplying what a module lacks. A sensible family is
two boards sharing the bus half's layout: **Card P** (a header for an existing Pi 3) and **Card M** (a Compute
Module socket).

On every variant the kernel's work for the slot itself is the same and small:
- `psmac_gpio.c`: a start-up step that waits for the card to report the grant (a status bit), logs it and times
  out with a clear message; a `mac.slot=pds` option (or the card identifying itself in the status register) so
  one kernel serves socket and slot cards.
- Stock mode: one register write and a log line.
- Nothing in the board layer (`psmac_board_se.c`), the drivers or the Mac side.

### 5.1 Card P: an existing Pi 3 on a header (3B, 3B+, 3A+, Zero 2 W)

The owner's own Pi plugs into a 40-pin header on the card, **the right way up**, components facing away from the
logic board, at the card's rear end where the chassis leaves the most height (section 6, "Where the card sits"),
with room for a heatsink. Nothing on the Pi changes, and the kernel, the card's files and the Mac side
are today's. This is the variant to build first: it proves the arbitration, E and stock mode on real SEs with
boards people already own, and every Pi 3-class board the socket route supports works the same way.

| Board | SoC, memory | Network today (README) | Notes for the slot card |
|---|---|---|---|
| Pi 3B | BCM2837, 1 GB | WiFi (BCM43438), Ethernet (LAN9514 on USB), Bluetooth | the owner's; the most tested; USB sockets and RJ45 face the card's edge, so they can line up with the rear cut-out if the card places the Pi so |
| Pi 3B+ | BCM2837B0, 1 GB | WiFi (CYW43455), Ethernet (LAN7800 on USB), Bluetooth | faster clock and hotter; its heat spreader helps a heatsink |
| Pi 3A+ | BCM2837B0, 512 MB | WiFi (CYW43455), Bluetooth; `eth=usb` for a USB adapter | draws least of the Pi 3s; 512 MB is enough (the Mac gets 128 MB; large images are served from the card) |
| Zero 2 W | RP3A0 (BCM2710A1), 512 MB | WiFi (BCM43436), Bluetooth; `eth=usb` | smallest; one micro-USB for data; mini-HDMI |

What the card adds for a Pi on a header: the bus half, power from the slot to the header's 5 V pins (with the
protection of section 6), and optionally a USB or HDMI extension to the rear cut-out. The Pi's own SD card stays
the boot medium, so it must be reachable without taking the card out of the slot (place the Pi's card edge at
the top of the card, or use an SD extension).

### 5.2 Card M with a Compute Module 3+ (or CM3)

The same SoC family as a Pi 3 (BCM2837B0 on the CM3+, BCM2837 on the CM3), on a 200-pin SO-DIMM module with no
network, no USB hub and no HDMI connector of its own: the card supplies them. Emu68 and the kernel run on it as on
a Pi 3; what differs is what the card wires up.
- **Storage:** the CM3+ comes with 8, 16 or 32 GB of eMMC, or as the Lite with the SD interface brought out for
  an SD socket on the card. Which of the SoC's two SD controllers the eMMC uses on these modules (the SDHOST the
  kernel drives today, `psmac_card.c`, or the other one) must be checked against the module's datasheet before
  assuming the card-image code works unchanged.
- **Network:** none on the module. The card can carry a LAN9514 or LAN7800 on the module's USB (both already
  driven, `psmac_usbnet.c`), and a WiFi/Bluetooth module on the SDIO and UART pins if wanted (a CYW43455 matches
  the driver; its antenna needs a path out of the SE's case).
- **USB:** one USB 2.0 port on the module (the DWC2 the kernel already drives); a hub on the card for more.
- **HDMI:** the module brings HDMI out; the card adds the connector at the rear cut-out.
- **GPIO:** the module exposes GPIO 0 to 45, so the bus half's lines are the same as on a Pi 3.
- Kernel work beyond section 5's slot step: the storage check above, and device-tree differences (the kernel
  reads the board's devices from the firmware's tree at start, as `psmac_usbnet.c` does for the wired port).

This is the cheapest module variant: Pi 3 software, a flat card with the module lying on it, and a heatsink or a
small fan over the SoC. It gains little in speed over a Pi 3B.

### 5.3 Card M with a Compute Module 4

The CM4 (BCM2711) on its two 100-pin connectors, in its variants: 1, 2, 4 or 8 GB of memory; eMMC (8, 16 or
32 GB) or Lite (an SD socket on the card); with or without WiFi and Bluetooth (CYW43455, with an antenna
connector, which helps inside a metal-shielded SE). What it brings: four Cortex-A72 cores at 1.5 GHz (roughly
twice the work per clock of the A53), memory to spare for the Mac and for disk images, Gigabit Ethernet from the
module (the card adds the PHY's magnetics and RJ45 at the rear cut-out), and PCIe if ever wanted.

What already exists for this family (to be confirmed on hardware, not assumed):
- Emu68 runs on the CM4 (the PiStorm32-lite uses one), and carries Pi 4 GPIO settings in `ps_protocol.h`
  (`GPFSEL*_PI4`, and `CLEAR_BITS_PI4`, which leaves GPIO 28 and 29, the Ethernet PHY's MDIO, alone) and Efinix
  FPGA images (`efinix_firmware_ps16.h`, `_ps32.h`; which boards the "ps16" image is for has not been checked).
- The network drivers came from Alice68, written for the BCM2711: GENET for Ethernet, the CYW43455 for WiFi
  (docs/07). The README's firmware steps already cover the Pi 4 and CM4.
- The CM4's own USB 2.0 port is a DWC2 controller, as on the Pi 3 (the Pi 4's USB 3 is a VL805 on PCIe, which a
  CM4 card does not need).

What the kernel needs for the CM4, each a piece of work and none done:
- **Storage.** `psmac_card.c` drives the Pi 3's SDHOST directly (`SDH` at peripheral base + 0x202000). The CM4's
  eMMC and SD are on its EMMC2 (SDHCI) controller: a new low-level driver under the same FAT and image code (the
  large-image work of docs/08 item 142 sits above that layer).
- **HDMI and its sound.** The second screen comes from the firmware's frame buffer (mailbox), which the BCM2711
  keeps; the HDMI sound and mode handling (`psmac_hdmi.c`, `psmac_audio.c`) need a line-by-line check against the
  BCM2711's HDMI blocks, which are not the Pi 3's.
- **Bluetooth.** Different UART wiring; docs/21 has the Pi 3's arrangement the port redoes.
- **Registers.** The project's own files that touch registers directly need a review for the BCM2711
  (`grep -n PERI emu68/src/psmac/*.c`); Emu68's upstream code already handles its peripheral base.
- **The bus transport.** With the CPLD protocol over GPIO: `psmac_gpio.c` with the Pi 4 GPIO settings. With an
  FPGA protocol like the PiStorm32-lite's: a new back end beside `psmac_gpio.c` behind the same `xfer` interface.
- `psmac_power.c` reads GET_THROTTLED through the mailbox, which the CM4's firmware answers the same way.

### 5.4 Not a candidate: the Compute Module 5

On the BCM2712 the GPIO pins sit behind the RP1 chip on PCIe, so each pin access is a PCIe transaction rather
than a register write next to the CPU: a GPIO bus protocol like the PiStorm's would be far slower per 68000
cycle. As far as we know Emu68 does not run on the Pi 5 family either (check upstream before ruling it out for
good).

### 5.5 Which first, and how much work

| Variant | Kernel work beyond today | What it gives |
|---|---|---|
| Card P, an existing Pi 3 | the slot step (days, once a card exists) | today's Aurora68, cooler, nothing in the socket, stock mode |
| Card M, CM3+ | the slot step; check the eMMC's controller; device-tree differences | the same, on a flat card with the module's eMMC |
| Card M, CM4 | the slot step; EMMC2 storage driver; HDMI sound; Bluetooth wiring; register review (weeks) | twice the speed, memory to spare, Gigabit Ethernet, eMMC |

A sensible order: Card P first, to prove sections 2 to 4 on real SEs with the Pis owners have; then Card M,
with the CM4 as the target and the CM3+ as a cheap step on the way if the CM4 work is not yet done.

## 6. Physical and electrical

- **Power: Apple's budget for the card** (*Designing Cards and Drivers*, 3rd edition, Table 13-3, page 298):

  | Rail | +5 V | -5 V | +12 V | -12 V |
  |---|---|---|---|---|
  | Expansion card | 1.5 A | 0.1 A | 0.15 A (1.5 A surges up to 10 s) | 0.1 A |

  and, "for thermal considerations, the total power of the expansion card should not exceed 7 W", because the card
  sits in "the restricted space over the Macintosh SE logic board". Apple allows a further 750 mA at +5 V for a
  peripheral outside the case, and budgets 1.5 A at +5 V and 0.9 A at +12 V for an internal SCSI disk.
  What that means for each variant (draws are typical figures, to be measured on the prototype):
  - **Pi 3A+, Zero 2 W, CM3+:** comfortably inside 7 W.
  - **Pi 3B:** 0.5 to 0.8 A at 5 V (docs/12, "Power through the socket"), 2.5 to 4 W: inside, with room for a small
    fan. **Pi 3B+:** more, still inside at the kernel's load but closer.
  - **CM4:** at full load with Ethernet and WiFi a CM4 can reach the 7 W line by itself. A CM4 card should be
    measured early; if it is over, cap the ARM clock (`arm_freq` in `config.txt`), or take part of the 5 V from the
    internal disk's budget, which many SEs no longer use since a BlueSCSI draws far less than a 1980s hard disk
    (take it from the disk power connector, not the slot, with its own fuse).
  - A 5 V fan of 0.1 to 0.2 A fits in any of these; a 12 V fan takes from the 0.15 A at +12 V.
  Feed the header's or module's 5 V through a fuse or current limit and an ideal-diode or reverse-current block, so a
  Pi on the header that is also powered from its own USB socket never back-feeds the SE. Replacement supplies vary
  (the Plus tester's PicoPSU-style supply had a noisy -12 V rail, docs/18 section 7): measure the first prototype's
  5 V current and ripple in the owner's own machine.
- **Heat.** Leave room for a heatsink on the SoC and a path for the SE's fan's air. The reason for the card is
  partly this (section 1).
- **A fan header (the owner's suggestion, 24 November 2026).** A 2- or 3-pin header for a small 5 V fan (25 to 40 mm)
  blowing across the SoC, switched by a MOSFET so it can be slowed or stopped:
  - **Who drives it.** On Card P every GPIO the Pi's header has is already used (GPIO 0 to 23 for the bus protocol,
    24 to 27 for the CPLD's JTAG, docs/18 K2), so the fan's PWM comes from a spare CPLD pin, set through one more
    protocol register; the fan's tachometer, if it has one, goes back the same way. On a Compute Module card a spare
    module GPIO can drive it directly (the CM3+ brings out GPIO 28 to 45; on a CM4 count what is left after the bus).
  - **What sets the speed.** The kernel already reads the SoC's temperature every few seconds for the status page
    (`alice_psmac_status.c`, the mailbox's GET_TEMPERATURE) and the firmware's throttling bits (`psmac_power.c`). A
    small loop there would set the duty: off below about 55 °C, rising to full at about 75 °C, before the firmware's
    80 °C cap (the thresholds are guesses to tune on a real SE). The status page and `aurora68.py log` would show the
    fan's duty and speed, and a `mac.fan=` option would force it on or off.
  - **A fallback that needs no software:** a jumper that runs the fan at full speed straight from 5 V, for bring-up
    and for anyone who prefers it.
  - Keep the fan's current off the Pi's 5 V path: take it from the slot's 5 V before the Pi's protection (or from
    +12 V with a 12 V fan, quieter at low speed), with its own fuse.
- **The rear panel.** The SE's back has an access cut-out for a slot card's connectors. Two ways to reach it, and the
  note's recommendation is the first, which is also Apple's (Figure 14-2, below):
  - **A short cable to a small panel board** screwed into the cut-out. Apple's figure marks a "suggested position of
    connector for cable to external port" on the card, near its rear end. It does not depend on where exactly the
    card sits relative to the cut-out, and it lets the card be designed before anyone has measured a case. What goes over it: Ethernet (the magnetics and RJ45 on the panel
    board, the PHY's pairs over a short cable; for Gigabit on a CM4, a short twisted-pair cable rather than flat
    ribbon), USB 2.0 (short and fine), and HDMI only as a short panel-mount HDMI extension, not ribbon.
  - **Connectors on the card itself, lined up with the cut-out.** Neater and no cable, but only if the card's edge
    really meets the cut-out: measure on a real SE (and in Apple's card dimensions for the SE in *Designing Cards and
    Drivers*) before choosing it. On Card P a Pi 3B placed with its USB and RJ45 edge towards the cut-out might
    manage it directly; the Pi's SD card must still be reachable.
  - Either way, a panel board can also carry a reset or stock-mode switch (section 4) and an activity LED.
- **Where the card sits** (*Designing Cards and Drivers*, 3rd edition, Figures 17-1 to 17-3, pages 384 to 386; the
  same drawing as the *Guide*'s Figure 14-2, which the owner sent). The card lies **flat, parallel to the logic
  board**, under the SE's metal chassis, across the logic board's width between its rear and front edges (Figure
  17-3, "Maximum outline expansion card"). Apple's numbers:
  - **Outline:** at most 213.25 mm by 120.42 mm (8.396 by 4.741 inches), 1.60 mm thick, with a 16.00 mm strip along
    one long edge marked "No components, traces only", two 3.86 mm mounting holes for the logic board's card supports
    (Figure 17-5 shows the supports beside the reset switch), and 2.0 mm by 45 degree corner chamfers.
  - **Height:** the side view, "Side view height restrictions" (Figure 17-2), gives a component height of **15.58 mm
    (0.614 inch) at most at one end of the card, falling to 4.61 mm (0.181 inch) at the other**, because the chassis
    above slopes. The owner adds that there is more room towards the back of the chassis. Put the tall parts (the Pi
    on its header or the module, its heatsink, the fan, the cable connector) where the 15.58 mm is, keep the low bus
    half where the 4.61 mm is, and check both against a real SE: a Pi 3B on a standard header plus a heatsink is
    taller than 15.58 mm, so Card P needs a low-profile header, the Pi mounted beside rather than above the card's
    parts, or the Pi's own height counted against the slope (open question 4).
  - **The connector:** a 96-pin, three-row Euro-DIN plug with compliant (press-fit) pins if components go on the top
    side (Figure 17-4; AMP was Apple's supplier; any standard mating connector fits).
  - **Away from the slot:** Apple's drawing has the card's own 96-pin plug at one end (pins 1A, 1B, 1C marked near a
    corner) and the "suggested position of connector for cable to external port" near the other.
  A fan there blows along the card, under the chassis.
- **The cable to the rear (Apple's design).** "Typically, a cable would be routed from the expansion card upward
  through a cutout in the back of the chassis, and then to a connector on a connector card you provide" (chapter 17,
  "External connections for the Macintosh SE"; Figures 17-7 and 17-8: a "preferred ribbon cable routing (shortest
  possible)" and an alternate one, the connector card held by two M3 screws at the rear housing's exit). Apple
  adds that a cable at least 220 mm long would also fit the SE/30 and the IIsi, and its Foldout 8 gives the connector
  card's dimensions. This settles the rear-panel choice above: a connector card on a cable, as Apple intended.
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

Answered on 24 November 2026: /BR, /BG and /BGACK are on the slot (C3, C5, C4); E, C8M and C16M are on it (A27-A29);
/HALT is wired to /RESET; the pinout is Apple's own (Figure 13-1); E keeps running from the 68000 with the bus
granted away (Apple's text); the card's power budget (1.5 A at +5 V, 7 W in all) and outline (213 by 120 mm, 15.58 to
4.61 mm of height); a cable to a connector card for the rear port. Still open:

1. Does a 68000 coming out of reset (power-on, or a board reset, both of which reset it on the SE) wait while
   /BGACK is held, or does it start its vector fetch? (Section 4.) *A precedent, not a measurement (section 10):* the
   MicroMac Performer keeps /BR asserted through and after every reset, never asserts /BGACK and never waits for /BG,
   and works; so a 68000 leaving reset with /BR asserted stays off the bus in practice. Aurora68's card should keep
   /BR asserted for as long as it owns the bus. Still to scope on an SE: the time from /RESET rising to /BG.
2. A meter on a real SE: the pinout, row letters and pin 1 as Apple's figure has them.
3. Which end of the card has the 15.58 mm and which the 4.61 mm (Figure 17-2 against the owner's "more room towards
   the back"), and how a Pi 3 on a header fits under it.
4. A CM4 card's real draw against the 7 W limit.
5. For Card P: where the Pi's SD card and connectors end up, reachable without taking the card out.
6. For a CM3+ card: which SD controller the module's eMMC is on.
7. For a CM4 card: the FPGA or the CPLD protocol, and which Emu68 back end it matches.

## 9. A first design: Card P in KiCad (24 November 2026)

`hardware/pds-card/` holds a first draft of Card P (section 5.1) as a KiCad 8 project, generated from a Python
description (`tools/design.py`) and checked by script, not yet by KiCad's ERC and DRC, not reviewed and not built;
its README is the long form. In short:

- **Circuit:** the PiStorm Rev B's, at the owner's wish (as simple and as close to the PiStorm as possible): an EPM240
  (or EPM570 in the same footprint), four 74LVC16373 latches, two CBTD3384 bus switches, the LM1117-class 3.3 V
  regulator, every PiStorm signal on the PiStorm's own CPLD pin, the Pi's GPIO 0-27 as in `ps_protocol.h`. Added: a
  polyfuse and an LM66100 ideal diode on the 5 V, the fan header with its MOSFET, jumper and fuse, a 10-way FFC to a
  rear connector card (USB from the 3A+, 5 V, the stock switch, an LED), an activity LED and a stock jumper.
- **CPLD logic** (`rtl/aurora68_pds.v`, MIT, from the PiStorm's): arbitration held across resets and safe whichever
  answer open question 1 gets, 6800 cycles on the board's E, /AS only in S2 (optionally on /PMCYC), read data latched
  at S6/S7, /BERR reported, stock mode, fan PWM and tachometer, status bits in REG_STATUS bits 0-12 and new settings
  in writes with bit 15 set (`PROTOCOL.md`): the kernel's transport is unchanged. Simulated with Icarus Verilog
  against a behavioural SE (154 checks); not synthesised.
- **Height (open question 3, our reading):** Figure 17-2's side view is drawn from the side of the machine, so the
  limit falls from 15.58 mm at the card's **front** edge to 4.61 mm at its **rear** edge, across the card's 110 mm
  depth, not along its length. The tall parts are along the front edge. To be measured in a real SE.
- **The Pi 3A+** sits components-down on a 6.0 mm socket over the card's left half (a Pi on a header cannot plug in
  the right way up), header along the front edge: its back is at 9.9 mm, 0.08 mm under the limit at its rear edge.
  The Zero 2 W fits too; **the 3B and 3B+ do not**: their 16 mm USB stacks point at the card. The card keeps their
  footprint clear for bench use on a riser.
- **Open question 5:** the 3A+'s microSD slot is at the card's left, under the chassis: reachable only with the card
  out, as with any card in this slot.

## 10. A working precedent: the MicroMac Performer (25 November 2026)

*Read from reverse-engineered design files; nothing here was built or measured.* The MicroMac Performer, a 68030
accelerator of the 1990s for the Plus, the Classic and the SE (docs/02), did what this note plans: it took the bus
from the logic board's own 68000 and kept it, and on the SE it sat in this slot. It shipped and sold, and its
clones run today. Three public repositories hold it:

- **Bolle's recreation** (68kMLA; github.com/TheRealBolle/Performer-SE-PL-CL, Eagle 9.6, 17 November 2022): the
  schematic "MM Performer.sch" redrawn from a real board, one universal PCB with a 68000 piggyback socket (Plus), a
  PLCC clip (Classic) and the slot connector J1 (SE), and the five GAL16V8 fuse maps dumped from the original (U3 to
  U7). Today's MacEffects "Performer PDS" is reported on the forums to be built from these files.
- **alxlab-zone66x/Performer-SE-PL-CL**: the same converted to KiCad, plus four **SE-only** boards ("Performer SE
  v1.0/v1.1, socket pga/plcc": J1 only, no U7).
- **luRaichu/polyphloisboisterous030** (Polyphloisboisterous '030, Rev. 1 December 2025): a smaller board for the
  Plus only, piggybacked on the 68000, no FPU (docs/02).

The fuse maps are the same bit for bit in all three, and the GALs' pins sit on the same nets on every board. All
three READMEs say their PCB files are **CC BY-NC-SA** (not for commercial use); none has a licence file, and the fuse
maps are dumps of a commercial product with no licence stated. Nothing of theirs is copied into Aurora68: the
pin-to-net readings and short equation excerpts below are our own.

**How it was read.** MAME's jedutil (0.264, `reference/mame`) decoded the fuse maps (`jedutil -view U4.jed GAL16V8`,
after giving the files the STX, ETX and transmission checksum the tool requires). Each GAL pin's net was read from
the pads of the KiCad boards (the Polyphloisboisterous, alxlab's universal board and its "Performer SE v1.1
socket-plcc"), and the Eagle schematic was used for its notes. Below, a name with "/" is an active-low net, "=0" and
"=1" are pin levels, and `next(X)` is a register after its clock edge; registered outputs power up high (the GAL's
power-up reset). To see the timing, the equations were run cycle by cycle in a small model (the 68030 on C16M, a
board answering /DTACK in S4 or later and /VPA in $E00000-$FFFFFF, E six C8M clocks low and four high); it is not in
the repository, and the statements that rest on it say "in the model".

### 10.1 The card around the GALs

- **One clock, no buffers.** The 68030's clock CLK_30 is the board's C16M, passed through U6 (pin 19 = pin 2): the
  card runs at 15.6672 MHz, synchronous with the SE. The only oscillator (QG1) clocks the optional 68882 (on the SE
  boards; forum owners of today's clone say its "25 MHz" and "33 MHz" crystals change the FPU's speed only). The
  68030's A0-A23, FC0-2, R/W and /IPL0-2 go straight to the slot, its D16-D31 to the slot's D0-D15 (a 16-bit port on
  the 68030's upper half, as dynamic bus sizing wants), A24-A31 nowhere: there are no bus buffers on the card.
- **Pulled up (4.7 kOhm) and otherwise unused:** the 68030's own /BR, /BGACK and /HALT, /STERM and /CBACK (no
  synchronous or burst cycles), /MMUDIS, /DSACK0 (only the 68882 drives it); and /EXT.DTK.
- **Slot pins used:** A1-A26 (FC2-FC0, A1-A23), A27 E, A28 C8M, A29 C16M, C1 /VPA, C2 /VMA, C3 /BR, C6 /DTACK, C7-C10
  R/W, /LDS, /UDS, /AS, C12 /RESET, C14-C29 data, B18-B20 /IPL0-2, B28 /EXT.DTK, +5 V. **Not connected:** C4 /BGACK,
  C5 /BG, B12 /HALT, B21 /BERR, C11 /PMCYC, the +12 V, -12 V and -5 V.

| GAL | Mode, clock | Inputs (pin: net) | Outputs (pin: net) |
|---|---|---|---|
| U3 | combinational | 1-5: A23-A19, 6-8: FC0-FC2, 9: /DTACK, 11: P3.B10, 13: /EXT.DTK | 12: /SCC.RW.DEC, 15: /DTACK_SYS+EX, 16: /CIIN, 17: /AVEC, 18: /FPUCS, 19: NOFC |
| U4 | registered, pin 1 SM_CLK16 (C16M inverted), /OE pin 11 = N$7 | 2: NOFC, 3: C8M, 4: /AS_30, 5: /DTACK_SYS+EX, 6: /VPA, 7: E, 8: R/W, 9: /RESET_30, 13: P2.A17, 19: N$1 | 12: /DSACK1, 16: /VMA, 18: /AS_00; 14, 15, 17 state bits |
| U5 | registered, pin 1 C8M, /OE grounded | 2: /AS_00, 3: SIZ1, 4: SIZ0, 5: R/W, 6: /IPL2, 7: /CIIN, 8: A22, 9: A0, 12: /RESET_00 | 13: /RESET_30, 16: /BR_00, 17: N$7, 18: /UDS, 19: /LDS; 14 state bit |
| U6 | registered, pin 1 C8M, /OE grounded | 2: C16M, 3: E, 4: /AS_00, 5: /AS_30, 6: /FPUCS, 7: FPUSENSE, 8: /RESET_30, 9: /SCC.RW.DEC | 12: N$1, 13: /BERR_30, 14: /CDIS (68030 pin H12), 18: SM_CLK16, 19: CLK_30; 15, 16 state bits |
| U7 | combinational (Plus and Classic only) | 3: C8M | 13: C16M; 14-16 a delay chain looped back to 4-6 |

"_00" nets are the logic board's (the 68000's), "_30" the 68030's.

### 10.2 What each GAL does

**U3: address and cycle-type decode.** Read from the equations:
```
/SCC.RW.DEC = 0  when A23=1, A22=0, A20=1                       the SCC ($9xxxxx read, $Bxxxxx write)
/CIIN       = 0  when A23=1, or A22=1 and (A21=1 or A20=1)       cache-inhibit $500000-$FFFFFF
/AVEC       = 0  when A23-A19 all 1 and FC2-0 = 111              interrupt acknowledge, autovectored on the card
/FPUCS      = 0  when A23-A19 all 0 and FC2-0 = 111              coprocessor space: the 68882
NOFC        = 0  when FC2-0 is not 111                           (gated by /EXT.DTK and P3.B10, see below)
/DTACK_SYS+EX = 0 when /DTACK=0 and (/EXT.DTK=1 or P3.B10=0)
```
So only RAM ($000000-$3FFFFF) and ROM ($400000-$4FFFFF) are cacheable; the SE's chips are not. The screen and sound
buffers are RAM and cached, which the 68030's write-through data cache allows. Only cycles with NOFC=0 go to the board:
an interrupt acknowledge is ended on the card by /AVEC and never reaches the board, as in Aurora68's kernel (section
2, item 4). With the pull-ups on /EXT.DTK (R16) and P3.B10 (R9), /DTACK_SYS+EX is the board's /DTACK unchanged and
NOFC depends on FC alone. The card never drives /EXT.DTK. *Inferred:* P2.A17 and P3.B10 are named after connectors
of the original board, and the Eagle schematic marks U5 pin 15 "nc - RAMCARD", so these unused inputs probably served
an optional memory card that could take over a cycle (P2.A17=0 disables U4's /AS_00 and /DSACK1).

**U4: the bus-cycle state machine.** Clocked on C16M's falling edges (SM_CLK16 is C16M inverted by U6), with C8M as
an input that tells it which half of C8M it is in. Its outputs to the board, /AS_00 and /VMA, are three-stated unless
N$7=0 (the card owns the bus), and every term needs /RESET_30=1 (a 68030 reset puts it back to idle). The excerpts:
```
next(/AS_00)=0 when /AS_30=0, NOFC=0, C8M=1, /DTACK_SYS+EX=1, N$1=0, P2.A17=1 and idle (state bits and /VMA high)
next(/VMA)=0   when /AS_00=0, /VPA=0, E=0, C8M=1, NOFC=0 and idle           (the 6800 cycle; one of six terms)
/DSACK1 = 0    when /AS_30=0 and Q17=0     (driven only while NOFC=0, /RESET_30=1, P2.A17=1)
```
In plain words, read from the equations and timed in the model:
- **/AS on the 68000's grid.** /AS_00 falls only at a C16M falling edge in C8M's high half: half a C16M period (32
  ns) plus the GAL's delay after the C8M rising edge that starts an even state. A 68030 cycle that starts in C8M's
  low half waits one C16M clock. So the board's cycle begins in S2, where a 68000 asserts /AS (MC68000 manual, Figure
  5-35), and /AS never falls near the S3/S4 boundary that the BBU forbids (section 4). It also waits for the previous
  cycle's /DTACK to go away, and for N$1 (U6). /UDS and /LDS follow /AS_00 through U5, so on writes they come with /AS
  rather than in S4 as a 68000's do.
- **/DTACK** is sampled at every C16M falling edge. When it is seen, Q17 asserts /DSACK1 (a 16-bit port); the 68030
  takes it at its next falling edge and latches read data one C16M clock later, and /AS_00 rises at that edge. In the
  model, with /DTACK in S4, /AS_00 is low from the middle of S2 to the middle of S7: the card latches read data a
  quarter of a C8M clock **after** the 68000's own point (the falling edge entering S7), not before it as the classic
  PiStorm does (docs/18). Back-to-back cycles come every four C8M clocks, the 68000's own rate: the card gains its
  speed from its caches, not from the board.
- **6800 cycles.** /VPA (the board's, for $E00000-$FFFFFF) and E low give /VMA; the state bits then count through E's
  high phase. A read ends at the last C16M falling edge before E falls; a write one clock later, so that /AS_00 and
  /VMA rise after E has fallen (in the model, about 1.5 C16M clocks after). E is only sampled; the card never drives
  it. In the model /VMA can be asserted as little as one C16M half-period before E rises, when /VPA arrives late in E's
  low phase. /VMA is also one of U4's state bits and goes low for one C16M clock in ordinary cycles too: harmless where
  the VIA is selected by address and /VPA (inferred, not checked against the SE's schematic).

**U5: arbitration, the 68030's reset, byte strobes.** Clocked on C8M's rising edges:
```
next(N$7)=0    when /RESET_00=1 and /BR_00=0 and (N$7=0 or Q14=1)
next(/BR_00)=0 when N$7=0, or Q14=1 and /BR_00=0, or Q14=1 and /RESET_00=0, or Q14=0 and /IPL2=1
next(Q14)=0    when the card runs (N$7=0, /BR_00=0, /RESET_00=1) and (Q14=0 or a cycle with /AS_00=0 and A22=1),
               or N$7=1, /IPL2=0 and two of: /RESET_00=0, Q14=0, /BR_00=0
/RESET_30 = 0  (driven) whenever N$7=1
/UDS = 0 when /AS_00=0 and (A0=0 or a cacheable read)                   } driven only
/LDS = 0 when /AS_00=0 and (SIZ0=0 or SIZ1=1 or A0=1 or a cacheable read) } while N$7=0
```
The three state bits (Q14, /BR_00, N$7) take these values (read from the equations by enumerating every input):

| Q14 /BR_00 N$7 | State | Left by |
|---|---|---|
| 1 1 1 | power-up, or disabled: no request, 68030 in reset, card three-stated | /RESET_00=0 only, to "request" |
| 1 0 1 | request: /BR asserted, 68030 in reset | /RESET_00=1: "running"; /RESET_00=0 with /IPL2=0: "disabling" |
| 1 0 0 | running: the card owns the bus, the 68030 runs | first cycle with A22=1 (the ROM): "running, ROM seen"; /RESET_00=0: "request" |
| 0 0 0 | running, ROM seen | /RESET_00=0: "request" |
| 0 1 1 (via 0 0 1) | disabling: /BR released, 68030 in reset | /RESET_00=1 with /IPL2 still 0: "disabled" (1 1 1); /IPL2=1 first: back to "request" |

In plain words:
- **/BR is asserted from the first C8M edge of any reset and is never released while the card runs**
  (`next(/BR_00)=0 when N$7=0`). The card never asserts /BGACK (C4 is not connected, the board's 3.3 kOhm pull-up keeps
  it negated) and never looks at /BG (C5 not connected).
- **It takes the bus one or two C8M clocks after /RESET rises,** at the first C8M rising edge at which U5 sees
  /RESET_00 high: N$7 falls, U4's strobes and U5's /UDS and /LDS are enabled, and /RESET_30 is let go. It does not
  wait for /BG. The 68030 then needs a few clocks of its own before its first cycle.
- **It is two-wire arbitration on a three-wire 68000.** Motorola's state diagram for the three-wire processors (MC68000
  user's manual, Figure 5-18 (a)) keeps the processor with BG asserted and its bus three-stated for as long as BR is
  asserted, whether or not BGACK ever comes, and section 5.2.1 says that "when no acknowledge is received before the
  bus request signal is negated, the processor continues the use of the bus": that is, only once BR goes away. The
  Performer relies on that, and on a 68000 that leaves reset with BR already asserted granting the bus before its
  first vector fetch (or at least not fighting the card's first cycle). The equations do not check it; that the
  product and its clones work is the evidence.
- **/RESET_00 and /RESET_30 are kept apart.** The card only reads the board's /RESET (C12) and leaves /HALT (B12)
  unconnected; it drives neither. /RESET_30 is the 68030's own: U5 holds it low whenever the card does not own the bus,
  and the 68030's RESET instruction pulls only /RESET_30, which idles U4 and U6. Read from the nets: a RESET
  instruction on the card does not reach the board's chips.
- **The disable switch.** The README's "hold Interrupt after the boot chime then quickly press Reset. After the second
  chime release Interrupt" is this table: /IPL2 is the programmer's switch's interrupt line. With /IPL2 held low through
  a few C8M clocks of reset, the card releases /BR and keeps the 68030 in reset; if /IPL2 is still low when the
  reset ends, the card goes to 1 1 1 and stays three-stated, and the SE's own 68000 starts its own ROM (the second
  chime). Only the next reset (the switch again, or power-on) brings the card back. Letting go of Interrupt before
  Reset aborts it. ("Quickly", our guess: an interrupt held without the reset lands the running Mac in its debugger.)
  Q14 records the card's first cycle to $400000-$7FFFFF; we found no input sequence in which it changes where the
  table ends up.

**U6: clocks, SCC spacing, the FPU and the caches.** Clocked on C8M's rising edges, except the clock buffers:
```
CLK_30   = C16M                                   the 68030's clock (pin 19)
SM_CLK16 = C16M inverted                          U4's clock (pin 18)
next(/CDIS)=0 when /RESET_30=0, or /CDIS=0 and not (/FPUCS=0 and /AS_30=0)
/BERR_30 = 0 (driven) when /AS_30=0, /FPUCS=0, FPUSENSE=1 and /CDIS=1
N$1 (with Q15, Q16): set at the end of a board cycle that had /SCC.RW.DEC=0, cleared by counting E
```
- **SCC recovery.** After any board cycle to the SCC, N$1 goes high when the cycle ends and holds off the start of the
  next board cycle, of any kind, until E has run through about one and a half periods (16 to 21 C8M clocks, 2.0 to 2.7
  microseconds, in the two E phases modelled). That is the SCC's recovery time, made with E as the time base, for the
  Plus, where no logic spaces SCC accesses; on the SE the gate array does it too (section 3), so there it only costs
  time.
- **No 68882:** FPUSENSE has a pull-up and the 68882's SENSE pin pulls it low; an access to coprocessor space with the
  socket empty gets /BERR_30, and the 68030 turns that into the F-line exception software uses to find no FPU.
- **The caches** are vetoed in hardware (/CDIS low) from every 68030 reset until the first cycle in coprocessor space,
  and then allowed until the next reset, whatever CACR says. So the SE's ROM boots uncached, and the caches can work
  only once something has touched the FPU's space. *Inferred:* the card's software (GemStart) or the system's FPU test
  does that; not checked in GemStart's code.

**U7: a C16M for the Plus.** `C16M = C8M xor (C8M delayed by four GAL delays)`, the delay made by three outputs looped
back to inputs (pins 16 to 4, 15 to 5, 14 to 6) and a fourth output: a pulse on each C8M edge, a 15.67 MHz clock locked
to C8M with a duty cycle set by gate delays. The Plus's 68000 socket has no C16M. The SE's slot has one (A29), so the SE
boards leave U7 out (the universal board's SJ2 links the slot's C16M instead; Bolle: "U7 is only needed for operation
in Macintosh Plus and Classic").

### 10.3 What it tells Aurora68's card (a precedent, not a measurement)

1. **The 68000 after reset (open question 1).** The Performer never asserts /BGACK and never releases /BR: it asserts
   /BR at the start of every reset, holds it for as long as it runs, and drives the bus one or two C8M clocks after
   /RESET rises without waiting for /BG. A shipped product and today's clones therefore show a 68000 coming out of
   reset with /BR asserted staying off the bus, which is what section 4 needs. Two things follow for Aurora68's card:
   keep **/BR asserted for as long as the card owns the bus**, not only until /BGACK is asserted as in section 4's step
   3 (asserting /BGACK as well does no harm, and section 9's logic already holds the arbitration across resets); and,
   as cheap insurance the Performer does not take, wait for /BG after a reset before driving the strobes. Still to be
   measured on an SE: the time from /RESET rising to /BG.
2. **C8M and E.** The whole card runs on C16M, the 68030 included; C8M is an input that gives the phase, so that /AS
   falls only in C8M's high half (S2) and never near S3/S4. /DTACK is sampled at every C16M falling edge and read data
   is latched in the middle of S7, a little after a 68000. E is sampled on C16M's falling edges: /VMA when /VPA and E
   low, the end of the cycle counted against E's fall. For Aurora68's CPLD: clocking from C16M (A29) with C8M as a
   phase input gives section 4's S-states directly; /PMCYC is not needed (the Performer leaves it unconnected); and
   /VMA's lead before E rises is worth checking against the 6522's set-up times rather than copying the Performer's
   first-sample rule.
3. **/DTACK, not /EXT.DTK.** The Performer ends board cycles on the board's /DTACK (C6). It only reads /EXT.DTK (B28),
   with its own pull-up, and never drives it, as section 3 plans.
4. **The shared /RESET and /HALT.** The Performer avoids the problem rather than solving it: it reads /RESET, leaves
   /HALT unconnected, never resets the board (its 68030's RESET instruction stays on the card), and survives every SE
   reset by requesting the bus during it. Aurora68 does reset the board (section 2, item 5), and that resets the 68000
   too; the precedent says that holding /BR through such a pulse is enough to keep the 68000 off the bus after it.
5. **Stock mode.** The Performer has one (the disable switch above): /BR released, everything three-stated, its
   processor held in reset, until the next reset. Section 4's stock mode keeps the card off until the next power-on or
   request; either works electrically.
6. **Differences to keep in mind.** The Performer does not connect /BERR (B21), so the gate array's SCSI time-out never
   reaches it; Aurora68 plans to report it (section 2, item 6). It has no bus buffers, which section 3's loading table
   and Apple's two-inch rule argue against for a bigger card.

### 10.4 The Plus piggyback and the SE slot boards

| | Polyphloisboisterous '030 (Plus) | Performer SE (slot) |
|---|---|---|
| Connection | a 64-pin socket soldered on top of the 68000 (IC1) | J1, a DIN 41612 3x32 in the slot |
| C16M | made by U7 from C8M | the slot's (A29); no U7 |
| 68000 pins left open | BG, BGACK, HALT, BERR | /BG, /BGACK, /HALT, /BERR, /PMCYC |
| FPU | none | optional 68882 with its own oscillator, or on CLK_30 (SJ1) |
| GALs | U3-U7, the same fuse maps | U3-U6, the same fuse maps |

The arbitration, reset, cycle and E logic is the same: on the Plus /BR_00 goes to the 68000's pin 13 directly; in the
SE it goes to the slot's C3. The universal Performer board carries all three connectors at once.

**Whose design.** The files carry no maker's mark (the fuse maps' signature bytes are empty; the schematic is titled
"MM Performer"). The Polyphloisboisterous README calls the board "logically identical" to Total Systems' Mercury, and
the Performer's software is Total Systems' GemStart (docs/02); Steve's Blog says the Performer was Dove's MaraThon
Racer under another name. docs/02 found one link between the two names: the MaraThon Racer's software includes an INIT
called "Mercury Cache Enable". Its probe (a MOVES read at $EC398000 in function code 3) would be an ordinary RAM cycle
on these GALs, so the hardware neither confirms nor rules out which lineage is right.

## Sources

- Apple, *Guide to the Macintosh Family Hardware*, 2nd edition, Figure 14-2, "An expansion card in the Macintosh SE
  assembly": the card flat above the logic board under the chassis, and the suggested connector for a cable to the
  external port.
- Chris Adams, "Mac SE port pinouts", The Mac SE Support Pages, May 1993: the SE's internal system expansion slot,
  pin by pin, with signal descriptions (section 3). Read from the mirror
  https://machut.net/mirrors/www.ccadams.org/se/pinouts.html (sent by the owner, 24 November 2026).
- Apple Computer, *Designing Cards and Drivers for the Macintosh Family*, 3rd edition (1992), sent by the owner:
  chapter 13 (Figure 13-1 the SE's slot pinout, page 284; Table 13-1 signals and loading, pages 285-287; coprocessor
  bus takeover and E; Figures 13-2 and 13-3 RAM timing; the address map; Table 13-3 the power budget, page 298) and
  chapter 17 (Figures 17-1 to 17-8: the card's outline and height, the connectors, the cable and connector card).
  Scan: https://www.vintageapple.org/inside_o/pdf/Designing_Cards_and_Drivers_for_the_Macintosh_Family_3rd_Edition_1992.pdf
- Apple, *Guide to the Macintosh Family Hardware*, 2nd edition: the SE's bus, BBU and VIA (docs/18 cites the
  pages it used); chapter 14, the signal tables of the 96-pin 68000 slot and the 120-pin 68030 slot.
- AllPinouts, "Apple Macintosh Processor-Direct Slot (PDS)" (updated 29 June 2026): the 120-pin 68030 slot (SE/30,
  II family) with the IIfx's differences; not the SE's slot.
- Motorola, *MC68000 User's Manual*: bus arbitration, the halt state, 6800 peripheral cycles.
- Apple technote HW 12, "Macintosh Portable PDS Development": the Portable's slot, which shares the connector and
  most names with the SE's and is not the SE's.
- `reference/MacSE-PiStorm-DirectSocket` (Frimples): arbitration RTL and testbenches for the socket.
- The PiStorm (captain-amygdala/pistorm) and Emu68's `src/pistorm/` (the classic protocol, Pi 4 GPIO settings,
  the Efinix images).
- The MicroMac Performer's reverse-engineered design (section 10), all three with PCB files under CC BY-NC-SA and the
  same five dumped GAL16V8 fuse maps: Bolle, github.com/TheRealBolle/Performer-SE-PL-CL (Eagle schematic "MM
  Performer.sch", universal Plus/Classic/SE board, 17 November 2022); alxlab-zone66x,
  github.com/alxlab-zone66x/Performer-SE-PL-CL (the KiCad conversion and the SE-only boards); luRaichu,
  github.com/luRaichu/polyphloisboisterous030 (Polyphloisboisterous '030, the Plus version, Rev. 1 December 2025).
  Read on 25 November 2026; decoded with MAME's jedutil.
- Motorola, *MC68000 8-/16-/32-Bit Microprocessors User's Manual* (the copy in the Polyphloisboisterous repository):
  5.2 bus arbitration (Figures 5-15 to 5-24, the three- and two-wire state diagrams in Figure 5-18), 5.5 reset,
  Figure 5-35 the synchronous read cycle.
- TinkerDifferent, "Blazing fast Mac: the Performer PDS 68030 accelerator by MacEffects"
  (https://tinkerdifferent.com/threads/blazing-fast-mac-the-performer-pds-68030-accelerator-by-maceffects.3902/page-2):
  owners' reports that the clone's 25 and 33 MHz crystals clock only the FPU, and that it is built from Bolle's files.
- Forum discussion of a slot card for the SE: TinkerDifferent, "PiStorm on the Macintosh SE"
  (https://tinkerdifferent.com/threads/pistorm-on-the-macintosh-se.2085/page-2).
- In this repository: docs/00 (the original plan), docs/07 (the Alice68 drivers), docs/12 (transport, power),
  docs/18 (the compact Macs' bus and the PiStorm's cycle), docs/21 (Bluetooth on the Pi 3), `hardware/pds-card/`
  (the first Card P design, section 9).
