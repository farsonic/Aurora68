/*
 * psmac_gpio.c — the classic PiStorm's GPIO protocol, as Aurora68's transport to the Macintosh.
 * Copyright (c) 2026 Aurora68 project. MPL-2.0.
 * The bus cycle sequences are those of Emu68's ps_classic_protocol.c (MIT; Claude Schwarz 2020,
 * reorganised by Niklas Ekström 2021), which this file keeps word for word where it matters.
 *
 * The classic PiStorm is a 68000 in a DIP-64 footprint: an Altera EPM240 CPLD drives the 68000's
 * bus under the Pi's direction through sixteen data lines (GPIO 8-23), two register-select lines
 * (A0, A1: GPIO 2, 3), read and write strobes (GPIO 6, 7), a transaction-in-progress line (GPIO 0),
 * an "IPL is zero" line (GPIO 1), the motherboard's RESET (GPIO 5) and a 200 MHz clock the Pi
 * generates for the CPLD on GPIO 4. A bus cycle is three register writes (data, address low,
 * address high with the size and direction bits) and, for a read, a register read once the CPLD
 * says the cycle is over. Addresses are 24 bits, data 8 or 16; 32-bit accesses are two cycles,
 * as on a 68000.
 *
 * In the testbed every motherboard access went to MAME over a UART or semihosting (psmac_bus.c,
 * bridge_xfer). On hardware it comes here instead; the rest of the kernel does not know which.
 * The service core (core 2) is the only caller: the protocol is a state machine in the CPLD and
 * one core must own it.
 *
 * Two departures from the Amiga code. The peripherals are reached through the kernel's high alias
 * (PSMAC_PERIPH_HI), not 0xF2000000: in this kernel the low 4 GiB are the Macintosh's and the
 * service core's view of them changes with every MMU table switch. And every wait on the CPLD is
 * bounded: a Pi on the bench with no SE behind it must still boot far enough to show the second
 * screen and the log, so after PSMAC_GPIO_TIMEOUT_US a cycle is abandoned, reads return 0xFF.. and
 * the transport is reported dead, instead of spinning for ever (docs/12).
 */
#include <stdint.h>
#include "support.h"
#include "psmac.h"

#define PERI            PSMAC_PERIPH_HI
static volatile uint32_t *const gpio  = (volatile uint32_t *)(PERI + 0x200000);
static volatile uint32_t *const gpclk = (volatile uint32_t *)(PERI + 0x101000);
static volatile uint32_t *const stmr  = (volatile uint32_t *)(PERI + 0x003000);   /* system timer: CLO at +4, CHI at +8 */

#define PIN_TXN_IN_PROGRESS 0
#define PIN_IPL_ZERO 1
#define PIN_A0 2
#define PIN_A1 3
#define PIN_CLK 4
#define PIN_RESET 5
#define PIN_RD 6
#define PIN_WR 7
#define REG_DATA 0
#define REG_ADDR_LO 1
#define REG_ADDR_HI 2
#define REG_STATUS 3
#define STATUS_BIT_INIT 1
#define STATUS_BIT_RESET 2
#define TXD_BIT (1 << 26)
#define CLK_PASSWD 0x5a000000
#define CLK_GP0_CTL 0x070
#define CLK_GP0_DIV 0x074

/* GPIO function-select words: all the protocol's pins as inputs, or the data lines as outputs too.
   The Pi 4 variant leaves GPIO 28/29 to the Ethernet PHY's management lines. */
#define CLEAR_BITS_PI3 0xffffec
#define CLEAR_BITS_PI4 (0xffffec & ~((1 << 28) | (1 << 29)))
#define GPFSEL0_INPUT 0x00244240
#define GPFSEL1_INPUT 0x00000000
#define GPFSEL2_INPUT_PI3 ((1 << 18) | (1 << 21) | (1 << 27))
#define GPFSEL2_INPUT_PI4 ((1 << 18) | (1 << 21) | (2 << 27) | (2 << 24))
#define GPFSEL0_OUTPUT 0x09244240
#define GPFSEL1_OUTPUT 0x09249249
#define GPFSEL2_OUTPUT_PI3 (0x00000249 | (1 << 18) | (1 << 21) | (1 << 27))
#define GPFSEL2_OUTPUT_PI4 (0x00000249 | (1 << 18) | (1 << 21) | (2 << 27) | (2 << 24))

/* A cycle is abandoned after 300 ms: longer than the SE's own bus-error timeout (the BBU ends a cycle
   nobody answers after 265 ms, Guide to the Macintosh Family Hardware p. 126), so that no cycle the SE
   would still finish is cut short. The SCSI chip's pseudo-DMA holds /DTACK until the drive has the byte,
   which a slow drive can make long; with the 100 ms of the first version such a read came back as $FF
   bytes (found by an outside review, 21 November 2026, docs/08 item 92). */
#define PSMAC_GPIO_TIMEOUT_US 300000u

static uint32_t INPUT[3], OUTPUT[3], CLEAR_BITS;
static int pi4;                 /* a Pi 4 / CM4 strobes twice as long (its GPIO block is faster) */
static int gpio_ready;
static uint32_t timeouts;
static inline void inp(void) { gpio[0] = LE32(INPUT[0]); gpio[1] = LE32(INPUT[1]); gpio[2] = LE32(INPUT[2]); }
static inline void outp(void) { gpio[0] = LE32(OUTPUT[0]); gpio[1] = LE32(OUTPUT[1]); gpio[2] = LE32(OUTPUT[2]); }

static inline uint64_t ustime(void)
{
    uint32_t hi = LE32(stmr[2]), lo = LE32(stmr[1]);
    if (hi != LE32(stmr[2])) { hi = LE32(stmr[2]); lo = LE32(stmr[1]); }
    return ((uint64_t)hi << 32) | lo;
}
static void usleep(uint32_t us) { uint64_t t = ustime() + us; while (ustime() < t) ; }

/* a register write to the CPLD: data lines out, value and register select on, strobe, lines back in */
static inline void reg_write(uint32_t reg, uint32_t value)
{
    gpio[7] = LE32(((value & 0xffff) << 8) | (reg << PIN_A0));
    if (pi4) { gpio[7] = LE32(1 << PIN_WR); gpio[7] = LE32(1 << PIN_WR); gpio[7] = LE32(1 << PIN_WR); gpio[10] = LE32(1 << PIN_WR); gpio[10] = LE32(1 << PIN_WR); }
    else     { gpio[7] = LE32(1 << PIN_WR); gpio[10] = LE32(1 << PIN_WR); }
    gpio[10] = LE32(CLEAR_BITS);
}
/* wait for the CPLD to finish the 68000 bus cycle; 0 on success, -1 if it never did */
static inline int txn_wait(void)
{
    uint32_t n = 0;
    while (gpio[13] & LE32(1 << PIN_TXN_IN_PROGRESS)) {
        if ((++n & 1023) == 0) { static uint64_t t0; if (n == 1024) t0 = ustime(); else if (ustime() - t0 > PSMAC_GPIO_TIMEOUT_US) { timeouts++; return -1; } }
    }
    return 0;
}

static void status_write(uint32_t value)
{
    outp();
    reg_write(REG_STATUS, value);
    inp();
}
static uint32_t status_read(void)
{
    gpio[7] = LE32(REG_STATUS << PIN_A0);
    gpio[7] = LE32(1 << PIN_RD); gpio[7] = LE32(1 << PIN_RD); gpio[7] = LE32(1 << PIN_RD); gpio[7] = LE32(1 << PIN_RD);
    if (pi4) { gpio[7] = LE32(1 << PIN_RD); gpio[7] = LE32(1 << PIN_RD); gpio[7] = LE32(1 << PIN_RD); gpio[7] = LE32(1 << PIN_RD); }
    uint32_t value = LE32(gpio[13]);
    gpio[10] = LE32(CLEAR_BITS);
    return (value >> 8) & 0xffff;
}

/* The 200 MHz clock for the CPLD on GPIO 4: GPCLK0 from PLLC's peripheral output (clock source 5),
   divided by 6 on a Pi 3 and by 5 on a Pi 4, as Emu68's ps_classic_protocol.c does on every board.
   Those numbers assume PLLC_PER at 1200 and 1000 MHz, the firmware's settings for the default core
   clock; the firmware picks PLLC, and a core_freq= in config.txt can move it. So the PLL is read
   back first (its registers as Linux's clk-bcm2835 describes them: NDIV, PDIV and the 20-bit
   fraction from the 19.2 MHz crystal, doubled when the feedback prescaler is on, then the PER
   channel's divider), the usual divider is kept whenever it gives 200 MHz within a tenth, and only
   a PLLC elsewhere gets the divider nearest 200 MHz. The log says what came out. The 3B, 3A+ and
   Zero 2 W all run their core clock at 400 MHz by default, and upstream's divider of 6 is what
   Emu68 uses on every Pi 3 board, the 3A+ the classic PiStorm was laid out for among them
   (20 November 2026: the PLL read here, not yet on a 3A+ or Zero 2 W). */
#define XOSC_HZ         19200000u
#define A2W_PLLC_CTRL   0x1120
#define A2W_PLLC_ANA1   0x1034
#define A2W_PLLC_FRAC   0x1220
#define A2W_PLLC_PER    0x1520
static uint32_t gpclk_div, gpclk_src_mhz;
static uint32_t pllc_per_hz(void)
{
    uint32_t ctrl = LE32(gpclk[A2W_PLLC_CTRL / 4]), frac = LE32(gpclk[A2W_PLLC_FRAC / 4]) & 0xfffffu;
    uint32_t ndiv = ctrl & 0x3ffu, pdiv = (ctrl >> 12) & 7u, chan = LE32(gpclk[A2W_PLLC_PER / 4]) & 0xffu;
    if (!ndiv || !pdiv || (ctrl & (1u << 16)))              /* not set up, or powered down */
        return 0;
    uint64_t vco = ((uint64_t)XOSC_HZ * (((uint64_t)ndiv << 20) + frac)) >> 20;
    if (LE32(gpclk[A2W_PLLC_ANA1 / 4]) & (1u << 14))         /* the feedback prescaler doubles it */
        vco *= 2;
    vco /= pdiv;
    return (uint32_t)(vco / (chan ? chan : 256));
}
static void setup_gpclk(void)
{
    uint32_t usual = pi4 ? 5 : 6, per = pllc_per_hz();
    gpclk_div = usual;
    if (per >= 400000000u && per <= 3200000000u) {
        uint32_t f = per / usual;
        if (f < 180000000u || f > 220000000u) {
            gpclk_div = (per + 100000000u) / 200000000u;
            if (gpclk_div < 2) gpclk_div = 2;
        }
    }
    else
        per = 0;                                            /* not a PLL rate: the registers mean something else */
    gpclk_src_mhz = per / 1000000u;
    gpclk[CLK_GP0_CTL / 4] = LE32(CLK_PASSWD | (1 << 5));
    usleep(10);
    { uint32_t n = 0; while ((gpclk[CLK_GP0_CTL / 4] & LE32(1 << 7)) && ++n < 1000000) ; }
    usleep(100);
    gpclk[CLK_GP0_DIV / 4] = LE32(CLK_PASSWD | (gpclk_div << 12));
    usleep(10);
    gpclk[CLK_GP0_CTL / 4] = LE32(CLK_PASSWD | 5 | (1 << 4));
    usleep(10);
    { uint32_t n = 0; while (!(gpclk[CLK_GP0_CTL / 4] & LE32(1 << 7)) && ++n < 1000000) ; }
    usleep(100);
    gpio[PIN_CLK / 10] |= LE32(4 << ((PIN_CLK % 10) * 3));      /* GPIO 4: alternate function 0, GPCLK0 */
}

/* The CPLD's JTAG IDCODE and USERCODE (22 November 2026, docs/18 K2 and docs/08 item 108): which firmware the
   PiStorm carries decides when it samples the data of a read (the stock image of today, usercode 00185866,
   latches about 128 ns before a 68000 would; the pre-June-2021 original, 0018736F, in S7), and nothing said
   which one a tester has. Emu68's cpld.c reads it through libxsvf, which the psmac build does not link; the
   two instructions it needs are read here by hand, on the JTAG pins the classic PiStorm wires to GPIO 24
   (TMS), 25 (TDO), 26 (TCK) and 27 (TDI), as cpld.c names them. Only IDCODE and USERCODE are loaded, both
   of which leave the device's logic running; the TAP is put back in Test-Logic-Reset after. GPIO 24-27
   are outside the bus protocol's lines (0-23), and GPFSEL2's other fields (GPIO 20-23, the data lines'
   top four, and 28-29) are kept as they are and the register restored afterwards: the protocol rewrites
   it at every cycle anyway (inp/outp). The service core calls this between bus cycles, never during one.
   MAX II (EPM240/EPM570) and MAX V: a 10-bit instruction register, IDCODE 0x006, USERCODE 0x007. */
static uint32_t cpld_id, cpld_uc; static int cpld_read;
int psmac_gpio_cpld(uint32_t *id, uint32_t *uc) { *id = cpld_id; *uc = cpld_uc; return cpld_read; }
#define JT_TMS 24
#define JT_TDO 25
#define JT_TCK 26
#define JT_TDI 27
static void jt_wait(void) { usleep(2); }
static int jt_clock(int tms, int tdi)               /* one TCK: TDO sampled while TCK is low, the TAP moves on the rising edge */
{
    gpio[tms ? 7 : 10] = LE32(1u << JT_TMS);
    gpio[tdi ? 7 : 10] = LE32(1u << JT_TDI);
    gpio[10] = LE32(1u << JT_TCK);
    jt_wait();
    int tdo = (LE32(gpio[13]) >> JT_TDO) & 1;
    gpio[7] = LE32(1u << JT_TCK);
    jt_wait();
    return tdo;
}
static uint32_t jt_shift_dr32(void)                 /* from Run-Test/Idle: Select-DR, Capture-DR, Shift-DR x32, Exit1, Update, Idle */
{
    jt_clock(1, 0); jt_clock(0, 0); jt_clock(0, 0);
    uint32_t v = 0;
    for (int i = 0; i < 32; i++) v |= (uint32_t)jt_clock(i == 31, 0) << i;
    jt_clock(1, 0); jt_clock(0, 0);
    return v;
}
static void jt_shift_ir(uint32_t ir)                /* from Run-Test/Idle: Select-DR, Select-IR, Capture-IR, Shift-IR x10, Exit1, Update, Idle */
{
    jt_clock(1, 0); jt_clock(1, 0); jt_clock(0, 0); jt_clock(0, 0);
    for (int i = 0; i < 10; i++) jt_clock(i == 9, (int)(ir >> i) & 1);
    jt_clock(1, 0); jt_clock(0, 0);
}
int psmac_gpio_jtag(uint32_t *idcode, uint32_t *usercode)
{
    if (!gpio_ready) return -1;
    uint32_t sel2 = LE32(gpio[2]), keep = sel2 & ~(0xfffu << 12);   /* GPIO 24-27: function fields at bits 12-23 */
    gpio[2] = LE32(keep | (1u << 12) | (0u << 15) | (1u << 18) | (1u << 21));   /* TMS, TCK, TDI outputs, TDO an input */
    for (int i = 0; i < 6; i++) jt_clock(1, 0);     /* Test-Logic-Reset from anywhere */
    jt_clock(0, 0);                                 /* Run-Test/Idle */
    jt_shift_ir(0x006); *idcode = jt_shift_dr32();
    jt_shift_ir(0x007); *usercode = jt_shift_dr32();
    for (int i = 0; i < 6; i++) jt_clock(1, 0);     /* Test-Logic-Reset: the device's own logic as before */
    gpio[2] = LE32(sel2);
    return 0;
}
/* the usercodes Emu68's cpld.c knows, with what docs/18 section 4 found of the ones that matter here */
const char *psmac_cpld_name(uint32_t uc)
{
    static const struct { uint32_t code; const char *name; } t[] = {
        { 0x00185866, "EPM240_experimental (stock, samples early)" },
        { 0x0018569E, "EPM240_bitstream (June 2021)" },
        { 0x0018736F, "EPM240_original (pre-2021, samples in S7)" },
        { 0x00186C9D, "EPM240_long_hold" }, { 0x00187FC6, "EPM240_longer_hold" }, { 0x00187C10, "EPM240_long_data" },
        { 0x00189844, "EPM240_PiStormX_basic" }, { 0x0018A0B9, "EPM240_PiStormX_skip_s5s6" },
        { 0x00336118, "EPM570 bitstream" }, { 0x003354F0, "EPM570 bitstream_experimental" }, { 0x00337130, "EPM570 bitstream_long_data" },
        { 0x00336000, "EPM570 bitstream_long_hold" }, { 0x00338451, "EPM570 bitstream_longer_hold" }, { 0x0033700A, "EPM570 bitstream_original" },
        { 0x00187AFD, "MAX V 5M240Z maxv_bitstream" },
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) if (t[i].code == uc) return t[i].name;
    if (uc == 0xffffffffu || uc == 0) return "no answer on the JTAG pins";
    return "not a usercode Emu68 knows";
}

/* the CPLD's clock as set up, for the bus check's report (psmac_buscheck.c): PLLC_PER in MHz (0 unread), the divider, the board's timings */
void psmac_gpio_clock(uint32_t *src_mhz, uint32_t *div, int *is_pi4) { *src_mhz = gpclk_src_mhz; *div = gpclk_div; *is_pi4 = pi4; }
int psmac_gpio_active(void) { return gpio_ready; }     /* the PiStorm transport owns GPIO 0-27 (psmac_bus.c's console asks) */
int psmac_gpio_reset_asserted(void) { return gpio_ready && (LE32(gpio[13]) & (1 << PIN_RESET)) == 0; }

/* Pulse the motherboard's RESET (every chip back to its reset state). Waits first for the line to
   be free, since at power-on the SE's own reset circuit holds it for a while. */
static void pulse_reset(uint32_t low_us, uint32_t after_us)
{
    uint64_t t0 = ustime();                                 /* two seconds at most: with no board the line floats low (the first bench Pi waited five) */
    while ((LE32(gpio[13]) & (1 << PIN_RESET)) == 0 && ustime() - t0 < 2000000) __asm__ volatile("yield");
    status_write(0);
    usleep(low_us);
    status_write(STATUS_BIT_RESET);
    usleep(after_us);
}
/* The 68000's RESET instruction, the ROM's first act on every boot: a real 68000 holds the line
   for 124 clocks (16 us at 7.83 MHz) and goes straight on. This held it 30 ms and waited 10 ms
   more, on the 68k's own path at every start (docs/08 item 50); one millisecond each way is still
   sixty times what the SE's chips get from its own processor. The power-on pulse keeps the long
   timing (psmac_gpio_init, on the service core, before the 68k is released). */
void psmac_gpio_pulse_reset(void) { pulse_reset(1000, 1000); }

void psmac_gpio_init(void)
{
    uint64_t frq; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(frq));
    /* The Pi 4 timings for a BCM2711 board (4B, 400, CM4), the Pi 3 ones for every BCM2837 board,
       the 3B, 3B+, 3A+ and Zero 2 W. The strobes below are counted in GPIO register writes, whose
       length is the peripheral bus's (the core clock, 400 MHz on all four), not the ARM's (1.2,
       1.4, 1.4 and 1.0 GHz): one set of Pi 3 timings serves them all, as in upstream Emu68. The
       board type decides (psmac_usbnet.c); without one, the generic timer's frequency as upstream
       does: 54 MHz on a Pi 4, 19.2 MHz on a Pi 3 (QEMU's raspi3 says 62.5, which until 20 November
       2026 gave the testbed the Pi 4 timings) */
    extern int psmac_board_type(void);
    int t = psmac_board_type();
    pi4 = t >= 0 ? (t == 0x11 || t == 0x13 || t == 0x14) : frq > 20000000;
    CLEAR_BITS = pi4 ? CLEAR_BITS_PI4 : CLEAR_BITS_PI3;
    OUTPUT[0] = GPFSEL0_OUTPUT; OUTPUT[1] = GPFSEL1_OUTPUT; OUTPUT[2] = pi4 ? GPFSEL2_OUTPUT_PI4 : GPFSEL2_OUTPUT_PI3;
    INPUT[0] = GPFSEL0_INPUT;   INPUT[1] = GPFSEL1_INPUT;   INPUT[2] = pi4 ? GPFSEL2_INPUT_PI4 : GPFSEL2_INPUT_PI3;
    setup_gpclk();
    gpio[10] = LE32(CLEAR_BITS);
    inp();
    gpio[7] = LE32(TXD_BIT);
    gpio_ready = 1;
    /* the CPLD's state machine to its start, then the motherboard's reset */
    status_write(STATUS_BIT_INIT);
    usleep(1500);
    status_write(0);
    usleep(100);
    pulse_reset(30000, 10000);
    if (gpclk_src_mhz)
        kprintf("[PSMAC:GPIO] CPLD clock on GPIO 4: PLLC_PER %u MHz / %u = %u MHz%s\n", gpclk_src_mhz, gpclk_div, gpclk_src_mhz / gpclk_div,
                gpclk_div == (pi4 ? 5u : 6u) ? "" : " (not the usual divider: PLLC is not where the firmware usually puts it)");
    else
        kprintf("[PSMAC:GPIO] CPLD clock on GPIO 4: PLLC unreadable, divider %u as usual (200 MHz from a %u MHz PLLC)\n", gpclk_div, pi4 ? 1000 : 1200);
    kprintf("[PSMAC:GPIO] classic PiStorm protocol up (%s timings, status %04x, RESET %s)\n",
            pi4 ? "Pi 4" : "Pi 3", status_read(), psmac_gpio_reset_asserted() ? "asserted" : "free");
    /* which firmware the CPLD carries (above, psmac_gpio_jtag), once, before the 68k's first cycle;
       mac.jtag=off leaves the JTAG pins alone */
    extern const char *psmac_option(const char *key);
    const char *jo = psmac_option("mac.jtag");
    if (!jo || jo[0] != 'o' || jo[1] != 'f') {
        if (psmac_gpio_jtag(&cpld_id, &cpld_uc) == 0) {
            cpld_read = 1;
            kprintf("[PSMAC:GPIO] CPLD JTAG: IDCODE %08x%s, usercode %08x: %s\n", cpld_id,
                    (cpld_id & 0x0fffffffu) == 0x020a10ddu ? " (EPM240)" : (cpld_id & 0x0fffffffu) == 0x020a20ddu ? " (EPM570)" : "", cpld_uc, psmac_cpld_name(cpld_uc));
        }
    }
}

/* ---- the 68000 bus cycles. Address high register: bits 0-7 are A16-A23, bit 8 is "byte",
   bit 9 is "read"; the CPLD makes UDS/LDS from them and A0. ---- */
static void write16(uint32_t address, uint32_t data)
{
    outp();
    reg_write(REG_DATA, data);
    reg_write(REG_ADDR_LO, address);
    reg_write(REG_ADDR_HI, (address >> 16) & 0xff);
    inp();
    txn_wait();
}
static void write8(uint32_t address, uint32_t data)
{
    data = (data & 0xff) | (data << 8);                    /* the byte on both halves: the CPLD strobes the right one */
    outp();
    reg_write(REG_DATA, data);
    reg_write(REG_ADDR_LO, address);
    reg_write(REG_ADDR_HI, 0x0100 | ((address >> 16) & 0xff));
    inp();
    txn_wait();
}
static uint32_t read_cycle(uint32_t address, uint32_t hi_bits)
{
    outp();
    reg_write(REG_ADDR_LO, address);
    reg_write(REG_ADDR_HI, hi_bits | ((address >> 16) & 0xff));
    inp();
    gpio[7] = LE32(REG_DATA << PIN_A0);
    gpio[7] = LE32(1 << PIN_RD);
    if (pi4) { gpio[7] = LE32(1 << PIN_RD); gpio[7] = LE32(1 << PIN_RD); gpio[7] = LE32(1 << PIN_RD); }
    int timed_out = txn_wait();
    uint32_t value = LE32(gpio[13]);
    gpio[10] = LE32(CLEAR_BITS);
    return timed_out ? 0xffff : (value >> 8) & 0xffff;
}
static uint32_t read16(uint32_t address) { return read_cycle(address, 0x0200); }
static uint32_t read8(uint32_t address)
{
    uint32_t v = read_cycle(address, 0x0300);
    return (address & 1) ? v & 0xff : (v >> 8) & 0xff;      /* even: upper byte (UDS); odd: lower (LDS) */
}

uint32_t psmac_gpio_read(uint32_t address, int size)
{
    address &= 0xffffff;
    switch (size) {
    case 1: return read8(address);
    case 2: if (address & 1) return (read8(address) << 8) | read8(address + 1);
            return read16(address);
    default:
        if (address & 1) return (read8(address) << 24) | (psmac_gpio_read(address + 1, 2) << 8) | read8(address + 3);
        return (read16(address) << 16) | read16(address + 2);
    }
}
void psmac_gpio_write(uint32_t address, int size, uint32_t data)
{
    address &= 0xffffff;
    switch (size) {
    case 1: write8(address, data); break;
    case 2: if (address & 1) { write8(address, data >> 8); write8(address + 1, data); } else write16(address, data); break;
    default:
        if (address & 1) { write8(address, data >> 24); psmac_gpio_write(address + 1, 2, data >> 8); write8(address + 3, data); }
        else { write16(address, data >> 16); write16(address + 2, data); }
    }
}

/* the interrupt level the motherboard drives: bits 13-15 of the status register (0 = none) */
unsigned psmac_gpio_ipl(void) { return (status_read() >> 13) & 7; }
int psmac_gpio_ipl_zero(void) { return (LE32(gpio[13]) & (1 << PIN_IPL_ZERO)) != 0; }
uint32_t psmac_gpio_timeouts(void) { return timeouts; }
