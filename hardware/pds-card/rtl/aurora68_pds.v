// SPDX-License-Identifier: MIT
/*
 * aurora68_pds.v - the CPLD of Aurora68's Card P, a card for the Macintosh SE's 96-pin processor
 * direct slot carrying a Raspberry Pi 3A+ (or a Zero 2 W) on its 40-pin header.
 *
 * Copyright 2020 Claude Schwarz
 * Copyright 2020 Niklas Ekstrom - rewrite in Verilog
 * Copyright 2026 Aurora68 project - the slot card's additions
 *
 * Derived from the classic PiStorm's rtl/pistorm.v (github.com/captain-amygdala/pistorm, MIT licence, notice
 * below), and wired like the PiStorm Rev B board: the CPLD's pins reach the bus through two SN74CBTD3384
 * bus switches (the 68000's strobes, function codes, /DTACK, /VPA, /BERR, E, /VMA, interrupts, /RESET and the
 * arbitration lines) and four 74LVC16373 latches (address and data), with the 68000 clock (here the slot's
 * C8M) on a 22 ohm resistor. The pin numbers of every signal the PiStorm has are the PiStorm's
 * (aurora68_pds.qsf). What is kept word for word is the Pi-side protocol: four registers selected by
 * PI_A[1:0] (REG_DATA, REG_ADDR_LO, REG_ADDR_HI, REG_STATUS), the read and write strobes on GPIO 6 and 7,
 * PI_TXN_IN_PROGRESS on GPIO 0, PI_IPL_ZERO on GPIO 1, PI_RESET on GPIO 5, the 200 MHz clock from the
 * Pi's GPCLK0 on GPIO 4, the latch strobes; so the kernel's transport (emu68/src/psmac/psmac_gpio.c)
 * runs unchanged. What is new (docs/22 sections 2 to 4 and docs/18 section 8, C2):
 *
 *  - Bus arbitration. The SE's own 68000 stays in its socket. /BR is asserted from power-up (an open-drain
 *    output enabled by a register's power-up value, so no clock is needed), the card takes the bus with
 *    /BGACK at the first idle moment after /BG, and keeps it, across resets too. On the SE /RESET and /HALT
 *    are one net, so every reset of the board resets the 68000 as well: the card re-asserts /BR during every
 *    reset and, after it, watches /AS for 16 C8M clocks before it drives the bus again. A 68000 that honours
 *    /BGACK stays off the bus; one that does not starts a cycle, finds /BR asserted, grants the bus after
 *    that cycle, and the card takes it again. Either way nothing drives the bus twice (docs/22 section 4,
 *    open question 1).
 *  - 6800 cycles against the board's E. The 68000 keeps generating E with the bus granted away (Apple,
 *    Designing Cards and Drivers, 3rd ed., ch. 13), so E is an input here (on the PiStorm's E pin): /VMA is
 *    asserted while E is low with two or more C8M clocks before E rises, the read latch is open while E is
 *    high and closes on the last C8M rising edge before E falls (447 ns after E rose, after the VIA's data is
 *    valid and before E's fall, so the VIA's 10 ns data hold is never in question), and the cycle ends after
 *    E falls.
 *  - RAM-safe /AS. /AS falls only on a C8M rising edge (the start of S2), never in S3 or S4, as the BBU
 *    requires (ch. 13, Figures 13-2 and 13-3); optionally a RAM cycle waits for /PMCYC's fall, which "always
 *    falls in S1" of a processor RAM slot (/PMCYC is on the PiStorm's /HALT pin: on the SE /HALT is /RESET).
 *  - Read data sampled where a 68000 samples it. /DTACK, /BERR and /VPA are looked at on C8M falling edges only
 *    (the end of S4 and of each wait state); after /DTACK the read latch stays transparent for one more C8M
 *    clock and closes on the falling edge entering S7, and PI_TXN_IN_PROGRESS drops only after it closed.
 *    The classic machine closed the latch one clock earlier (docs/18 section 4). This needs transparent
 *    latches (74LVC16373), not edge-triggered ones: see README "Departures from the PiStorm".
 *  - /BERR ends a cycle and is reported. The card has no time-out of its own: the SE's gate array asserts
 *    /BERR whenever /AS stays low for about 250 ms (Apple, Table 13-2), and the kernel gives up at 300 ms.
 *  - Stock mode: the card lets go of /BR and /BGACK, three-states everything and pulses /RESET, so the SE
 *    starts its own ROM on its own 68000. From a jumper or switch (works with no Pi at all: the override is
 *    combinational) or from the kernel.
 *  - A fan PWM (16 steps, full speed from power-up until the kernel says otherwise), a tachometer counter
 *    and an activity LED.
 *  - Status bits for the kernel in REG_STATUS bits 0-12, which the classic CPLD returns as zero; settings
 *    through REG_STATUS writes with bit 15 set, which the classic kernel never writes. PROTOCOL.md.
 *
 * Kept small for an EPM240 (240 logic elements); rtl/README.md has the size estimate and how it was made.
 * Simulated with Icarus Verilog against a behavioural SE (tb/tb_card.v). Not synthesised with Quartus,
 * not run on hardware.
 *
 * ---- PiStorm licence (MIT) ----
 * Copyright (c) 2021 PiStorm developers
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
 * associated documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the
 * following conditions: The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
 * KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
 * PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
`timescale 1ns/100ps
module aurora68_pds #(
    parameter STOCK_BITS = 6            // stock-mode /RESET pulse: 2^6 ticks of 32.7 us = 2.1 ms
  ) (
    // ---- the Pi (GPIO numbers as in emu68/src/pistorm/ps_protocol.h) ----
    output reg      PI_TXN_IN_PROGRESS, // GPIO0
    output reg      PI_IPL_ZERO,        // GPIO1
    input   [1:0]   PI_A,               // GPIO3..2
    input           PI_CLK,             // GPIO4, GPCLK0, 200 MHz
    output reg      PI_RESET,           // GPIO5: low while the board's /RESET is low (and not pulled by us)
    input           PI_RD,              // GPIO6
    input           PI_WR,              // GPIO7
    inout   [15:0]  PI_D,               // GPIO23..8

    // ---- the four 74LVC16373 latches (as on the PiStorm) ----
    output reg      LTCH_A_0,           // address latch 1, low half: A0-A7 (A0 not wired)
    output reg      LTCH_A_8,           // address latch 1, high half: A8-A15
    output reg      LTCH_A_16,          // address latch 2, low half: A16-A23
    output reg      LTCH_A_24,          // address latch 2, high half: not wired on the SE (24-bit bus)
    output          LTCH_A_OE_n,
    output reg      LTCH_D_RD_U,
    output reg      LTCH_D_RD_L,
    output reg      LTCH_D_RD_OE_n,
    output reg      LTCH_D_WR_U,
    output reg      LTCH_D_WR_L,
    output          LTCH_D_WR_OE_n,

    // ---- the slot, through the CBTD3384 bus switches (C8M through 22 ohm) ----
    input           M68K_CLK,           // the slot's C8M
    output  [2:0]   M68K_FC,
    inout           M68K_AS_n,          // driven while the card holds the bus, read while it does not
    output          M68K_UDS_n,
    output          M68K_LDS_n,
    output          M68K_RW,
    input           M68K_DTACK_n,
    input           M68K_BERR_n,
    input           M68K_VPA_n,
    input           M68K_E,             // the 68000's E: an input on the slot card
    output          M68K_VMA_n,
    input   [2:0]   M68K_IPL_n,
    inout           M68K_RESET_n,       // open drain; read back (on the SE it is also /HALT)
    input           M68K_PMCYC_n,       // on the PiStorm's /HALT pin
    inout           M68K_BR_n,          // open drain
    inout           M68K_BGACK_n,       // open drain; read back
    input           M68K_BG_n,

    // ---- the rest of the card ----
    input           SLOT_PWR,           // high when the SE's +5 V is present (a divider from the slot's +5 V)
    output          FAN_PWM,            // high: fan MOSFET on
    input           FAN_TACH,           // open collector from the fan, pulled up on the card
    input           STOCK_SW_n,         // low: the stock-mode jumper or switch is closed
    output          LED_ACT             // high: LED on
  );

  localparam REG_DATA    = 2'd0;
  localparam REG_ADDR_LO = 2'd1;
  localparam REG_ADDR_HI = 2'd2;
  localparam REG_STATUS  = 2'd3;

  wire c200m = PI_CLK;

  // ------------------------------------------------------------------------------------------
  // Synchronisers: every slot input is asynchronous to the Pi's 200 MHz clock. C8M gets three stages
  // as on the classic PiStorm (its edges time the cycle); the others two.
  // ------------------------------------------------------------------------------------------
  reg [2:0] c8m_sync = 3'b000;
  reg [1:0] e_sync = 2'b00, pmcyc_sync = 2'b11, dtack_sync = 2'b11, vpa_sync = 2'b11, berr_sync = 2'b11;
  reg [1:0] bg_sync = 2'b11, bgack_sync = 2'b11, as_sync = 2'b11, reset_sync = 2'b00, pwr_sync = 2'b00;
  reg [1:0] tach_sync = 2'b11, sw_sync = 2'b11;
  reg       e_last = 1'b0, pmcyc_last = 1'b1, tach_last = 1'b1;
  always @(posedge c200m) begin
    c8m_sync   <= {c8m_sync[1:0], M68K_CLK};
    e_sync     <= {e_sync[0], M68K_E};
    pmcyc_sync <= {pmcyc_sync[0], M68K_PMCYC_n};
    dtack_sync <= {dtack_sync[0], M68K_DTACK_n};
    vpa_sync   <= {vpa_sync[0], M68K_VPA_n};
    berr_sync  <= {berr_sync[0], M68K_BERR_n};
    bg_sync    <= {bg_sync[0], M68K_BG_n};
    bgack_sync <= {bgack_sync[0], M68K_BGACK_n};
    as_sync    <= {as_sync[0], M68K_AS_n};
    reset_sync <= {reset_sync[0], M68K_RESET_n};
    pwr_sync   <= {pwr_sync[0], SLOT_PWR};
    tach_sync  <= {tach_sync[0], FAN_TACH};
    sw_sync    <= {sw_sync[0], STOCK_SW_n};
    e_last     <= e_sync[1];
    pmcyc_last <= pmcyc_sync[1];
    tach_last  <= tach_sync[1];
  end
  wire c8m_rising  = !c8m_sync[2] &&  c8m_sync[1];
  wire c8m_falling =  c8m_sync[2] && !c8m_sync[1];
  wire e_s = e_sync[1];
  wire e_rising  = e_s && !e_last;
  wire e_falling = !e_s && e_last;
  wire pmcyc_fall = !pmcyc_sync[1] && pmcyc_last;
  wire dtack_s = !dtack_sync[1];          // active high from here on
  wire vpa_s   = !vpa_sync[1];
  wire berr_s  = !berr_sync[1];
  wire bg_s    = !bg_sync[1];
  wire bgack_seen = !bgack_sync[1];       // the slot's /BGACK, our own drive included
  wire as_seen = !as_sync[1];
  wire board_reset = !reset_sync[1];      // the board's /RESET is low, whoever pulls it
  wire slot_pwr_s = pwr_sync[1];

  // ------------------------------------------------------------------------------------------
  // The Pi's strobes (classic)
  // ------------------------------------------------------------------------------------------
  reg [1:0] rd_sync = 2'b00;
  reg [1:0] wr_sync = 2'b00;
  always @(posedge c200m) begin
    rd_sync <= {rd_sync[0], PI_RD};
    wr_sync <= {wr_sync[0], PI_WR};
  end
  wire rd_rising = !rd_sync[1] && rd_sync[0];
  wire wr_rising = !wr_sync[1] && wr_sync[0];

  // ------------------------------------------------------------------------------------------
  // Registers. status_cl is the classic status word's meaningful bit (bit 1, "reset released"; bit 0,
  // INIT, acts as a strobe); writes with bit 15 set are the slot card's own (PROTOCOL.md).
  // ------------------------------------------------------------------------------------------
  reg       status_cl = 1'b0;             // power-up: the board held in reset, as the classic CPLD does
  reg       stock_req = 1'b0;             // ext 0 bit 0
  reg       pmcyc_align = 1'b0;           // ext 0 bit 1
  reg [3:0] fan_duty = 4'hF;              // ext 1: full speed until the kernel says otherwise
  wire init_strobe = wr_rising && PI_A == REG_STATUS && !PI_D[15] && PI_D[0];

  always @(posedge c200m) begin
    if (wr_rising && PI_A == REG_STATUS) begin
      if (!PI_D[15])
        status_cl <= PI_D[1];
      else if (PI_D[14:12] == 3'd0) begin
        stock_req <= PI_D[0];
        pmcyc_align <= PI_D[1];
      end else if (PI_D[14:12] == 3'd1)
        fan_duty <= PI_D[3:0];
    end
  end

  // ------------------------------------------------------------------------------------------
  // Latch strobes (classic: combinational, transparent while the Pi's write strobe is high)
  // ------------------------------------------------------------------------------------------
  always @(*) begin
    LTCH_D_WR_U = PI_A == REG_DATA && PI_WR;
    LTCH_D_WR_L = PI_A == REG_DATA && PI_WR;
    LTCH_A_0    = PI_A == REG_ADDR_LO && PI_WR;
    LTCH_A_8    = PI_A == REG_ADDR_LO && PI_WR;
    LTCH_A_16   = PI_A == REG_ADDR_HI && PI_WR;
    LTCH_A_24   = PI_A == REG_ADDR_HI && PI_WR;
    LTCH_D_RD_OE_n = !(PI_A == REG_DATA && PI_RD);
  end

  // ------------------------------------------------------------------------------------------
  // Interrupt level (classic)
  // ------------------------------------------------------------------------------------------
  reg [2:0] ipl = 3'd0, ipl_1 = 3'd0, ipl_2 = 3'd0;
  always @(posedge c200m) begin
    if (c8m_falling) begin
      ipl_1 <= ~M68K_IPL_n;
      ipl_2 <= ipl_1;
    end
    if (ipl_2 == ipl_1)
      ipl <= ipl_2;
    PI_IPL_ZERO <= ipl == 3'd0;
  end

  // ------------------------------------------------------------------------------------------
  // A slow tick for the fan, the LED, the switch and the stock pulse: every 256 C8M rising edges
  // (32.7 us), from the fan's PWM counter.
  // ------------------------------------------------------------------------------------------
  reg [7:0] pwm_cnt = 8'd0;
  reg       tick = 1'b0;
  always @(posedge c200m) begin
    tick <= 1'b0;
    if (c8m_rising) begin
      pwm_cnt <= pwm_cnt + 8'd1;
      if (pwm_cnt == 8'hFF) tick <= 1'b1;
    end
  end
  // 30.6 kHz, 16 steps; full speed needs no clock: a fan on a card whose Pi has not started yet still turns
  assign FAN_PWM = (fan_duty == 4'hF) || (pwm_cnt[7:4] < fan_duty);

  // tachometer: a free-running count of falling edges; the kernel reads it twice and takes the difference
  reg [5:0] tach_cnt = 6'd0;
  always @(posedge c200m)
    if (!tach_sync[1] && tach_last) tach_cnt <= tach_cnt + 6'd1;

  // the stock switch: entered on its first synchronised sample (the combinational override below has
  // already let go of the lines, so the state machine must not think it still owns the bus), left only
  // once it has read open on three ticks in a row (about 100 us)
  reg [2:0] sw_hist = 3'b111;
  reg       sw_closed = 1'b0;
  always @(posedge c200m) begin
    if (tick) begin
      sw_hist <= {sw_hist[1:0], sw_sync[1]};
      if (sw_hist == 3'b111) sw_closed <= 1'b0;
    end
    if (!sw_sync[1]) sw_closed <= 1'b1;
  end
  wire stock_raw = !STOCK_SW_n;           // the switch as it is, for the combinational overrides

  // ------------------------------------------------------------------------------------------
  // Arbitration
  // ------------------------------------------------------------------------------------------
  localparam A_REQ = 3'd0, A_TAKE = 3'd1, A_OWN = 3'd2, A_RST = 3'd3, A_WATCH = 3'd4, A_STOCK = 3'd5;
  reg [2:0]  arb = A_REQ;
  reg        br_assert = 1'b1;            // power-up: /BR asserted (no clock needed)
  reg        bgack_assert = 1'b0;
  reg        own_ok = 1'b0;               // the bus may be driven
  reg [4:0]  arb_cnt = 5'd0;
  reg [STOCK_BITS-1:0] stock_cnt = {STOCK_BITS{1'b0}};
  reg        stock_pulse = 1'b0;
  wire       stock_mode = (arb == A_STOCK);
  wire       want_stock = stock_req || sw_closed;
  wire       bus_idle = !as_seen && !dtack_s && !(bgack_seen && !bgack_assert);

  always @(posedge c200m) begin
    if (want_stock && arb != A_STOCK) begin
      arb <= A_STOCK;
      br_assert <= 1'b0;
      bgack_assert <= 1'b0;
      own_ok <= 1'b0;
      stock_pulse <= 1'b1;
      stock_cnt <= {STOCK_BITS{1'b0}};
    end else case (arb)
      A_REQ: begin                        // /BR held; take the bus at the first idle moment after /BG
        br_assert <= 1'b1;
        own_ok <= 1'b0;
        if (bg_s && bus_idle && !board_reset && slot_pwr_s) begin
          bgack_assert <= 1'b1;
          arb_cnt <= 5'd0;
          arb <= A_TAKE;
        end
      end
      A_TAKE: begin                       // /BGACK on; a C8M clock later /BR off and the bus is ours
        if (c8m_rising) arb_cnt <= arb_cnt + 5'd1;
        if (arb_cnt == 5'd2) begin
          br_assert <= 1'b0;
          own_ok <= 1'b1;
          arb <= A_OWN;
        end
      end
      A_OWN: begin
        if (board_reset || !slot_pwr_s) begin   // a reset (ours or the SE's) resets the 68000 too
          own_ok <= 1'b0;
          br_assert <= 1'b1;
          arb <= A_RST;
        end
      end
      A_RST: begin                        // /BGACK and /BR held through the reset, nothing driven
        own_ok <= 1'b0;
        br_assert <= 1'b1;
        arb_cnt <= 5'd0;
        if (!board_reset && slot_pwr_s) arb <= A_WATCH;
      end
      A_WATCH: begin                      // does the 68000 stay off the bus? 16 C8M clocks of quiet /AS
        if (board_reset)
          arb <= A_RST;
        else if (as_seen)
          arb <= A_REQ;                   // it did not: /BR is still asserted, take the bus at its /BG
        else begin
          if (c8m_rising) arb_cnt <= arb_cnt + 5'd1;
          if (arb_cnt == 5'd16) begin
            arb_cnt <= 5'd0;
            arb <= A_TAKE;
          end
        end
      end
      A_STOCK: begin                      // the SE's own 68000 runs; every line three-stated
        br_assert <= 1'b0;
        bgack_assert <= 1'b0;
        own_ok <= 1'b0;
        if (stock_pulse) begin
          if (tick) stock_cnt <= stock_cnt + 1'b1;
          if (tick && &stock_cnt) stock_pulse <= 1'b0;
        end else if (!want_stock) begin
          br_assert <= 1'b1;              // back to Aurora68: request the bus again
          arb <= A_REQ;
        end
      end
      default: arb <= A_REQ;
    endcase
  end

  // open-drain drivers with combinational overrides that need no clock (no SE power, the stock switch)
  wire reset_req = stock_mode ? stock_pulse : !status_cl;
  wire drv_br    = br_assert && SLOT_PWR && !stock_raw && !stock_mode;
  wire drv_bgack = bgack_assert && SLOT_PWR && !stock_raw;
  wire drv_reset = reset_req && SLOT_PWR && (!stock_raw || stock_pulse);
  assign M68K_BR_n    = drv_br    ? 1'b0 : 1'bz;
  assign M68K_BGACK_n = drv_bgack ? 1'b0 : 1'bz;
  assign M68K_RESET_n = drv_reset ? 1'b0 : 1'bz;
  wire bus_enable = own_ok && slot_pwr_s && SLOT_PWR && !stock_raw;

  always @(posedge c200m)
    PI_RESET <= (reset_req && !stock_mode) ? 1'b1 : !board_reset;   // classic: "free" while we pull it

  // ------------------------------------------------------------------------------------------
  // The 68000 bus cycle
  // ------------------------------------------------------------------------------------------
  localparam S_IDLE = 4'd0, S_S1 = 4'd1, S_S2 = 4'd2, S_WAIT = 4'd3, S_S6 = 4'd4, S_S7 = 4'd5,
             S_S0 = 4'd6, S_VPA = 4'd7, S_VPA_E = 4'd8, S_VPA_H = 4'd9, S_VPA_F = 4'd10;
  reg [3:0]  state = S_IDLE;
  reg        op_req = 1'b0, op_rw = 1'b1, op_uds_n = 1'b1, op_lds_n = 1'b1, a0 = 1'b0;
  reg        op_ram = 1'b0;               // A23 = A22 = 0: the SE's RAM ($000000-$3FFFFF)
  reg        a_oe = 1'b0, d_wr_oe = 1'b0;
  reg        pmcyc_seen = 1'b0;
  reg        last_berr = 1'b0;
  reg [2:0]  e_low_cnt = 3'd7;            // C8M falling edges since E fell (7: not known yet)
  reg [1:0]  e_hi_cnt = 2'd0;             // C8M rising edges since E rose, within a 6800 cycle
  reg [2:0]  txn_delay = 3'd0;
  reg        as_r = 1'b1, uds_r = 1'b1, lds_r = 1'b1, rw_r = 1'b1, vma_r = 1'b1;

  always @(posedge c200m) begin
    if (e_falling) e_low_cnt <= 3'd0;
    else if (c8m_falling && !e_s && e_low_cnt != 3'd7) e_low_cnt <= e_low_cnt + 3'd1;
  end

  assign LTCH_A_OE_n    = !(a_oe && bus_enable);
  assign LTCH_D_WR_OE_n = !(d_wr_oe && bus_enable);
  assign M68K_AS_n  = bus_enable ? as_r  : 1'bz;
  assign M68K_UDS_n = bus_enable ? uds_r : 1'bz;
  assign M68K_LDS_n = bus_enable ? lds_r : 1'bz;
  assign M68K_RW    = bus_enable ? rw_r  : 1'bz;
  assign M68K_VMA_n = bus_enable ? vma_r : 1'bz;
  assign M68K_FC    = bus_enable ? 3'b101 : 3'bzzz;     // supervisor data, always; never 111 (docs/22 section 2)

  always @(posedge c200m) begin
    if (wr_rising) begin
      case (PI_A)
        REG_ADDR_LO: begin
          a0 <= PI_D[0];
          PI_TXN_IN_PROGRESS <= 1'b1;
        end
        REG_ADDR_HI: begin
          op_req <= 1'b1;
          op_rw <= PI_D[9];
          op_uds_n <= PI_D[8] ? a0 : 1'b0;
          op_lds_n <= PI_D[8] ? !a0 : 1'b0;
          op_ram <= PI_D[7:6] == 2'b00;
        end
        default: ;
      endcase
    end

    if (init_strobe || (!own_ok && state != S_IDLE)) begin
      // classic STATUS_BIT_INIT, or the bus lost in the middle of a cycle (a reset, stock mode): the
      // cycle machine back to its start, the Pi's wait ends
      state <= S_IDLE;
      op_req <= 1'b0;
      PI_TXN_IN_PROGRESS <= 1'b0;
      as_r <= 1'b1; uds_r <= 1'b1; lds_r <= 1'b1; vma_r <= 1'b1; rw_r <= 1'b1;
      a_oe <= 1'b0; d_wr_oe <= 1'b0;
      LTCH_D_RD_U <= 1'b0; LTCH_D_RD_L <= 1'b0;
    end else case (state)
      S_IDLE: begin                       // S0, waiting for the Pi and for the bus
        rw_r <= 1'b1;
        pmcyc_seen <= 1'b0;
        if (op_req && own_ok && c8m_falling) begin    // the falling edge that begins S1
          a_oe <= 1'b1;                   // address valid a half clock before /AS (docs/18 C2)
          rw_r <= op_rw;
          last_berr <= 1'b0;
          state <= S_S1;
        end
      end
      S_S1: begin
        if (pmcyc_fall) pmcyc_seen <= 1'b1;
        if (c8m_rising && (!pmcyc_align || !op_ram || pmcyc_seen || pmcyc_fall)) begin
          as_r <= 1'b0;                   // S2: /AS only ever falls here, on a C8M rising edge
          uds_r <= op_rw ? op_uds_n : 1'b1;
          lds_r <= op_rw ? op_lds_n : 1'b1;
          d_wr_oe <= !op_rw;
          op_req <= 1'b0;
          state <= S_S2;
        end
      end
      S_S2: begin
        if (c8m_falling) begin            // S3: data strobes for a write
          uds_r <= op_uds_n;
          lds_r <= op_lds_n;
          state <= S_WAIT;
        end
      end
      S_WAIT: begin                       // S4 and wait states: terminations looked at on falling edges only
        if (c8m_falling) begin
          if (dtack_s || berr_s) begin
            last_berr <= berr_s && !dtack_s;
            LTCH_D_RD_U <= 1'b1;          // transparent through S5 and S6
            LTCH_D_RD_L <= 1'b1;
            state <= S_S6;
          end else if (vpa_s)
            state <= S_VPA;
        end
      end
      S_S6: begin
        if (c8m_falling) begin            // the falling edge entering S7: where a 68000 latches
          LTCH_D_RD_U <= 1'b0;
          LTCH_D_RD_L <= 1'b0;
          state <= S_S7;
        end
      end
      S_VPA: begin                        // a 6800 cycle: /VMA while E is low, two or more clocks before E rises
        if (c8m_falling && !e_s && e_low_cnt <= 3'd3) begin
          vma_r <= 1'b0;
          state <= S_VPA_E;
        end
      end
      S_VPA_E: begin
        if (e_rising) begin
          LTCH_D_RD_U <= 1'b1;            // transparent while E is high
          LTCH_D_RD_L <= 1'b1;
          e_hi_cnt <= 2'd0;
          state <= S_VPA_H;
        end
      end
      S_VPA_H: begin
        if (c8m_rising) begin
          e_hi_cnt <= e_hi_cnt + 2'd1;
          if (e_hi_cnt == 2'd3) begin     // the fourth rising edge with E high: 447 ns after E rose
            LTCH_D_RD_U <= 1'b0;
            LTCH_D_RD_L <= 1'b0;
            state <= S_VPA_F;
          end
        end
      end
      S_VPA_F: begin
        if (e_falling) state <= S_S7;     // the VIA has taken a write's data on E's fall
      end
      S_S7: begin                         // strobes off one 5 ns tick after the latch closed (hold 0 ns)
        as_r <= 1'b1;
        uds_r <= 1'b1;
        lds_r <= 1'b1;
        vma_r <= 1'b1;
        txn_delay <= 3'b111;
        state <= S_S0;
      end
      S_S0: begin                         // the Pi may read now; data and address held to S0's start
        txn_delay <= {txn_delay[1:0], 1'b0};
        if (txn_delay[2]) PI_TXN_IN_PROGRESS <= 1'b0;
        if (c8m_rising) begin             // (PI_TXN_IN_PROGRESS is not touched here: the Pi may already
          d_wr_oe <= 1'b0;                //  have started its next transaction, which sets it again)
          a_oe <= 1'b0;
          rw_r <= 1'b1;
          state <= S_IDLE;
        end
      end
      default: state <= S_IDLE;
    endcase
  end

  // ------------------------------------------------------------------------------------------
  // Activity LED, stretched to about 4 ms
  // ------------------------------------------------------------------------------------------
  reg [6:0] act_cnt = 7'd0;
  always @(posedge c200m) begin
    if (state == S_S2) act_cnt <= 7'h7F;
    else if (tick && act_cnt != 7'd0) act_cnt <= act_cnt - 7'd1;
  end
  assign LED_ACT = act_cnt != 7'd0;

  // ------------------------------------------------------------------------------------------
  // Status read: bits 15-13 the interrupt level (classic), 12-8 the card's flags, 7-0 its inputs
  // ------------------------------------------------------------------------------------------
  reg [15:0] data_out;                  // (no power-up value: bit 12, the identity, is a constant)
  always @(posedge c200m) begin
    if (rd_rising && PI_A == REG_STATUS)
      data_out <= {ipl, 1'b1, own_ok, stock_mode, last_berr, board_reset, tach_cnt, sw_closed, slot_pwr_s};
  end
  assign PI_D = (PI_A == REG_STATUS && PI_RD) ? data_out : 16'bz;

  initial begin
    PI_TXN_IN_PROGRESS = 1'b0; PI_IPL_ZERO = 1'b0; PI_RESET = 1'b0;
    LTCH_D_RD_U = 1'b0; LTCH_D_RD_L = 1'b0;
  end
endmodule
