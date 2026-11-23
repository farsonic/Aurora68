/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_ip.h - the kernel's own small IPv4 stack (psmac_ip.c) and the remote-control service on
 * top of it (psmac_remote.c, docs/13).
 *
 * Three kinds of callers:
 *   - the network drivers (alice_net.c: the WiFi and GENET; psmac_usbnet.c: the Pi 3's wired
 *     chips; alice_psmac.c: the testbed's file bridge and simulated radio) show the stack every
 *     frame they receive, before the Mac's receive ring, and send its frames when the Mac's
 *     transmit ring is empty. Unit 0 is the WiFi, unit 1 the wired port, as on the network board;
 *   - the service loop (alice_psmac.c) polls it once a turn;
 *   - the remote service is its one application: TCP connections on its ports.
 * Everything runs on core 3, the service core (the WiFi's coroutine included), so nothing here
 * takes a lock.
 */
#ifndef PSMAC_IP_H
#define PSMAC_IP_H
#include <stdint.h>

/* ---- for the drivers ---- */
#define IPK_TO_MAC   0      /* the frame goes to the Mac as before (broadcasts: the kernel has had its look) */
#define IPK_TAKEN    1      /* the kernel's alone: not for the Mac */
#define IPK_DROP     2      /* nobody's: seen only because the chip is promiscuous for the kernel */
extern int psmac_ip_active;                     /* 0 unless remote=on and a token: then every hook below is one test */
int  psmac_ip_rx(int unit, const volatile uint8_t *f, uint32_t len, const volatile uint8_t *mac_addr, int mac_promisc);
uint32_t psmac_ip_tx_take(int unit, uint8_t *buf, uint32_t max);   /* a frame of the kernel's to send on unit, or 0 */
int  psmac_ip_tx_pending(int unit);             /* frames of the kernel's wait for unit */
void psmac_ip_mac_tx(int unit, const volatile uint8_t *f, uint32_t len);   /* a frame the Mac sends: its address is learnt */
int  psmac_ip_promisc(int unit);                /* the wired chip should pass every unicast frame (one address register) */
int  psmac_ip_hwaddr(int unit, uint8_t out[6]); /* the kernel's own MAC address on unit, 0 if it shares the Mac's */

/* ---- for the service loop ---- */
void psmac_ip_init(void);
void psmac_ip_init_recovery(const char *name);    /* Aurora68 Setup and Recovery: the WiFi's own address, link-local without DHCP */
void psmac_ip_ap(const uint8_t ip[4]);            /* Setup and Recovery's access point: ip/24 on the WiFi, DHCP and DNS for its guests */
void psmac_ip_ap_off(void);                       /* ... and back, when it did not start */
int  psmac_ip_ap_clients(void);                   /* the access point's guests with a lease */
void psmac_ip_poll(void);
int  psmac_ip_busy(void);                        /* frames or connections to see to: poll every turn */

/* ---- TCP, for the one application ---- */
#define IPK_CONNS 4
/* the application: psmac_remote.c */
int      remote_tcp_accept(int c, uint16_t port, const uint8_t peer[4]);   /* 1: take it; 0: refuse (RST) */
void     remote_tcp_recv(int c, const uint8_t *data, uint32_t len);     /* at most remote_tcp_room(c) bytes */
uint32_t remote_tcp_room(int c);                                         /* bytes it takes now: the receive window */
uint32_t remote_tcp_produce(int c, uint8_t *buf, uint32_t room);         /* more bytes to send, 0 if none yet */
void     remote_tcp_closed(int c);                                       /* gone: the slot may be reused */
/* the stack */
void     ipk_tcp_close(int c);                  /* the application is done: FIN once all it produced is sent */
void     ipk_tcp_abort(int c);                  /* RST, now */

/* ---- what the state endpoint shows ---- */
struct ipk_info {
    int unit;                                   /* the interface in use, -1 none */
    int shared;                                 /* WiFi: the Mac's MAC address; 2: the Mac's IP address too */
    uint8_t hw[6], ip[4], mask[4], gw[4], mac_ip[2][4];
    uint32_t lease_s, rx, tx, rx_dropped, tx_dropped, conns;
    const char *phase;                          /* "no link", "DHCP", "bound", "static", "shared" */
};
void psmac_ip_info(struct ipk_info *out);
void psmac_ip_report(void);                     /* the heartbeat's [REMOTE] line, when something changed */
const char *psmac_ip_name(void);                /* the mDNS host name, without .local */

#endif
