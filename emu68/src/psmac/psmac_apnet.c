/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_apnet.c - the access point's DHCP server (RFC 2131, RFC 2132) and DNS answerer (RFC 1035): see
 * psmac_apnet.h. 23 November 2026, docs/08 item 117.
 *
 * The DHCP server is the smallest that phones and laptops accept: sixteen leases keyed by the client's
 * hardware address (a phone that comes back gets its address again), DISCOVER answered with an OFFER,
 * REQUEST with an ACK or a NAK (a request for an address of another network, as a phone that was last on
 * a home network makes on joining, is refused at once so that it starts again), RELEASE and DECLINE
 * honoured, INFORM answered. Every answer names the Pi as router and DNS server: phones check a network
 * for the internet by asking for a known name and fetching a page from it, and with every name leading to
 * the Pi that fetch reaches the recovery page's server, which sends the browser to the page (the captive
 * portal sign-in that phones then offer).
 */
#include <stdint.h>
#include "psmac_apnet.h"

static uint16_t g16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void p16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void p32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static int same(const uint8_t *a, const uint8_t *b, unsigned n) { while (n--) if (*a++ != *b++) return 0; return 1; }
static void cp(uint8_t *d, const uint8_t *s, unsigned n) { while (n--) *d++ = *s++; }
static void zero(uint8_t *d, unsigned n) { while (n--) *d++ = 0; }

void apn_init(struct apn *a, const uint8_t ip[4], uint8_t first, uint32_t lease_s)
{
    zero((uint8_t *)a, sizeof *a);
    cp(a->ip, ip, 4);
    a->mask[0] = a->mask[1] = a->mask[2] = 255; a->mask[3] = 0;
    a->first = first;
    a->lease_s = lease_s < 120 ? 120 : lease_s;
}

static int expired(const struct apn_lease *l, uint32_t now_s) { return l->state != APN_FREE && (int32_t)(now_s - l->until_s) >= 0; }
int apn_clients(const struct apn *a, uint32_t now_s)
{
    int n = 0;
    for (int i = 0; i < APN_LEASES; i++) if (a->l[i].state == APN_BOUND && !expired(&a->l[i], now_s)) n++;
    return n;
}
/* the lease index of an address of the pool, or -1 */
static int pool_index(const struct apn *a, const uint8_t ip[4])
{
    if (!same(ip, a->ip, 3) || ip[3] < a->first || ip[3] >= a->first + APN_LEASES) return -1;
    return ip[3] - a->first;
}
static int find_hw(struct apn *a, const uint8_t hw[6], uint32_t now_s)
{
    for (int i = 0; i < APN_LEASES; i++)
        if ((a->l[i].state == APN_OFFERED || a->l[i].state == APN_BOUND) && !expired(&a->l[i], now_s) && same(a->l[i].hw, hw, 6)) return i;
    return -1;
}
/* a lease for hw: its own, else a free one (an expired one is free; a client's last address is kept for
   it longest, by taking the free one that expired first) */
static int pick(struct apn *a, const uint8_t hw[6], uint32_t now_s)
{
    int i = find_hw(a, hw, now_s);
    if (i >= 0) return i;
    for (i = 0; i < APN_LEASES; i++) if (a->l[i].state != APN_FREE && expired(&a->l[i], now_s) && same(a->l[i].hw, hw, 6)) return i;
    int best = -1;
    for (i = 0; i < APN_LEASES; i++) {
        if (a->l[i].state == APN_FREE) return i;
        if (expired(&a->l[i], now_s) && (best < 0 || (int32_t)(a->l[i].until_s - a->l[best].until_s) < 0)) best = i;
    }
    return best;
}

/* the options of a client's message: its type, the address asked for, the server it chose */
struct opts { int type; int has_req, has_server; uint8_t req[4], server[4]; };
static int parse(const uint8_t *m, uint32_t n, struct opts *o)
{
    o->type = 0; o->has_req = o->has_server = 0;
    if (n < 240 || m[0] != 1 || m[1] != 1 || m[2] != 6) return -1;
    if (!(m[236] == 0x63 && m[237] == 0x82 && m[238] == 0x53 && m[239] == 0x63)) return -1;
    for (uint32_t k = 240; k < n && m[k] != 255; ) {
        if (m[k] == 0) { k++; continue; }
        if (k + 1 >= n) break;
        uint8_t t = m[k], l = m[k + 1];
        if (k + 2 + l > n) break;
        const uint8_t *v = m + k + 2;
        if (t == 53 && l >= 1) o->type = v[0];
        else if (t == 50 && l >= 4) { cp(o->req, v, 4); o->has_req = 1; }
        else if (t == 54 && l >= 4) { cp(o->server, v, 4); o->has_server = 1; }
        k += 2u + l;
    }
    return o->type ? 0 : -1;
}

/* an answer to m: the fixed part and the options every answer has */
static uint32_t answer(struct apn *a, const uint8_t *m, uint8_t *out, int type, const uint8_t yi[4], int with_lease)
{
    zero(out, 300);
    out[0] = 2; out[1] = 1; out[2] = 6;
    cp(out + 4, m + 4, 4);                                    /* xid */
    cp(out + 10, m + 10, 2);                                  /* flags */
    if (type == 5 && !with_lease) cp(out + 12, m + 12, 4);   /* INFORM's ACK: ciaddr back */
    if (yi) cp(out + 16, yi, 4);
    cp(out + 20, a->ip, 4);                                   /* siaddr */
    cp(out + 28, m + 28, 16);                                 /* chaddr */
    out[236] = 0x63; out[237] = 0x82; out[238] = 0x53; out[239] = 0x63;
    uint8_t *o = out + 240;
    *o++ = 53; *o++ = 1; *o++ = (uint8_t)type;
    *o++ = 54; *o++ = 4; cp(o, a->ip, 4); o += 4;
    if (type != 6) {
        if (with_lease) {
            *o++ = 51; *o++ = 4; p32(o, a->lease_s); o += 4;
            *o++ = 58; *o++ = 4; p32(o, a->lease_s / 2); o += 4;
            *o++ = 59; *o++ = 4; p32(o, a->lease_s / 8 * 7); o += 4;
        }
        *o++ = 1; *o++ = 4; cp(o, a->mask, 4); o += 4;
        *o++ = 3; *o++ = 4; cp(o, a->ip, 4); o += 4;          /* router: the Pi (nothing is routed on) */
        *o++ = 6; *o++ = 4; cp(o, a->ip, 4); o += 4;          /* DNS: the Pi */
    }
    *o++ = 255;
    uint32_t len = (uint32_t)(o - out);
    return len < 300 ? 300 : len;
}

uint32_t apn_dhcp(struct apn *a, uint32_t now_s, const uint8_t *m, uint32_t n, uint8_t *out, uint32_t max,
                  int *bcast, uint8_t to_ip[4], uint8_t to_hw[6])
{
    struct opts o;
    *bcast = 1; zero(to_ip, 4); zero(to_hw, 6);
    if (max < 320 || parse(m, n, &o)) return 0;
    const uint8_t *hw = m + 28, *ciaddr = m + 12;
    int want_bcast = (m[10] & 0x80) != 0;
    cp(to_hw, hw, 6);
    static const uint8_t z4[4];
    switch (o.type) {
    case 1: {                                                 /* DISCOVER */
        a->discovers++;
        int i = pick(a, hw, now_s);
        if (i < 0) return 0;                                  /* the pool is full: no offer */
        if (a->l[i].state != APN_BOUND || !same(a->l[i].hw, hw, 6)) {
            cp(a->l[i].hw, hw, 6); a->l[i].state = APN_OFFERED; a->l[i].until_s = now_s + 60;
        }
        uint8_t yi[4] = { a->ip[0], a->ip[1], a->ip[2], (uint8_t)(a->first + i) };
        a->offers++;
        *bcast = want_bcast; cp(to_ip, yi, 4);
        return answer(a, m, out, 2, yi, 1);
    }
    case 3: {                                                 /* REQUEST */
        if (o.has_server && !same(o.server, a->ip, 4)) {      /* it chose another server: our offer goes */
            int i = find_hw(a, hw, now_s);
            if (i >= 0 && a->l[i].state == APN_OFFERED) a->l[i].state = APN_FREE;
            return 0;
        }
        uint8_t req[4];
        if (o.has_req) cp(req, o.req, 4); else cp(req, ciaddr, 4);
        int i = pool_index(a, req);
        int mine = i >= 0 && (a->l[i].state == APN_OFFERED || a->l[i].state == APN_BOUND) && !expired(&a->l[i], now_s) && same(a->l[i].hw, hw, 6);
        int free_ = i >= 0 && (a->l[i].state == APN_FREE || expired(&a->l[i], now_s));
        if (i >= 0 && !mine && free_) {
            int j = find_hw(a, hw, now_s);                    /* a lease it held elsewhere in the pool goes */
            if (j >= 0) a->l[j].state = APN_FREE;
        }
        if (!mine && !free_) {                                /* another network's address, or someone else's */
            a->naks++;
            *bcast = 1;
            return answer(a, m, out, 6, 0, 0);
        }
        cp(a->l[i].hw, hw, 6); a->l[i].state = APN_BOUND; a->l[i].until_s = now_s + a->lease_s;
        a->acks++;
        if (!same(ciaddr, z4, 4)) { *bcast = 0; cp(to_ip, ciaddr, 4); }
        else { *bcast = want_bcast; cp(to_ip, req, 4); }
        return answer(a, m, out, 5, req, 1);
    }
    case 4: {                                                 /* DECLINE: the address is in use by someone */
        if (!o.has_req) return 0;
        int i = pool_index(a, o.req);
        if (i >= 0) { a->l[i].state = APN_DECLINED; a->l[i].until_s = now_s + a->lease_s; zero(a->l[i].hw, 6); }
        return 0;
    }
    case 7: {                                                 /* RELEASE */
        int i = pool_index(a, ciaddr);
        if (i >= 0 && same(a->l[i].hw, hw, 6)) { a->l[i].state = APN_FREE; a->releases++; }
        return 0;
    }
    case 8:                                                   /* INFORM: the options, no lease */
        if (same(ciaddr, z4, 4)) return 0;
        *bcast = 0; cp(to_ip, ciaddr, 4);
        a->acks++;
        return answer(a, m, out, 5, 0, 0);
    default:
        return 0;
    }
}

uint32_t apn_dns(struct apn *a, const uint8_t *q, uint32_t n, uint8_t *out, uint32_t max)
{
    if (n < 12 + 5 || n > 512) return 0;
    uint16_t flags = g16(q + 2);
    if ((flags & 0x8000) || ((flags >> 11) & 15) != 0 || g16(q + 4) != 1) return 0;   /* a standard query, one question */
    uint32_t p = 12, total = 0;
    for (;;) {
        if (p >= n) return 0;
        uint8_t l = q[p];
        if (l == 0) { p++; break; }
        if (l & 0xc0) return 0;                               /* no compression in a question */
        total += l + 1u;
        if (total > 255 || p + 1 + l > n) return 0;
        p += 1u + l;
    }
    if (p + 4 > n) return 0;
    uint16_t qtype = g16(q + p), qclass = g16(q + p + 2);
    uint32_t qend = p + 4;
    int a_rec = qclass == 1 && (qtype == 1 || qtype == 255);
    if (qend + (a_rec ? 16u : 0u) > max) return 0;
    a->dns_queries++;
    cp(out, q, qend);                                         /* id, and the question as it came */
    p16(out + 2, 0x8400u | (flags & 0x0100u));                /* an answer, authoritative, RD echoed, no error */
    p16(out + 4, 1); p16(out + 6, a_rec ? 1 : 0); p16(out + 8, 0); p16(out + 10, 0);
    uint32_t o = qend;
    if (a_rec) {
        out[o++] = 0xc0; out[o++] = 12;                       /* the name: the question's */
        p16(out + o, 1); p16(out + o + 2, 1); p32(out + o + 4, 10); p16(out + o + 8, 4); cp(out + o + 10, a->ip, 4);
        o += 14;
        a->dns_answers++;
    }
    return o;
}
