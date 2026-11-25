<!-- SPDX-License-Identifier: MPL-2.0 -->
# Card P's CPLD

| File | What |
|---|---|
| `aurora68_pds.v` | the design (MIT, derived from the classic PiStorm's `rtl/pistorm.v`; its notice is kept in the file) |
| `aurora68_pds.qsf` | Quartus pin assignments, written by `tools/gen_kicad.py` from the same table as the schematic |
| `aurora68_pds.sdc` | the 200 MHz clock; every slot input is synchronised, so the inputs are false paths |
| `tb/tb_card.v` | the testbench: a behavioural Macintosh SE (C16M, C8M, E, the 68000 still in its socket, the gate array's /DTACK, /VPA, /BERR and video hold-off, RAM, a VIA), the card's latches, and the Pi's register sequences from `psmac_gpio.c` |
| `run_tests.sh` | runs the testbench, then two broken copies of the design that it must catch |
| `../PROTOCOL.md` | the register map |

## What it does

The classic PiStorm's cycle machine (docs/18 section 4) with the changes docs/18 C2 asks for and the slot's own
needs (docs/22 sections 2-4): bus arbitration with /BR, /BG and /BGACK, held across resets; 6800 cycles against
the board's E, which is an input here; /AS only on C8M rising edges (S2), optionally aligned with /PMCYC; read
data latched on the falling edge entering S7, where a 68000 latches, with PI_TXN_IN_PROGRESS dropped only after;
/BERR ending a cycle and reported; stock mode; a fan PWM and tachometer; status bits. The file's header has the
detail and the reasons.

## Simulated (24 November 2026)

Icarus Verilog 13.0 (installed from conda-forge into a scratch directory, not part of the repository):

    ./run_tests.sh          # IVERILOG=... VVP=... if they are not on PATH

154 checks pass: power-up (/RESET and /BR held by the card, nothing driven, fan on); the kernel's start sequence
(INIT, the reset pulse) ending with the card holding /BGACK and /BR released; word and byte reads and writes to
RAM with video hold-off and garbage on the data bus until 15 ns before the 68000's latch point; a ROM-space read;
VIA reads and writes through /VPA and the board's E; /BERR; a kernel reset with a 68000 that honours /BGACK after
reset and with one that does not (it runs a cycle; the card waits, takes the bus back at /BG, and the two never
drive together); the SE's own reset; /PMCYC-aligned RAM cycles; stock mode from the kernel and from the switch and
back; no SE power; the fan's duty and tachometer; the status bits. Throughout, the testbench checks that /AS falls
only 0-45 ns after a C8M rising edge (S2, never S3 or S4), that the address is stable 15 ns before /AS, that /VMA
falls while E is low at least 200 ns before E rises and rises after E falls, and that the card never drives the
strobes while the 68000 does. `run_tests.sh` then breaks the design twice (the read latch closed straight after
/DTACK, as the classic machine effectively does; /AS on a falling edge) and confirms the testbench fails both.

What the simulation does not show: the real SE's timing (the model's delays are round numbers from Apple's text,
not measurements), metastability (the synchronisers are the usual two and three stages), the CBTD3384's and the
latches' propagation delays (4 ns in the model for the latches, 0 for the switches), and above all how a real
68000 behaves when it comes out of reset with /BGACK already asserted (docs/22 open question 1): both behaviours
are modelled and handled, but which one the SE's 68000 shows needs a scope.

## Size

No Quartus here, so no fit. yosys 0.71 (also from conda-forge, removed afterwards) mapped the design to generic
4-input LUTs: **154 LUTs and 122 flip-flops** (`synth -flatten -lut 4`). A MAX II logic element is one 4-input LUT
and one flip-flop, so the design needs between 154 LEs (every flip-flop packed with a LUT) and about 230 (few
packed). The EPM240 has 240: it probably fits, without much room. The board is wired like the PiStorm Rev B's
for an EPM570 (its pins 37, 39, 88 and 90 are power), which an EPM240 also fits, so if the fitter says no the
EPM570T100C5N goes in the same footprint. To cut LEs if needed: the fan PWM (24 LEs or so) and the tachometer
(8) are the first candidates.

I/O: 61 pins (the PiStorm's 59 less CLK_SEL, C1 and C3, plus /PMCYC on the old /HALT pin, SLOT_PWR, FAN_PWM,
FAN_TACH, STOCK_SW_n, LED_ACT), of 80 on the EPM240 and 76 on the EPM570.

## Building it in Quartus (not done)

Quartus Prime Lite with MAX II support: open `aurora68_pds.qsf` (device EPM240T100C5; change to EPM570T100C5 if
it does not fit), compile, then `quartus_cpf -c -q 100.0kHz -g 3.3 -n p output_files/aurora68_pds.pof
aurora68_pds.svf` for an SVF that Emu68's `ps_cpld_load` or `pistormflash` can program over the Pi's GPIO 24-27,
as on a PiStorm. Check the timing report for the 200 MHz domain: the classic design meets it on a C5 part.
