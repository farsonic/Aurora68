<!-- SPDX-License-Identifier: MPL-2.0 -->
# Card P's CPLD

| File | What |
|---|---|
| `aurora68_pds.v` | the design (MIT, derived from the classic PiStorm's `rtl/pistorm.v`; its notice is kept in the file) |
| `aurora68_pds.qsf` | Quartus pin assignments, written by `tools/gen_kicad.py` from the same table as the schematic |
| `aurora68_pds.sdc` | the 200 MHz clock; every slot input is synchronised, so the inputs are false paths |
| `tb/tb_card.v` | the testbench: a behavioural Macintosh SE (C16M, C8M, E, the 68000 still in its socket, the gate array's /DTACK, /VPA, /BERR and video hold-off, RAM, a VIA), the card's latches, and the Pi's register sequences from `psmac_gpio.c` |
| `run_tests.sh` | runs the testbench, then four broken copies of the design that it must catch |
| `../PROTOCOL.md` | the register map |

## What it does

The classic PiStorm's cycle machine (docs/18 section 4) with the changes docs/18 C2 asks for and the slot's own
needs (docs/22 sections 2-4 and 10): bus arbitration with /BR held for as long as the card owns the bus and
through every reset, as the MicroMac Performer did it, plus /BGACK after /BG as insurance; 6800 cycles against
the board's E, which is an input here; /AS only on C8M rising edges (S2); read data latched on the falling edge
entering S7, where a 68000 latches, with PI_TXN_IN_PROGRESS dropped only after; /BERR ending a cycle and reported;
stock mode from the kernel or from the programmer's Interrupt switch held through a reset (in C8M's clock domain,
so it works with no Pi); a fan PWM and tachometer; status bits. /PMCYC is no longer used. The file's header has
the detail and the reasons.

## Simulated (25 November 2026)

Icarus Verilog 13.0 (installed from conda-forge into a scratch directory, not part of the repository):

    ./run_tests.sh          # IVERILOG=... VVP=... if they are not on PATH

**1608 checks pass, 0 fail** (most are the continuous /BR watch, one per C8M clock while it is on): power-up
(/RESET and /BR held by the card, nothing driven, fan on); the kernel's start sequence (INIT, the reset pulse)
ending with the card holding /BGACK **and /BR**; word and byte reads and writes to RAM with video hold-off and
garbage on the data bus until 15 ns before the 68000's latch point; a ROM-space read; VIA reads and writes through
/VPA and the board's E; /BERR; **/BR held on every C8M clock across a kernel reset pulse, the SE's own reset and the
cycles between**, with a 68000 that grants at once after reset and with one that runs a cycle of its own first (the
card waits for /BG; the two never drive together); stock mode from the kernel and back; **stock mode from the
Interrupt switch held through the SE's reset**: /BR let go during the reset, the SE's 68000 running its ROM after it,
still so after Interrupt is let go, and the card back with the next reset; **cancelled** when Interrupt is let go
before Reset (/BR asserted again at once, the card owns the bus after the reset); **with the Pi's clock stopped**
and the card holding /RESET as at power-up, Interrupt alone makes it let go of /RESET and /BR and the 68000 runs,
and once the clock returns the status shows the switch's stock mode and the next reset brings the card back; no SE
power; the fan's duty and tachometer; the status bits. Throughout, the testbench checks that /AS falls only 0-45 ns
after a C8M rising edge (S2, never S3 or S4), that the address is stable 15 ns before /AS, that /VMA falls while E
is low at least 200 ns before E rises and rises after E falls, and that the card never drives the strobes while the
68000 does. `run_tests.sh` then breaks the design four times (the read latch closed straight after /DTACK, as the
classic machine effectively does; /AS on a falling edge; /BR released once /BGACK is asserted, the earlier
arbitration; an Interrupt let go before the reset ends not cancelling) and confirms the testbench fails all four.

**/VMA against the VIA.** /VMA falls on a C8M falling edge while E is low with at least two C8M clocks (255 ns)
before E rises, less the synchroniser and switch delays (about 20 ns): about 235 ns. A 1 MHz 6522 wants chip select
and register select stable about 180 ns before its clock rises (R6522 data sheet, tACR/tACW, from memory: check the
part the SE actually carries). The Performer's GAL can assert /VMA as little as one C16M half-period before E rises
(docs/22 section 10.2), which is why this card does not copy its rule.

What the simulation does not show: the real SE's timing (the model's delays are round numbers from Apple's text,
not measurements), metastability (the synchronisers are the usual two and three stages), the CBTD3384's and the
latches' propagation delays (4 ns in the model for the latches, 0 for the switches), and how a real 68000 behaves
coming out of reset with /BR asserted (docs/22 open question 1): the Performer's working is evidence that it stays
off the bus, both behaviours are modelled and handled, and a scope on a real SE has the last word.

## Size

No Quartus here, so no fit. yosys 0.71 (also from conda-forge, removed afterwards) mapped the design to generic
4-input LUTs: **145 LUTs and 117 flip-flops** on 25 November 2026, down from 154 and 122 (`synth -flatten -lut 4`,
with PI_D's input side given its own port for the estimate, since yosys otherwise reads the bidirectional bus back
through its own driver). A MAX II logic element is one 4-input LUT and one flip-flop, so the design needs between
145 LEs (every flip-flop packed with a LUT) and about 220 (few packed). The EPM240 has 240: it probably fits. The board is wired like the PiStorm Rev B's
for an EPM570 (its pins 37, 39, 88 and 90 are power), which an EPM240 also fits, so if the fitter says no the
EPM570T100C5N goes in the same footprint. To cut LEs if needed: the fan PWM (24 LEs or so) and the tachometer
(8) are the first candidates.

I/O: 59 pins (the PiStorm's 59 less CLK_SEL, C1, C3 and its /HALT pin 68, plus SLOT_PWR, FAN_PWM, FAN_TACH,
LED_ACT), of 80 on the EPM240 and 76 on the EPM570. Pins 68 (/PMCYC until 25 November) and 82 (the stock switch)
are free. The design has a second, small clock domain on C8M (pin 62, a global clock pin) for the Interrupt-switch
stock mode; Quartus will want C8M declared as a clock (`aurora68_pds.sdc`).

## Building it in Quartus (not done)

Quartus Prime Lite with MAX II support: open `aurora68_pds.qsf` (device EPM240T100C5; change to EPM570T100C5 if
it does not fit), compile, then `quartus_cpf -c -q 100.0kHz -g 3.3 -n p output_files/aurora68_pds.pof
aurora68_pds.svf` for an SVF that Emu68's `ps_cpld_load` or `pistormflash` can program over the Pi's GPIO 24-27,
as on a PiStorm. Check the timing report for the 200 MHz domain: the classic design meets it on a C5 part.
