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
struct discovery disc;

enum { TCPCreate = 30, TCPActiveOpen = 32, TCPSend = 34, TCPRcv = 37, TCPAbort = 39, TCPRelease = 42 };
enum { UDPCreate = 20, UDPRead = 21, UDPBfrReturn = 22, UDPWrite = 23, UDPRelease = 24, ipctlGetAddr = 15 };
#define connectionClosing (-23005)
#define commandTimeout    (-23016)
#define cacheFault        (-23042)
#define TCP_BUF  8192u                 /* the connection's receive buffer (MacTCP wants 4 KB at least) */
#define RBUF     4096u                 /* one TCPRcv */
#define BODY_MAX 49152L                /* the longest answer: a page of 200 records, or 64 thumbnails */
#define PB_SIZE  132
#define IO_RESULT(pb) (*(volatile short *)((pb) + 16))

static unsigned char *tcp_buf, *rbuf;
static char *buf;                      /* the whole answer, headers and body */
static long got;
static unsigned char pb_ctl[PB_SIZE], pb_rcv[PB_SIZE], pb_end[PB_SIZE];
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
static IP6iopb rpb;                    /* .IPP6's lookup, asynchronous */
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
}
static void teardown(void)
{
	unsigned long t0;
	if (net_is_v6() && fetch.st == FS_RESOLVE && rpb.ioResult > 0) {   /* a lookup withdrawn */
		IP6iopb c;
		memset(&c, 0, sizeof c);
		c.ioCRefNum = net_ipp(); c.csCode = ip6ResolveCancel; c.csParam.cancel.target = &rpb;
		mac_PBControl(&c);
	}
	if (stream) {
		pb_setup(pb_end, TCPAbort);
		mac_PBControl(pb_end);
		for (t0 = LM_TICKS; (IO_RESULT(pb_ctl) > 0 || (rcv_out && IO_RESULT(pb_rcv) > 0)) && LM_TICKS - t0 < 180; ) { }
		pb_setup(pb_end, TCPRelease);
		mac_PBControl(pb_end);
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
	if (!stream) {
		pb_setup(pb_end, TCPCreate);
		*(unsigned char **)(pb_end + 32) = tcp_buf;
		*(unsigned long *)(pb_end + 36) = TCP_BUF;
		short e = mac_PBControl(pb_end);
		if (e) { fail("MacTCP could not make a connection for the library.", e); return; }
		stream = *(unsigned long *)(pb_end + 28);
	}
	pb_setup(pb_ctl, TCPActiveOpen);
	pb_ctl[32] = r_i + 1 < r_n ? 6 : 15;     /* seconds to connect: 6 while another address remains */
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
	if (net_is_v6()) {
		memset(&rpb, 0, sizeof rpb);
		rpb.ioCRefNum = net_ipp();
		rpb.csParam.resolve.hostName = c_host;
		rpb.csParam.resolve.port = c_port;
		if (dotted(c_host, &a4) || has_colon(c_host)) {
			rpb.csCode = ip6StrToAddr;
			short r = mac_PBControl(&rpb);
			if (r) { fail("That is not a server address Aurora68 Play understands.", r); return; }
			c_addr6 = rpb.csParam.resolve.addr;
			c_have = 1;
			r_n = r_i = 0;
			open_start();
			return;
		}
		rpb.csCode = ip6Resolve;
		rpb.csParam.resolve.flags = net_prefer == 4 ? kIP6OrderIPv4First : kIP6OrderIPv6First;   /* Connect By */
		rpb.csParam.resolve.results = r_addrs;
		rpb.csParam.resolve.maxResults = 4;
		mac_PBControlAsync(&rpb);
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
	p = put_str(p, "\r\nUser-Agent: Aurora68-Play/1.3 (Macintosh)\r\nX-Aurora: 1\r\nConnection: close\r\n\r\n");
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
	switch (fetch.st) {
	case FS_RESOLVE:
		if (net_is_v6()) {
			if (rpb.ioResult > 0) { if (now - t_state > 30 * 60) fail("The server's name could not be found: is the network up?", 0); return; }
			if (rpb.ioResult || !rpb.csParam.resolve.count) { fail("The server's name could not be found: is the network up?", rpb.ioResult); return; }
			r_n = rpb.csParam.resolve.count; r_i = 0;
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
		if (IO_RESULT(pb_ctl) > 0) return;
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
		if (IO_RESULT(pb_ctl) > 0) return;
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
static unsigned char pb_u[PB_SIZE], pb_ur[PB_SIZE];
static const char probe[] = "AURORA68-MUSIC?";
static struct { unsigned short len; const char *ptr; unsigned short end; } uwds;

static void u_setup(unsigned char *pb, short cs)
{
	memset(pb, 0, PB_SIZE);
	*(short *)(pb + 24) = udp_ref;
	*(short *)(pb + 26) = cs;
	*(unsigned long *)(pb + 28) = udp_stream;
}
static void u_send(unsigned long to)
{
	u_setup(pb_u, UDPWrite);
	*(unsigned long *)(pb_u + 34) = to;             /* remoteHost */
	*(unsigned short *)(pb_u + 38) = 8070;          /* remotePort */
	*(void **)(pb_u + 40) = &uwds;                  /* wdsPtr */
	pb_u[44] = 1;                                   /* checkSum */
	short e = mac_PBControl(pb_u);
	char t[60], *p = put_str(t, "probe sent to ");
	int i;
	for (i = 24; i >= 0; i -= 8) { p = put_num(p, to >> i & 255); if (i) *p++ = '.'; }
	if (e) { p = put_str(p, ": refused, error -"); p = put_num(p, (unsigned long)-e); }
	*p = 0;
	logs(t, 0);
}
static void u_read(void)
{
	u_setup(pb_ur, UDPRead);
	*(unsigned short *)(pb_ur + 32) = 2;            /* timeOut: seconds */
	mac_PBControlAsync(pb_ur);
}
int discover_start(void)
{
	ParamBlock pb;
	discover_stop();
	memset(&disc, 0, sizeof disc);
	if (!udp_buf && !(udp_buf = mac_newptr(4096))) return 0;
	if (!udp_ref) {
		memset(&pb, 0, sizeof pb);
		pb.ioNamePtr = (const unsigned char *)"\x04.IPP";
		if (mac_PBOpen(&pb)) { logs("no .IPP for the probe", 0); return 0; }
		udp_ref = pb.ioRefNum;
	}
	u_setup(pb_u, UDPCreate);
	*(unsigned char **)(pb_u + 32) = udp_buf;
	*(unsigned long *)(pb_u + 36) = 4096;
	short e = mac_PBControl(pb_u);
	if (e) { logs("UDPCreate failed", 0); return 0; }
	udp_stream = *(unsigned long *)(pb_u + 28);
	uwds.len = sizeof probe - 1; uwds.ptr = probe; uwds.end = 0;
	u_send(0xFFFFFFFFul);
	memset(pb_u, 0, PB_SIZE);                       /* the network's own broadcast address */
	*(short *)(pb_u + 24) = udp_ref; *(short *)(pb_u + 26) = ipctlGetAddr;
	if (!mac_PBControl(pb_u)) {
		unsigned long me = *(unsigned long *)(pb_u + 28), mask = *(unsigned long *)(pb_u + 32);
		if (me && mask && mask != 0xFFFFFFFFul) u_send(me | ~mask);
	}
	u_send(0xEFFF4446ul);                           /* 239.255.68.70 */
	u_read();
	d_until = LM_TICKS + 4 * 60;
	disc.st = D_WAIT;
	return 1;
}
void discover_poll(void)
{
	if (disc.st != D_WAIT) return;
	short r = IO_RESULT(pb_ur);
	if (r > 0) return;
	if (!r) {
		unsigned char *d = *(unsigned char **)(pb_ur + 40);
		unsigned short n = *(unsigned short *)(pb_ur + 44);
		unsigned long from = *(unsigned long *)(pb_ur + 34);
		static const char head[] = "AURORA68-MUSIC\t1\t";
		int ok = n > sizeof head && !memcmp(d, head, sizeof head - 1);
		if (ok) {
			unsigned short i = sizeof head - 1, k = 0, port = 0;
			while (i < n && d[i] != '\t' && k < sizeof disc.name - 1) disc.name[k++] = (char)d[i++];
			disc.name[k] = 0;
			while (i < n && d[i] != '\t') i++;
			for (i++; i < n && d[i] >= '0' && d[i] <= '9'; i++) port = (unsigned short)(port * 10 + (d[i] - '0'));
			for (k = 0, i++; i < n && d[i] != '\t' && d[i] != '\r' && k < sizeof disc.v4 - 1; ) disc.v4[k++] = (char)d[i++];
			disc.v4[k] = 0;
			for (k = 0, i++; i < n && d[i] != '\t' && d[i] != '\r' && k < sizeof disc.v6 - 1; ) disc.v6[k++] = (char)d[i++];
			disc.v6[k] = 0;
			char *p = disc.host;
			int j;
			for (j = 24; j >= 0; j -= 8) { p = put_num(p, from >> j & 255); if (j) *p++ = '.'; }
			*p = 0;
			disc.port = port;
			ok = port != 0;
		}
		u_setup(pb_u, UDPBfrReturn);
		*(unsigned char **)(pb_u + 40) = d;
		mac_PBControl(pb_u);
		if (ok) {
			logs("found the music server at ", disc.host);
			discover_stop();
			disc.st = D_FOUND;
			return;
		}
	}
	if ((long)(LM_TICKS - d_until) >= 0) { logs("no music server answered the probe", 0); discover_stop(); disc.st = D_NONE; return; }
	u_read();
}
void discover_stop(void)
{
	unsigned long t0;
	if (!udp_stream) return;
	for (t0 = LM_TICKS; IO_RESULT(pb_ur) > 0 && LM_TICKS - t0 < 240; ) { }   /* a read out ends with its timeout */
	u_setup(pb_u, UDPRelease);
	mac_PBControl(pb_u);
	udp_stream = 0;
	if (disc.st == D_WAIT) disc.st = D_IDLE;
}
