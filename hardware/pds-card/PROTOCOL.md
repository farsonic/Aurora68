<!-- SPDX-License-Identifier: MPL-2.0 -->
# Card P's CPLD: the Pi-side protocol

*24 November 2026.* Card P's CPLD (`rtl/aurora68_pds.v`) speaks the classic PiStorm's protocol unchanged, so the
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
| 1 | 000 | bit 0 STOCK, bit 1 PMCYC | **control.** STOCK = 1: release /BR and /BGACK, three-state everything, pulse /RESET (2.1 ms), let the SE run its own ROM on its own 68000. STOCK = 0: request the bus again (/BR), take it at the next /BG. PMCYC = 1: start RAM cycles ($000000-$3FFFFF) at a /PMCYC falling edge (docs/22 section 4); 0 (power-up): at the next C8M edge, waiting for /DTACK as a 68000 does. |
| 1 | 001 | bits 3-0 | **fan duty**, 0 (off) to 15 (full). 30.6 kHz PWM. Power-up: 15, full speed, which needs no clock: the fan runs before the Pi has started. |
| 1 | other | | ignored (room for later) |

### REG_STATUS reads

| Bits | Name | Meaning |
|---|---|---|
| 15-13 | IPL | the board's interrupt level (classic) |
| 12 | CARD_PDS | always 1 on Card P; 0 on a socket PiStorm. A kernel can tell the two apart by it (docs/22 section 5: `mac.slot=pds` or the card identifying itself) |
| 11 | GRANTED | the card holds the bus (/BGACK asserted, outputs enabled). Bus cycles wait until it is set |
| 10 | STOCK | stock mode (the jumper, the switch, or a STOCK write) |
| 9 | BERR | the last bus cycle ended with /BERR (the SE's gate array asserts it after about 250 ms with no answer) instead of /DTACK |
| 8 | RESET_LINE | the board's /RESET (and /HALT) line is low now |
| 7-2 | TACH | fan tachometer: a free-running count of falling edges, modulo 64. Read twice and divide the difference by the time between (most fans give two pulses a turn) |
| 1 | STOCK_SW | the stock jumper or switch is closed (debounced) |
| 0 | SE_POWER | the SE's +5 V is present at the slot |

## What the kernel would do with it (not written yet)

The kernel works unchanged with Card P: its first bus cycle simply waits until the card holds the bus (a few
microseconds after the reset it pulses at start). The slot step of docs/22 section 5 would add:
1. after `psmac_gpio_init`'s reset pulse, read REG_STATUS; if bit 12 is set, log "PDS card", wait (bounded) for
   bit 11, and log the grant or a clear time-out message;
2. `mac.stock=1` (or a menu item): write `0x8001`, log it, stop using the bus;
3. the fan loop of docs/22 section 6: write `0x9000 | duty` from the SoC temperature; report TACH in the status
   page and `aurora68.py log`;
4. a bus-error report when bit 9 is set after a cycle.
