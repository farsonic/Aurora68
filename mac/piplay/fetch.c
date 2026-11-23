/* SPDX-License-Identifier: MPL-2.0
 *
 * mac/piplay/fetch.c - Aurora68 Play's small HTTP client for the music server's lists and covers, and
 * the UDP probe that finds the server.
 *
 * Backstory (22 November 2026, docs/08 item 99). The Library window (library.c) asks the music server
 * (tools/music-server) for artists, albums, tracks and covers while a track may be playing through the
 * radio's connection (radio.c), so it needs a connection of its own. Its answers are small (a page of a
 * list is a few KB, a cover 0.5 to 4 KB, a page of thumbnails up to 9 KB) and come whole: so this is the
 * simplest of clients, one request at a time, HTTP/1.0, the body read until the server closes, and the
 * event loop driving every step (fetch_poll). Nothing runs at interrupt time: the receive is
 * asynchronous without a completion routine and its parameter block is looked at from the loop, so a
 * list being fetched never stops the music or the menus.
 *
 * The driver is the one the radio opened (net_open in radio.c): Riptide's .IPP6 (IPv6 and IPv4, names by
 * the driver) when it is there, else MacTCP's .IPP with the resolver ('dnrp') for names. A name is looked
 * up once and kept until another server is chosen (fetch_forget_host).
 *
 * Finding the server: a UDP datagram "AURORA68-MUSIC?" to port 8070, sent three ways, because the stacks
 * differ in what they let through: to the limited broadcast 255.255.255.255 (Apple's MacTCP sends it), to
 * the network's own broadcast address (from ipctlGetAddr), and to the multicast group 239.255.68.70, which
 * Riptide's MacTCP interface sends where it refuses a broadcast (its UDP streams are not marked for
 * broadcast, as of 22 November 2026). The server listens to all three (aurora68_music_server.py) and
 * answers "AURORA68-MUSIC <tab> 1 <tab> NAME <tab> PORT". Always through .IPP, which Riptide also
 * offers: MacTCP's UDP calls take IPv4 addresses, and a probe needs no more.
 *
 * Every server that answers (Aurora68 Play 1.7, 23 November 2026, docs/08 item 120). The probe kept the first
 * answer and stopped; with two servers on a network the one heard first won, and the owner could not choose. Now
 * the answers are gathered for a second after the first one (four seconds when none comes), each server (the
 * address it answered from and its port) once, and library.c chooses: the one remembered, the only one, or the
 * owner's pick from Library > Servers. The read waits a second at most, and the stream is released only when no
 * read is out, so the probe no longer holds the event loop while a read times out.
 */
#include "toolbox.h"
#include "fetch.h"
#include "radio.h"
#define __MACTCP__ 1
#include "IPP6.h"

short mac_PBControl(void *pb), mac_PBControlAsync(void *pb);
long mac_dnr_call(void *code, long sel, long a, long b, long c, long d);
void mac_dnr_result(void);

struct fetch fetch;
int fetch_next_quick;
struct discovery disc;

enum { TCPCreate = 30, TCPActiveOpen = 32, TCPSend = 34, TCPRcv = 37, TCPAbort = 39, TCPRelease = 42 };
enum { UDPCreate = 20, UDPRead = 21, UDPBfrReturn = 22, UDPWrite = 23, UDPRelease = 24, ipctlGetAddr = 15 };
#define connectionClosing (-23005)
#define commandTimeout    (-23016)
#define cacheFault        (-23042)
#define TCP_BUF  8192u                 /* the connection's receive buffer (MacTCP wants 4 KB at least) */
#define RBUF     4096u                 /* one TCPRcv */
#define BODY_MAX 49152L                /* the longest answer: a page of 200 records, or 64 thumbnails */
#define PB_SIZE  NET_PB_SIZE
#define IO_RESULT(pb) (*(volatile short *)((pb) + 16))

static unsigned char *tcp_buf, *rbuf;
static char *buf;                      /* the whole answer, headers and body */
static long got;
static unsigned char *pb_ctl, *pb_rcv, *pb_end;   /* from the heap (1.8.1): one the driver keeps is left to it (net_leave) */
static unsigned long stream, t_state;
static int rcv_out;
static char req[640];                          /* a device's state with a station's title and artist, %XX-escaped */
static struct { unsigned short len; char *ptr; unsigned short end; } wds;

/* the server's address, kept */
static char c_host[64];
static unsigned short c_port;
static int c_have;                     /* 1: c_addr6 / c_addr4 hold the host's address */
static IP6Address c_addr6;
static unsigned long c_addr4;
static IP6iopb *rpb, *pb_cancel;       /* .IPP6's lookup, asynchronous, and its withdrawal */
static IP6Address r_addrs[4];
static int r_n, r_i;                   /* the addresses the name has, and the one being tried */
static struct { long rtnCode; char cname[255]; char pad; unsigned long addr[4]; } hi;   /* MacTCP's hostInfo */
static volatile long dnr_done;

static int has_colon(const char *s) { while (*s) if (*s++ == ':') return 1; return 0; }
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int same(const char *a, const char *b) { while (*a && lower(*a) == lower(*b)) { a++; b++; } return !*a && !*b; }
static void copy(char *d, const char *s, int max) { int i = 0; while (s[i] && i < max - 1) { d[i] = s[i]; i++; } d[i] = 0; }
static int dotted(const char *h, unsigned long *a)
{
	unsigned long v = 0, part;
	int n;
	for (n = 0; n < 4; n++) {
		if (*h < '0' || *h > '9') return 0;
		for (part = 0; *h >= '0' && *h <= '9'; h++) part = part * 10 + (unsigned long)(*h - '0');
		if (part > 255) return 0;
		v = v << 8 | part;
		if (n < 3 && *h++ != '.') return 0;
	}
	if (*h) return 0;
	*a = v;
	return 1;
}
static void logs(const char *a, const char *b)
{
	char t[128], *p = put_str(t, "library: ");
	for (; a && *a && p < t + 126; a++) *p++ = *a;
	for (; b && *b && p < t + 126; b++) *p++ = *b;
	*p = 0;
	pi_log(t);
}
static void pb_setup(unsigned char *pb, short cs)
{
	memset(pb, 0, PB_SIZE);
	*(short *)(pb + 24) = net_ipp();
	*(short *)(pb + 26) = cs;
	*(unsigned long *)(pb + 28) = stream;
}

void fetch_init(void)
{
	tcp_buf = mac_newptr(TCP_BUF);
	rbuf = mac_newptr(RBUF);
	buf = mac_newptr(BODY_MAX + 1);
	pb_ctl = mac_newptr(PB_SIZE);
	pb_rcv = mac_newptr(PB_SIZE);
	pb_end = mac_newptr(PB_SIZE);
	rpb = mac_newptr(PB_SIZE);
}
static int busy(const unsigned char *pb) { return pb && IO_RESULT(pb) > 0; }
/* the stream and what the driver may still hold of it left to the driver (1.8.1): released later if need be */
static void leave_stream(short release_cs)
{
	struct net_left *l = net_leave(net_ipp(), release_cs, stream, 0);
	net_leave_block(l, &pb_end, PB_SIZE, 1);
	if (busy(pb_ctl)) net_leave_block(l, &pb_ctl, PB_SIZE, 1);
	if (busy(pb_rcv)) net_leave_block(l, &pb_rcv, PB_SIZE, 1);
	net_leave_block(l, &tcp_buf, TCP_BUF, 0);
	net_leave_block(l, &rbuf, RBUF, 0);
}
/* The end of every fetch, and the call that froze the owner's SE twice on 23 November 2026: its TCPAbort, synchronous,
   never came back (Riptide had left it in the Device Manager's queue: its drvr.S, fixed the same day). Now each call is
   waited for five seconds at most (net_call, radio.c) with the radio's receive chain kept going, and what the driver
   does not give back is left to it with the stream; the next fetch takes new blocks and buffers (1.8.1) */
static void teardown(void)
{
	unsigned long t0;
	if (net_is_v6() && rpb && rpb->ioResult > 0) {   /* a lookup withdrawn */
		IP6iopb *c = net_fresh(&pb_cancel, "ip6ResolveCancel");
		if (c) {
			c->ioCRefNum = net_ipp(); c->csCode = ip6ResolveCancel; c->csParam.cancel.target = rpb;
			if (net_call(c, 5, "library: ip6ResolveCancel", "the lookup left to Riptide") == 1)
				net_leave_block(net_leave(0, 0, 0, 0), &pb_cancel, PB_SIZE, 1);
		}
		if (rpb->ioResult > 0) net_leave_block(net_leave(0, 0, 0, 0), &rpb, PB_SIZE, 1);
	}
	if (stream) {
		pb_setup(pb_end, TCPAbort);
		if (net_call(pb_end, 5, "library: TCPAbort", "the stream left to the driver") == 1) leave_stream(TCPRelease);
		else {
			for (t0 = LM_TICKS; (busy(pb_ctl) || (rcv_out && busy(pb_rcv))) && LM_TICKS - t0 < 180; ) radio_keep();
			pb_setup(pb_end, TCPRelease);
			if (net_call(pb_end, 5, "library: TCPRelease", "the stream left to the driver") == 1) leave_stream(0);
			else if (busy(pb_ctl) || busy(pb_rcv)) {   /* released, yet a call is still out: its block and buffer stay its */
				struct net_left *l = net_leave(0, 0, 0, 0);
				if (busy(pb_ctl)) net_leave_block(l, &pb_ctl, PB_SIZE, 1);
				if (busy(pb_rcv)) { net_leave_block(l, &pb_rcv, PB_SIZE, 1); net_leave_block(l, &rbuf, RBUF, 0); }
				logs("a call was still out after TCPRelease: its block left to the driver", 0);
			}
		}
		stream = 0;
	}
	rcv_out = 0;
}
static void fail(const char *why, long code)
{
	if (code) { char n[16]; *n = '-'; put_num(n + 1, (unsigned long)-code)[0] = 0; logs(why, n); }
	else logs(why, 0);
	teardown();
	fetch.error = why;
	fetch.st = FS_FAILED;
}
void fetch_cancel(void)
{
	if (fetch.st != FS_IDLE && fetch.st != FS_DONE && fetch.st != FS_FAILED) teardown();
	fetch.st = FS_IDLE;
}
void fetch_forget_host(void) { c_have = 0; c_host[0] = 0; }
long fetch_received(void) { return fetch.st == FS_RECV ? got : 0; }

static void open_start(void)
{
	if (!pb_ctl || !pb_rcv || !pb_end || !tcp_buf || !rbuf) { fail("There is not enough memory for the library.", 0); return; }
	if (!stream) {
		pb_setup(pb_end, TCPCreate);
		*(unsigned char **)(pb_end + 32) = tcp_buf;
		*(unsigned long *)(pb_end + 36) = TCP_BUF;
		short e = net_call(pb_end, 5, "library: TCPCreate", "the stream it makes left to the driver");
		if (e == 1) {                            /* the stream it makes, if it does, is released once it comes back */
			struct net_left *l = net_leave(net_ipp(), TCPRelease, 0, 1);
			net_leave_block(l, &pb_end, PB_SIZE, 1);
			net_leave_block(l, &tcp_buf, TCP_BUF, 0);
			fail("The network driver did not answer in 5 seconds.", 0);
			return;
		}
		if (e) { fail("MacTCP could not make a connection for the library.", e); return; }
		stream = *(unsigned long *)(pb_end + 28);
	}
	pb_setup(pb_ctl, TCPActiveOpen);
	pb_ctl[32] = r_i + 1 < r_n ? 6 : fetch.quick ? 5 : 15;   /* seconds to connect: 6 while another address remains, 5
	                                                               when the library has another of the server's (1.8) */
	pb_ctl[33] = 1; pb_ctl[34] = 0xC0;
	if (net_is_v6()) {
		c_addr6.port = c_port;
		memcpy(pb_ctl + 36, &c_addr6, sizeof c_addr6);
	} else {
		*(unsigned long *)(pb_ctl + 36) = c_addr4;
		*(unsigned short *)(pb_ctl + 40) = c_port;
	}
	mac_PBControlAsync(pb_ctl);
	fetch.st = FS_OPEN;
	t_state = LM_TICKS;
}
/* the server's address: kept from the last fetch, written as a number, or looked up */
static void resolve_start(void)
{
	unsigned long a4;
	if (c_have) { open_start(); return; }
	if (net_v4only && has_colon(c_host)) { fail("That is an IPv6 address, and Connect By says IPv4 Only.", 0); fetch.unreachable = 1; return; }
	if (net_is_v6()) {
		if (!net_fresh(&rpb, "the library's lookup")) { fail("There is not enough memory for the library.", 0); return; }
		rpb->ioCRefNum = net_ipp();
		rpb->csParam.resolve.hostName = c_host;
		rpb->csParam.resolve.port = c_port;
		if (dotted(c_host, &a4) || has_colon(c_host)) {
			rpb->csCode = ip6StrToAddr;
			short r = net_call(rpb, 5, "library: ip6StrToAddr", "its block left to Riptide");
			if (r == 1) { net_leave_block(net_leave(0, 0, 0, 0), &rpb, PB_SIZE, 1); fail("Riptide did not answer in 5 seconds.", 0); return; }
			if (r) { fail("That is not a server address Aurora68 Play understands.", r); return; }
			c_addr6 = rpb->csParam.resolve.addr;
			c_have = 1;
			r_n = r_i = 0;
			open_start();
			return;
		}
		rpb->csCode = ip6Resolve;
		rpb->csParam.resolve.flags = net_prefer == 4 ? kIP6OrderIPv4First : kIP6OrderIPv6First;   /* Connect By */
		rpb->csParam.resolve.results = r_addrs;
		rpb->csParam.resolve.maxResults = 4;
		mac_PBControlAsync(rpb);
	} else {
		if (has_colon(c_host)) { fail("An IPv6 address needs Riptide's .IPP6, which this Macintosh does not have.", 0); return; }
		if (dotted(c_host, &c_addr4)) { c_have = 1; open_start(); return; }
		void **dnr = net_dnr();
		if (!dnr) { fail("Aurora68 Play could not find MacTCP's name resolver: give the server's number address instead.", 0); return; }
		memset(&hi, 0, sizeof hi);
		dnr_done = 0;
		long r = (short)mac_dnr_call(*dnr, 3 /* STRTOADDR */, (long)c_host, (long)&hi, (long)mac_dnr_result, (long)&dnr_done);
		if (r && r != cacheFault) { fail("The server's name could not be found: is the network up?", r); return; }
		if (!r) dnr_done = 1;
	}
	fetch.st = FS_RESOLVE;
	t_state = LM_TICKS;
}
int fetch_start(const char *host, unsigned short port, const char *path)
{
	char *p;
	if (fetch.st != FS_IDLE && fetch.st != FS_DONE && fetch.st != FS_FAILED) return 0;
	memset(&fetch, 0, sizeof fetch);
	fetch.quick = fetch_next_quick;
	fetch_next_quick = 0;
	if (!tcp_buf || !rbuf || !buf) { fetch.st = FS_FAILED; fetch.error = "There is not enough memory for the library."; return 1; }
	if (!net_open()) { fetch.st = FS_FAILED; fetch.error = "MacTCP is not there: the library needs MacTCP or Riptide."; return 1; }
	if (!same(host, c_host) || port != c_port) { copy(c_host, host, sizeof c_host); c_port = port; c_have = 0; }
	p = put_str(req, "GET ");
	p = put_str(p, path);
	p = put_str(p, " HTTP/1.0\r\nHost: ");
	if (has_colon(host)) *p++ = '[';
	p = put_str(p, host);
	if (has_colon(host)) *p++ = ']';
	*p++ = ':'; p = put_num(p, port);
	/* X-Aurora: the server wants it on a device's requests (a web page elsewhere cannot send it unasked), and it
	   does no harm on the others */
	p = put_str(p, "\r\nUser-Agent: Aurora68-Play/1.8.1 (Macintosh)\r\nX-Aurora: 1\r\nConnection: close\r\n\r\n");
	wds.len = (unsigned short)(p - req); wds.ptr = req; wds.end = 0;
	got = 0;
	resolve_start();
	return 1;
}
static void rcv_issue(void)
{
	pb_setup(pb_rcv, TCPRcv);
	pb_rcv[32] = 20;                                  /* commandTimeoutValue: 20 s without a byte */
	*(unsigned char **)(pb_rcv + 36) = rbuf;
	*(unsigned short *)(pb_rcv + 40) = RBUF;
	rcv_out = 1;
	mac_PBControlAsync(pb_rcv);
}
/* the whole answer is in: the status line, the end of the headers, the body */
static void answer_in(void)
{
	long i;
	buf[got] = 0;
	if (got < 12 || buf[0] != 'H' || buf[1] != 'T' || buf[2] != 'T' || buf[3] != 'P') { fail("The music server's answer is not HTTP: is that the right port?", 0); return; }
	for (i = 0; i < got && buf[i] != ' '; i++) { }
	fetch.status = 0;
	for (i++; i < got && buf[i] >= '0' && buf[i] <= '9'; i++) fetch.status = fetch.status * 10 + (buf[i] - '0');
	for (i = 0; i + 3 < got; i++)
		if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') break;
	if (i + 3 >= got) { fail("The music server's answer was cut short.", 0); return; }
	fetch.body = buf + i + 4;
	fetch.len = got - (i + 4);
	fetch.st = FS_DONE;
}
void fetch_poll(void)
{
	unsigned long now = LM_TICKS;
	net_leave_poll();
	switch (fetch.st) {
	case FS_RESOLVE:
		if (net_is_v6()) {
			if (!rpb) { fail("There is not enough memory for the library.", 0); return; }
			if (rpb->ioResult > 0) { if (now - t_state > 30 * 60) { fail("The server's name could not be found: is the network up?", 0); fetch.unreachable = 1; } return; }
			if (rpb->ioResult || !rpb->csParam.resolve.count) { fail("The server's name could not be found: is the network up?", rpb->ioResult); fetch.unreachable = 1; return; }
			r_n = rpb->csParam.resolve.count; r_i = 0;
			if (net_v4only) {                         /* Connect By > IPv4 Only (1.8): the name's IPv4 addresses alone */
				int k, m = 0;
				for (k = 0; k < r_n; k++) if (IP6_IS_V4(&r_addrs[k])) r_addrs[m++] = r_addrs[k];
				r_n = m;
				if (!r_n) { fail("The server's name has no IPv4 address, and Connect By says IPv4 Only.", 0); fetch.unreachable = 1; return; }
			}
			c_addr6 = r_addrs[0];
			c_have = 1;
		} else {
			if (!dnr_done) { if (now - t_state > 30 * 60) fail("The server's name could not be found: is the network up?", 0); return; }
			if (hi.rtnCode) { fail("The server's name could not be found: is the network up?", hi.rtnCode); return; }
			c_addr4 = hi.addr[0];
			c_have = 1;
		}
		open_start();
		return;
	case FS_OPEN:
		if (IO_RESULT(pb_ctl) > 0) {               /* 1.8.1: an open the driver never answers (its own timeout is 15 s at most) */
			if (now - t_state > 30 * 60) { fail("The network driver did not answer the connection in 30 seconds.", 0); fetch.unreachable = 1; }
			return;
		}
		if (IO_RESULT(pb_ctl)) {
			if (r_i + 1 < r_n) {                      /* the name's next address: the other family, usually */
				char t[60];
				put_num(put_str(t, "no connection, error -"), (unsigned long)-IO_RESULT(pb_ctl))[0] = 0;
				logs(t, ": trying the server's next address");
				c_addr6 = r_addrs[++r_i];
				open_start();
				return;
			}
			c_have = 0;                               /* the next try looks the name up again */
			fail("The music server did not answer. Is it running, and is the address right?", IO_RESULT(pb_ctl));
			fetch.unreachable = 1;
			return;
		}
		fetch.ipv = net_is_v6() && !IP6_IS_V4((IP6Address *)(pb_ctl + 36)) ? 6 : 4;
		if (r_n > 1) { r_addrs[0] = r_addrs[r_i]; r_n = 1; r_i = 0; }   /* the one that answered is kept */
		pb_setup(pb_ctl, TCPSend);
		pb_ctl[32] = 15; pb_ctl[33] = 1; pb_ctl[34] = 0xC0; pb_ctl[35] = 1;
		*(void **)(pb_ctl + 38) = &wds;
		mac_PBControlAsync(pb_ctl);
		fetch.st = FS_SEND;
		t_state = now;
		return;
	case FS_SEND:
		if (IO_RESULT(pb_ctl) > 0) { if (now - t_state > 30 * 60) fail("The network driver did not take the request in 30 seconds.", 0); return; }
		if (IO_RESULT(pb_ctl)) { fail("The music server did not take the request.", IO_RESULT(pb_ctl)); return; }
		rcv_issue();
		fetch.st = FS_RECV;
		t_state = now;
		return;
	case FS_RECV: {
		int rounds;
		for (rounds = 0; rounds < 8 && fetch.st == FS_RECV; rounds++) {   /* what has come, a few receives a pass */
			short r = IO_RESULT(pb_rcv);
			if (r > 0) { if (now - t_state > 40 * 60) fail("The music server stopped answering.", 0); return; }
			rcv_out = 0;
			if (r == connectionClosing || (r && got)) {   /* all of it */
				teardown();
				answer_in();
				return;
			}
			if (r) { fail(r == commandTimeout ? "The music server stopped answering." : "The connection to the music server failed.", r); return; }
			long n = *(unsigned short *)(pb_rcv + 40);
			if (got + n > BODY_MAX) {
				if (fetch.cut_ok) {                   /* the first BODY_MAX bytes will do (a command's long track list) */
					n = BODY_MAX - got;
					memcpy(buf + got, rbuf, (unsigned long)n);
					got += n;
					teardown();
					answer_in();
					fetch.cut = 1;
					return;
				}
				fail("The music server's answer is too long for Aurora68 Play.", 0);
				fetch.too_long = 1;
				return;
			}
			memcpy(buf + got, rbuf, (unsigned long)n);
			got += n;
			t_state = now;
			rcv_issue();
		}
		return;
	}
	}
}

/* ---- finding the server: the UDP probe ------------------------------------------------------------ */
static short udp_ref;
static unsigned long udp_stream, d_until;
static unsigned char *udp_buf;
static unsigned char *pb_u, *pb_ur;              /* from the heap (1.8.1), as the TCP blocks */
static const char probe[] = A68_PROBE;
static struct { unsigned short len; const char *ptr; unsigned short end; } uwds;

static void u_setup(unsigned char *pb, short cs)
{
	memset(pb, 0, PB_SIZE);
	*(short *)(pb + 24) = udp_ref;
	*(short *)(pb + 26) = cs;
	*(unsigned long *)(pb + 28) = udp_stream;
}
/* a UDP call waited for (1.8.1): its result, or 1 with its block left to the driver and a new one in pb_u */
static short u_call(const char *what)
{
	short e = net_call(pb_u, 5, what, "its block left to the driver");
	if (e == 1) net_leave_block(net_leave(0, 0, 0, 0), &pb_u, PB_SIZE, 1);
	return e;
}
static void u_send(unsigned long to)
{
	if (!pb_u) return;
	u_setup(pb_u, UDPWrite);
	*(unsigned long *)(pb_u + 34) = to;             /* remoteHost */
	*(unsigned short *)(pb_u + 38) = A68_DISCOVERY_PORT;   /* remotePort */
	*(void **)(pb_u + 40) = &uwds;                  /* wdsPtr */
	pb_u[44] = 1;                                   /* checkSum */
	short e = u_call("library: UDPWrite");
	char t[60], *p = put_str(t, "probe sent to ");
	int i;
	for (i = 24; i >= 0; i -= 8) { p = put_num(p, to >> i & 255); if (i) *p++ = '.'; }
	if (e) { p = put_str(p, ": refused, error -"); p = put_num(p, (unsigned long)-e); }
	*p = 0;
	logs(t, 0);
}
static int u_out;                                  /* a UDPRead is out */
static void u_read(void)
{
	u_setup(pb_ur, UDPRead);
	*(unsigned short *)(pb_ur + 32) = 1;            /* timeOut: seconds */
	u_out = 1;
	mac_PBControlAsync(pb_ur);
}
int discover_start(void)
{
	ParamBlock pb;
	discover_stop();
	memset(&disc, 0, sizeof disc);
	if (!udp_buf && !(udp_buf = mac_newptr(4096))) return 0;
	if (!pb_u) pb_u = mac_newptr(PB_SIZE);
	if (!net_fresh(&pb_ur, "the probe's read") || !pb_u) return 0;
	if (!udp_ref) {
		memset(&pb, 0, sizeof pb);
		pb.ioNamePtr = (const unsigned char *)"\x04.IPP";
		if (mac_PBOpen(&pb)) { logs("no .IPP for the probe", 0); return 0; }
		udp_ref = pb.ioRefNum;
	}
	u_setup(pb_u, UDPCreate);
	*(unsigned char **)(pb_u + 32) = udp_buf;
	*(unsigned long *)(pb_u + 36) = 4096;
	short e = net_call(pb_u, 5, "library: UDPCreate", "the stream it makes left to the driver");
	if (e == 1) {                                   /* released once it comes back */
		struct net_left *l = net_leave(udp_ref, UDPRelease, 0, 1);
		net_leave_block(l, &pb_u, PB_SIZE, 1);
		net_leave_block(l, &udp_buf, 4096, 0);
		return 0;
	}
	if (e) { logs("UDPCreate failed", 0); return 0; }
	udp_stream = *(unsigned long *)(pb_u + 28);
	uwds.len = sizeof probe - 1; uwds.ptr = probe; uwds.end = 0;
	u_send(0xFFFFFFFFul);
	if (pb_u) {                                     /* the network's own broadcast address */
		memset(pb_u, 0, PB_SIZE);
		*(short *)(pb_u + 24) = udp_ref; *(short *)(pb_u + 26) = ipctlGetAddr;
	}
	if (pb_u && !u_call("library: ipctlGetAddr")) {
		unsigned long me = *(unsigned long *)(pb_u + 28), mask = *(unsigned long *)(pb_u + 32);
		if (me && mask && mask != 0xFFFFFFFFul) u_send(me | ~mask);
	}
	u_send(A68_DISCOVERY_GROUP);                    /* 239.255.68.70 */
	u_read();
	d_until = LM_TICKS + 4 * 60;
	disc.st = D_WAIT;
	return 1;
}
/* an answer kept, unless that server (the address it came from, its port) answered already */
static void d_keep(const struct a68_disc *a, unsigned long from)
{
	struct disc_found *f;
	char t[120], *p;
	int i;
	if (disc.n >= D_MAX) return;
	f = &disc.found[disc.n];
	*a68_put_ipv4(f->host, f->host + sizeof f->host - 1, from) = 0;
	for (i = 0; i < disc.n; i++)
		if (disc.found[i].d.port == a->port && same(disc.found[i].host, f->host)) return;
	f->d = *a;
	disc.n++;
	p = put_str(t, "found the music server \"");
	p = put_str(p, a->name); p = put_str(p, "\" at "); p = put_str(p, f->host); *p++ = ':'; p = put_num(p, a->port);
	*p = 0;
	logs(t, 0);
	/* the addresses it names (1.8): the owner's SE took IPv4 where IPv6 was wanted, and the log did not say why */
	p = put_str(t, "  it names IPv4 "); p = put_str(p, a->v4[0] ? a->v4 : "(none)");
	p = put_str(p, ", IPv6 "); p = put_str(p, a->v6[0] ? a->v6 : "(none: the server has no usable IPv6 address)");
	*p = 0;
	logs(t, 0);
	if (disc.n == 1 && (long)(d_until - (LM_TICKS + 60)) > 0) d_until = LM_TICKS + 60;   /* the others: a second more */
}
void discover_poll(void)
{
	if (disc.st != D_WAIT) return;
	short r = IO_RESULT(pb_ur);
	if (r > 0) return;
	u_out = 0;
	if (!r) {
		unsigned char *d = *(unsigned char **)(pb_ur + 40);
		unsigned short n = *(unsigned short *)(pb_ur + 44);
		unsigned long from = *(unsigned long *)(pb_ur + 34);
		struct a68_disc a;
		if (a68_disc_parse(d, n, &a)) d_keep(&a, from);   /* play/core/address.c: the name, the port, the addresses */
		if (pb_u) {
			u_setup(pb_u, UDPBfrReturn);
			*(unsigned char **)(pb_u + 40) = d;
			u_call("library: UDPBfrReturn");
		}
	}
	if ((long)(LM_TICKS - d_until) >= 0) {
		int n = disc.n;
		if (!n) logs("no music server answered the probe", 0);
		else if (n > 1) { char t[40]; put_str(put_num(t, (unsigned long)n), " music servers answered")[0] = 0; logs(t, 0); }
		discover_stop();
		disc.st = n ? D_FOUND : D_NONE;
		return;
	}
	u_read();
}
void discover_stop(void)
{
	unsigned long t0;
	if (!udp_stream) return;
	for (t0 = LM_TICKS; u_out && IO_RESULT(pb_ur) > 0 && LM_TICKS - t0 < 90; ) radio_keep();   /* a read out ends within its second */
	u_out = 0;
	if (!pb_u) pb_u = mac_newptr(PB_SIZE);
	if (pb_u) {
		u_setup(pb_u, UDPRelease);
		if (net_call(pb_u, 5, "library: UDPRelease", "the stream left to the driver") == 1) {
			struct net_left *l = net_leave(0, 0, 0, 0);
			net_leave_block(l, &pb_u, PB_SIZE, 1);
			if (IO_RESULT(pb_ur) > 0) net_leave_block(l, &pb_ur, PB_SIZE, 1);
			net_leave_block(l, &udp_buf, 4096, 0);
		}
	}
	if (pb_ur && IO_RESULT(pb_ur) > 0) {            /* a read still out: its block and the buffer it hands out stay the driver's */
		struct net_left *l = net_leave(0, 0, 0, 0);
		net_leave_block(l, &pb_ur, PB_SIZE, 1);
		net_leave_block(l, &udp_buf, 4096, 0);
	}
	udp_stream = 0;
	if (disc.st == D_WAIT) disc.st = D_IDLE;
}
