/*
 * psmac_adb.c — the Pi's keyboards and mice as two ADB devices on the Macintosh SE's own ADB bus.
 * Copyright (c) 2026 Aurora68 project. MPL-2.0.
 * SPDX-License-Identifier: MPL-2.0
 *
 * A system that does not run .PiInput (A/UX 3, whose kernel reads keyboard and mouse from ADB itself) gets
 * nothing from the card's input ring. On the SE board the IIci ROM's and A/UX's ADB drivers talk to the SE's
 * real transceiver through the SE's VIA: the shift register, its interrupt, port B bits 4-5 (the state: 0 a
 * command, 1 and 2 data bytes, 3 idle) and bit 3 (the transceiver's line, 0 = asserted). This file sits
 * between them (the VIA path of psmac_board_se.c) and adds a keyboard and a mouse that exist only here:
 *
 *   - every command goes to the real bus; a real device's answer passes through untouched;
 *   - a Talk the real bus leaves unanswered (the line low at the first data byte) is answered here if a
 *     virtual device is at that address: the real transceiver clocks its three bytes as for an empty
 *     address, and the 68k reads the virtual device's bytes and line instead (a substitution);
 *   - a Talk register 3 a real device answers at a virtual device's address is that device's collision:
 *     it stays put at the next "move if no collision" (Listen register 3, handler $FE);
 *   - with the 68k in the idle state, a virtual device with data is announced as the transceiver would:
 *     a shift register interrupt raised here, then the data bytes if the last Talk was that device's
 *     register 0 (the autopoll), else a time-out with a service request, after which the host asks
 *     around. For those bytes the real transceiver is left idle: the state lines stay at 3 on the wire,
 *     the shift register is answered here, and the real chip's shift register interrupt is off, so a
 *     byte of the real transceiver's waits until the 68k's next command or return to idle.
 *
 * The protocol as the IIci ROM drives it is in docs/08 item 151; the A/UX side in docs/23 section 23.
 * Locking: the level lock of psmac_board_se.c covers everything here; never held across a bus cycle.
 */
#include <stdint.h>
#include "support.h"
#include "../alice/alice_psmac.h"

extern int psmac_board_se, psmac_board_solo, psmac_board_plus;
uint32_t psmac_wire_read(uint32_t addr, int size);
void psmac_wire_write(uint32_t addr, int size, uint32_t data, int posted);
void psmac_se_plus_lock(void);              /* psmac_board_se.c: the level lock ... */
void psmac_se_plus_unlock(void);            /* ... and its release, with the interrupt level brought up to date */
void psmac_se_via_guest(uint8_t *orb, uint8_t *ddrb, uint8_t *ier);
int psmac_se_floppy_busy(void);

#ifdef PSMAC_ADB_HOST                       /* tools/adb_test.c: this file on the host, against a model of the transceiver and of the ROM */
uint64_t adb_host_now(void);
static inline uint64_t cnt_now(void) { return adb_host_now(); }
static inline uint64_t cnt_frq(void) { return 1000000u; }
#else
static inline uint64_t cnt_now(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0":"=r"(v)); return v; }
static inline uint64_t cnt_frq(void) { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0":"=r"(v)); return v; }
#endif
static uint64_t us_ticks(uint32_t us) { return (cnt_frq() * us) / 1000000u; }

#define SE_VIA_REG(reg) (0xEFE1FEu + ((uint32_t)(reg) << 9))
#define BYTE_US   300u                      /* a virtual byte's interrupt after the state lines moved */
#define IDLE_US   2000u                     /* the 68k idle this long before a virtual device is announced: the real
                                               transceiver's own Talk on the bus is over by then */

static int adb_mode;                        /* mac.adb=: 0 auto, 1 virtual, 2 off */
static uint32_t trace_left;                 /* mac.adbtrace=N */
#define TRACE(...) do { if (trace_left) { trace_left--; kprintf("[PSMAC:ADB] " __VA_ARGS__); } } while (0)

/* ---- the two devices ------------------------------------------------------------------------ */
#define KEYQ 64
enum { M_CTRL = 1, M_SHIFT = 2, M_OPT = 4, M_CMD = 8, M_RCTRL = 16, M_RSHIFT = 32, M_ROPT = 64 };
static const uint8_t mod_code[7] = { 0x36, 0x38, 0x3a, 0x37, 0x7d, 0x7b, 0x7c };
struct vdev { int addr, handler; uint8_t srq_en, collided, seen; uint32_t answers;
              uint64_t polled; };           /* the host's last Talk register 0 here that had its data states: it polls by itself */
static struct {
    struct vdev d;
    uint8_t q[KEYQ]; int qh, qt;            /* ADB key codes, bit 7 = released */
    uint8_t mods;                           /* the modifier keys reported down (M_*) */
    uint8_t hid_mods;                       /* the keyboards' HID modifier byte */
    uint8_t caps, leds;
} kb;
static struct {
    struct vdev d;
    int32_t dx, dy;                         /* counts not yet reported */
    int button, reported;                   /* the button as the next report has it, and as the last one had */
    uint8_t buttons;                        /* the mice's buttons: 1 left, 2 right, 4 middle */
    uint8_t btn_mods;                       /* the modifier the right or middle button holds (M_*) */
} ms;
static uint8_t rb_mod = M_CMD;              /* mac.rightbutton=: the right button's modifier */
/* mac.adbspeed=: the mouse's counts in percent, and the hundredths not yet a count. Half by default: the host puts its
   own acceleration on an ADB mouse's counts, which .PiInput's pointer (a count a pixel) never had (docs/23 section 23) */
static int32_t ms_speed = 50, ms_fx, ms_fy;
static struct { uint32_t keys, reports, dropped, nohost, abs, srqs, autopolls, subst, real, collisions, kick_lost, kick_cancel, listens, held;
                uint32_t preempt, real_srq, take_us_max, xfer_us_max, real_events; } ct;   /* the last four: what the real SE's transceiver and host do (docs/23 section 23) */
static uint32_t lcg = 0x2545f491u;

static int kb_has_data(void) { return kb.qh != kb.qt; }
static int ms_has_data(void) { return ms.dx || ms.dy || ms.button != ms.reported; }
static int clamp7(int32_t v) { return v > 63 ? 63 : v < -64 ? -64 : (int)v; }
static void key_put(uint8_t code, int up)
{
    if (!kb.d.seen) { ct.nohost++; return; }
    int nq = (kb.qh + 1) % KEYQ;
    if (nq == kb.qt) { ct.dropped++; return; }
    kb.q[kb.qh] = (uint8_t)((up ? 0x80 : 0) | code); kb.qh = nq;
}
/* the modifier keys as they should be down now: the keyboards', and the mouse buttons' */
static void mods_sync(void)
{
    uint8_t h = kb.hid_mods, want = 0;
    if (!kb.d.seen) return;                 /* no host yet: they go down when it has found the keyboard */
    if (h & 0x01) want |= M_CTRL;
    if (h & 0x02) want |= M_SHIFT;
    if (h & 0x04) want |= M_OPT;
    if (h & 0x88) want |= M_CMD;
    if (h & 0x10) want |= kb.d.handler == 3 ? M_RCTRL : M_CTRL;      /* handler 3: the right-hand keys have codes of their own */
    if (h & 0x20) want |= kb.d.handler == 3 ? M_RSHIFT : M_SHIFT;
    if (h & 0x40) want |= kb.d.handler == 3 ? M_ROPT : M_OPT;
    want |= ms.btn_mods;
    for (int i = 0; i < 7; i++) {
        if ((want ^ kb.mods) & (1u << i)) key_put(mod_code[i], !(want & (1u << i)));
    }
    kb.mods = want;
}
/* A mouse with more buttons than one (mac/piinput.s does the same): the right button is a Command-click
   (mac.rightbutton=ctrl: Control, none: plain), the middle one an Option-click. The modifier key goes down on
   the keyboard first and the button follows once the host has taken it; it comes up after the button. */
static void buttons_settle(void)
{
    int want = ms.buttons != 0;
    if (want && !ms.button) {
        uint8_t m = (uint8_t)(((ms.buttons & 2) ? rb_mod : 0) | ((ms.buttons & 4) ? M_OPT : 0));
        if (m != ms.btn_mods) { ms.btn_mods = m; mods_sync(); }
        if (!m || !kb.d.seen || !kb_has_data()) ms.button = 1;
    } else if (!want && ms.button) ms.button = 0;
    if (!want && !ms.button && !ms.reported && ms.btn_mods) { ms.btn_mods = 0; mods_sync(); }
}
static void devices_reset(void)             /* SendReset, or the 68k's RESET: every device back to its default */
{
    kb.d.addr = 2; kb.d.handler = 2; kb.d.srq_en = 1; kb.d.collided = 0;
    kb.qh = kb.qt = 0; kb.mods = 0; kb.leds = 7;
    ms.d.addr = 3; ms.d.handler = 1; ms.d.srq_en = 1; ms.d.collided = 0;
    ms.dx = ms.dy = 0; ms.reported = ms.button;
}
static struct vdev *dev_at(int addr) { return addr == kb.d.addr ? &kb.d : addr == ms.d.addr ? &ms.d : 0; }
static int dev_pending(struct vdev *d) { return d->seen && (d == &kb.d ? kb_has_data() : ms_has_data()); }
/* worth an event in the idle state: not for a host that asks the device itself every few milliseconds (A/UX) */
static int dev_announce(struct vdev *d, uint64_t now) { return dev_pending(d) && (!d->polled || now - d->polled > us_ticks(500000)); }
/* a device other than the one at addr wants the host: the service request */
static int srq_wanted(int addr)
{
    return (kb.d.addr != addr && kb.d.srq_en && dev_pending(&kb.d)) || (ms.d.addr != addr && ms.d.srq_en && dev_pending(&ms.d));
}
/* a Talk nobody on the real bus answered: the virtual device's bytes, if it has any */
static int dev_talk(int addr, int reg, uint8_t *b)
{
    struct vdev *d = dev_at(addr);
    if (!d) return 0;
    if (reg == 3) {
        lcg = lcg * 1664525u + 1013904223u;
        b[0] = (uint8_t)(0x40 | (d->srq_en ? 0x20 : 0) | ((lcg >> 24) & 15)); b[1] = (uint8_t)d->handler;
        d->collided = 0;
    } else if (d == &kb.d && reg == 0 && kb_has_data()) {
        b[0] = kb.q[kb.qt]; kb.qt = (kb.qt + 1) % KEYQ; ct.keys++;
        if (kb_has_data()) { b[1] = kb.q[kb.qt]; kb.qt = (kb.qt + 1) % KEYQ; ct.keys++; }
        else b[1] = 0xff;
    } else if (d == &kb.d && reg == 2) {
        b[0] = (uint8_t)~((kb.caps ? 0x20 : 0) | ((kb.mods & (M_CTRL | M_RCTRL)) ? 0x08 : 0) | ((kb.mods & (M_SHIFT | M_RSHIFT)) ? 0x04 : 0) |
                          ((kb.mods & (M_OPT | M_ROPT)) ? 0x02 : 0) | ((kb.mods & M_CMD) ? 0x01 : 0));
        b[1] = (uint8_t)(0xf8 | kb.leds);
    } else if (d == &ms.d && reg == 0 && ms_has_data()) {
        int dx = clamp7(ms.dx), dy = clamp7(ms.dy);          /* more than 63 counts: the rest in the next report */
        ms.dx -= dx; ms.dy -= dy; ms.reported = ms.button;
        b[0] = (uint8_t)((ms.button ? 0 : 0x80) | (dy & 0x7f)); b[1] = (uint8_t)(0x80 | (dx & 0x7f));
        ct.reports++;
    } else return 0;
    d->answers++;
    buttons_settle();
    return 2;
}
/* the bytes of a Listen: register 3 moves a device, enables its service request or sets its handler */
static void dev_listen(int addr, int reg, const uint8_t *b, int n)
{
    struct vdev *d = dev_at(addr);
    if (!d || n < 2) return;
    ct.listens++;
    TRACE("listen to %d register %d: %02x %02x (the virtual %s)\n", addr, reg, b[0], b[1], d == &kb.d ? "keyboard" : "mouse");
    if (reg == 2 && d == &kb.d) { kb.leds = b[1] & 7; return; }
    if (reg != 3) return;
    if (b[1] == 0xfe) {                                      /* move if no collision */
        if (d->collided) d->collided = 0; else d->addr = b[0] & 15;
    } else if (b[1] == 0x00) { d->addr = b[0] & 15; d->srq_en = (b[0] >> 5) & 1; }
    else if (b[1] == 0xff || b[1] == 0xfd) return;           /* self-test; move if the activator is pressed: it has none */
    else if (d == &kb.d ? (b[1] == 2 || b[1] == 3) : (b[1] == 1 || b[1] == 2)) d->handler = b[1];
}

/* ---- USB HID usages to ADB key codes (mac/piinput.s usb_to_mac, with ADB's own codes for the arrows) ---- */
static const uint8_t hid_to_adb[0x68] = {
    0xff, 0xff, 0xff, 0xff, 0x00, 0x0b, 0x08, 0x02, 0x0e, 0x03, 0x05, 0x04, 0x22, 0x26, 0x28, 0x25,   /* A-L */
    0x2e, 0x2d, 0x1f, 0x23, 0x0c, 0x0f, 0x01, 0x11, 0x20, 0x09, 0x0d, 0x07, 0x10, 0x06, 0x12, 0x13,   /* M-Z, 1 2 */
    0x14, 0x15, 0x17, 0x16, 0x1a, 0x1c, 0x19, 0x1d, 0x24, 0x35, 0x33, 0x30, 0x31, 0x1b, 0x18, 0x21,   /* 3-0, Return Esc Delete Tab Space - = [ */
    0x1e, 0x2a, 0x2a, 0x29, 0x27, 0x32, 0x2b, 0x2f, 0x2c, 0x39, 0x7a, 0x78, 0x63, 0x76, 0x60, 0x61,   /* ] \ # ; ' ` , . / Caps F1-F6 */
    0x62, 0x64, 0x65, 0x6d, 0x67, 0x6f, 0x69, 0x6b, 0x71, 0x72, 0x73, 0x74, 0x75, 0x77, 0x79, 0x3c,   /* F7-F12 F13-F15 Help Home PgUp Del End PgDn Right */
    0x3b, 0x3d, 0x3e, 0x47, 0x4b, 0x43, 0x4e, 0x45, 0x4c, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,   /* Left Down Up Clear, keypad / * - + Enter 1-7 */
    0x5b, 0x5c, 0x52, 0x41, 0x32, 0x6e, 0xff, 0x51,                                                   /* keypad 8 9 0 ., non-US \, the menu key, power (left out), keypad = */
};
/* a Mac virtual key code (the remote control's) as the ADB keyboard sends it */
static uint8_t virt_to_adb(uint8_t v)
{
    switch (v) {
    case 0x7b: return 0x3b; case 0x7c: return 0x3c; case 0x7d: return 0x3d; case 0x7e: return 0x3e;   /* the arrows */
    case 0x37: case 0x38: case 0x3a: case 0x3b: case 0x3c: case 0x3d: case 0x3e: return 0xff;        /* modifiers: from the modifier byte */
    default: return v;
    }
}
static void key_event(uint8_t code, int down, uint8_t hid_mods)
{
    if (code == 0x39) {                     /* Caps Lock locks on an ADB keyboard: down when it engages, up when it lets go */
        if (down && kb.d.seen) { kb.caps ^= 1; key_put(0x39, !kb.caps); }
        return;
    }
    if (down) { kb.hid_mods = hid_mods; mods_sync(); key_put(code, 0); }
    else { key_put(code, 1); kb.hid_mods = hid_mods; mods_sync(); }
}

/* ---- the multiplexer ------------------------------------------------------------------------ */
enum { PH_REAL,                             /* everything passes through */
       PH_WATCH,                            /* a Talk to a virtual device's address is out: what does the real bus say? */
       PH_SUBST,                            /* nobody there: the 68k reads the virtual device's bytes on the real transceiver's clock */
       PH_KICK,                             /* idle, a shift register interrupt of our own raised; the real transceiver left idle */
       PH_VXFER };                          /* the 68k is taking the bytes of that event */
enum { KICK_AUTOPOLL = 1, KICK_SRQ };
static struct {
    volatile int on;                        /* the devices exist */
    volatile int phase, st;                 /* st: the state lines as the 68k drives them */
    uint8_t orb, ddrb;
    volatile uint8_t ier;                   /* the 68k's own interrupt enables */
    uint8_t sr; int sr_fresh, sr_held;      /* the last byte the 68k wrote to the shift register; since the last state change; not on the wire yet */
    uint8_t cmd, last_cmd; int cmd_known, last_valid, preempted;
    int flag_seen;                          /* the real shift register flag seen set since the last state change */
    int listening, listen_addr, listen_reg, listen_n, listen_first0, listen_tail; uint8_t listen[8];
    uint8_t buf[2]; int n, bi, srq;         /* the virtual answer; the byte the state lines are at; a service request on the second byte */
    int kick, kick_seen, kick_addr;
    volatile int vflag;                     /* our shift register interrupt flag */
    int vdue_on; uint64_t vdue;
    int real_busy;                          /* the 68k is taking a byte the real transceiver sent in the idle state */
    int ier_off;                            /* the real chip's shift register interrupt is off (ours to put back) */
    uint64_t idle_since, kick_at, next_kick;
    uint32_t srq_run;                       /* service requests in a row with no virtual device read */
    uint32_t cmds;
    int trace_quiet; uint8_t traced[256];   /* mac.adbtrace: a command seen eight times is no longer worth a line */
} mx;

static int lines_state(void) { return (((mx.orb & mx.ddrb) | ~mx.ddrb) >> 4) & 3; }
static int isolated(void) { return mx.phase == PH_KICK || mx.phase == PH_VXFER; }
int psmac_adb_on(void) { return mx.on; }
int psmac_adb_irq(void) { return mx.on && mx.vflag && (mx.ier & 4); }
int psmac_adb_takes_input(void) { return mx.on; }
int psmac_adb_found(void) { return mx.on && kb.d.seen && ms.d.seen; }   /* the host has found both (the testbed's script waits for it) */

static void listen_finish(void)
{
    if (!mx.listening) return;
    mx.listening = 0;
    int n = mx.listen_n;
    if (mx.listen_first0 && mx.listen_tail && n) n--;        /* a byte written and never clocked out: the next command's */
    dev_listen(mx.listen_addr, mx.listen_reg, mx.listen, n);
}
static void cmd_latch(uint8_t c)
{
    mx.cmd = c; mx.cmd_known = 1; mx.cmds++;
    if (((c >> 2) & 3) == 2) { mx.listening = 1; mx.listen_addr = c >> 4; mx.listen_reg = c & 3; mx.listen_n = 0; mx.listen_first0 = 0; mx.listen_tail = 0; }
}
/* The host has talked to a virtual device's address: from now on the device is the host's to read, alone there or
   beside a real device that answers at the same address (a host that sorts out no collisions, as A/UX: docs/23
   section 23; the real one's answer then goes first, and the virtual one answers when the real one is silent) */
static void dev_found(struct vdev *d, int shared)
{
    if (d->seen) return;
    d->seen = 1;
    kprintf("[PSMAC:ADB] the host talks to the virtual %s at address %d, handler %d%s\n", d == &kb.d ? "keyboard" : "mouse", d->addr, d->handler,
            shared ? ", an address a real device answers at too: they share it" : "");
}
/* the real bus has spoken for the Talk that is out: pb3 = the transceiver's line, 1 = a device answers */
static void decide(int pb3)
{
    int addr = mx.cmd >> 4, reg = mx.cmd & 3;
    struct vdev *d = dev_at(addr);
    if (mx.phase != PH_WATCH) return;
    if (d) dev_found(d, pb3);
    if (d && reg == 0) d->polled = cnt_now() | 1;
    if (pb3) {
        mx.phase = PH_REAL; ct.real++;
        if (d && reg == 3) { d->collided = 1; ct.collisions++; }
        if (!mx.trace_quiet) TRACE("talk %02x (address %d register %d): a real device answers%s\n", mx.cmd, addr, reg, d && reg == 3 ? ", the virtual one there has collided" : "");
        return;
    }
    mx.n = dev_talk(addr, reg, mx.buf);
    mx.srq = srq_wanted(addr);
    mx.bi = 0;
    if (!mx.n && !mx.srq) { mx.phase = PH_REAL; if (!mx.trace_quiet) TRACE("talk %02x (address %d register %d): nobody, and nothing to say here\n", mx.cmd, addr, reg); return; }
    mx.phase = PH_SUBST;
    if (mx.n) { ct.subst++; mx.srq_run = 0; }
    TRACE("talk %02x (address %d register %d): nobody on the real bus; %s %02x %02x%s\n", mx.cmd, addr, reg, mx.n ? "the virtual device answers" : "no data",
          mx.n ? mx.buf[0] : 0xff, mx.n ? mx.buf[1] : 0xff, mx.srq ? ", with a service request" : "");
}
/* the transceiver's line as the 68k reads it during a substitution or one of our own events; real = the wire's */
static int virt_pb3(int real)
{
    if (mx.phase == PH_KICK) return mx.kick == KICK_AUTOPOLL;
    if (mx.phase == PH_VXFER && mx.kick == KICK_SRQ) return 0;
    (void)real;
    if (mx.bi == 1) return mx.srq ? 0 : mx.bi < mx.n;       /* the second byte: low for a service request of ours, never the real line's time-out */
    return mx.bi < mx.n;
}
static void byte_due(void) { mx.vflag = 0; mx.vdue = cnt_now() + us_ticks(BYTE_US); mx.vdue_on = 1; }

#define ADBW_LOCAL   1                      /* nothing goes to the wire */
#define ADBW_SR      2                      /* first the held byte to the real shift register */
#define ADBW_IER     4                      /* first the real shift register interrupt enabled again */
static int isolation_ends(void)
{
    int f = 0;
    if (mx.sr_held) { f |= ADBW_SR; mx.sr_held = 0; }
    if (mx.ier_off) { mx.ier_off = 0; if (mx.ier & 4) f |= ADBW_IER; }
    if (mx.phase == PH_KICK) ct.kick_lost++;
    else {
        uint32_t us = (uint32_t)((cnt_now() - mx.kick_at) * 1000000u / cnt_frq());
        if (us > ct.xfer_us_max) ct.xfer_us_max = us;
    }
    mx.vflag = 0; mx.vdue_on = 0;
    return f;
}
static int state_change(int old, int s)
{
    int f = 0;
    uint64_t now = cnt_now();
    mx.st = s;
    if (s == 0) {                           /* a new command */
        listen_finish();
        if (isolated()) f = isolation_ends();
        mx.phase = PH_REAL; mx.cmd_known = 0; mx.preempted = 0; mx.real_busy = 0;
        if (mx.sr_fresh) cmd_latch(mx.sr);
    } else if (old == 0) {                  /* the command has gone out */
        if (mx.cmd_known && !mx.preempted) {
            int addr = mx.cmd >> 4, kind = (mx.cmd >> 2) & 3;
            static const char *const kn[4] = { "reset/flush", "?", "listen", "talk" };
            mx.last_cmd = mx.cmd; mx.last_valid = 1;
            mx.trace_quiet = mx.traced[mx.cmd] >= 8;
            if (!mx.trace_quiet) { mx.traced[mx.cmd]++; TRACE("command %02x (%s, address %d, register %d), then state %d\n", mx.cmd, kn[kind], addr, mx.cmd & 3, s); }
            if (kind == 3) { if (s != 3 && dev_at(addr)) { mx.phase = PH_WATCH; mx.bi = 0; } }
            else if (kind == 0 && (mx.cmd & 15) == 0) { devices_reset(); for (int i = 0; i < 256; i++) mx.traced[i] = 0; TRACE("SendReset\n"); }
            else if (kind == 0 && (mx.cmd & 15) == 1) { if (addr == kb.d.addr) kb.qh = kb.qt = 0; if (addr == ms.d.addr) ms.dx = ms.dy = 0; }
        } else mx.listening = 0;
        if (s == 3) { listen_finish(); mx.idle_since = now; }
        else if (mx.listening && mx.listen_n) mx.listen_tail = 0;
    } else if (s == 3) {                    /* back to idle */
        listen_finish();
        if (isolated()) f = isolation_ends();
        mx.phase = PH_REAL; mx.real_busy = 0; mx.idle_since = now;
    } else if (old == 3) {                  /* out of idle with no command: the bytes of an event */
        if (mx.phase == PH_KICK) {
            uint32_t us = (uint32_t)((now - mx.kick_at) * 1000000u / cnt_frq());
            if (us > ct.take_us_max) ct.take_us_max = us;
            mx.phase = PH_VXFER; mx.bi = 0; mx.n = 0; mx.srq = 0;
            if (mx.kick == KICK_AUTOPOLL) {
                mx.n = dev_talk(mx.kick_addr, 0, mx.buf); mx.srq = srq_wanted(mx.kick_addr);
                if (mx.n) { ct.autopolls++; mx.srq_run = 0; }
            } else { ct.srqs++; mx.srq_run++; }
            TRACE("the 68k takes our event: %s for address %d: %02x %02x%s\n", mx.kick == KICK_AUTOPOLL ? "the autopoll" : "a service request", mx.kick_addr,
                  mx.n ? mx.buf[0] : 0xff, mx.n ? mx.buf[1] : 0xff, mx.srq ? ", with a service request" : "");
            byte_due();
        } else { mx.real_busy = 1; ct.real_events++; if (ct.real_events <= 16) TRACE("an event of the real transceiver's (the last command %02x)\n", mx.last_cmd); }
    } else {                                /* between the two data states */
        if (mx.phase == PH_SUBST) mx.bi++;
        else if (mx.phase == PH_VXFER) { mx.bi++; byte_due(); }
        else if (mx.phase == PH_WATCH) mx.phase = PH_REAL;
        if (mx.listening) mx.listen_tail = 0;
    }
    mx.sr_fresh = 0; mx.flag_seen = 0;
    return f;
}

/* The 68k writes VIA register reg (core 0, the level lock held). *wire is what goes to the SE's VIA; *pre the
   byte for ADBW_SR. Returns ADBW_* */
int psmac_adb_via_write(uint32_t reg, uint8_t d, uint32_t *wire, uint8_t *pre)
{
    int f = 0;
    switch (reg) {
    case 0: case 2: {
        if (reg == 0) mx.orb = d; else mx.ddrb = d;
        int s = lines_state();
        *pre = mx.sr;
        if (s != mx.st) f = state_change(mx.st, s);
        if (reg == 0 && isolated()) *wire |= 0x30;           /* the real transceiver stays idle */
        return f;
    }
    case 10:
        mx.sr = d; mx.sr_fresh = 1;
        if (mx.st == 0 && !mx.cmd_known) cmd_latch(d);
        else if (mx.listening && mx.st != 3 && mx.listen_n < 8) {
            if (mx.st == 0 && !mx.listen_n) mx.listen_first0 = 1;
            mx.listen[mx.listen_n++] = d; mx.listen_tail = 1;
        }
        if (isolated()) { mx.sr_held = 1; mx.vflag = 0; return ADBW_LOCAL; }
        return 0;
    case 13:
        if (!isolated()) return 0;
        if (d & 4) mx.vflag = 0;
        *wire = d & ~4u;
        return (*wire & 0x7f) ? 0 : ADBW_LOCAL;
    case 14:
        if (d & 0x80) mx.ier |= d & 0x7f; else mx.ier &= ~(d & 0x7f);
        if (!isolated() || !mx.ier_off) return 0;
        *wire = d & ~4u;
        return (*wire & 0x7f) ? 0 : ADBW_LOCAL;
    default: return 0;
    }
}
#define ADBR_LOCAL   1                      /* answered here: *val */
#define ADBR_IFR     2                      /* first the real IFR (psmac_adb_via_feed) */
#define ADBR_PB      4                      /* first the real port B */
int psmac_adb_via_read_pre(uint32_t reg, uint32_t *val)
{
    if (reg == 10 && isolated()) {
        *val = mx.phase == PH_VXFER && mx.bi < mx.n ? mx.buf[mx.bi] : mx.phase == PH_KICK ? 0x00 : 0xff;
        if (mx.vflag && mx.phase == PH_KICK) mx.kick_seen = 1;
        mx.vflag = 0;
        return ADBR_LOCAL;
    }
    if (reg == 10 && mx.phase == PH_WATCH) return ADBR_PB;
    if (reg == 0 && mx.phase == PH_WATCH && !mx.flag_seen) return ADBR_IFR;
    return 0;
}
void psmac_adb_via_feed(uint32_t reg, uint32_t r)
{
    if (reg == 13) { if ((r & 4) && !mx.flag_seen) mx.flag_seen = 1; }
    else if (reg == 0) decide((r >> 3) & 1);
}
/* what the 68k reads, from what the SE's VIA answered */
uint32_t psmac_adb_via_read_post(uint32_t reg, uint32_t r)
{
    switch (reg) {
    case 0:
        if (mx.st == 0 && mx.flag_seen && !(r & 8) && !mx.preempted) { mx.preempted = 1; ct.preempt++; }   /* the transceiver had a byte of its own: the command was not sent */
        if (mx.phase == PH_SUBST && mx.bi == 1 && !(r & 8) && mx.flag_seen == 1) { mx.flag_seen = 2; ct.real_srq++; }   /* a real device's service request beside our answer */
        if (mx.phase == PH_WATCH && mx.flag_seen) decide((r >> 3) & 1);
        if (mx.phase == PH_SUBST || isolated()) r = (r & ~8u) | (virt_pb3((r >> 3) & 1) ? 8u : 0);
        if (isolated()) r = (r & ~0x30u) | (((mx.orb & mx.ddrb) | ~mx.ddrb) & 0x30);
        return r;
    case 10:
        if (mx.phase == PH_SUBST && mx.bi < mx.n) return mx.buf[mx.bi];
        if (mx.phase == PH_REAL && mx.st == 3) mx.real_busy = 1;
        return r;
    case 13:
        if (isolated()) {
            r = (r & 0x7bu) | (mx.vflag ? 4u : 0);
            if (r & mx.ier & 0x7f) r |= 0x80;
            if (mx.vflag && mx.phase == PH_KICK) mx.kick_seen = 1;
        } else if (r & 4) {
            if (!mx.flag_seen) mx.flag_seen = 1;
            if (mx.st == 3) mx.real_busy = 1;
        }
        return r;
    case 14:
        if (mx.ier_off) r = (r & ~4u) | (mx.ier & 4);
        return r;
    default: return r;
    }
}
/* time passes (the level lock held): a virtual byte's interrupt */
void psmac_adb_tick(void)
{
    if (!mx.on || !mx.vdue_on) return;
    if (cnt_now() < mx.vdue) return;
    mx.vdue_on = 0;
    if (mx.phase == PH_VXFER) mx.vflag = 1;
}

/* the service core, between bus cycles: a virtual device with data, and the 68k idle */
void psmac_adb_service(void)
{
    if (!mx.on) return;
    if (mx.vdue_on) { psmac_se_plus_lock(); psmac_adb_tick(); psmac_se_plus_unlock(); }
    uint64_t now = cnt_now();
    if (mx.phase == PH_KICK) {              /* an event the 68k never looked at (interrupts off, no driver): taken back */
        int back = 0;
        if (now - mx.kick_at < us_ticks(500000)) return;
        psmac_se_plus_lock();
        if (mx.phase == PH_KICK && mx.st == 3 && (!mx.kick_seen || now - mx.kick_at > us_ticks(2000000))) {
            mx.phase = PH_REAL; mx.vflag = 0; ct.kick_cancel++;
            back = mx.ier_off && (mx.ier & 4); mx.ier_off = 0;
            mx.next_kick = now + us_ticks(2000000);
        }
        psmac_se_plus_unlock();
        if (back) psmac_wire_write(SE_VIA_REG(14), 1, 0x84, 0);
        return;
    }
    if (mx.phase != PH_REAL || mx.st != 3 || !(mx.ier & 4) || mx.real_busy || now < mx.next_kick) return;
    if (now - mx.idle_since < us_ticks(IDLE_US)) return;
    if (!dev_announce(&kb.d, now) && !dev_announce(&ms.d, now)) return;
    if (psmac_se_floppy_busy()) return;
    /* the real transceiver must have nothing for the 68k: its interrupt off first, then a look at its flag */
    psmac_wire_write(SE_VIA_REG(14), 1, 0x04, 0);
    uint32_t ifr = psmac_wire_read(SE_VIA_REG(13), 1);
    int ok = 0;
    psmac_se_plus_lock();
    if (mx.phase == PH_REAL && mx.st == 3 && (mx.ier & 4) && !mx.real_busy && !(ifr & 4)) {
        int la = mx.last_cmd >> 4;
        struct vdev *d = mx.last_valid && (mx.last_cmd & 15) == 0x0c ? dev_at(la) : 0;
        if (d && dev_announce(d, now)) { mx.kick = KICK_AUTOPOLL; mx.kick_addr = la; ok = 1; }
        else if ((kb.d.srq_en && dev_announce(&kb.d, now)) || (ms.d.srq_en && dev_announce(&ms.d, now))) {
            mx.kick = KICK_SRQ; mx.kick_addr = dev_announce(&kb.d, now) ? kb.d.addr : ms.d.addr; ok = 1;
            /* a host that never comes round to the device: asked less often */
            if (mx.srq_run >= 32) { mx.srq_run = 16; mx.next_kick = now + us_ticks(250000); ok = 0; }
        }
        if (ok) { mx.phase = PH_KICK; mx.kick_seen = 0; mx.kick_at = now; mx.ier_off = 1; mx.sr_held = 0; mx.vdue_on = 0; mx.vflag = 1; }
    } else if (ifr & 4) { ct.held++; mx.next_kick = now + us_ticks(1000); }
    int back = !ok && (mx.ier & 4);
    psmac_se_plus_unlock();
    if (back) psmac_wire_write(SE_VIA_REG(14), 1, 0x84, 0);
}

/* ---- events from the Pi's keyboards and mice (alice_psmac.c: what would have gone into .PiInput's ring) ---- */
void psmac_adb_input(uint32_t type, int32_t a, int32_t b, int32_t c)
{
    if (!mx.on) return;
    psmac_se_plus_lock();
    switch (type) {
    case ALICE_IN_KEY:
        if (b == 2) break;                  /* the Pi side's key repeat: an ADB host repeats keys itself */
        if (!a) { kb.hid_mods = (uint8_t)c; mods_sync(); }
        else if ((uint32_t)a < sizeof hid_to_adb && hid_to_adb[a] != 0xff) key_event(hid_to_adb[a], b != 0, (uint8_t)c);
        break;
    case ALICE_IN_MACKEY:
        if ((uint32_t)a < 0x80 && virt_to_adb((uint8_t)a) != 0xff) key_event(virt_to_adb((uint8_t)a), b != 0, (uint8_t)c);
        else { kb.hid_mods = (uint8_t)c; mods_sync(); }
        break;
    case ALICE_IN_MOUSE:
        if (c != -1) ms.buttons = (uint8_t)(c & 7);
        if (!ms.d.seen) { ct.nohost += (a || b); break; }
        ms_fx += a * ms_speed; ms_fy += b * ms_speed;
        ms.dx += ms_fx / 100; ms_fx %= 100;
        ms.dy += ms_fy / 100; ms_fy %= 100;
        if (ms.dx > 4096) ms.dx = 4096;
        if (ms.dx < -4096) ms.dx = -4096;
        if (ms.dy > 4096) ms.dy = 4096;
        if (ms.dy < -4096) ms.dy = -4096;
        break;
    case ALICE_IN_MOUSEABS:                 /* the remote control's pointer: an ADB mouse cannot be told where to be */
        if (!ct.abs++) kprintf("[PSMAC:ADB] the remote control's pointer has no way through an ADB mouse: its points are dropped (its keys are typed)\n");
        break;
    default: break;
    }
    buttons_settle();
    psmac_se_plus_unlock();
}

/* ---- coming and going ------------------------------------------------------------------------ */
/* the devices appear (the caller holds the level lock, or nothing runs yet) */
static void appear(const char *why, int running)
{
    if (mx.on || adb_mode == 2 || !psmac_board_se || psmac_board_solo || psmac_board_plus) return;
    mx.orb = mx.ddrb = 0; mx.ier = 0;
    if (running) psmac_se_via_guest(&mx.orb, &mx.ddrb, (uint8_t *)&mx.ier);   /* the VIA as the 68k has it now */
    mx.st = lines_state(); mx.phase = PH_REAL; mx.vflag = 0; mx.vdue_on = 0; mx.ier_off = 0; mx.sr_held = 0;
    mx.cmd_known = 0; mx.last_valid = 0; mx.listening = 0; mx.real_busy = 0; mx.srq_run = 0;
    mx.idle_since = cnt_now(); mx.next_kick = 0;
    kb.d.seen = ms.d.seen = 0; kb.d.polled = ms.d.polled = 0; kb.hid_mods = 0; kb.caps = 0; ms.buttons = 0; ms.button = 0; ms.btn_mods = 0; ms_fx = ms_fy = 0;
    devices_reset();
    mx.on = 1;
    kprintf("[PSMAC:ADB] a virtual ADB keyboard (address 2, handler 2) and mouse (address 3, handler 1) on the SE's ADB bus, beside its own: %s; "
            "the Pi's keyboards and mice go to them, not to .PiInput (the mouse at %d%% of its counts: mac.adbspeed)\n", why, (int)ms_speed);
}
void psmac_adb_handoff(void) { if (adb_mode == 0) appear("A/UX's tables are on (mac.adb=auto)", 1); }
/* Linux started directly (psmac_linux.c, docs/27), before the 68k's first instruction: the devices there from the start,
   so that Linux's first scan of the bus finds them beside the SE's own and gives every one an address of its own */
void psmac_adb_linux(void) { if (adb_mode == 0) appear("Linux is started (mac.linux=, mac.adb=auto)", 0); }
/* the 68k's RESET (the level lock held): the SE's VIA is reset with it */
void psmac_adb_reset(void)
{
    if (!mx.on) return;
    mx.on = 0; mx.vflag = 0;
    if (adb_mode == 1) appear("mac.adb=virtual, after the 68k's RESET", 0);
    else kprintf("[PSMAC:ADB] the 68k's RESET: the virtual ADB keyboard and mouse are gone; the Pi's keyboards and mice go to .PiInput again\n");
}
void psmac_adb_options(const char *args)
{
    extern const char *find_token(const char *, const char *);
    extern int psmac_bootarg_val(const char *key, char *out, unsigned max);
    const char *tok; char o[8];
    if ((tok = find_token(args, "mac.adb="))) { tok += 8; adb_mode = tok[0] == 'v' ? 1 : (tok[0] == 'o' || tok[0] == '0') ? 2 : 0; }
    if ((tok = find_token(args, "mac.adbtrace="))) {
        tok += 13; uint32_t n = 0; while (*tok >= '0' && *tok <= '9') n = n * 10 + (uint32_t)(*tok++ - '0'); trace_left = n;
    }
    if ((tok = find_token(args, "mac.adbspeed="))) {
        tok += 13; int32_t n = 0; while (*tok >= '0' && *tok <= '9') n = n * 10 + (*tok++ - '0');
        ms_speed = n < 5 ? 5 : n > 1000 ? 1000 : n;
    }
    if (psmac_bootarg_val("mac.rightbutton", o, sizeof o)) {
        if ((o[0] | 32) == 'c' && (o[1] | 32) == 't') rb_mod = M_CTRL;
        else if ((o[0] | 32) == 'n' || (o[0] | 32) == 'o' || o[0] == '0') rb_mod = 0;
    }
    if (adb_mode == 1) appear("mac.adb=virtual", 0);
}
void psmac_adb_report(void)
{
    if (!mx.on) return;
    kprintf("[PSMAC:ADB] virtual keyboard at %d%s, mouse at %d%s | delivered %u keys, %u mouse reports; dropped %u, %u more before the host found the devices | "
            "%u service requests, %u autopolls, %u of the host's own Talks answered; real devices answered %u, %u collisions, and sent %u events of their own | "
            "events lost to a new command %u, never taken %u, put off for the real transceiver %u; commands the transceiver pre-empted %u, real service requests beside an answer %u | "
            "an event taken after %u us at most, over after %u us at most | state %d/%d, %u commands, last %02x%s\n",
            kb.d.addr, kb.d.seen ? "" : " (not found yet)", ms.d.addr, ms.d.seen ? "" : " (not found yet)",
            ct.keys, ct.reports, ct.dropped, ct.nohost, ct.srqs, ct.autopolls, ct.subst, ct.real, ct.collisions, ct.real_events,
            ct.kick_lost, ct.kick_cancel, ct.held, ct.preempt, ct.real_srq, ct.take_us_max, ct.xfer_us_max, mx.st, mx.phase, mx.cmds, mx.last_cmd,
            kb_has_data() || ms_has_data() ? ", data waiting" : "");
}
