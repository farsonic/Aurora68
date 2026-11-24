# The compact Macintosh's bus: why the 128K, 512K, 512Ke and Plus are harder than the SE

22 November 2026. A research record, written the evening the first field reports from a Plus-class machine
came in. Nothing here was measured on hardware by the project; it is what the sources say, what the
PiStorm's own logic does, and what follows from putting the two side by side. Where a statement rests on a
source, the source is named beside it; where it is this document's reasoning, it says **(inference)**.

## Backstory

The SE has been a good host for Aurora68: on the owner's SE the classic PiStorm (Rev B, EPM240 CPLD,
74LVC16373A latches, docs/12) drives the screen, the speaker, SCSI, the floppy and the ADB keyboard and
mouse through the SE's VIA, with nothing in the bus path that needed attention after a solder bridge was
found (docs/12, "Stripes over the top hundred rows").

The Plus, 128K, 512K and 512Ke came in on 22 November (docs/08 item 96): a board layer for them in the
MAME testbed the same day, and a tester's machine that evening. That machine is a 512Ke, run with
`mac.board=plus` (its 512 KB repeats through the Plus's address range). The owner later added two facts:
the PiStorm sits in a **socket** fitted to the logic board in place of the soldered 68000 (not a clip), and
the machine is believed to be a **new build**, most likely a reproduction logic board with new sockets,
not a 1980s original. What it showed:

| Symptom | Numbers | Reading |
|---|---|---|
| System 7.0.1 starts from a disk image on the card; keyboard and mouse work | | most bus cycles work |
| The kernel copies the screen into the board's RAM with 16-bit writes | occasional single wrong words (short dotted marks in the desktop pattern, gone when the row is rewritten) | writes nearly always land |
| The kernel reads those rows back with 16-bit reads | 6,011 of 6,605 rows differed, about 29 of 32 words a row | reads of the board's RAM are mostly wrong |
| The mouse's quadrature lines (X1/Y1 in the SCC's RR0, X2/Y2 and the button in VIA port B) | Y1 changed tens of thousands of times more than Y2; 107,004 Y1 against 5,566 Y2 changes in one log; X1 4,782 against X2 4,768 | X1 reads believable, Y1 not |
| The keyboard (VIA shift register and IFR) and the button (VIA PB3) | 34,076 keyboard "keys" and 114,431 mouse "clicks" in a few minutes | VIA reads unreliable too |
| A faint whine from the speaker | stays with the sound buffer filled with one level; the machine has an LCD conversion, no CRT | not the buffer's content |

The question this document answers: what is different about these boards' 68000 bus, what the PiStorm does
on that bus, and what the kernel, the CPLD and the hardware can each do about it.

The short answer, argued below: the classic PiStorm's current CPLD firmware takes read data **one CPU clock
earlier than a 68000 does** whenever /DTACK is already asserted at the start of S4 (it samples /DTACK on
the rising clock edge and closes its read latch on the next falling edge, where a 68000 samples /DTACK on
the falling edge and latches data one clock later). The SE's glue chip hides that; the Plus-era PALs do
not, because they assert /DTACK for the ROM, the SCC and the IWM the moment the address is decoded and
rely on the 68000's fixed latch point, and (as far as the published reverse engineering shows) assert it
for RAM before the DRAM's data is on the bus. The Plus and earlier also need 2.2 microseconds between SCC
accesses, which nothing on the board enforces and the kernel's mouse poll does not leave. A reproduction
board can make both worse (faster PLDs, a CMOS VIA with CMOS input levels). The fixes, ranked, are at the
end, with the measurements that tell the hypotheses apart.

## The sources, and how they were read

| Source | What it is | How used |
|---|---|---|
| Apple, *Guide to the Macintosh Family Hardware*, 2nd ed. (1990), archive.org item `Guide_to_the_Macintosh_Family_Hardware_Second_Edition` (OCR text) | Apple's hardware reference | cited by page; not committed |
| Motorola, *M68000 8-/16-/32-Bit Microprocessors User's Manual*, Rev. 8 (1993), bitsavers mirror (`components/motorola/68000/68000/M68000UM_AD...Rev8_1993.pdf`) | the 68000's bus cycle and AC timing | cited by section and page |
| captain-amygdala/pistorm, `rtl/pistorm.v` at `88937a6` (2025-09-15), and its history | the classic PiStorm's CPLD source | read and quoted by line |
| michalsc/Emu68: `src/pistorm/cpld.c`, `ps_classic_protocol.c`, `firmware/pistorm-classic/` (commits `20d5f73`, `87edb0a`, 25 October 2026) | Emu68's classic transport and the firmware images it ships | usercode table, firmware list |
| Frimples/PiStorm-Firmware (README, `rtl/*.svf`) | a bundle of every classic PiStorm firmware, with one-line descriptions | what `original`, `long_hold`, `long_data`, `PiStormX_basic` are |
| Frimples/MacSE-PiStorm-DirectSocket (`pistorm.v`, `docs/HARDWARE_TEST_LOG.md`, September 2026) | an SE-specific CPLD firmware | its timing audit and changes |
| Frimples/pistorm-macintosh (AlexCL's SE fork, README) | the 2022-2024 Musashi-on-SE port | "stock firmware has video issues with the SE" |
| Andrew Makousky, `mac128pal.v` in Macintosh-HW/macsehw (git.applefritter.com, 2020, CC0) | Verilog model of the 128K/512K PALs from the Unitron clone's reverse-engineering documentation; the author says it is "not identical" and parts are marked FIXME | the only published equations found; treated as a model, not as Apple's design |
| MAME `src/mame/apple/mac128.cpp` (0.264, header comments) | the emulator's notes on these machines | address map, VPA, raster timing |
| twvd/snow PR #400 (ozbenh, 2026) and source (`core/src/mac/compact/`) | a cycle-level Mac emulator's notes | SCC strobes, phase read decode |
| Big Mess o' Wires, "68000 Interleaved Memory Controller Design" (25 August 2011) | Plus Too's author on the Plus's memory sharing | his beliefs, labelled as such |
| max234252/ITXPlus (schematic PDF), max234252/MacPlus-Reloaded, RichCini/MacRedux; TheRealBolle/SE30 README; hkzlab/MiniPerformerPlus README; tinkerdifferent ITXPlus thread | reproduction boards and a Plus accelerator clone | what new boards are made of, and what their builders found |
| WDC W65C22 data sheet (westerndesigncenter.com, 2010 edition) | the CMOS VIA sold today | input levels and data delay |
| Zilog Z85C30 Customer Procurement Specification DC-4158-02 (jlab.org mirror) | the CMOS SCC | recovery time, read delay, errata |
| TidBITS, "For Little Macs" (15 July 1991); Low End Mac on the Brainstorm | a period accelerator | what an accelerator had to replace |

Not read, and why: Wouter de Waal's PAL notes (retro.co.za) answer automated clients with a "bot check"
page whose author asks bots to stay away, and that was respected; the PLD Archive wiki (the reproduction
PAL files) is behind a Cloudflare challenge; 68kmla.org threads are behind an Anubis challenge. A person
with a browser should read all three (the PLD Archive's Plus page lists which GAL types the recreated PALs
were tested in). *Inside Macintosh* vol. III was not consulted: the *Guide* covers what was needed. No
Apple ROM and no Apple software was downloaded.

## 1. The 68000 read cycle the boards were designed around

The 68000's bus cycle is eight half-clock states, S0 to S7; even states begin on a rising edge of CLK, odd
states on a falling edge. For a read (M68000 UM, section 5.1.1, p. 5-4): the address is driven entering
S1; /AS and /UDS, /LDS are asserted on the rising edge of S2; during S4 the processor waits for /DTACK,
/BERR or /VPA, and "if neither termination signal is asserted before the falling edge at the end of S4,
the processor inserts wait states (full clock cycles)"; "on the falling edge of the clock entering state 7
(S7), the processor latches data from the addressed device and negates AS". The note beside it: "VPA and
BERR are sampled on every falling edge of the clock beginning with S4, and data is latched on the falling
edge of S6 during a read cycle."

Two rules make that a design tool (section 5.7, p. 5-33; section 5.8, pp. 5-38 to 5-39; table notes in
section 10): a signal that meets the asynchronous setup time (#47, 10 ns at 8 MHz) "is guaranteed to be
recognized at the next falling edge"; and "if DTACK is recognized on a falling edge, valid data is latched
into the processor (during a read cycle) on the next falling edge, provided the data meets the setup time
required (parameter #27)". In a "pseudo-asynchronous system, timing specifications allow DTACK to be
asserted for a read cycle before the data from a slave device is valid", by up to #31. So a board may
assert /DTACK as early as it likes, and only has to have its data on the bus 10 ns before the falling edge
one clock after /DTACK was recognized. The Plus-era PALs use exactly that freedom (section 2).

At the Mac's 7.8336 MHz one clock is 127.66 ns, one state 63.83 ns. The 8 MHz figures of the AC table
(section 10, pp. 10-10 to 10-11): address valid to /AS asserted (#11) 30 ns minimum; data-in valid to clock
low (#27) 10 ns; /AS negated to data-in invalid (#29, hold) 0 ns; /DTACK asserted to data-in valid (#31)
90 ns maximum; asynchronous input setup (#47) 10 ns.

| Edge (from the rising edge that begins S2) | 68000 |
|---|---|
| -64 ns (S1 begins) | address valid |
| 0 (S2) | /AS, /UDS, /LDS asserted (#9: 3-60 ns after the edge) |
| +128 ns (S4 begins, rising) | nothing sampled |
| +191 ns (S4 ends, falling) | /DTACK sampled (needs it by +181 ns) |
| +319 ns (S6 ends, falling) | **data latched** (needs it by +309 ns); /AS negated |

The 6800-style cycle (Appendix B, pp. B-1 to B-2): E is the CPU clock divided by ten, "low for six system
clocks and high for four"; on /VPA the processor waits for E low, asserts /VMA, the peripheral transfers
while E is high, and "on a read cycle, the data is latched as E goes low internally". At 7.8336 MHz E is
783.36 kHz (the *Guide*, p. 149), high for 510.6 ns.

## 2. The 128K, 512K, 512Ke and Plus logic board

**General logic.** These machines have no glue chip (the IWM and the Sony sound chip are their only custom
parts): PALs "decode addresses ..., assert the device-select
signal ..., generate the RAM address strobes (/RAS and /CAS) and control the multiplexers ..., control the
timing and sequence of video functions", the sound and disk-speed PWM, and "generate the 7.8336 MHz clock
used by the main processor" and the SCC's 3.672 MHz (*Guide*, pp. 111-112). The 128K/512K set is TSM
(timing state machine, 16R4), LAG (linear address generator, 16R8), BMU0 and BMU1 (bus management),
TSG (timing signal generator, 16R6) and ASG (16R8) (`mac128pal.v`); the Plus has BMU2, a 20R4, where the
older boards have BMU0 (ITXPlus schematic; the PLD Archive's listing as summarised by a search engine).
"The PALs respond to any address in the range $00 0000 through $DF FFFF with a /DTACK signal and to any
address in the range $E0 0000 through $FF FFFF with a /VPA signal" (*Guide*, p. 122; the scan's OCR has
"ATA" for /VPA). MAME's header: "The MC68000's FC outputs are not used even for autovectoring. The
VIA's address range is overdecoded to generate VPA for both." The clock comes from a 15.6672 MHz
oscillator (C16M); the CPU clock is C16M divided by two in a PAL register.

**Sharing the RAM.** "The main processor's RAM-access cycles are interleaved with the video display's access
cycles. The video display access cycles occur only during the active portion of a screen scan line" (*Guide*,
p. 194). The raster: 512 visible pixels (32.68 us) and 192 of horizontal blanking (12.25 us) a line, 28 lines
of vertical blanking and 342 displayed (MAME, quoting Apple's BBU ERS, which describes the SE, whose
raster is the Plus's **(inference)**); one sound word is fetched per line, at the end of horizontal
blanking, 370 a frame, 22.2545 kHz (*Guide*, pp. 430-431). The measured cost: Table 5-3 (*Guide*, p. 194) gives an average RAM access rate
of 2.56 MB/s for the Plus and 3.22 MB/s for the SE at the same 7.83 MHz; a 68000 with no wait states moves
2 bytes every 4 clocks, 3.92 MB/s, so the Plus averages about 6.1 clocks per RAM word and the SE about 4.9
(arithmetic on Apple's figures). How the slots are cut is not documented by Apple. Big Mess o' Wires
(2011) believed "four clock cycles of CPU access followed by four clock cycles of video access", from a
MacTech article, and "more likely that it used the 68000 /DTACK line to insert CPU wait states"; Snow
models it that way ("While the video circuitry is accessing the RAM, the memory controller de-asserts
DTACK", `core/src/mac/compact/video.rs`). In the `mac128pal.v` model the video slot (`s1`) needs both the
2 MHz and the 1 MHz counters high, one video word per 1 MHz period (eight CPU clocks, sixteen pixels).

**When /DTACK comes.** The model's TSM equation for /DTACK has four terms (comments translated from the
Unitron documentation by the model's author):

    dtack = ~( ~romen                    // "if the ROM is 250 nS or SCC or IWM"
             | ~ras & ~ramen & ~s1       // "guarantees that it will be recognized on the falling edge of pclk in state s5"
             | ~as & ~dtack & ramen      // "expects as to rise for disable"
             | ~as & ~dtack & ~s1);      // "but avoid video cycles"

and BMU1 makes `romen` from /AS and the address alone (it covers $400000, the overlay, $800000-$BFFFFF and
$C00000-$DFFFFF, "for generating DTACK (not accessing ROM)"). Read literally, and with the caution the
model asks for:

- for the **ROM, SCC and IWM**, /DTACK is combinational on /AS and the address: it is asserted within two
  PAL delays of /AS, in S2. The device's data need only be valid at the end of S6, two clocks after /AS:
  "if the ROM is 250 nS" is that budget. These parts are timed by the 68000's fixed latch point, not by
  /DTACK. **(inference from the model and the UM)**
- for **RAM**, /DTACK follows /RAS in a CPU slot. /RAS is a registered output clocked by C16M, so it moves
  at a CPU clock edge; /CAS follows /RAS by up to one C16M period (`s0`), and the DRAM's data reaches the
  CPU through a pair of 74LS245s (enabled by BMU0's `g244` while the CPU reads RAM). So /DTACK for RAM
  comes before the data, by about a /CAS delay plus the DRAM's access from /CAS plus a '245; the
  comment says the design aims for the 68000 to see /DTACK at the falling edge, and the 68000 latches a
  clock after that. **(inference)**
- the *Guide* says the same in Apple's words for the SCC: "On the Macintosh Plus and earlier Macintosh
  models, it is necessary to let the SCC lines stabilize for 2.2 us between accesses. On later models, it
  is not necessary to do so because the general logic IC (GLU or BBU) delays the acknowledge signal
  (/DTACK or /DSACK0) until the SCC lines have stabilized" (pp. 363-364).

**The phase.** "At system startup, the operating system reads an address in the range $F0 0000 through
$F7 FFFF (labeled Phase read ...) to determine whether the computer's high-frequency timing signals are
correctly in phase. When the timing signals are not in phase, RAM accesses are not timed correctly, causing
an unstable video display, RAM errors, and VIA errors. A word-wide access to any SCC address causes a phase
shift in the processor clock, and is used by the operating system to correct the phase when necessary"
(*Guide*, p. 122; repeated p. 365). The PAL registers have no reset ("PAL registers do not have a
deterministic initialization", `mac128pal.v`, whose author gives that as the reason for the software
correction), and the TSG drives the phase onto D0 when A19 = 0 inside the /VPA space (Snow's decode notes;
the model's `d0` term). **Aurora68 never does this**: the Mac runs the IIci ROM, which knows nothing of the
Plus, and the kernel's board layer (`psmac_board_plus.c`, `psmac_board_se.c`) has no phase read. So the
board runs in whichever phase it powered up in. What the wrong phase does to a PiStorm, as opposed to a
68000, is not known; Apple's own list of symptoms (RAM errors and VIA errors) is the tester's list.
**(the omission is a fact; its effect is open)**

**The SCC.** Even-addressed byte reads, odd-addressed byte writes; "a byte-wide read from an odd address
resets the SCC"; "be careful never to make a word-wide access to the SCC" (the phase shift above) (*Guide*,
p. 365). Snow's notes: the SCC's /RD comes from the read range ($800000-$9FFFFF), its /WR is /LDS. The
2.2 us between accesses is the CPU's job on these boards (above). The Z85C30 (CMOS) specifies a valid access
recovery time of 3.5 PCLK (953 ns at the Mac's 3.672 MHz), /RD to read data valid of 135 ns and address to
read data valid of 210 ns in its 8.5 MHz grade, and has an erratum: it "does not qualify the write strobe
(/WR) with chip enable (/CE)", so a write to another device inside the recovery time can corrupt the
SCC's internal write (Zilog DC-4158-02, AC table #25, #27, #49a, and "Errata"). The NMOS Z8530 of the
original boards was not checked here; Apple's 2.2 us presumably covers it **(inference)**.

**The VIA.** Timed by E: "the MC68000 has to wait for the phase of the relatively slow E clock signal to
coincide with a normal processor-access cycle before it can access the VIA", 1.0 us on average (*Guide*,
p. 149). It sits on D8-D15 at even addresses, with /VMA on its /CS2 and A20 = 0 selecting it (Snow).
Port B on the Plus: PB3 the mouse button, PB4/PB5 X2/Y2, PB6 the H4 timing line, PB7 the sound enable
("writing 1 (off) or 0 (on) to bit 7 of VIA Data register B", *Guide*, p. 431); PA0-PA2 the volume, PA3 the
sound buffer.

**The SE, by contrast.** One custom chip, the BBU, does the decode, /RAS and /CAS, video, sound, the SCC
and SCSI handshakes and a bus timeout: "During a SCSI access in pseudo-DMA mode, the BBU does not assert
/DTACK until it receives a SCSI DRQ"; "If any access has not terminated within 265 ms, the BBU asserts the
bus error signal /BERR" (*Guide*, pp. 112, 126); and it holds /DTACK for the SCC until its lines have
settled (p. 364). No bus-error timer is listed among the PALs' functions (pp. 111-112), so on the Plus a
cycle nobody answers is simply answered (every address gets /DTACK or /VPA, p. 122) **(inference from the
list)**. The SE gives the CPU more of the RAM (3.22 against 2.56 MB/s), and the BBU is one die with one
clock: whatever its /DTACK timing is, it is evidently compatible with the PiStorm's sampling (docs/12,
docs/14). **(inference)**

## 3. Reproduction boards

Most of the Plus can now be built from new parts. The projects found:

- **ITXPlus** (max234252, 2025): "a ITX sized Macintosh Plus clone that can be built with no original
  parts", 4 MB of 1Mx4 DRAM soldered, the IWM replaced by a PLD, a socketed 68000; its schematic labels the
  PALs by their original types (TSM PAL16R4, BMU1 PAL16L8, TSG PAL16R6, BMU2 PAL20R4, LAG and ASG PAL16R8)
  and credits "Porchy, Hkz and Bolle's Reverse engineered PAL's" on the PLD Archive. In the tinkerdifferent
  thread the PALs are in PLCC packages, and Hackaday (21 May 2025) reports the 68000 socketed so that builders
  can fit a ceramic part.
- **MacPlus-Reloaded** (max234252): "1:1 Reproduction of the Mac Plus PCB" (Gerbers and layout only).
- **MacRedux** (RichCini): a redraw of the 128K board 820-0086-C aiming at a 128K/512K hybrid, with the
  PALs "not fully decoded" (search summary of the repository; not cloned).
- **TheRealBolle/SE30**, not a Plus but the same reverse engineers' practice: JEDECs "intended to be used
  with a 16V8-type device (GAL16V8, PALCE16V8, ATF16V8)"; "Certain low power ATF16V8B don't work";
  "ATF16V8C use pinkeepers instead of internal pullups and might be problematic as well."
- **MiniPerformerPlus** (hkzlab, the "Hkz" of the PAL credits), a 68030 accelerator for a Plus clone, the
  closest thing found to our situation: "my clone Mac Plus had no issues in using a WDC 65C22 and fast OTP
  ROMs. With the accelerator installed, it gets a bit finicky on the parts and sometimes refuses to boot at
  all with those installed. Replacing the ROMs with standard 27C512 rated at around 150nS, plus using a
  plain old 6522 solved all the boot issues for me"; and "the choice of PLD type for the build is
  important ... I had good luck with PALCE16V8 rated at 25ns or ATF16V8 rated at 15. Pain and tears with
  GAL16V8D though."

What a new board can change on the bus, against a 1985 board **(all inference unless cited)**:

- **Faster PLDs.** Today's GAL/ATF parts are faster than the 1980s PALs (a GAL16V8D-15's propagation delay
  is 15 ns; the part on the tester's board must be read off it). /DTACK, being one or two PAL delays after
  a clock edge or after /AS, comes earlier relative to the CPU clock. A 68000 does not care (it samples at
  the falling edge and latches a clock later); the PiStorm's current firmware does (section 4): the
  earlier /DTACK arrives after a rising edge, the likelier the CPLD sees it on that edge and takes its data
  a clock early.
- **A CMOS VIA with CMOS input levels.** The W65C22S, the part WDC sells today, specifies an input high
  voltage of VDD x 0.8 for all inputs, 4.0 V at 5 V (WDC data sheet, Table 4-2b); the W65C22N specifies
  2.0 V (Table 4-2a). The PiStorm's EPM240 runs its I/O at 3.3 V and drives E and /VMA itself, and its
  74LVC16373 latches drive the address lines at 3.3 V: a W65C22S on the board receives its PHI2 clock,
  chip select and register select below its specified high level. (The 1985 NMOS 6522s and Apple's own
  drivers were TTL-level parts; a W65C22S behind a real NMOS 68000, whose outputs are only guaranteed at
  2.4 V, is out of specification too, and hkzlab's clone worked with one until an accelerator was added.)
  The W65C22's data delay is short (tCDR 20 ns in its fastest table column), so read timing is not its
  problem; levels may be.
- **A Z85C30 in place of the Z8530**, with the recovery time and the erratum above.
- **Faster DRAM** (modern 60-70 ns parts): data valid sooner after /CAS, which narrows the PiStorm's early
  sampling problem rather than widening it.
- **New solder joints** on a new board and a new socket: section 6.

## 4. How the classic PiStorm performs a 68000 bus cycle

**The hardware.** The EPM240 sits between the Pi's GPIO and the 68000 socket; the address and both data
directions go through 74LVC16373 latches the CPLD strobes (`LTCH_A_*`, `LTCH_D_WR_*`, `LTCH_D_RD_*`). The
Pi writes the data, the low address and the high address with the size and direction (three register
writes), the CPLD runs the cycle, holds `PI_TXN_IN_PROGRESS` (GPIO 0) while it does, and for a read the Pi
then reads GPIO 8-23, which the read latch drives while the Pi holds its read strobe with register 0
selected (`LTCH_D_RD_OE_n <= !(PI_A == REG_DATA && PI_RD)`, line 123). Our transport,
`emu68/src/psmac/psmac_gpio.c`, does exactly what Emu68's `ps_classic_protocol.c` does: `read_cycle()`
raises the read strobe, spins on GPIO 0 (`txn_wait`), and reads `gpio[13]` once, at once.

**The state machine** (`rtl/pistorm.v` at `88937a6`). The 68000 clock reaches the 200 MHz domain through
three flip-flops (`c7m_sync <= {c7m_sync[1:0], (CLK_SEL?M68K_CLK:c1c3_clk)}`, line 129; edges are
`!c7m_sync[2] && c7m_sync[1]` and the reverse, lines 132-133), so the CPLD sees each clock edge 5 to 15 ns
late. Then:

    3'd1: begin // S1
      if (op_req) begin
        if(c7m_rising) begin
          state <= 3'd2;
    ...
    3'd2: begin // S2
      M68K_RW <= op_rw; // S1 -> S2
      LTCH_D_WR_OE_n <= op_rw;
      LTCH_A_OE_n <= 1'b0;
      M68K_AS_n <= 1'b0;
    ...
    3'd3: begin // S3
      op_req <= 1'b0;
      if(c7m_rising) begin
        if (!M68K_DTACK_n || (!M68K_VMA_n && e_counter == 4'd8)) begin
          state <= 3'd4;
          PI_TXN_IN_PROGRESS_delay[2:0] <= 3'b111;
    ...
    3'd4: begin // S4
      PI_TXN_IN_PROGRESS_delay <= {PI_TXN_IN_PROGRESS_delay[1:0],1'b0};
      PI_TXN_IN_PROGRESS <= PI_TXN_IN_PROGRESS_delay[2];
      LTCH_D_RD_U <= 1'b1;
      LTCH_D_RD_L <= 1'b1;
      if (c7m_falling) begin
        state <= 3'd5;
        PI_TXN_IN_PROGRESS <= 1'b0;
    ...
    3'd5: begin // S5
      LTCH_D_RD_U <= 1'b0;
      LTCH_D_RD_L <= 1'b0;

(lines 204-256). What that means, edge by edge, against the 68000 (section 1's table; the CPLD's delays
are its synchronizer's, not measured):

| From the rising edge that begins S2 | 68000 | PiStorm, current firmware |
|---|---|---|
| -64 ns | address valid | address latch still off: `LTCH_A_OE_n` goes low in state 2, **with** /AS |
| 0 + 5-15 ns | /AS, /UDS, /LDS (read) | /AS, /UDS, /LDS and the address together |
| +128 ns + 5-15 ns | | **/DTACK sampled** (raw, unsynchronized) |
| +128 ns + ~20 ns | | read latch transparent; GPIO 0 drops after three 200 MHz cycles: the Pi may read from here |
| +191 ns | /DTACK sampled | **read latch closes** (+5-15 ns) |
| +319 ns | **data latched**, /AS negated | /AS negated (state 7) |

So when /DTACK is already low on the rising edge that begins S4 (or a wait state), the PiStorm takes its
data at the end of S4, **one clock (128 ns) before a 68000 would**, or even earlier if the Pi's read of GPIO
8-23 lands while the latch is still transparent. When /DTACK falls during the high half of a clock, the
CPLD sees it on the next rising edge and closes the latch on the falling edge after, which is where a
68000 latches: the two agree. The cycle's length on the board is the 68000's either way (the strobes end
in state 7 at S7). Writes are driven from state 2 until state 7 and do not depend on the sampling point.
**(the table is read from the source; the ns figures are inference)**

**Where that came from.** Before 18 June 2021 the state machine sampled /DTACK on the falling edge
(`always @(negedge c7m)`, state 2 "S4|Sw -> S5|Sw") and held the read latch open during S7
(`LTCH_D_RD_U <= S7`), released GPIO 0 at S7 too. Commit `d602910` (shanshe, 18 June 2021), "Improve 68k
cycle state machine: S7 and S0 states are made faster to gain almost a c7m cycle, making Chip access
faster. (sysinfo 4.4 chip speed from 0.97 to 1.13)", replaced it with the machine above. The Amiga's chip
bus evidently has its data ready when its /DTACK arrives; the change was an Amiga speed-up. The firmware
images and their JTAG usercodes (read from the SVFs; names as Emu68's `cpld.c` gives them):

| Usercode | Name | What it is |
|---|---|---|
| `0018736F` | EPM240_original | the pre-June-2021 machine (Emu68's file has the same programming data, line endings apart, as `rtl/EPM240_bitstream.svf` at `75bbc70`, April 2021): /DTACK on the falling edge, data taken in S7 |
| `0018569E` | EPM240_bitstream | `d602910`'s machine (June 2021) |
| `00185866` | EPM240_experimental | the same plus the C1/C3 clock option (`36afc41`, October 2021): **the current `rtl/EPM240_bitstream.svf` of the PiStorm repository** |
| `00186C9D`, `00187FC6` | EPM240_long_hold, _longer_hold | "Long hold version of the original firmware. (Has longer AS, Data and Address hold times)" (Frimples/PiStorm-Firmware README); shipped by Emu68 since 25 October 2026 |
| `00187C10` | EPM240_long_data | "only extends Data hold time. Also includes a counter which forces the 68k bus statemachine to maintain the typical 4-cycle access scheme of a real 68k" (same README) |
| `00189844`, `0018A0B9` | EPM240_PiStormX_basic, _skip_s5s6 | ports of "the experimental PiStormX firmware" (same README); AlexCL: "The current stock firmware for the PiStorm has video issues with the Macintosh SE. Install the basic PiStormX firmware" (pistorm-macintosh README) |

The sources of the `long_*` and `PiStormX` images are not in any repository read here. Which image the
owner's SE board and the tester's board carry is not known: the psmac build links neither `cpld.c` nor
libxsvf (`emu68/CMakeLists.txt` builds them for `pistorm-classic` only), so the kernel never reads the
usercode.

**VPA and E.** Implemented. E counts on falling clock edges, `e_counter` 0-9, E high for counts 6-9 (four
clocks) and low for six (lines 155-169); state 3 asserts /VMA when /VPA is seen at a rising edge with
`e_counter == 2`, and ends the cycle at the rising edge with `e_counter == 8`; the read latch then closes on
the next falling edge, while E is still high, and E falls one clock later. So VIA data is taken about
383 ns after E rises (plus the synchronizer), where a 68000 takes it as E falls, 511 ns after. With a
W65C22 (20-80 ns data delay) that margin is ample; with a slow NMOS 6522 it is smaller (its data delay was
not checked here). **(timing read from the source)**

**What is not implemented.** `M68K_BERR_n` is declared (line 36) and never read; FC0-FC2 are only
initialised to 0 (line 72); /BR and /BGACK are inputs never read, /BG is never asserted; /HALT and /RESET
are driven only while the Pi holds reset (lines 105-106). None of these matters on the Plus (no bus master,
no FC decoding, no bus-error timer), except that a cycle nobody would answer cannot hang the board either.
The missing half-clock of address setup before /AS (#11 wants 30 ns) matters wherever a device decodes
the address with /AS: BMU1 does exactly that for every chip select (`csiwm = ~(a23 & a22 & ~a21 & ~as)`
and the rest, `mac128pal.v`), so for a few nanoseconds after /AS the decode sees the latch's outputs
switching on **(inference)**.

**The SE-specific firmware.** Frimples' DirectSocket audit (21 September 2026) found the same thing from
the other side: "The current RTL's state-3 termination block ... checks synchronized DTACK, BERR, and VPA
only when c7m_rising is true. That is a concrete phase mismatch", and commit `6022b23` moved the sampling to
`c7m_falling`. In that version state 4 still drops GPIO 0 three 200 MHz cycles after entering, while the
latch stays transparent until the next falling edge, so the Pi may still read before the 68000's latch
point **(inference)**. Its author's SE booted with "artifacts present" on the screen and in the sound
(`docs/HARDWARE_TEST_LOG.md`); the timing-corrected image had not been tried on hardware when the log was
written.

## 5. The tester's symptoms, against the hypotheses

Ranked by how well each explains what was seen; each with what would tell it apart (section 9 collects the
measurements).

**H1. The CPLD takes read data before the board has driven it** (section 4). Explains: reads of board RAM
mostly wrong while writes land; the SE unaffected (the BBU's timing suits the firmware empirically); a
faster-PLD board worse than an original. The statistic fits a sampling instant on the data's edge rather
than a whole cycle off: 29 of 32 words wrong means about 9% of words had all 16 bits right; if bits failed
independently that is a per-bit error rate of about 14% ((1-p)^16 = 0.09), whereas a read one whole cycle
off would return whole stale words, and in a desktop pattern whose rows repeat one word, stale words would
mostly *match* **(inference)**. Against: in the `mac128pal.v` model /RAS, and so /DTACK for RAM, moves a
PAL delay *after* the CPU clock's rising edge, which the CPLD's 5-15 ns synchronizer might sample before it
does; whether the CPLD wins that race depends on the board's PLDs and on where the CPU clock is measured.
The scope settles it in a minute (section 9, M1). The SCC's and IWM's /DTACK, being immediate, certainly
precedes the S4 edge on every board: there the firmware takes data one clock early by construction, about
150 ns after the address appears, under the Z85C30's 210 ns address-to-data maximum **(inference from the
equations and the data sheet)**.

**H2. SCC accesses closer together than 2.2 us.** The mouse poll reads channel A's RR0 and then channel B's
back to back (`psmac_board_plus.c` line 411: `x1 = ...PLUS_SCC_A_RD...; y1 = ...PLUS_SCC_B_RD...`), one
GPIO bus cycle apart, well under 2.2 us **(inference: a cycle is four clocks, 0.51 us, plus the Pi's
register writes)**, and the board layer passes the Mac's own SCC accesses
(the IIci ROM's serial driver, written for glue that delays /DTACK) with no spacing either. Explains: the
line read *second*, Y1, is the noisy one, X1 believable. Prediction: swapping the order moves the noise to
X1; 3 us between the reads removes it. Independent of H1, and the cheapest to test.

**H3. VIA reads failing.** 34,076 keys and 114,431 clicks need wrong reads of the shift register, IFR and
port B. Candidates: H1 does not apply to /VPA cycles in the same way (the latch closes 128 ns before E
falls, which a 6522 meets by its data delay); Apple's own "VIA errors" in the wrong phase (H4); a W65C22S
on a new board driven at 3.3 V (section 3); the reads themselves have side effects the kernel tolerates
(a read of port B clears the CB1/CB2 flags, a read of the shift register restarts it). Which VIA is on the
board is the first question for the tester.

**H4. The board in the wrong phase** (section 2): Apple names RAM errors and VIA errors, and nobody in
Aurora68 checks or corrects the phase. Prediction: the error rates of M2 and M7 differ from one power-on
to the next, in two clusters, and a word access to the SCC moves a machine from one cluster to the other.
Apple also names "an unstable video display", which the tester has not reported (the LCD conversion
resamples the video, which may hide it) **(inference)**.

**H5. The socket, the rework, the wiring** (section 6). A poor contact on a data line hurts writes at least
as much as reads (the PiStorm's write has to charge the whole board's bus through it; the read only the
latch's input), and writes are nearly perfect, so the data lines are probably sound; a poor or ringing CLK,
/DTACK or E contact is different, since the CPLD acts on single edges of those. Prediction: errors
concentrated on particular bits or lanes (M3) point here.

**H6. Power.** The Pi draws its 0.5-0.8 A (docs/12, "Power through the socket") through the 68000's two VCC
and two GND pins. Ground bounce on a new board's planes is unlikely to corrupt 90% of reads and leave
writes alone; it is a better candidate for the whine (section 7).

## 6. The physical side: a socket on a new board

The tester's PiStorm is in a socket, not a clip. With a clip (the "Killy clip" of the period
accelerators; TidBITS in 1991 describes the Brainstorm's Plus version as installed on the processor), the
board's own 68000 must be held off the bus or removed, the clip's leaf contacts and any ribbon add
resistance, inductance and capacitance to every line, and reads fail first because the board's weak NMOS drivers must charge the
clip and the PiStorm's inputs while the PiStorm's strong 3.3 V drivers have no trouble on writes
**(inference)**. A socket on a reworked or newly built board has its own list. What a tester should check,
with the power off and the PiStorm out:

1. **Every socket joint**, under a loupe, and reflow any dull or cratered one: on a new board, hand-soldered
   sockets are the likeliest fault (the owner's own PiStorm had a solder bridge, docs/12). The lines the
   CPLD acts on by single edges first: CLK (pin 15), /DTACK (10), /VPA (21), E (20), /VMA (19), /AS (6),
   /UDS (7), /LDS (8), R/W (9); then D0-D15 (pins 5 to 1 for D0-D4, 64 to 54 for D5-D15), VCC (14, 49) and
   GND (16, 53). (The 64-pin DIP assignment as in the M68000 UM, section 11.1, Figure 11-1; check it there.)
2. **Continuity from each socket pin to where the signal goes**, not just to the socket's own pad: D0-D15
   to the two 74LS245s (or their replacements) and D8-D15 to the VIA and the SCC; /DTACK to the TSM; /VPA
   to BMU1; E to the VIA's PHI2; /VMA to the VIA's /CS2; CLK to the TSG's C8M output (through whatever
   resistor the board has there). A reproduction board's schematic (ITXPlus's is public) gives the pins.
3. **Seating**: the PiStorm's pins fully in the socket's contacts, square, with no stress from the Pi or a
   cable bending it (a 3B overhangs the board, docs/12). Machined-pin sockets want long pins; the
   MiniPerformerPlus calls for ">=9.7mm length" machined pins to clear the board.
4. **Shorts between adjacent pins** (the SE's bridge was between pins 42 and 43) and between the socket and
   whatever sits under the PiStorm.
5. **The parts list**: the PLDs' types and speed grades, the VIA (W65C22S, W65C22N, Rockwell, Synertek?),
   the SCC (Z8530 or Z85C30), the DRAM or SIMMs and their speed, the oscillator; and whether the board is
   a Plus-type (BMU2) or a 512K-type (BMU0) design.

The kernel's own address-line test (docs/12) proves nothing on this board: it reads back what it wrote,
and reads are what is in doubt **(inference)**.

## 7. The whine

Facts: the Plus's sound is a PWM pulse per scan line from the high byte of each of 370 words, integrated
and attenuated in "the Sony sound chip" by a 3-bit volume from VIA PA0-2, switched by PB7 (*Guide*, pp.
430-431); a constant buffer is a constant level. The kernel fills the buffer with one level between
sounds and keeps PB7 high while silent (`psmac_board_se.c`, `snd_quiet`, `snd_lines`); the whine stayed.
Candidates **(all inference)**, with what tells them apart:

1. **Supply and ground noise from the Pi and the PiStorm.** The Pi's switching regulators and its load,
   which follows what the Pi does (the mouse poll at 8 kHz, now spread by `mac.plusjitter`; the screen
   copy 60 times a second; the heartbeat), flow through the socket's two VCC and two GND pins and the logic
   board's ground into the sound path's reference, and on an LCD conversion into whatever amplifier and
   supply replaced the analog board's. Tests: does the pitch follow `mac.plusmouse=0`, `mac.plusmouse=2000`
   or `mac.plusjitter=0`, `mac.screencheck=off`, an idle desktop against a busy one? With the original
   68000 in the socket (if the tester has one), is there any whine?
2. **PB7 toggled by VIA timer 1.** The 6522 can drive PB7 from T1 (ACR bit 7), which is how Inside
   Macintosh's square-wave technique works on these machines (*Guide*, p. 431). The Plus layer passes the
   IIci ROM's ACR bits 6-7 to the wire (`psmac_board_plus.c` line 258: `(d & ~0x1cu) | kb_mode`). If the ROM
   or the System sets ACR bit 7 with T1 free-running, the sound enable chops at T1's rate. Test: the
   heartbeat already prints the ACR; bit 7 set is this cause. Fix: clear ACR bit 7 on the wire.
3. **Wrong bytes in the sound buffer.** If writes to board RAM occasionally fail (the dotted marks), a
   byte of the buffer can be off; repeated at 60.15 Hz that is a buzz, not a whine. Test: rewrite the
   buffer periodically (K4) and listen.
4. **Acoustic noise, not sound.** Inductors and ceramic capacitors in a switching supply (the conversion's
   or the Pi's) sing under load that changes at audio rates. Test: disconnect the speaker; if it is still
   there, find it with a tube held to the ear.
5. **The volume.** Set the Sound control panel's volume to 0 and to 7: a whine that scales with it is in
   the signal before the volume stage (the PWM or the Sony chip's input), one that does not is after it
   (amplifier, supply).

**Found (24 November 2026).** The tester's hum on the Plus came from the machine's PicoPSU-style replacement power
supply, not from the kernel: the source was the supply. Candidates 1 to 5 above stay as
the record of what was considered; a compact Mac on a small DC-DC supply is worth checking first when a speaker hums.

## 8. Recommendations, ranked

**Kernel, now (no hardware change):**

- **K1. Space SCC accesses by 3 us on the 128K, 512K, 512Ke and Plus** (Apple's 2.2 us with a margin): a
  timestamp of the last SCC access on the wire, kept by the service core, which carries every cycle; the
  mouse poll's two RR0 reads and the Mac's own SCC accesses through the board layer both wait on it. Fixes
  H2. Confirm: with the mouse still, Y1's change count drops to the order of X1's; reversing the two reads
  in a test build moves the noise to X1 if the spacing is left out.
- **K2. Read and log the CPLD's usercode at start**, before the protocol is set up: link `cpld.c` and
  libxsvf into the psmac build and call `ps_cpld_load(NULL, 0, 0, 0)` (mode 0 only reads the usercode;
  JTAG is on GPIO 24-27, outside the protocol's lines), and print the name from `cpld.c`'s table. Every
  report then says which firmware it ran on. Confirm: the log line on the SE and on the 512Ke.
- **K3. Read the board's RAM only in the bus check.** The kernel needs no data from board RAM (the Mac's
  memory is the Pi's; the screen and sound buffers on the board are written, and their truth is the Pi's
  copy); `mac.screencheck` already stops when reads are bad (`d336f77`). Instead of reading rows back,
  **rewrite the whole screen and the sound buffer blind** every few seconds from the Pi's copy: 10,944
  word writes, about 11 ms at roughly 1 us a cycle (an estimate), every 2 s is half a percent of the
  wire. Fixes the dotted marks whatever their cause, without trusting a read. Confirm: marks vanish
  within the period.
- **K4. Plausibility on the VIA reads that matter**, as damage control until the cause is fixed: the button
  only when two port B reads 50 us apart agree; IFR read twice; keyboard bytes accepted only when they are
  valid Plus keyboard answers (a transition with bit 0 set, $79 then a keypad key, $7B) and arrive in
  answer to an Inquiry; X2/Y2 already debounced. Do not "vote" on reads with side effects: the shift
  register (a read restarts it) and port B in quick succession during a keyboard exchange (it clears CB1's
  flag; the poll already stays out of `kb_edge_watch`). This does not fix anything; it keeps phantom keys
  and clicks from reaching the Mac. Confirm: the heartbeat's key and click counts fall to what the tester
  did.
- **K5. The phase**, after the bus check exists: at start, read $F00000 a few dozen times and log D0; run
  the RAM read test; if the error rate is high, make one word access to the SCC ($9FFFF8, before the Mac's
  serial driver opens it, since a word access resets the SCC), and measure again; keep the better phase and
  log both rates. Which D0 value Apple's ROM wants is not documented in the sources read here, so the
  choice is empirical. Fixes H4 if H4 is real. Confirm: across several power-ons, the logged phase and
  error rate.
- **Not worth doing:** slowing the Pi's requests or adding delays between them (the problem is inside the
  cycle, which only the CPLD times; spacing helps only the SCC, K1); majority votes on board RAM as a fix
  (with about 14% of bits wrong per read, a vote of five still leaves about a fifth of words wrong, by the
  same arithmetic as section 5); "using VPA cycles for the VIA" (they already are /VPA cycles: BMU1 asserts
  /VPA for $E00000 up, and the CPLD runs the E-clock cycle).

**CPLD:**

- **C1. Try the `original` firmware first** (usercode `0018736F`, in Emu68's `firmware/pistorm-classic/`
  and Frimples/PiStorm-Firmware's `flash_original.sh`, flashed from Pi OS with `pistormflash`; keep the
  board's current SVF to go back: the PiStorm repository's current one is `00185866`). It samples /DTACK
  on the falling edge and takes data in S7. It is a ten-minute experiment and decisive for H1: the bus
  check's RAM read error rate should fall from about 90% to near zero. Caveat **(inference)**: its latch is
  open during S7, after /AS has risen and the board's '245s have begun to let go, so it relies on the bus
  holding its last value for a few tens of ns; `long_data`, which keeps a real 68000's four-clock rhythm,
  is the second image to try.
- **C2. An Aurora68 firmware that samples like a 68000** (the same GPIO protocol, so no kernel change):
  sample /DTACK, /BERR and /VPA on `c7m_falling` through a two-stage synchronizer; on recognition go to a
  state that keeps the read latch transparent through the next clock and **closes it on the following
  `c7m_falling`** (S6 to S7), then negates /AS; drop `PI_TXN_IN_PROGRESS` only *after* the latch has
  closed (in the state after it), never while it is transparent; enable the address latch one state before
  /AS (on the falling edge that begins S1); in a /VPA cycle close the latch on the falling edge on which E
  falls, as the 68000 does. In outline:

      3'd3: if (c7m_falling) begin                          // the end of S4, or of a wait state
              if (!dtack_s || !berr_s) begin                    // berr: also set a status bit for the Pi
                LTCH_D_RD_U <= 1'b1; LTCH_D_RD_L <= 1'b1;       // open through S5 and S6
                state <= 3'd4;
              end else if (!M68K_VMA_n && e_counter == 4'd9) begin
                LTCH_D_RD_U <= 1'b0; LTCH_D_RD_L <= 1'b0;       // E falls on the same 200 MHz tick:
                state <= 3'd7;                                  // the 6800 cycle's data taken as E goes low
              end else if (!vpa_s && !M68K_E && e_counter == 4'd2) begin
                M68K_VMA_n <= 1'b0;
                LTCH_D_RD_U <= 1'b1; LTCH_D_RD_L <= 1'b1;       // open while E is high
              end
            end
      3'd4: if (c7m_falling) begin                          // the falling edge entering S7
              LTCH_D_RD_U <= 1'b0; LTCH_D_RD_L <= 1'b0;
              state <= 3'd7;
            end
      3'd7: begin M68K_AS_n <= 1'b1; M68K_VMA_n <= 1'b1; ... PI_TXN_IN_PROGRESS <= 1'b0; state <= 3'd0; end

  (`dtack_s` and the others are the synchronized inputs; E and its counter change on the clock's falling
  edge one 200 MHz tick after `c7m_falling` is seen, the latch's register on the same tick, so the latch
  closes as E falls, within the W65C22's 10 ns data hold (tHR; an NMOS part's was not checked); /AS rises
  one tick after the latch closes, inside the 68000's 0 ns hold, #29). The cost: reads whose /DTACK came early take one clock longer, about 128 ns.
  Write a testbench like DirectSocket's (`tb_timing.v`) with a model of the TSM's /DTACK; flash with
  `pistormflash`. Fixes H1 on every board, including the SCC's and the IWM's reads (floppy reads on a Plus
  will need it) and the VIA's margin. Confirm: the bus check on the 512Ke and, as a regression, the SE.
- **C3. Optional**: honour /BERR (end the cycle, flag it to the Pi) and a glitch filter on CLK (accept an
  edge only after two agreeing samples), which matter for a clip or a long cable more than for this socket.

**Hardware:**

- **H-1. The checks of section 6**, and the parts list, before anything else is changed.
- **H-2. The VIA.** If it is a W65C22S, try a W65C22N (TTL input levels, WDC Table 4-2a) or an original
  NMOS/Rockwell part; hkzlab's experience (section 3) says the same. Confirm: the click and key counts.
- **H-3. A scope at the socket** (M1, M6): CLK, /DTACK, /AS, E and /VMA at the VIA, one data line during a
  read; the 5 V and ground at pins 14/49 and 16/53, AC-coupled, while the whine is heard.
- **H-4. Power**: the kernel's `power:` lines (docs/12) for under-voltage; if the 5 V at the socket sags or
  is noisy, a short, heavy 5 V and ground lead from the board's power connector to the PiStorm's supply
  pins (the same rail, so nothing is back-fed) is the remedy to try for the whine.

## 9. Measurements for the bus check

For the worker writing the kernel's bus check: these are the numbers that separate the hypotheses. Each
should be logged with the CPLD usercode (K2) and, where it applies, the phase read's D0 (K5).

- **M1 (scope, not kernel).** /DTACK's falling edge relative to CLK's rising edge at the socket, for a RAM
  read and for an SCC read. Low before the edge or within about 15 ns after it: H1 holds for that cycle.
- **M2. RAM read error rate by place and time.** Write a region outside the screen with known patterns
  ($0000, $FFFF, $AAAA, $5555, the address as data, a walking one), then read each word once; count wrong
  words, and record for each wrong read the time since the last vertical blanking (the VIA's CA1 edge, as
  the sound code already tracks it). Errors only during the active part of lines, none in horizontal or
  vertical blanking, point at the video interleave (contended cycles); errors everywhere point at H1 as a
  plain sampling-point problem.
- **M3. Which bits.** A histogram of wrong bits by bit position and by direction (expected 0 read as 1 and
  the reverse), and by byte lane. Evenly spread: timing (H1). One line or one lane: socket, joint or a
  '245 (H5). A bias toward 1s: a floating bus at the sampling instant.
- **M4. What the wrong values are.** For each wrong read, is the value the previous cycle's data (bus
  hold), a word of the screen (a video fetch's data still on the RAM bus), or unrelated? Compare against
  the last written word and the screen's contents.
- **M5. Repeat reads.** Read one word 1,000 times: how many distinct values, and is the right one the
  commonest? Independent errors (timing) against a stuck wrong value (the RAM really holds it: a write
  error). Then the same for writes: write a word, read it 15 times, take the majority, compare.
- **M6. Byte reads against word reads**, and reads of the board's ROM at $400000 (if the board has one):
  ROM has immediate /DTACK and no interleave; perfect ROM reads with bad RAM reads isolate the RAM path,
  bad ROM reads too isolate the sampling point. The ROM can be checked against itself without a copy of it
  (read it twice; or compare the sum of its words after the first four bytes with the checksum in those four
  bytes, the usual description of Apple's ROM checksum, not checked against a source here).
- **M7. VIA registers with no side effects and known contents**: DDRA, DDRB, the T1 and T2 latches, ACR,
  PCR, IER as the kernel last wrote them, 10,000 reads each; mismatch rate per register and per bit. And the
  IFR against itself (two reads in a row).
- **M8. SCC spacing.** With the mouse still: read RR0 of A then B with 0.5, 1, 2, 3 and 5 us between, then
  B then A, 10,000 pairs each; count DCD changes per channel. The noise following the second read and
  vanishing above about 2 us is H2.
- **M9. A static SCC register**: write WR2 (the interrupt vector, a register the Plus does not use) and
  read it back through RR2 of channel A, with K1's spacing and without; mismatches with spacing point at
  H1 for the SCC.
- **M10. Across power-ons**: M2 and M7 at five or more cold starts, with the phase read's D0; two clusters
  that follow D0 are H4. Then the same with K5's word access to the SCC.
- **M11. With each CPLD image** (C1, C2): M2, M7 and M8 again. This is the measurement that decides H1.
- **M12. For the whine**: the heartbeat's ACR (bit 7), the tester's ear with `mac.plusmouse=0`, with the
  speaker unplugged, and at volume 0 and 7 (section 7).

## 10. What is still open

- When the Plus's (as opposed to the 128K's) PALs assert /DTACK for RAM, and when the data is valid, in
  nanoseconds: nobody seems to have published a measurement. M1 on the tester's board gives the answer for
  that board.
- The phase read's meaning: which D0 value is "in phase", what the ROM does about it, and how often the
  word access to the SCC must be repeated. The Plus ROM's start-up code would say; it was not read here.
- The Verilog of the `long_*` and `PiStormX` images.
- Wouter de Waal's PAL notes, the PLD Archive's reproduction files and 68kmla's threads, which a person
  can read in a browser (see "The sources").
- Whether the owner's SE board carries the same firmware as the tester's (K2 will say), and whether the
  SE would show the same read errors on the bus check: if it does not, that is the BBU's timing at work,
  and a C2 firmware must be checked on the SE as well before it becomes the default.

## Sources

- Apple Computer, *Guide to the Macintosh Family Hardware*, 2nd ed., Addison-Wesley, 1990:
  https://archive.org/details/Guide_to_the_Macintosh_Family_Hardware_Second_Edition (pp. 90, 111-112, 122,
  125-126, 149, 194, 363-365, 430-431)
- Motorola, *M68000 8-/16-/32-Bit Microprocessors User's Manual*, M68000UM/AD Rev. 8, 1993:
  https://bitsavers.trailing-edge.com/components/motorola/68000/68000/M68000UM_AD_M68000_Microprocessor_Users_Manual_Rev8_1993.pdf
  (sections 5.1.1, 5.7, 5.8, 10, 11.1, Appendix B)
- PiStorm CPLD source and history: https://github.com/captain-amygdala/pistorm (`rtl/pistorm.v` at
  `88937a6`; commits `75bbc70`, `d602910`, `bab7a84`, `36afc41`)
- Emu68: https://github.com/michalsc/Emu68 (`src/pistorm/cpld.c`, `src/pistorm/ps_classic_protocol.c`,
  `firmware/pistorm-classic/`); this repository's copy in `emu68/`
- https://github.com/Frimples/PiStorm-Firmware (README, `rtl/*.svf`)
- https://github.com/Frimples/MacSE-PiStorm-DirectSocket (`pistorm.v`, `docs/HARDWARE_TEST_LOG.md`, commits
  `5ae21d0`, `6022b23`)
- https://github.com/Frimples/pistorm-macintosh (README, "Macintosh SE Specific")
- A. Makousky, `mac128pal.v`, Macintosh-HW/macsehw:
  https://git.applefritter.com/Macintosh-HW/macsehw/src/commit/198bf1fac1b60281ca425d7627e37fc79fa8d826/hardware/fpga/bbu/mac128pal.v
- MAME, `src/mame/apple/mac128.cpp` (0.264), header comments: https://github.com/mamedev/mame
- Snow emulator: https://github.com/twvd/snow/pull/400 and `core/src/mac/compact/` in https://github.com/twvd/snow
- S. Chamberlin, "68000 Interleaved Memory Controller Design", Big Mess o' Wires, 25 August 2011:
  https://www.bigmessowires.com/2011/08/25/68000-interleaved-memory-controller-design/
- ITXPlus: https://github.com/max234252/ITXPlus (README, `Hardware/MLB/ITXPlus MLB Schematic.pdf`);
  https://tinkerdifferent.com/threads/itxplus-a-itx-sized-macintosh-plus-logicboard-reproduction.4302/;
  https://hackaday.com/2025/05/21/a-new-mac-plus-motherboard-no-special-chips-required
- https://github.com/max234252/MacPlus-Reloaded; https://github.com/RichCini/MacRedux;
  https://github.com/TheRealBolle/SE30
- MiniPerformerPlus: https://github.com/hkzlab/MiniPerformerPlus (README, "Notes")
- Western Design Center, W65C22 data sheet: https://www.westerndesigncenter.com/wdc/documentation/w65c22.pdf
  (Tables 4-2a, 4-2b, section 4.3)
- Zilog, Z85C30 CMOS SCC, Customer Procurement Specification DC-4158-02:
  https://hallaweb.jlab.org/tech/Detectors/public_html/manuals/chip_specs/M-Z/zilog/z85c30.pdf
- TidBITS, "For Little Macs", 15 July 1991: https://tidbits.com/1991/07/15/for-little-macs/; Low End Mac,
  "Mac Plus with 16 MHz Brainstorm Upgrade Benchmarked": https://lowendmac.com/accel/brains.shtml
- Not read (blocked to automated clients): http://www.retro.co.za/ccc/mac/ReverseEngineering/PALs.html;
  https://wiki.pldarchive.co.uk/index.php?title=Macintosh_128k%2F512k%2FPlus; 68kmla.org threads
