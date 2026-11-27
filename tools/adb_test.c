/* SPDX-License-Identifier: MPL-2.0
 *
 * adb_test.c - the SE board's virtual ADB keyboard and mouse (emu68/src/psmac/psmac_adb.c, docs/08 item 151) on the
 * host. Run by tools/adb_test.sh from `make -C mac check`.
 *
 * Three models around the file under test: the SE's VIA and its ADB transceiver with a real keyboard and a real
 * mouse behind it (the wire); the VIA path of psmac_board_se.c (g_read, g_write); and a host that drives the
 * transceiver as the IIci ROM does (commands, the data states, its poll and the idle state, address resolution,
 * the transceiver's own byte in place of a command). The transceiver counts what a real one should never be
 * shown: the data states entered from idle with no event of its own. Each scene runs twice: with a transceiver
 * that keeps its line low after a time-out (MAME's model) and with one that lowers it for a service request only.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static uint64_t clk;
uint64_t adb_host_now(void) { return clk; }
int psmac_board_se = 1, psmac_board_solo, psmac_board_plus;
static int quiet = 1;
#define kprintf(...) do { if (!quiet) printf(__VA_ARGS__); } while (0)
#define PCCARD_HOST_SUPPORT_H
#include "../emu68/src/psmac/psmac_adb.c"

static int fails, checks;
static void ok(int c, const char *what) { checks++; if (!c) { fails++; printf("FAIL: %s\n", what); } }

/* ---- the wire: the SE's VIA, its transceiver, a real keyboard and mouse ---- */
static struct { uint8_t orb, sr, acr, ier, ifr; } rv;
struct rdev { int addr, handler, has; uint8_t d[2]; };
static struct rdev rk, rm;
static struct {
    int st, mame_like;
    uint8_t data[8]; int n, pos, line_low, srq;
    int listening, laddr, lreg, ln; uint8_t lb[8];
    int pending;                             /* a byte clocked in the idle state: its data waits for state 1 */
    uint8_t last_cmd;
    uint64_t next_poll;
    uint32_t unexpected, preempted, cmds, events;
} tr;
static struct rdev *rdev_at(int a) { return rk.addr == a ? &rk : rm.addr == a ? &rm : 0; }
static int real_talk(int addr, int reg, uint8_t *o)
{
    struct rdev *d = rdev_at(addr);
    if (!d) return 0;
    if (reg == 3) { o[0] = (uint8_t)(0x60 | d->addr); o[1] = (uint8_t)d->handler; return 2; }
    if (reg == 0 && d->has) { o[0] = d->d[0]; o[1] = d->d[1]; d->has = 0; return 2; }
    return 0;
}
static int real_srq(int addr) { return (rk.has && rk.addr != addr) || (rm.has && rm.addr != addr); }
static void tr_state(int s)
{
    int old = tr.st;
    tr.st = s;
    if (s == 0) {
        if (tr.pending) { tr.line_low = 1; tr.preempted++; rv.ifr |= 4; return; }   /* its own byte first: the command is not sent */
        uint8_t c = rv.sr;
        int addr = c >> 4, kind = (c >> 2) & 3;
        tr.cmds++; tr.last_cmd = c; tr.line_low = 0; tr.n = tr.pos = 0; tr.listening = 0; tr.srq = 0;
        if (kind == 3) { tr.n = real_talk(addr, c & 3, tr.data); tr.srq = real_srq(addr); }
        else if (kind == 2) { tr.listening = 1; tr.laddr = addr; tr.lreg = c & 3; tr.ln = 0; }
        else if ((c & 15) == 0) { rk.addr = 2; rm.addr = 3; }
        rv.ifr |= 4;
    } else if (s == 3) {
        tr.line_low = 0; tr.next_poll = clk + 11000;
        if (old == 0 && !tr.listening && (tr.n || tr.srq)) { tr.pos = 0; tr.pending = 1; tr.events++; rv.sr = 0; rv.ifr |= 4; }   /* the Talk's answer, as an event */
    } else {
        if (tr.pending) tr.pending = 0;      /* the data of its own event */
        else if (old == 3) tr.unexpected++;  /* the data states out of idle, and nothing to give */
        if (tr.listening) { if (tr.ln < 8) tr.lb[tr.ln++] = rv.sr; }
        else if (tr.pos < tr.n) { rv.sr = tr.data[tr.pos++]; tr.line_low = tr.pos == 2 && tr.srq; }   /* a service request shows at the second byte */
        else {
            int k = tr.pos++ - tr.n;         /* bytes past the data: the end; with no data, the time-out, the service request, the end */
            rv.sr = 0xff;
            tr.line_low = tr.n || k != 1 || tr.srq || tr.mame_like;
        }
        rv.ifr |= 4;
    }
}
/* the transceiver's autopoll in the idle state: the device last talked to answers, or another one asks for service */
static void tr_time(void)
{
    if (tr.st != 3 || tr.pending || clk < tr.next_poll) return;
    tr.next_poll = clk + 11000;
    if ((tr.last_cmd & 15) != 0x0c) return;
    int addr = tr.last_cmd >> 4;
    int n = real_talk(addr, 0, tr.data), srq = real_srq(addr);
    if (!n && !srq) return;
    tr.n = n; tr.pos = 0; tr.srq = srq; tr.pending = 1; tr.events++;
    rv.sr = 0; rv.ifr |= 4;
}
static void wire_write(uint32_t reg, uint8_t v)
{
    switch (reg) {
    case 0: {
        int old = (rv.orb >> 4) & 3, s = (v >> 4) & 3;
        if (tr.listening && (s == 0 || s == 3) && old != 0) {            /* a Listen's bytes are out: register 3 moves a real device */
            struct rdev *d = rdev_at(tr.laddr);
            if (d && tr.ln >= 2 && tr.lreg == 3 && (tr.lb[1] == 0xfe || tr.lb[1] == 0)) d->addr = tr.lb[0] & 15;
            tr.listening = 0;
        }
        rv.orb = v;
        if (s != old) tr_state(s);
        break;
    }
    case 10: rv.sr = v; rv.ifr &= ~4; break;
    case 11: rv.acr = v; break;
    case 13: rv.ifr &= (uint8_t)~(v & 0x7f); break;
    case 14: if (v & 0x80) rv.ier |= v & 0x7f; else rv.ier &= (uint8_t)~(v & 0x7f); break;
    default: break;
    }
}
static uint8_t wire_read(uint32_t reg)
{
    switch (reg) {
    case 0: return (uint8_t)((rv.orb & 0xf7) | (tr.line_low ? 0 : 8));
    case 10: rv.ifr &= ~4; return rv.sr;
    case 11: return rv.acr;
    case 13: return (uint8_t)(rv.ifr | ((rv.ifr & rv.ier & 0x7f) ? 0x80 : 0));
    case 14: return (uint8_t)(rv.ier | 0x80);
    default: return 0;
    }
}
uint32_t psmac_wire_read(uint32_t addr, int size) { (void)size; return wire_read((addr - 0xEFE1FEu) >> 9); }
void psmac_wire_write(uint32_t addr, int size, uint32_t data, int posted) { (void)size; (void)posted; wire_write((addr - 0xEFE1FEu) >> 9, (uint8_t)data); }
void psmac_se_plus_lock(void) { }
void psmac_se_plus_unlock(void) { }
int psmac_se_floppy_busy(void) { return 0; }
const char *find_token(const char *args, const char *key) { return strstr(args, key); }
int psmac_bootarg_val(const char *key, char *out, unsigned max) { (void)key; (void)out; (void)max; return 0; }

/* ---- psmac_board_se.c's VIA path ---- */
static uint8_t v_orb, v_ddrb, v_ier;
void psmac_se_via_guest(uint8_t *orb, uint8_t *ddrb, uint8_t *ier) { *orb = v_orb; *ddrb = v_ddrb; *ier = v_ier; }
static void g_write(uint32_t reg, uint8_t d)
{
    uint32_t wd = d; uint8_t pre = 0;
    if (reg == 0) v_orb = d; else if (reg == 2) v_ddrb = d;
    else if (reg == 14) { if (d & 0x80) v_ier |= d & 0x7f; else v_ier &= (uint8_t)~(d & 0x7f); }
    if (psmac_adb_on()) {
        int f = psmac_adb_via_write(reg, d, &wd, &pre);
        if (f & 2) wire_write(10, pre);
        if (f & 4) wire_write(14, 0x84);
        if (f & 1) return;
    }
    wire_write(reg, (uint8_t)wd);
}
static uint8_t g_read(uint32_t reg)
{
    int adb = psmac_adb_on();
    if (adb) {
        uint32_t v = 0;
        int f = psmac_adb_via_read_pre(reg, &v);
        if (f & 1) return (uint8_t)v;
        if (f & 6) psmac_adb_via_feed(f & 2 ? 13 : 0, wire_read(f & 2 ? 13 : 0));
    }
    uint32_t r = wire_read(reg);
    if (adb) r = psmac_adb_via_read_post(reg, r);
    return (uint8_t)r;
}
static int irq(void) { return (rv.ifr & rv.ier & 0x7f) != 0 || psmac_adb_irq(); }
static void advance(uint32_t us)
{
    for (uint32_t t = 0; t < us; t += 50) { clk += 50; tr_time(); psmac_adb_tick(); psmac_adb_service(); }
}

/* ---- the host: the IIci ROM's ADB Manager, its VIA half ---- */
static int sr_first;                         /* a driver that reads the shift register before port B */
static int stuck;
static struct { int addr, n; uint8_t b[2]; } rx[4096];
static int rx_n, poll_addr, devmap;
static uint8_t lastcmd;
static void wait_irq(void) { for (int t = 0; t < 400000 && !irq(); t += 50) advance(50); if (!irq()) stuck++; }
static int host_state(void) { return (v_orb >> 4) & 3; }
/* the interrupt's entry: the dispatcher's reads, then the line */
static int sr_int(void) { (void)g_read(13); (void)g_read(14); return (g_read(0) >> 3) & 1; }
static int send_cmd(uint8_t c)               /* 1 = sent; 0 = the transceiver had a byte of its own */
{
    g_write(11, g_read(11) | 0x1c); g_write(10, c); g_write(0, g_read(0) & 0xcf);
    wait_irq();
    return sr_int();
}
static int read_data(uint8_t *b, int *srq, uint8_t toggle)
{
    int n = 0, timeout, line;
    g_write(11, g_read(11) & ~0x10); (void)g_read(10); g_write(0, g_read(0) ^ toggle);
    wait_irq();
    if (sr_first) { b[0] = g_read(10); timeout = !sr_int(); n = 1; g_write(0, g_read(0) ^ 0x30); }
    else { timeout = !sr_int(); b[n++] = g_read(10); g_write(0, g_read(0) ^ 0x30); }
    wait_irq(); line = sr_int();
    if (!line) *srq = 1;
    for (;;) {
        uint8_t x = g_read(10);
        if (n < 8) b[n++] = x;
        g_write(0, g_read(0) ^ 0x30);
        wait_irq(); line = sr_int();
        if (!line || n >= 8) break;
    }
    return timeout ? 0 : n;
}
static void deliver(int addr, const uint8_t *b, int n)
{
    if (!n || rx_n >= 4096) return;
    rx[rx_n].addr = addr; rx[rx_n].n = n; rx[rx_n].b[0] = b[0]; rx[rx_n].b[1] = b[1]; rx_n++;
}
static int next_dev(int a) { do a = (a + 1) & 15; while (!(devmap & (1 << a))); return a; }
static int talk(uint8_t c, uint8_t *b)       /* an explicit Talk: the bytes of its answer */
{
    int srq = 0;
    for (int tries = 0; tries < 8; tries++) {
        if (send_cmd(c)) { lastcmd = c; return read_data(b, &srq, 0x10); }
        uint8_t x[8]; int n = read_data(x, &srq, 0x10);          /* the transceiver's own data, of the command before */
        deliver(lastcmd >> 4, x, n);
    }
    stuck++;
    return 0;
}
static void listen(uint8_t c, uint8_t d0, uint8_t d1)
{
    uint8_t x[8]; int srq = 0;
    while (!send_cmd(c)) { int n = read_data(x, &srq, 0x10); deliver(lastcmd >> 4, x, n); }
    lastcmd = c;
    g_write(10, d0); g_write(0, g_read(0) ^ 0x10); wait_irq(); sr_int();
    g_write(10, d1); g_write(0, g_read(0) ^ 0x30); wait_irq(); sr_int();
}
static void poll(void)                       /* Talk register 0 of the poll address, then idle at once */
{
    for (int tries = 0; tries < 8; tries++) {
        uint8_t c = (uint8_t)(poll_addr << 4 | 0x0c), x[8]; int srq = 0;
        if (send_cmd(c)) {
            lastcmd = c;
            g_write(11, g_read(11) & ~0x10); (void)g_read(10); g_write(0, g_read(0) ^ 0x30);
            return;
        }
        int n = read_data(x, &srq, 0x10);
        deliver(lastcmd >> 4, x, n);
        if (srq) poll_addr = next_dev(poll_addr);
    }
    stuck++;
}
static int host_deaf;                        /* interrupts off */
static void host_run(uint32_t us)
{
    for (uint32_t t = 0; t < us; t += 50) {
        if (irq() && !host_deaf) {
            uint8_t x[8]; int srq = 0;
            if (host_state() != 3) { stuck++; return; }
            sr_int();
            int n = read_data(x, &srq, 0x20);
            deliver(lastcmd >> 4, x, n);
            if (srq) poll_addr = next_dev(poll_addr);
            poll();
        }
        advance(50);
    }
}
static int free_addr(void) { for (int a = 15; a > 0; a--) if (!(devmap & (1 << a))) return a; return -1; }
static void start(int mame_like)
{
    uint8_t b[8];
    memset(&rv, 0, sizeof rv); memset(&tr, 0, sizeof tr); memset(&mx, 0, sizeof mx); memset(&ct, 0, sizeof ct);
    memset(&kb, 0, sizeof kb); memset(&ms, 0, sizeof ms);
    rk.addr = 2; rk.handler = 2; rk.has = 0; rm.addr = 3; rm.handler = 1; rm.has = 0;
    tr.st = 3; tr.mame_like = mame_like; rv.orb = 0x30;
    v_orb = v_ddrb = v_ier = 0; rx_n = 0; stuck = 0; devmap = 0; host_deaf = 0; sr_first = 0;
    adb_mode = 0; psmac_adb_options("mac.adb=virtual mac.adbspeed=100");
    g_write(2, 0x30); g_write(0, 0x30); g_write(14, 0x84);
    talk(0x00, b);                           /* SendReset */
    for (int a = 0; a < 16; a++) if (talk((uint8_t)(a << 4 | 0x0f), b)) devmap |= 1 << a;
    for (int round = 0; round < 3; round++) {
        for (int a = 0; a < 16; a++) {
            if (!(devmap & (1 << a))) continue;
            int f = free_addr();
            listen((uint8_t)(a << 4 | 0x0b), (uint8_t)f, 0xfe);
            if (talk((uint8_t)(a << 4 | 0x0f), b)) devmap |= 1 << f;                       /* two were there: the one that moved stays away */
            else { listen((uint8_t)(f << 4 | 0x0b), (uint8_t)a, 0xfe); if (talk((uint8_t)(f << 4 | 0x0f), b)) devmap |= 1 << f; }
        }
    }
    poll_addr = next_dev(0);
    poll();
}
static int rx_count(int addr) { int n = 0; for (int i = 0; i < rx_n; i++) n += rx[i].addr == addr; return n; }
static int s7(uint8_t v) { return (v & 0x40) ? (int)(v & 0x7f) - 128 : (int)(v & 0x7f); }

static void scene(int mame_like)
{
    char t[120]; const char *m = mame_like ? "MAME's transceiver" : "a transceiver that keeps its line";
#define OK(c, what) do { snprintf(t, sizeof t, "%s: %s", m, what); ok(c, t); } while (0)
    /* address resolution: a real keyboard and mouse at 2 and 3, the virtual ones there too */
    start(mame_like);
    OK(kb.d.seen && ms.d.seen, "the host talked to both virtual devices");
    OK(kb.d.addr != rk.addr && kb.d.addr != rm.addr && ms.d.addr != rk.addr && ms.d.addr != rm.addr && kb.d.addr != ms.d.addr && rk.addr != rm.addr, "four devices at four addresses");
    OK((devmap & (1 << kb.d.addr)) && (devmap & (1 << ms.d.addr)) && (devmap & (1 << rk.addr)) && (devmap & (1 << rm.addr)), "all four in the host's table");
    OK(ct.collisions >= 2, "the virtual devices collided with the real ones");
    OK(!tr.unexpected && !stuck, "resolution: nothing a transceiver should not see, no interrupt missing");

    /* typing: Shift-A */
    rx_n = 0;
    psmac_adb_input(ALICE_IN_KEY, 0x04, 1, 0x02); psmac_adb_input(ALICE_IN_KEY, 0x04, 0, 0x02); psmac_adb_input(ALICE_IN_KEY, 0, 0, 0);
    host_run(200000);
    { uint8_t got[8]; int n = 0;
      for (int i = 0; i < rx_n; i++) if (rx[i].addr == kb.d.addr) { for (int k = 0; k < 2; k++) if (rx[i].b[k] != 0xff && n < 8) got[n++] = rx[i].b[k]; }
      OK(n == 4 && got[0] == 0x38 && got[1] == 0x00 && got[2] == 0x80 && got[3] == 0xb8, "Shift down, A down, A up, Shift up, in that order"); }
    OK(!tr.unexpected && !stuck, "typing: nothing a transceiver should not see");
    OK(ct.keys == 4 && !ct.dropped, "four key transitions delivered, none dropped");

    /* the mouse: more counts than one report holds */
    rx_n = 0;
    psmac_adb_input(ALICE_IN_MOUSE, 200, -100, -1);
    host_run(200000);
    { int dx = 0, dy = 0, big = 0;
      for (int i = 0; i < rx_n; i++) if (rx[i].addr == ms.d.addr) { dx += s7(rx[i].b[1]); dy += s7(rx[i].b[0]); if (s7(rx[i].b[1]) > 63 || s7(rx[i].b[1]) < -64) big = 1; }
      OK(dx == 200 && dy == -100 && !big && rx_count(ms.d.addr) >= 4, "200 right and 100 up, over several reports"); }

    /* the right button: Command down, the button, the button up, Command up */
    rx_n = 0;
    psmac_adb_input(ALICE_IN_MOUSE, 0, 0, 2); host_run(100000);
    psmac_adb_input(ALICE_IN_MOUSE, 0, 0, 0); host_run(100000);
    { int seq[8], n = 0;
      for (int i = 0; i < rx_n && n < 8; i++) {
          if (rx[i].addr == kb.d.addr) seq[n++] = rx[i].b[0];
          else if (rx[i].addr == ms.d.addr) seq[n++] = (rx[i].b[0] & 0x80) ? 0x1ff : 0x100;
      }
      OK(n == 4 && seq[0] == 0x37 && seq[1] == 0x100 && seq[2] == 0x1ff && seq[3] == 0xb7, "the right button: Command down, button down, button up, Command up"); }

    /* Caps Lock: a locking key */
    rx_n = 0;
    psmac_adb_input(ALICE_IN_KEY, 0x39, 1, 0); psmac_adb_input(ALICE_IN_KEY, 0x39, 0, 0); host_run(100000);
    psmac_adb_input(ALICE_IN_KEY, 0x39, 1, 0); psmac_adb_input(ALICE_IN_KEY, 0x39, 0, 0); host_run(100000);
    OK(rx_count(kb.d.addr) == 2 && rx[0].b[0] == 0x39 && rx[rx_n - 1].b[0] == 0xb9, "Caps Lock: down at the first press, up at the second");

    /* the real mouse and the virtual one at once: every report of both, each once */
    rx_n = 0;
    { int real_sent = 0, vsum = 0, rsum = 0, rn = 0;
      for (int i = 0; i < 400; i++) {
          if (!rm.has && real_sent < 40) { rm.has = 1; rm.d[0] = 0x80; rm.d[1] = (uint8_t)(0x80 | 3); real_sent++; }
          if (i < 200) psmac_adb_input(ALICE_IN_MOUSE, 1, 0, -1);
          host_run(1500);
      }
      host_run(300000);
      for (int i = 0; i < rx_n; i++) { if (rx[i].addr == ms.d.addr) vsum += s7(rx[i].b[1]); if (rx[i].addr == rm.addr) { rsum += s7(rx[i].b[1]); rn++; } }
      if (!quiet) printf("both: vsum %d rn %d real_sent %d rsum %d has %d | preempted %u events %u unexpected %u | ct.preempt %u held %u lost %u real_events %u srq %u\n", vsum, rn, real_sent, rsum, rm.has, tr.preempted, tr.events, tr.unexpected, ct.preempt, ct.held, ct.kick_lost, ct.real_events, ct.srqs);
      OK(vsum == 200, "both mice moving: the virtual mouse's 200 counts all arrive");
      OK(rn == real_sent && rsum == 3 * real_sent && !rm.has, "both mice moving: every report of the real mouse arrives once");
      OK(!tr.unexpected && !stuck, "both mice moving: nothing a transceiver should not see"); }

    /* a real byte that arrives while a virtual event is being read waits, and is not lost */
    rx_n = 0;
    { uint32_t held0 = ct.preempt + ct.held, got = 0;
      for (int k = 0; k < 60; k++) {
          psmac_adb_input(ALICE_IN_MOUSE, 2, 0, -1);
          for (int t = 0; t < 40000 && !isolated(); t += 50) host_run(50);
          if (isolated()) { rm.has = 1; rm.d[0] = 0x80; rm.d[1] = 0x81; tr.next_poll = clk; tr_time(); }
          host_run(60000);
      }
      for (int i = 0; i < rx_n; i++) got += rx[i].addr == rm.addr;
      OK(got == 60 && !rm.has, "a real report clocked in during a virtual event is delivered afterwards, every time");
      OK(ct.preempt + ct.held > held0, "and it was met as the transceiver's own byte in place of a command");
      OK(!tr.unexpected && !stuck, "nothing a transceiver should not see"); }

    /* a host that does not answer: the event is taken back, the real interrupt enable restored */
    rx_n = 0;
    host_deaf = 1;
    psmac_adb_input(ALICE_IN_KEY, 0x05, 1, 0); psmac_adb_input(ALICE_IN_KEY, 0x05, 0, 0);
    host_run(700000);
    OK(ct.kick_cancel >= 1 && (rv.ier & 4) && !psmac_adb_irq() && mx.phase == PH_REAL, "an event nobody takes is taken back, the real chip's interrupt on again");
    host_deaf = 0;
    host_run(3000000);
    OK(rx_count(kb.d.addr) >= 1 && ct.keys >= 2 && !kb_has_data(), "and it is offered again and taken once the host listens");

    /* a driver that reads the shift register before the line, in an explicit Talk with its data states */
    { uint8_t b[8]; int n;
      host_run(50000);
      sr_first = 1;
      psmac_adb_input(ALICE_IN_KEY, 0x06, 1, 0);
      host_deaf = 1; advance(100);           /* before any event of ours */
      n = talk((uint8_t)(kb.d.addr << 4 | 0x0c), b);
      OK((n >= 2 && b[0] == 0x08) || rx_count(kb.d.addr), "an explicit Talk register 0 gets the key (shift register read before the line)");
      n = talk((uint8_t)(kb.d.addr << 4 | 0x0e), b);
      OK(n >= 2 && b[0] == 0xff && (b[1] & 0xf8) == 0xf8, "Talk register 2: no modifier down");
      n = talk((uint8_t)(rk.addr << 4 | 0x0f), b);
      OK(n >= 2 && b[1] == 2 && (b[0] & 15) == rk.addr, "the real keyboard's register 3 passes through");
      n = talk((uint8_t)(1 << 4 | 0x0f), b);
      OK(n == 0, "an empty address stays empty");
      sr_first = 0; host_deaf = 0; poll(); host_run(100000); }

    /* mac.adbspeed: half the counts, none lost to rounding */
    rx_n = 0; ms_speed = 50; ms_fx = ms_fy = 0;
    for (int i = 0; i < 101; i++) psmac_adb_input(ALICE_IN_MOUSE, 1, -1, -1);
    host_run(200000);
    { int dx = 0, dy = 0;
      for (int i = 0; i < rx_n; i++) if (rx[i].addr == ms.d.addr) { dx += s7(rx[i].b[1]); dy += s7(rx[i].b[0]); }
      OK(dx == 50 && dy == -50, "mac.adbspeed=50: 101 counts become 50"); }
    ms_speed = 100;
    /* the 68k's RESET in the middle of everything */
    psmac_adb_input(ALICE_IN_MOUSE, 5, 5, -1);
    for (int t = 0; t < 40000 && !isolated(); t += 50) host_run(50);
    psmac_adb_reset();
    OK(mx.on && mx.phase == PH_REAL && !psmac_adb_irq() && !kb.d.seen && kb.d.addr == 2 && ms.d.addr == 3, "RESET: the devices back at 2 and 3, nothing pending");
#undef OK
}

/* A host as A/UX 3.1.1 showed itself in the testbed (docs/23 section 23): SendReset, Talk register 3, Flush and Listen
   register 3 at addresses 2 and 3 and nowhere else, no collisions sorted out, then Talk register 0 of 2 and of 3 in
   turn, each with its data states. The virtual devices share 2 and 3 with the real ones, or are alone there. */
static void scene_aux(int mame_like, int with_real)
{
    char t[240]; uint8_t b[8]; int n;
#define OK(c, what) do { snprintf(t, sizeof t, "a host that polls 2 and 3 (%s, %s): %s", mame_like ? "MAME's transceiver" : "a transceiver that keeps its line", with_real ? "real devices there too" : "no real devices", what); ok(c, t); } while (0)
    memset(&rv, 0, sizeof rv); memset(&tr, 0, sizeof tr); memset(&mx, 0, sizeof mx); memset(&ct, 0, sizeof ct);
    memset(&kb, 0, sizeof kb); memset(&ms, 0, sizeof ms);
    rk.addr = with_real ? 2 : -1; rk.handler = 2; rk.has = 0; rm.addr = with_real ? 3 : -1; rm.handler = 1; rm.has = 0;
    tr.st = 3; tr.mame_like = mame_like; rv.orb = 0x30; rv.ier = 4;
    v_orb = 0x30; v_ddrb = 0x30; v_ier = 4; rx_n = 0; stuck = 0; host_deaf = 0; sr_first = 0;
    adb_mode = 0; psmac_adb_options("mac.board=se mac.adbspeed=100"); psmac_adb_handoff();
    talk(0x00, b);
    for (int a = 2; a <= 3; a++) {
        n = talk((uint8_t)(a << 4 | 0x0f), b);
        OK(n >= 2, "Talk register 3 answered");
        send_cmd((uint8_t)(a << 4 | 0x01));                      /* Flush, and state 2 after it, as that host does */
        (void)g_read(10); g_write(0, g_read(0) ^ 0x20); wait_irq(); sr_int();
        listen((uint8_t)(a << 4 | 0x0b), (uint8_t)(0x20 | a), 0x01);
    }
    OK(kb.d.seen && ms.d.seen && kb.d.addr == 2 && ms.d.addr == 3 && kb.d.handler == 2, "both virtual devices the host's, where they were");
    if (with_real) OK(rk.addr == 2 && rm.addr == 3, "the real ones too");
    psmac_adb_input(ALICE_IN_KEY, 0x04, 1, 0x02); psmac_adb_input(ALICE_IN_KEY, 0x04, 0, 0x02); psmac_adb_input(ALICE_IN_KEY, 0, 0, 0);
    psmac_adb_input(ALICE_IN_MOUSE, 150, 0, -1);
    int real_k = 0, real_m = 0, vk = 0, vsum = 0, rsum = 0; uint8_t got[8];
    for (int i = 0; i < 60; i++) {
        if (with_real && i == 3) { rk.has = 1; rk.d[0] = 0x31; rk.d[1] = 0xff; }
        if (with_real && i == 5) { rm.has = 1; rm.d[0] = 0x80; rm.d[1] = 0x85; }
        n = talk(0x2c, b);
        if (n >= 2) { if (b[0] == 0x31) real_k++; else for (int k = 0; k < 2; k++) if (b[k] != 0xff && vk < 8) got[vk++] = b[k]; }
        n = talk(0x3c, b);
        if (n >= 2) { if (b[1] == 0x85) { real_m++; rsum += 5; } else vsum += s7(b[1]); }
        advance(8000);
    }
    OK(vk == 4 && got[0] == 0x38 && got[1] == 0x00 && got[2] == 0x80 && got[3] == 0xb8, "Shift-A from the virtual keyboard at address 2");
    OK(vsum == 150, "150 counts from the virtual mouse at address 3");
    if (with_real) OK(real_k == 1 && real_m == 1 && !rk.has && !rm.has, "the real keyboard's key and the real mouse's report, each once");
    if (!quiet) printf("aux: unexpected %u stuck %d dropped %u nohost %u lost %u\n", tr.unexpected, stuck, ct.dropped, ct.nohost, ct.kick_lost);
    OK(!tr.unexpected && !stuck && !ct.dropped, "nothing a transceiver should not see, nothing dropped");
    OK(!ct.kick_lost && !ct.srqs && !ct.autopolls && ct.subst >= 4, "a host that polls is answered in its own Talks, never interrupted in between");
#undef OK
}

int main(int argc, char **argv)
{
    quiet = !(argc > 1 && argv[1][0] == 'v');
    scene(1);
    scene(0);
    for (int k = 0; k < 4; k++) scene_aux(k & 1, k >> 1);
    /* mac.adb=off and auto: nothing exists until the hand-over */
    memset(&mx, 0, sizeof mx); adb_mode = 0; psmac_adb_options("mac.adb=off"); psmac_adb_handoff();
    ok(!psmac_adb_on() && !psmac_adb_takes_input(), "mac.adb=off: never");
    adb_mode = 0; psmac_adb_options("mac.board=se");
    ok(!psmac_adb_on(), "mac.adb=auto: not before A/UX's tables");
    psmac_adb_handoff();
    ok(psmac_adb_on() && psmac_adb_takes_input(), "mac.adb=auto: from the hand-over");
    psmac_adb_reset();
    ok(!psmac_adb_on(), "mac.adb=auto: gone with the 68k's RESET");
    printf("adb_test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
