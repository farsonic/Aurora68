/*
 * psmac_board_plus.c — the Macintosh Plus's logic board behind the Macintosh IIci ROM.
 * Copyright (c) 2026 Aurora68 project. MPL-2.0.
 *
 * mac.board=plus is the SE board layer (psmac_board_se.c) with the Plus's differences. Started
 * 22 November 2026 against MAME's macplus_ps (reference/mame-psmac.patch); nobody on the project
 * has a Plus, so the testbed's warnings stand in for the hardware (docs/12, "The Plus").
 *
 * The Plus has the SE's chips at addresses the SE layer already uses (VIA $EFE1FE, SCC $9FFFF8 and
 * $BFFFF9, IWM $DFE1FF, the 5380 at $5FF000 inside the Plus's $580000-$5FFFFF), the same RAM sizes
 * and screen and sound buffers at the top of RAM, and the same clock chip on port B bits 0-2. What
 * differs, and where it is handled:
 *
 *   ADB       none. Port B bit 3 is the mouse button, bits 4 and 5 the mouse's X2 and Y2, bit 6 the
 *             H4 video timing line: inputs the board drives, so the VIA must never drive them; the
 *             keyboard is on the shift register with the Plus keyboard protocol. The IIci ROM's ADB
 *             Manager drives a transceiver through port B bits 3-5 and the shift register, and waits
 *             at start-up ($4080A8E6) until its first transaction completes. So this file is that
 *             transceiver (the solo board's model, MAME's macadb.cpp): the shift register, its
 *             interrupt flag and enable, the ACR's shift mode bits and port B bits 3-5 are answered
 *             here and never reach the Plus's VIA. Behind it are two ADB devices, a keyboard at
 *             address 2 and a mouse at address 3, fed from the Plus's own keyboard and mouse (below).
 *   keyboard  the Plus keyboard protocol on the real VIA's shift register, run by the service core:
 *             Inquiry ($10) shifted out under the keyboard's clock (ACR shift mode 7), the answer
 *             shifted in (mode 3): a key transition, $7B for none within a quarter second, $79 before
 *             a keypad key. Key codes become ADB key codes; the keypad's = / * +, which the M0110A
 *             sends as Shift-arrows, become ADB's keypad keys (kb_key).
 *   mouse     X1 and Y1 on the SCC's DCD inputs, X2 and Y2 on VIA PB4 and PB5, the button on PB3
 *             (0 = down). The Plus ROM takes an SCC interrupt on each X1 or Y1 edge; here the service
 *             core reads both channels' RR0 and port B (mac.plusmouse= times a second, 8000 by default) and, on an
 *             edge, port B: with the pins, X2 equal to X1 is right and Y2 different from Y1 down; one count per edge
 *             as on the Plus. The Mac's writes to WR15 lose the DCD interrupt enable, so the IIci ROM's
 *             serial driver is never interrupted by a moving mouse and RR0 follows the lines.
 *   overlay   VIA port A bit 4, 1 = ROM at 0 (the SE clears it with an access to $400000). The IIci
 *             ROM's own port A bit 4 is its overlay too, which it clears; the wire always gets 0.
 *   PB6       the SE layer drives it as an output (the SE's SCSI interrupt enable); on a Plus it is
 *             H4, an input: never driven here.
 *   PA7       SCC W/REQ, an input on both; never driven.
 *   SCSI      no glue holds DTACK until the 5380 raises DRQ: a pseudo-DMA access without DRQ is a
 *             plain register access. The board layer waits for DRQ (bus and status register bit 6)
 *             before each byte (psmac_board_se.c, the pseudo-DMA window).
 *
 * Locking: the level lock of psmac_board_se.c covers this file's ADB and VIA state; core 0's VIA
 * accesses take it in psmac_board_se.c, the service core here (psmac_se_plus_lock). It is never held
 * across a bus cycle on the wire: the service core owns the transport.
 * The SCC's register pointer: the Mac writes a register number into WR0 and the next control access,
 * on either channel (the Z8530 has one pointer), reaches that register. The service core's RR0 reads must not fall between the two, so core 0 marks
 * a pointer as pending before the write goes out (scc_pending), clears it only after the access that
 * uses the pointer has been carried out (22 November: clearing it before that write went out let a poll
 * in between use the pointer up, the Mac's value landed in WR0, and the SCC interrupted without end),
 * and the mouse poll skips while it is.
 * The poll runs on the core that carries out every bus cycle, between them, so the mark is enough.
 */
#include <stdint.h>
#include "support.h"
#include "psmac.h"

int psmac_board_plus;                       /* mac.board=plus (implies psmac_board_se); also set for the original Macintosh */
int psmac_board_orig;                       /* mac.board=128k, 512k, 512ke: 1, 2, 3 (psmac_board_se.c: no SCSI, its own RAM size, the old clock chip) */

uint32_t psmac_wire_read(uint32_t addr, int size);
void psmac_wire_write(uint32_t addr, int size, uint32_t data, int posted);
void psmac_se_plus_lock(void);              /* psmac_board_se.c: the level lock ... */
void psmac_se_plus_unlock(void);            /* ... and its release, with the interrupt level brought up to date */

static inline uint64_t cnt_now(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0":"=r"(v)); return v; }
static inline uint64_t cnt_frq(void) { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0":"=r"(v)); return v; }
static uint64_t us_ticks(uint32_t us) { return (cnt_frq() * us) / 1000000u; }

#define PLUS_VIA(reg)  (0xEFE1FEu + ((uint32_t)(reg) << 9))
#define PLUS_SCC_B_RD  0x9FFFF8u            /* channel B control, read (RR0 with the pointer at 0) */
#define PLUS_SCC_A_RD  0x9FFFFAu            /* channel A control */

/* the VIA's registers as the ROM wrote them, where the Plus's must differ */
static struct {
    uint8_t sr, acr, ddrb, orb, pcr;
    uint8_t ier_cb;                         /* IER bits 3 and 4 (CB2, CB1) as the ROM set them: never on the wire (the keyboard's clock is on CB1) */
    int ifr_sr, ier_sr;                     /* the shift register's interrupt flag and enable (IFR/IER bit 2) */
    uint32_t sr_irqs;
} pv;
static volatile uint8_t kb_mode;            /* the ACR's shift mode bits the keyboard needs on the wire (0, $0C or $1C) */
static volatile uint8_t kb_pcr = 0x00;      /* the PCR's CB2 bits on the wire: $00 an input, $C0 held low (the keyboard's request) */
static uint8_t wire_pcr(void) { return (uint8_t)((pv.pcr & 0x0f) | 0x10 | kb_pcr); }   /* CB1 flags rising edges (the keyboard's clock: KB_EDGE below) */

/* ---- the ADB devices ---------------------------------------------------------------------- */
#define KEYQ 16
static struct {
    int addr, hi;                           /* address; register 3's high byte (SRQ enable $20 | address) */
    uint8_t q[KEYQ]; int qh, qt;            /* ADB key codes, bit 7 = released */
    uint32_t talks_logged;
    uint8_t mods;                           /* register 2's high byte, active low: $20 caps, $04 shift, $02 option, $01 command */
} kbd;
static struct {
    int addr, hi;
    int32_t dx, dy;                         /* counts not yet reported */
    int button, reported_button;
} mouse;
static void devices_reset(void)
{
    kbd.addr = 2; kbd.hi = 0x20 | 2; kbd.qh = kbd.qt = 0; kbd.mods = 0xff;
    mouse.addr = 3; mouse.hi = 0x20 | 3; mouse.dx = mouse.dy = 0; mouse.reported_button = mouse.button;
}
static int kbd_has_data(void) { return kbd.qh != kbd.qt; }
static int mouse_has_data(void) { return mouse.dx || mouse.dy || mouse.button != mouse.reported_button; }
static int clamp7(int32_t v) { return v > 63 ? 63 : v < -64 ? -64 : (int)v; }

/* ---- the ADB transceiver (macadb.cpp's model, as psmac_board_solo.c) ---------------------- */
enum { ADB_NEW_COMMAND = 0, ADB_XFER_EVEN = 1, ADB_XFER_ODD = 2, ADB_IDLE = 3 };
static struct {
    int state, irq;                         /* irq: the transceiver's interrupt line, 1 = asserted (PB3 reads 0) */
    int direction;                          /* 1 = the Mac is shifting out (a command, Listen data), 0 = a device answers */
    int datasize; uint8_t buffer[8];
    int pending; uint64_t due; uint8_t send;
    int listen_addr, listen_reg, listen_n; uint8_t listen[8];
    int last_talk; uint8_t last_cmd;
    int inited;                             /* the ROM has run a command: autopoll and SRQ only after that */
    uint32_t cmds, talks, listens, resets, answered, autopolls, srqs;
} adb = { .state = ADB_IDLE, .last_talk = -1 };
static uint32_t adb_trace_left;             /* mac.plustrace=N: the first N ADB commands */

/* a Talk: fill the answer, if the device has one */
static void talk(int addr, int reg)
{
    adb.datasize = 0;
    if (addr == kbd.addr) {
        if (reg == 0 && kbd_has_data()) {
            adb.buffer[0] = kbd.q[kbd.qt]; kbd.qt = (kbd.qt + 1) % KEYQ;
            if (kbd_has_data()) { adb.buffer[1] = kbd.q[kbd.qt]; kbd.qt = (kbd.qt + 1) % KEYQ; }
            else adb.buffer[1] = 0xff;
            adb.datasize = 2;
            if (++kbd.talks_logged <= 64) kprintf("[PSMAC:PLUS] ADB keyboard register 0: %02x %02x\n", adb.buffer[0], adb.buffer[1]);
        } else if (reg == 2) { adb.buffer[0] = kbd.mods; adb.buffer[1] = 0xff; adb.datasize = 2; }
        else if (reg == 3) { adb.buffer[0] = (uint8_t)kbd.hi; adb.buffer[1] = 0x01; adb.datasize = 2; }
    } else if (addr == mouse.addr) {
        if (reg == 0 && mouse_has_data()) {
            int dx = clamp7(mouse.dx), dy = clamp7(mouse.dy);
            mouse.dx -= dx; mouse.dy -= dy; mouse.reported_button = mouse.button;
            adb.buffer[0] = (uint8_t)((mouse.button ? 0 : 0x80) | (dy & 0x7f));
            adb.buffer[1] = (uint8_t)(0x80 | (dx & 0x7f));
            adb.datasize = 2;
        } else if (reg == 3) { adb.buffer[0] = (uint8_t)mouse.hi; adb.buffer[1] = 0x01; adb.datasize = 2; }
    }
    if (adb.datasize) adb.answered++;
}
/* the bytes of a Listen, once both are in: register 3 moves a device or sets its handler */
static void listen_done(void)
{
    if (adb_trace_left) kprintf("[PSMAC:PLUS] ADB listen to %d reg %d: %d bytes %02x %02x\n", adb.listen_addr, adb.listen_reg, adb.listen_n, adb.listen[0], adb.listen[1]);
    if (adb.listen_reg != 3 || adb.listen_n < 2) return;
    int na = adb.listen[0] & 0x0f; uint8_t h = adb.listen[1];
    if (h != 0xfe && h != 0x00) return;     /* a handler change (1, 2, 3...): ours stays 1 */
    if (adb.listen_addr == kbd.addr) { kbd.addr = na; kbd.hi = (adb.listen[0] & 0x20) | na; }
    else if (adb.listen_addr == mouse.addr) { mouse.addr = na; mouse.hi = (adb.listen[0] & 0x20) | na; }
}
static void adb_command(uint8_t cmd)
{
    int addr = cmd >> 4, kind = (cmd >> 2) & 3, reg = cmd & 3;
    adb.cmds++; adb.last_cmd = cmd; adb.inited = 1;
    adb.direction = 0; adb.datasize = 0;
    if (kind == 3) { adb.talks++; adb.last_talk = addr; talk(addr, reg); }
    else if (kind == 2) {
        adb.listens++;
        if (addr == kbd.addr || addr == mouse.addr) { adb.direction = 1; adb.listen_addr = addr; adb.listen_reg = reg; adb.listen_n = 0; }
    } else if (reg == 0 && kind == 0) { adb.resets++; devices_reset(); }   /* SendReset: every device back to its default */
    else adb.resets++;                                                       /* Flush */
    if (adb_trace_left) {
        adb_trace_left--;
        kprintf("[PSMAC:PLUS] ADB %02x (addr %d %s reg %d): %s\n", cmd, addr,
                kind == 3 ? "talk" : kind == 2 ? "listen" : reg == 0 ? "reset" : "flush", reg,
                adb.datasize ? "answered" : adb.direction ? "listening" : kind == 3 ? "timed out" : "done");
    }
}
static void adb_newstate(int s)
{
    if (s == adb.state) return;
    if (adb_trace_left && (adb.direction || s == ADB_NEW_COMMAND || adb.state == ADB_NEW_COMMAND) && adb.listen_addr >= 0)
        kprintf("[PSMAC:PLUS]   ADB state %d -> %d (direction %d, sr %02x, listen bytes %d)\n", adb.state, s, adb.direction, pv.sr, adb.listen_n);
    /* a Listen's bytes are in once the ROM leaves the data states: for the next command (the IIci ROM
       goes straight from state 2 to state 0 when it has more to say) or for idle */
    if ((adb.state == ADB_XFER_EVEN || adb.state == ADB_XFER_ODD) && (s == ADB_NEW_COMMAND || s == ADB_IDLE) && adb.direction && adb.listen_n) {
        listen_done(); adb.listen_n = 0;
    }
    adb.state = s;
    switch (s) {
    case ADB_NEW_COMMAND:
        adb.direction = 1; adb.irq = 0;
        adb.pending = 1; adb.due = cnt_now() + us_ticks(800);
        break;
    case ADB_XFER_EVEN: case ADB_XFER_ODD:
        if (adb.direction) {                                 /* Listen data: the byte the ROM put in the shift register goes out */
            adb.irq = 0;
        } else if (adb.datasize > 0) {
            adb.send = adb.buffer[0]; adb.datasize--;
            for (int i = 0; i < adb.datasize; i++) adb.buffer[i] = adb.buffer[i + 1];
        } else { adb.send = 0; adb.irq = 1; }                /* nothing more: the line the ROM reads as "no response" */
        adb.pending = 1; adb.due = cnt_now() + us_ticks(800);
        break;
    case ADB_IDLE:
        adb.direction = 0;
        adb.irq = 0; adb.pending = 0;
        break;
    }
}
static void sr_flag(void) { pv.ifr_sr = 1; pv.sr_irqs++; }

/* time passes (the caller holds the level lock): a byte shifted after 800 us; in the idle state the
   transceiver's autopoll of the device last talked to, or a service request for another one */
void psmac_plus_tick(void)
{
    if (!psmac_board_plus) return;
    uint64_t now = cnt_now();
    if (adb.pending) {
        if (now < adb.due) return;
        adb.pending = 0;
        if (adb.state == ADB_NEW_COMMAND) { adb_command(pv.sr); sr_flag(); }
        else if (adb.state == ADB_XFER_EVEN || adb.state == ADB_XFER_ODD) {
            if (adb.direction) { if (adb.listen_n < 8) adb.listen[adb.listen_n++] = pv.sr; }
            else pv.sr = adb.send;
            sr_flag();
        } else if (adb.state == ADB_IDLE) sr_flag();          /* the autopoll's answer or the SRQ, below */
        return;
    }
    if (adb.state != ADB_IDLE || !adb.inited || pv.ifr_sr) return;
    int kd = kbd_has_data(), md = mouse_has_data();
    if (!kd && !md) return;
    if ((adb.last_talk == kbd.addr && kd) || (adb.last_talk == mouse.addr && md)) {
        talk(adb.last_talk, adb.last_cmd & 3);               /* autopoll: the last Talk again, now with data */
        adb.autopolls++;
        adb.send = 0; adb.direction = 0;
    } else {
        adb.irq = 1; adb.srqs++;                             /* SRQ: the ROM asks around */
    }
    pv.sr = 0;
    adb.pending = 1; adb.due = now + us_ticks(800);
}

/* the level-1 interrupt this file adds to the VIA's own */
int psmac_plus_irq(void) { return psmac_board_plus && pv.ifr_sr && pv.ier_sr; }

static void portb_follow(void)
{
    if (pv.ddrb & 0x30) adb_newstate((pv.orb >> 4) & 3);
}

/* The ROM writes VIA register reg. Returns 1 if the write is answered here alone; otherwise *data
   is what goes to the Plus's VIA (the caller holds the level lock, and writes the wire after
   releasing it) */
int psmac_plus_via_write(uint32_t reg, uint32_t *data)
{
    uint8_t d = (uint8_t)*data;
    psmac_plus_tick();
    switch (reg) {
    case 0:  pv.orb = d; portb_follow(); return 0;                 /* bits 3-6 are inputs on the wire: harmless */
    case 2:  pv.ddrb = d; *data = d & 0x87; portb_follow(); return 0;   /* only the clock chip's lines and the sound enable driven */
    case 1: case 15: *data = d & ~0x10u; return 0;                 /* the overlay stays off */
    case 3:  *data = (d & 0x7f) | 0x10; return 0;                  /* PA7 (SCC W/REQ) an input, PA4 (overlay) an output */
    case 10: pv.sr = d; pv.ifr_sr = 0; return 1;                    /* the shift register is the transceiver's */
    case 11: pv.acr = d; *data = (d & ~0x1cu) | kb_mode; return 0; /* the real shift register is the keyboard's */
    case 12: pv.pcr = d; *data = wire_pcr(); return 0;              /* CB2 is the keyboard's data line */
    case 13: if (d & 0x04) pv.ifr_sr = 0; *data = d & ~0x1cu; return 0;
    case 14:
        if (d & 0x04) pv.ier_sr = (d & 0x80) != 0;
        if (d & 0x18) { if (d & 0x80) pv.ier_cb |= d & 0x18; else pv.ier_cb &= ~(d & 0x18); }
        *data = d & ~0x1cu;
        if (!(*data & 0x7f)) return 1;                             /* nothing left for the real chip */
        return 0;
    default: return 0;
    }
}
/* the shift register read: answered here without a bus cycle (the caller holds the level lock) */
int psmac_plus_via_local(uint32_t reg, uint32_t *val)
{
    if (reg != 10) return 0;
    psmac_plus_tick();
    *val = pv.sr; pv.ifr_sr = 0;
    return 1;
}
/* The ROM reads VIA register reg; r is what the Plus's VIA answered (after the SE layer's own
   mending). Returns what the ROM sees (the caller holds the level lock) */
uint32_t psmac_plus_via_read(uint32_t reg, uint32_t r)
{
    psmac_plus_tick();
    switch (reg) {
    case 0:                                                        /* PB3 the transceiver's interrupt (0 = asserted), PB4-5 its state as the ROM drives it */
        r &= ~0x38u;
        if (!adb.irq) r |= 0x08;
        r |= (pv.orb & pv.ddrb & 0x30) | (~pv.ddrb & 0x30);
        return r;
    case 2:  return (r & 0x87) | (pv.ddrb & 0x78);
    case 11: return (r & ~0x1cu) | (pv.acr & 0x1c);
    case 12: return pv.pcr;
    case 13: {                                                     /* SR the transceiver's; CB1 and CB2 (the keyboard's clock and data) never seen */
        uint32_t any = (r & 0x80) || (pv.ifr_sr && pv.ier_sr);
        return (r & 0x63) | (pv.ifr_sr ? 0x04 : 0) | (any ? 0x80 : 0);
    }
    case 14: return (r & ~0x1cu) | (pv.ier_sr ? 0x04 : 0) | pv.ier_cb | 0x80;
    default: return r;
    }
}

/* ---- the SCC's register pointer (core 0, psmac_board_se.c's SCC path) ----------------------- */
static volatile uint32_t scc_pending;       /* a register number written, not yet used (the Z8530 has one pointer for both channels) */
static uint8_t scc_ptr;
static int scc_clear_after;                 /* the write being made uses the pointer: clear the mark once it has gone out */
static uint32_t scc_dcd_ie_cleared;
static int scc_dcd_keep;                    /* mac.plusdcd=keep: WR15 as the Mac writes it (testing) */
/* a write to SCC register ch (0 B control, 2 A control, 4 B data, 6 A data): what goes to the chip */
uint32_t psmac_plus_scc_write(uint32_t ch, uint32_t data)
{
    if (!psmac_board_plus || (ch & 4)) return data;
    if (scc_ptr) {                                       /* the register the pointer named */
        int reg = scc_ptr;
        scc_ptr = 0;
        scc_clear_after = 1;                             /* not yet: until the write has reached the service core (psmac_plus_scc_written),
                                                            a poll in between would read the register the pointer still names and use it up */
        if (reg == 15 && (data & 0x08) && !scc_dcd_keep) { data &= ~0x08u; scc_dcd_ie_cleared++; }   /* WR15: no DCD interrupts, the mouse is on DCD */
        return data;
    }
    int reg = (data & 7) | (((data >> 3) & 7) == 1 ? 8 : 0);   /* WR0: register bits, and "point high" */
    if (reg) { scc_ptr = (uint8_t)reg; __atomic_store_n(&scc_pending, 1, __ATOMIC_RELEASE); }
    return data;
}
/* the write psmac_plus_scc_write prepared has gone out (the service core has taken it, in order) */
void psmac_plus_scc_written(void)
{
    if (scc_clear_after) { scc_clear_after = 0; __atomic_store_n(&scc_pending, 0, __ATOMIC_RELEASE); }
}
/* a read of SCC register ch has been made */
void psmac_plus_scc_read(uint32_t ch)
{
    if (!psmac_board_plus || (ch & 4)) return;
    if (scc_ptr) { scc_ptr = 0; __atomic_store_n(&scc_pending, 0, __ATOMIC_RELEASE); }
}

/* ---- the Plus's mouse (service core) -------------------------------------------------------
   A quadrature decoder (22 November 2026). The first version counted one step on each X1 or Y1 edge seen
   in RR0 and read X2 and Y2 from port B afterwards, and only when X1 or Y1 had moved. On the owner's
   tester's real Plus the pointer was jerky and wandered ("a mind of its own"): a real mouse moved briskly
   gives X1 edges faster than one poll's worth, so two edges between polls were no edge at all, and an X2
   that changed between the RR0 read and the port B read turned a step round. MAME's mouse makes slow,
   clean pulses and showed neither. Now each poll reads both RR0s and port B back to back and follows
   each axis's two-bit state (X1, X2) through every transition, X2's as well as X1's; two transitions are
   one step, the Plus's own resolution (it counts X1 edges only). The tester's first log (114 s) had 1,180
   X1 edges and 49,584 Y1 edges, 435 a second: a line that chatters, or reads that are not the line. So a
   line's new level counts only once two polls in a row agree (mac.plusdebounce=0 takes the first), a poll
   that finds both bits of an axis changed is counted as a jump and ignored (crediting it in the last
   direction turned noise into drift), and the heartbeat counts each of the four lines' changes apart. */
static uint32_t mouse_hz = 8000;            /* mac.plusmouse=: polls a second (3 reads each), 0 = off; the debounce
                                               needs two polls per transition, so a brisk mouse (2,000 a second) wants 8,000 */
static int mouse_debounce = 1;              /* mac.plusdebounce=0: a line's level as read, not confirmed */
static int mouse_jitter = 1;                /* mac.plusjitter=0: polls at exactly mac.plusmouse= a second */
static struct {
    int init; uint8_t qx, qy; int button;   /* qx: (X1 as RR0 reads it) << 1 | X2; qy the same for Y (accepted) */
    uint8_t rx, ry;                         /* the last poll's reading, for the debounce */
    int accx, accy;                         /* transitions not yet a whole step */
    uint64_t next;
    uint32_t polls, skipped, edges_x, edges_y, jumps, clicks, glitches;
    uint32_t line[4];                       /* accepted changes of X1, X2, Y1, Y2 */
    uint32_t lcg;                           /* the poll timing's jitter (mouse_poll) */
} ms;
/* Reading VIA port B clears CB1's flag, which the keyboard's turn-round waits for (KB_EDGE below): while it
   waits, the mouse leaves port B alone (both run on the service core, one after the other) */
static int kb_edge_watch;
/* One axis's transition from state o to n (bit 1 the RR0 line, bit 0 the port B line): +1, -1, 0 for none,
   2 for both bits changed. The signs are the first version's, which the Plus ROM agrees with in the testbed:
   on an RR0-line change X moves +1 when RR0's bit differs from X2, Y +1 when it equals Y2; on a port-B-line
   change, the other way round (the next quarter of the same cycle). */
static int quad_step(uint8_t o, uint8_t n, int y)
{
    if (o == n) return 0;
    uint8_t a = n >> 1, b = n & 1;
    if ((o ^ n) == 3) return 2;
    int d = (o ^ n) == 2 ? (a != b ? 1 : -1) : (b == a ? 1 : -1);
    return y ? -d : d;               /* Y: the same rules the other way round */
}
static int quad_count(uint8_t o, uint8_t n, int y, int *acc, uint32_t *edges, uint32_t *line)
{
    int t = quad_step(o, n, y);
    if (!t) return 0;
    if ((o ^ n) & 2) line[0]++;
    if ((o ^ n) & 1) line[1]++;
    if (t == 2) { ms.jumps++; return 0; }
    *edges += 1;
    *acc += t;
    int step = *acc / 2;            /* two transitions a step; the remainder waits for the next */
    *acc -= step * 2;
    return step;
}
/* the debounce: a reading becomes the accepted state only when the poll before read the same */
static uint8_t settle(uint8_t raw, uint8_t *prev, uint8_t cur)
{
    uint8_t keep = (uint8_t)~(raw ^ *prev) & 3;      /* bits read the same twice running */
    if ((*prev ^ cur) & ~(raw ^ cur) & 3) ms.glitches++;   /* a level read once, then gone back: filtered */
    *prev = raw;
    return mouse_debounce ? (uint8_t)((cur & ~keep) | (raw & keep)) : raw;
}
static void mouse_poll(uint64_t now)
{
    if (!mouse_hz || now < ms.next || kb_edge_watch) return;
    /* each wait between polls from half to one and a half periods, not one exactly (22 November 2026):
       the tester's 512Ke had a faint steady whine from its speaker, and bus reads at a fixed 8 kHz (2 kHz
       before) are a tone that can couple into the amplifier; spread, they are at most a hiss.
       mac.plusjitter=0 keeps the fixed period, to compare */
    {
        uint64_t per = cnt_frq() / mouse_hz;
        if (mouse_jitter) { ms.lcg = ms.lcg * 1664525u + 1013904223u; per = per / 2 + (per * (ms.lcg >> 16)) / 65536u; }
        ms.next = now + per;
    }
    if (__atomic_load_n(&scc_pending, __ATOMIC_ACQUIRE)) { ms.skipped++; return; }
    ms.polls++;
    uint8_t x1 = (psmac_wire_read(PLUS_SCC_A_RD, 1) >> 3) & 1, y1 = (psmac_wire_read(PLUS_SCC_B_RD, 1) >> 3) & 1;
    uint8_t pb = (uint8_t)psmac_wire_read(PLUS_VIA(0), 1);
    uint8_t rx = (uint8_t)(x1 << 1 | ((pb >> 4) & 1)), ry = (uint8_t)(y1 << 1 | ((pb >> 5) & 1));
    int button = !(pb & 0x08), dx = 0, dy = 0;
    if (!ms.init) { ms.qx = ms.rx = rx; ms.qy = ms.ry = ry; ms.init = 1; ms.button = button; return; }
    uint8_t qx = settle(rx, &ms.rx, ms.qx), qy = settle(ry, &ms.ry, ms.qy);
    dx = quad_count(ms.qx, qx, 0, &ms.accx, &ms.edges_x, &ms.line[0]);
    dy = quad_count(ms.qy, qy, 1, &ms.accy, &ms.edges_y, &ms.line[2]);
    ms.qx = qx; ms.qy = qy;
    if (!dx && !dy && button == ms.button) return;
    if (button != ms.button && button) ms.clicks++;
    ms.button = button;
    psmac_se_plus_lock();
    mouse.dx += dx; mouse.dy += dy; mouse.button = button;
    psmac_plus_tick();
    psmac_se_plus_unlock();
}

/* ---- the Plus's keyboard (service core) ------------------------------------------------------
   Inside Macintosh vol. III/IV, "The Keyboard": the Mac sends a command, the keyboard clocks it in
   and answers one byte; Inquiry waits up to a quarter second for a key. A transition is the key
   number shifted left once with bit 0 set, bit 7 set for a release; $79 comes first for a keypad
   key (the keypad's key numbers are $40 up, as ADB's), $7B is "nothing". The modifier keys report
   transitions like any key, with ADB's numbers ($37 command, $38 shift, $39 caps lock, $3A option);
   the arrow keys on the M0110A are keypad keys with their own numbers. */
static int kbd_on = 1;                      /* mac.pluskbd=off: leave the keyboard alone */
static uint32_t kb_log = 64;                /* mac.pluskeylog=N: the first N key transitions logged (tools/plus_keys_check.py) */
enum { KB_IDLE, KB_SEND, KB_EDGE, KB_RECV };
static struct {
    int st, prefix, present, line_first;
    int hold_shift; uint8_t hold_arrow, hold_raw; uint32_t kp_down;   /* the keypad's shifted keys (kb_key) */
    uint64_t t, t2, next, poll;
    uint32_t bytes, nulls, keys, absent, timeouts, send_us, edge_missed, dropped;
    uint8_t last;
} kb;
static void kb_pcr_set(uint8_t cb2)
{
    psmac_se_plus_lock();
    kb_pcr = cb2;
    uint8_t v = wire_pcr();
    psmac_se_plus_unlock();
    psmac_wire_write(PLUS_VIA(12), 1, v, 0);
}
static void kb_acr(uint8_t mode)
{
    psmac_se_plus_lock();
    kb_mode = mode;
    uint8_t v = (uint8_t)((pv.acr & ~0x1cu) | mode);
    psmac_se_plus_unlock();
    psmac_wire_write(PLUS_VIA(11), 1, v, 0);
}
/* the M0110A's keypad and arrow keys ($79 first), to ADB key codes */
static uint8_t keypad_to_adb(uint8_t k)
{
    switch (k) {
    case 0x46: return 0x3b;                 /* left */
    case 0x42: return 0x3c;                 /* right */
    case 0x48: return 0x3d;                 /* down */
    case 0x4d: return 0x3e;                 /* up */
    default:   return k;                    /* the keypad's own: the same numbers as ADB's */
    }
}
/* one ADB key transition into the keyboard's queue (the level lock taken here) */
static void kb_emit(uint8_t raw, uint8_t k, int up)
{
    kb.keys++;
    psmac_se_plus_lock();
    uint8_t m = 0;
    if (k == 0x39) m = 0x20; else if (k == 0x38) m = 0x04; else if (k == 0x3a) m = 0x02; else if (k == 0x37) m = 0x01;
    if (m) { if (up) kbd.mods |= m; else kbd.mods &= ~m; }
    int nq = (kbd.qh + 1) % KEYQ;
    if (nq != kbd.qt) { kbd.q[kbd.qh] = (uint8_t)((up ? 0x80 : 0) | k); kbd.qh = nq; } else kb.dropped++;
    psmac_plus_tick();
    psmac_se_plus_unlock();
    if (kb.keys <= kb_log) kprintf("[PSMAC:PLUS] keyboard: %02x -> ADB key %02x %s%s\n", raw, k, up ? "up" : "down", kb.keys == kb_log ? " (no more keys logged: mac.pluskeylog=N)" : "");
}
/* The M0110A's keypad =, /, * and + are its arrow keys' codes with a Shift of the keyboard's own around
   them: Shift down, the key down, Shift up, then the key up when it is let go (the testbed, 22 November
   2026: $71, $79 $0D, $F1 ... $79 $8D for keypad +). A Plus took them for Shift-arrows and its key map
   made them + * / =; ADB has keys of their own for them. So a Shift down from the keyboard is held back
   until the next byte: an arrow code down followed at once by Shift up is the keypad key, delivered
   without the Shift; anything else, and the Shift goes through as it came (a Shift held while an arrow
   is pressed stays down past the arrow, which tells the two apart). */
static uint8_t keypad_shifted(uint8_t k)      /* the arrow code's keypad key, or 0 */
{
    switch (k) { case 0x46: return 0x45; case 0x42: return 0x43; case 0x4d: return 0x4b; case 0x48: return 0x51; default: return 0; }
}
static void kb_flush(void)
{
    if (kb.hold_shift) { kb_emit(0x71, 0x38, 0); kb.hold_shift = 0; }
    if (kb.hold_arrow) { kb_emit(kb.hold_raw, keypad_to_adb(kb.hold_arrow), 0); kb.hold_arrow = 0; }
}
static void kb_key(uint8_t b)
{
    kb.bytes++; kb.last = b;
    if (b == 0x7b) { kb.nulls++; kb_flush(); return; }
    if (b == 0x79) { kb.prefix = 1; return; }
    int up = (b & 0x80) != 0, pre = kb.prefix;
    uint8_t k = (b >> 1) & 0x3f;
    kb.prefix = 0;
    if (pre) k = 0x40 | k;
    if (kb.hold_arrow) {                                    /* Shift and an arrow code held: is this the Shift going up? */
        if (!pre && k == 0x38 && up) {
            uint8_t kp = keypad_shifted(kb.hold_arrow);
            kb.kp_down |= 1u << (kp & 31);
            kb_emit(kb.hold_raw, kp, 0);
            kb.hold_shift = 0; kb.hold_arrow = 0;
            return;
        }
        kb_flush();
    } else if (kb.hold_shift) {
        if (pre && !up && keypad_shifted(k)) { kb.hold_arrow = k; kb.hold_raw = b; return; }
        kb_flush();
    }
    if (!pre && k == 0x38 && !up) { kb.hold_shift = 1; return; }
    if (pre && up && keypad_shifted(k) && (kb.kp_down & (1u << (keypad_shifted(k) & 31)))) {   /* the keypad key let go */
        uint8_t kp = keypad_shifted(k);
        kb.kp_down &= ~(1u << (kp & 31));
        kb_emit(b, kp, 1);
        return;
    }
    kb_emit(b, pre ? keypad_to_adb(k) : k, up);
}
static void kb_service(uint64_t now)
{
    if (!kbd_on) return;
    switch (kb.st) {
    case KB_IDLE:
        if (now < kb.next) return;
        /* The keyboard clocks a command in only once the data line is low, and the 6522 moves CB2 only
           on the clock's edges in shift-out mode, so the line is pulled low by the PCR (CB2 a manual
           output). The command must be in the shift register before the keyboard's first edge: pulled
           low first, the keyboard can clock a bit while the shift register is still off, the command
           arrives one bit short and is ignored (the testbed, 22 November 2026: most Inquiries went
           unanswered, the shift register untouched). So: shift-out mode, the command, then the line.
           MAME's 6522 lets the PCR drive CB2 in shift-out mode; should a real one not, the keyboard
           never clocks, and the other order (line first) is tried, and kept if it works. */
        /* The PCR's "CB2 low" is set and at once taken back to "CB2 an input": the 6522 leaves the pin
           where it was until the shift register moves it, and a PCR write of the ROM's in the meantime
           (wire_pcr carries kb_pcr) can no longer pull the line low in the middle of a bit or after the
           turn (it did: most Inquiries were never answered). */
        if (!kb.line_first) {
            kb_acr(0x1c);                                    /* shift out under the keyboard's clock */
            psmac_wire_write(PLUS_VIA(10), 1, 0x10, 0);      /* Inquiry */
            kb_pcr_set(0xc0);                                /* CB2 low: the request */
            kb_pcr_set(0x00);
        } else {
            kb_acr(0x00);
            kb_pcr_set(0xc0);
            kb_pcr_set(0x00);
            kb_acr(0x1c);
            psmac_wire_write(PLUS_VIA(10), 1, 0x10, 0);
        }
        kb.st = KB_SEND; kb.t = now; kb.poll = now;
        return;
    case KB_SEND: {
        /* watched on every pass of the service loop: the moment the eighth bit is out matters (KB_EDGE) */
        uint32_t ifr = psmac_wire_read(PLUS_VIA(13), 1);
        if (ifr & 0x04) {
            /* The flag comes on the eighth falling edge of the keyboard's clock; the keyboard reads that
               last bit (always 0, so the line stays low) on the rising edge after it. Turning the line
               round before then hands it a 1 and a command it ignores ($11 for $10): it never answers
               (22 November 2026, the shift register found holding $20 after every silent Inquiry). So
               CB1's flag (rising edges, wire_pcr) is cleared now and the turn waits for it. */
            psmac_wire_write(PLUS_VIA(13), 1, 0x10, 0);
            kb.send_us = (uint32_t)((now - kb.t) * 1000000u / cnt_frq());
            kb.st = KB_EDGE; kb.t2 = now; kb_edge_watch = 1;
            return;
        }
        if (now - kb.t < us_ticks(500000)) return;          /* the keyboard clocks a command in at once (2.5 ms; the testbed's MAME runs several times slower than the clock) */
        if (kb.present || !kb.absent) kprintf("[PSMAC:PLUS] keyboard: no clock from the Plus keyboard (not plugged in?); asking again every second\n");
        kb.present = 0; kb.absent++;
        kb.line_first ^= 1;                                  /* the other way of starting a command, next time */
        kb_pcr_set(0x00);
        kb_acr(0x0c);                                        /* an input: CB2 left to the keyboard */
        kb.st = KB_IDLE; kb.next = now + cnt_frq();
        return;
    }
    case KB_EDGE: {
        uint32_t ifr = psmac_wire_read(PLUS_VIA(13), 1);
        if (!(ifr & 0x10) && now - kb.t2 < us_ticks(10000)) return;   /* the last rising edge (or, past 10 ms, it came before the flag was cleared) */
        kb_edge_watch = 0;
        if (!(ifr & 0x10)) kb.edge_missed++;
        /* ...and the line stays low a while after that edge: the M0110A looks at it again some tens of
           microseconds later, and a line already let go makes it drop the command without a word
           (MAME's keyboard log, 22 November 2026: answered every time the line went up 71 us or more
           after the last edge, never at 62 us or less). Held for 300 us of the Pi's time and at least
           20 bus reads: the time for a real keyboard, the reads for the testbed, whose clock moves
           only with bus traffic. */
        { uint64_t until = cnt_now() + us_ticks(300); int reads = 0;
          while (cnt_now() < until || reads < 20) { psmac_wire_read(PLUS_VIA(13), 1); reads++; } }
        kb_acr(0x0c);                                        /* shift in under the keyboard's clock: the line is let go, the keyboard answers */
        psmac_wire_read(PLUS_VIA(10), 1);                    /* reading the shift register clears its flag and starts the count */
        kb_pcr_set(0x00);                                    /* the request over: CB2 an input under the PCR again */
        kb.st = KB_RECV; kb.t = now; kb.poll = now;
        return;
    }
    case KB_RECV: {
        if (now < kb.poll) return;
        kb.poll = now + us_ticks(500);
        uint32_t ifr = psmac_wire_read(PLUS_VIA(13), 1);
        if (!(ifr & 0x04)) {
            /* Inquiry answers at once with a key, or with $7B after a quarter second of the keyboard's
               time. A new Inquiry before that answer restarts the keyboard's wait (and a key typed in
               between was lost): in the testbed MAME's clock advances only with bus traffic, so the
               quarter second can take several seconds here. The limit only matters if the keyboard
               goes away, so it is long. */
            if (now - kb.t < us_ticks(10000000)) return;
            kb.timeouts++;
            if (kb.timeouts <= 4)                            /* what the real VIA holds when an answer never came */
                kprintf("[PSMAC:PLUS] keyboard: no answer (%u): VIA ACR %02x PCR %02x IFR %02x SR %02x; the command took %u us to go out\n", kb.timeouts,
                        psmac_wire_read(PLUS_VIA(11), 1) & 0xff, psmac_wire_read(PLUS_VIA(12), 1) & 0xff, psmac_wire_read(PLUS_VIA(13), 1) & 0xff,
                        psmac_wire_read(PLUS_VIA(10), 1) & 0xff, kb.send_us);
            kb.st = KB_IDLE; kb.next = now + cnt_frq() / 4;
            return;
        }
        uint8_t b = (uint8_t)psmac_wire_read(PLUS_VIA(10), 1);
        if (!kb.present) { kb.present = 1; kprintf("[PSMAC:PLUS] keyboard: the Plus keyboard answers (first byte %02x; commands started %s)\n", b,
                                                    kb.line_first ? "with the line pulled low first" : "with the command loaded first"); }
        kb_key(b);
        kb.st = KB_IDLE; kb.next = now + us_ticks(2000);     /* a breath before the next Inquiry (one sent within a millisecond of the answer went unanswered too) */
        return;
    }
    }
}

/* the service core, between bus cycles (psmac_bus.c) */
void psmac_plus_service(void)
{
    if (!psmac_board_plus) return;
    uint64_t now = cnt_now();
    kb_service(now);
    mouse_poll(now);
}

/* the Plus's reset line pulsed (or the start): the VIA's registers cleared, the transceiver idle */
void psmac_plus_reset(void)
{
    pv.sr = pv.acr = pv.ddrb = pv.orb = pv.pcr = pv.ier_cb = 0; pv.ifr_sr = pv.ier_sr = 0;
    adb.state = ADB_IDLE; adb.irq = 0; adb.pending = 0; adb.datasize = 0; adb.inited = 0; adb.last_talk = -1;
    devices_reset();
    scc_ptr = 0; scc_pending = 0;
    kb.st = KB_IDLE; kb.prefix = 0; kb.next = cnt_now() + cnt_frq() / 10; kb_edge_watch = 0;
    kb.hold_shift = 0; kb.hold_arrow = 0; kb.kp_down = 0;
}

void psmac_plus_options(const char *args)
{
    extern const char *find_token(const char *, const char *);
    const char *tok;
    if ((tok = find_token(args, "mac.plustrace="))) {
        tok += 14; uint32_t n = 0; while (*tok >= '0' && *tok <= '9') n = n * 10 + (*tok++ - '0'); adb_trace_left = n;
    }
    if ((tok = find_token(args, "mac.plusdebounce="))) mouse_debounce = tok[17] != '0';
    if ((tok = find_token(args, "mac.plusjitter="))) mouse_jitter = tok[15] != '0';
    if ((tok = find_token(args, "mac.plusmouse="))) {
        tok += 14; uint32_t n = 0; while (*tok >= '0' && *tok <= '9') n = n * 10 + (*tok++ - '0'); mouse_hz = n > 20000 ? 20000 : n;
    }
    if ((tok = find_token(args, "mac.pluskbd="))) { tok += 12; kbd_on = !(tok[0] == 'o' && tok[1] == 'f'); }
    if ((tok = find_token(args, "mac.pluskeylog="))) { tok += 15; uint32_t n = 0; while (*tok >= '0' && *tok <= '9') n = n * 10 + (*tok++ - '0'); kb_log = n; }
    if ((tok = find_token(args, "mac.plusdcd="))) { tok += 12; scc_dcd_keep = tok[0] == 'k'; }
    devices_reset();
}

void psmac_plus_report(void)
{
    if (!psmac_board_plus) return;
    kprintf("[PSMAC:PLUS] ADB: state %d, %u commands (%u talk, %u answered, %u listen, %u reset/flush), %u autopolls, %u SRQs, last %02x | keyboard at %d, mouse at %d | SR interrupts %u, IFR.SR %d IER.SR %d, ACR %02x\n",
            adb.state, adb.cmds, adb.talks, adb.answered, adb.listens, adb.resets, adb.autopolls, adb.srqs, adb.last_cmd, kbd.addr, mouse.addr,
            pv.sr_irqs, pv.ifr_sr, pv.ier_sr, pv.acr);
    kprintf("[PSMAC:PLUS]   Plus keyboard: %s, %u bytes (%u keys, %u none), %u answer timeouts, %u turns without the last edge, last %02x | Plus mouse: %u Hz, %u polls (%u skipped for the SCC pointer), %u x and %u y transitions (lines X1 %u X2 %u Y1 %u Y2 %u), %u jumps, %u one-poll glitches%s, %u clicks%s, button %s | WR15 DCD enables cleared %u\n",
            !kbd_on ? "off (mac.pluskbd=off)" : kb.present ? "answering" : "not answering", kb.bytes, kb.keys, kb.nulls, kb.timeouts, kb.edge_missed, kb.last,
            mouse_hz, ms.polls, ms.skipped, ms.edges_x, ms.edges_y, ms.line[0], ms.line[1], ms.line[2], ms.line[3], ms.jumps, ms.glitches, mouse_debounce ? " filtered" : " (not filtered)", ms.clicks, mouse_jitter ? ", polls jittered" : ", polls at a fixed rate", ms.button ? "down" : "up", scc_dcd_ie_cleared);
}
