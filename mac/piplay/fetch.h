/* SPDX-License-Identifier: MPL-2.0
 *
 * mac/piplay/fetch.h - Aurora68 Play's second network client (fetch.c): one small HTTP request at a
 * time to the music server for the Library window's lists and covers, beside the radio's stream; and
 * the UDP probe that finds a music server on the network. Written on 22 November 2026 (docs/08 item 99).
 */
#ifndef PIPLAY_FETCH_H
#define PIPLAY_FETCH_H
#include "address.h"

enum { FS_IDLE, FS_RESOLVE, FS_OPEN, FS_SEND, FS_RECV, FS_DONE, FS_FAILED };

struct fetch {
	int st;                          /* F_*: FS_DONE and FS_FAILED stay until the next fetch_start */
	int status;                      /* the HTTP status, 0 before the answer */
	char *body;                      /* FS_DONE: the answer's body, len bytes (in buf) */
	long len;
	const char *error;               /* FS_FAILED: why, in words for the user */
	int ipv;                         /* 4 or 6: the family of the last connection */
	int cut_ok;                      /* set after fetch_start: an answer longer than the buffer is cut, not failed */
	int cut;                         /* FS_DONE: it was cut */
};
extern struct fetch fetch;

void fetch_init(void);
/* a GET of path on host:port (host: a name, a dotted address, or an IPv6 address without brackets);
   0 if one is already under way. The answer is in fetch.body once fetch.st is FS_DONE. */
int fetch_start(const char *host, unsigned short port, const char *path);
void fetch_poll(void);                 /* from the event loop */
void fetch_cancel(void);
void fetch_forget_host(void);          /* the next fetch looks the name up again (another server chosen) */
long fetch_received(void);             /* bytes of the answer received so far (FS_RECV), else 0 */

/* the network probe (UDP, port 8070): "AURORA68-MUSIC?" broadcast and sent to the group 239.255.68.70;
   the first answer (d, read by play/core/address.c) gives the server's name and TCP port and the addresses
   it has (either may be empty); host is the address it answered from. a68_disc_hosts puts them in the order
   the Connect By preference names */
enum { D_IDLE, D_WAIT, D_FOUND, D_NONE };
struct discovery { int st; char host[40]; struct a68_disc d; };
extern struct discovery disc;
int discover_start(void);              /* 0 if MacTCP's UDP cannot be had */
void discover_poll(void);
void discover_stop(void);

/* from radio.c: the network driver it opened (.IPP6 or .IPP), and MacTCP's resolver */
int net_open(void);
short net_ipp(void);
int net_is_v6(void);
void **net_dnr(void);
#endif
