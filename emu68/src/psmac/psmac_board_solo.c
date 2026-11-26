/*
 * psmac_board_solo.c — no Macintosh at all: the kernel is the logic board.
 * Copyright (c) 2026 Aurora68 project. MPL-2.0.
 *
 * A Pi on the bench with an HDMI monitor and a disk image in its container boots System 7 from
 * that image and shows it on the second screen, with no SE behind it. The IIci ROM still thinks
 * it is on a IIci (psmac_board_se.c keeps answering the RBV, ASC and VDAC, and its identity
 * bits); the four chips that file forwards to the SE's bus are modelled here instead:
 *
 *   VIA1    the 6522: ports, both timers at 783.36 kHz, the shift register, interrupt flags; the
 *           60.15 Hz vertical blank on CA1 and the clock chip's one-second tick on CA2
 *   RTC     the clock / parameter RAM chip on port B bits 0-2 (MAME's macrtc.cpp is the model),
 *           256 bytes of PRAM seeded from what the testbed's IIci left behind, the seconds counter
 *   ADB     the transceiver state machine on port B bits 3-5 and the shift register (MAME's
 *           macadb.cpp): no devices yet, so every Talk times out the way an empty bus does
 *   SCC     a Z8530 register file that remembers what it is told; no ports, no interrupts
 *   IWM     a floppy controller with no drive: every sense line reads "nothing here"
 *   SCSI    an NCR 5380 as the initiator sees it, with the pseudo-DMA windows, and a direct-access
 *           target behind it for each disk image: INQUIRY, READ/WRITE 6 and 10, READ CAPACITY,
 *           MODE SENSE with Apple's page $30, REQUEST SENSE, the rest answered as good. Since 25 November
 *           2026 the 5380 and the targets are psmac_scsidev.c's, shared with the SE board's virtual SCSI
 *           devices (scsi.cd=, scsi.disk=; docs/08 item 148); this file keeps the images' list
 *
 * Selected with mac.board=solo (which implies the SE layer's address translation and the second
 * screen as the main screen). Disk images come from the container (*.img, *.hda, *.image, *.hfv)
 * in RAM, so writes last until the power goes, or in the testbed from host files (mac.scsi0=path).
 * Written 17 November 2026, 10:30, after the first Pi 3B stood alone on a desk with its WiFi up
 * and nothing to boot (docs/08, item 35).
 */
#include <stdint.h>
#include "support.h"
#include "tlsf.h"
#include "psmac.h"
#include "psmb.h"
#include "psmac_pram_default.h"
#include "psmac_scsidev.h"

int psmac_board_solo;                       /* mac.board=solo */
extern int psmac_board_se;

int64_t psmac_sh_open(const char *name, uint64_t mode);
int     psmac_sh_read(int64_t fd, void *buf, uint64_t len);
int     psmac_sh_write(int64_t fd, const void *buf, uint64_t len);
int64_t psmac_sh_seek(int64_t fd, uint64_t pos);
int64_t psmac_sh_flen(int64_t fd);
int     psmac_sh_available(void);
const char *psmac_option(const char *key);
int psmac_se_rbv_irq(void);

static inline uint64_t cnt_now(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0":"=r"(v)); return v; }
static inline uint64_t cnt_frq(void) { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0":"=r"(v)); return v; }

/* one lock for every chip: the JIT core (bus accesses) and the service core (time) both come here */
static volatile uint8_t solo_lock;
static inline void lock(void) { while (__atomic_test_and_set(&solo_lock, __ATOMIC_ACQUIRE)) __asm__ volatile("yield"); }
static inline void unlock(void) { __atomic_clear(&solo_lock, __ATOMIC_RELEASE); }

static uint32_t trace_left;                 /* mac.solotrace=N: SCSI commands, ADB commands, RTC commands */
#define TRACE(...) do { if (trace_left) { trace_left--; kprintf("[PSMAC:SOLO] " __VA_ARGS__); } } while (0)

/* ==== time: the VIA's clock ================================================================== */
#define VIA_HZ   783360u
#define VBL_TICKS 13023u                    /* 783360 / 60.15 */
static uint64_t t_base;
static uint64_t via_now(void)
{
    uint64_t d = cnt_now() - t_base, f = cnt_frq();
    return (d / f) * VIA_HZ + ((d % f) * VIA_HZ) / f;     /* no 128-bit division in a freestanding kernel */
}
static uint64_t us_to_ticks(uint32_t us) { return ((uint64_t)us * VIA_HZ) / 1000000u; }

/* ==== the clock chip ========================================================================== */
static struct {
    uint8_t pram[256];
    uint32_t seconds;                       /* since 1904, as the Mac counts */
    int enb, clk, data_in, data_out;
    uint8_t byte; int bits, dir;            /* the byte in flight; dir 1 = chip to host */
    uint8_t cmd; int state;                 /* 0 normal, 1 write data pending, 2 extended command, 3 extended write */
    int xpaddr, write_protect;
    uint64_t next_second;
    uint32_t cmds;
} rtc;

/* Parameter RAM across power cycles (19 November): a real Mac's chip has a battery, this one had
   the defaults at every power-on, so AppleTalk, the Network control panel's connection, Monitors'
   depth and the startup disk lasted only until the Pi was switched off. The card writer keeps it in
   pram.bin on the SD card (psmac_card.c): read at the card's first look, which the chip's first
   command waits for (a few tens of milliseconds at most after the 68k starts), and written back a
   second after the Mac's last change, once the boot is over. mac.appletalk= and mac.xpram= still
   apply on top of what the card held. */
static volatile uint32_t pram_gen, pram_saved_gen;      /* changes made, changes handed to the card */
static volatile uint64_t pram_changed_at;
static int pram_loaded;
static void pram_overrides(void);
static void pram_set(int addr, uint8_t v)
{
    if (rtc.pram[addr & 0xff] == v) return;
    rtc.pram[addr & 0xff] = v;
    pram_gen++; pram_changed_at = cnt_now();
}
static void pram_from_card(void)
{
    extern const uint8_t *psmac_card_pram(int *state, uint32_t *len);
    int st = 0; uint32_t len = 0; const uint8_t *p = 0;
    uint64_t t0 = cnt_now();
    while ((p = psmac_card_pram(&st, &len)) == 0 && st == 0 && cnt_now() - t0 < 3 * cnt_frq()) __asm__ volatile("yield");
    pram_loaded = 1;
    if (!p || len < 256) { kprintf("[PSMAC:SOLO] parameter RAM: the defaults (no pram.bin on the card)\n"); return; }
    for (int i = 0; i < 256; i++) rtc.pram[i] = p[i];
    if (len >= 260) {                        /* the clock as it was at the last save: better than 17 November */
        uint32_t sec = (uint32_t)p[256] << 24 | (uint32_t)p[257] << 16 | (uint32_t)p[258] << 8 | p[259];
        if (sec > rtc.seconds) rtc.seconds = sec;
    }
    pram_overrides();
    pram_saved_gen = pram_gen;
    kprintf("[PSMAC:SOLO] parameter RAM from pram.bin on the card (%u bytes)\n", len);
}
/* core 3 (psmac_card.c): the parameter RAM and the clock to save, once a second has passed since
   the Mac's last change and the boot is over; 0 when there is nothing to save */
int psmac_solo_pram_take(uint8_t out[260])
{
    extern int psmac_boot_settled(void);
    if (!psmac_board_solo || pram_gen == pram_saved_gen || cnt_now() - pram_changed_at < cnt_frq() || !psmac_boot_settled()) return 0;
    lock();
    uint32_t g = pram_gen;
    for (int i = 0; i < 256; i++) out[i] = rtc.pram[i];
    out[256] = rtc.seconds >> 24; out[257] = rtc.seconds >> 16; out[258] = rtc.seconds >> 8; out[259] = rtc.seconds;
    unlock();
    pram_saved_gen = g;
    return 1;
}

static void rtc_execute(uint8_t data)
{
    if (!pram_loaded) pram_from_card();
    rtc.cmds++;
    if (rtc.state == 2) {                   /* extended: second byte carries the low address bits */
        rtc.xpaddr = ((rtc.cmd & 7) << 5) | ((data & 0x7c) >> 2);
        if (rtc.cmd & 0x80) {
            extern uint8_t psmac_startup_read(int addr, uint8_t real);   /* $78-$7B: slot 9's PiDisk while an image may start the Mac (psmac_mbox.c) */
            rtc.dir = 1; rtc.byte = psmac_startup_read(rtc.xpaddr, rtc.pram[rtc.xpaddr]); rtc.state = 0;
        }
        else { rtc.state = 3; rtc.byte = 0; rtc.bits = 0; }
        return;
    }
    if (rtc.state == 3) { extern int psmac_startup_write(int addr, uint8_t v); if (!psmac_startup_write(rtc.xpaddr, data)) pram_set(rtc.xpaddr, data); rtc.state = 0; return; }
    if (rtc.state == 1) {
        rtc.state = 0;
        int i = (rtc.cmd >> 2) & 0x1f;
        if (rtc.write_protect && i != 13) return;
        if (i < 4) { rtc.seconds = (rtc.seconds & ~(0xffu << (8 * i))) | ((uint32_t)data << (8 * i)); }
        else if (i >= 8 && i < 12) pram_set(i, data);
        else if (i == 12) { /* test register */ }
        else if (i == 13) rtc.write_protect = (data & 0x80) != 0;
        else if (i >= 16) pram_set(i, data);
        return;
    }
    rtc.cmd = data;
    if ((rtc.cmd & 0x78) == 0x38) { rtc.state = 2; rtc.byte = 0; rtc.bits = 0; return; }
    if (rtc.cmd & 0x80) {
        int i = (rtc.cmd >> 2) & 0x1f;
        rtc.dir = 1;
        if (i < 8) rtc.byte = (rtc.seconds >> (8 * (i & 3))) & 0xff;
        else if (i >= 8 && i < 12) rtc.byte = rtc.pram[i];
        else if (i >= 16) rtc.byte = rtc.pram[i];
        else rtc.byte = 0;
    } else { rtc.state = 1; rtc.byte = 0; rtc.bits = 0; }
}
/* port B as the ROM drives it: bit 2 enable (low = listening), bit 1 clock, bit 0 data */
static void rtc_lines(int enb, int clk, int data)
{
    if (enb != rtc.enb) { rtc.enb = enb; rtc.byte = 0; rtc.bits = 0; rtc.dir = 0; rtc.data_out = 0; rtc.state = 0; }
    rtc.data_in = data;
    if (!clk && rtc.clk && !rtc.enb) {      /* falling edge shifts */
        if (rtc.dir) { if (rtc.bits > 0) rtc.bits--; rtc.data_out = (rtc.byte >> rtc.bits) & 1; }
        else { rtc.byte = (rtc.byte << 1) | (data ? 1 : 0); if (++rtc.bits == 8) { rtc_execute(rtc.byte); if (rtc.dir) rtc.bits = 8; else { rtc.byte = 0; rtc.bits = 0; } } }
    }
    rtc.clk = clk;
}

/* ==== the VIA ================================================================================ */
enum { IFR_CA2 = 0x01, IFR_CA1 = 0x02, IFR_SR = 0x04, IFR_CB2 = 0x08, IFR_CB1 = 0x10, IFR_T2 = 0x20, IFR_T1 = 0x40 };
static struct {
    uint8_t orb, ora, ddrb, ddra, acr, pcr, ifr, ier, sr;
    uint16_t t1l, t2l;
    uint64_t t1_start, t2_start; int t1_run, t2_run, t1_fired; uint64_t t1_seen;
    uint64_t next_vbl;
    uint32_t vbls, irqs;
} via;

/* ---- the ADB transceiver, as the ROM sees it through the VIA (macadb.cpp's model) ---- */
enum { ADB_NEW_COMMAND = 0, ADB_XFER_EVEN = 1, ADB_XFER_ODD = 2, ADB_IDLE = 3 };
static struct {
    int state, irq;                         /* irq: the transceiver's interrupt line, 1 = asserted (PB3 reads 0) */
    int direction;                          /* 1 = the Mac is shifting out, 0 = a device would shift in */
    int datasize; uint8_t buffer[8]; int waiting_cmd, last_talk;
    int pending; uint64_t due; uint8_t send;
    uint32_t cmds, talks;
} adb;

static void adb_talk(uint8_t cmd)
{
    int addr = cmd >> 4, kind = (cmd >> 2) & 3;
    adb.cmds++;
    TRACE("ADB command %02x (addr %d %s reg %d): no devices\n", cmd, addr, kind == 3 ? "talk" : kind == 2 ? "listen" : "reset/flush", cmd & 3);
    adb.direction = 0; adb.datasize = 0;
    if (kind == 3) { adb.last_talk = addr; adb.talks++; }
    adb.waiting_cmd = 0;
}
static void adb_newstate(int s)
{
    if (s == adb.state) return;
    adb.state = s;
    switch (s) {
    case ADB_NEW_COMMAND:
        adb.direction = 1; adb.waiting_cmd = 1; adb.irq = 0;
        adb.pending = 1; adb.due = via_now() + us_to_ticks(800);
        break;
    case ADB_XFER_EVEN: case ADB_XFER_ODD:
        if (adb.datasize > 0 && !adb.direction) { adb.send = adb.buffer[0]; adb.datasize--; for (int i = 0; i < adb.datasize; i++) adb.buffer[i] = adb.buffer[i + 1]; }
        else { adb.send = 0; adb.irq = 1; }          /* nothing to say: the line the ROM reads as "no response" */
        adb.pending = 1; adb.due = via_now() + us_to_ticks(800);
        break;
    case ADB_IDLE:
        adb.irq = 0; adb.pending = 0;
        break;
    }
}
static void adb_tick(uint64_t now)
{
    if (!adb.pending || now < adb.due) return;
    adb.pending = 0;
    if (adb.state == ADB_NEW_COMMAND) { adb_talk(via.sr); via.ifr |= IFR_SR; }
    else if (adb.state == ADB_XFER_EVEN || adb.state == ADB_XFER_ODD) { if (!adb.direction) via.sr = adb.send; via.ifr |= IFR_SR; }
}

static void via_timers(uint64_t now)
{
    if (via.t1_run) {
        uint64_t el = now - via.t1_start, period = (uint64_t)via.t1l + 2;
        if (via.acr & 0x40) { uint64_t n = el / period; if (n > via.t1_seen) { via.ifr |= IFR_T1; via.t1_seen = n; } }
        else if (!via.t1_fired && el >= period) { via.ifr |= IFR_T1; via.t1_fired = 1; }
    }
    if (via.t2_run && !(via.acr & 0x20)) {
        uint64_t el = now - via.t2_start;
        if (el >= (uint64_t)via.t2l + 2) { via.ifr |= IFR_T2; via.t2_run = 0; }
    }
    while (now >= via.next_vbl) { via.ifr |= IFR_CA1; via.next_vbl += VBL_TICKS; via.vbls++; }
    while (now >= rtc.next_second) { via.ifr |= IFR_CA2; rtc.next_second += VIA_HZ; rtc.seconds++; }
    adb_tick(now);
}
static uint16_t via_t1(uint64_t now)
{
    if (!via.t1_run) return via.t1l;
    uint64_t el = now - via.t1_start, period = (uint64_t)via.t1l + 2;
    if (via.acr & 0x40) return (uint16_t)(via.t1l - (el % period));
    return (uint16_t)(via.t1l - el);
}
static uint16_t via_t2(uint64_t now)
{
    if (!via.t2_start) return via.t2l;
    return (uint16_t)(via.t2l - (now - via.t2_start));
}
static int via_irq(void) { return (via.ifr & via.ier & 0x7f) != 0; }

static void via_apply_portb(void)
{
    /* outputs the ROM drives: the clock chip's three lines (only the bits set as outputs count) */
    uint8_t out = via.orb | ~via.ddrb;          /* an input line floats high */
    rtc_lines((out >> 2) & 1, (out >> 1) & 1, out & 1);
    if (via.ddrb & 0x30) adb_newstate((via.orb >> 4) & 3);
}
static uint8_t via_portb_in(void)
{
    uint8_t in = 0xff;
    if (!rtc.data_out) in &= ~0x01;
    if (adb.irq) in &= ~0x08;
    return in;
}
static uint32_t via_read(uint32_t reg)
{
    uint64_t now = via_now(); via_timers(now);
    uint32_t r;
    switch (reg) {
    case 0:  via.ifr &= ~(IFR_CB1 | IFR_CB2); r = (via.orb & via.ddrb) | (via_portb_in() & ~via.ddrb); break;
    case 1:  via.ifr &= ~IFR_CA1; if ((via.pcr & 0x0a) != 0x02) via.ifr &= ~IFR_CA2; /* fall through */
    case 15: r = ((via.ora & via.ddra) & 0x38) | 0xc7; break;   /* the IIci's identity (psmac_board_se.c), SCC W/REQ idle */
    case 2:  r = via.ddrb; break;
    case 3:  r = via.ddra; break;
    case 4:  via.ifr &= ~IFR_T1; r = via_t1(now) & 0xff; break;
    case 5:  r = via_t1(now) >> 8; break;
    case 6:  r = via.t1l & 0xff; break;
    case 7:  r = via.t1l >> 8; break;
    case 8:  via.ifr &= ~IFR_T2; r = via_t2(now) & 0xff; break;
    case 9:  r = via_t2(now) >> 8; break;
    case 10: via.ifr &= ~IFR_SR; r = via.sr; break;
    case 11: r = via.acr; break;
    case 12: r = via.pcr; break;
    case 13: r = (via.ifr & 0x7f) | (via_irq() ? 0x80 : 0); break;
    case 14: r = via.ier | 0x80; break;
    default: r = 0; break;
    }
    return r;
}
static void via_write(uint32_t reg, uint8_t d)
{
    uint64_t now = via_now(); via_timers(now);
    switch (reg) {
    case 0:  via.ifr &= ~(IFR_CB1 | IFR_CB2); via.orb = d; via_apply_portb(); break;
    case 1:  via.ifr &= ~IFR_CA1; if ((via.pcr & 0x0a) != 0x02) via.ifr &= ~IFR_CA2; /* fall through */
    case 15: via.ora = d; break;
    case 2:  via.ddrb = d; via_apply_portb(); break;
    case 3:  via.ddra = d; break;
    case 4: case 6: via.t1l = (via.t1l & 0xff00) | d; break;
    case 5:  via.t1l = (via.t1l & 0x00ff) | (d << 8); via.t1_start = now; via.t1_run = 1; via.t1_fired = 0; via.t1_seen = 0; via.ifr &= ~IFR_T1; break;
    case 7:  via.t1l = (via.t1l & 0x00ff) | (d << 8); via.ifr &= ~IFR_T1; break;
    case 8:  via.t2l = (via.t2l & 0xff00) | d; break;
    case 9:  via.t2l = (via.t2l & 0x00ff) | (d << 8); via.t2_start = now ? now : 1; via.t2_run = 1; via.ifr &= ~IFR_T2; break;
    case 10: via.ifr &= ~IFR_SR; via.sr = d; break;
    case 11: via.acr = d; break;
    case 12: via.pcr = d; break;
    case 13: via.ifr &= ~(d & 0x7f); break;
    case 14: if (d & 0x80) via.ier |= d & 0x7f; else via.ier &= ~(d & 0x7f); break;
    }
}

/* ==== the SCC: a register file, and a transmitter that finishes ===========================
   Nothing is plugged into the ports, so nothing is ever received. What is sent goes nowhere, but
   the transmitter has to say it went. AppleTalk's LocalTalk driver (System 7.1's .MPP, active
   once AppleTalk is switched on in the Chooser) sends its address claims in SDLC and runs as a
   coroutine on the channel's interrupts. Each byte goes out on a transmit-buffer-empty interrupt
   (WR1 bit 1), and the frame ends on the external/status interrupt for transmit underrun/EOM
   (WR15 bit 6, RR0 bit 6). With a register file that never interrupted, the Mac waited for that
   underrun for ever: a boot that stopped at "Welcome to Macintosh", and the Network control panel
   that froze the user's Pi, 18 November. Now:
   - a data write empties the transmit buffer at once and, with transmit interrupts on, sets the
     channel's Tx IP;
   - when no byte has followed for 50 us, the transmitter underruns, RR0's underrun/EOM bit
     comes up and, with WR15 bit 6 and WR1 bit 0, so does the external/status IP;
   - WR0 resets them (reset Tx IP, reset ext/status, reset Tx underrun/EOM latch) as the chip's
     commands do;
   - RR3 (channel A) shows the pending bits, RR2 read through channel B gives the vector modified
     by the highest one (WR9 VIS, status low);
   - the line goes to level 4 while WR9 MIE is set and something is pending.
   In SDLC mode RR0 also says "hunt": no frame is coming, which is what an empty network looks
   like. With nobody answering its claims, LocalTalk takes its node number and carries on. */
static struct {
    uint8_t wr[2][16], ptr[2];
    uint8_t ip;                     /* RR3 layout: 0 B ext, 1 B Tx, 2 B Rx, 3 A ext, 4 A Tx, 5 A Rx */
    uint8_t eom[2];                 /* the Tx underrun/EOM latch (RR0 bit 6): 1 = underrun reported */
    uint8_t txbusy[2];              /* a byte written since the last underrun */
    uint64_t txt[2];
    uint32_t writes, irqs;
} scc;
static inline uint64_t scc_now(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v)); return v; }
static inline uint64_t scc_hz(void)  { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v; }
static int scc_sdlc(int ch) { return (scc.wr[ch][4] & 0x0c) == 0 && (scc.wr[ch][4] & 0x30) == 0x20; }
/* ch 0 = B, 1 = A: the bit of a source in RR3 */
static inline uint8_t scc_bit(int ch, int src) { return (uint8_t)(1u << (src + (ch ? 3 : 0))); }
static void scc_tick(void)
{
    for (int ch = 0; ch < 2; ch++) {
        if (scc.txbusy[ch] && scc_now() - scc.txt[ch] > scc_hz() / 20000) {
            scc.txbusy[ch] = 0;
            if (!scc.eom[ch]) {
                scc.eom[ch] = 1;
                if ((scc.wr[ch][15] & 0x40) && (scc.wr[ch][1] & 0x01))
                    scc.ip |= scc_bit(ch, 0);
            }
        }
    }
}
static int scc_irq(void)
{
    scc_tick();
    return (scc.wr[1][9] & 0x08) && scc.ip;
}
static uint8_t scc_rr0(int ch)
{
    uint8_t r = 0x2c;                           /* Tx empty, DCD, CTS: an idle port */
    if (scc.eom[ch] || !scc.txbusy[ch]) r |= 0x40;
    if (scc_sdlc(ch)) r |= 0x10;                /* hunt: no frame arriving */
    return r;
}
static uint8_t scc_rr2b(void)
{
    /* the highest pending source: A Rx, A Tx, A ext, B Rx, B Tx, B ext; none = 011 */
    static const uint8_t code[6] = { 1, 0, 2, 5, 4, 6 };     /* by RR3 bit: B ext 001, B Tx 000, B Rx 010, A ext 101, A Tx 100, A Rx 110 */
    uint8_t v = scc.wr[1][2], st = 3;
    for (int b = 5; b >= 0; b--)
        if (scc.ip & (1u << b)) { st = code[b]; break; }
    if (scc.wr[1][9] & 0x01) {
        if (scc.wr[1][9] & 0x10) v = (uint8_t)((v & 0x8f) | (uint8_t)((st & 1) << 6 | (st & 2) << 4 | (st & 4) << 2));   /* status high: bits 4-6, reversed */
        else v = (uint8_t)((v & 0xf1) | (uint8_t)(st << 1));
    }
    return v;
}
static uint32_t scc_read(uint32_t off)
{
    int ch = (off & 2) ? 1 : 0;                /* +0 B control, +2 A control, +4 B data, +6 A data; index 0 = B, 1 = A */
    if (off & 4) return 0;                      /* data: nothing received */
    int p = scc.ptr[ch]; scc.ptr[ch] = 0;
    scc_tick();
    switch (p) {
    case 0:  return scc_rr0(ch);
    case 1:  return 0x07;                       /* all sent, residue codes */
    case 2:  return ch ? scc.wr[1][2] : scc_rr2b();
    case 3:  return ch ? scc.ip : 0;
    case 10: return 0;
    case 12: case 13: case 15: return scc.wr[ch][p];
    default: return 0;
    }
}
static void scc_write(uint32_t off, uint8_t d)
{
    int ch = (off & 2) ? 1 : 0;
    scc.writes++;
    if (off & 4) {                              /* data: sent nowhere, at once */
        scc.txbusy[ch] = 1;
        scc.txt[ch] = scc_now();
        if (scc.wr[ch][1] & 0x02)
            scc.ip |= scc_bit(ch, 1);
        return;
    }
    if (scc.ptr[ch] == 0) {
        switch (d & 0x38) {
        case 0x10: scc.ip &= (uint8_t)~scc_bit(ch, 0); break;         /* reset ext/status interrupts */
        case 0x28: scc.ip &= (uint8_t)~scc_bit(ch, 1); break;         /* reset Tx int pending */
        }
        if ((d & 0xc0) == 0xc0) scc.eom[ch] = 0;                      /* reset Tx underrun/EOM latch */
        int p = d & 7; if ((d & 0x38) == 0x08) p += 8;
        if (p) scc.ptr[ch] = p;
        return;
    }
    scc.wr[ch][scc.ptr[ch]] = d;
    if (scc.ptr[ch] == 9) {
        scc.wr[0][9] = d; scc.wr[1][9] = d;
        if (d & 0xc0) {                         /* channel or hardware reset: nothing pending */
            if ((d & 0xc0) == 0xc0) scc.ip = 0;
            else scc.ip &= (d & 0x80) ? 0x07 : 0x38;
        }
    }
    if (scc.ptr[ch] == 1 && !(d & 0x02)) scc.ip &= (uint8_t)~scc_bit(ch, 1);     /* Tx interrupts off */
    scc.ptr[ch] = 0;
}

/* ==== the IWM: no drive ======================================================================= */
static struct { uint16_t lines; uint8_t mode; } iwm;
static uint32_t iwm_access(uint32_t reg, int write, uint8_t d)
{
    if (reg & 1) iwm.lines |= 1u << (reg >> 1); else iwm.lines &= ~(1u << (reg >> 1));
    int q6 = (iwm.lines >> 6) & 1, q7 = (iwm.lines >> 7) & 1, motor = (iwm.lines >> 4) & 1;
    if (write) { if (q7 && q6 && !motor) iwm.mode = d & 0x1f; return 0; }
    if (!q7 && q6) return 0x80 | (motor ? 0x20 : 0) | iwm.mode;   /* status: sense line high (no drive), mode */
    if (q7 && q6) return 0xc0;                                    /* handshake: ready, no underrun */
    return 0xff;                                                  /* data: nothing */
}

/* ==== disk images ============================================================================= */
/* the images' list: the Mac's disks, a SCSI target each (psmac_scsidev.c) or a PiDisk volume */
struct disk { char name[48]; uint8_t *ram; int64_t fd; uint32_t blocks; int writable, scsi; int card; };   /* card: an image served from the card (psmac_card.c), its handle + 1 */
static struct disk disks[7]; static int ndisk;

/* The first disk image is SCSI ID 0, the startup disk. Further images are PiDisk volumes (the
   slot-9 card's own disk driver, psmac_mbox.c), which mount themselves after the Finder starts:
   as SCSI disks they need their own driver from their own driver partition, and the one on the
   user's images, Iomega's Zip driver, loads, reads its partition map and never mounts a second
   drive (19 November, docs/08 item 52). mac.disks=scsi makes every image a SCSI disk as before.
   An ID a virtual device holds (scsi.cd=, scsi.disk=, psmac_scsidev.c) is not given to an image: that
   image is a PiDisk volume instead. */
static int disks_scsi = -1;
int psmac_image_is_scsi(int i) { return i >= 0 && i < ndisk && disks[i].scsi; }
static void disk_publish(struct disk *d, int id)
{
    if (!d->scsi) return;
    if (psmac_scsidev_id_taken(id)) { d->scsi = 0; kprintf("[PSMAC:SOLO] disk image %s: SCSI ID %d is a virtual device's (scsi.cd=, scsi.disk=): a PiDisk volume instead\n", d->name, id); return; }
    psmac_scsidev_add_image(id, d->name, d->ram, d->fd, d->card, (uint64_t)d->blocks * 512u, d->writable);
}
void psmac_image_add(const char *name, uint8_t *data, uint32_t len)
{
    if (ndisk >= 7 || len < 1024) return;
    if (disks_scsi < 0) { const char *o = psmac_option("mac.disks"); disks_scsi = o && o[0] == 's'; }
    struct disk *d = &disks[ndisk];
    int k = 0; while (name[k] && k < 47) { d->name[k] = name[k]; k++; } d->name[k] = 0;
    d->ram = data; d->fd = -1; d->card = 0; d->blocks = len / 512; d->writable = 1;
    d->scsi = !psmac_board_solo || disks_scsi || ndisk == 0;
    if (psmac_board_solo) disk_publish(d, ndisk);
    __atomic_store_n(&ndisk, ndisk + 1, __ATOMIC_RELEASE);   /* complete before it is seen: a disk image on the card arrives from core 3 (psmac_card.c) */
}
/* a disk image served from the card as the Mac asks (psmac_card.c, core 3, 24 November 2026): read and written
   through the card's cache, a SCSI command's blocks waited for as a disk's are */
void psmac_image_add_card(const char *name, int handle, uint32_t len, int writable)
{
    if (ndisk >= 7 || len < 1024) return;
    if (disks_scsi < 0) { const char *o = psmac_option("mac.disks"); disks_scsi = o && o[0] == 's'; }
    struct disk *d = &disks[ndisk];
    int k = 0; while (name[k] && k < 47) { d->name[k] = name[k]; k++; } d->name[k] = 0;
    d->ram = 0; d->fd = -1; d->card = handle + 1; d->blocks = len / 512; d->writable = writable;
    d->scsi = !psmac_board_solo || disks_scsi || ndisk == 0;
    if (psmac_board_solo) disk_publish(d, ndisk);
    __atomic_store_n(&ndisk, ndisk + 1, __ATOMIC_RELEASE);
}
int psmac_image_count(void) { return ndisk; }
/* name and size of image i; data its memory (0 for a host file or an image served from the card); 1 when it is
   one in memory, the only kind psmac_mbox_init serves itself */
int psmac_image_get(int i, const char **name, uint8_t **data, uint32_t *blocks)
{
    if (i < 0 || i >= ndisk) return 0;
    *name = disks[i].name; *data = disks[i].ram; *blocks = disks[i].blocks;
    return disks[i].ram != 0;
}
static void disk_add_file(const char *path)
{
    if (ndisk >= 7 || !psmac_sh_available()) return;
    int64_t fd = psmac_sh_open(path, 3); int writable = 1;
    if (fd < 0) { fd = psmac_sh_open(path, 1); writable = 0; }
    if (fd < 0) { kprintf("[PSMAC:SOLO] cannot open disk image %s\n", path); return; }
    struct disk *d = &disks[ndisk];
    const char *base = path; for (const char *s = path; *s; s++) if (*s == '/') base = s + 1;
    int k = 0; while (base[k] && k < 47) { d->name[k] = base[k]; k++; } d->name[k] = 0;
    d->ram = 0; d->fd = fd; d->card = 0; d->blocks = (uint32_t)(psmac_sh_flen(fd) / 512); d->writable = writable; d->scsi = 1;
    disk_publish(d, ndisk);
    ndisk++;
}

/* ==== the bus layer's entry: the four chips the SE layer would forward ======================== */
/* A real VIA drops its IRQ output the moment the handler clears the flag; the level the JIT sees
   (INTF.IPL) was only brought up to date by the service core's poll, up to a millisecond later.
   In that millisecond the 68k ran on with the old level standing, and any instruction that
   lowered the mask took an interrupt that was no longer there. System 7 has such a place: the
   level-1 handler's call to the Deferred Task Manager ($6E4) goes through a patch that clears
   the mask before it returns to the handler's RTE. With deferred tasks pending and the Deferred
   Task Manager busy (a task running at level 0, interrupted), every pass took the stale level 1
   again, one level deeper each time, 44 bytes of stack each, until the stack ran through the
   heap below it: Speedometer's Run ALL Tests ended in "bad F-Line instruction" in the Finder
   (docs/08, item 48). So every access to a chip sets the level it now drives, under the lock, as
   the service core's poll does, so that neither can store a level the other has just made stale. */
extern struct M68KState *__m68k_state;
unsigned psmac_solo_ipl(void);
static void set_level(void)                 /* the caller holds the lock */
{
    if (__m68k_state) PSMAC_STORE_IPL(__m68k_state, psmac_solo_ipl());
}
void psmac_solo_level_now(void)             /* for the RBV in psmac_board_se.c */
{
    lock(); set_level(); unlock();
}

int psmac_solo_access(uint32_t addr, int size, int write, uint32_t data, uint32_t *val)
{
    if ((addr & 0xff000000u) != 0x50000000u) return 0;
    uint32_t off = addr & 0x000fffffu, r = 0;
    int handled = 1;
    lock();
    if (off < 0x2000 || (off >= 0x40000 && off < 0x42000)) {
        uint32_t reg = (off >> 9) & 15;
        if (write) via_write(reg, data & 0xff); else r = via_read(reg);
    } else if (off >= 0x4000 && off < 0x6000) {
        if (write) scc_write(off & 6, data & 0xff); else r = scc_read(off & 6);
    } else if (off >= 0x10000 && off < 0x12000) {
        uint32_t reg = (off >> 4) & 7;
        if (write) psmac_scsidev_write(reg, data & 0xff); else r = psmac_scsidev_read(reg);
    } else if ((off >= 0x12000 && off < 0x14000) || (off >= 0x6000 && off < 0x8000)) {
        if (write) { for (int i = 0; i < size; i++) psmac_scsidev_pdma_write((data >> (8 * (size - 1 - i))) & 0xff); }
        else { for (int i = 0; i < size; i++) r = (r << 8) | psmac_scsidev_pdma_read(); }
        set_level();
        unlock();
        *val = r;
        return 1;
    } else if (off >= 0x16000 && off < 0x18000) {
        uint32_t reg = (off >> 9) & 15;
        r = iwm_access(reg, write, data & 0xff);
    } else handled = 0;
    if (handled) set_level();
    unlock();
    if (!handled) return 0;
    if (!write) *val = size == 1 ? r : size == 2 ? (r << 8) | r : (r << 24) | (r << 16) | (r << 8) | r;
    return 1;
}

/* the 5380's interrupt line, for the RBV's flag bit 3 (psmac_board_se.c): the ROM's SCSI Manager
   enables that bit and waits for the level-2 interrupt at the end of every transfer; without it
   each command completed on a timeout and System 7 took a second per block (docs/08, item 35) */
int psmac_solo_scsi_irq(void) { return psmac_scsidev_irq(); }

/* the level the board drives: the VIA on 1, the RBV (psmac_board_se.c) on 2, the SCC on 4 */
unsigned psmac_solo_ipl(void)
{
    unsigned ipl = via_irq() ? 1 : 0;
    if (psmac_se_rbv_irq() && ipl < 2) ipl = 2;
    if (scc_irq()) { ipl = 4; scc.irqs++; }
    return ipl;
}

/* the service core: time passes for the chips, the JIT learns the level */
void psmac_solo_service(void)
{
    if (!psmac_board_solo) return;
    lock(); via_timers(via_now()); set_level(); unlock();
}
void psmac_solo_reset(void)
{
    lock();
    uint64_t now = via_now();
    via.orb = via.ora = via.ddrb = via.ddra = via.acr = via.pcr = via.ifr = via.ier = 0; via.t1_run = via.t2_run = 0;
    via.next_vbl = now + VBL_TICKS; rtc.next_second = now + VIA_HZ;
    adb.state = ADB_IDLE; adb.irq = 0; adb.pending = 0; adb.datasize = 0;
    for (int c = 0; c < 2; c++) { for (int i = 0; i < 16; i++) scc.wr[c][i] = 0; scc.ptr[c] = 0; scc.eom[c] = 1; scc.txbusy[c] = 0; }
    scc.ip = 0;
    iwm.lines = 0;
    psmac_scsidev_reset();
    unlock();
}
void psmac_solo_init(void)
{
    t_base = cnt_now();
    for (int i = 0; i < 256; i++) rtc.pram[i] = psmac_pram_default[i];
    rtc.seconds = 0xe7214980u;              /* 17 November 2026, 00:00, in the Mac's seconds since 1904 */
    pram_loaded = 0;                         /* pram.bin from the card at the chip's first command */
    pram_overrides();
    rtc.enb = 1;
    psmac_scsidev_init(1);                   /* the virtual SCSI devices first: the IDs they hold are not given to an image */
    static const char *const keys[] = { "mac.scsi0", "mac.scsi1", "mac.scsi2", "mac.scsi3", "mac.scsi4", "mac.scsi5", "mac.scsi6" };
    for (int i = 0; i < 7; i++) { const char *p = psmac_option(keys[i]); if (p) disk_add_file(p); }
    psmac_solo_reset();
    kprintf("[PSMAC:SOLO] no Macintosh: VIA, RTC/PRAM, ADB (no devices), SCC, IWM (no drive) and an NCR 5380 answered here\n");
}
static void pram_overrides(void)
{
    /* mac.appletalk=1: AppleTalk active from the start (SPConfig, PRAM $13, low nibble 1 = the
       printer port is AppleTalk's), whatever the defaults or pram.bin on the card say */
    { const char *a = psmac_option("mac.appletalk"); if (a && a[0] == '1') rtc.pram[0x13] = (uint8_t)((rtc.pram[0x13] & 0xf0) | 1); }
    /* mac.xpram=ADDR:HEXBYTES[,ADDR:HEXBYTES]: preset extended PRAM, for testing what the
       control panels would store (e0:098100a8 = the AppleTalk connection: slot 9, sResource
       $81, 'atlk' 168, the LAP Manager's long at $E0) */
    { const char *x = psmac_option("mac.xpram");
      while (x && *x && *x != ' ') {
          unsigned addr = 0; int nib = -1;
          while (*x && *x != ':' && *x != ' ') { char c = *x++; addr = addr * 16 + (unsigned)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10); }
          if (*x != ':') break;
          x++;
          while (*x && *x != ',' && *x != ' ') {
              char c = *x++; int v = c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
              if (nib < 0) nib = v;
              else { rtc.pram[addr++ & 0xff] = (uint8_t)(nib << 4 | v); nib = -1; }
          }
          if (*x == ',') x++;
      } }
}
/* from psmac_setup_memory once the container has been read: the disks the Mac will find */
void psmac_solo_start(void)
{
    if (!psmac_board_solo) return;
    kprintf("[PSMAC:SOLO] %d SCSI disk%s%s\n", ndisk, ndisk == 1 ? "" : "s", ndisk ? ":" : " (put a *.img, *.hda or *.image file in the container, or on the card beside it)");
    for (int i = 0; i < ndisk; i++) kprintf("[PSMAC:SOLO]   %s %d: %s, %u blocks (%u MB)%s%s\n", disks[i].scsi ? "SCSI" : "PiDisk volume, image", i, disks[i].name, disks[i].blocks, disks[i].blocks / 2048, disks[i].ram ? ", in RAM (changes last until power off)" : ", host file", disks[i].writable ? "" : ", read-only");
}
void psmac_solo_options(const char *args)
{
    extern const char *find_token(const char *, const char *);
    const char *tok;
    if ((tok = find_token(args, "mac.board="))) { tok += 10; if (tok[0] == 's' && tok[1] == 'o' && tok[2] == 'l' && tok[3] == 'o') { psmac_board_solo = 1; psmac_board_se = 1; } }
    if ((tok = find_token(args, "mac.solotrace="))) { tok += 14; uint32_t v = 0; while (*tok >= '0' && *tok <= '9') v = v * 10 + (*tok++ - '0'); trace_left = v; }
}
void psmac_solo_report(void)
{
    if (!psmac_board_solo) return;
    kprintf("[PSMAC:SOLO] VIA IFR %02x IER %02x ACR %02x T1 %s T2 %s, %u VBLs | ADB state %d %u cmds | RTC %u cmds secs %08x\n",
            via.ifr, via.ier, via.acr, via.t1_run ? "on" : "off", via.t2_run ? "on" : "off", via.vbls, adb.state, adb.cmds, rtc.cmds, rtc.seconds);
    /* every disk, and how often each ID was selected: a second disk the Mac never shows (the user's
       RiptideFix.img, 19 November) was invisible here when only disk 0 was counted (psmac_scsidev.c's lines) */
    psmac_scsidev_report();
    kprintf("[PSMAC:SOLO]   SCC: %u writes, %u interrupts, pending %02x; A (modem) WR1 %02x WR3 %02x WR4 %02x WR5 %02x WR9 %02x WR10 %02x WR11 %02x WR12 %02x WR14 %02x WR15 %02x | B (printer) WR1 %02x WR3 %02x WR4 %02x WR5 %02x WR10 %02x WR11 %02x WR12 %02x WR14 %02x WR15 %02x\n",
            scc.writes, scc.irqs, scc.ip, scc.wr[1][1], scc.wr[1][3], scc.wr[1][4], scc.wr[1][5], scc.wr[1][9], scc.wr[1][10], scc.wr[1][11], scc.wr[1][12], scc.wr[1][14], scc.wr[1][15],
            scc.wr[0][1], scc.wr[0][3], scc.wr[0][4], scc.wr[0][5], scc.wr[0][10], scc.wr[0][11], scc.wr[0][12], scc.wr[0][14], scc.wr[0][15]);
}
