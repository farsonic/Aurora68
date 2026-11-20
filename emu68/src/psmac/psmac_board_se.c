/*
 * psmac_board_se.c — the Macintosh SE's logic board behind the Macintosh IIci ROM.
 * Copyright (c) 2026 PiStorm-SE project. MPL-2.0.
 *
 * The ROM on the Pi is a IIci's and talks to a IIci: a VIA at 0x50F00000, an SCC at 0x50F04000,
 * an NCR 5380 at 0x50F10000 with its pseudo-DMA windows, a SWIM, an ASC, a VDAC, the RBV with
 * the slot interrupts and the monitor sense. The board in the socket is a Macintosh SE's: the
 * same VIA at $EFE1FE, the same SCC at $9FFFF8 (reads) and $BFFFF9 (writes), the same 5380 at
 * $5FF000 with a pseudo-DMA window at $5FF200, an IWM at $DFE1FF, 4 MB of RAM at 0 whose top
 * holds the 512 by 342 screen the BBU scans out, and nothing else (docs/01, docs/03). This file
 * is the translation: the first four chips are the real ones behind an address change and, for
 * the VIA, two port bits whose meaning differs; the RBV, ASC and VDAC are answered here; the
 * SE's screen is fed from the slot-9 card's frame buffer, row by row, as it changes.
 *
 * Selected with mac.board=se (the testbed's IIci motherboard is the default); exercised against
 * MAME's macse with the bridge CPU (reference/mame-psmac.patch: macse_ps) before the hardware.
 * Written 17 November 2026, 04:00 (docs/12).
 *
 * The 68000 bus is 16 bits wide. A long read of the SCSI pseudo-DMA window, which the IIci ROM's
 * driver does to move four bytes at once, becomes four byte reads here; the wire (GPIO or
 * bridge) splits ordinary longs itself.
 */
#include <stdint.h>
#include "support.h"
#include "psmac.h"
#include "psmb.h"
#include "psmac_pram_default.h"

int psmac_board_se;                         /* mac.board=se (also set by mac.board=solo, psmac_board_solo.c) */
extern int psmac_board_solo;
int psmac_solo_access(uint32_t addr, int size, int write, uint32_t data, uint32_t *val);
unsigned psmac_solo_ipl(void); void psmac_solo_level_now(void); void psmac_solo_service(void); void psmac_solo_options(const char *); void psmac_solo_report(void);
static uint32_t se_ram_mb = 4;              /* mac.seram=N: the SE's own memory, 1, 2, 2.5 or 4 MB (its screen sits at the top) */
static uint32_t se_ram_size(void) { return se_ram_mb << 20; }
/* mac.selinefault= (testbed, docs/08 item 68): an address-line fault on the wire to the SE's RAM (below
   $400000), so the address-line test below can be tried. N: line N stuck high; N,M: line N joined to
   M, M winning (the user's PiStorm had A14 and A15 bridged); N&M: the two joined, a 0 on either
   winning */
static uint32_t se_fault_or; static int se_fault_n, se_fault_m, se_fault_and;
uint32_t psmac_se_wire_addr(uint32_t a)
{
    if (a >= 0x400000u) return a;
    a |= se_fault_or;
    if (se_fault_m && se_fault_and) { uint32_t b = (a >> se_fault_n) & (a >> se_fault_m) & 1; a = (a & ~(1u << se_fault_n) & ~(1u << se_fault_m)) | b << se_fault_n | b << se_fault_m; }
    else if (se_fault_m) a = (a & ~(1u << se_fault_n)) | (((a >> se_fault_m) & 1) << se_fault_n);
    return a;
}
uint32_t psmac_se_fb_base(void) { return se_ram_size() - 0x5900; }   /* the main screen buffer: 21,888 bytes, 64 per row */

/* the transport, as the bus layer exposes it to this file (psmac_bus.c) */
uint32_t psmac_wire_read(uint32_t addr, int size);
void psmac_wire_write(uint32_t addr, int size, uint32_t data, int posted);

/* ---- the chips the SE does not have: a pseudo-VIA (RBV), an ASC, a VDAC ------------------- */
static uint8_t rbv_regs[256], rbv_ifr, rbv_ier;   /* MAME's rbv.cpp is the model: IFR/IER with the bit-7 set/clear convention */
static uint8_t asc_regs[0x1000];
static uint8_t vdac_regs[32];
static uint32_t local_reads, local_writes;
static uint32_t se_trace_left;              /* mac.setrace=N[,dev]: the first N translated accesses (of one chip), with their values */
static char se_trace_dev[5];
static void trace(const char *what, int write, uint32_t addr, uint32_t se, int size, uint32_t v)
{
    static uint32_t hist_se[4] = { 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu }, hist_v[4], hist_n, repeats;
    if (!se_trace_left) return;
    if (se_trace_dev[0] && (what[0] != se_trace_dev[0] || what[1] != se_trace_dev[1] || what[2] != se_trace_dev[2])) return;
    /* a read that repeats one of the last four (register, value) pairs is a poll: counted, not
       printed, so a loop over two or three registers does not use up the trace before anything
       happens; a write ends the run */
    if (!write) {
        for (int i = 0; i < 4; i++) if (hist_se[i] == se && hist_v[i] == v) { repeats++; return; }
        hist_se[hist_n & 3] = se; hist_v[hist_n & 3] = v; hist_n++;
    } else { for (int i = 0; i < 4; i++) hist_se[i] = 0xffffffffu; }
    if (repeats) { kprintf("[PSMAC:SE]   ... %u polled reads folded\n", repeats); repeats = 0; }
    se_trace_left--;
    kprintf("[PSMAC:SE] %s %c%d %08x -> $%06x %s %08x\n", what, write ? 'W' : 'R', size, addr, se, write ? "<-" : "=", v);
}

/* the slot-9 line of the Pi's card (psmac_bus.c raises it): bit 0 of the slot IFR, 0 = asserted */
extern uint32_t psmac_slot_irq_state(void);
static int slot_line(void) { return (psmac_slot_irq_state() >> 16) & 1; }

int psmac_solo_scsi_irq(void);
static void rbv_recalc(void)
{
    uint8_t ifr = rbv_ifr & 0x7f;
    if (slot_line() && (rbv_regs[0x12] & 0x01)) ifr |= 0x02;          /* any slot interrupt, gated by the slot enable register */
    else ifr &= ~0x02;
    if (psmac_board_solo) { if (psmac_solo_scsi_irq()) ifr |= 0x08; else ifr &= ~0x08; }   /* the 5380's interrupt, level-sensitive */
    else ifr &= ~0x08;
    /* On the SE the 5380's interrupt never reaches this file (port B bit 6 keeps it off the SE's
       bus, above), so the flag is 0 whatever the ROM wrote. Until 20 November 2026 it kept the 1
       the ROM's power-on test writes into the IFR (MAME's "1 bits write 1s"; the solo board
       recomputes it from its model), so whenever the SCSI Manager enabled the interrupt (IER
       bit 3, PC $4080A08A) the RBV raised level 2 at once, for nothing, until it was disabled
       again. The real SE at 8 MiB (docs/08 item 63) stalled with IFR $08 and IER $08, the main
       code waiting for an I/O at $4080EFDE and every bus cycle a read of the 5380's
       reset-interrupt register ($5FF070), which is what that interrupt's handler reads; the MAME
       SE, far slower per cycle, always got past. The MAME SE boots to the Finder with the flag
       held at 0 in the same time as before (145 s), so nothing was waiting on it. */
    rbv_ifr = ifr;
}
/* the level-2 interrupt the RBV would raise: any enabled flag */
int psmac_se_rbv_irq(void) { rbv_recalc(); return (rbv_ifr & rbv_ier & 0x7f) != 0; }

static uint32_t rbv_read(uint32_t off)
{
    uint32_t reg = off & 0x1fff;
    if (reg >= 0x100) { reg >>= 9; if (reg == 13) { rbv_recalc(); return rbv_ifr | ((rbv_ifr & rbv_ier & 0x7f) ? 0x80 : 0); } if (reg == 14) return rbv_ier; return 0; }
    switch (reg) {
    case 0x02: return (rbv_regs[2] & 0x7f) | (slot_line() ? 0 : 0x01) | 0x3e;   /* slot IFR: slots A-E idle (1), slot 9 ours */
    case 0x03: rbv_recalc(); return rbv_ifr | ((rbv_ifr & rbv_ier & 0x7f) ? 0x80 : 0);
    case 0x10: return (rbv_regs[0x10] & ~0x38) | (7 << 3);          /* monitor sense 7: no monitor on the built-in video */
    case 0x12: return rbv_regs[0x12] & 0x7f;
    case 0x13: return rbv_ier & 0x7f;
    default:   return rbv_regs[reg];
    }
}
static void rbv_write(uint32_t off, uint8_t data)
{
    uint32_t reg = off & 0x1fff;
    if (reg >= 0x100) { reg >>= 9; if (reg == 13) { if (data & 0x80) rbv_ifr |= data & 0x7f; else rbv_ifr &= ~(data & 0x7f); } else if (reg == 14) { if (data & 0x80) rbv_ier |= data & 0x7f; else rbv_ier &= ~(data & 0x7f); } return; }
    switch (reg) {
    case 0x02: rbv_regs[2] |= data & 0x40; break;
    case 0x03: if (data & 0x80) rbv_ifr |= data & 0x7f; else rbv_ifr &= ~(data & 0x7f); break;
    case 0x12: if (data & 0x80) rbv_regs[0x12] |= data & 0x7f; else rbv_regs[0x12] &= ~(data & 0x7f); break;
    case 0x13: if (data & 0x80) { rbv_ier |= data & 0x7f; if (data == 0xff) rbv_ier = 0x1f; } else rbv_ier &= ~(data & 0x7f); break;   /* the ROM's POST writes $FF and expects $1F back (MAME's note) */
    default:   rbv_regs[reg] = data; break;
    }
}
/* the ASC: no sound chip on an SE. Its 2 KB of wave RAM ($000-$7FF) is real memory here, because
   the ROM's tests write it and read it back (the first SE boots failed exactly there, fs174, and
   went to the serial-console error report); version 0; the other registers remembered and handed
   back.

   The FIFOs (mode register $801 = 1) fill and drain as the chip's do: each write to $000-$3FF
   (channel A) or $400-$7FF (B) adds a byte, the chip plays them at 22257 per second, and the
   status register ($804) says half empty (bits 0 and 2) below 512 bytes and full (bits 1 and 3)
   at 1024. They used to read "half empty, never full", and the Sound Manager's feeder, which
   writes until the FIFO is full and runs from the main screen's VBL task (our card's display on
   the SE and the solo board), wrote up to 65535 bytes, three trapped accesses each, every
   sixtieth of a second: seconds per tick, a Mac that froze whenever a sound played (the end of
   Speedometer's tests, Lemmings), 18 November. Bit 7 of $803 empties both. */
static uint32_t asc_level[2];
static uint64_t asc_t[2];
#define ASC_RATE 22257u
static inline uint64_t asc_now(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v)); return v; }
static inline uint64_t asc_hz(void)  { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v; }
static void asc_drain(int ch)
{
    uint64_t now = asc_now(), hz = asc_hz();
    uint64_t played = (now - asc_t[ch]) * ASC_RATE / hz;
    if (!asc_level[ch] || played >= asc_level[ch]) { asc_level[ch] = 0; asc_t[ch] = now; return; }
    asc_level[ch] -= (uint32_t)played;
    asc_t[ch] += played * hz / ASC_RATE;
}
static uint32_t asc_read(uint32_t off)
{
    off &= 0xfff;
    if (off == 0x800) return 0;
    if (off == 0x804) {
        if (asc_regs[0x801] != 1) return 0x05;      /* not in FIFO mode: nothing plays, both half empty */
        uint32_t st = 0;
        for (int ch = 0; ch < 2; ch++) {
            asc_drain(ch);
            if (asc_level[ch] < 512) st |= 1u << (2 * ch);
            if (asc_level[ch] >= 1024) st |= 2u << (2 * ch);
        }
        return st;
    }
    return asc_regs[off];
}
static void asc_write(uint32_t off, uint8_t data)
{
    off &= 0xfff;
    asc_regs[off] = data;
    if (off < 0x800 && asc_regs[0x801] == 1) {
        int ch = off >= 0x400;
        asc_drain(ch);
        if (asc_level[ch] < 1024) asc_level[ch]++;
    } else if (off == 0x803 && (data & 0x80)) {
        asc_level[0] = asc_level[1] = 0;
    }
}

/* ---- the VIA: the same chip, two bits apart --------------------------------------------------
   Port A on the IIci: 7 SCC wait/request, 6 (unused), 5 floppy head select, 4 overlay, 3 sync
   modem, 2-0 unused. On the SE: 7 SCC wait/request, 6 screen buffer (1 = main), 5 head select,
   4 floppy drive select (0 = internal), 3 unused, 2-0 speaker volume. Bit 6 is forced to 1 so the
   SE scans out its main buffer whatever the ROM wrote; the ROM's clearing of the overlay (bit 4)
   selects the internal drives, which is what we want. Port B is the same on both (RTC on 0-2,
   ADB interrupt 3, ADB state 4-5, sound enable 7) except bit 6: the SE's SCSI interrupt enable,
   active low; it is forced to 1, so the 5380's interrupt never reaches the 68000's level 2, which
   the IIci ROM would take for its RBV. The data-direction registers get the same bits set so
   the forced values are driven. */
#define SE_VIA   0xEFE1FEu
static uint32_t via_data(uint32_t reg, uint32_t data)
{
    switch (reg) {
    case 0: case 2: return data | 0x40;                /* ORB / DDRB */
    case 1: case 3: case 15: return data | 0x40;       /* ORA / DDRA / ORA without handshake */
    default: return data;
    }
}
/* What the ROM reads back from port A is the machine's identity: the Mac II family wires bits
   6, 2, 1 and 0 of VIA1 port A as a box code, and the IIci's reads as $C7 (MAME's maciici:
   PA6, PA2, PA1 with PA7 and PA0 high). An SE's port A reads $81 (PA7 and PA0), which the IIci
   ROM takes for another box and sets flags for that never let it past the SCC check (fs172, PC
   $40842F6A). So the identity bits are the IIci's; bits 7, 5, 4 and 3 are the SE's own: the SCC
   wait/request line and the latches the ROM wrote (overlay, head select, sync). */
/* Port A bit 4 decides one more thing. The ROM's product table (0x408032C4) has two entries for the
   IIci's decoder, told apart by bit 4 read while the ROM has made it an input (the probe at
   0x4080321A masks DDRA): 0 is the IIci (0x408036C0), 1 the IIci with parity (0x40803700), whose
   UnivROMFlags ($DD4) bit 0 says parity hardware is there. With that bit the System's IIci patch
   (PTCH 1660) answers Gestalt 'prty' with "parity capable", "enabled" only if VIA port B bit 6
   reads 0, which on this board it never does (via_read_fix below), and its INIT 10 then shows
   "Parity has been disabled because the parity circuitry is not functioning" at every start. On an
   SE port A bit 4 is the floppy drive select, an output the ROM drives; as an input it reads
   whatever the SE's pin is left at, and on one start of the user's SE (20 November) it read 1. So
   while DDRA bit 4 is an input the ROM reads 0 there: no parity, as the Pi's memory has none.
   mac.sepa4=1 (testbed) reads 1 instead, as that SE did (docs/08 item 69). */
static uint8_t via_ddra, se_pa4_sim;
static uint32_t via_read_fix(uint32_t reg, uint32_t r)
{
    if (reg == 1 || reg == 15) {
        r = (r & 0xb8) | 0x47;
        if (!(via_ddra & 0x10)) r = (r & ~0x10u) | (se_pa4_sim ? 0x10u : 0);
        return r;
    }
    if (reg == 0) return r | 0x40;                   /* PB6: an input tied high on the IIci; the SE's video beam / SCSI interrupt enable */
    return r;
}

/* ---- the clock chip: what the IIci ROM makes of an SE's parameter RAM ----------------------
   The SE's clock chip (343-0042-B, 256 bytes of parameter RAM) sits behind VIA port B as on the
   IIci: bit 2 enable (low = selected), bit 1 clock, bit 0 data, MSB first. The ROM sets each bit
   it sends with the clock low and raises the clock; for a byte it receives it turns DDRB bit 0
   into an input, lowers and raises the clock and reads port B after the rise (0x4080B1E4-B26A).
   This file follows that protocol on the ROM's writes to port B, so it always knows which byte is
   in flight, and on three occasions hands the ROM something other than what the chip says (the
   third, the default startup device at $78-$7B when a disk image on the card is to be the startup
   disk, is psmac_mbox.c's business: docs/08 item 67).

   Byte $8A. The IIci ROM reads extended byte $8A before it builds its MMU tables (0x408043D4) and
   that byte decides the memory layout: bit 0 32-bit addressing, bit 2 the same for the next start
   (docs/08 item 26), bit 4 a layout with RAM at logical $38000000 (0x408043EE adds $37FF0000 to the
   logical top of RAM). The first boot on a real SE (20 November 2026) read it with bit 4 set: with
   128 MiB the table builder at 0x4080476C ran past its 8 KiB frame on the stack and returned to
   $00000011, one of its own descriptors; with mac.ram=8 the ROM got past that, mapped RAM at
   $38000000 and never started the video. The MAME SE with $8A = $15 reproduces the crash register
   for register (docs/08 item 63). So on the SE board the byte is mended on its way to the ROM,
   every time it is read, whoever reads it (the ROM's start-up code or _ReadXPRam): bit 4 cleared
   always, bits 0 and 2 set when the Mac has more than 8 MiB (in 24-bit mode the rest is unused, and
   the HDMI screen's thousands and millions need 32-bit addressing), the other bits as they are.
   (System 7.1 writes back the mode it started in, $05, so after one boot a working chip agrees.)
   mac.pram8a=keep turns this off.

   A chip that does not answer. The second boot on the SE (kernel c6d203f, 20 November) read $FF
   from byte $8A three times, before and after the ROM's InitUtil (0x4080C5CC) had found the
   signature missing and written 'NuMc' and its defaults: the chip neither answered nor kept what
   it was told, and the data line simply stayed high. The IIci ROM then never opened the video and
   waited for ever in the File Manager's queue on its open of .Sony at 0x40801138 (PC 0x4080EFDE,
   ioResult never set). The MAME SE with a chip made to read that way (mac.rtcsim=dead below) stops
   at the same PC with the same registers, stack and VIA traffic, and serving the defaults with
   one byte at a time put back to $FF found the one that matters: extended byte $AE, which the ROM
   reads at 0x4081AB44 and, by the look of the code there, uses with its ROM resource table to
   choose a set of ROM resources; it checks the byte with a signed compare (0x4081AB66), so $FF,
   that is -1, passes for a valid choice. So the board layer asks the chip itself before the 68k
   starts (rtc_probe): the signature at $0C-$0F and the four bytes of the clock, read with the
   ROM's own sequence, nothing written. When every one of them reads $FF the chip is taken as absent and this file answers in
   its place: each byte the ROM reads comes from pram[] (the solo board's validated defaults,
   psmac_pram_default.h, with $8A mended as above) and the seconds from a clock that starts at
   20 November 2026 and runs with the Pi's timer; what the ROM writes is kept there for the rest of
   the session (Restart included) and still goes to the chip, which this file never writes itself.
   A chip that answers but holds no signature (a flat battery) is left alone: the ROM resets it to
   its defaults, which is what every Mac does then (the MAME SE with all bytes $FF but a working
   chip boots to the question-mark disk). mac.sepram=chip turns the stand-in off. */
static struct {
    int enb, clk, dir, bits, state, keep, sim_dead, wp;
    int ddr0;                                     /* DDRB bit 0: 1 while the ROM drives the data line (then a read returns its own latch) */
    int addr;                                     /* the byte in flight: 0-255 parameter RAM, 0x100-0x103 the clock's bytes, 0x1ff a register */
    uint8_t byte, cmd, real, given;               /* real: what the chip sent; given: what the ROM got */
    uint32_t mended;                              /* reads of $8A that were changed, for the log */
    int serve, probed, chip_off;                  /* serve: the chip does not answer, this file does; chip_off: mac.sepram=chip */
    int startup;                                  /* a byte of the default startup device ($78-$7B) from a chip that answers: given whole, below */
    int hold_bit;                                 /* -1, or the data bit the chip is sent instead of the ROM's (xp_wire_orb) */
    uint8_t hold_byte;                            /* the chip's own byte at the address being written, while the Mac's view of it is up */
    int hold;                                     /* a write to $78-$7B goes into the view: the chip is sent its own byte back */
    uint8_t pram[256];
    uint32_t secs; uint64_t secs_t0;              /* the stand-in clock: secs at the timer's secs_t0 */
    uint32_t reads, reads_ff, writes;             /* bytes the ROM read (and how many of them the chip sent as $FF), bytes it wrote */
} xp = { .enb = 1, .ddr0 = 1, .hold_bit = -1 };
#define XP_SECS_START 0xe7253e00u                 /* 20 November 2026, 00:00, in the Mac's seconds since 1904 */
static uint8_t xp8a_bits(void) { return psmac_ram_size() > (8u << 20) ? 0x05 : 0; }   /* bits 0 and 2: 32-bit addressing now and at the next start */
static uint8_t xp8a_mend(uint8_t v) { return xp.keep ? v : (uint8_t)((v & ~0x10) | xp8a_bits()); }
static uint32_t xp_secs(void) { return xp.secs + (uint32_t)((asc_now() - xp.secs_t0) / asc_hz()); }
static void xp_set_secs(uint32_t s) { xp.secs = s; xp.secs_t0 = asc_now(); }
/* what the stand-in sends for the byte in flight */
/* psmac_mbox.c: the default startup device, parameter RAM $78-$7B, as the Mac sees it while a disk
   image on the card may start it (docs/08 item 67) */
uint8_t psmac_startup_read(int addr, uint8_t real); int psmac_startup_card(void); void psmac_startup_seen(int addr, uint8_t real);
uint8_t psmac_startup_view(int addr); int psmac_startup_write(int addr, uint8_t v); int psmac_startup_real(int addr, uint8_t *v);
static uint8_t xp_served(int addr)
{
    if (addr >= 0x78 && addr <= 0x7b) return psmac_startup_read(addr, xp.pram[addr]);
    if (addr < 0x100) return addr == 0x8a ? xp8a_mend(xp.pram[0x8a]) : xp.pram[addr];
    if (addr < 0x104) return (uint8_t)(xp_secs() >> (8 * (addr & 3)));
    return 0;                                     /* the test and write-protect registers read 0 (MAME's macrtc) */
}
/* a byte the ROM wrote: the stand-in keeps it (the chip has had it too) */
static void xp_store(int addr, uint8_t v)
{
    xp.writes++;
    if (addr == 0x1fd) { xp.wp = (v & 0x80) != 0; return; }        /* register 13, write protect: always taken */
    if (psmac_startup_write(addr, v)) return;                       /* into the Mac's view of $78-$7B (for the chip, xp_wire_orb kept its own byte) */
    if (!xp.serve || xp.wp) return;
    if (addr < 0x100) xp.pram[addr] = v;
    else if (addr < 0x104) { uint32_t s = xp_secs(), sh = 8 * (addr & 3); xp_set_secs((s & ~(0xffu << sh)) | (uint32_t)v << sh); }
}
static void xp_end(void)
{
    if (xp.dir && xp.bits == 0 && xp.addr >= 0) {
        xp.reads++; if (xp.real == 0xff) xp.reads_ff++;
        if (!xp.serve && xp.addr >= 0x78 && xp.addr <= 0x7b) psmac_startup_seen(xp.addr, xp.real);
        if (xp.addr == 0x8a && !xp.serve && xp.real != xp.given && xp.mended++ < 4) {
            int b4 = (xp.real & 0x10) != 0, b02 = (~xp.real & xp8a_bits()) != 0;
            kprintf("[PSMAC:SE] parameter RAM $8A: the SE's clock chip says $%02x, the ROM is given $%02x:%s%s (mac.pram8a=keep to leave it)\n",
                    xp.real, xp.given, b4 ? " bit 4 cleared (RAM at logical $38000000: with 128 MiB the ROM's MMU table builder overruns its stack, with 8 the boot stalls; docs/08 item 63)" : "",
                    b02 ? (b4 ? "; 32-bit addressing on (more than 8 MiB of RAM)" : " 32-bit addressing on (more than 8 MiB of RAM; 24-bit mode would use 8)") : "");
        }
    }
    xp.addr = -1;
}
/* a read command is complete: the chip sends the byte at addr from the next falling edge */
static void xp_reading(int addr)
{
    xp.dir = 1; xp.bits = 8; xp.addr = addr; xp.real = 0; xp.state = 0;
    xp.given = xp.serve ? xp_served(addr) : 0;
    xp.startup = 0;
    /* The default startup device from a chip that answers: what the Mac is given is decided before
       the first bit, what the chip holds is known after the last (xp_end reports it) */
    if (!xp.serve && addr >= 0x78 && addr <= 0x7b && psmac_startup_card()) { xp.startup = 1; xp.given = psmac_startup_view(addr); }
}
/* the ROM wrote port B (ORB): follow the enable, clock and data lines */
static void xp_lines(uint8_t orb)
{
    int enb = (orb >> 2) & 1, clk = (orb >> 1) & 1, data = orb & 1;
    if (enb != xp.enb) { if (enb) xp_end(); xp.enb = enb; xp.dir = 0; xp.bits = 0; xp.byte = 0; xp.state = 0; xp.addr = -1; xp.hold = 0; xp.hold_bit = -1; }
    if (!clk && xp.clk && !xp.enb) {             /* falling edge: the next bit moves */
        xp.hold_bit = -1;
        if (xp.dir) { if (xp.bits > 0) xp.bits--; }
        else {
            if (xp.state == 3 && xp.hold) xp.hold_bit = (xp.hold_byte >> (7 - xp.bits)) & 1;   /* this bit, until the next falling edge */
            xp.byte = (uint8_t)(xp.byte << 1 | data);
            if (++xp.bits == 8) {
                uint8_t b = xp.byte; int i = (xp.cmd >> 2) & 0x1f; xp.bits = 0; xp.byte = 0;
                if (xp.state == 2) {                 /* the second byte of an extended command: the low address bits */
                    int addr = ((xp.cmd & 7) << 5) | ((b & 0x7c) >> 2);
                    if (xp.cmd & 0x80) xp_reading(addr);
                    else { xp.state = 3; xp.addr = addr; xp.hold = !xp.serve && psmac_startup_card() && psmac_startup_real(addr, &xp.hold_byte); }
                } else if (xp.state == 3) { xp_store(xp.addr, b); xp.state = 0; xp.addr = -1; }
                else if (xp.state == 1) {            /* the data byte of a register write */
                    xp_store(i < 4 ? 0x100 + i : (i >= 8 && i < 12) || i >= 16 ? i : 0x1f0 + i, b);
                    xp.state = 0;
                } else {
                    xp.cmd = b; i = (b >> 2) & 0x1f;
                    if ((b & 0x78) == 0x38) xp.state = 2;
                    else if (b & 0x80) xp_reading(i < 8 ? 0x100 + (i & 3) : (i >= 8 && i < 12) || i >= 16 ? i : 0x1ff);
                    else xp.state = 1;
                }
            }
        }
    }
    xp.clk = clk;
}
/* What goes to the chip when the ROM writes port B: as written, except while the ROM writes a byte
   of $78-$7B that goes into the Mac's view (psmac_startup_write): then the chip is sent its own byte
   back, bit for bit, so it keeps what it held (the ROM's parameter RAM test would otherwise leave the
   slot-9 record in it: psmac_mbox.c). The chip takes a bit on the rising edge that follows the
   falling one this file counts it on, so the substitute holds from one falling edge to the next. */
static uint8_t xp_wire_orb(uint8_t orb)
{
    return xp.hold_bit < 0 ? orb : (uint8_t)((orb & ~1u) | (unsigned)xp.hold_bit);
}
/* the ROM reads port B: while a byte comes out of the chip, bit 0 is the chip's, mended for $8A,
   or the stand-in's */
static uint32_t xp_read(uint32_t r)
{
    if (xp.sim_dead && !xp.ddr0) r |= 1;             /* mac.rtcsim=dead: a chip that never drives the line */
    if (!xp.dir || xp.addr < 0 || xp.ddr0 || xp.bits > 7 || xp.bits < 0) return r;   /* after the last bit the ROM takes the line back first: its read-modify-write of the enable bit is its own */
    int n = xp.bits;                                 /* the bit the chip presents now, 7 first */
    uint8_t real = r & 1, bit = real;
    xp.real = (uint8_t)((xp.real & ~(1u << n)) | (real << n));
    if (xp.serve || xp.startup) bit = (xp.given >> n) & 1;
    else if (xp.addr == 0x8a && !xp.keep) {
        if (n == 4) bit = 0;
        else if ((xp8a_bits() >> n) & 1) bit = 1;
        xp.given = (uint8_t)((xp.given & ~(1u << n)) | (bit << n));
    }
    return (r & ~1u) | bit;
}

/* Before the 68k starts: is there a clock chip that answers? The ROM's sequence, on the service
   core, with nothing written to the chip but the command bytes of eight reads. The VIA is as
   reset left it (port B all inputs); its DDRB is put back afterwards and the ROM programs the
   rest itself. */
#define SE_ORB  (SE_VIA + 0 * 0x200)
#define SE_DDRB (SE_VIA + 2 * 0x200)
static void rtc_send(uint8_t b)
{
    for (int k = 7; k >= 0; k--) {
        uint8_t o = 0x40 | ((b >> k) & 1);           /* enable low, clock low, the bit (PB6 high as via_data drives it) */
        psmac_wire_write(SE_ORB, 1, o, 0);
        psmac_wire_write(SE_ORB, 1, o | 0x02, 0);     /* clock high: the chip takes the bit */
    }
}
static uint8_t rtc_recv(void)
{
    uint8_t v = 0;
    psmac_wire_write(SE_DDRB, 1, 0x46, 0);           /* the data line an input */
    for (int k = 7; k >= 0; k--) {
        psmac_wire_write(SE_ORB, 1, 0x40, 0);
        psmac_wire_write(SE_ORB, 1, 0x42, 0);
        uint32_t r = psmac_wire_read(SE_ORB, 1);
        if (xp.sim_dead) r |= 1;
        v = (uint8_t)(v << 1 | (r & 1));
    }
    psmac_wire_write(SE_DDRB, 1, 0x47, 0);
    psmac_wire_write(SE_ORB, 1, 0x46, 0);             /* enable high: the transaction is over */
    return v;
}
static void rtc_probe(void)
{
    if (xp.probed) return;
    xp.probed = 1;
    uint8_t ddrb = psmac_wire_read(SE_DDRB, 1) & 0xff, sig[4], sec[4];
    psmac_wire_write(SE_ORB, 1, 0x46, 0);             /* enable high, clock high, before the lines become outputs */
    psmac_wire_write(SE_DDRB, 1, 0x47, 0);
    for (int i = 0; i < 4; i++) { int a = 0x0c + i; rtc_send(0xb8 | (a >> 5)); rtc_send((a & 0x1f) << 2); sig[i] = rtc_recv(); }
    for (int i = 0; i < 4; i++) { rtc_send(0x81 | (i << 2)); sec[i] = rtc_recv(); }
    psmac_wire_write(SE_DDRB, 1, ddrb, 0);
    int all_ff = 1;
    for (int i = 0; i < 4; i++) if (sig[i] != 0xff || sec[i] != 0xff) all_ff = 0;
    uint32_t s = (uint32_t)sec[3] << 24 | (uint32_t)sec[2] << 16 | (uint32_t)sec[1] << 8 | sec[0];
    int numc = sig[0] == 'N' && sig[1] == 'u' && sig[2] == 'M' && sig[3] == 'c';
    if (!all_ff) {
        kprintf("[PSMAC:SE] clock chip: answers; parameter RAM %s (signature %02x %02x %02x %02x), clock %08x\n",
                numc ? "valid" : "not valid, the ROM will reset it to its defaults", sig[0], sig[1], sig[2], sig[3], s);
        return;
    }
    if (xp.chip_off) {
        kprintf("[PSMAC:SE] clock chip: reads $FF for the signature and the clock: it does not answer, and mac.sepram=chip leaves it so (the ROM will not start its video)\n");
        return;
    }
    for (int i = 0; i < 256; i++) xp.pram[i] = psmac_pram_default[i];
    xp_set_secs(XP_SECS_START);
    xp.serve = 1;
    kprintf("[PSMAC:SE] clock chip: reads $FF for the signature and the clock: it does not answer (a dead or disconnected chip; look for a leaked battery around it). "
            "The board layer answers in its place: default parameter RAM, kept until the Pi is switched off, and a clock from 20 November 2026 (mac.sepram=chip to leave the chip alone)\n");
}

/* ---- the SCSI chip: the same 5380, read on even addresses, written on odd ones -------------- */
#define SE_SCSI_RD   0x5FF000u
#define SE_SCSI_WR   0x5FF001u
#define SE_SCSI_PDMA 0x5FF200u                  /* the register window again, with DACK: the BBU holds DTACK until the 5380 raises DRQ */
#define SE_SCSI_PDMA_RD (SE_SCSI_PDMA + 0x60)   /* data comes from the input data register (6) in that window: $5FF260 */
#define SE_SCSI_PDMA_WR (SE_SCSI_PDMA + 0x01)   /* and goes to the output data register (0), written on the odd byte: $5FF201 */

/* A SCSI bus with nothing on it (20 November 2026, docs/08 item 70). The user's SE with no internal
   disk and the BlueSCSI taken off, booting from a disk image on the card, sat at the question-mark
   disk: the 68k at PC $408076D8 from five seconds on, 640,000 accesses a second to the 5380, the last
   at $5FF010. That is the SCSI Manager's arbitration ($408076D2-$408076EE): the mode register set to
   ARBITRATE, then a wait for the ICR's "arbitration in progress" bit, which the chip sets once it has
   seen the bus free (BSY and SEL false), with a timeout of TimeSCSIDB x 256 reads; then, at
   $408076F4-$4080770E, "lost arbitration" (ICR bit 5, set when another device asserts SEL) or a
   higher ID on the data lines sends it back to the start, with no timeout at all. On a floating bus
   the 5380 can see the bus free for a moment and SEL asserted the next: the loop never ends, the
   Start Manager's scan never ends, and the card's boot record never runs. With the BlueSCSI attached
   SCSI worked: the BlueSCSI terminates the bus. Without a terminator the lines float, and the 5380
   reads BSY (or SEL) as asserted; MAME's SCSI bus is always clean, so the testbed never showed it.
   So before the 68k starts, after the address lines (psmac_se_start), the board layer looks at the
   bus without touching it: our own 5380's ICR and mode register cleared (it drives nothing then), and
   the Current SCSI Bus Status register (4) and the Bus and Status register (5) read eight times over
   two milliseconds. We are the only initiator and nobody has been selected, so a terminated bus,
   with or without devices, reads BSY and SEL false; when every read has one of them asserted the bus
   is taken to be floating, and the 5380's registers are answered here as an empty, terminated bus
   would answer them (scsi_empty_*, below, the solo board's 5380 with no target): arbitration wins at
   once, the selection of every ID times out (the ROM's 10 ms, rom/patches/iici.json), the pseudo-DMA
   windows read $FF. The scan then ends in about 80 ms and the Start Manager goes on to the floppy and
   the card's boot record. mac.sescsi=auto (the default) lets the probe decide, =real always uses
   the SE's chip, =empty always the empty bus. The probe is made again after each RESET in auto mode
   (a dozen bus cycles), logged only when its answer changes; SCSI devices cannot be plugged in while
   the SE is on in any case. mac.sefloat=1 (testbed) makes the real chip's registers read as on a
   floating bus: every line asserted (registers 0, 4, 5 and 6), and in the ICR arbitration always in
   progress and always lost (bits 6 and 5), which sends the ROM round the retry at $408076FA that has
   no timeout: MAME's bus is clean, and this is the likeliest way a floating one gives the hang. */
static int scsi_mode = 0;                       /* mac.sescsi: 0 auto, 1 real, 2 empty */
static int scsi_empty, scsi_float_sim, scsi_probed;
static struct { uint8_t odr, icr, mr, tcr; int aip, irq; uint32_t selections; } se5380;
static uint32_t scsi_float_fix(uint32_t reg, uint32_t r)
{
    if (!scsi_float_sim) return r;
    if (reg == 0 || reg == 6) return 0xff;         /* every data line asserted */
    if (reg == 4) return r | 0xfe;                 /* RST, BSY, REQ, MSG, C/D, I/O and SEL read asserted */
    if (reg == 5) return r | 0x03;                 /* ATN and ACK too */
    if (reg == 1) return r | 0x60;                 /* the bus seen free a moment ("arbitration in progress"), then lost to a floating SEL */
    return r;
}
static uint32_t scsi_empty_read(uint32_t reg)
{
    switch (reg) {
    case 0: case 6: return (se5380.icr & 0x01) || (se5380.mr & 0x01) ? se5380.odr : 0;   /* the data lines: only what we drive */
    case 1: return (se5380.icr & 0x9f) | (se5380.aip ? 0x40 : 0);                       /* never "lost arbitration" */
    case 2: return se5380.mr;
    case 3: return se5380.tcr;
    case 4: return (se5380.icr & 0x80 ? 0x80 : 0) | (se5380.icr & 0x08 ? 0x40 : 0) | (se5380.icr & 0x04 ? 0x02 : 0);   /* RST, BSY, SEL: ours alone */
    case 5: return (se5380.irq ? 0x10 : 0) | (se5380.icr & 0x02 ? 0x02 : 0) | (se5380.icr & 0x10 ? 0x01 : 0);         /* no REQ, no phase match */
    case 7: se5380.irq = 0; return 0;
    }
    return 0;
}
static void scsi_empty_write(uint32_t reg, uint8_t d)
{
    switch (reg) {
    case 0: se5380.odr = d; break;
    case 1: {
        uint8_t was = se5380.icr;
        se5380.icr = d & 0x9f;
        if ((d & 0x80) && !(was & 0x80)) se5380.irq = 1;                               /* a bus reset interrupts */
        if ((d & 0x04) && !(was & 0x04)) se5380.selections++;
        break; }
    case 2: se5380.mr = d; if (d & 0x01) se5380.aip = 1; break;                        /* the bus is free at once */
    case 3: se5380.tcr = d & 0x0f; break;
    default: break;                                                                     /* the DMA starts: nothing will ever ask for a byte */
    }
}
/* the probe: 1 if the bus floats (BSY or SEL asserted at every read), with the last two readings */
static int scsi_bus_floats(uint8_t *csbs, uint8_t *bas)
{
    psmac_wire_write(SE_SCSI_WR + (2 << 4), 1, 0, 0);    /* mode register: no arbitration, no DMA */
    psmac_wire_write(SE_SCSI_WR + (1 << 4), 1, 0, 0);    /* ICR: assert nothing */
    int floating = 1;
    for (int i = 0; i < 8; i++) {
        uint64_t t0 = asc_now();
        *csbs = (uint8_t)scsi_float_fix(4, psmac_wire_read(SE_SCSI_RD + (4 << 4), 1) & 0xff);
        *bas = (uint8_t)(psmac_wire_read(SE_SCSI_RD + (5 << 4), 1) & 0xff);
        if (!(*csbs & 0x42)) floating = 0;
        while (asc_now() - t0 < asc_hz() / 4000) __asm__ volatile("yield");   /* 250 us apart */
    }
    return floating;
}
static void scsi_probe(void)
{
    if (scsi_mode == 1) { if (!scsi_probed) kprintf("[PSMAC:SE] SCSI: the SE's 5380 as it is (mac.sescsi=real)\n"); scsi_probed = 1; scsi_empty = 0; return; }
    if (scsi_mode == 2) { if (!scsi_probed) kprintf("[PSMAC:SE] SCSI: answered here as an empty bus (mac.sescsi=empty); the SE's 5380 is not used\n"); scsi_probed = 1; scsi_empty = 1; return; }
    uint8_t csbs = 0, bas = 0;
    int floats = scsi_bus_floats(&csbs, &bas);
    if (scsi_probed && floats == scsi_empty) return;
    if (floats)
        kprintf("[PSMAC:SE] SCSI: the SE's SCSI bus has nothing on it, or no terminator: %s asserted with nobody arbitrating (bus status $%02x, bus and status $%02x). "
                "The board layer answers the 5380 as an empty bus: no SCSI disks this session (mac.sescsi=real to use the chip all the same)\n",
                (csbs & 0x42) == 0x42 ? "BSY and SEL read" : csbs & 0x40 ? "BSY reads" : "SEL reads", csbs, bas);
    else
        kprintf("[PSMAC:SE] SCSI: the SE's SCSI bus is terminated and free (bus status $%02x, bus and status $%02x): the SE's 5380 is used%s\n",
                csbs, bas, scsi_probed ? " again" : "");
    scsi_probed = 1; scsi_empty = floats;
    se5380.odr = se5380.icr = se5380.mr = se5380.tcr = 0; se5380.aip = se5380.irq = 0;
}

/* ---- translation ----------------------------------------------------------------------------
   One IIci-side access of the given size, read or written. Returns 1 when answered (locally or
   on the SE's bus), with the read value in *val. The caller handles addresses this file declines. */
int psmac_se_access(uint32_t addr, int size, int write, uint32_t data, uint32_t *val, int posted)
{
    if ((addr & 0xff000000u) != 0x50000000u) {
        /* outside the I/O block: other slots, the IIci's extra spaces. Nothing there on an SE. */
        if (!write) { *val = 0xffffffffu; local_reads++; } else local_writes++;
        return 1;
    }
    if (psmac_board_solo && psmac_solo_access(addr, size, write, data, val)) return 1;   /* the four chips, modelled */
    uint32_t off = addr & 0x000fffffu;          /* the IIci decoder ignores A20-A23 in the I/O block */
    uint32_t r = 0;
    if (off < 0x2000 || (off >= 0x40000 && off < 0x42000)) {
        /* VIA1: register n at +n*0x200 on both machines */
        uint32_t reg = (off >> 9) & 15, se = SE_VIA + (reg << 9);
        if (write) {
            if (reg == 0) { xp_lines(data & 0xff); data = xp_wire_orb(data & 0xff); }
            else if (reg == 2) xp.ddr0 = data & 1;
            else if (reg == 3) via_ddra = (uint8_t)data;
            trace("VIA", 1, addr, se, size, via_data(reg, data & 0xff));
            psmac_wire_write(se, 1, via_data(reg, data & 0xff), posted);
            return 1;
        }
        r = via_read_fix(reg, psmac_wire_read(se, 1) & 0xff);
        if (reg == 0) r = xp_read(r);
        trace("VIA", 0, addr, se, size, r);
        *val = size == 1 ? r : size == 2 ? r << 8 | r : r << 24 | r << 16 | r << 8 | r;
        return 1;
    }
    if (off >= 0x4000 && off < 0x6000) {
        /* SCC: B control, A control, B data, A data at +0, +2, +4, +6 on the IIci; the SE reads
           them at $9FFFF8 (upper byte) and writes them at $BFFFF9 (lower byte) */
        uint32_t ch = off & 6;
        if (write) { trace("SCC", 1, addr, 0xBFFFF9u + ch, size, data & 0xff); psmac_wire_write(0xBFFFF9u + ch, 1, data & 0xff, posted); return 1; }
        r = psmac_wire_read(0x9FFFF8u + ch, 1) & 0xff;
        trace("SCC", 0, addr, 0x9FFFF8u + ch, size, r);
        *val = size == 1 ? r : r << 8 | r;
        return 1;
    }
    if ((off >= 0x10000 && off < 0x12000)) {
        /* the 5380's registers: reg*16 on both */
        uint32_t reg = (off >> 4) & 7;
        { static int first = 1; if (first && write && reg == 1 && (data & 0x04)) { first = 0; psmac_boot_mark(scsi_empty ? "SCSI: first selection (the empty bus answered here)" : "SCSI: first selection on the SE's 5380"); } }
        if (scsi_empty) {
            if (write) { trace("SCSI", 1, addr, SE_SCSI_WR + (reg << 4), size, data & 0xff); scsi_empty_write(reg, data & 0xff); local_writes++; }
            else { r = scsi_empty_read(reg); trace("SCSI", 0, addr, SE_SCSI_RD + (reg << 4), size, r); *val = size == 1 ? r : r << 8 | r; local_reads++; }
            return 1;
        }
        if (write) { trace("SCSI", 1, addr, SE_SCSI_WR + (reg << 4), size, data & 0xff); psmac_wire_write(SE_SCSI_WR + (reg << 4), 1, data & 0xff, posted); return 1; }
        r = scsi_float_fix(reg, psmac_wire_read(SE_SCSI_RD + (reg << 4), 1) & 0xff);
        trace("SCSI", 0, addr, SE_SCSI_RD + (reg << 4), size, r);
        *val = size == 1 ? r : r << 8 | r;
        return 1;
    }
    if ((off >= 0x12000 && off < 0x14000) || (off >= 0x6000 && off < 0x8000)) {
        /* pseudo-DMA, plain (0x12000) or with the DRQ handshake (0x6000): the SE has one window
           and its glue does the waiting. A byte per bus cycle; longs and words are several. */
        if (scsi_empty) { if (!write) *val = size == 1 ? 0xffu : size == 2 ? 0xffffu : 0xffffffffu; return 1; }
        if (write) { trace("SCSI", 1, addr, SE_SCSI_PDMA_WR, size, data); for (int i = 0; i < size; i++) psmac_wire_write(SE_SCSI_PDMA_WR, 1, (data >> (8 * (size - 1 - i))) & 0xff, 0); return 1; }
        { static int first = 1; if (first) { first = 0; psmac_boot_mark("SCSI: first pseudo-DMA read: the first block from the SE's disk"); } }
        r = 0;
        for (int i = 0; i < size; i++) r = r << 8 | (psmac_wire_read(SE_SCSI_PDMA_RD, 1) & 0xff);
        trace("SCSI", 0, addr, SE_SCSI_PDMA_RD, size, r);
        *val = r;
        return 1;
    }
    if (off >= 0x16000 && off < 0x18000) {
        /* SWIM on the IIci, IWM on the SE: register n at +n*0x200, odd addresses on the SE */
        uint32_t reg = (off >> 9) & 15, se = 0xDFE1FFu + (reg << 9);
        if (write) { trace("IWM", 1, addr, se, size, data & 0xff); psmac_wire_write(se, 1, data & 0xff, posted); return 1; }
        r = psmac_wire_read(se, 1) & 0xff;
        trace("IWM", 0, addr, se, size, r);
        *val = size == 1 ? r : r << 8 | r;
        return 1;
    }
    /* chips the SE lacks: answered here */
    if (off >= 0x26000 && off < 0x28000) {
        if (write) { trace("RBV", 1, addr, off, size, data & 0xff); rbv_write(off, data & 0xff); }
        else { r = rbv_read(off); trace("RBV", 0, addr, off, size, r); *val = size == 1 ? r : r << 8 | r; }
        if (psmac_board_solo) psmac_solo_level_now();   /* the level-2 line follows its flags at once (psmac_board_solo.c) */
        return 1;
    }
    if (off >= 0x14000 && off < 0x16000) { if (write) asc_write(off, data & 0xff); else { r = asc_read(off); *val = size == 1 ? r : r << 8 | r; } return 1; }
    if (off >= 0x24000 && off < 0x26000) { if (write) vdac_regs[off & 31] = data; else *val = vdac_regs[off & 31]; return 1; }
    /* VIA2 (0x2000: the ROM probes it to tell decoders apart; the IIci has none), 0x21C00, the rest: open bus */
    if (!write) { *val = 0; local_reads++; } else local_writes++;
    return 1;
}

/* The interrupt level the SE drives, read as the IIci ROM expects it: the SE's level 1 is the VIA
   on both; its level 2 is the SCC (the IIci's level 4; the 5380's interrupt is kept off); the
   IIci's level 2, the RBV, is this file's own. */
unsigned psmac_se_ipl(unsigned raw)
{
    if (psmac_board_solo) return psmac_solo_ipl();
    unsigned ipl = 0;
    if (raw & 2) ipl = 4;
    else if (raw & 1) ipl = 1;
    if (psmac_se_rbv_irq() && ipl < 2) ipl = 2;
    return ipl;
}

/* ---- the SE's screen -------------------------------------------------------------------------
   The slot-9 card's frame (psmac_display.c) is 512 by 342 here, one bit a pixel, 64 bytes a row,
   exactly the SE's own layout. Core 3 marks the rows that changed since the last copy; the
   service core, which owns the wire, writes them into the SE's screen buffer between the Mac's
   own bus cycles, 32 words a row. A full screen is 10,944 words; at the testbed's bridge pace
   that is seconds, on the GPIO about ten milliseconds, so only what changed goes. */
#define SE_ROWS 342
#define SE_ROWBYTES 64
static uint8_t shadow[SE_ROWS * SE_ROWBYTES];
static volatile uint32_t dirty[(SE_ROWS + 31) / 32];
static volatile int shadow_valid;
static uint32_t rows_sent, frames_marked, se_lines_bad;   /* se_lines_bad: address lines that failed the test at the start (bit n = An) */

/* core 3: compare the card's frame with what the SE has and mark the rows that differ */
void psmac_se_mark(const uint8_t *frame, uint32_t width, uint32_t height, uint32_t rowbytes, uint32_t depth)
{
    if (psmac_board_solo) return;
    if (!psmac_board_se || depth != 1 || width != 512 || height < SE_ROWS || rowbytes != SE_ROWBYTES) return;
    for (uint32_t y = 0; y < SE_ROWS; y++) {
        const uint64_t *a = (const uint64_t *)(frame + y * SE_ROWBYTES), *b = (const uint64_t *)(shadow + y * SE_ROWBYTES);
        int differs = !shadow_valid;
        for (int k = 0; k < 8 && !differs; k++) differs = a[k] != b[k];
        if (differs) { __builtin_memcpy(shadow + y * SE_ROWBYTES, frame + y * SE_ROWBYTES, SE_ROWBYTES); __atomic_fetch_or(&dirty[y >> 5], 1u << (y & 31), __ATOMIC_RELEASE); }
    }
    shadow_valid = 1; frames_marked++;
}

/* The first time rows 0 and 100 are sent ($3FA700 and $3FC000: the two sides of the boundary the
   hardware's stripes stopped at, docs/08 item 68), they are read back at once and the log says
   whether the SE's memory holds what was written there. */
static void se_first_rows_check(uint32_t y)
{
    static int checked[2];
    int k = y == 100;
    if (checked[k]) return;
    checked[k] = 1;
    const uint16_t *want = (const uint16_t *)(shadow + y * SE_ROWBYTES);
    uint32_t base = psmac_se_fb_base() + y * SE_ROWBYTES, bad = 0, first = 0, got = 0;
    for (int i = 0; i < SE_ROWBYTES / 2; i++) {
        uint32_t r = psmac_wire_read(base + 2 * i, 2) & 0xffff;
        if (r != want[i] && !bad++) { first = i; got = r; }
    }
    if (!bad) kprintf("[PSMAC:SE] first send of screen row %u: its 32 words at $%06x read back as written\n", y, base);
    else kprintf("[PSMAC:SE] first send of screen row %u: %u of its 32 words at $%06x read back differently (the first at $%06x: wrote $%04x, read $%04x)\n",
                 y, bad, base, base + 2 * first, want[first], got);
}

/* the service core: a few rows per call, so the Mac's own accesses are never far behind */
void psmac_se_service(void)
{
    if (psmac_board_solo) { psmac_solo_service(); return; }
    if (!psmac_board_se) return;
    for (int n = 0; n < 4; n++) {
        int y = -1;
        for (uint32_t w = 0; w < (SE_ROWS + 31) / 32 && y < 0; w++) {
            uint32_t bits = dirty[w];
            if (bits) { int b = __builtin_ctz(bits); __atomic_fetch_and(&dirty[w], ~(1u << b), __ATOMIC_ACQUIRE); y = w * 32 + b; }
        }
        if (y < 0) return;
        const uint16_t *src = (const uint16_t *)(shadow + y * SE_ROWBYTES);
        uint32_t dst = psmac_se_fb_base() + y * SE_ROWBYTES;
        for (int i = 0; i < SE_ROWBYTES / 2; i++) psmac_wire_write(dst + 2 * i, 2, src[i], 1);
        rows_sent++;
        if (y == 0 || y == 100) se_first_rows_check(y);
    }
}

/* The address lines between the Pi and the SE's memory. The second and third boots on a real SE
   (20 November 2026) showed the top hundred rows of the SE's screen as stripes of old memory that
   never changed, whatever the Mac drew there (the Finder's menu bar never appeared), and the rest
   of the screen right. Row 100 starts at $3FC000: the rows above it have A14 = 0 and A15 = 1, every
   row from there down A14 = A15 = 1, and so does every address of the SE's chips the kernel uses
   (VIA $EFE1FE, SCC $9FFFF8 and $BFFFF9, SCSI $5FF000-$5FF260, IWM $DFE1FF): a sliver of solder
   joined A14 and A15 at the PiStorm's address latch (U2, pins 22 and 23), so those two lines could
   only agree, and nothing but those screen rows depended on their disagreeing (docs/08 item 68).
   A read-back cannot see such a fault, since reads go astray exactly as the writes did. So before
   anything is drawn the board layer tests the lines itself, in the SE's RAM (its own hardware sees
   only the screen and the sound buffer there; the rest is the kernel's to use):
   - each line alone: a word at a base address and another at the base with that line flipped,
     both read back: a line stuck at 0 or 1, or not connected, makes the two the same memory;
   - each pair of lines: the four addresses with the two lines at 00, 01, 10 and 11, each written a
     word of its own and read back; those that read the same are the same memory. Two lines joined
     so that a 0 (or a 1) on either wins make three of the four one memory, which no stuck line
     does (a stuck line makes two pairs).
   One kind of short looks exactly like a stuck line from here: a line that simply follows another
   (A14 driven by A15, as on the user's board) leaves the memory in the same two halves as A14 stuck
   would, and only the SE's video, which reads the RAM with its own addresses, sees the difference.
   The first test names that line, and its message says a short is one of the possibilities.
   About 1,800 bus cycles: a few milliseconds on the GPIO, a fraction of a second in the testbed.
   The words are cleared again afterwards. */
static int se_dip64_pin(int line) { return line <= 20 ? 28 + line : 29 + line; }   /* 68000 DIP-64: A1-A20 pins 29-48, VCC 49, A21-A23 pins 50-52 */
static uint16_t se_rd(uint32_t a) { return psmac_wire_read(a, 2) & 0xffff; }
static void se_wr(uint32_t a, uint16_t v) { psmac_wire_write(a, 2, v, 0); }
/* one line alone: 1 if it works */
static int se_line_ok(uint32_t base, int n, uint16_t *ra, uint16_t *rb)
{
    uint32_t alt = base ^ (1u << n);
    se_wr(base, (uint16_t)(0x5a00 | n)); se_wr(alt, (uint16_t)(0xa500 | n));
    *ra = se_rd(base); *rb = se_rd(alt);
    se_wr(alt, 0);
    return *ra == (0x5a00 | n) && *rb == (0xa500 | n);
}
/* two lines: write the four combinations, read them back, and group the ones that read the same (the
   same memory: the last word written there). 1 if three of them are one memory, the mark of two
   lines joined so that a 0 (or a 1) on either wins */
static int se_pair_joined(uint32_t base, int n, int m)
{
    uint32_t a[4]; uint16_t r[4];
    for (int c = 0; c < 4; c++) { a[c] = base ^ (c & 1 ? 1u << n : 0) ^ (c & 2 ? 1u << m : 0); se_wr(a[c], (uint16_t)(0x3300 | c << 6 | n)); }
    for (int c = 0; c < 4; c++) r[c] = se_rd(a[c]);
    for (int c = 0; c < 4; c++) se_wr(a[c], 0);
    for (int c = 0; c < 4; c++) {
        int same = 0;
        for (int d = 0; d < 4; d++) same += r[d] == r[c];
        if (same == 3) return 1;
    }
    return 0;
}
static void se_ram_lines(void)
{
    uint32_t base = psmac_se_fb_base(), size = se_ram_size(), stuck = 0, joined = 0;
    int top = 0;
    for (int n = 1; n <= 21 && (base ^ (1u << n)) < size; n++) top = n;
    for (int n = 1; n <= top; n++)
        for (int m = n + 1; m <= top; m++) {
            if ((base ^ (1u << n) ^ (1u << m)) >= size || !se_pair_joined(base, n, m)) continue;
            joined |= 1u << n | 1u << m;
            uint32_t z = base & ~(1u << n) & ~(1u << m);   /* both lines 0 */
            kprintf("[PSMAC:SE] address lines A%d and A%d are joined: of $%06x, $%06x, $%06x and $%06x (the two lines at 00, 01, 10, 11) three are the same memory. "
                    "Look for a solder bridge or a short between them: the 68000 socket's pins %d and %d, and the address latch's outputs for them\n",
                    n, m, z, z | 1u << m, z | 1u << n, z | 1u << n | 1u << m, se_dip64_pin(n), se_dip64_pin(m));
        }
    for (int n = 1; n <= top; n++) {
        uint16_t ra, rb;
        if (se_line_ok(base, n, &ra, &rb)) continue;
        stuck |= 1u << n;
        if (joined >> n & 1) continue;               /* named above */
        kprintf("[PSMAC:SE] address line A%d does not reach the SE as both 0 and 1: written $%04x at $%06x and $%04x at $%06x (A%d flipped), read back $%04x and $%04x%s. "
                "The line is stuck, not connected, or joined to another line that overrides it (from the Pi's side the three look the same; the user's PiStorm had A14 bridged to A15). "
                "Check the PiStorm's A%d: the 68000 socket's pin %d, the address latch output that drives it and its neighbours\n",
                n, 0x5a00 | n, base, 0xa500 | n, base ^ (1u << n), n, ra, rb, ra == rb ? ": the same memory" : "", n, se_dip64_pin(n));
    }
    se_wr(base, 0);
    se_lines_bad = stuck | joined;
    if (!se_lines_bad)
        kprintf("[PSMAC:SE] address lines A1-A%d good: each line read back as 0 and as 1 (a word at $%06x and one at each address one line away), and every pair of them "
                "kept four words apart at the four combinations of the two lines. Measured before the first screen row is sent\n", top, base);
}

/* Once, as the service core starts and before the 68k's first instruction: the SE powers up with
   its ROM overlaid at 0 and RAM at $600000; one access to the ROM's own space ($400000) puts RAM
   back at 0, where the screen buffer must be. Then the address lines are tested (above), the
   shadow forgotten, and the clock chip is asked whether it answers (rtc_probe, above). Core 0
   waits for all of it (psmac_se_wait_ready), since the ROM's first act, a RESET, would reset the
   VIA under the probe. Each RESET the 68k issues afterwards (the ROM's first, every Restart)
   pulses the SE's reset line, which brings the overlay back: psmac_se_after_reset clears it again.
   (Until 20 November 2026 the comment here said this ran "after every reset the ROM pulses"; it
   never did, and nothing put RAM back at 0 after the ROM's RESET. The screen showed all the same,
   so the overlay evidently does not hide the kernel's writes to the screen buffer.) */
static volatile int se_ready;
void psmac_se_start(void)
{
    if (!psmac_board_se || psmac_board_solo) { se_ready = 1; return; }
    psmac_wire_read(0x400000u, 2);
    se_ram_lines();
    shadow_valid = 0;
    for (uint32_t w = 0; w < (SE_ROWS + 31) / 32; w++) dirty[w] = 0;
    kprintf("[PSMAC:SE] board layer up: SE memory %u MB, screen buffer at $%06x, VIA $EFE1FE, SCC $9FFFF8/$BFFFF9, SCSI $5FF000, IWM $DFE1FF; RBV, ASC, VDAC answered here\n",
            se_ram_mb, psmac_se_fb_base());
    scsi_probe();
    rtc_probe();
    __asm__ volatile("dmb ish" ::: "memory");
    se_ready = 1;
}
/* right after a RESET instruction has pulsed the SE's reset line: its VIA's ports are inputs again
   (via_ddra, for port A bit 4), and one access to $400000 puts RAM back at 0 */
void psmac_se_after_reset(void)
{
    if (!psmac_board_se || psmac_board_solo) return;
    via_ddra = 0;
    psmac_wire_read(0x400000u, 2);
    if (scsi_mode == 0) scsi_probe();             /* the reset line has reset the 5380 too: a dozen cycles to look again */
    se5380.odr = se5380.icr = se5380.mr = se5380.tcr = 0; se5380.aip = se5380.irq = 0;
}
/* core 0, just before it releases the 68k: at most a second, so a service core that never comes
   up cannot hold the boot for ever (its absence is logged elsewhere) */
void psmac_se_wait_ready(void)
{
    if (!psmac_board_se || psmac_board_solo) return;
    uint64_t t0 = asc_now(), hz = asc_hz();
    while (!se_ready && asc_now() - t0 < hz) __asm__ volatile("yield");
}

void psmac_se_options(const char *args)
{
    extern const char *find_token(const char *, const char *);
    const char *tok;
    if ((tok = find_token(args, "mac.board="))) { tok += 10; if (tok[0] == 's' && tok[1] == 'e') psmac_board_se = 1; }
    psmac_solo_options(args);
    if ((tok = find_token(args, "mac.setrace="))) {
        tok += 12; uint32_t n = 0; while (*tok >= '0' && *tok <= '9') n = n * 10 + (*tok++ - '0'); se_trace_left = n;
        if (*tok == ',') { tok++; for (int i = 0; i < 4 && *tok && *tok != ' '; i++, tok++) se_trace_dev[i] = (*tok >= 'a' && *tok <= 'z') ? *tok - 32 : *tok; }
    }
    if ((tok = find_token(args, "mac.pram8a="))) { tok += 11; if (tok[0] == 'k') xp.keep = 1; }
    if ((tok = find_token(args, "mac.sepram="))) { tok += 11; if (tok[0] == 'c') xp.chip_off = 1; }
    if ((tok = find_token(args, "mac.sepa4="))) { tok += 10; se_pa4_sim = tok[0] == '1'; }   /* the testbed: port A bit 4 read as 1 while an input (the IIci with parity) */
    if ((tok = find_token(args, "mac.rtcsim="))) { tok += 11; if (tok[0] == 'd') xp.sim_dead = 1; }   /* the testbed: the clock chip read as one that never answers */
    if ((tok = find_token(args, "mac.selinefault="))) {   /* the testbed: N (stuck high), N,M (N follows M) or N&M (joined, 0 wins) on the wire to the SE's RAM */
        tok += 16; int n = 0, m = 0; while (*tok >= '0' && *tok <= '9') n = n * 10 + (*tok++ - '0');
        if (*tok == ',' || *tok == '&') { se_fault_and = *tok == '&'; tok++; while (*tok >= '0' && *tok <= '9') m = m * 10 + (*tok++ - '0'); }
        if (n >= 1 && n <= 21 && m >= 1 && m <= 21 && m != n) { se_fault_n = n; se_fault_m = m; }
        else if (n >= 1 && n <= 21) se_fault_or = 1u << n;
    }
    if ((tok = find_token(args, "mac.sescsi="))) { tok += 11; scsi_mode = tok[0] == 'r' ? 1 : tok[0] == 'e' ? 2 : 0; }
    if ((tok = find_token(args, "mac.sefloat="))) { tok += 12; scsi_float_sim = tok[0] == '1'; }   /* the testbed: the 5380 read as on an unterminated bus */
    if ((tok = find_token(args, "mac.seram="))) { tok += 10; uint32_t n = 0; while (*tok >= '0' && *tok <= '9') n = n * 10 + (*tok++ - '0'); if (n >= 1 && n <= 4) se_ram_mb = n; }
}

void psmac_se_report(void)
{
    if (psmac_board_solo) { psmac_solo_report(); return; }
    if (!psmac_board_se) return;
    kprintf("[PSMAC:SE] rows sent %u%s, frames compared %u, local reads %u writes %u, RBV IFR %02x IER %02x SIER %02x | clock chip: %s, %u bytes read (%u of them $FF from the chip), %u written\n",
            rows_sent, se_lines_bad ? " (an address line failed its test at the start)" : "", frames_marked, local_reads, local_writes, rbv_ifr, rbv_ier, rbv_regs[0x12],
            xp.serve ? "not answering, the board layer answers" : "answering", xp.reads, xp.reads_ff, xp.writes);
    if (scsi_empty) kprintf("[PSMAC:SE]   SCSI: the empty bus answered here, %u selections (none answered)\n", se5380.selections);
}
