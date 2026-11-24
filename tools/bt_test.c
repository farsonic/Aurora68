/* SPDX-License-Identifier: GPL-2.0-only
 *
 * bt_test.c - the kernel's Bluetooth host (emu68/src/alice/bt/bthost.c and btle.c) on the PC, against a scripted
 * controller: the parts of it the testbed's QEMU cannot reach (it has no Bluetooth radio). Run by
 * tools/bt_test.sh from `make -C mac check`. GPL-2.0-only like the code it tests.
 *
 *   - AES-128 against FIPS-197's example, and the address hash ah against the Core specification's (Vol 3 Part H,
 *     appendix D.7);
 *   - a controller without the resolving list (Bluetooth 4.1, the Pi 3B's BCM43438): a bonded LE mouse that
 *     advertises with a resolvable private address is found by the host's own scan, connected at that address and
 *     encrypted with its stored key (docs/08 item 136: "Bluetooth mouse stopped reconnecting on the Pi 3B");
 *     an address that does not resolve is left alone; a controller with the resolving list keeps the accept list;
 *   - the owner's pair (24 November 2026, docs/08 item 139): an LE keyboard with an IRK connected and an LE mouse with
 *     a static random address and no IRK away, on the 4.1 controller: the host's own scan goes on for the mouse (the
 *     accept list, which the BCM43438 never answered with a link up, is not armed), hears its ADV_IND, connects to it
 *     and encrypts with its key; the heartbeat's words for it (bt_key_activity) and what was heard; the scan started
 *     afresh every 30 s while nothing is heard; a failed connection noted against the device;
 *   - the firmware sent at 921600 baud and the line back at 115200 after Launch_RAM; a refusal keeps 115200;
 *   - the radio's time (docs/08 item 140: WiFi got no address on the owner's Pi 3B with Bluetooth on): the page scan
 *     only while a paired Classic device is away, "fast connectable" for the first minute and then the default 1.28 s;
 *     the host's own LE scan 30 ms per 160 ms for that minute, then 30 ms per 1.28 s, then only a 10 s sweep in every five
 *     minutes when nothing has been heard for five minutes; a link going down brings the fast minute back at most once
 *     per three minutes; the WiFi joining or its DHCP quiets every scan for 10 s, at most two thirds of the time;
 *     bt.scan=fast keeps it fast, bt.scan=off keeps the radio quiet until a Connect; the shares bt_radio_now reports;
 *   - the lock-up hunt and Raspberry Pi OS's parameters (25 November 2026, docs/21 and docs/08 item 141): LE connections
 *     asked for at Linux's 30 to 50 ms, a direct connection's scan 30 ms per 120 ms; a device's Connection Parameter
 *     Update Request checked as Linux does (refused when invalid) and its interval floored at 15 ms where its range
 *     allows; the link's interval from LE Connection Complete and Update Complete; a Command Complete lost on the UART
 *     no longer stops every later command (2 s, Linux's HCI_CMD_TIMEOUT); ACL packets in flight handed back when their
 *     link goes and its queued ones dropped, the pool never above the controller's count; an H4 header that makes no
 *     sense dropped and the stream searched again, a packet cut by a UART error dropped; an LE Rand answer not taken
 *     for an LE Encrypt.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "bthost.c"                 /* the host itself, so that the tests reach its queue and helpers (since 25 November 2026) */

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the scripted controller ---- */
static uint32_t t_ms;
#define MAXOUT 512
static struct { uint8_t b[300]; unsigned n; } out[MAXOUT];
static unsigned nout, answered;
static uint32_t bauds[8]; static int nbauds;
static int verbose;
static uint8_t res_status;                       /* what the resolving list's commands answer: 0, or 0x01 (unknown command) */
static uint8_t baud_status;
static int drop_answers;                         /* the next N commands get no answer at all (lost on the UART) */

static void io_send(void *c, const uint8_t *p, unsigned n) { (void)c; if (nout < MAXOUT && n <= sizeof out[0].b) { memcpy(out[nout].b, p, n); out[nout].n = n; nout++; } }
static void io_baud(void *c, uint32_t b) { (void)c; if (nbauds < 8) bauds[nbauds++] = b; }
static uint32_t io_ms(void *c) { (void)c; return t_ms; }
static void io_log(void *c, const char *l) { (void)c; if (verbose) printf("  %s\n", l); }
static int keys_seen, kb_seen;
static void io_kb(void *c, const uint8_t r[8]) { (void)c; (void)r; kb_seen++; }
static void io_mouse(void *c, int dx, int dy, int w, unsigned b) { (void)c; (void)dx; (void)dy; (void)w; (void)b; }
static void io_keys(void *c) { (void)c; keys_seen++; }
static struct bt_io io = { 0, io_send, io_baud, io_ms, io_log, io_kb, io_mouse, io_keys, 0, 0 };
static struct bt_host H;

static uint16_t op_of(unsigned i) { return out[i].b[0] == 1 ? (uint16_t)(out[i].b[1] | out[i].b[2] << 8) : 0; }
static void ev(const uint8_t *e, unsigned n) { uint8_t b[300]; b[0] = 4; memcpy(b + 1, e, n); bt_rx(&H, b, n + 1); }
static void cc(uint16_t op, const uint8_t *ret, unsigned n)
{
    uint8_t e[260] = { 0x0e, (uint8_t)(3 + n), 1, (uint8_t)op, (uint8_t)(op >> 8) };
    memcpy(e + 5, ret, n);
    ev(e, 5 + n);
}
/* every command not answered yet gets its Command Complete (and what a real controller sends with it) */
static void answer(void)
{
    while (answered < nout) {
        unsigned i = answered++;
        uint16_t op = op_of(i);
        if (!op) continue;                                  /* ACL */
        if (drop_answers > 0) { drop_answers--; continue; }
        uint8_t r[16]; memset(r, 0, sizeof r);
        unsigned n = 1;
        switch (op) {
        case 0x1009: n = 7; r[1] = 0x01; r[2] = 0x02; r[3] = 0x03; r[4] = 0x04; r[5] = 0x05; r[6] = 0x06; break;   /* BD_ADDR */
        case 0x1005: n = 8; r[1] = 0xfd; r[2] = 0x03; r[3] = 64; r[4] = 8; r[6] = 1; break;                       /* buffer size */
        case 0x1001: n = 9; r[1] = 7; break;                                                                    /* version: 4.1 */
        case 0x2002: n = 4; r[1] = 27; r[3] = 8; break;                                                         /* LE buffers */
        case 0xfc18: r[0] = baud_status; break;
        case 0x2027: case 0x2029: case 0x202d: r[0] = res_status; break;
        case 0x0401: case 0x200d: case 0x2019: {                                                                 /* answered by Command Status */
            uint8_t e[6] = { 0x0f, 4, 0, 1, (uint8_t)op, (uint8_t)(op >> 8) }; ev(e, 6); continue; }
        default: break;
        }
        cc(op, r, n);
        if (op == 0x200e && !r[0]) {                        /* a cancelled LE Create Connection ends with status 0x02 */
            uint8_t e[21] = { 0x3e, 19, 0x01, 0x02 }; ev(e, 21);
        }
    }
}
static void run(uint32_t ms) { for (uint32_t k = 0; k < ms; k += 10) { answer(); bt_poll(&H); answer(); t_ms += 10; } }
static int find_op(uint16_t op, unsigned from) { for (unsigned i = from; i < nout; i++) if (op_of(i) == op) return (int)i; return -1; }
static int count_op(uint16_t op, unsigned from) { int c = 0; for (unsigned i = from; i < nout; i++) if (op_of(i) == op) c++; return c; }
static void reset_out(void) { nout = answered = 0; nbauds = 0; }
static int last_op(uint16_t op) { int r = -1; for (unsigned i = 0; i < nout; i++) if (op_of(i) == op) r = (int)i; return r; }
static unsigned le16v(const uint8_t *p) { return (unsigned)(p[0] | p[1] << 8); }

/* the spec's IRK (D.7: ec0234a3 57c8ad05 341010a6 0a397d9b, most significant first) as SMP gives it */
static uint8_t irk_lsb[16];
static const uint8_t ident[6] = { 0x55, 0x44, 0x33, 0x22, 0x11, 0xc0 };           /* C0:11:22:33:44:55, a static random one */
static const uint8_t rpa[6] = { 0xaa, 0xfb, 0x0d, 0x94, 0x81, 0x70 };             /* prand 708194, hash 0dfbaa */
static const uint8_t ltk[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
static const uint8_t rnd8[8] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };

static void to_ready(const uint8_t *fw, unsigned fwlen)
{
    reset_out();
    memset(&H, 0, sizeof H);
    bt_add_le_key(&H, ident, 1, ltk, 0x1234, rnd8, irk_lsb, BTK_MOUSE);
    bt_init(&H, &io, fw, fwlen);
    run(3000);
}
static void adv(uint8_t evt, uint8_t atype, const uint8_t a[6])
{
    uint8_t e[20] = { 0x3e, 12, 0x02, 1, evt, atype };
    memcpy(e + 6, a, 6); e[12] = 0; e[13] = (uint8_t)-60;
    ev(e, 14);
}
static void le_conn_h(uint8_t st, uint8_t atype, const uint8_t a[6], uint8_t handle)
{
    uint8_t e[21] = { 0x3e, 19, 0x01, st, handle, 0x00, 0x00, atype };
    memcpy(e + 8, a, 6);
    ev(e, 21);
}
static void le_conn(uint8_t st, uint8_t atype, const uint8_t a[6]) { le_conn_h(st, atype, a, 0x40); }

int main(int argc, char **argv)
{
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    static const uint8_t irk_msb[16] = { 0xec, 0x02, 0x34, 0xa3, 0x57, 0xc8, 0xad, 0x05, 0x34, 0x10, 0x10, 0xa6, 0x0a, 0x39, 0x7d, 0x9b };
    for (int i = 0; i < 16; i++) irk_lsb[i] = irk_msb[15 - i];

    /* FIPS-197 appendix C.1 */
    {
        uint8_t k[16], p[16], o[16];
        static const uint8_t want[16] = { 0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30, 0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a };
        for (int i = 0; i < 16; i++) { k[i] = (uint8_t)i; p[i] = (uint8_t)(i * 0x11); }
        bt_aes128(k, p, o);
        CHECK(!memcmp(o, want, 16), "AES-128 of FIPS-197 C.1");
    }
    /* ah, Core Vol 3 Part H D.7: prand 708194 gives 0dfbaa */
    CHECK(bt_rpa_match(irk_lsb, rpa), "the spec's resolvable address resolves with its IRK");
    { uint8_t b[6]; memcpy(b, rpa, 6); b[0] ^= 1; CHECK(!bt_rpa_match(irk_lsb, b), "a wrong hash does not resolve"); }
    { uint8_t b[6]; memcpy(b, rpa, 6); b[5] = 0xf0; CHECK(!bt_rpa_match(irk_lsb, b), "a static address is not taken for a private one"); }

    /* a 4.1 controller: the resolving list's commands are unknown to it */
    res_status = 0x01;
    to_ready(0, 0);
    CHECK(H.state == BT_READY, "the host is ready (state %d, step %d)", H.state, H.step);
    CHECK(find_op(0x0c1a, 0) < 0 && !H.ps_on, "only an LE device paired: no page scan (as Linux: nothing Classic to wake for)");
    CHECK(H.no_resolv == 1, "the missing resolving list is noticed");
    CHECK(H.bg_scan == 1, "the host's own scan runs for the bonded mouse");
    {
        int sp = -1; for (unsigned i = 0; i < nout; i++) if (op_of(i) == 0x200b) sp = (int)i;
        CHECK(sp >= 0 && out[sp].b[4] == 0, "a passive scan");
        CHECK(H.le_init == 0, "no accept-list connection left out (le_init %d)", H.le_init);
    }
    unsigned mark = nout;
    { uint8_t other[6]; memcpy(other, rpa, 6); other[1] ^= 0x40; adv(0x00, 1, other); run(50); }
    CHECK(find_op(0x200d, mark) < 0, "an address that does not resolve is left alone");
    mark = nout;
    adv(0x01, 1, rpa);                                       /* ADV_DIRECT_IND from the private address */
    run(50);
    {
        int c = find_op(0x200d, mark);
        CHECK(c >= 0 && out[c].b[8] == 0 && out[c].b[9] == 1 && !memcmp(out[c].b + 10, rpa, 6), "connected at the address it advertised (policy %d, type %d)",
              c >= 0 ? out[c].b[8] : -1, c >= 0 ? out[c].b[9] : -1);
        CHECK(H.le_init == 3 && !H.bg_scan, "the scan stopped while it connects");
    }
    mark = nout;
    le_conn(0, 1, rpa);
    run(50);
    {
        int e = find_op(0x2019, mark);
        CHECK(e >= 0 && !memcmp(out[e].b + 6, rnd8, 8) && out[e].b[14] == 0x34 && out[e].b[15] == 0x12 && !memcmp(out[e].b + 16, ltk, 16),
              "encrypted with the mouse's stored key (Rand, EDIV, LTK)");
        int c = -1; for (int i = 0; i < BT_MAXCONN; i++) if (H.conn[i].used) c = i;
        CHECK(c >= 0 && !memcmp(H.conn[c].addr, ident, 6), "the link is the bonded mouse's, by its identity address");
        CHECK(find_op(0x0406, mark) < 0, "not disconnected as unknown");
    }
    /* the link goes: the scan comes back */
    { uint8_t e[6] = { 0x05, 4, 0, 0x40, 0x00, 0x08 }; ev(e, 6); }
    run(1500);
    CHECK(H.bg_scan == 1, "the host's scan again once the mouse has gone (supervision timeout)");
    /* a directed advertisement that gets no answer: cancelled, the scan again */
    mark = nout; adv(0x00, 1, rpa); run(5000);
    CHECK(find_op(0x200e, mark) >= 0 && H.bg_scan == 1, "a connection that is not answered in 4 s is cancelled and the scan resumes");

    /* the owner's pair on the 4.1 controller: the keyboard (with an IRK) comes back, the mouse (static random, no IRK) is away */
    {
        static const uint8_t ltk2[16] = { 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1 };
        static const uint8_t mouse[6] = { 0x55, 0x44, 0x33, 0x22, 0x11, 0xc4 };    /* C4:11:22:33:44:55: static random */
        res_status = 0x01;
        reset_out();
        memset(&H, 0, sizeof H);
        bt_add_le_key(&H, ident, 1, ltk, 0x1234, rnd8, irk_lsb, BTK_KEYBOARD);     /* the keyboard: identity C0:..., its IRK */
        bt_add_le_key(&H, mouse, 1, ltk2, 0x4321, rnd8, 0, BTK_MOUSE);
        bt_init(&H, &io, 0, 0);
        run(3000);
        CHECK(H.state == BT_READY && H.no_resolv && H.bg_scan, "two LE devices away: the host's own scan");
        { uint32_t ms; CHECK(!strcmp(bt_key_activity(&H, 1, &ms), "LE: the host's own scan listens for it"), "the heartbeat: the scan listens for the mouse"); }
        CHECK(H.keys[1].heard_n == 0, "the mouse not heard yet");
        /* the keyboard advertises from a private address: connected */
        mark = nout; adv(0x00, 1, rpa); run(50);
        le_conn(0, 1, rpa); run(50);
        CHECK(find_op(0x2019, mark) >= 0, "the keyboard connected and encrypted");
        CHECK(H.keys[0].heard_n == 1 && H.keys[0].heard_evt == 0 && !memcmp(H.keys[0].heard_addr, rpa, 6), "the keyboard's advertisement noted, at its private address");
        /* only the mouse is away now: until 24 November 2026 the accept list took over here (le_init 2) */
        mark = nout; run(2000);
        CHECK(H.bg_scan == 1 && H.le_init != 2, "the mouse alone away: the host's scan goes on, the accept list stays off (le_init %d, scan %d)", H.le_init, H.bg_scan);
        CHECK(count_op(0x200d, mark) == 0, "no LE Create Connection to the accept list");
        /* nothing heard for a while: the scan starts afresh, so that its duplicate filter forgets */
        run(31000);
        CHECK(H.bg_restarts >= 1 && H.bg_scan == 1, "the scan started again after 30 s (%d restarts)", H.bg_restarts);
        /* the mouse wakes and advertises from its static address: connected directly, encrypted with its key */
        uint32_t tries0 = H.keys[1].tries;
        mark = nout; adv(0x00, 1, mouse); run(50);
        {
            int c = find_op(0x200d, mark);
            CHECK(c >= 0 && out[c].b[8] == 0 && out[c].b[9] == 1 && !memcmp(out[c].b + 10, mouse, 6), "LE Create Connection to the mouse's address (policy %d)", c >= 0 ? out[c].b[8] : -1);
            CHECK(H.keys[1].heard_n == 1 && H.keys[1].heard_atype == 1 && H.keys[1].tries == tries0 + 1, "the mouse heard once, one connection asked for");
            uint32_t ms; CHECK(!strcmp(bt_key_activity(&H, 1, &ms), "LE: connecting to it at the address it advertised"), "the heartbeat: connecting to it");
        }
        /* the first try fails (the controller says why): noted against the mouse; the scan again, the second try links */
        mark = nout; le_conn_h(0x3e, 1, mouse, 0x41); run(500);
        CHECK(H.keys[1].err_what == BT_KE_CONN && H.keys[1].err_code == 0x3e, "the failed connection is the mouse's last error (%d, %d)", H.keys[1].err_what, H.keys[1].err_code);
        CHECK(H.bg_scan == 1, "the scan again after the failure");
        mark = nout; adv(0x00, 1, mouse); run(50); le_conn_h(0, 1, mouse, 0x41); run(50);
        {
            int e = find_op(0x2019, mark);
            CHECK(e >= 0 && out[e].b[14] == 0x21 && out[e].b[15] == 0x43 && !memcmp(out[e].b + 16, ltk2, 16), "the mouse encrypted with its own key (EDIV 4321)");
            int n = 0; for (int i = 0; i < BT_MAXCONN; i++) n += H.conn[i].used;
            CHECK(n == 2, "two links up (%d)", n);
            CHECK(!H.bg_scan, "nothing away: the scan stops");
        }
    }

    /* a 4.2 controller: its accept list and resolving list do the work */
    res_status = 0;
    to_ready(0, 0);
    CHECK(H.state == BT_READY && !H.no_resolv && !H.bg_scan, "with the resolving list: no scan of the host's");
    { int c = find_op(0x200d, 0); CHECK(c >= 0 && out[c].b[8] == 1 && H.le_init == 2, "the accept list connects"); }
    CHECK(count_op(0x2027, 0) == 1, "the IRK into the resolving list");

    /* the firmware at 921600 baud: a small .hcd of two records, the last Launch_RAM */
    static const uint8_t hcd[] = { 0x4c, 0xfc, 0x05, 0x00, 0x00, 0x21, 0x00, 0xaa, 0x4e, 0xfc, 0x04, 0xff, 0xff, 0xff, 0xff };
    io.fw_baud = 921600; baud_status = 0;
    to_ready(hcd, sizeof hcd);
    {
        int b = find_op(0xfc18, 0), m = find_op(0xfc2e, 0), l = find_op(0xfc4e, 0), r2 = l >= 0 ? find_op(0x0c03, (unsigned)l) : -1;
        CHECK(b >= 0 && out[b].b[3] == 6 && out[b].b[6] == 0x00 && out[b].b[7] == 0x10 && out[b].b[8] == 0x0e, "Update UART Baud Rate 921600");
        CHECK(b >= 0 && m > b && l > m && r2 > l, "the speed, the minidriver, the records, the reset");
        CHECK(nbauds == 2 && bauds[0] == 921600 && bauds[1] == 115200, "the line at 921600 for the download, then 115200 (%d changes)", nbauds);
        CHECK(H.state == BT_READY && H.fast == 2, "ready after it");
    }
    baud_status = 0x0c;
    to_ready(hcd, sizeof hcd);
    CHECK(nbauds == 0 && H.state == BT_READY && find_op(0xfc4e, 0) >= 0, "the chip keeps 115200: the download goes at it");
    io.fw_baud = 0;
    to_ready(hcd, sizeof hcd);
    CHECK(find_op(0xfc18, 0) < 0 && nbauds == 0 && H.state == BT_READY, "without fw_baud: as before");

    /* the radio's time (docs/08 item 140): a Classic keyboard and an LE mouse paired, both away, on the 4.1 controller */
    {
        static const uint8_t kb[6] = { 0x55, 0x44, 0x33, 0x22, 0x11, 0x00 };     /* 00:11:22:33:44:55 */
        static const uint8_t lk[16] = { 9 };
        res_status = 0x01;
        reset_out();
        memset(&H, 0, sizeof H);
        bt_add_key(&H, kb, lk, 4, BTK_KEYBOARD);
        bt_add_le_key(&H, ident, 1, ltk, 0x1234, rnd8, irk_lsb, BTK_MOUSE);
        bt_init(&H, &io, 0, 0);
        run(3000);
        CHECK(H.state == BT_READY && H.scan_mode == BT_SCAN_AUTO, "ready, bt.scan=auto");
        {
            int a = find_op(0x0c1c, 0), t = find_op(0x0c47, 0), s = find_op(0x0c1a, 0);
            CHECK(a >= 0 && out[a].b[4] == 0x00 && out[a].b[5] == 0x01 && out[a].b[6] == 0x12, "the first minute: page scan every 160 ms, 11.25 ms windows");
            CHECK(t >= 0 && out[t].b[4] == 1, "page scan interlaced");
            CHECK(s > t && t > a && out[s].b[4] == 0x02, "set before the page scan is switched on");
            int sp = last_op(0x200b);
            CHECK(sp >= 0 && le16v(out[sp].b + 5) == 0x0100 && le16v(out[sp].b + 7) == 0x0030, "the host's LE scan 30 ms every 160 ms");
            struct bt_radio r; bt_radio_now(&H, &r);
            CHECK(r.tier == BT_TIER_FAST && r.page_on && r.le_on && r.permille == 327, "fast: 14 %% page scan + 19 %% LE scan (%u per mille)", r.permille);
            /* the Classic keyboard's start-up call is out; it does not answer */
            int cc0 = find_op(0x0405, 0);
            if (cc0 >= 0) { uint8_t e[13] = { 0x03, 11, 0x04, 0, 0 }; memcpy(e + 5, kb, 6); e[11] = 1; ev(e, 13); }
        }
        unsigned mark = nout;
        run(60000);
        {
            int a = last_op(0x0c1c), t = last_op(0x0c47), sp = last_op(0x200b);
            CHECK(a >= (int)mark && le16v(out[a].b + 4) == 0x0800 && out[a].b[6] == 0x12, "after a minute: page scan every 1.28 s");
            CHECK(t >= (int)mark && out[t].b[4] == 0, "standard, not interlaced");
            CHECK(sp >= (int)mark && le16v(out[sp].b + 5) == 0x0800 && le16v(out[sp].b + 7) == 0x0030 && H.bg_scan, "the LE scan 30 ms every 1.28 s");
            struct bt_radio r; bt_radio_now(&H, &r);
            CHECK(r.tier == BT_TIER_SLOW && r.permille < 40, "slow: under 4 %% of the radio (%u per mille)", r.permille);
            uint32_t ms; CHECK(strstr(bt_key_activity(&H, 0, &ms), "page scan slow") != 0, "the heartbeat: the keyboard waits with a slow page scan");
        }
        /* the mouse is heard (it does not connect): no idle tier for five minutes after */
        run(200000);
        adv(0x02, 1, rpa);                                           /* ADV_SCAN_IND: heard, not connectable */
        CHECK(H.heard_any_ms == t_ms && H.keys[1].heard_n == 1, "the mouse heard");
        run(200000);
        { struct bt_radio r; bt_radio_now(&H, &r); CHECK(r.tier == BT_TIER_SLOW, "a paired device heard 200 s ago: still slow (%s)", bt_tier_word(r.tier)); }
        mark = nout;
        run(110000);
        {
            struct bt_radio r; bt_radio_now(&H, &r);
            CHECK(r.tier == BT_TIER_IDLE, "nothing heard for five minutes: idle (%s)", bt_tier_word(r.tier));
            CHECK(last_op(0x0c1c) < (int)mark, "the page scan stays at 1.28 s");
            /* the LE scan only in its sweep: 10 s in every 5 minutes, at the fast parameters */
            int on = 0, off = 0, swept = 0; uint32_t on_ms = 0;
            unsigned m2 = nout;
            for (int i = 0; i < 700; i++) {                         /* 700 s, a second at a time */
                run(1000);
                if (H.bg_scan) { on++; on_ms += 1000; } else off++;
                if (H.bg_scan && !swept) {
                    int sp = last_op(0x200b);
                    swept = sp >= (int)m2 && le16v(out[sp].b + 5) == 0x0100 && le16v(out[sp].b + 7) == 0x0030;
                    bt_radio_now(&H, &r);
                    CHECK(r.sweep && r.le_on && r.page_on && r.permille == 195, "a sweep: 19 %% LE + 0.9 %% page scan (%u per mille)", r.permille);
                }
                if (nout > MAXOUT - 40) { reset_out(); m2 = 0; }
            }
            CHECK(swept, "the sweep scans at 30 ms per 160 ms");
            CHECK(on >= 18 && on <= 25 && off >= 670, "idle: the LE scan on about 20 s of 700 (%d on, %d off)", on, off);
            bt_radio_now(&H, &r);
            CHECK(r.tier == BT_TIER_IDLE, "still idle");
            if (!r.sweep) CHECK(!r.le_on && r.permille < 10, "idle between sweeps: only the slow page scan (%u per mille)", r.permille);
        }
        /* the keyboard calls and connects, then its link goes: the fast minute again */
        { uint8_t e[13] = { 0x03, 11, 0x00, 0x20, 0x00 }; memcpy(e + 5, kb, 6); e[11] = 1; ev(e, 13); run(100); }
        { struct bt_radio r; bt_radio_now(&H, &r); CHECK(!r.page_on && !H.ps_on, "nothing Classic away: the page scan off"); }
        mark = nout;
        { uint8_t e[6] = { 0x05, 4, 0, 0x20, 0x00, 0x08 }; ev(e, 6); run(100); }
        {
            int a = last_op(0x0c1c), s = last_op(0x0c1a);
            CHECK(a >= (int)mark && le16v(out[a].b + 4) == 0x0100 && s >= (int)mark && out[s].b[4] == 0x02, "a link went down: the fast page scan again, switched on");
            struct bt_radio r; bt_radio_now(&H, &r); CHECK(r.tier == BT_TIER_FAST, "fast again");
        }
        /* it comes back and goes again within three minutes: slow this time (bounded) */
        run(70000);
        { uint8_t e[13] = { 0x03, 11, 0x00, 0x21, 0x00 }; memcpy(e + 5, kb, 6); e[11] = 1; ev(e, 13); run(100); }
        { uint8_t e[6] = { 0x05, 4, 0, 0x21, 0x00, 0x08 }; ev(e, 6); run(100); }
        { struct bt_radio r; bt_radio_now(&H, &r); CHECK(r.tier == BT_TIER_SLOW, "a second loss within three minutes: slow (%s)", bt_tier_word(r.tier)); }
        /* a Connect asked for: fast at once */
        bt_connect(&H, ident); run(100);
        { struct bt_radio r; bt_radio_now(&H, &r); CHECK(r.tier == BT_TIER_FAST, "a Connect: fast"); }
        /* the Mac's DHCP over the WiFi: no scans for 10 s, not again within the next 5 s */
        {
            CHECK(H.bg_scan && H.ps_on, "after the Connect: scans on");
            bt_hush(&H, BT_HUSH_MS, "test"); run(100);
            struct bt_radio r; bt_radio_now(&H, &r);
            CHECK(!H.bg_scan && !H.ps_on && r.hush_ms > 9000 && r.permille == 0, "DHCP: no page scan, no LE scan (%u per mille)", r.permille);
            uint32_t ms; CHECK(strstr(bt_key_activity(&H, 1, &ms), "WiFi") != 0, "the heartbeat says why");
            run(10000);
            CHECK(H.bg_scan && H.ps_on, "10 s later the scans again");
            bt_hush(&H, BT_HUSH_MS, "test"); run(100);
            CHECK(H.bg_scan && H.ps_on, "a second DHCP within 5 s of the first's end: not quiet again (scans at least a third of the time)");
            run(10000);
            bt_hush(&H, BT_HUSH_MS, "test"); run(100);
            CHECK(!H.bg_scan && H.hush_n == 2, "after the gap, quiet again (%u)", H.hush_n);
            run(10500);
        }
    }
    /* bt.scan=off: no page scan and no LE scan, until a Connect; bt.scan=fast: fast for good */
    {
        static const uint8_t kb[6] = { 0x55, 0x44, 0x33, 0x22, 0x11, 0x00 };
        static const uint8_t lk[16] = { 9 };
        res_status = 0x01;
        reset_out();
        memset(&H, 0, sizeof H);
        H.scan_mode = BT_SCAN_OFF;
        bt_add_key(&H, kb, lk, 4, BTK_KEYBOARD);
        bt_add_le_key(&H, ident, 1, ltk, 0x1234, rnd8, irk_lsb, BTK_MOUSE);
        bt_init(&H, &io, 0, 0);
        run(10000);
        CHECK(H.state == BT_READY && H.scan_mode == BT_SCAN_OFF, "bt.scan=off kept by bt_init");
        CHECK(find_op(0x0c1a, 0) < 0 && find_op(0x200b, 0) < 0 && !H.bg_scan, "off: no page scan, no LE scan");
        { struct bt_radio r; bt_radio_now(&H, &r); CHECK(r.tier == BT_TIER_OFF && r.permille == 0, "off: nothing (%u per mille)", r.permille); }
        bt_connect(&H, ident); run(100);
        CHECK(H.bg_scan && find_op(0x0c1a, 0) >= 0, "a Connect: the scans for a minute");
        run(61000);
        CHECK(!H.bg_scan && !H.ps_on, "and off again after it");
        reset_out();
        memset(&H, 0, sizeof H);
        H.scan_mode = BT_SCAN_FAST;
        bt_add_key(&H, kb, lk, 4, BTK_KEYBOARD);
        bt_add_le_key(&H, ident, 1, ltk, 0x1234, rnd8, irk_lsb, BTK_MOUSE);
        bt_init(&H, &io, 0, 0);
        run(600000);
        { struct bt_radio r; bt_radio_now(&H, &r); CHECK(r.tier == BT_TIER_FAST && r.permille == 327, "bt.scan=fast: fast after ten minutes (%u per mille)", r.permille); }
    }

    /* ---- 25 November 2026: Raspberry Pi OS's parameters, and what a lossy UART must not break (docs/21) ---- */
    {
        /* the validity of a device's request, as Linux's hci_check_conn_params */
        CHECK(bt_le_params_ok(6, 12, 0, 500) && bt_le_params_ok(24, 40, 4, 200), "valid connection parameters taken");
        CHECK(!bt_le_params_ok(12, 6, 0, 500) && !bt_le_params_ok(5, 12, 0, 500) && !bt_le_params_ok(6, 3201, 0, 3200), "min > max, min < 7.5 ms, max > 4 s refused");
        CHECK(!bt_le_params_ok(6, 80, 0, 10) && !bt_le_params_ok(6, 40, 4, 10), "a timeout shorter than two intervals, a latency the timeout cannot cover refused");
        CHECK(bt_le_min_floor(6, 12) == 12 && bt_le_min_floor(6, 9) == 6 && bt_le_min_floor(24, 40) == 24, "the floor of 15 ms only where the device's range reaches it");

        res_status = 0;                                          /* a 4.2 controller: the accept list connects */
        to_ready(0, 0);
        int c = find_op(0x200d, 0);
        CHECK(c >= 0 && le16v(out[c].b + 17) == 0x0018 && le16v(out[c].b + 19) == 0x0028 && le16v(out[c].b + 21) == 0 && le16v(out[c].b + 23) == 0x01f4,
              "LE Create Connection asks for 30 to 50 ms, latency 0, timeout 5 s (as Linux, but its 420 ms) (%04x-%04x)", c >= 0 ? le16v(out[c].b + 17) : 0, c >= 0 ? le16v(out[c].b + 19) : 0);
        /* the link comes up with an interval of 50 ms */
        unsigned mark = nout;
        { uint8_t e[21] = { 0x3e, 19, 0x01, 0, 0x40, 0x00, 0x00, 1 }; memcpy(e + 8, ident, 6); e[14] = 0x28; e[18] = 0xf4; e[19] = 0x01; ev(e, 21); }
        run(50);
        int ci = -1; for (int i = 0; i < BT_MAXCONN; i++) if (H.conn[i].used) ci = i;
        CHECK(ci >= 0 && H.conn[ci].l.ci == 0x28 && H.conn[ci].l.cto == 0x01f4, "the link's interval kept from LE Connection Complete (%u)", ci >= 0 ? H.conn[ci].l.ci : 0);
        struct bt_conn *C = ci >= 0 ? &H.conn[ci] : &H.conn[0];
        /* encrypted with its key: the GATT discovery starts, one ATT request goes out and is in flight */
        uint16_t le0 = H.le_free;
        { uint8_t e[6] = { 0x08, 4, 0x00, 0x40, 0x00, 0x01 }; ev(e, 6); }
        run(20);
        CHECK(C->sent == 1 && H.le_free == le0 - 1 && H.le_pkts == 8, "an ATT request in flight: one LE buffer taken (sent %u, free %u of %u)", C->sent, H.le_free, H.le_pkts);
        /* the device asks for 7.5 to 15 ms (a keyboard's usual wish): accepted, 15 ms asked of the controller */
        mark = nout;
        { uint8_t a[] = { 0x02, 0x40, 0x20, 16, 0, 12, 0, 0x05, 0x00, 0x12, 0x07, 8, 0, 6, 0, 12, 0, 0, 0, 0xf4, 0x01 }; bt_rx(&H, a, sizeof a); }
        run(20);
        {
            int u = find_op(0x2013, mark), r = -1;
            for (unsigned i = mark; i < nout; i++) if (out[i].b[0] == 2 && out[i].b[7] == 0x05 && out[i].b[9] == 0x13) r = (int)i;
            CHECK(r >= 0 && out[r].b[10] == 0x07 && out[r].b[13] == 0, "Connection Parameter Update Response: accepted, same identifier");
            CHECK(u >= 0 && le16v(out[u].b + 6) == 12 && le16v(out[u].b + 8) == 12 && le16v(out[u].b + 12) == 0x01f4, "LE Connection Update 15-15 ms asked (%u-%u)", u >= 0 ? le16v(out[u].b + 6) : 0, u >= 0 ? le16v(out[u].b + 8) : 0);
            CHECK(C->l.upd_asked == 1, "counted");
        }
        /* an invalid request (timeout 100 ms, latency 4 at up to 50 ms): refused, nothing asked of the controller */
        mark = nout;
        { uint8_t a[] = { 0x02, 0x40, 0x20, 16, 0, 12, 0, 0x05, 0x00, 0x12, 0x08, 8, 0, 6, 0, 40, 0, 4, 0, 10, 0 }; bt_rx(&H, a, sizeof a); }
        run(20);
        {
            int r = -1;
            for (unsigned i = mark; i < nout; i++) if (out[i].b[0] == 2 && out[i].b[7] == 0x05 && out[i].b[9] == 0x13) r = (int)i;
            CHECK(r >= 0 && out[r].b[13] == 1 && find_op(0x2013, mark) < 0 && C->l.upd_refused == 1, "invalid parameters refused (result 1), no update");
        }
        /* the controller reports the update */
        { uint8_t e[12] = { 0x3e, 10, 0x03, 0, 0x40, 0x00, 12, 0, 0, 0, 0xf4, 0x01 }; ev(e, 12); }
        CHECK(C->l.ci == 12, "the interval from LE Connection Update Complete (%u)", C->l.ci);
        /* a header that makes no sense (a byte lost): dropped, and the real event right after it still read */
        uint32_t rs = H.rx_resyncs;
        { uint8_t g[] = { 0x04, 0x99, 0x3e, 10, 0x03, 0, 0x40, 0x00, 24, 0, 0, 0, 0xf4, 0x01 }; bt_rx(&H, g, sizeof g); }
        CHECK(H.rx_resyncs == rs + 1 && C->l.ci == 12, "event code 99: dropped, counted");
        { uint8_t g[] = { 0x04, 0x0f, 0x07, 0x04, 0x3e, 10, 0x03, 0, 0x40, 0x00, 24, 0, 0, 0, 0xf4, 0x01 }; bt_rx(&H, g, sizeof g); }
        CHECK(C->l.ci == 24, "a Command Status of the wrong length dropped and the LE event inside the bytes found (%u)", C->l.ci);
        /* a packet cut by a UART error: dropped at the break, the next one read */
        { uint8_t g[] = { 0x04, 0x3e, 10, 0x03, 0, 0x40 }; bt_rx(&H, g, sizeof g); bt_rx_break(&H);
          uint8_t e[] = { 0x04, 0x3e, 10, 0x03, 0, 0x40, 0x00, 40, 0, 0, 0, 0xf4, 0x01 }; bt_rx(&H, e, sizeof e); }
        CHECK(C->l.ci == 40 && H.rx_breaks == 1, "a cut packet dropped, the next taken (%u, breaks %u)", C->l.ci, H.rx_breaks);
        /* an ACL header with broadcast flags: not a packet */
        rs = H.rx_resyncs;
        { uint8_t g[] = { 0x02, 0x40, 0xe0, 0x10, 0x00 }; bt_rx(&H, g, sizeof g); }
        CHECK(H.rx_resyncs == rs + 1, "an ACL header with broadcast flags dropped");
        /* the next request is queued behind no buffer: then the link goes. Its buffers come back, its queue is dropped */
        H.le_free = 0;
        unsigned aq = H.aq_tail - H.aq_head;
        { uint8_t d[3] = { 0x0a, 0x10, 0x00 }; l2_send(&H, C, 4, d, 3); }
        CHECK(H.aq_tail - H.aq_head == aq + 1, "a frame waits for a buffer");
        uint16_t inflight = C->sent;
        { uint8_t e[6] = { 0x05, 4, 0, 0x40, 0x00, 0x08 }; ev(e, 6); }
        CHECK(inflight == 3 && H.aq_tail == H.aq_head && H.le_free == inflight, "the link gone: its waiting frame dropped, its %u in flight back (free %u)", inflight, H.le_free);
        /* a Number Of Completed Packets for the gone link: nothing more (Linux: an unknown handle is skipped) */
        { uint8_t e[7] = { 0x13, 5, 1, 0x40, 0x00, 1, 0 }; ev(e, 7); }
        CHECK(H.le_free == inflight, "completed packets of a gone link not counted twice (%u)", H.le_free);
        { uint8_t e[7] = { 0x13, 5, 1, 0x41, 0x00, 50, 0 }; ev(e, 7); }
        CHECK(H.le_free <= H.le_pkts, "the pool never above the controller's count");
        /* a Command Complete lost on the UART: the next commands still go, after 2 s */
        run(2000);
        mark = nout;
        uint32_t to0 = H.cmd_timeouts;
        drop_answers = 1;
        bt_forget(&H, ident);                                    /* a command or two of its own (the lists rewritten later) */
        uint8_t z = 0; cmd(&H, C_LE_RES_ENABLE, &z, 1); cmd(&H, C_LE_ACC_CLEAR, 0, 0);
        run(500);
        CHECK(H.credits == 0 && find_op(0x2010, mark) < 0, "the answer lost: the next command waits");
        run(2000);
        CHECK(H.cmd_timeouts == to0 + 1 && find_op(0x2010, mark) >= 0, "after 2 s the host takes the credit back and the queue goes on (timeouts %u)", H.cmd_timeouts);
        /* an LE Rand answer is not taken for the LE Encrypt that is out */
        H.aes_busy = 1; H.aes_op = C_LE_ENCRYPT; H.aes_handle = 0x77;
        { uint8_t r[9] = { 0 }; cc(C_LE_RAND, r, 9); }
        CHECK(H.aes_busy == 1, "a stray LE Rand answer left alone");
        H.aes_busy = 0;
    }
    /* a direct connection (a pairing) scans 30 ms per 120 ms */
    {
        res_status = 0;
        to_ready(0, 0);
        unsigned mark = nout;
        H.found[0].le = 1; memcpy(H.found[0].addr, rpa, 6); H.found[0].addr_type = 1; H.found[0].cod = 5u << 8 | 0x40; H.nfound = 1;
        bt_pair(&H, rpa); run(50);
        int c = -1; for (unsigned i = mark; i < nout; i++) if (op_of(i) == 0x200d && out[i].b[8] == 0) c = (int)i;
        CHECK(c >= 0 && le16v(out[c].b + 4) == 0x00c0 && le16v(out[c].b + 6) == 0x0030 && le16v(out[c].b + 17) == 0x0018,
              "the pairing's connection: scan 30 ms per 120 ms (Linux 60 per 60), 30-50 ms asked (%04x/%04x)", c >= 0 ? le16v(out[c].b + 4) : 0, c >= 0 ? le16v(out[c].b + 6) : 0);
        struct bt_radio r; bt_radio_now(&H, &r);
        CHECK(r.le_on && r.le_int == 0x00c0, "the heartbeat's radio line knows it");
        bt_scan(&H); run(20);
        int sp = last_op(0x200b);
        CHECK(sp >= 0 && out[sp].b[4] == 1 && le16v(out[sp].b + 5) == 0x00c0 && le16v(out[sp].b + 7) == 0x0030, "a scan for new devices: the LE side 30 ms per 120 ms beside the inquiry");
    }

    printf("bt_test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
