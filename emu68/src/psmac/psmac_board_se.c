/*
 * psmac_board_se.c — the Macintosh SE's logic board behind the Macintosh IIci ROM.
 * Copyright (c) 2026 Aurora68 project. MPL-2.0.
 *
 * The ROM on the Pi is a IIci's and talks to a IIci: a VIA at 0x50F00000, an SCC at 0x50F04000,
 * an NCR 5380 at 0x50F10000 with its pseudo-DMA windows, a SWIM, an ASC, a VDAC, the RBV with
 * the slot interrupts and the monitor sense. The board in the socket is a Macintosh SE's: the
 * same VIA at $EFE1FE, the same SCC at $9FFFF8 (reads) and $BFFFF9 (writes), the same 5380 at
 * $5FF000 with a pseudo-DMA window at $5FF200, an IWM at $DFE1FF, 4 MB of RAM at 0 whose top
 * holds the 512 by 342 screen the BBU scans out, and nothing else (docs/01, docs/03). This file
 * is the translation: the first four chips are the real ones behind an address change and, for
 * the VIA, two port bits whose meaning differs; the RBV, ASC and VDAC are answered here; the
 * SE's screen is fed from the slot-9 card's frame buffer, row by row, as it changes, and the SE's
 * speaker from the ASC's samples, through the SE's sound buffer (20 November 2026).
 *
 * Selected with mac.board=se (the testbed's IIci motherboard is the default); exercised against
 * MAME's macse with the bridge CPU (reference/mame-psmac.patch: macse_ps) before the hardware.
 * Written 17 November 2026, 04:00 (docs/12).
 *
 * The 68000 bus is 16 bits wide. A long read of the SCSI pseudo-DMA window, which the IIci ROM's
 * driver does to move four bytes at once, becomes four byte reads here; the wire (GPIO or
 * bridge) splits ordinary longs itself.
 */
#include <stdbool.h>
#include "globals.h"
#include <stdint.h>
#include "support.h"
#include "M68k.h"
#include "psmac.h"
#include "psmb.h"
#include "psmac_pram_default.h"

int psmac_board_se;                         /* mac.board=se (also set by mac.board=solo, psmac_board_solo.c, and mac.board=plus) */
extern int psmac_board_solo;
/* mac.board=plus (psmac_board_plus.c): the Plus's differences, hooked in below where they apply */
extern int psmac_board_plus;
extern int psmac_board_orig;                /* psmac_board_plus.c: 0 the Plus, 1 the 128K, 2 the 512K, 3 the 512Ke (mac.board=128k/512k/512ke) */
int psmac_plus_via_write(uint32_t reg, uint32_t *data); int psmac_plus_via_local(uint32_t reg, uint32_t *val);
uint32_t psmac_plus_via_read(uint32_t reg, uint32_t r); void psmac_plus_tick(void); int psmac_plus_irq(void);
void psmac_plus_reset(void); void psmac_plus_options(const char *); void psmac_plus_report(void);
uint32_t psmac_plus_scc_write(uint32_t ch, uint32_t data); void psmac_plus_scc_written(void); void psmac_plus_scc_read(uint32_t ch);
#define PB6 (psmac_board_plus ? 0u : 0x40u)   /* VIA port B bit 6: driven high on the SE (its SCSI interrupt enable), an input on the Plus (H4) */
int psmac_solo_access(uint32_t addr, int size, int write, uint32_t data, uint32_t *val);
unsigned psmac_se_ipl(unsigned raw);
static inline void level_lock(void), level_unlock(void); static void store_level(unsigned raw);   /* the level the JIT sees (psmac_se_set_level, below) */
static unsigned se_wire_ipl;                /* the SE's level as the last cycle on the wire brought it */
unsigned psmac_solo_ipl(void); void psmac_solo_level_now(void); void psmac_solo_service(void); void psmac_solo_options(const char *); void psmac_solo_report(void);
static uint32_t se_ram_kb = 4096;           /* mac.seram=N: the SE's own memory, 1, 2, 2.5 or 4 MB (its screen and sound buffers sit at the top); 128 or 512 KB on mac.board=128k/512k/512ke */
static uint32_t se_ram_size(void) { return se_ram_kb << 10; }
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
void psmac_serve_waiting(void);                  /* the 68k's request, if one is waiting: between the cycles of a long errand */

/* ---- the chips the SE does not have: a pseudo-VIA (RBV), an ASC, a VDAC ------------------- */
static uint8_t rbv_regs[256], rbv_ifr, rbv_ier;   /* MAME's rbv.cpp is the model: IFR/IER with the bit-7 set/clear convention */
static volatile uint8_t asc_regs[0x1000];       /* written by core 0, read by the speaker on the service core as well */
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
   Speedometer's tests, Lemmings), 18 November. Bit 7 of $803 empties both.

   Since 20 November the bytes are also kept, for the SE's own speaker (below, "the SE's speaker"):
   each channel's FIFO bytes go into a ring of their own, which the service core empties into the
   SE's sound buffer as the SE plays it. The level above still paces the Sound Manager by the
   Pi's timer, as before, so whatever the speaker does the feeder never waits on it. */
static uint32_t asc_level[2];
static uint64_t asc_t[2];
/* Since 20 November 2026 the bytes also go to the Pi's own sound output (psmac_audio.c, HDMI or the
   jack, on core 1), which reads the chip's registers for the wavetable voices itself; the speaker's
   rings below are the SE's alone. */
void psmac_audio_asc_byte(int ch, uint8_t v); void psmac_audio_asc_flush(void);
int psmac_audio_spk_level(void); uint8_t psmac_audio_spk_take(void); int psmac_audio_wanted(void);
#define ASC_RATE 22257u
static inline uint64_t asc_now(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v)); return v; }
static inline uint64_t asc_hz(void)  { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v; }
/* the FIFO bytes on their way to the speaker: core 0 (the Mac's writes) moves the heads, the
   service core (the speaker) the tails. 4096 bytes a channel is 184 ms, four times what the chip
   holds, so the rings only overflow when the speaker has stopped taking them */
#define SND_RING 4096u
static uint8_t snd_ring[2][SND_RING];
static uint32_t snd_head[2], snd_tail[2];
static volatile uint32_t snd_flush;             /* bumped when the Mac empties the FIFOs or changes mode: the speaker drops what it has */
static volatile uint32_t snd_wt_gen;            /* bumped when the Mac writes a wavetable phase: the speaker takes the new phases */
static uint32_t snd_dropped;                    /* bytes that found a ring full */
static int snd_off;                             /* mac.sesound=off: the ASC's bytes are thrown away, as before */
const volatile uint8_t *psmac_asc_regs(void) { return asc_regs; }   /* for psmac_audio.c's wavetable voices */
uint32_t psmac_asc_wt_gen(void) { return snd_wt_gen; }
static void snd_push(int ch, uint8_t v)
{
    if (snd_off) return;
    uint32_t h = snd_head[ch];
    if (h - __atomic_load_n(&snd_tail[ch], __ATOMIC_ACQUIRE) >= SND_RING) { snd_dropped++; return; }
    snd_ring[ch][h % SND_RING] = v;
    __atomic_store_n(&snd_head[ch], h + 1, __ATOMIC_RELEASE);
}
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
    uint8_t mode = asc_regs[0x801];
    asc_regs[off] = data;
    if (off < 0x800 && mode == 1) {
        int ch = off >= 0x400;
        asc_drain(ch);
        if (asc_level[ch] < 1024) { asc_level[ch]++; snd_push(ch, data); psmac_audio_asc_byte(ch, data); }   /* what the chip would have taken */
    } else if (off == 0x803 && (data & 0x80)) {
        asc_level[0] = asc_level[1] = 0;
        snd_flush++; psmac_audio_asc_flush();
    } else if (off == 0x801 && data != mode) {
        snd_flush++; psmac_audio_asc_flush();       /* a new mode starts with empty FIFOs (MAME's asc.cpp) */
    } else if (off >= 0x810 && off < 0x830) {
        snd_wt_gen++;
    }
}

/* ---- the VIA: the same chip, two bits apart --------------------------------------------------
   Port A on the IIci: 7 SCC wait/request, 6 (unused), 5 floppy head select, 4 overlay, 3 sync
   modem, 2-0 unused. On the SE: 7 SCC wait/request, 6 screen buffer (1 = main), 5 head select,
   4 floppy drive select (0 = internal), 3 unused, 2-0 speaker volume. Bit 6 is forced to 1 so the
   SE scans out its main buffer whatever the ROM wrote. Bit 4 is forced to an output at 0, the
   SE's internal drive: as its floppy driver starts, the ROM writes DDRA with bit 4 clear ($6F
   reached the wire), and a pin the 6522 does not drive is left to the logic it feeds, a TTL input
   that floats high (MAME's 6522 presents it as 1); high selects the SE's second internal drive
   (vDriveSel), absent from nearly every SE, and the internal floppy was never seen (docs/08 item
   93). The IIci ROM knows nothing of a second internal drive, so it is not reachable; the
   external drive is, by the IWM's own SELECT line. Port B is the same on both (RTC on 0-2,
   ADB interrupt 3, ADB state 4-5, sound enable 7) except bit 6: the SE's SCSI interrupt enable,
   active low; it is forced to 1, so the 5380's interrupt never reaches the 68000's level 2, which
   the IIci ROM would take for its RBV. The data-direction registers get the same bits set so
   the forced values are driven. */
#define SE_VIA   0xEFE1FEu
static uint32_t via_data(uint32_t reg, uint32_t data)
{
    switch (reg) {
    case 0: case 2: return data | PB6;                 /* ORB / DDRB (not on the Plus: H4 is an input there) */
    case 1: case 15:                                   /* ORA / ORA without handshake: the main screen; on the SE its internal drive */
        return psmac_board_plus ? data | 0x40 : (data | 0x40) & ~0x10u;   /* (the Plus's and the 128K's PA4 is the overlay: the ROM's own) */
    case 3: return data | (psmac_board_plus ? 0x40 : 0x50);   /* DDRA: those driven */
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
/* Four more bits are the speaker's (20 November 2026, "the SE's speaker" below): on the SE port A bits
   0-2 are the volume and bit 3 the sound buffer (1 = main), port B bit 7 the sound enable (0 = on).
   The service core drives them as outputs on the wire (psmac_se_wire_write); the ROM, which has its
   own uses for those bits on the IIci, reads back what it wrote itself (port A bit 3, port B bit 7
   and the four data-direction bits), so it never sees the speaker's values. */
static uint8_t via_ddra, via_ddrb, via_ora, via_orb, via_ier, se_pa4_sim;   /* the ROM's own values (core 0) */
static uint32_t via_read_fix(uint32_t reg, uint32_t r)
{
    if (reg == 1 || reg == 15) {
        r = (r & 0xb8) | 0x47;
        if (!(via_ddra & 0x10)) r = (r & ~0x10u) | (se_pa4_sim ? 0x10u : 0);
        else if (psmac_board_plus) r = (r & ~0x10u) | (via_ora & 0x10);   /* the Plus's overlay: the ROM's latch, the wire's is always 0 */
        if (!snd_off) r = (r & ~0x08u) | (via_ddra & via_ora & 0x08);   /* PA3: the ROM's latch as an output, 0 as an input (MAME's SE reads $81) */
        return r;
    }
    if (reg == 0) {
        r |= 0x40;                                   /* PB6: an input tied high on the IIci; the SE's video beam / SCSI interrupt enable */
        if (!snd_off && (via_ddrb & 0x80)) r = (r & 0x7f) | (via_orb & 0x80);
        return r;
    }
    if (!snd_off && reg == 2) return (r & 0x7f) | (via_ddrb & 0x80);
    if (!snd_off && reg == 3) return (r & 0xf0) | (via_ddra & 0x0f);
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
   chip boots to the question-mark disk). mac.sepram=chip turns the stand-in off.

   The stand-in on the card (20 November 2026). Kept "until the Pi is switched off" meant AppleTalk,
   the startup disk, Monitors' choices and the clock lost at every power-off. Now the stand-in keeps
   them in pram.bin in the card's root, the solo board's file and format (psmac_card.c: the 256 bytes,
   then the clock as the Mac's seconds since 1904, big-endian): read before the ROM's first parameter
   RAM access (xp_card_load, which waits for the card's first look as the solo board does), written
   by the card code on core 3 a second after the Mac's last change, at most every ten seconds, never
   during the boot, and every ten minutes besides while the Mac runs, so that the clock the next start
   resumes from is never older than that. The Pi has no clock of its own and the kernel's network
   stack no time service, so the clock goes on from the last save: it stands still while the Pi is
   off, and the Date & Time control panel (or the Alarm Clock) sets it, which is saved too. The rules
   above apply to what is loaded exactly as to the defaults: the file holds what the Mac wrote, $8A
   included, and every read of $8A is mended as before (bit 4 cleared, 32-bit addressing with more
   than 8 MiB); the Mac's view of $78-$7B (psmac_mbox.c) is the Mac's view, never stored, so the file
   keeps the real startup device; and a parity record ($F9 bit 0 with $FB = $8E, the start-up test's
   "failed", which a solo kernel before item 69 may have saved) is replaced on loading by the
   defaults' $F0-$F3 and $F9-$FB. A chip that answers is never replaced by the file and the file is
   not written for it; the kernel never writes the chip either way. */
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
    int card_loaded;                              /* pram.bin looked for (xp_card_load) */
    uint32_t clock_set;                           /* writes of the clock's bytes (the Mac setting the time) */
    uint32_t gen;                                 /* changes the Mac made to the stand-in (core 0); the card saves them */
    uint64_t changed_at;
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
/* pram.bin on the card, before the stand-in's first byte goes to the ROM (core 0). The card's first
   look runs on core 3 and comes a few tens of milliseconds after the 68k starts; at most three
   seconds are waited for it, as on the solo board. */
static uint8_t xp_saved[256]; static int xp_have_saved; static uint64_t xp_saved_at;   /* what pram.bin holds now, and since when */
static void xp_card_load(void)
{
    extern const uint8_t *psmac_card_pram(int *state, uint32_t *len);
    if (xp.card_loaded) return;
    int st = 0; uint32_t len = 0; const uint8_t *p = 0;
    uint64_t t0 = asc_now();
    while ((p = psmac_card_pram(&st, &len)) == 0 && st == 0 && asc_now() - t0 < 3 * asc_hz()) __asm__ volatile("yield");
    xp_saved_at = asc_now();
    if (!p || len < 256) {
        kprintf("[PSMAC:SE] stand-in parameter RAM: the defaults (%s); it goes into pram.bin on the card once the Mac changes it\n",
                st < 0 ? "no pram.bin on the card, or no card" : "the card did not answer in time");
        __atomic_store_n(&xp.card_loaded, 1, __ATOMIC_RELEASE);
        return;
    }
    for (int i = 0; i < 256; i++) xp_saved[i] = xp.pram[i] = p[i];
    xp_have_saved = 1;
    int parity = (xp.pram[0xf9] & 1) && xp.pram[0xfb] == 0x8e;
    if (parity) {                                 /* the start-up test's record of a failed parity test: the defaults' bytes instead (docs/08 item 69) */
        static const uint8_t rec[] = { 0xf0, 0xf1, 0xf2, 0xf3, 0xf9, 0xfa, 0xfb };
        for (unsigned i = 0; i < sizeof rec; i++) xp.pram[rec[i]] = psmac_pram_default[rec[i]];
    }
    uint32_t sec = len >= 260 ? (uint32_t)p[256] << 24 | (uint32_t)p[257] << 16 | (uint32_t)p[258] << 8 | p[259] : 0;
    if (sec > XP_SECS_START) xp_set_secs(sec);
    kprintf("[PSMAC:SE] stand-in parameter RAM from pram.bin on the card (%u bytes; signature %02x %02x %02x %02x%s), clock %08x%s\n",
            len, xp.pram[12], xp.pram[13], xp.pram[14], xp.pram[15], parity ? "; a parity test record replaced by the defaults" : "",
            xp_secs(), sec > XP_SECS_START ? ", from the last save" : ", the default (the file's is earlier)");
    __atomic_store_n(&xp.card_loaded, 1, __ATOMIC_RELEASE);
}
/* a byte the ROM wrote: the stand-in keeps it (the chip has had it too) */
static void xp_store(int addr, uint8_t v)
{
    xp.writes++;
    if (addr == 0x1fd) { xp.wp = (v & 0x80) != 0; return; }        /* register 13, write protect: always taken */
    if (psmac_startup_write(addr, v)) return;                       /* into the Mac's view of $78-$7B (for the chip, xp_wire_orb kept its own byte) */
    if (!xp.serve || xp.wp) return;
    xp_card_load();
    if (addr < 0x100) { if (xp.pram[addr] == v) return; xp.pram[addr] = v; }
    else if (addr < 0x104) { uint32_t s = xp_secs(), sh = 8 * (addr & 3); xp_set_secs((s & ~(0xffu << sh)) | (uint32_t)v << sh); xp.clock_set++; }
    else return;
    xp.changed_at = asc_now();
    __atomic_store_n(&xp.gen, xp.gen + 1, __ATOMIC_RELEASE);
}
/* core 3 (psmac_card.c): the stand-in's 256 bytes and clock to save, or 0 when nothing is due. Due:
   a second after the Mac's last change, ten seconds after the last save, the boot over; and every ten
   minutes for the clock. A copy the Mac changed while it was taken is left for the next call, and
   bytes that are back to what was saved (the ROM's parameter RAM test writes every byte at each start
   and puts it back) are not a change. */
int psmac_se_pram_take(uint8_t out[260])
{
    extern int psmac_boot_settled(void);
    static uint32_t saved_gen, saved_clock;
    if (!psmac_board_se || psmac_board_solo || !xp.serve || !__atomic_load_n(&xp.card_loaded, __ATOMIC_ACQUIRE)) return 0;
    uint64_t now = asc_now(), hz = asc_hz();
    uint32_t g = __atomic_load_n(&xp.gen, __ATOMIC_ACQUIRE), c = xp.clock_set;
    int clock_due = xp_have_saved && now - xp_saved_at >= 600 * hz;
    if (g == saved_gen && !clock_due) return 0;
    if (!psmac_boot_settled() || now - xp.changed_at < hz || now - xp_saved_at < 10 * hz) return 0;
    for (int i = 0; i < 256; i++) out[i] = xp.pram[i];
    uint32_t s = xp_secs();
    __asm__ volatile("dmb ish" ::: "memory");
    if (__atomic_load_n(&xp.gen, __ATOMIC_ACQUIRE) != g) return 0;
    saved_gen = g;
    int same = xp_have_saved;
    for (int i = 0; i < 256 && same; i++) same = out[i] == xp_saved[i];
    if (same && c == saved_clock && !clock_due) return 0;
    saved_clock = c;
    out[256] = s >> 24; out[257] = s >> 16; out[258] = s >> 8; out[259] = s;
    for (int i = 0; i < 256; i++) xp_saved[i] = out[i];
    xp_have_saved = 1; xp_saved_at = now;
    return 1;
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
    if (xp.serve) xp_card_load();
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
        uint8_t o = (uint8_t)(PB6 | ((b >> k) & 1)); /* enable low, clock low, the bit (PB6 high on the SE as via_data drives it) */
        psmac_wire_write(SE_ORB, 1, o, 0);
        psmac_wire_write(SE_ORB, 1, o | 0x02, 0);     /* clock high: the chip takes the bit */
    }
}
static uint8_t rtc_recv(void)
{
    uint8_t v = 0;
    psmac_wire_write(SE_DDRB, 1, PB6 | 0x06, 0);     /* the data line an input */
    for (int k = 7; k >= 0; k--) {
        psmac_wire_write(SE_ORB, 1, PB6, 0);
        psmac_wire_write(SE_ORB, 1, PB6 | 0x02, 0);
        uint32_t r = psmac_wire_read(SE_ORB, 1);
        if (xp.sim_dead) r |= 1;
        v = (uint8_t)(v << 1 | (r & 1));
    }
    psmac_wire_write(SE_DDRB, 1, PB6 | 0x07, 0);
    psmac_wire_write(SE_ORB, 1, PB6 | 0x06, 0);       /* enable high: the transaction is over */
    return v;
}
static void rtc_probe(void)
{
    if (xp.probed) return;
    xp.probed = 1;
    if (psmac_board_orig == 1 || psmac_board_orig == 2) {
        /* The 128K and 512K have the clock chip of 20 bytes (343-0040): no extended parameter RAM, which
           the IIci ROM keeps its addressing mode ($8A), slot records and startup device in. So the
           stand-in answers for all of it, the clock too, from pram.bin on the card, exactly as for a
           chip that does not answer; the old chip is not asked anything. */
        for (int i = 0; i < 256; i++) xp.pram[i] = psmac_pram_default[i];
        xp_set_secs(XP_SECS_START);
        xp.serve = 1;
        kprintf("[PSMAC:SE] clock chip: the Macintosh %s's (20 bytes of parameter RAM, none extended): the board layer answers in its place, "
                "parameter RAM and clock from pram.bin on the card\n", psmac_board_orig == 1 ? "128K" : "512K");
        return;
    }
    uint8_t ddrb = psmac_wire_read(SE_DDRB, 1) & 0xff, sig[4], sec[4];
    psmac_wire_write(SE_ORB, 1, PB6 | 0x06, 0);       /* enable high, clock high, before the lines become outputs */
    psmac_wire_write(SE_DDRB, 1, PB6 | 0x07, 0);
    for (int i = 0; i < 4; i++) { int a = 0x0c + i; rtc_send(0xb8 | (a >> 5)); rtc_send((a & 0x1f) << 2); sig[i] = rtc_recv(); }
    for (int i = 0; i < 4; i++) { rtc_send(0x81 | (i << 2)); sec[i] = rtc_recv(); }
    psmac_wire_write(SE_DDRB, 1, ddrb, 0);
    int all_ff = 1;
    for (int i = 0; i < 4; i++) if (sig[i] != 0xff || sec[i] != 0xff) all_ff = 0;
    uint32_t s = (uint32_t)sec[3] << 24 | (uint32_t)sec[2] << 16 | (uint32_t)sec[1] << 8 | sec[0];
    int numc = sig[0] == 'N' && sig[1] == 'u' && sig[2] == 'M' && sig[3] == 'c';
    if (!all_ff) {
        kprintf("[PSMAC:SE] clock chip: answers; parameter RAM %s (signature %02x %02x %02x %02x), clock %08x; pram.bin on the card is not used\n",
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
            "The board layer answers in its place: parameter RAM and clock from pram.bin on the card (the defaults and 20 November 2026 without it), saved there as the Mac changes them (mac.sepram=chip to leave the chip alone)\n");
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
static int scsi_wait_ms = 250;                  /* mac.sescsiwait: the SCSI Manager's selection timeout on the SE's bus */
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

/* The Plus's pseudo-DMA (22 November 2026, psmac_board_plus.c): the SE's BBU holds DTACK on the DMA
   window until the 5380 raises DRQ, so a byte is moved per bus cycle and the glue does the waiting;
   the Plus has no such glue, and an access to the window without DRQ is a plain register access
   (MAME's mac128.cpp and macplus_ps). So before each byte the bus and status register (5) is read
   until DRQ (bit 6) is up: one or two cycles more per byte. It gives up when the phase no longer
   matches (bit 3: the target has moved on, a status or message phase) or after 100,000 reads; the
   byte is then $FF on a read and not written. */
static uint32_t plus_drq_bytes, plus_drq_waits, plus_drq_timeouts;
static int plus_wait_drq(void)
{
    plus_drq_bytes++;
    for (uint32_t i = 0; i < 100000u; i++) {
        uint32_t bsr = psmac_wire_read(SE_SCSI_RD + (5 << 4), 1);
        if (bsr & 0x40) return 1;
        if (!(bsr & 0x08)) break;
        if (i) plus_drq_waits++;
    }
    plus_drq_timeouts++;
    return 0;
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
            if (reg == 0) { via_orb = (uint8_t)data; xp_lines(data & 0xff); data = xp_wire_orb(data & 0xff); }
            else if (reg == 1 || reg == 15) via_ora = (uint8_t)data;
            else if (reg == 2) { xp.ddr0 = data & 1; via_ddrb = (uint8_t)data; }
            else if (reg == 3) via_ddra = (uint8_t)data;
            else if (reg == 14) { if (data & 0x80) via_ier |= data & 0x7f; else via_ier &= ~(data & 0x7f); }
            uint32_t wd = via_data(reg, data & 0xff);
            if (psmac_board_plus) {                  /* the shift register and ADB's lines are answered here (psmac_board_plus.c) */
                level_lock(); int local = psmac_plus_via_write(reg, &wd); store_level(se_wire_ipl); level_unlock();
                if (local) { trace("VIA", 1, addr, se, size, data & 0xff); local_writes++; return 1; }
            }
            trace("VIA", 1, addr, se, size, wd);
            psmac_wire_write(se, 1, wd, posted);
            return 1;
        }
        if (psmac_board_plus) {
            uint32_t v = 0;
            level_lock(); int local = psmac_plus_via_local(reg, &v); if (local) store_level(se_wire_ipl); level_unlock();
            if (local) { trace("VIA", 0, addr, se, size, v); local_reads++; *val = size == 1 ? v : size == 2 ? v << 8 | v : v << 24 | v << 16 | v << 8 | v; return 1; }
        }
        r = via_read_fix(reg, psmac_wire_read(se, 1) & 0xff);
        if (reg == 0) r = xp_read(r);
        if (psmac_board_plus) {
            if (reg == 3) r = via_ddra;              /* the ROM's own: the wire's has PA4 out and PA7 in whatever it wrote */
            level_lock(); r = psmac_plus_via_read(reg, r); store_level(se_wire_ipl); level_unlock();
        }
        trace("VIA", 0, addr, se, size, r);
        *val = size == 1 ? r : size == 2 ? r << 8 | r : r << 24 | r << 16 | r << 8 | r;
        return 1;
    }
    if (off >= 0x4000 && off < 0x6000) {
        /* SCC: B control, A control, B data, A data at +0, +2, +4, +6 on the IIci; the SE reads
           them at $9FFFF8 (upper byte) and writes them at $BFFFF9 (lower byte) */
        uint32_t ch = off & 6;
        if (write) {
            if (psmac_board_plus) data = psmac_plus_scc_write(ch, data & 0xff);   /* the register pointer followed, WR15's DCD enable kept off (the Plus's mouse) */
            trace("SCC", 1, addr, 0xBFFFF9u + ch, size, data & 0xff); psmac_wire_write(0xBFFFF9u + ch, 1, data & 0xff, posted);
            if (psmac_board_plus) psmac_plus_scc_written();
            return 1;
        }
        r = psmac_wire_read(0x9FFFF8u + ch, 1) & 0xff;
        if (psmac_board_plus) psmac_plus_scc_read(ch);
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
        if (write) {
            trace("SCSI", 1, addr, SE_SCSI_PDMA_WR, size, data);
            for (int i = 0; i < size; i++) {
                if (psmac_board_plus && !plus_wait_drq()) continue;
                psmac_wire_write(SE_SCSI_PDMA_WR, 1, (data >> (8 * (size - 1 - i))) & 0xff, 0);
            }
            return 1;
        }
        { static int first = 1; if (first) { first = 0; psmac_boot_mark("SCSI: first pseudo-DMA read: the first block from the SE's disk"); } }
        r = 0;
        for (int i = 0; i < size; i++) {
            if (psmac_board_plus && !plus_wait_drq()) { r = r << 8 | 0xff; continue; }
            r = r << 8 | (psmac_wire_read(SE_SCSI_PDMA_RD, 1) & 0xff);
        }
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
        if (!psmac_board_solo) level_lock();
        if (write) { trace("RBV", 1, addr, off, size, data & 0xff); rbv_write(off, data & 0xff); }
        else { r = rbv_read(off); trace("RBV", 0, addr, off, size, r); *val = size == 1 ? r : r << 8 | r; }
        if (psmac_board_solo) psmac_solo_level_now();   /* the level-2 line follows its flags at once (psmac_board_solo.c) */
        else { store_level(se_wire_ipl); level_unlock(); }   /* and on the SE board too (below, psmac_se_set_level) */
        return 1;
    }
    if (off >= 0x14000 && off < 0x16000) { if (write) asc_write(off, data & 0xff); else { r = asc_read(off); *val = size == 1 ? r : r << 8 | r; } return 1; }
    if (off >= 0x24000 && off < 0x26000) { if (write) vdac_regs[off & 31] = data; else *val = vdac_regs[off & 31]; return 1; }
    /* VIA2 (0x2000: the ROM probes it to tell decoders apart; the IIci has none), 0x21C00, the rest: open bus */
    if (!write) { *val = 0; local_reads++; } else local_writes++;
    return 1;
}

/* The level the JIT sees (INTF.IPL) on the SE board (20 November 2026, docs/08 item 76). It is the
   SE's own level, which comes back with every cycle on the wire (VIA, SCC), combined with the RBV's
   level 2, which is this file's: the card's interrupt line (the screens' VBLs, the network, the
   input ring), raised and cleared in the slot-9 window, Pi memory the Mac writes without a bus
   cycle. Until now only the wire's cycles and the service core's poll stored the combination (20
   times a millisecond on the GPIO, once a millisecond in the testbed), so after a slot handler had
   cleared its source the JIT still saw level 2. The ROM's slot dispatcher reads the RBV again
   after each handler ($40806EAA, the loop through Lvl2DT+4), but on this board that read did not
   bring the level up to date, as it has on the solo board since item 48. Two things followed: the
   handler's RTE went straight back into the level-2 handler with nothing to do, again and again
   until the next poll, and with a deferred task queued, System 7.1's patch of the Deferred Task
   Manager ($6E4: `andi #$F8FF,sr; rts` before the handler's own RTE) took the stale level 2 inside
   the handler, a level deeper each time: item 48's runaway of the stack, now on the SE board,
   whose two screens have twice the VBL interrupts. Now every RBV access stores the level as the
   poll does, under one lock with the wire's stores, so that neither stores a level the other has
   just made stale. The wire's last level is kept for it. */
static volatile uint8_t se_level_lock;
static inline void level_lock(void) { while (__atomic_test_and_set(&se_level_lock, __ATOMIC_ACQUIRE)) __asm__ volatile("yield"); }
static inline void level_unlock(void) { __atomic_clear(&se_level_lock, __ATOMIC_RELEASE); }
extern struct M68KState *__m68k_state;
static void store_level(unsigned raw)        /* the caller holds the lock */
{
    if (__m68k_state) __m68k_state->INTF.IPL = psmac_se_ipl(raw);
}
/* the Plus's keyboard and mouse on the service core (psmac_board_plus.c) take the same lock */
void psmac_se_plus_lock(void) { level_lock(); }
void psmac_se_plus_unlock(void) { store_level(se_wire_ipl); level_unlock(); }
/* The programmer's interrupt switch (21 November 2026, docs/08 item 92). On the SE it pulls /IPL2 alone,
   raw level 4 (Guide to the Macintosh Family Hardware, table 3-2); on the IIci it is the NMI, level 7.
   Until an outside review pointed it out the mapper below dropped the bit, so pressing the switch on a
   hung Mac did nothing. Level 7 is taken on its edge by the 68000, so it is not a level here: a press
   is the bit seen without a break for 10 ms (a switch is held far longer; a glitch on the wire is not),
   and each press asks once for the NMI the remote's "nmi" also uses (INTF.ARM_err). */
static void interrupt_switch(unsigned raw)
{
    static uint64_t held_since;
    static int pressed;
    uint64_t now = asc_now();
    if (!(raw & 4)) { held_since = 0; pressed = 0; return; }
    if (!held_since) held_since = now;
    if (!pressed && now - held_since >= asc_hz() / 100) {
        pressed = 1;
        kprintf("[PSMAC:SE] the programmer's interrupt switch: an NMI for the Mac\n");
        if (__m68k_state) __atomic_store_n(&__m68k_state->INTF.ARM_err, 1, __ATOMIC_RELEASE);
    }
}
/* Aurora68 Recovery (psmac_recovery.c): the switch held as the 68k would be released. mac.switchsim=1 is the
   testbed's switch: MAME's SE has no programmer's switch to press */
int psmac_se_switch_held(void)
{
    extern const char *psmac_option(const char *);
    const char *o = psmac_option("mac.switchsim");
    if (o && o[0] == '1') return 1;
    return psmac_board_se && !psmac_board_solo && (se_wire_ipl & 4) != 0;
}
void psmac_se_set_level(unsigned raw)         /* psmac_bus.c, after each cycle on the wire and each poll */
{
    interrupt_switch(raw);
    level_lock(); se_wire_ipl = raw; psmac_plus_tick(); store_level(raw); level_unlock();
}

/* The interrupt level the SE drives, read as the IIci ROM expects it: the SE's level 1 is the VIA
   on both; its level 2 is the SCC (the IIci's level 4; the 5380's interrupt is kept off); the
   IIci's level 2, the RBV, is this file's own. Bit 2, the interrupt switch, is an edge, handled by
   interrupt_switch above, not a level. */
unsigned psmac_se_ipl(unsigned raw)
{
    if (psmac_board_solo) return psmac_solo_ipl();
    unsigned ipl = 0;
    if (raw & 2) ipl = 4;
    else if (raw & 1) ipl = 1;
    if (psmac_se_rbv_irq() && ipl < 2) ipl = 2;
    if (psmac_plus_irq() && ipl < 1) ipl = 1;    /* the Plus: the ADB transceiver's shift-register interrupt, the VIA's level */
    return ipl;
}

/* ---- the SE's speaker ------------------------------------------------------------------------
   (20 November 2026.) The SE has no sound chip: its BBU reads one word of a 370-word buffer at the
   top of RAM in every horizontal blanking interval, the 342 visible lines and the 28 of vertical
   blanking, and plays the word's high byte as the width of a pulse: 370 samples a frame, 60.15
   frames a second, 22,255 samples a second, the ASC's 22,257 within a tenth of a per mille (the low
   byte is the 400K drive's speed, left alone here). The main buffer starts $300 below the top of
   RAM ($3FFD00 with 4 MB, $27FD00 with 2.5, $1FFD00 with 2, $FFD00 with 1; the alternate one $5F00
   below, chosen by VIA port A bit 3 = 0). VIA port A bits 0-2 are the volume, port B bit 7 switches
   the sound off when 1 (MAME's mac128.cpp: snd_push, mac_via_out_a_se, mac_via_out_b_se; Guide to
   the Macintosh Family Hardware for PA3).
   What the IIci ROM and the Sound Manager write into the ASC becomes the SE's samples here:
   - FIFO mode (mode $801 = 1: System 7's alert sounds, everything the Sound Manager plays): the
     two channels' bytes from the rings asc_write fills, mixed to one (their mean when both have
     data); wavetable mode (2: the IIci ROM's startup chime): the chip's four voices, synthesised
     here at the SE's rate, each a 512-byte table from wave RAM stepped by its 9.15 increment
     ($814 + 8n) as MAME's asc.cpp does, their sum over four. With neither, the last sample is held.
   - The buffer is written in halves of 185 words, from the service core between the Mac's own bus
     cycles, as the screen's rows are: whenever the SE's play position crosses into one half, the
     other half gets the next 185 samples, so a word is always written about a quarter to three
     quarters of a frame before it is played and never while it is. Byte writes to the even
     addresses: the high bytes only. A cycle the Mac asks for while a half is being written goes
     out between two of its bytes (psmac_serve_waiting, 21 November): before, it waited for all
     185, a third of a millisecond on the GPIO, twenty GCR bytes of a floppy (docs/08 item 93).
   - The play position comes from the SE's vertical blanking, the VIA's CA1 flag, which the service
     core reads every millisecond while sound plays (a read of the IFR has no side effects). The VBL
     starts at word 342 (MAME's screen: the first line of vertical blanking); the frame's length is
     measured from consecutive edges, so the position is known for the whole frame. While the Mac
     has CA1 disabled (the chime plays before interrupts are on), the service core clears the flag
     itself to see the next edge; once the Mac takes the interrupt, its own handler does.
   - The volume is the ASC's (bits 5-7 of $806, which the ROM and the Sound Manager set from the
     Sound control panel) on PA0-2, the main buffer on PA3, and PB7 low while there is sound: a
     sound starts with both halves written first, and a quarter second after the last sample the
     speaker is switched off again, so the SE does not hum between sounds. Volume 0 keeps it off.
   These bits are driven on the wire alone (psmac_se_wire_write rewrites the Mac's VIA writes as
   they go out, on the service core, and the speaker writes them itself when they must change, port
   A through its no-handshake register so the VBL flag is not cleared); the ROM reads back its own.
   mac.sesound=off leaves all of it as it was before: the bytes thrown away, the four bits the ROM's.
   The Sound Manager's pacing does not depend on any of this (asc_drain, above). */
#define SND_WORDS 370u                           /* one word a line, 342 visible and 28 of vertical blanking */
#define SND_HALF  (SND_WORDS / 2)
#define SND_VBL_WORD 342u                        /* the word played first after the VBL edge */
#define SE_VIA_IFR (SE_VIA + 13 * 0x200)
static uint32_t snd_base(void) { return se_ram_size() - 0x300; }
static struct {
    int active, edge_ok, ca1_held, clear_seen, half, fresh, on, vol, edge_halves, feeding;
    uint64_t edge_t, period, poll_t, last_t;
    uint8_t hold;
    uint32_t flush_seen, wt_seen, phase[4];
    uint8_t w_ora, w_ddra, w_orb, w_ddrb;        /* the VIA's registers as last written on the wire (service core) */
    uint32_t edges, own_clears, halves, late, held, starts, underruns, played, half_us_max, quiets;
} snd = { .half = -1 };

/* the next sample, at the SE's rate */
static uint32_t be24(uint32_t off) { return (uint32_t)asc_regs[off] << 16 | (uint32_t)asc_regs[off + 1] << 8 | asc_regs[off + 2]; }
static int snd_wavetable_on(void)
{
    if (asc_regs[0x801] != 2) return 0;
    for (int v = 0; v < 4; v++) if (be24(0x815 + 8 * v)) return 1;
    return 0;
}
static int snd_fifo_level(int ch) { return (int)(__atomic_load_n(&snd_head[ch], __ATOMIC_ACQUIRE) - snd_tail[ch]); }
static uint8_t snd_next(void)
{
    uint8_t mode = asc_regs[0x801];
    if (mode == 2) {
        if (snd.wt_seen != snd_wt_gen) { snd.wt_seen = snd_wt_gen; for (int v = 0; v < 4; v++) snd.phase[v] = be24(0x811 + 8 * v); }
        int sum = 0;
        for (int v = 0; v < 4; v++) {
            snd.phase[v] = (snd.phase[v] + be24(0x815 + 8 * v)) & 0xffffffu;
            sum += (int8_t)(asc_regs[(v >> 1) * 0x400 + (v & 1) * 0x200 + ((snd.phase[v] >> 15) & 0x1ff)] ^ 0x80);
        }
        snd.played++;
        return snd.hold = (uint8_t)(0x80 + sum / 4);
    }
    int n = 0, sum = 0;
    for (int ch = 0; ch < 2; ch++)
        if (snd_fifo_level(ch) > 0) { sum += snd_ring[ch][snd_tail[ch] % SND_RING]; __atomic_store_n(&snd_tail[ch], snd_tail[ch] + 1, __ATOMIC_RELEASE); n++; }
    if (psmac_audio_spk_level() > 0) { sum += psmac_audio_spk_take(); n++; }   /* the sound card's samples (psmac_audio.c), when the Sound Manager plays there */
    if (!n) { if (mode == 1 && snd.feeding) snd.underruns++; return snd.hold; }   /* a gap inside a sound, not the quiet after it */
    snd.played++;
    return snd.hold = (uint8_t)(sum / n);
}

/* the VIA's lines for the speaker: what psmac_se_wire_write makes of a write, and what the speaker
   writes itself when its state changes */
static uint8_t snd_lines(uint32_t reg, uint8_t v)
{
    v = (uint8_t)via_data(reg, v);               /* bit 6 of both ports, as every write of the ROM's gets it */
    switch (reg) {
    case 0:  return (uint8_t)((v & 0x7f) | (snd.on ? 0 : 0x80));
    case 2:  return v | 0x80;
    case 1: case 15: return (uint8_t)((v & 0xf0) | 0x08 | (snd.vol & 7));
    case 3:  return v | 0x0f;
    default: return v;
    }
}
static void snd_seen(uint32_t reg, uint8_t v)
{
    if (reg == 0) snd.w_orb = v; else if (reg == 2) snd.w_ddrb = v; else if (reg == 1 || reg == 15) snd.w_ora = v; else if (reg == 3) snd.w_ddra = v;
}
/* the service core, for every request it puts on the wire for core 0 (psmac_bus.c): a write to the
   VIA gets the speaker's bits; a RESET (the VIA's registers back to 0) is noted */
uint32_t psmac_se_wire_write(int reset, uint32_t addr, uint32_t data)
{
    if (!psmac_board_se || psmac_board_solo || snd_off) return data;
    if (reset) { snd.w_ora = snd.w_ddra = snd.w_orb = snd.w_ddrb = 0; return data; }
    if ((addr & ~0x1e00u) != SE_VIA) return data;
    uint32_t reg = (addr >> 9) & 15;
    uint8_t v = snd_lines(reg, (uint8_t)data);
    snd_seen(reg, v);
    return v;
}
static void snd_via(uint32_t reg, uint8_t v) { psmac_wire_write(SE_VIA + (reg << 9), 1, v, 0); snd_seen(reg, v); }
static void snd_apply(void)
{
    if ((snd.w_ddra & 0x0f) != 0x0f) snd_via(3, snd_lines(3, snd.w_ddra));
    if ((snd.w_ora & 0x0f) != (0x08 | (snd.vol & 7))) snd_via(15, snd_lines(15, snd.w_ora));
    if (!(snd.w_ddrb & 0x80)) snd_via(2, snd_lines(2, snd.w_ddrb));
    if (((snd.w_orb >> 7) & 1) != !snd.on) snd_via(0, snd_lines(0, snd.w_orb));
}

/* the SE's vertical blanking: an edge of CA1 seen in the IFR */
static void snd_vbl_poll(uint64_t now, uint64_t hz)
{
    if (now - snd.poll_t < hz / 1000) return;
    uint64_t prev = snd.poll_t;
    snd.poll_t = now;
    uint32_t ifr = psmac_wire_read(SE_VIA_IFR, 1) & 0xff;
    if (!(ifr & 0x02)) { snd.ca1_held = 0; snd.clear_seen = 1; return; }
    if (snd.ca1_held) return;                    /* the edge seen already; the Mac has not taken it yet */
    uint64_t t = prev && now - prev < hz / 500 ? prev + (now - prev) / 2 : now;
    if (snd.edge_ok && snd.clear_seen) {
        /* consecutive edges (the flag was seen clear between them): the frame's length, smoothed */
        uint64_t dt = t - snd.edge_t;
        if (dt > snd.period / 2 && dt < snd.period * 3 / 2) snd.period = snd.period - snd.period / 8 + dt / 8;   /* the next edge: the frame's length, smoothed */
    }
    snd.edge_t = t; snd.edge_ok = 1; snd.edges++; snd.edge_halves = 0;
    snd.clear_seen = 0;
    if (!(via_ier & 0x02)) { psmac_wire_write(SE_VIA_IFR, 1, 0x02, 0); snd.own_clears++; snd.ca1_held = 0; }
    else snd.ca1_held = 1;
}
/* the word the SE plays now */
static uint32_t snd_beam(uint64_t now)
{
    uint64_t lines = (now - snd.edge_t) * SND_WORDS / snd.period;
    return (uint32_t)((SND_VBL_WORD + lines) % SND_WORDS);
}
/* Silence left in the buffer (22 November 2026). Between sounds the speaker is switched off (PB7) and the
   buffer was left as it was: the last 370 samples of the last sound, or whatever the board's RAM held at
   power-on, which the SE and the Plus go on playing 60 times a second. A tester's Plus had "a tiny high
   pitched whine" from the speaker: a fragment repeated at the frame rate is a tone, and what the sound
   switch lets through of it is heard. So the whole buffer gets one level when a sound ends (the last
   sample's, so nothing steps) and once before the first sound (the middle, $80): a constant level is no
   sound whatever the switch lets through. 370 bytes, the Mac's own requests served between them. */
static void snd_quiet(uint8_t level)
{
    uint32_t dst = snd_base();
    for (uint32_t i = 0; i < SND_WORDS; i++) {
        psmac_wire_write(dst + 2 * i, 1, level, 1);
        psmac_serve_waiting();
    }
    snd.hold = level; snd.quiets++;
}
static void snd_service(void)
{
    if (snd_off) return;
    uint64_t now = asc_now(), hz = asc_hz();
    if (!snd.period) { snd.period = hz * 704u * 370u / 15667200u; snd_quiet(0x80); }   /* the SE's frame: 704 dots by 370 lines at 15.6672 MHz, 60.15 Hz; the buffer silent from the start */
    if (snd.flush_seen != snd_flush) { snd.flush_seen = snd_flush; for (int ch = 0; ch < 2; ch++) snd_tail[ch] = __atomic_load_n(&snd_head[ch], __ATOMIC_ACQUIRE); }
    int sounding = snd_fifo_level(0) > 0 || snd_fifo_level(1) > 0 || snd_wavetable_on() || psmac_audio_spk_level() > 0;
    if (sounding) snd.last_t = now;
    snd.feeding = now - snd.last_t < hz / 30;    /* the Sound Manager refills the FIFO at least every other frame while it plays */
    if (!snd.active) {
        if (!sounding) { snd_apply(); return; }  /* idle: the lines stay as they are, unless a RESET took them */
        snd.active = 1; snd.edge_ok = 0; snd.ca1_held = 0; snd.clear_seen = 0; snd.half = -1; snd.fresh = 0; snd.starts++;
    }
    snd.vol = (asc_regs[0x806] >> 5) & 7;
    snd_vbl_poll(now, hz);
    if (snd.edge_ok) {
        uint32_t pos = snd_beam(now);
        int half = pos >= SND_HALF;
        /* Two halves a frame, counted from the VBL edge: the frame's length only places them within
           the frame, so the samples go out at the SE's own rate even when the estimate is off (MAME's
           emulated time runs in bursts against the wall clock: 53 s of chime in its recording against
           69 s of the Pi's timer). Past two frames without an edge (the Mac holding CA1) it runs free. */
        if (half != snd.half && snd.edge_halves >= 2 && now - snd.edge_t < 2 * snd.period) { if (snd.edge_halves == 2) { snd.held++; snd.edge_halves = 3; } half = snd.half; }
        if (half != snd.half) {
            /* the SE has just moved into this half: the other one gets the next samples */
            if (snd.half >= 0 && (pos % SND_HALF) > SND_HALF / 2) snd.late++;   /* noticed late: less than a quarter frame to write it in */
            snd.half = half;
            uint32_t dst = snd_base() + 2 * (half ? 0 : SND_HALF);
            /* a request of the Mac's that comes meanwhile goes between two bytes, not after the 185th
               (docs/08 item 93): the floppy driver polls the IWM a byte every 16 us */
            for (uint32_t i = 0; i < SND_HALF; i++) {
                psmac_wire_write(dst + 2 * i, 1, snd_next(), 1);
                psmac_serve_waiting();
            }
            uint32_t us = (uint32_t)((asc_now() - now) * 1000000u / hz);
            if (us > snd.half_us_max) snd.half_us_max = us;
            snd.halves++; snd.edge_halves++;
            if (snd.fresh < 2) snd.fresh++;
        }
    }
    /* on once both halves hold fresh samples; off a quarter second after the last one */
    snd.on = snd.fresh >= 2 && snd.vol && now - snd.last_t < hz / 4;
    if (now - snd.last_t >= hz / 4) { snd.active = 0; snd_apply(); snd_quiet(snd.hold); return; }
    snd_apply();
}

/* the service core, between the Mac's requests as well as when it has none (psmac_bus.c): a check of
   the time and the rings when nothing is due */
void psmac_se_sound(void) { if (psmac_board_se && !psmac_board_solo) snd_service(); }
/* mac.buswait's figures (psmac_bus.c) are filed by this: a sound is playing and its halves are being written */
int psmac_se_speaker_busy(void) { return psmac_board_se && !psmac_board_solo && !snd_off && snd.active; }

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

/* The rows checked as well as sent (22 November 2026). A tester's 512Ke showed single words of the desktop
   wrong on its glass, short dotted marks that went when the pointer passed over them (the row sent
   again): the Pi's copy was right and the board's memory held a word that was not. So while no row is
   waiting, one row a tick (60 a second, the screen every 5.7 s) is read back from the board and any word
   that differs from the copy is written again; the first few are logged with the address, the word
   written and the word read, to tell an address line (the neighbouring row's pattern) from a data line. */
static uint32_t verify_row, verify_rows, verify_words_bad, verify_rows_bad, verify_logged;
static uint64_t verify_next;
/* mac.screencheck=off: never; on (the default): until the read-backs show the reads, not the screen, are
   wrong. The tester's 512Ke, the same day: 6,011 of 6,605 rows read back with words that differ, 29 of 32
   on average, while its screen showed only a few stray dots: its reads of the board's memory come back
   wrong and its writes do not, so the check rewrote right words and added 2,000 reads a second to a bus
   whose reads were already in doubt. Past 64 rows with more than a quarter of them differing, it stops. */
static int verify_on = 1;
static void se_verify_row(void)
{
    uint64_t now = asc_now();
    if (!verify_on || !shadow_valid || now < verify_next) return;
    if (verify_rows >= 64 && verify_rows_bad * 4 > verify_rows) {
        verify_on = 0;
        kprintf("[PSMAC:SE] screen check stopped: %u of %u rows read back differently (%u words): the board's memory reads back "
                "wrong while the screen shows what was written, so reads over the wire are in doubt here, not the screen "
                "(the VIA's and the SCC's reads may be too: the mouse and keyboard lines)\n", verify_rows_bad, verify_rows, verify_words_bad);
        return;
    }
    verify_next = now + asc_hz() / 60;
    uint32_t y = verify_row; verify_row = (verify_row + 1) % SE_ROWS;
    if (dirty[y >> 5] & (1u << (y & 31))) return;           /* a send is coming anyway */
    const uint16_t *want = (const uint16_t *)(shadow + y * SE_ROWBYTES);
    uint32_t base = psmac_se_fb_base() + y * SE_ROWBYTES, bad = 0;
    for (int i = 0; i < SE_ROWBYTES / 2; i++) {
        uint16_t w = want[i];
        uint32_t r = psmac_wire_read(base + 2 * i, 2) & 0xffff;
        psmac_serve_waiting();
        if (r == w || (dirty[y >> 5] & (1u << (y & 31)))) continue;   /* changed meanwhile: the send will cover it */
        bad++;
        if (verify_logged < 12) {
            verify_logged++;
            kprintf("[PSMAC:SE] screen check: row %u word %d at $%06x held $%04x, $%04x was sent (xor $%04x)\n",
                    y, i, base + 2 * i, r, w, r ^ w);
        }
        psmac_wire_write(base + 2 * i, 2, w, 1);
    }
    verify_rows++;
    if (bad) { verify_rows_bad++; verify_words_bad += bad; }
}

/* the service core: a few rows per call, so the Mac's own accesses are never far behind */
void psmac_se_service(void)
{
    if (psmac_board_solo) { psmac_solo_service(); return; }
    if (!psmac_board_se) return;
    snd_service();                                /* the speaker first: its halves have deadlines, the rows do not */
    for (int n = 0; n < 4; n++) {
        int y = -1;
        for (uint32_t w = 0; w < (SE_ROWS + 31) / 32 && y < 0; w++) {
            uint32_t bits = dirty[w];
            if (bits) { int b = __builtin_ctz(bits); __atomic_fetch_and(&dirty[w], ~(1u << b), __ATOMIC_ACQUIRE); y = w * 32 + b; }
        }
        if (y < 0) { if (!n) se_verify_row(); return; }
        const uint16_t *src = (const uint16_t *)(shadow + y * SE_ROWBYTES);
        uint32_t dst = psmac_se_fb_base() + y * SE_ROWBYTES;
        for (int i = 0; i < SE_ROWBYTES / 2; i++) {
            psmac_wire_write(dst + 2 * i, 2, src[i], 1);
            psmac_serve_waiting();                /* the Mac's own cycles between the words, as for the speaker */
        }
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
/* RAM back at 0: the SE's BBU on an access to the ROM's space, the Plus's VIA port A bit 4 = 0 (with
   PA6 = 1, the main screen, and PA3 = 1, the main sound buffer, as the Plus's own ROM leaves them) */
static void se_overlay_off(void)
{
    if (!psmac_board_plus) { psmac_wire_read(0x400000u, 2); return; }
    psmac_wire_write(SE_VIA + 15 * 0x200, 1, 0x48, 0);
    psmac_wire_write(SE_VIA + 3 * 0x200, 1, 0x58, 0);
}
void psmac_se_start(void)
{
    if (!psmac_board_se || psmac_board_solo) { se_ready = 1; return; }
    se_overlay_off();
    se_ram_lines();
    shadow_valid = 0;
    for (uint32_t w = 0; w < (SE_ROWS + 31) / 32; w++) dirty[w] = 0;
    static const char *const orig_name[] = { "Macintosh Plus", "Macintosh 128K", "Macintosh 512K", "Macintosh 512Ke" };
    kprintf("[PSMAC:SE] board layer up: %s memory %u KB, screen buffer at $%06x, VIA $EFE1FE, SCC $9FFFF8/$BFFFF9, %s, IWM $DFE1FF; RBV, ASC, VDAC answered here%s\n",
            psmac_board_plus ? orig_name[psmac_board_orig & 3] : "SE", se_ram_kb, psmac_se_fb_base(),
            psmac_board_orig ? "no SCSI (the 5380 answered here as an empty bus)" : "SCSI $5FF000",
            psmac_board_plus ? "; ADB answered here (the Plus keyboard and mouse as its keyboard and mouse), overlay on VIA PA4, PB3-PB6 and PA7 never driven" : "");
    if (snd_off) kprintf("[PSMAC:SE] sound: mac.sesound=off, the ASC's samples are not played (the SE's speaker stays as the ROM leaves it)\n");
    else kprintf("[PSMAC:SE] sound: the ASC's samples go to the SE's speaker through its sound buffer at $%06x, 370 a frame, written in halves after the SE's own VBL; "
                 "volume on VIA PA0-2, PB7 low only while a sound plays (mac.sesound=off to leave the speaker alone)\n", snd_base());
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
    via_ddra = via_ddrb = via_ora = via_orb = via_ier = 0;
    if (psmac_board_plus) { level_lock(); psmac_plus_reset(); store_level(se_wire_ipl); level_unlock(); }
    se_overlay_off();
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
    if ((tok = find_token(args, "mac.board="))) {
        tok += 10;
        if (tok[0] == 's' && tok[1] == 'e') psmac_board_se = 1;
        else if (tok[0] == 'p' && tok[1] == 'l' && tok[2] == 'u' && tok[3] == 's') { psmac_board_se = 1; psmac_board_plus = 1; }
        else if ((tok[0] == '1' && tok[1] == '2' && tok[2] == '8' && (tok[3] == 'k' || tok[3] == 'K')) ||
                 (tok[0] == '5' && tok[1] == '1' && tok[2] == '2' && (tok[3] == 'k' || tok[3] == 'K'))) {
            /* the original Macintosh: the Plus's board layer without SCSI, with its own RAM (the screen
               and sound buffers at the top of 128K or 512K) and, but on the 512Ke, the old clock chip */
            psmac_board_se = 1; psmac_board_plus = 1;
            psmac_board_orig = tok[0] == '1' ? 1 : (tok[4] == 'e' || tok[4] == 'E') ? 3 : 2;
            se_ram_kb = tok[0] == '1' ? 128 : 512;
            scsi_mode = 2;                          /* no 5380: answered here as an empty bus, the wire never touched there */
        }
    }
    psmac_solo_options(args);
    psmac_plus_options(args);
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
    if ((tok = find_token(args, "mac.sescsi=")) && !psmac_board_orig) { tok += 11; scsi_mode = tok[0] == 'r' ? 1 : tok[0] == 'e' ? 2 : 0; }
    if ((tok = find_token(args, "mac.screencheck="))) verify_on = tok[16] != 'o' || tok[17] != 'f';
    if ((tok = find_token(args, "mac.sescsiwait="))) {
        int v = 0;
        for (tok += 15; *tok >= '0' && *tok <= '9'; tok++) v = v * 10 + (*tok - '0');
        if (v >= 1 && v <= 1000) scsi_wait_ms = v;
    }
    if ((tok = find_token(args, "mac.sefloat="))) { tok += 12; scsi_float_sim = tok[0] == '1'; }   /* the testbed: the 5380 read as on an unterminated bus */
    if ((tok = find_token(args, "mac.seram="))) {   /* 1, 2, 2.5 or 4: the SE's address arithmetic takes the top of what is fitted */
        tok += 10; uint32_t n = 0; while (*tok >= '0' && *tok <= '9') n = n * 10 + (*tok++ - '0');
        uint32_t kb = n << 10; if (tok[0] == '.' && tok[1] == '5') kb += 512;
        if (kb == 1024 || kb == 2048 || kb == 2560 || kb == 4096) se_ram_kb = kb;
    }
    if ((tok = find_token(args, "mac.sesound="))) { tok += 12; snd_off = tok[0] == 'o' && tok[1] == 'f'; }
}

void psmac_se_report(void)
{
    if (psmac_board_solo) { psmac_solo_report(); return; }
    if (!psmac_board_se) return;
    kprintf("[PSMAC:SE] rows sent %u%s, rows checked %u (%u with %u words wrong, written again%s), frames compared %u, local reads %u writes %u, RBV IFR %02x IER %02x SIER %02x | clock chip: %s, %u bytes read (%u of them $FF from the chip), %u written\n",
            rows_sent, se_lines_bad ? " (an address line failed its test at the start)" : "", verify_rows, verify_rows_bad, verify_words_bad, verify_on ? "" : "; check stopped: reads in doubt", frames_marked, local_reads, local_writes, rbv_ifr, rbv_ier, rbv_regs[0x12],
            xp.serve ? "not answering, the board layer answers" : "answering", xp.reads, xp.reads_ff, xp.writes);
    if (scsi_empty) kprintf("[PSMAC:SE]   SCSI: the empty bus answered here, %u selections (none answered)\n", se5380.selections);
    if (psmac_board_plus) {
        psmac_plus_report();
        if (plus_drq_waits || plus_drq_timeouts) kprintf("[PSMAC:PLUS]   SCSI pseudo-DMA: %u bytes waited for DRQ (%u polls beyond the first), %u gave up\n", plus_drq_bytes, plus_drq_waits, plus_drq_timeouts);
    }
    if (!snd_off && (snd.starts || snd_dropped))
        kprintf("[PSMAC:SE]   sound: %u starts, %u samples played, %u halves written (%u late, %u held for the next VBL), %u underruns, %u dropped | speaker %s, volume %d, ASC mode %u | "
                "VBL %u edges (%u cleared here), frame %u us, a half written in %u us at most | buffer silenced %u times\n",
                snd.starts, snd.played, snd.halves, snd.late, snd.held, snd.underruns, snd_dropped, snd.on ? "on" : "off", snd.vol, asc_regs[0x801],
                snd.edges, snd.own_clears, (uint32_t)(snd.period * 1000000u / asc_hz()), snd.half_us_max, snd.quiets);
}

/* The IIci ROM's startup chime is the ASC's wavetable for 30000 steps of 14 VIA reads; the project's
   ROM patch (rom/patches/iici.json, docs/08 item 50) cut it to two steps, since nothing played the ASC
   on an SE or a bare Pi. On the SE with its speaker it plays again: about a second on the SE's VIA.
   Called with the ROM in the kernel's memory, once patched (psmac_bus.c); the checksum word follows,
   so the ROM's own test still passes. The patched bytes are checked first, the table around them
   ($40807158: volume $0204, the delay $0D, 300 steps per voice) as well. */
void psmac_se_rom_fixup(uint8_t *rom, uint32_t len)
{
    static const uint8_t table[] = { 0x02, 0x04, 0x00, 0x00, 0x00, 0x0d, 0x00, 0x00, 0x01, 0x2c, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04 };
    /* the SE with its speaker, or any board whose sound the Pi plays (psmac_audio.c, mac.audio) */
    if (!psmac_audio_wanted() && (!psmac_board_se || psmac_board_solo || snd_off)) return;
    if (len < 0x80000) return;
    for (unsigned i = 0; i < sizeof table; i++) if (rom[0x7158 + i] != table[i]) return;
    rom[0x7164] = 0x75; rom[0x7165] = 0x30;
    uint32_t sum = (uint32_t)rom[0] << 24 | (uint32_t)rom[1] << 16 | (uint32_t)rom[2] << 8 | rom[3];
    sum += 0x7530u - 0x0001u;
    rom[0] = sum >> 24; rom[1] = sum >> 16; rom[2] = sum >> 8; rom[3] = sum;
    kprintf("[PSMAC:SE] sound: the startup chime's full length given back to the ROM (30000 steps; checksum word now %08x)\n", sum);
}

/* mac.fpu=none (issue #28): the IIci ROM does not probe for its FPU. At $40800612 it copies HWCfgFlags
   ($B22) from the ProductInfo record UnivInfoPtr ($DD8) points to (measured: $408036C0, the IIci's), word
   +$10 = $DC00, whose bit 12 is hwCbFPU; Gestalt 'fpu ' (ROM $40816600) answers from that bit, and System 7
   then runs FPU code that would only take the line-F exception. Clear the bit there when there is no FPU,
   checked against the record's bytes first, with the ROM checksum word kept right (the start-up test
   compares it). The other ProductInfo records are not for this box and are left alone. */
void psmac_fpu_rom_fixup(uint8_t *rom, uint32_t len)
{
    static const uint8_t want[] = { 0xff, 0xff, 0xfd, 0xcc, 0x00, 0x00, 0x02, 0xaa, 0x00, 0x00, 0x02, 0xc2,
                                    0x00, 0x00, 0x03, 0x02, 0xdc, 0x00 };
    if (!globals.fpu_disable || len < 0x80000) return;
    for (unsigned i = 0; i < sizeof want; i++)
        if (rom[0x36c0 + i] != want[i]) {
            kprintf("[PSMAC] FPU: WARNING: mac.fpu=none, but the ROM's ProductInfo record at $36C0 is not the IIci's as known: "
                    "HWCfgFlags keeps hwCbFPU and Gestalt will report an FPU that traps\n");
            return;
        }
    rom[0x36d0] = 0xcc;   /* $DC00 -> $CC00: hwCbFPU (bit 12) clear */
    uint32_t sum = (uint32_t)rom[0] << 24 | (uint32_t)rom[1] << 16 | (uint32_t)rom[2] << 8 | rom[3];
    sum -= 0x1000u;
    rom[0] = sum >> 24; rom[1] = sum >> 16; rom[2] = sum >> 8; rom[3] = sum;
    kprintf("[PSMAC] FPU: the ROM's ProductInfo HWCfgFlags $DC00 -> $CC00 (hwCbFPU clear: no FPU for Gestalt; checksum word now %08x)\n", sum);
}

/* The SCSI Manager's selection timeout on the SE's own bus (22 November 2026, docs/08 item 101). The
   project's ROM patch (docs/08 item 50) cut it from Apple's 250 ms to 10 ms, so that every empty ID
   cost less at each scan; the testbed's 5380 answers a selection at once and never noticed. The owner's
   SE then saw none of its BlueSCSI's three disks: about 35,000 accesses to the 5380 in the scan and not
   one data transfer, the same after a warm restart (so not the BlueSCSI still starting). The timeout is
   counted in TimeSCSIDB loops of bus reads, which on the real SE are not the 68000's, and a target that
   answers late is missed at every ID. On the SE board with its real bus it is Apple's 250 ms again
   (mac.sescsiwait=MS for another value; an empty bus answered here, mac.sescsi=empty, keeps 10 ms).
   The bytes are checked first ("move.w #10,$62(a4)" at $40809554) and the checksum word follows. */
void psmac_se_scsi_fixup(uint8_t *rom, uint32_t len)
{
    static const uint8_t want[] = { 0x39, 0x7c, 0x00, 0x0a, 0x00, 0x62 };
    if (!psmac_board_se || psmac_board_solo || scsi_mode == 2 || len < 0x80000) return;
    for (unsigned i = 0; i < sizeof want; i++) if (rom[0x9554 + i] != want[i]) return;
    rom[0x9556] = (uint8_t)(scsi_wait_ms >> 8); rom[0x9557] = (uint8_t)scsi_wait_ms;
    uint32_t sum = (uint32_t)rom[0] << 24 | (uint32_t)rom[1] << 16 | (uint32_t)rom[2] << 8 | rom[3];
    sum += (uint32_t)scsi_wait_ms - 10u;
    rom[0] = sum >> 24; rom[1] = sum >> 16; rom[2] = sum >> 8; rom[3] = sum;
    kprintf("[PSMAC:SE] SCSI: selection timeout %d ms for the SE's own bus (the ROM patch's 10 ms is for the Pi alone; mac.sescsiwait=MS; checksum word now %08x)\n",
            scsi_wait_ms, sum);
}
