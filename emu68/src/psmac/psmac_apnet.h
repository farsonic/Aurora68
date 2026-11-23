/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_apnet.h - the access point's own network services: a DHCP server and a DNS answerer (23 November
 * 2026, docs/08 item 117). Aurora68 Setup and Recovery can make the Pi's WiFi an access point (a phone joins
 * it to reach the recovery page with no other network about); the phone then needs an address, and the
 * page is easiest to find when every name it looks up leads to the Pi, as a hotel's captive portal does.
 *
 * Plain C99 with no kernel calls: psmac_ip.c feeds it the UDP payloads that arrive on ports 67 and 53 and
 * sends what it answers; tools/apnet_test.c runs the same code on the host.
 */
#ifndef PSMAC_APNET_H
#define PSMAC_APNET_H
#include <stdint.h>

#define APN_LEASES 16                    /* 192.168.4.10 to .25: a phone, a laptop, a tablet, with room */
struct apn_lease { uint8_t hw[6]; uint8_t state; uint8_t pad; uint32_t until_s; };
enum { APN_FREE, APN_OFFERED, APN_BOUND, APN_DECLINED };
struct apn {
    uint8_t ip[4], mask[4];              /* the server's own: 192.168.4.1/24 */
    uint8_t first;                       /* the pool: ip[0..2].first ... first + APN_LEASES - 1 */
    uint32_t lease_s;
    struct apn_lease l[APN_LEASES];
    uint32_t discovers, offers, acks, naks, releases, dns_queries, dns_answers;
};
void apn_init(struct apn *a, const uint8_t ip[4], uint8_t first, uint32_t lease_s);

/* A DHCP message from a client (the UDP payload of a datagram to port 67, n bytes) at time now_s (seconds).
   The answer goes into out (max bytes, 300 at least): its length, or 0 for none. Where it goes: *bcast 1
   is 255.255.255.255 at ff:ff:ff:ff:ff:ff, else to_ip at to_hw (RFC 2131 4.1). */
uint32_t apn_dhcp(struct apn *a, uint32_t now_s, const uint8_t *m, uint32_t n, uint8_t *out, uint32_t max,
                  int *bcast, uint8_t to_ip[4], uint8_t to_hw[6]);

/* A DNS query (the UDP payload of a datagram to port 53): every name's A record is the server's own
   address (ten seconds, so nothing remembers it long after the access point is gone); other types and
   classes are answered with no records, so a phone asking for AAAA falls back to IPv4. The answer's
   length, or 0 for none (not a query, more than one question, a malformed name). */
uint32_t apn_dns(struct apn *a, const uint8_t *q, uint32_t n, uint8_t *out, uint32_t max);

/* clients holding a lease now */
int apn_clients(const struct apn *a, uint32_t now_s);

#endif
