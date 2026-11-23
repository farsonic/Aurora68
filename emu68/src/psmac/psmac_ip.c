/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_ip.c - the kernel's own IPv4: enough of it for the remote control (docs/13).
 *
 * Backstory. Until 20 November the kernel had no network address: the Mac's TCP/IP (Riptide or
 * MacTCP) runs on the 68k, and the kernel only moved frames between the network chips and the
 * Mac's rings. The user then asked for a remote control that works without a program on the Mac,
 * and even while the Mac is hung, so the Pi needed an address of its own. This is the smallest
 * stack that does that well on a home network:
 *   - ARP: answers for its address. It never asks: every reply goes back to the Ethernet address
 *     the request came from (on the local network the client, otherwise the router), DHCP is
 *     broadcast and mDNS multicast, so there is nothing to resolve;
 *   - ICMP echo, so `ping` finds it;
 *   - a DHCP client (RFC 2131) with a client identifier and a host name, or a static address
 *     from remote.ip=;
 *   - mDNS (RFC 6762): A queries for aurora68-pi.local (remote.name= for another name);
 *   - TCP (RFC 793), listening only: four connections, a 32 KiB send buffer each, in-order
 *     receive, go-back-N retransmission from the oldest unacknowledged byte. No congestion
 *     control and no IPv6: the remote is for the local network.
 * The interface is the wired port while its link is up, else the WiFi (remote.link=). On the
 * wired port the kernel has its own MAC address; on the WiFi it shares the Mac's, because a
 * station has one per association, and gets its own IP address by a different DHCP client
 * identifier, falling back to the Mac's IP address with one TCP port of its own if the router
 * will not tell the two apart (docs/13, "Addresses").
 *
 * The access point (23 November 2026, docs/08 item 118): when Setup and Recovery makes the Pi's WiFi an access
 * point of its own, the WiFi gets the fixed address 192.168.4.1/24, no DHCP client runs, and the stack serves
 * its guests: a DHCP server and a DNS answerer (psmac_apnet.c) on ports 67 and 53. Replies still go back to
 * the Ethernet address a request came from, so nothing new is needed for ARP.
 *
 * Everything runs on core 3. The drivers' receive hook copies a frame into a small queue and the
 * service loop's poll does the work, so the hook costs a driver a few comparisons and a copy, and
 * the WiFi's coroutine never runs stack code. The kernel's frames wait in a queue that the drivers
 * empty when the Mac's transmit ring is empty: the Mac's frames always go first.
 */
#include <stdint.h>
#include "support.h"
#include "psmac.h"
#include "../alice/alice_net.h"
#include "psmac_ip.h"
#include "psmac_apnet.h"

const char *psmac_option(const char *key);              /* psmac_bus.c */
void sprintf(char *buf, const char *format, ...);        /* support.c */
extern volatile uint8_t *alice_net_board(void);          /* alice_net.c */

int psmac_ip_active;

static inline uint64_t now_ticks(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v)); return v; }
static inline uint64_t tick_hz(void) { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v; }
static uint32_t now_ms(void) { return (uint32_t)(now_ticks() / (tick_hz() / 1000u)); }

static inline uint16_t get16(const volatile uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t get32(const volatile uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static inline int eq(const volatile uint8_t *a, const volatile uint8_t *b, unsigned n) { while (n--) if (*a++ != *b++) return 0; return 1; }
static inline void cpy(volatile uint8_t *d, const volatile uint8_t *s, unsigned n) { while (n--) *d++ = *s++; }
static const uint8_t bcast_hw[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const uint8_t mdns_hw[6] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0xfb };
static const uint8_t mdns_ip[4] = { 224, 0, 0, 251 };
static const uint8_t zero4[4];

#define REMOTE_PORT_ALT 8068                /* always the kernel's; in the shared-address mode the only one */
#define FRAME_MAX 1536

/* ---- the interface ---- */
static struct {
    int unit;                               /* 0 WiFi, 1 wired, -1 none */
    int shared;                             /* 0 own MAC; 1 the Mac's MAC, own IP; 2 the Mac's MAC and IP */
    int want_link;                          /* remote.link: -1 either (wired first), 0 WiFi, 1 wired */
    int wifi_shared;                        /* the WiFi's default; remote.wifi=own clears it */
    int have_static; uint8_t st_ip[4], st_mask[4], st_gw[4];
    uint8_t own_hw[6]; int own_hw_set;      /* remote.mac= */
    uint8_t hw[6], ip[4], mask[4], gw[4];
    uint8_t mac_ip[2][4];                   /* the Mac's own address on each unit, learnt from its frames */
    uint32_t rx, tx, rx_dropped, tx_dropped;
    uint32_t sent;                          /* frames the driver took from the queue (tx counts those queued) */
    uint32_t arp_asks, arp_answers, pings, syns, resets;   /* for the heartbeat's line (psmac_ip_report) */
    uint32_t next_check;
    int ll_ok, ll;                          /* a link-local address when DHCP does not answer (Aurora68 Setup and Recovery); in use */
    int ap, relink, want_link_before;       /* the WiFi is an access point (Setup and Recovery): a fixed address, servers for its guests */
    uint8_t ap_ip[4];
    char name[32];
} I = { .unit = -1 };
static struct apn AP;                       /* the access point's DHCP leases and counts (psmac_apnet.c) */

/* ---- DHCP client state ---- */
enum { D_OFF, D_DISCOVER, D_REQUEST, D_BOUND };
static struct {
    int state; uint32_t xid, next_ms, t_bound, lease_s, tries;
    uint8_t server[4], offered[4];
} D;

/* ---- receive queue: the drivers' copies, worked through by the poll ---- */
/* 64 frames (23 November 2026; 16 before): a 64 KiB receive window is 45 segments, which a sender may send
   back to back, and a WiFi turn can hand over several at once */
#define RXQ 64
static struct { uint16_t len; uint8_t unit; uint8_t data[FRAME_MAX]; } rxq[RXQ];
static uint32_t rxq_head, rxq_tail;
/* ---- transmit queue: the kernel's frames, taken by the driver of its unit ---- */
#define TXQ 24
static struct { uint16_t len; uint8_t unit; uint8_t data[FRAME_MAX]; } txq[TXQ];
static uint32_t txq_head, txq_tail;

static uint8_t *tx_begin(void)
{
    if (I.unit < 0 || txq_head - txq_tail >= TXQ) { I.tx_dropped++; return 0; }
    return txq[txq_head % TXQ].data;
}
static void tx_commit(uint32_t len)
{
    txq[txq_head % TXQ].len = (uint16_t)(len < 60 ? 60 : len);   /* padded to Ethernet's shortest */
    for (uint32_t i = len; i < 60; i++) txq[txq_head % TXQ].data[i] = 0;
    txq[txq_head % TXQ].unit = (uint8_t)I.unit;
    txq_head++; I.tx++;
}
static int tx_room(void) { return TXQ - (int)(txq_head - txq_tail); }

/* the queue holds frames for the interface in use only: use_link empties it when the interface changes */
uint32_t psmac_ip_tx_take(int unit, uint8_t *buf, uint32_t max)
{
    if (!psmac_ip_active || txq_head == txq_tail || txq[txq_tail % TXQ].unit != unit) return 0;
    uint32_t n = txq[txq_tail % TXQ].len;
    if (n > max) { txq_tail++; I.tx_dropped++; return 0; }
    cpy(buf, txq[txq_tail % TXQ].data, n);
    txq_tail++; I.sent++;
    return n;
}
int psmac_ip_tx_pending(int unit) { return psmac_ip_active && txq_head != txq_tail && txq[txq_tail % TXQ].unit == unit; }

/* ---- checksums ---- */
static uint32_t sum16(const uint8_t *p, uint32_t n, uint32_t s)
{
    for (; n > 1; p += 2, n -= 2) s += (uint32_t)(p[0] << 8 | p[1]);
    if (n) s += (uint32_t)p[0] << 8;
    return s;
}
static uint16_t fold(uint32_t s) { while (s >> 16) s = (s & 0xffff) + (s >> 16); return (uint16_t)~s; }

/* an IPv4 frame's headers at f: Ethernet to dst_hw, IP to dst; payload starts at f + 34 */
static uint8_t *ip_header(uint8_t *f, const uint8_t *dst_hw, const uint8_t *src_ip, const uint8_t *dst, uint8_t proto, uint32_t payload, uint8_t ttl)
{
    static uint16_t ip_id = 0x6800;
    cpy(f, dst_hw, 6); cpy(f + 6, I.hw, 6); put16(f + 12, 0x0800);
    uint8_t *h = f + 14;
    h[0] = 0x45; h[1] = 0; put16(h + 2, 20 + payload); put16(h + 4, ip_id++); put16(h + 6, 0x4000);   /* don't fragment */
    h[8] = ttl; h[9] = proto; put16(h + 10, 0);
    cpy(h + 12, src_ip, 4); cpy(h + 16, dst, 4);
    put16(h + 10, fold(sum16(h, 20, 0)));
    return f + 34;
}
static uint16_t l4_sum(const uint8_t *src, const uint8_t *dst, uint8_t proto, const uint8_t *p, uint32_t n)
{
    uint32_t s = sum16(src, 4, 0);
    s = sum16(dst, 4, s);
    s += proto; s += n;
    return fold(sum16(p, n, s));
}

/* ---- configuration ---- */
static int parse_ip(const char *s, uint8_t out[4], const char **end)
{
    for (int i = 0; i < 4; i++) {
        uint32_t v = 0; int d = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (uint32_t)(*s++ - '0'); d++; }
        if (!d || v > 255) return 0;
        out[i] = (uint8_t)v;
        if (i < 3) { if (*s != '.') return 0; s++; }
    }
    if (end) *end = s;
    return 1;
}
static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static void ip_init(const char *default_name);
void psmac_ip_init(void) { ip_init("aurora68-pi"); }
/* Aurora68 Setup and Recovery (psmac_recovery.c, docs/08 item 106): no Mac runs, so on the WiFi the kernel takes the
   radio's address and asks for an IPv4 address of its own (nothing to share), and a link with no DHCP
   server (a laptop on a direct cable) gets a link-local address after about ten seconds */
void psmac_ip_init_recovery(const char *name)
{
    ip_init(name);
    I.wifi_shared = 0;
    I.ll_ok = 1;
}
/* the WiFi is an access point from now on: ip/24 on it alone, no DHCP client, a DHCP server for ip's .10 to .25
   and every DNS name answered with ip (docs/08 item 118) */
void psmac_ip_ap(const uint8_t ip[4])
{
    cpy(I.ap_ip, ip, 4);
    if (!I.ap) I.want_link_before = I.want_link;
    I.ap = 1; I.want_link = 0; I.relink = 1;
    apn_init(&AP, ip, 10, 600);
    kprintf("[REMOTE] access point mode: %d.%d.%d.%d/24 on the WiFi; DHCP for its guests (.10 to .25, ten-minute leases), every DNS name answered with %d.%d.%d.%d\n",
            ip[0], ip[1], ip[2], ip[3], ip[0], ip[1], ip[2], ip[3]);
}
/* the access point did not start: the links as before (the wired port first, the WiFi with DHCP) */
void psmac_ip_ap_off(void)
{
    if (!I.ap) return;
    I.ap = 0; I.want_link = I.want_link_before; I.relink = 1;
    kprintf("[REMOTE] access point mode off: the wired port or the WiFi as before\n");
}
int psmac_ip_ap_clients(void) { return I.ap ? apn_clients(&AP, now_ms() / 1000u) : 0; }
static void ip_init(const char *default_name)
{
    const char *o;
    I.want_link = -1;
    if ((o = psmac_option("remote.link"))) I.want_link = o[0] == 'w' && o[1] == 'i' ? 0 : o[0] == 'w' ? 1 : -1;   /* wifi | wired */
    /* On the WiFi the kernel shares the Mac's IP address by default and takes port 8068 alone. Its
       own address (the Mac's MAC, a second IP by DHCP client identifier) worked on the testbed's
       simulated radio, but the first real access point (21 November 2026) never delivered the ARP
       request for it: many access points allow one IP address per wireless station. remote.wifi=own
       asks for the own address anyway (a router and access point known to allow it). */
    I.wifi_shared = 1;
    if ((o = psmac_option("remote.wifi")) && o[0] == 'o') I.wifi_shared = 0;
    if ((o = psmac_option("remote.ip"))) {
        /* A.B.C.D/NN[,GATEWAY] */
        const char *e; uint8_t gw[4] = { 0 };
        if (parse_ip(o, I.st_ip, &e)) {
            uint32_t bits = 24;
            if (*e == '/') { bits = 0; for (e++; *e >= '0' && *e <= '9'; e++) bits = bits * 10 + (uint32_t)(*e - '0'); }
            if (bits > 32) bits = 24;
            uint32_t m = bits ? ~0u << (32 - bits) : 0;
            put32(I.st_mask, m);
            if (*e == ',' && parse_ip(e + 1, gw, 0)) cpy(I.st_gw, gw, 4);
            I.have_static = 1;
        } else kprintf("[REMOTE] remote.ip=%s not understood (A.B.C.D/NN,GATEWAY): using DHCP\n", o);
    }
    if ((o = psmac_option("remote.mac"))) {
        int k = 0;
        for (int i = 0; i < 6 && o[k]; i++) {
            int a = hexv(o[k]), b = hexv(o[k + 1]);
            if (a < 0 || b < 0) break;
            I.own_hw[i] = (uint8_t)(a << 4 | b); k += 2;
            if (o[k] == ':' || o[k] == '-') k++;
            if (i == 5) I.own_hw_set = 1;
        }
        if (I.own_hw_set) I.own_hw[0] &= 0xfe;               /* unicast */
    }
    const char *nm = psmac_option("remote.name");
    if (!nm) nm = default_name;
    unsigned n = 0;
    for (; nm[n] && n < sizeof I.name - 1; n++) I.name[n] = nm[n];
    I.name[n] = 0;
    psmac_ip_active = 1;
}
const char *psmac_ip_name(void) { return I.name; }

/* the wired chips with one address register: promiscuous while the kernel needs its own address */
int psmac_ip_promisc(int unit) { return psmac_ip_active && unit == 1; }
int psmac_ip_hwaddr(int unit, uint8_t out[6])
{
    if (!psmac_ip_active || unit != 1) return 0;
    if (I.own_hw_set) { cpy(out, I.own_hw, 6); return 1; }
    volatile uint8_t *nb = alice_net_board();
    if (!nb) return 0;
    volatile struct an_unit *u = (volatile struct an_unit *)(nb + AN_OFF_UNIT(1));
    /* the Pi's own address with the locally administered bit set and the last byte's top bit flipped:
       different from the Pi's whether that is a factory address or already a local one */
    cpy(out, u->factory_mac, 6);
    if (!out[0] && !out[1] && !out[2]) cpy(out, u->mac, 6);
    out[0] = (uint8_t)((out[0] | 0x02) & 0xfe);
    out[5] ^= 0x80;
    return 1;
}

/* ---- the receive hook ---- */
static int ip_is_mine(const volatile uint8_t *ip)
{
    return eq(ip, I.ip, 4) && !eq(I.ip, zero4, 4);
}
/* a frame on unit: does the kernel want a copy (1), is it the kernel's alone (2)? */
static int classify(int unit, const volatile uint8_t *f, uint32_t len, int unicast_to_us)
{
    if (unit != I.unit || len < 14) return 0;
    uint16_t type = get16(f + 12);
    if (type == 0x0806 && len >= 42) {                       /* ARP: a request for us is ours; a reply to us, too */
        if (I.shared == 2) return 0;
        if (get16(f + 20) == 1) return eq(f + 38, I.ip, 4) && !eq(I.ip, zero4, 4) ? 1 : 0;
        return unicast_to_us && eq(f + 38, I.ip, 4) ? 2 : 0;
    }
    if (type != 0x0800 || len < 34 || (f[14] >> 4) != 4) return unicast_to_us && !I.shared ? 2 : 0;
    uint32_t ihl = (f[14] & 15) * 4u;
    uint8_t proto = f[23];
    const volatile uint8_t *dst = f + 30, *l4 = f + 14 + ihl;
    if (14 + ihl + 8 > len) return 0;
    if (proto == 17) {
        uint16_t dport = get16(l4 + 2);
        if (dport == 68 && 14 + ihl + 8 + 8 <= len && get32(l4 + 8 + 4) == D.xid && D.state != D_OFF) return 2;   /* our DHCP answer */
        if (dport == 5353 && eq(dst, mdns_ip, 4)) return 1;  /* mDNS: the Mac may have a responder too */
        if (I.ap && (dport == 67 || dport == 53)) return 2;  /* the access point's guests: DHCP (broadcast) and DNS (to any server) */
    }
    if (I.shared == 2) {                                     /* the Mac's address: only TCP to our port */
        if (proto == 6 && eq(dst, I.ip, 4) && !eq(I.ip, zero4, 4) && get16(l4 + 2) == REMOTE_PORT_ALT) return 2;
        return 0;
    }
    if (ip_is_mine(dst)) return 2;
    if (unicast_to_us && !I.shared) return 2;                /* our own MAC address: ours whatever it is */
    return 0;
}
int psmac_ip_rx(int unit, const volatile uint8_t *f, uint32_t len, const volatile uint8_t *mac_addr, int mac_promisc)
{
    if (!psmac_ip_active || len < 14 || len > FRAME_MAX) return IPK_TO_MAC;
    int group = f[0] & 1;
    int to_us = !group && unit == I.unit && eq(f, I.hw, 6);
    int to_mac = group || mac_promisc || (mac_addr && eq(f, mac_addr, 6));
    int c = classify(unit, f, len, to_us);
    if (c) {
        if (rxq_head - rxq_tail < RXQ) {
            rxq[rxq_head % RXQ].len = (uint16_t)len; rxq[rxq_head % RXQ].unit = (uint8_t)unit;
            cpy(rxq[rxq_head % RXQ].data, f, len);
            rxq_head++; I.rx++;
        } else I.rx_dropped++;
        if (c == 2) return IPK_TAKEN;
    }
    if (to_mac) return IPK_TO_MAC;
    /* a unicast frame for neither: the chip passed it only because it is promiscuous for us */
    return psmac_ip_promisc(unit) ? IPK_DROP : IPK_TO_MAC;
}

/* the Mac's own frames: its address on that unit */
void psmac_ip_mac_tx(int unit, const volatile uint8_t *f, uint32_t len)
{
    if (!psmac_ip_active || (unit & ~1) || len < 34) return;
    const volatile uint8_t *src = 0;
    if (get16(f + 12) == 0x0800) src = f + 26;
    else if (get16(f + 12) == 0x0806 && len >= 42) src = f + 28;
    if (src && (src[0] | src[1] | src[2] | src[3]) && src[0] != 169)
        cpy(I.mac_ip[unit], src, 4);
}

/* ---- ARP and ICMP ---- */
static void arp_send(int op, const uint8_t *dst_hw, const uint8_t *tha, const uint8_t *tpa)
{
    uint8_t *f = tx_begin();
    if (!f) return;
    cpy(f, dst_hw, 6); cpy(f + 6, I.hw, 6); put16(f + 12, 0x0806);
    put16(f + 14, 1); put16(f + 16, 0x0800); f[18] = 6; f[19] = 4; put16(f + 20, (uint32_t)op);
    cpy(f + 22, I.hw, 6); cpy(f + 28, I.ip, 4); cpy(f + 32, tha, 6); cpy(f + 38, tpa, 4);
    tx_commit(42);
}
static void arp_in(const uint8_t *f, uint32_t len)
{
    if (len < 42 || get16(f + 14) != 1 || get16(f + 16) != 0x0800) return;
    if (get16(f + 20) == 1 && eq(f + 38, I.ip, 4) && !eq(I.ip, zero4, 4)) {
        I.arp_asks++;
        arp_send(2, f + 22, f + 22, f + 28);
        I.arp_answers++;
    }
}
static void icmp_in(const uint8_t *f, const uint8_t *ip, const uint8_t *p, uint32_t n)
{
    if (n < 8 || p[0] != 8 || !ip_is_mine(ip + 16) || I.shared == 2 || n > FRAME_MAX - 34) return;
    I.pings++;
    uint8_t *o = tx_begin();
    if (!o) return;
    uint8_t *q = ip_header(o, f + 6, I.ip, ip + 12, 1, n, 64);
    cpy(q, p, n);
    q[0] = 0; put16(q + 2, 0);
    put16(q + 2, fold(sum16(q, n, 0)));
    tx_commit(34 + n);
}

/* ---- UDP out ---- */
static void udp_send(const uint8_t *dst_hw, const uint8_t *src_ip, const uint8_t *dst, uint16_t sport, uint16_t dport, const uint8_t *data, uint32_t n, uint8_t ttl)
{
    if (n > FRAME_MAX - 42) return;
    uint8_t *f = tx_begin();
    if (!f) return;
    uint8_t *u = ip_header(f, dst_hw, src_ip, dst, 17, 8 + n, ttl);
    put16(u, sport); put16(u + 2, dport); put16(u + 4, 8 + n); put16(u + 6, 0);
    cpy(u + 8, data, n);
    uint16_t s = l4_sum(src_ip, dst, 17, u, 8 + n);
    put16(u + 6, s ? s : 0xffff);
    tx_commit(42 + n);
}

/* ---- DHCP (RFC 2131, RFC 2132) ---- */
static const char *link_name(int unit) { return unit == 1 ? "the wired port" : unit == 0 ? "the WiFi" : "no link"; }
static void announce(void);
static void dhcp_send(int type)
{
    static uint8_t m[300];
    for (unsigned i = 0; i < sizeof m; i++) m[i] = 0;
    m[0] = 1; m[1] = 1; m[2] = 6;                            /* BOOTREQUEST, Ethernet */
    put32(m + 4, D.xid);
    put16(m + 10, 0x8000);                                   /* answer by broadcast: the address is not ours yet */
    if (D.state == D_BOUND) cpy(m + 12, I.ip, 4);            /* ciaddr while renewing */
    cpy(m + 28, I.hw, 6);
    put32(m + 236, 0x63825363);                              /* the magic cookie */
    uint8_t *o = m + 240;
    *o++ = 53; *o++ = 1; *o++ = (uint8_t)type;
    /* the client identifier is the lease's key (RFC 2131 4.2): on the WiFi, where the kernel uses the
       Mac's MAC address, a name of its own keeps the two leases apart */
    unsigned nl = 0; while (I.name[nl]) nl++;
    *o++ = 61;
    if (I.shared) { *o++ = (uint8_t)(1 + nl); *o++ = 0; cpy(o, (const uint8_t *)I.name, nl); o += nl; }
    else { *o++ = 7; *o++ = 1; cpy(o, I.hw, 6); o += 6; }
    *o++ = 12; *o++ = (uint8_t)nl; cpy(o, (const uint8_t *)I.name, nl); o += nl;   /* host name: the router's client list shows it */
    *o++ = 57; *o++ = 2; put16(o, 1500); o += 2;
    *o++ = 55; *o++ = 3; *o++ = 1; *o++ = 3; *o++ = 51;     /* mask, router, lease */
    if (type == 3 && D.state == D_REQUEST) {
        *o++ = 50; *o++ = 4; cpy(o, D.offered, 4); o += 4;
        *o++ = 54; *o++ = 4; cpy(o, D.server, 4); o += 4;
    }
    *o++ = 255;
    udp_send(bcast_hw, D.state == D_BOUND ? I.ip : zero4, (const uint8_t *)"\xff\xff\xff\xff", 68, 67, m, (uint32_t)(o - m) < 300 ? 300 : (uint32_t)(o - m), 64);
}
static void dhcp_start(void)
{
    D.state = D_DISCOVER; D.xid = 0x41363800u ^ (uint32_t)now_ticks(); D.tries = 0; D.next_ms = now_ms();
}
static void bound(const char *how)
{
    char gw[20] = "none";
    if (!eq(I.gw, zero4, 4)) { sprintf(gw, "%d.%d.%d.%d", I.gw[0], I.gw[1], I.gw[2], I.gw[3]); }
    kprintf("[REMOTE] address %d.%d.%d.%d/%d.%d.%d.%d on %s (%s, router %s): http://%d.%d.%d.%d/ or http://%s.local/\n",
            I.ip[0], I.ip[1], I.ip[2], I.ip[3], I.mask[0], I.mask[1], I.mask[2], I.mask[3], link_name(I.unit), how, gw,
            I.ip[0], I.ip[1], I.ip[2], I.ip[3], I.name);
    if (I.shared != 2) arp_send(1, bcast_hw, (const uint8_t *)"\0\0\0\0\0\0", I.ip);     /* gratuitous: switches and caches learn us */
    announce();
}
/* WiFi: the router gave us the Mac's own address (it keys leases by MAC address), or none at all */
static void go_shared(const char *why)
{
    if (I.shared == 2) return;
    I.shared = 2; D.state = D_OFF;
    cpy(I.ip, I.mac_ip[0], 4); for (int i = 0; i < 4; i++) I.gw[i] = I.mask[i] = 0;
    kprintf("[REMOTE] %s: the kernel shares the Mac's address on the WiFi and takes TCP port %u: http://%d.%d.%d.%d:%u/\n",
            why, REMOTE_PORT_ALT, I.ip[0], I.ip[1], I.ip[2], I.ip[3], REMOTE_PORT_ALT);
}
static void dhcp_in(const uint8_t *p, uint32_t n)
{
    if (n < 240 || p[0] != 2 || get32(p + 4) != D.xid || get32(p + 236) != 0x63825363 || D.state == D_OFF) return;
    int type = 0; uint8_t server[4] = { 0 }, mask[4] = { 255, 255, 255, 0 }, router[4] = { 0 }; uint32_t lease = 3600;
    for (uint32_t k = 240; k + 1 < n && p[k] != 255; ) {
        if (p[k] == 0) { k++; continue; }
        uint8_t t = p[k], l = p[k + 1];
        const uint8_t *v = p + k + 2;
        if (k + 2 + l > n) break;
        if (t == 53 && l >= 1) type = v[0];
        else if (t == 54 && l >= 4) cpy(server, v, 4);
        else if (t == 1 && l >= 4) cpy(mask, v, 4);
        else if (t == 3 && l >= 4) cpy(router, v, 4);
        else if (t == 51 && l >= 4) lease = get32(v);
        k += 2u + l;
    }
    if (type == 2 && D.state == D_DISCOVER) {               /* OFFER */
        cpy(D.offered, p + 16, 4); cpy(D.server, server, 4);
        D.state = D_REQUEST; D.tries = 0; D.next_ms = now_ms();
    } else if (type == 5 && (D.state == D_REQUEST || D.state == D_BOUND)) {   /* ACK */
        int fresh = D.state == D_REQUEST || !eq(I.ip, p + 16, 4);
        I.ll = 0;
        cpy(I.ip, p + 16, 4); cpy(I.mask, mask, 4); cpy(I.gw, router, 4);
        D.state = D_BOUND; D.lease_s = lease < 60 ? 60 : lease; D.t_bound = now_ms(); D.tries = 0;
        D.next_ms = D.t_bound + D.lease_s * 500u;           /* renew at half the lease */
        if (I.shared == 1 && eq(I.ip, I.mac_ip[0], 4)) { go_shared("the router gave the kernel the Mac's own address"); return; }
        if (fresh) {
            char how[48]; sprintf(how, "DHCP from %d.%d.%d.%d, lease %u s", server[0], server[1], server[2], server[3], D.lease_s);
            bound(how);
        }
    } else if (type == 6) {                                  /* NAK: start again */
        kprintf("[REMOTE] DHCP: the server said no; asking again\n");
        for (int i = 0; i < 4; i++) I.ip[i] = 0;
        dhcp_start();
    }
}
static void dhcp_poll(void)
{
    uint32_t t = now_ms();
    if (D.state == D_OFF || (int32_t)(t - D.next_ms) < 0) return;
    if (D.state == D_DISCOVER || D.state == D_REQUEST) {
        if (D.state == D_REQUEST && D.tries >= 4) { D.state = D_DISCOVER; D.tries = 0; }
        dhcp_send(D.state == D_DISCOVER ? 1 : 3);
        D.tries++;
        D.next_ms = t + (D.tries < 3 ? 2000u : I.ll ? 15000u : 4000u);
        /* no DHCP server after five tries (about twelve seconds): an address from 169.254/16 (RFC 3927), made
           from the MAC address so it is the same at every start, while DHCP goes on asking more slowly. A
           laptop on a direct cable gives itself one too, and finds the kernel by mDNS (NAME.local). The
           address is not probed first: on a cable with one laptop there is nobody to collide with */
        if (I.ll_ok && !I.ll && D.state == D_DISCOVER && D.tries == 5 && eq(I.ip, zero4, 4)) {
            I.ll = 1;
            I.ip[0] = 169; I.ip[1] = 254; I.ip[2] = (uint8_t)(1 + (I.hw[4] ^ I.hw[2]) % 254); I.ip[3] = (uint8_t)(1 + (I.hw[5] ^ I.hw[3]) % 254);
            I.mask[0] = 255; I.mask[1] = 255; I.mask[2] = 0; I.mask[3] = 0;
            for (int i = 0; i < 4; i++) I.gw[i] = 0;
            bound("link-local: no DHCP server answered; DHCP still asked every 15 s");
        }
        /* the WiFi, nobody answering after half a minute: the Mac's address, if it has one */
        if (I.shared == 1 && D.state == D_DISCOVER && D.tries == 8 && I.mac_ip[0][0]) go_shared("no DHCP answer for the kernel on the WiFi");
        return;
    }
    /* bound: renew every quarter of the lease after the first half; give it up when it runs out */
    if (t - D.t_bound >= D.lease_s * 1000u) {
        kprintf("[REMOTE] DHCP: the lease ran out\n");
        for (int i = 0; i < 4; i++) I.ip[i] = 0;
        dhcp_start();
        return;
    }
    dhcp_send(3);
    D.next_ms = t + D.lease_s * 250u;
}

/* ---- the access point's servers (psmac_apnet.c) ---- */
static void dhcps_in(const uint8_t *u, uint32_t n)
{
    static uint8_t out[600];
    int bc; uint8_t to_ip[4], to_hw[6];
    if (n < 8) return;
    uint32_t len = apn_dhcp(&AP, now_ms() / 1000u, u + 8, n - 8, out, sizeof out, &bc, to_ip, to_hw);
    if (!len) return;
    udp_send(bc ? bcast_hw : to_hw, I.ip, bc ? (const uint8_t *)"\xff\xff\xff\xff" : to_ip, 67, 68, out, len, 64);
    if (out[242] == 5 && out[16])                            /* an ACK with an address: the guest is on (its name and address not logged) */
        kprintf("[REMOTE] access point: DHCP gave %d.%d.%d.%d to a device (%d with a lease)\n", out[16], out[17], out[18], out[19], apn_clients(&AP, now_ms() / 1000u));
    else if (out[242] == 6) kprintf("[REMOTE] access point: DHCP said no to an address of another network (the device asks again)\n");
}
static void dns_in(const uint8_t *f, const uint8_t *ip, const uint8_t *u, uint32_t n)
{
    static uint8_t out[600];
    if (n < 8) return;
    uint32_t len = apn_dns(&AP, u + 8, n - 8, out, sizeof out);
    /* from the address it was sent to: a phone set to use another DNS server sends it here too, through the "router" */
    if (len) udp_send(f + 6, ip + 16, ip + 12, 53, get16(u), out, len, 64);
}

/* ---- mDNS (RFC 6762): "NAME.local" A ---- */
static int name_lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
/* the name at p in the message m (n bytes) is NAME.local? *end gets the byte after it */
static int mdns_name_is_ours(const uint8_t *m, uint32_t n, uint32_t p, uint32_t *end)
{
    char got[80]; uint32_t g = 0, jumps = 0, ret = 0;
    *end = n;
    while (p < n) {
        uint8_t l = m[p];
        if (l == 0) { p++; break; }
        if ((l & 0xc0) == 0xc0) { if (p + 1 >= n || ++jumps > 8) return 0; if (!ret) ret = p + 2; p = (uint32_t)(l & 0x3f) << 8 | m[p + 1]; continue; }
        if (p + 1 + l > n || g + l + 1 >= sizeof got) return 0;
        if (g) got[g++] = '.';
        for (uint32_t i = 0; i < l; i++) got[g++] = (char)name_lower(m[p + 1 + i]);
        p += 1u + l;
    }
    got[g] = 0;
    *end = ret ? ret : p;
    unsigned k = 0;
    for (; I.name[k]; k++) if (name_lower(I.name[k]) != got[k]) return 0;
    const char *loc = ".local";
    for (unsigned j = 0; loc[j]; j++) if (got[k + j] != loc[j]) return 0;
    return got[k + 6] == 0;
}
static uint32_t mdns_answer(uint8_t *o, int legacy)
{
    uint8_t *q = o;
    for (const char *s = I.name; ; ) {                      /* the name, label by label */
        const char *e = s; while (*e && *e != '.') e++;
        *q++ = (uint8_t)(e - s); for (; s < e; s++) *q++ = (uint8_t)*s;
        if (!*e) break;
        s = e + 1;
    }
    *q++ = 5; cpy(q, (const uint8_t *)"local", 5); q += 5; *q++ = 0;
    put16(q, 1); put16(q + 2, legacy ? 1 : 0x8001);          /* A, IN (cache flush on the multicast answer) */
    put32(q + 4, legacy ? 10 : 120); put16(q + 8, 4); cpy(q + 10, I.ip, 4);
    return (uint32_t)(q + 14 - o);
}
static void announce(void)
{
    uint8_t m[128];
    for (int i = 0; i < 12; i++) m[i] = 0;
    put16(m + 2, 0x8400); put16(m + 6, 1);
    uint32_t n = 12 + mdns_answer(m + 12, 0);
    udp_send(mdns_hw, I.ip, mdns_ip, 5353, 5353, m, n, 255);
}
static void mdns_in(const uint8_t *f, const uint8_t *ip, const uint8_t *u, uint32_t n)
{
    if (n < 8 + 12 || eq(I.ip, zero4, 4)) return;
    const uint8_t *m = u + 8; uint32_t mn = n - 8;
    uint16_t sport = get16(u), flags = get16(m + 2), qd = get16(m + 4);
    if (flags & 0x8000) return;                              /* an answer, not a question */
    uint32_t p = 12;
    for (uint32_t i = 0; i < qd && i < 8; i++) {
        uint32_t end = mn;
        int ours = mdns_name_is_ours(m, mn, p, &end);
        if (end + 4 > mn) return;
        uint16_t qt = get16(m + end), qc = get16(m + end + 2) & 0x7fff;
        p = end + 4;
        if (!ours || qc != 1 || (qt != 1 && qt != 255)) continue;
        uint8_t r[160]; int legacy = sport != 5353;
        for (int k = 0; k < 12; k++) r[k] = 0;
        put16(r + 2, 0x8400); put16(r + 6, 1);
        uint32_t rn = 12;
        if (legacy) {                                        /* RFC 6762 6.7: the question echoed, the id kept */
            put16(r, get16(m)); put16(r + 4, 1);
            rn += mdns_answer(r + rn, 1) - 14;               /* the name ... */
            put16(r + rn, 1); put16(r + rn + 2, 1); rn += 4;  /* ... as a question */
            rn += mdns_answer(r + rn, 1);
            udp_send(f + 6, I.ip, ip + 12, 5353, sport, r, rn, 64);
        } else {
            rn += mdns_answer(r + rn, 0);
            udp_send(mdns_hw, I.ip, mdns_ip, 5353, 5353, r, rn, 255);
        }
        return;
    }
}

/* ---- TCP ---- */
#define SBUF 32768u                         /* a power of two: the send buffer is a ring */
/* The receive window: what the application can take now (remote_tcp_room), at most RCV_WIN. Until 21
   November 2026 it was always 8 KiB and everything that came was handed over; an upload to the Mac's
   disk (psmac_files.c) goes only as fast as the Mac writes it, so the window now closes while the file
   job's buffer is full and opens again (a window update) when the Mac has taken a chunk. On 23 November
   it grew from 24 KiB to the most a window without scaling says, with the file job's buffer (64 KiB,
   psmac_files.c): a window of exactly one chunk left the last 1216 bytes of every chunk to a BSD sender's
   persist timer, five seconds each (docs/08 item 123). */
#define RCV_WIN 65535u
enum { T_FREE, T_SYN_RCVD, T_ESTAB, T_CLOSING };
struct tcb {
    int state;
    uint8_t hw[6], ip[4];
    uint16_t lport, rport, mss;
    uint32_t una, nxt, rcv, iss;            /* snd.una, snd.nxt, rcv.nxt */
    uint32_t win;                           /* the peer's window */
    uint32_t sb_start, sb_len;              /* the ring: sb_len bytes from una on, sent or not */
    int app_done, fin_sent, fin_acked, peer_fin, need_ack;
    uint32_t rto_ms, rto_at, retries, last_ms;
    uint32_t adv;                           /* the window last advertised */
    uint8_t sb[SBUF];
};
static struct tcb tcbs[IPK_CONNS];
static uint32_t conns_total;
static uint32_t rcv_room(struct tcb *t)
{
    if (t < tcbs || t >= tcbs + IPK_CONNS) return 0;           /* a reset's reply: no connection */
    uint32_t r = remote_tcp_room((int)(t - tcbs));
    return r < RCV_WIN ? r : RCV_WIN;
}

static void tcp_segment(struct tcb *t, uint32_t seq, uint8_t flags, const uint8_t *data1, uint32_t n1, const uint8_t *data2, uint32_t n2, int syn_opts)
{
    uint8_t *f = tx_begin();
    if (!f) return;
    uint32_t hl = syn_opts ? 24 : 20, n = n1 + n2;
    uint8_t *h = ip_header(f, t->hw, I.ip, t->ip, 6, hl + n, 64);
    put16(h, t->lport); put16(h + 2, t->rport); put32(h + 4, seq); put32(h + 8, t->rcv);
    t->adv = rcv_room(t);
    h[12] = (uint8_t)((hl / 4) << 4); h[13] = flags; put16(h + 14, (uint16_t)t->adv); put16(h + 16, 0); put16(h + 18, 0);
    if (syn_opts) { h[20] = 2; h[21] = 4; put16(h + 22, 1460); }
    cpy(h + hl, data1, n1); cpy(h + hl + n1, data2, n2);
    put16(h + 16, l4_sum(I.ip, t->ip, 6, h, hl + n));
    tx_commit(34 + hl + n);
    t->need_ack = 0;
}
static void tcp_reset_reply(const uint8_t *f, const uint8_t *ip, const uint8_t *h, uint32_t dlen)
{
    struct tcb t = { 0 };
    cpy(t.hw, f + 6, 6); cpy(t.ip, ip + 12, 4);
    t.lport = get16(h + 2); t.rport = get16(h);
    uint8_t fl = h[13];
    if (fl & 0x04) return;
    if (fl & 0x10) { t.rcv = 0; tcp_segment(&t, get32(h + 8), 0x04, 0, 0, 0, 0, 0); }
    else { t.rcv = get32(h + 4) + dlen + ((fl & 0x02) ? 1 : 0) + ((fl & 0x01) ? 1 : 0); tcp_segment(&t, 0, 0x14, 0, 0, 0, 0, 0); }
}
static void tcb_free(int c, const char *why)
{
    struct tcb *t = &tcbs[c];
    if (t->state == T_FREE) return;
    (void)why;
    t->state = T_FREE;
    remote_tcp_closed(c);
}
void ipk_tcp_close(int c) { if (c >= 0 && c < IPK_CONNS && tcbs[c].state != T_FREE) tcbs[c].app_done = 1; }
void ipk_tcp_abort(int c)
{
    if (c < 0 || c >= IPK_CONNS || tcbs[c].state == T_FREE) return;
    tcp_segment(&tcbs[c], tcbs[c].nxt, 0x14, 0, 0, 0, 0, 0);
    tcb_free(c, "aborted");
}
/* send what the window allows: new data from the application, a FIN when it is done */
static void tcp_output(int c)
{
    struct tcb *t = &tcbs[c];
    if (t->state != T_ESTAB && t->state != T_CLOSING) return;
    if (!t->app_done && t->sb_len < SBUF) {
        uint32_t end = (t->sb_start + t->sb_len) & (SBUF - 1);
        uint32_t room = SBUF - t->sb_len, cont = SBUF - end;
        uint32_t got = remote_tcp_produce(c, t->sb + end, room < cont ? room : cont);
        if (got > (room < cont ? room : cont)) got = 0;
        t->sb_len += got;
    }
    uint32_t win = t->win ? t->win : 1;                      /* a zero window: probe with one byte at the retransmission time */
    for (;;) {
        uint32_t off = t->nxt - t->una;
        if (t->fin_sent) break;
        if (off >= t->sb_len) {
            if (t->app_done) {
                if (tx_room() < 1) break;
                tcp_segment(t, t->nxt, 0x11, 0, 0, 0, 0, 0);
                t->nxt++; t->fin_sent = 1; t->state = T_CLOSING;
                if (!t->rto_at) t->rto_at = now_ms() + t->rto_ms;
            }
            break;
        }
        if (off >= win || tx_room() < 2) break;              /* leave the queue room for ACKs and ARP */
        uint32_t n = t->sb_len - off;
        if (n > t->mss) n = t->mss;
        if (n > win - off) n = win - off;
        if (!t->win && off) break;
        uint32_t a = (t->sb_start + off) & (SBUF - 1), n1 = n <= SBUF - a ? n : SBUF - a;
        tcp_segment(t, t->nxt, (uint8_t)(0x10 | (off + n == t->sb_len ? 0x08 : 0)), t->sb + a, n1, t->sb, n - n1, 0);
        t->nxt += n;
        if (!t->rto_at) t->rto_at = now_ms() + t->rto_ms;
    }
    /* a window update once the application has taken enough to matter (two segments' worth), or as soon as
       a window below one segment has grown at all: the end of an upload may need only a few hundred bytes
       more, and a sender held at a zero window waits for an update (the first test of an upload whose last
       chunk was 324 bytes stalled until the 30 s idle timeout) */
    uint32_t room = rcv_room(t);
    if (!t->peer_fin && room > t->adv && (t->adv < t->mss || room >= t->adv + 2u * t->mss)) t->need_ack = 1;
    if (t->need_ack) tcp_segment(t, t->nxt, 0x10, 0, 0, 0, 0, 0);
}
static void tcp_in(const uint8_t *f, const uint8_t *ip, const uint8_t *h, uint32_t n)
{
    if (n < 20) return;
    uint32_t hl = (h[12] >> 4) * 4u;
    if (hl < 20 || hl > n || l4_sum(ip + 12, ip + 16, 6, h, n) != 0) return;
    uint16_t sport = get16(h), dport = get16(h + 2);
    uint32_t seq = get32(h + 4), ack = get32(h + 8), dlen = n - hl;
    uint8_t fl = h[13];
    const uint8_t *data = h + hl;
    int c = -1;
    for (int i = 0; i < IPK_CONNS; i++)
        if (tcbs[i].state != T_FREE && tcbs[i].rport == sport && tcbs[i].lport == dport && eq(tcbs[i].ip, ip + 12, 4)) { c = i; break; }
    if (c < 0) {
        if ((fl & 0x17) != 0x02) { I.resets++; tcp_reset_reply(f, ip, h, dlen); return; }   /* not a bare SYN */
        I.syns++;
        int ok_port = I.shared == 2 ? dport == REMOTE_PORT_ALT : (dport == 80 || dport == REMOTE_PORT_ALT);
        for (int i = 0; i < IPK_CONNS && c < 0; i++) if (tcbs[i].state == T_FREE) c = i;
        if (!ok_port || c < 0) { if (!ok_port) { I.resets++; tcp_reset_reply(f, ip, h, dlen); } return; }   /* all busy: the client tries again */
        struct tcb *t = &tcbs[c];
        t->state = T_SYN_RCVD; cpy(t->hw, f + 6, 6); cpy(t->ip, ip + 12, 4);
        t->lport = dport; t->rport = sport; t->rcv = seq + 1;
        t->iss = 0x41360000u ^ (uint32_t)(now_ticks() * 2654435761u); t->una = t->iss; t->nxt = t->iss + 1;
        t->win = get16(h + 14); t->mss = 536;
        for (uint32_t k = 20; k + 1 < hl; ) {                /* the peer's MSS */
            uint8_t kind = h[k];
            if (kind == 0) break;
            if (kind == 1) { k++; continue; }
            if (kind == 2 && h[k + 1] == 4 && k + 4 <= hl) t->mss = get16(h + k + 2);
            if (h[k + 1] < 2) break;
            k += h[k + 1];
        }
        if (t->mss > 1460) t->mss = 1460;
        if (t->mss < 64) t->mss = 64;
        t->sb_start = t->sb_len = 0; t->app_done = t->fin_sent = t->fin_acked = t->peer_fin = t->need_ack = 0;
        t->rto_ms = 300; t->rto_at = now_ms() + t->rto_ms; t->retries = 0; t->last_ms = now_ms();
        if (!remote_tcp_accept(c, dport, t->ip)) { t->state = T_FREE; I.resets++; tcp_reset_reply(f, ip, h, dlen); return; }
        conns_total++;
        tcp_segment(t, t->iss, 0x12, 0, 0, 0, 0, 1);
        return;
    }
    struct tcb *t = &tcbs[c];
    t->last_ms = now_ms();
    if (fl & 0x04) { tcb_free(c, "reset by the peer"); return; }
    if (fl & 0x02) { if (t->state == T_SYN_RCVD) tcp_segment(t, t->iss, 0x12, 0, 0, 0, 0, 1); return; }   /* its SYN again */
    if (!(fl & 0x10)) return;
    /* the acknowledgement */
    if ((int32_t)(ack - t->una) > 0 && (int32_t)(ack - t->nxt) <= 0) {
        uint32_t acked = ack - t->una;
        if (t->state == T_SYN_RCVD) { acked--; t->state = T_ESTAB; }   /* our SYN */
        uint32_t data_acked = acked > t->sb_len ? t->sb_len : acked;
        t->sb_start = (t->sb_start + data_acked) & (SBUF - 1); t->sb_len -= data_acked;
        if (t->fin_sent && ack == t->nxt) t->fin_acked = 1;
        t->una = ack; t->retries = 0; t->rto_ms = 300;
        t->rto_at = t->una == t->nxt ? 0 : now_ms() + t->rto_ms;
    } else if (t->state == T_SYN_RCVD && ack == t->una + 1) { t->state = T_ESTAB; t->una = ack; t->rto_at = 0; }
    t->win = get16(h + 14);
    /* data, in order only. A segment that begins before rcv.nxt but runs past it (a retransmission that
       overlaps what was already taken, as after a segment taken only in part because the window was
       nearly full) is trimmed to its new bytes rather than dropped: dropping it left the sender resending
       the same overlapping segment (docs/08 item 95, an upload over the owner's WiFi that stalled). */
    if (dlen && seq != t->rcv && (int32_t)(t->rcv - seq) > 0 && (int32_t)(seq + dlen - t->rcv) > 0) {
        uint32_t old = t->rcv - seq;
        data += old; dlen -= old; seq = t->rcv;
    }
    if (dlen || (fl & 0x01)) {
        if (seq == t->rcv) {
            uint32_t room = rcv_room(t), take = dlen < room ? dlen : room;   /* what does not fit comes again */
            if (take) { remote_tcp_recv(c, data, take); t->rcv += take; }
            if ((fl & 0x01) && take == dlen) { t->rcv++; t->peer_fin = 1; }
        }
        t->need_ack = 1;
    }
    if (t->state == T_FREE) return;                          /* the application aborted it */
    if (t->fin_acked && t->peer_fin) { if (t->need_ack) tcp_segment(t, t->nxt, 0x10, 0, 0, 0, 0, 0); tcb_free(c, "closed"); return; }
    tcp_output(c);
}
static void tcp_timers(void)
{
    uint32_t t_ms = now_ms();
    for (int c = 0; c < IPK_CONNS; c++) {
        struct tcb *t = &tcbs[c];
        if (t->state == T_FREE) continue;
        if (t_ms - t->last_ms > 30000u || (t->fin_acked && t_ms - t->last_ms > 5000u)) { ipk_tcp_abort(c); continue; }   /* idle, or its FIN never came */
        if (t->rto_at && (int32_t)(t_ms - t->rto_at) >= 0) {
            if (++t->retries > 8) { ipk_tcp_abort(c); continue; }
            if (t->state == T_SYN_RCVD) tcp_segment(t, t->iss, 0x12, 0, 0, 0, 0, 1);
            else { t->nxt = t->una; if (t->fin_sent && !t->fin_acked) t->fin_sent = 0; }   /* go back to the oldest byte */
            t->rto_ms = t->rto_ms * 2 > 3000 ? 3000 : t->rto_ms * 2;
            t->rto_at = t_ms + t->rto_ms;
        }
        tcp_output(c);
    }
}

/* ---- a received frame ---- */
static void ip_in(const uint8_t *f, uint32_t len)
{
    const uint8_t *ip = f + 14;
    uint32_t ihl = (ip[0] & 15) * 4u, tot = get16(ip + 2);
    if (ihl < 20 || tot < ihl || 14 + tot > len || fold(sum16(ip, ihl, 0)) != 0) return;
    if (get16(ip + 6) & 0x3fff) return;                      /* fragments: nothing of ours is ever that big */
    const uint8_t *p = ip + ihl; uint32_t n = tot - ihl;
    switch (ip[9]) {
    case 1: icmp_in(f, ip, p, n); break;
    case 6: if (I.shared == 2 ? eq(ip + 16, I.ip, 4) : ip_is_mine(ip + 16)) tcp_in(f, ip, p, n); break;
    case 17:
        if (n < 8) break;
        if (get16(p + 2) == 68) dhcp_in(p + 8, n - 8);
        else if (get16(p + 2) == 5353) mdns_in(f, ip, p, n);
        else if (I.ap && get16(p + 2) == 67) dhcps_in(p, n);
        else if (I.ap && get16(p + 2) == 53) dns_in(f, ip, p, n);
        break;
    }
}

/* ---- which link ---- */
static volatile struct an_unit *unit_of(int u)
{
    volatile uint8_t *nb = alice_net_board();
    return nb ? (volatile struct an_unit *)(nb + AN_OFF_UNIT(u)) : 0;
}
static int link_up(int u)
{
    volatile struct an_unit *x = unit_of(u);
    if (!x || !x->link) return 0;
    return u == 1 ? x->kind == AN_KIND_ETHER : x->kind == AN_KIND_WIFI;
}
static void use_link(int u)
{
    for (int c = 0; c < IPK_CONNS; c++) if (tcbs[c].state != T_FREE) tcb_free(c, "the link changed");
    txq_tail = txq_head; rxq_tail = rxq_head;
    for (int i = 0; i < 4; i++) I.ip[i] = I.mask[i] = I.gw[i] = 0;
    I.ll = 0;
    D.state = D_OFF;
    I.unit = u;
    if (u < 0) { kprintf("[REMOTE] no network link: waiting for one\n"); return; }
    if (I.ap && u == 0) {                                    /* the access point: its own fixed address, no DHCP client */
        volatile struct an_unit *x = unit_of(0);
        cpy(I.hw, x->mac, 6); I.shared = 0;
        cpy(I.ip, I.ap_ip, 4); I.mask[0] = I.mask[1] = I.mask[2] = 255; I.mask[3] = 0;
        bound("the access point's own fixed address; a DHCP server and DNS for its guests");
        return;
    }
    if (u == 1) { psmac_ip_hwaddr(1, I.hw); I.shared = 0; }
    else { volatile struct an_unit *x = unit_of(0); cpy(I.hw, x->mac, 6); I.shared = I.wifi_shared ? 2 : 1; }
    kprintf("[REMOTE] on %s, MAC address %02x:%02x:%02x:%02x:%02x:%02x%s\n", link_name(u),
            I.hw[0], I.hw[1], I.hw[2], I.hw[3], I.hw[4], I.hw[5], I.shared ? " (the Mac's: a station has one)" : " (the kernel's own)");
    if (I.shared == 2) { I.shared = 1; go_shared("the WiFi's default (remote.wifi=own for an address of its own)"); return; }
    if (I.have_static) {
        cpy(I.ip, I.st_ip, 4); cpy(I.mask, I.st_mask, 4); cpy(I.gw, I.st_gw, 4);
        bound("static, remote.ip=");
        return;
    }
    dhcp_start();
}

void psmac_ip_poll(void)
{
    if (!psmac_ip_active) return;
    uint32_t t = now_ms();
    if ((int32_t)(t - I.next_check) >= 0) {                 /* the link, twice a second */
        I.next_check = t + 500;
        int w = link_up(1), wi = link_up(0), want;
        if (I.want_link == 1) want = w ? 1 : -1;
        else if (I.want_link == 0) want = wi ? 0 : -1;
        else want = w ? 1 : wi ? 0 : -1;
        if (want != I.unit || I.relink) { I.relink = 0; use_link(want); }
        if (I.shared == 2 && !eq(I.ip, I.mac_ip[0], 4)) {   /* the Mac's address moved: follow it */
            cpy(I.ip, I.mac_ip[0], 4);
            kprintf("[REMOTE] the Mac's WiFi address is now %d.%d.%d.%d: http://%d.%d.%d.%d:%u/\n", I.ip[0], I.ip[1], I.ip[2], I.ip[3], I.ip[0], I.ip[1], I.ip[2], I.ip[3], REMOTE_PORT_ALT);
        }
        if (I.shared == 1 && D.state == D_BOUND && eq(I.ip, I.mac_ip[0], 4)) go_shared("the Mac took the kernel's address too");
    }
    for (int guard = 0; guard < RXQ && rxq_tail != rxq_head; guard++) {
        uint32_t k = rxq_tail % RXQ;
        const uint8_t *f = rxq[k].data;
        if (rxq[k].unit == I.unit) {
            uint16_t type = get16(f + 12);
            if (type == 0x0806) arp_in(f, rxq[k].len);
            else if (type == 0x0800) ip_in(f, rxq[k].len);
        }
        rxq_tail++;
    }
    if (I.unit >= 0) { dhcp_poll(); tcp_timers(); }
}

/* work waiting: frames in, frames out, a connection open (the poll then runs every turn) */
int psmac_ip_busy(void)
{
    if (rxq_head != rxq_tail || txq_head != txq_tail) return 1;
    for (int c = 0; c < IPK_CONNS; c++) if (tcbs[c].state != T_FREE) return 1;
    return 0;
}

void psmac_ip_info(struct ipk_info *o)
{
    o->unit = I.unit; o->shared = I.shared;
    cpy(o->hw, I.hw, 6); cpy(o->ip, I.ip, 4); cpy(o->mask, I.mask, 4); cpy(o->gw, I.gw, 4);
    cpy(o->mac_ip[0], I.mac_ip[0], 4); cpy(o->mac_ip[1], I.mac_ip[1], 4);
    o->lease_s = D.state == D_BOUND ? D.lease_s : 0;
    o->rx = I.rx; o->tx = I.tx; o->rx_dropped = I.rx_dropped; o->tx_dropped = I.tx_dropped; o->conns = conns_total;
    o->phase = I.unit < 0 ? "no link" : I.ap && I.unit == 0 ? "access point" : I.shared == 2 ? "shared" : I.have_static ? "static" : D.state == D_BOUND ? "bound" : I.ll ? "link-local" : "DHCP";
}

/* one line for the kernel's heartbeat (psmac_bus.c): the remote control's address and what came and
   went, printed only when something changed so Aurora68 Log is not buried. It keeps the address in
   the log after the start-up lines have scrolled away, and shows where a connection stops: ARP asked
   and answered, pings answered, SYNs seen, frames queued against frames the driver actually sent */
void psmac_ip_report(void)
{
    if (!psmac_ip_active) return;
    static uint32_t last[11]; static int last_unit = -2, last_shared = -1;
    uint32_t now[11] = { I.rx, I.tx, I.sent, I.rx_dropped + I.tx_dropped, I.arp_asks, I.pings, I.syns, I.resets, get32(I.ip), AP.acks, AP.dns_queries };
    int same = I.unit == last_unit && I.shared == last_shared;
    for (int i = 0; i < 11; i++) { if (now[i] != last[i]) same = 0; last[i] = now[i]; }
    last_unit = I.unit; last_shared = I.shared;
    if (same) return;
    if (I.unit < 0) { kprintf("[REMOTE]   no network link\n"); return; }
    kprintf("[REMOTE]   %d.%d.%d.%d on %s (%s): in %u, out %u queued %u sent, %u dropped | ARP asks %u answered %u, pings %u, SYNs %u, resets %u\n",
            I.ip[0], I.ip[1], I.ip[2], I.ip[3], link_name(I.unit),
            I.shared == 2 ? "the Mac's MAC and IP, port 8068" : I.shared ? "the Mac's MAC, its own IP" : "its own MAC and IP",
            I.rx, I.tx, I.sent, I.rx_dropped + I.tx_dropped, I.arp_asks, I.arp_answers, I.pings, I.syns, I.resets);
    if (I.ap) kprintf("[REMOTE]   access point: %d device%s with a lease; DHCP %u discovers, %u offers, %u acks, %u naks; DNS %u queries, %u answered with an address\n",
                      apn_clients(&AP, now_ms() / 1000u), apn_clients(&AP, now_ms() / 1000u) == 1 ? "" : "s", AP.discovers, AP.offers, AP.acks, AP.naks, AP.dns_queries, AP.dns_answers);
}
