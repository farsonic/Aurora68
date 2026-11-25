/* SPDX-License-Identifier: MPL-2.0
 *
 * ip_test.c - the kernel's own IPv4 (emu68/src/psmac/psmac_ip.c) on the host, around the shared WiFi address
 * (25 November 2026, docs/08 item 149). On the owner's WiFi a laptop could not resolve the Mac's address, and
 * under a system with no TCP/IP the kernel had no address at all; psmac_ip.c now answers ARP for the shared
 * address, takes the Mac's last address from the card (macip.txt) after two ARP probes, gives it up when
 * another device has it, and answers a ping the Mac leaves unanswered. This drives the stack through the same
 * calls the drivers make (psmac_ip_rx for a received frame, psmac_ip_mac_tx for the Mac's, psmac_ip_tx_take
 * for the kernel's) with a clock of its own, and checks the frames that come out and the heartbeat's line.
 * Each case runs in a child process, since the stack's state is static. Built and run by tools/ip_test.sh,
 * from `make -C mac check`. Addresses are documentation ones (RFC 5737).
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include "psmac_ip.h"
#include "alice_net.h"

uint64_t ip_test_ticks = 5000000;           /* microseconds (tick_hz is 1 MHz on the host) */
static char logbuf[65536]; static size_t logn;
static int verbose;
void ip_test_kprintf(const char *format, ...)
{
    va_list ap; va_start(ap, format);
    char line[1024]; vsnprintf(line, sizeof line, format, ap);
    va_end(ap);
    size_t n = strlen(line);
    if (logn + n < sizeof logbuf) { memcpy(logbuf + logn, line, n); logn += n; logbuf[logn] = 0; }
    if (verbose) fputs(line, stdout);
}
void ip_test_sprintf(char *buf, const char *format, ...)
{
    va_list ap; va_start(ap, format); vsprintf(buf, format, ap); va_end(ap);
}

/* ---- the kernel around the stack ---- */
static const char *opts[16][2]; static int nopts;
const char *psmac_option(const char *key)
{
    for (int i = 0; i < nopts; i++) if (!strcmp(opts[i][0], key)) return opts[i][1];
    return 0;
}
static void opt(const char *k, const char *v) { opts[nopts][0] = k; opts[nopts][1] = v; nopts++; }
static uint8_t board[AN_BOARD_BYTES] __attribute__((aligned(16)));
volatile uint8_t *alice_net_board(void) { return board; }
static struct an_unit *unit_p(int u) { return (struct an_unit *)(board + AN_OFF_UNIT(u)); }
static int card_state = -1; static uint8_t card_ip[4]; static int saves; static uint8_t saved[4];
static int card_writable;
const uint8_t *psmac_card_macip(int *state) { *state = card_state; return card_state == 1 ? card_ip : 0; }
int psmac_card_writable(void) { return card_writable; }
void psmac_card_save_macip(const uint8_t ip[4]) { saves++; memcpy(saved, ip, 4); }
int remote_tcp_accept(int c, uint16_t port, const uint8_t peer[4]) { (void)c; (void)port; (void)peer; return 1; }
void remote_tcp_recv(int c, const uint8_t *data, uint32_t len) { (void)c; (void)data; (void)len; }
uint32_t remote_tcp_room(int c) { (void)c; return 4096; }
uint32_t remote_tcp_produce(int c, uint8_t *buf, uint32_t room) { (void)c; (void)buf; (void)room; return 0; }
void remote_tcp_closed(int c) { (void)c; }

static int checks, failed;
#define CHECK(c, ...) do { checks++; if (!(c)) { failed++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const uint8_t MAC_HW[6] = { 0x02, 0x50, 0x53, 0x57, 0x49, 0x46 };    /* the Mac's (and in the shared mode the kernel's) */
static const uint8_t PEER_HW[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x20 };   /* a laptop */
static const uint8_t OTHER_HW[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x30 };  /* another device */
static const uint8_t BC[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const uint8_t IP10[4] = { 192, 0, 2, 10 }, IP11[4] = { 192, 0, 2, 11 }, IP20[4] = { 192, 0, 2, 20 }, IP50[4] = { 192, 0, 2, 50 };
static const uint8_t Z4[4], Z6[6];

/* ---- frames ---- */
static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static unsigned get16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static uint16_t csum(const uint8_t *p, unsigned n, uint32_t s)
{
    for (; n > 1; p += 2, n -= 2) s += (uint32_t)(p[0] << 8 | p[1]);
    if (n) s += (uint32_t)p[0] << 8;
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return (uint16_t)~s;
}
static unsigned arp(uint8_t *f, const uint8_t *dst, int op, const uint8_t *sha, const uint8_t *spa, const uint8_t *tha, const uint8_t *tpa)
{
    memset(f, 0, 60);
    memcpy(f, dst, 6); memcpy(f + 6, sha, 6); put16(f + 12, 0x0806);
    put16(f + 14, 1); put16(f + 16, 0x0800); f[18] = 6; f[19] = 4; put16(f + 20, (unsigned)op);
    memcpy(f + 22, sha, 6); memcpy(f + 28, spa, 4); memcpy(f + 32, tha, 6); memcpy(f + 38, tpa, 4);
    return 60;
}
static unsigned ipv4(uint8_t *f, const uint8_t *dst_hw, const uint8_t *src_hw, const uint8_t *sip, const uint8_t *dip, int proto, const uint8_t *l4, unsigned n)
{
    memset(f, 0, 64 + n);
    memcpy(f, dst_hw, 6); memcpy(f + 6, src_hw, 6); put16(f + 12, 0x0800);
    uint8_t *h = f + 14;
    h[0] = 0x45; put16(h + 2, 20 + n); h[8] = 64; h[9] = (uint8_t)proto;
    memcpy(h + 12, sip, 4); memcpy(h + 16, dip, 4);
    put16(h + 10, csum(h, 20, 0));
    memcpy(f + 34, l4, n);
    return 34 + n < 60 ? 60 : 34 + n;
}
static unsigned echo(uint8_t *f, int type, const uint8_t *dst_hw, const uint8_t *src_hw, const uint8_t *sip, const uint8_t *dip, unsigned id, unsigned seq)
{
    uint8_t m[40] = { 0 };
    m[0] = (uint8_t)type; put16(m + 4, id); put16(m + 6, seq);
    for (int i = 8; i < 40; i++) m[i] = (uint8_t)i;
    put16(m + 2, csum(m, 40, 0));
    return ipv4(f, dst_hw, src_hw, sip, dip, 1, m, 40);
}
static unsigned syn(uint8_t *f, const uint8_t *dst_hw, const uint8_t *src_hw, const uint8_t *sip, const uint8_t *dip, unsigned dport)
{
    uint8_t t[20] = { 0 };
    put16(t, 41000); put16(t + 2, dport); t[4] = 0x12; t[7] = 0x34; t[12] = 5 << 4; t[13] = 0x02; put16(t + 14, 65535);
    uint8_t ph[12]; memcpy(ph, sip, 4); memcpy(ph + 4, dip, 4); ph[8] = 0; ph[9] = 6; put16(ph + 10, 20);
    uint32_t s = 0; for (int i = 0; i < 12; i += 2) s += (uint32_t)(ph[i] << 8 | ph[i + 1]);
    put16(t + 16, csum(t, 20, s));
    return ipv4(f, dst_hw, src_hw, sip, dip, 6, t, 20);
}
static unsigned mac_udp(uint8_t *f, const uint8_t *sip)          /* a frame the Mac sends from sip */
{
    uint8_t u[16] = { 0x14, 0xe9, 0x14, 0xe9, 0, 16, 0, 0 };
    return ipv4(f, BC, MAC_HW, sip, (const uint8_t *)"\xff\xff\xff\xff", 17, u, 16);
}

/* ---- the drivers' side ---- */
static uint8_t out[64][1600]; static unsigned outn[64]; static int nout;
static void rx(int u, const uint8_t *f, unsigned n) { psmac_ip_rx(u, f, n, unit_p(u)->mac, 0); }
static void poll_for(int ms)
{
    for (int i = 0; i < ms; i++) {
        ip_test_ticks += 1000;
        psmac_ip_poll();
        for (int u = 0; u < 2; u++) {
            uint8_t b[1600]; uint32_t n;
            while ((n = psmac_ip_tx_take(u, b, sizeof b))) if (nout < 64) { memcpy(out[nout], b, n); outn[nout++] = n; }
        }
    }
}
static void clear_out(void) { nout = 0; }
/* the kernel's frames since clear_out: an ARP of op with sender address spa and target address tpa (0: any) */
static int find_arp(int op, const uint8_t *dst, const uint8_t *spa, const uint8_t *tpa)
{
    for (int i = 0; i < nout; i++) {
        const uint8_t *f = out[i];
        if (get16(f + 12) != 0x0806 || (int)get16(f + 20) != op) continue;
        if (dst && memcmp(f, dst, 6)) continue;
        if (memcmp(f + 6, MAC_HW, 6) || memcmp(f + 22, MAC_HW, 6)) continue;
        if (spa && memcmp(f + 28, spa, 4)) continue;
        if (tpa && memcmp(f + 38, tpa, 4)) continue;
        return 1;
    }
    return 0;
}
static int find_ip(int proto, int l4type, const uint8_t *src, const uint8_t *dst)
{
    for (int i = 0; i < nout; i++) {
        const uint8_t *f = out[i];
        if (get16(f + 12) != 0x0800 || f[23] != proto || memcmp(f + 26, src, 4) || memcmp(f + 30, dst, 4)) continue;
        if (csum(f + 14, 20, 0) != 0) continue;
        if (proto == 1 && f[34] != l4type) continue;
        if (proto == 6 && f[34 + 13] != l4type) continue;
        if (proto == 1 && csum(f + 34, get16(f + 16) - 20, 0) != 0) continue;
        return 1;
    }
    return 0;
}
static void wifi_up(void)
{
    struct an_unit *w = unit_p(0);
    w->kind = AN_KIND_WIFI; w->link = 1; memcpy(w->mac, MAC_HW, 6); memcpy(w->factory_mac, MAC_HW, 6);
}
static void report(void) { logn = 0; logbuf[0] = 0; psmac_ip_report(); }

/* ---- the cases ---- */
static void ask(const uint8_t *tpa) { uint8_t f[64]; rx(0, f, arp(f, BC, 1, PEER_HW, IP20, Z6, tpa)); }

/* the Mac's stack speaks: the kernel follows its address, answers ARP for it, leaves the pings the Mac answers to the Mac */
static void case_shared_mac(void)
{
    uint8_t f[200];
    psmac_ip_init(); wifi_up();
    poll_for(600);
    CHECK(strstr(logbuf, "the address is not known yet"), "shared mode with no address yet: %s", logbuf);
    psmac_ip_mac_tx(0, f, mac_udp(f, IP10));
    clear_out(); poll_for(600);
    CHECK(find_arp(1, BC, IP10, IP10), "a gratuitous ARP for the Mac's address once the kernel knows it");
    CHECK(strstr(logbuf, "the Mac's WiFi address is now 192.0.2.10:"), "the address followed: %s", logbuf);
    CHECK(saves == 0, "no card: nothing saved (%d)", saves);
    clear_out(); ask(IP10); poll_for(5);
    CHECK(find_arp(2, PEER_HW, IP10, IP20), "the kernel answers an ARP request for the shared address, from the shared MAC");
    CHECK(strstr(logbuf, "the first ARP request for the Mac's address 192.0.2.10 arrived"), "first request logged");
    clear_out(); ask(IP11); poll_for(5);
    CHECK(!find_arp(2, 0, 0, 0), "a request for another address is not answered");
    psmac_ip_mac_tx(0, f, arp(f, PEER_HW, 2, MAC_HW, IP10, PEER_HW, IP20));     /* the Mac's own answer to the first */
    /* a ping the Mac answers: nothing from the kernel */
    clear_out(); rx(0, f, echo(f, 8, MAC_HW, PEER_HW, IP20, IP10, 7, 1)); poll_for(100);
    psmac_ip_mac_tx(0, f, echo(f, 0, PEER_HW, MAC_HW, IP10, IP20, 7, 1));
    poll_for(900);
    CHECK(!find_ip(1, 0, IP10, IP20), "the Mac answered the ping: the kernel does not");
    /* a ping the Mac does not answer: the kernel's, after half a second */
    clear_out(); rx(0, f, echo(f, 8, MAC_HW, PEER_HW, IP20, IP10, 7, 2)); poll_for(400);
    CHECK(!find_ip(1, 0, IP10, IP20), "the kernel waits for the Mac's answer first");
    poll_for(200);
    CHECK(find_ip(1, 0, IP10, IP20), "no answer from the Mac: the kernel answers the ping");
    CHECK(strstr(logbuf, "the kernel answers such pings"), "said once in the log");
    /* TCP: 8068 only */
    clear_out(); rx(0, f, syn(f, MAC_HW, PEER_HW, IP20, IP10, 8068)); poll_for(5);
    CHECK(find_ip(6, 0x12, IP10, IP20), "SYN-ACK on port 8068 of the shared address");
    clear_out(); rx(0, f, syn(f, MAC_HW, PEER_HW, IP20, IP10, 80)); poll_for(5);
    CHECK(!find_ip(6, 0x12, IP10, IP20), "port 80 is the Mac's in the shared mode");
    report();
    CHECK(strstr(logbuf, "ARP requests 2, for the Mac's address 1: answered by the kernel 1, by the Mac 1"), "the heartbeat's ARP counts: %s", logbuf);
    CHECK(strstr(logbuf, "pings 2: the Mac answered 1, the kernel 1"), "the heartbeat's ping counts: %s", logbuf);
}

/* the card's address and a silent Mac: probed, taken, answered on; then the Mac's own address wins and is saved */
static void case_card(void)
{
    uint8_t f[200];
    card_state = 1; memcpy(card_ip, IP10, 4); card_writable = 1;
    psmac_ip_init(); wifi_up();
    clear_out(); poll_for(600);
    CHECK(strstr(logbuf, "the card remembers the Mac's last WiFi address, 192.0.2.10"), "the card's address read: %s", logbuf);
    CHECK(find_arp(1, BC, Z4, IP10), "an ARP probe (sender address 0) for the card's address");
    CHECK(!find_arp(1, BC, IP10, IP10), "no announcement before the probes");
    clear_out(); ask(IP10); poll_for(5);
    CHECK(!find_arp(2, 0, 0, 0), "not answered while probing");
    clear_out(); poll_for(2600);
    CHECK(find_arp(1, BC, Z4, IP10), "the second probe");
    CHECK(find_arp(1, BC, IP10, IP10), "then the gratuitous ARP");
    CHECK(strstr(logbuf, "(the card's, the Mac not heard from yet)"), "adopted from the card: %s", logbuf);
    clear_out(); ask(IP10); poll_for(5);
    CHECK(find_arp(2, PEER_HW, IP10, IP20), "the card's address answered without the Mac");
    clear_out(); rx(0, f, syn(f, MAC_HW, PEER_HW, IP20, IP10, 8068)); poll_for(5);
    CHECK(find_ip(6, 0x12, IP10, IP20), "and the remote's port");
    clear_out(); rx(0, f, echo(f, 8, MAC_HW, PEER_HW, IP20, IP10, 9, 1)); poll_for(600);
    CHECK(find_ip(1, 0, IP10, IP20), "and pings, half a second late");
    report();
    CHECK(strstr(logbuf, "the address from the card"), "the heartbeat says where the address came from: %s", logbuf);
    CHECK(saves == 0, "the card's own address is not written back (%d)", saves);
    /* the Mac speaks from the same address: nothing to write; from another: followed and kept */
    psmac_ip_mac_tx(0, f, mac_udp(f, IP10)); poll_for(600);
    CHECK(saves == 0, "the same address: not written (%d)", saves);
    psmac_ip_mac_tx(0, f, mac_udp(f, IP11));
    clear_out(); poll_for(600);
    CHECK(saves == 1 && !memcmp(saved, IP11, 4), "the Mac's new address goes onto the card (%d)", saves);
    CHECK(find_arp(1, BC, IP11, IP11), "announced");
    clear_out(); ask(IP10); ask(IP11); poll_for(5);
    CHECK(!find_arp(2, PEER_HW, IP10, IP20) && find_arp(2, PEER_HW, IP11, IP20), "the old address left, the new one answered");
}

/* a writable card with no macip.txt yet: the Mac's first address is written, once */
static void case_card_new(void)
{
    uint8_t f[200];
    card_state = -1; card_writable = 1;
    psmac_ip_init(); wifi_up();
    clear_out(); poll_for(3000);
    CHECK(!find_arp(1, BC, Z4, 0), "nothing to probe without an address on the card");
    psmac_ip_mac_tx(0, f, mac_udp(f, IP10)); poll_for(600);
    psmac_ip_mac_tx(0, f, mac_udp(f, IP10)); poll_for(600);
    CHECK(saves == 1 && !memcmp(saved, IP10, 4), "the Mac's address written once (%d)", saves);
}

/* another device answers the probe: the card's address is not used */
static void case_card_taken(void)
{
    uint8_t f[200];
    card_state = 1; memcpy(card_ip, IP10, 4);
    psmac_ip_init(); wifi_up();
    poll_for(600);
    rx(0, f, arp(f, MAC_HW, 2, OTHER_HW, IP10, MAC_HW, Z4));
    clear_out(); poll_for(3000);
    CHECK(!find_arp(1, BC, IP10, IP10), "no announcement of an address another device has");
    CHECK(strstr(logbuf, "another device (02:00:00:00:00:30) has the Mac's last WiFi address 192.0.2.10"), "said in the log: %s", logbuf);
    clear_out(); ask(IP10); poll_for(5);
    CHECK(!find_arp(2, 0, 0, 0), "and not answered");
}

/* another device claims the card's address after the kernel took it: given up */
static void case_card_claimed(void)
{
    uint8_t f[200];
    card_state = 1; memcpy(card_ip, IP10, 4);
    psmac_ip_init(); wifi_up();
    card_writable = 1;
    poll_for(3200);
    clear_out(); ask(IP10); poll_for(5);
    CHECK(find_arp(2, PEER_HW, IP10, IP20), "taken from the card");
    rx(0, f, arp(f, BC, 1, OTHER_HW, IP10, Z6, IP10));    /* its gratuitous ARP */
    poll_for(600);
    clear_out(); ask(IP10); poll_for(5);
    CHECK(!find_arp(2, 0, 0, 0), "given up once another device announced it");
    CHECK(strstr(logbuf, "which the Mac has not used since"), "said in the log: %s", logbuf);
    /* the Mac's own frames bring an address back */
    psmac_ip_mac_tx(0, f, mac_udp(f, IP11)); poll_for(600);
    clear_out(); ask(IP11); poll_for(5);
    CHECK(find_arp(2, PEER_HW, IP11, IP20), "the Mac's own address answered");
}

/* the wired port: the kernel's own MAC and a static address, as before; ARP and pings answered at once */
static void case_wired(void)
{
    uint8_t f[200];
    opt("remote.ip", "192.0.2.50/24");
    struct an_unit *e = unit_p(1);
    static const uint8_t PI_HW[6] = { 0xb8, 0x27, 0xeb, 0x00, 0x00, 0x01 };
    e->kind = AN_KIND_ETHER; e->link = 1; memcpy(e->factory_mac, PI_HW, 6); memcpy(e->mac, PI_HW, 6);
    psmac_ip_init();
    poll_for(600);
    uint8_t hw[6]; psmac_ip_hwaddr(1, hw);
    clear_out();
    psmac_ip_rx(1, f, arp(f, BC, 1, PEER_HW, IP20, Z6, IP50), e->mac, 0);
    poll_for(2);
    int ok = 0;
    for (int i = 0; i < nout; i++) if (get16(out[i] + 20) == 2 && !memcmp(out[i] + 22, hw, 6) && !memcmp(out[i] + 28, IP50, 4)) ok = 1;
    CHECK(ok, "the wired port's own address answered by ARP");
    clear_out(); psmac_ip_rx(1, f, echo(f, 8, hw, PEER_HW, IP20, IP50, 3, 1), e->mac, 0); poll_for(2);
    CHECK(find_ip(1, 0, IP50, IP20), "a ping answered at once on the wired port");
    report();
    CHECK(strstr(logbuf, "ARP requests 1, for this address 1, answered 1, pings 1"), "the heartbeat: %s", logbuf);
}

int main(int argc, char **argv)
{
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    static const struct { const char *name; void (*fn)(void); } cases[] = {
        { "shared mode, the Mac's stack", case_shared_mac }, { "shared mode, the card's address", case_card },
        { "no macip.txt yet", case_card_new }, { "the card's address answered by another device", case_card_taken }, { "the card's address claimed later", case_card_claimed },
        { "the wired port", case_wired },
    };
    int total = 0, bad = 0;
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        fflush(stdout);
        pid_t p = fork();
        if (p == 0) { cases[i].fn(); printf("  %s: %d checks, %d failed\n", cases[i].name, checks, failed); fflush(stdout); _exit(failed ? 1 : 0); }
        int st = 0; waitpid(p, &st, 0);
        total++;
        if (!WIFEXITED(st) || WEXITSTATUS(st)) bad++;
    }
    printf("ip_test: %d cases, %d failed\n", total, bad);
    return bad ? 1 : 0;
}
