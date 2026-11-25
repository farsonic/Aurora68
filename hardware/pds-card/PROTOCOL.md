<!-- SPDX-License-Identifier: MPL-2.0 -->
# Card P's CPLD: the Pi-side protocol

*24 November 2026; arbitration and stock mode changed 25 November 2026 (below).* Card P's CPLD (`rtl/aurora68_pds.v`) speaks the classic PiStorm's protocol unchanged, so the
kernel's transport (`emu68/src/psmac/psmac_gpio.c`) and Emu68's `ps_classic_protocol.c` drive it as they drive a
PiStorm in the 68000's socket. What the slot card adds sits where the classic CPLD has nothing: bits 0-12 of the
status register on reads (the classic CPLD returns zeros there) and status-register writes with bit 15 set (which
the classic kernel never makes: it writes 0, 1 and 2). Nothing here has run on hardware; the register behaviour is
what `rtl/tb/tb_card.v` checks in simulation.

## The wires (unchanged)

| GPIO | Signal | Direction | Use |
|---|---|---|---|
| 0 | PI_TXN_IN_PROGRESS | CPLD to Pi | high from the ADDR_LO write until the bus cycle has finished (and, for a read, the data is latched) |
| 1 | PI_IPL_ZERO | CPLD to Pi | high while the board's interrupt level is 0 |
| 2, 3 | PI_A0, PI_A1 | Pi to CPLD | register select |
| 4 | PI_CLK | Pi to CPLD | GPCLK0, 200 MHz: the CPLD's only clock |
| 5 | PI_RESET | CPLD to Pi | low while the board's /RESET is low, unless the CPLD is pulling it itself |
| 6 | PI_RD | Pi to CPLD | read strobe |
| 7 | PI_WR | Pi to CPLD | write strobe |
| 8-23 | PI_D0-PI_D15 | both | data |
| 24-27 | TMS, TDO, TCK, TDI | | the CPLD's JTAG (programming, and the kernel's usercode read, docs/18 K2) |

## Registers (PI_A1, PI_A0)

| Register | Write | Read |
|---|---|---|
| 0 REG_DATA | the write data (into the write latch) | the read latch: the data of the last read cycle |
| 1 REG_ADDR_LO | A0-A15 (into the address latch); starts PI_TXN_IN_PROGRESS | - |
| 2 REG_ADDR_HI | bits 0-7 A16-A23, bit 8 "byte", bit 9 "read": starts the bus cycle | - |
| 3 REG_STATUS | below | below |

### REG_STATUS writes

| Bit 15 | Bits 14-12 | Bits 11-0 | Meaning |
|---|---|---|---|
| 0 | | bit 1, bit 0 | **classic.** Bit 1 = 0 holds the board's /RESET low (the power-up state, as on the PiStorm); bit 1 = 1 releases it. Bit 0 = 1 (INIT) puts the bus-cycle machine back to its start. `psmac_gpio.c` writes 1, 0, then 0 and 2 for each reset pulse. |
| 1 | 000 | bit 0 STOCK | **control.** STOCK = 1: release /BR and /BGACK, three-state everything, pulse /RESET (2.1 ms), let the SE run its own ROM on its own 68000. STOCK = 0: request the bus again (/BR), take it at the next /BG. Bit 1 (until 25 November the /PMCYC alignment) is ignored: /PMCYC is no longer connected. |
| 1 | 001 | bits 3-0 | **fan duty**, 0 (off) to 15 (full): the share of each 30.6 kHz period the FAN_PWM pin is high. It drives the fan's PWM input directly (J3 pin 2, Raspberry Pi 5 pinout), where high is full speed on a standard 4-wire fan. Power-up: 15, the pin held high, which needs no clock: the fan runs before the Pi has started (and while the CPLD is blank, the fan's own pull-up does the same). A fan that ran slower as the duty rose would want `15 - duty`: the tachometer tells the kernel which. |
| 1 | other | | ignored (room for later) |

### REG_STATUS reads

| Bits | Name | Meaning |
|---|---|---|
| 15-13 | IPL | the board's interrupt level (classic) |
| 12 | CARD_PDS | always 1 on Card P; 0 on a socket PiStorm. A kernel can tell the two apart by it (docs/22 section 5: `mac.slot=pds` or the card identifying itself) |
| 11 | GRANTED | the card holds the bus (/BR and /BGACK asserted, outputs enabled). Bus cycles wait until it is set |
| 10 | STOCK | stock mode (the Interrupt switch held through a reset, or a STOCK write) |
| 9 | BERR | the last bus cycle ended with /BERR (the SE's gate array asserts it after about 250 ms with no answer) instead of /DTACK |
| 8 | RESET_LINE | the board's /RESET (and /HALT) line is low now |
| 7-2 | TACH | fan tachometer: a free-running count of falling edges, modulo 64. Read twice and divide the difference by the time between (most fans give two pulses a turn) |
| 1 | STOCK_SW | stock mode was entered from the Interrupt switch held through a reset; the next reset clears it |
| 0 | SE_POWER | the SE's +5 V is present at the slot |

## Arbitration, resets and stock mode (25 November 2026)

After the MicroMac Performer, a shipped 68030 card that took the bus from this slot (docs/22 section 10):

- **/BR is asserted from power-up and held for as long as the card owns the bus**, through every reset (the
  kernel's reset pulses and the SE's own). A 68000 stays off the bus while BR is asserted (MC68000 manual 5.2.1,
  Figure 5-18 (a)); the Performer relied on exactly that. As insurance it did not take, the card waits after
  power-up and after every reset for /BG and an idle bus before it asserts /BGACK (held with /BR) and drives
  anything; GRANTED (bit 11) is set then. During a reset it lets go of /BGACK and keeps /BR.
- **Stock mode without a switch on the card**, as on the Performer: hold the programmer's **Interrupt** switch
  through a **Reset** (press Interrupt, press and release Reset, then release Interrupt). After four C8M clocks of
  reset with Interrupt held the card lets go of /BR and of its own hold on /RESET; if Interrupt is still held when
  the reset ends, it stays off the bus, three-stated, and the SE's own 68000 boots its ROM, until the next reset.
  Letting go of Interrupt before Reset cancels. This logic runs on C8M, not on the Pi's clock, so it works before
  the Pi has started or with no Pi fitted: with no Pi the card holds /RESET from power-up, and holding Interrupt
  alone is then enough. The kernel's STOCK write stays.
- **/PMCYC is not used**, and the cycle machine still runs on the Pi's 200 MHz clock, sampling C8M as the PiStorm
  samples its 68000 clock: /AS falls just after a C8M rising edge (S2, C8M high) and the read latch closes just
  after the falling edge entering S7, the 68000's own point (the Performer, clocked from C16M, latches mid-S7).

## What the kernel would do with it (not written yet)

The kernel works unchanged with Card P: its first bus cycle simply waits until the card holds the bus (a few
microseconds after the reset it pulses at start). The slot step of docs/22 section 5 would add:
1. after `psmac_gpio_init`'s reset pulse, read REG_STATUS; if bit 12 is set, log "PDS card", wait (bounded) for
   bit 11, and log the grant or a clear time-out message;
2. `mac.stock=1` (or a menu item): write `0x8001`, log it, stop using the bus;
3. the fan loop of docs/22 section 6: write `0x9000 | duty` from the SoC temperature; report TACH in the status
   page and `aurora68.py log`;
4. a bus-error report when bit 9 is set after a cycle.
