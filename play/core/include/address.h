/* SPDX-License-Identifier: MPL-2.0
 *
 * play/core/include/address.h - where the music server and the stations are: addresses as the owner types
 * them, URLs with IPv6 in brackets, the order a name's addresses are tried in (IPv6 first or IPv4 first, the
 * owner's choice), and finding the server on the network (the UDP probe's answer).
 *
 * Both Plays let the owner choose the family tried first (Connect By on the classic Play, Settings > Connect
 * by on the macOS one): on their network a name has both, and which one reaches the server depends on the
 * room the computer is in. So neither Play leaves the choice to a resolver: the addresses are put in order
 * here and tried one after the other as numbers. Sockets, MacTCP and Riptide's .IPP6 stay with each Play;
 * this is only what is decided about the text. Written on 22 November 2026 (docs/08 item 107), from the
 * classic Play's radio.c, fetch.c and library.c and the macOS Play's Address.swift.
 */
#ifndef A68_ADDRESS_H
#define A68_ADDRESS_H

#define A68_DEFAULT_PORT   8000          /* the music server's own (MUSIC_PORT): an address typed without one */
#define A68_DISCOVERY_PORT 8070          /* UDP: "AURORA68-MUSIC?" is answered with the server's name and port */
#define A68_DISCOVERY_GROUP 0xEFFF4446ul /* 239.255.68.70: Riptide's MacTCP sends to it where it refuses a broadcast */
#define A68_DISCOVERY_GROUP_TEXT "239.255.68.70"
#define A68_PROBE "AURORA68-MUSIC?"
#define A68_HOST_MAX 256                 /* a name (RFC 1035: 253) or an IPv6 address with its zone */

enum { A68_NAME = 0, A68_IPV4 = 4, A68_IPV6 = 6 };

/* what a host is: A68_IPV4 ("192.168.1.20"), A68_IPV6 ("fd00::20", "[fd00::20]", "fe80::1%en0") or A68_NAME */
int a68_family(const char *host);
int a68_is_ipv4(const char *host);
int a68_is_ipv6(const char *host);
/* a dotted address as a number (MacTCP's form); 0 if host is not one */
int a68_ipv4_value(const char *host, unsigned long *addr);
char *a68_put_ipv4(char *p, char *end, unsigned long addr);

/* the host as a URL writes it: an IPv6 address in brackets, a zone's '%' as "%25" (RFC 6874) */
char *a68_put_url_host(char *p, char *end, const char *host);
/* "http://HOST:PORT" and path (which starts with '/'; 0 for none) into out; its length, 0 if it did not fit */
long a68_url(char *out, long max, const char *host, unsigned port, const char *path);

/* an address as the owner types it: "192.168.1.20:8423", "[fd00::20]:8423", "fd00::20" (no port can follow a
   bare IPv6 address), "nas.local", "http://nas.local:8423/live" (the scheme and the path are left out); spaces
   around it are ignored. The host into host (hmax bytes, without brackets), the port into *port (default_port
   if none); 1 if it is an address, 0 if not (nothing, a space inside, a bracket not closed, a port that is no
   port) */
int a68_parse_address(const char *typed, unsigned default_port, char *host, long hmax, unsigned *port);

/* a station's address, "http://host[:port][/path]" (the "http://" may be left out; "icy://" is taken as
   http): host (without brackets), port (80 if none), path ("/" if none), *bracketed set for an IPv6 address
   in brackets. 0 if it is good, else why not in words for the window ("plain http:// only" for https) */
const char *a68_parse_url(const char *url, char *host, long hmax, unsigned *port, char *path, long pmax, int *bracketed);

/* hosts in the order to try them: the preferred family's (A68_IPV6 or A68_IPV4) first, then the other's,
   then names; the same text twice is tried once. Indices into hosts[] into order[]; how many */
int a68_order(const char *const *hosts, int n, int prefer, int *order);

/* the probe's answer: "AURORA68-MUSIC <tab> 1 <tab> NAME <tab> PORT <tab> IPV4 <tab> IPV6", Mac Roman (it is
   written for the classic Play as well; the macOS Play turns the name into a String itself) */
struct a68_disc {
	char name[64];                   /* bytes as the server sent them */
	unsigned port;
	char v4[16], v6[64];             /* the server's own addresses: either may be empty (checked: a v4 is IPv4) */
};
/* 1 if d (n bytes) is an answer */
int a68_disc_parse(const unsigned char *d, long n, struct a68_disc *r);
/* the hosts to try, best first, into out[] (pointers into r and sender; 3 at most): the preferred family
   first; for IPv4 the address the answer came from (sender: known to reach this computer, where the server's
   own idea of its address may be its container's) before the one the server names. can_v6 0: this computer
   cannot use IPv6 (MacTCP without Riptide's .IPP6): the IPv6 address is left out. How many */
int a68_disc_hosts(const struct a68_disc *r, const char *sender, int prefer, int can_v6, const char **out);
#endif
