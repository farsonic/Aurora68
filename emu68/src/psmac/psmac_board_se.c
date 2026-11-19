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

int psmac_board_se;                         /* mac.board=se (also set by mac.board=solo, psmac_board_solo.c) */
extern int psmac_board_solo;
int psmac_solo_access(uint32_t addr, int size, int write, uint32_t data, uint32_t *val);
unsigned psmac_solo_ipl(void); void psmac_solo_level_now(void); void psmac_solo_service(void); void psmac_solo_options(const char *); void psmac_solo_report(void);
static uint32_t se_ram_mb = 4;              /* mac.seram=N: the SE's own memory, 1, 2, 2.5 or 4 MB (its screen sits at the top) */
static uint32_t se_ram_size(void) { return se_ram_mb << 20; }
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
static uint32_t via_read_fix(uint32_t reg, uint32_t r)
{
    if (reg == 1 || reg == 15) return (r & 0xb8) | 0x47;
    if (reg == 0) return r | 0x40;                   /* PB6: an input tied high on the IIci; the SE's video beam / SCSI interrupt enable */
    return r;
}

/* ---- the SCSI chip: the same 5380, read on even addresses, written on odd ones -------------- */
#define SE_SCSI_RD   0x5FF000u
#define SE_SCSI_WR   0x5FF001u
#define SE_SCSI_PDMA 0x5FF200u                  /* the register window again, with DACK: the BBU holds DTACK until the 5380 raises DRQ */
#define SE_SCSI_PDMA_RD (SE_SCSI_PDMA + 0x60)   /* data comes from the input data register (6) in that window: $5FF260 */
#define SE_SCSI_PDMA_WR (SE_SCSI_PDMA + 0x01)   /* and goes to the output data register (0), written on the odd byte: $5FF201 */

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
        if (write) { trace("VIA", 1, addr, se, size, via_data(reg, data & 0xff)); psmac_wire_write(se, 1, via_data(reg, data & 0xff), posted); return 1; }
        r = via_read_fix(reg, psmac_wire_read(se, 1) & 0xff);
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
        { static int first = 1; if (first && write && reg == 1 && (data & 0x04)) { first = 0; psmac_boot_mark("SCSI: first selection on the SE's 5380"); } }
        if (write) { trace("SCSI", 1, addr, SE_SCSI_WR + (reg << 4), size, data & 0xff); psmac_wire_write(SE_SCSI_WR + (reg << 4), 1, data & 0xff, posted); return 1; }
        r = psmac_wire_read(SE_SCSI_RD + (reg << 4), 1) & 0xff;
        trace("SCSI", 0, addr, SE_SCSI_RD + (reg << 4), size, r);
        *val = size == 1 ? r : r << 8 | r;
        return 1;
    }
    if ((off >= 0x12000 && off < 0x14000) || (off >= 0x6000 && off < 0x8000)) {
        /* pseudo-DMA, plain (0x12000) or with the DRQ handshake (0x6000): the SE has one window
           and its glue does the waiting. A byte per bus cycle; longs and words are several. */
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
static uint32_t rows_sent, frames_marked;

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
    }
}

/* at the start, and after every reset the ROM pulses: the SE powers up with its ROM overlaid at
   0 and RAM at $600000; one access to the ROM's own space ($400000) puts RAM back at 0, where the
   screen buffer must be. Then the SE's screen is cleared and the shadow forgotten. */
void psmac_se_start(void)
{
    if (!psmac_board_se || psmac_board_solo) return;
    psmac_wire_read(0x400000u, 2);
    shadow_valid = 0;
    for (uint32_t w = 0; w < (SE_ROWS + 31) / 32; w++) dirty[w] = 0;
    kprintf("[PSMAC:SE] board layer up: SE memory %u MB, screen buffer at $%06x, VIA $EFE1FE, SCC $9FFFF8/$BFFFF9, SCSI $5FF000, IWM $DFE1FF; RBV, ASC, VDAC answered here\n",
            se_ram_mb, psmac_se_fb_base());
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
    if ((tok = find_token(args, "mac.seram="))) { tok += 10; uint32_t n = 0; while (*tok >= '0' && *tok <= '9') n = n * 10 + (*tok++ - '0'); if (n >= 1 && n <= 4) se_ram_mb = n; }
}

void psmac_se_report(void)
{
    if (psmac_board_solo) { psmac_solo_report(); return; }
    if (!psmac_board_se) return;
    kprintf("[PSMAC:SE] rows sent %u, frames compared %u, local reads %u writes %u, RBV IFR %02x IER %02x SIER %02x\n",
            rows_sent, frames_marked, local_reads, local_writes, rbv_ifr, rbv_ier, rbv_regs[0x12]);
}
