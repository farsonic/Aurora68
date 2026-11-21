/* SPDX-License-Identifier: MPL-2.0
 *
 * mac/piplay/radio.c - internet radio in PiPlay: an MP3 stream over HTTP, fetched with MacTCP, its
 * audio poured into the card's stream channel for the Pi to decode.
 *
 * Backstory (21 November 2026, docs/08 item 83). With MP3 files playing on the card (item 82) and the
 * user's SE on WiFi through Riptide, the next wish was SomaFM. A 68k cannot decode MP3, and could not
 * do the TLS of https either; but Shoutcast and Icecast still serve plain HTTP, and the card already
 * takes an open-ended stream whose decoder finds its way again after a gap. So the Mac does only what
 * a Mac of 1995 could: TCP and a little parsing.
 *
 * MacTCP's interface only, as every MacTCP program of the time used it: the ".IPP" driver opened by
 * name, Control calls with the MacTCP Programmer's Guide's parameter blocks (TCPCreate, TCPActiveOpen,
 * TCPSend, TCPRcv, TCPAbort, TCPRelease), the name looked up through the resolver code ('dnrp') that
 * Apple's DNR glue finds in the "MacTCP DNR" or a control panel (written here again: the glue is
 * Apple's). Nothing depends on Riptide, so Apple's MacTCP 2.0.6 does as well (the testbed runs both).
 *
 * The request is HTTP/1.0 with "Icy-MetaData: 1"; the answer (icy.c) may be "HTTP/1.x 200", "ICY 200
 * OK", a redirection (301, 302, 303, 307, 308: followed three times at most, plain http only) or a
 * playlist (.pls, .m3u: its first http:// stream is taken). A server that wants HTTP/1.1 (426) is
 * asked again so, and then its chunks are undone.
 *
 * Audio keeps flowing whatever the Mac is doing. The receive call is asynchronous and its completion
 * routine (net.s, c_rcv_done) does the work at interrupt time: the bytes through the ICY parser, the
 * audio into the card's ring, the next TCPRcv. So a held menu, a modal dialog or another program in
 * front does not stop the stream; the event loop (radio_poll) only connects, reconnects and redraws.
 * The card is started paused and resumed once about three seconds are in hand (the Pi would start at
 * 48 KB, three seconds of 128 kbit/s; the pause makes it three seconds at any rate). When the
 * connection ends or nothing comes for ten seconds, PiPlay connects again after 1, 2, 4 ... up to 30
 * seconds, and leaves the card playing what it has; the Pi's decoder finds the next frame in what the
 * new connection brings (psmac_audio.c), so a reconnect costs at most what the buffer cannot cover.
 *
 * Everything is logged as "[MAC] PiPlay: radio: ...". Plain words for the user when something fails:
 * "The station did not answer.", "The name could not be found: is the network up?" ...
 */
#include "toolbox.h"
#include "psmb.h"
#include "icy.h"
#include "radio.h"

extern volatile struct psmb_stream *ps;          /* piplay.c: the card's stream block and ring */
extern volatile unsigned char *ring;

/* piplay/net.s and fs.s */
short mac_PBControl(void *pb), mac_PBControlAsync(void *pb);
void mac_rcv_completion(void);
long mac_dnr_call(void *code, long sel, long a, long b, long c, long d);
void mac_dnr_result(void);
void mac_HLock(void *h), mac_HUnlock(void *h), mac_DisposPtr(void *p), mac_DisposHandle(void *h);
short mac_PBRead(ParamBlock *pb), mac_PBGetEOF(ParamBlock *pb), mac_PBHOpen(ParamBlock *pb), mac_PBHCreate(ParamBlock *pb),
      mac_PBHGetFInfo(ParamBlock *pb), mac_PBHSetFInfo(ParamBlock *pb), mac_PBHRename(ParamBlock *pb);
short mac_FindFolder(short vRefNum, long type, int create, short *vref, long *dirid);

struct radio_ui radio;
struct station *stations;
int nstations;

/* ---- MacTCP (the MacTCP Programmer's Guide, 1993) --------------------------------------------- */
enum { TCPCreate = 30, TCPActiveOpen = 32, TCPSend = 34, TCPRcv = 37, TCPAbort = 39, TCPRelease = 42 };
#define connectionClosing     (-23005)         /* the other end has closed: all it sent is delivered */
#define connectionDoesntExist (-23008)
#define connectionTerminated  (-23012)
#define openFailed            (-23015)
#define commandTimeout        (-23016)
#define cacheFault            (-23042)         /* StrToAddr: the answer comes through the result procedure */
#define noNameServer          (-23044)         /* none configured (MacTCP's control panel, "More...") */
#define authNameErr           (-23045)
#define noAnsErr              (-23046)
#define TCP_BUF   32768u                       /* the stream's receive buffer: MacTCP wants 4 KB at least */
#define RBUF      8192u                        /* one TCPRcv: half a second of 128 kbit/s */
#define RCV_TIMEOUT 10                         /* seconds without a byte: the station is gone */
#define PB_SIZE   128                          /* a TCPiopb is 104 bytes; room to spare */

static short ipp;                              /* .IPP's reference number */
static unsigned long stream;                   /* the TCP stream (StreamPtr), 0 when there is none */
static unsigned char *tcp_buf, *rbuf;
static unsigned char pb_ctl[PB_SIZE];          /* open and send: asynchronous, looked at by the event loop */
static unsigned char pb_end[PB_SIZE];          /* create, abort, release: synchronous */
static unsigned char pb_rcv[PB_SIZE];          /* the receive chain's, owned by its completion routine */
#define IO_RESULT(pb) (*(volatile short *)((pb) + 16))

static void pb_setup(unsigned char *pb, short cs)
{
	memset(pb, 0, PB_SIZE);
	*(short *)(pb + 24) = ipp;                   /* ioCRefNum */
	*(short *)(pb + 26) = cs;                    /* csCode */
	*(unsigned long *)(pb + 28) = stream;        /* tcpStream */
}
static short ints_off(void)
{
	short sr;
	__asm__ volatile("move.w %%sr,%0\n\tori.w #0x0700,%%sr" : "=d"(sr) : : "cc", "memory");
	return sr;
}
static void ints_on(short sr) { __asm__ volatile("move.w %0,%%sr" : : "d"(sr) : "cc", "memory"); }

static int ipp_open(void)
{
	static const unsigned char name[] = "\x04.IPP";
	ParamBlock pb;
	if (ipp) return 1;
	memset(&pb, 0, sizeof pb);
	pb.ioNamePtr = name;
	short e = mac_PBOpen(&pb);
	if (e) {
		char t[80];
		put_num(put_str(t, "radio: MacTCP's .IPP driver could not be opened, error -"), (unsigned long)-e)[0] = 0;
		pi_log(t);
		return 0;
	}
	ipp = pb.ioRefNum;
	return 1;
}

/* ---- the receive chain: at interrupt time ------------------------------------------------------ */
static struct icy *icy;
static volatile int rcv_busy;                  /* a TCPRcv is with MacTCP */
static volatile int rcv_wait_room;             /* the chain stopped because the ring was full */
static volatile short rcv_result;              /* why the chain ended: MacTCP's result, 0 if PiPlay ended it */
static volatile int rcv_stop;                  /* teardown: no more receives */
static volatile int hdr_seen, bad_answer;
static volatile unsigned long title_count, rcv_calls, last_rx;
static volatile int in_done, done_again;

static unsigned long ring_room(void) { return PSMB_STR_SIZE - (ps->head - ps->tail); }
/* the audio into the card's ring (room for it was looked at before the receive was made) */
static void to_ring(const unsigned char *p, unsigned long n)
{
	unsigned long h = ps->head;
	while (n) {
		unsigned long off = h % PSMB_STR_SIZE, c = PSMB_STR_SIZE - off;
		if (c > n) c = n;
		memcpy((void *)(ring + off), p, c);
		p += c; n -= c; h += c;
	}
	ps->head = h;
}
static void rcv_issue(void)
{
	pb_setup(pb_rcv, TCPRcv);
	*(void **)(pb_rcv + 12) = (void *)mac_rcv_completion;   /* ioCompletion */
	pb_rcv[32] = RCV_TIMEOUT;                    /* commandTimeoutValue, seconds */
	*(unsigned char **)(pb_rcv + 36) = rbuf;     /* rcvBuff */
	*(unsigned short *)(pb_rcv + 40) = RBUF;     /* rcvBuffLen: in, the room; out, what came */
	rcv_busy = 1;
	rcv_calls++;
	mac_PBControlAsync(pb_rcv);
}
/* MacTCP's completion of a TCPRcv (net.s keeps the registers). A receive made from here may complete
   before PBControl returns, calling this again: the inner call only says so, the loop takes it. */
void c_rcv_done(void)
{
	if (in_done) { done_again = 1; return; }
	in_done = 1;
	for (;;) {
		short r = IO_RESULT(pb_rcv), sr;
		done_again = 0;
		if (r == 0) {
			unsigned long n = *(unsigned short *)(pb_rcv + 40);
			unsigned ev = icy_feed(icy, rbuf, n, to_ring);
			last_rx = LM_TICKS;
			if (ev & ICY_EV_HEADERS) hdr_seen = 1;
			if (ev & ICY_EV_TITLE) title_count++;
			if (ev & ICY_EV_ERROR) bad_answer = 1;
			if (rcv_stop || icy->state == ICY_DONE || bad_answer) { rcv_result = 0; rcv_busy = 0; }
			else if (ring_room() < RBUF) { rcv_wait_room = 1; rcv_busy = 0; }
			else rcv_issue();
		} else {
			rcv_result = r;
			rcv_busy = 0;
		}
		sr = ints_off();
		if (!done_again) { in_done = 0; ints_on(sr); return; }
		ints_on(sr);
	}
}

/* ---- the resolver, found as Apple's DNR glue (dnr.c, 1988-1993) finds it ----------------------- */
/* hostInfo (MacTCP's AddressXlation.h): rtnCode, the name, then four addresses at 260 */
struct hostInfo { long rtnCode; char cname[255]; char pad; unsigned long addr[4]; };
_Static_assert(__builtin_offsetof(struct hostInfo, addr) == 260, "hostInfo");
enum { OPENRESOLVER = 1, CLOSERESOLVER = 2, STRTOADDR = 3 };
static void **dnr_h;
static struct { struct hostInfo hi; volatile long done; int busy; } look[2];
static int lk;                                 /* the lookup the connection waits for */

/* one folder's files of a type and creator, by index, the first whose resource fork holds a 'dnrp':
   its resource file is left open (the current one), its reference number returned; -1 if none */
static short search_folder(long type, long creator, short vref, long dir, char *found)
{
	ParamBlock pb;
	unsigned char name[64];
	short i;
	for (i = 1; i < 400; i++) {
		short ref;
		memset(&pb, 0, sizeof pb);
		pb.ioNamePtr = name; pb.ioVRefNum = vref;
		pb.u.f.ioFDirIndex = i;
		pb.u.f.ioFlNum = dir;                    /* ioDirID */
		if (mac_PBHGetFInfo(&pb)) return -1;
		if (pb.u.f.fdType != type || pb.u.f.fdCreator != creator) continue;
		ref = LM_SYSVERSION >= 0x0700 ? HOpenResFile(vref, dir, name, 1) : OpenRFPerm(name, vref, 1);   /* fsRdPerm */
		if (ref == -1) continue;
		if (!GetIndResource(TYPE('d', 'n', 'r', 'p'), 1)) { CloseResFile(ref); continue; }
		memcpy(found, name + 1, name[0]);
		found[name[0]] = 0;
		return ref;
	}
	return -1;
}
/* the glue's order: the Control Panels folder for 'cdev' 'ztcp' (MacTCP 1.1 and later), the System
   Folder for 'cdev' 'mtcp' ("MacTCP DNR", and Riptide DNR), the Control Panels folder for 'cdev'
   'mtcp'. On System 6 the control panels are in the System Folder (BootDrive's working directory). */
static int open_resolver(void)
{
	short vref = *(volatile short *)0x210, cvref = vref, ref = -1;   /* BootDrive */
	long dir = 0, cdir = 0, e;
	char found[64] = "the System file";
	char t[120];
	if (dnr_h) return 1;
	if (LM_SYSVERSION >= 0x0700) {
		mac_FindFolder(-32768, TYPE('m', 'a', 'c', 's'), 0, &vref, &dir);
		mac_FindFolder(-32768, TYPE('c', 't', 'r', 'l'), 0, &cvref, &cdir);
	}
	ref = search_folder(TYPE('c', 'd', 'e', 'v'), TYPE('z', 't', 'c', 'p'), cvref, cdir, found);
	if (ref == -1) ref = search_folder(TYPE('c', 'd', 'e', 'v'), TYPE('m', 't', 'c', 'p'), vref, dir, found);
	if (ref == -1) ref = search_folder(TYPE('c', 'd', 'e', 'v'), TYPE('m', 't', 'c', 'p'), cvref, cdir, found);
	void **h = (void **)GetIndResource(TYPE('d', 'n', 'r', 'p'), 1);   /* else one in the System file (a 512Ke) */
	if (!h) {
		if (ref != -1) CloseResFile(ref);
		put_num(put_str(t, "radio: no resolver ('dnrp') found, ResError -"), (unsigned long)-ResError())[0] = 0;
		pi_log(t);
		return 0;
	}
	DetachResource(h);
	if (ref != -1) CloseResFile(ref);
	mac_HLock(h);
	e = (short)mac_dnr_call(*h, OPENRESOLVER, 0, 0, 0, 0);
	if (e) {
		mac_HUnlock(h);
		mac_DisposHandle(h);
		put_num(put_str(put_str(put_str(t, "radio: the resolver from \""), found), "\" did not open, error -"), (unsigned long)-e)[0] = 0;
		pi_log(t);
		return 0;
	}
	dnr_h = h;
	put_str(put_str(put_str(t, "radio: the resolver ('dnrp') from \""), found), "\" is open")[0] = 0;
	pi_log(t);
	return 1;
}
static void close_resolver(void)
{
	if (!dnr_h) return;
	mac_dnr_call(*dnr_h, CLOSERESOLVER, 0, 0, 0, 0);
	mac_HUnlock(dnr_h);
	mac_DisposHandle(dnr_h);
	dnr_h = 0;
}

/* ---- the station's address --------------------------------------------------------------------- */
static char host[100], path[160], cur_url[200];
static unsigned short port;
static unsigned long addr;
static int redirects, http11, connected_once, playlist_hop;
static unsigned long backoff, t_state, conn_audio;
static const char *last_problem;

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int strcmp_(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a != *b; }
static int prefix(const char *s, const char *w)
{
	for (; *w; s++, w++)
		if (lower((unsigned char)*s) != *w) return 0;
	return 1;
}
static void copy(char *d, const char *s, int max)
{
	int i = 0;
	while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
	d[i] = 0;
}
/* "http://host[:port][/path]" (the "http://" may be left out) into host, port and path */
static const char *parse_url(const char *u)
{
	int i = 0;
	unsigned long p = 80;
	while (*u == ' ') u++;
	if (prefix(u, "https://")) return "Aurora68 Play plays plain http:// streams only: a Macintosh cannot do the encryption of https.";
	if (prefix(u, "http://")) u += 7;
	else if (prefix(u, "icy://")) u += 6;
	else { const char *c = u; while (*c && *c != '/' && *c != ':') c++; if (c[0] == ':' && c[1] == '/') return "Aurora68 Play plays http:// addresses only."; }
	while (*u && *u != '/' && *u != ':' && i < (int)sizeof host - 1) host[i++] = *u++;
	host[i] = 0;
	if (!i || (*u && *u != '/' && *u != ':')) return "That address has no station's name in it.";
	if (*u == ':') {
		u++;
		if (*u < '0' || *u > '9') return "The port number in that address is not a number.";
		p = 0;
		while (*u >= '0' && *u <= '9') p = p * 10 + (unsigned long)(*u++ - '0');
		if (!p || p > 65535) return "The port number in that address is not a port.";
	}
	port = (unsigned short)p;
	if (*u != '/') copy(path, "/", sizeof path);
	else copy(path, u, sizeof path);
	for (i = 0; path[i]; i++)
		if (path[i] == ' ') return "There is a space in that address.";
	return 0;
}
/* a dotted address ("192.168.68.1") as a number; 0 if the host is a name */
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
static char *put_addr(char *p, unsigned long a)
{
	int i;
	for (i = 24; i >= 0; i -= 8) { p = put_num(p, a >> i & 255); if (i) *p++ = '.'; }
	return p;
}
static void logs(const char *a, const char *b, const char *c)
{
	char t[128];
	char *p = put_str(t, "radio: ");
	if (a) for (; *a && p < t + 126; a++) *p++ = *a;
	if (b) for (; *b && p < t + 126; b++) *p++ = *b;
	if (c) for (; *c && p < t + 126; c++) *p++ = *c;
	*p = 0;
	pi_log(t);
}
static void logn(const char *a, long n, const char *b)
{
	char num[16], *p = num;
	if (n < 0) { *p++ = '-'; n = -n; }
	put_num(p, (unsigned long)n)[0] = 0;
	logs(a, num, b);
}

/* ---- the connection's life --------------------------------------------------------------------- */
static void set_state(int s) { radio.st = s; t_state = LM_TICKS; radio.seq++; }
static int busy(const unsigned char *pb) { return IO_RESULT(pb) > 0; }
/* everything given back: the receive and any open or send finished (TCPAbort makes them end), the
   stream released. Waits three seconds at most for each, with the event loop's work suspended. */
static void teardown(void)
{
	unsigned long t0;
	rcv_stop = 1;
	if (stream) {
		pb_setup(pb_end, TCPAbort);
		mac_PBControl(pb_end);
		for (t0 = LM_TICKS; (rcv_busy || busy(pb_ctl)) && LM_TICKS - t0 < 180; ) { }
		pb_setup(pb_end, TCPRelease);
		short e = mac_PBControl(pb_end);
		if (e) logn("TCPRelease said ", e, "");
		stream = 0;
	}
	rcv_busy = rcv_wait_room = 0;
	rcv_result = 0;
	rcv_stop = 0;
	hdr_seen = bad_answer = 0;
}
/* the card: a new open-ended MPEG stream, paused until enough is in hand */
static void card_start(void)
{
	if (ps->state != PSMB_STR_S_IDLE) pi_command(PSMB_STR_C_STOP);
	ps->eof = 0;
	ps->format = PSMB_STR_MPEG;
	ps->pcm_rate = ps->pcm_channels = ps->pcm_bits = ps->pcm_flags = 0;
	ps->data_bytes = 0;                          /* open-ended: no length, no end */
	pi_command(PSMB_STR_C_START);
	pi_command(PSMB_STR_C_PAUSE);
	radio.prebuffering = 1;
}
static void card_title(const char *t)
{
	int i;
	for (i = 0; t[i] && i < 127; i++) ps->title[i] = t[i];
	ps->title[i] = 0;
	ps->title_seq = ps->title_seq + 1;
}
static void connect_start(void);
/* the connection is gone or never came: try again later if this station has played, else tell the user */
static int lost;                               /* the connection was lost and has not come back yet */
static void failed(const char *say, long code)
{
	if (code) { char t[220]; put_str(put_str(t, say), " (error ")[0] = 0; logn(t, code, ")"); }   /* MacTCP's number too */
	else logs(say, 0, 0);
	teardown();
	if (connected_once) {                        /* it played: connect again after 1, 2, 4 ... 30 s */
		last_problem = say;
		lost = 1;
		radio.retry_at = LM_TICKS + backoff * 60;
		logn("trying again in ", (long)backoff, " s");
		backoff = backoff * 2 > 30 ? 30 : backoff * 2;
		set_state(RS_RETRY);
		return;
	}
	radio.error = say;
	radio.prebuffering = 0;
	if (ps->state != PSMB_STR_S_IDLE) pi_command(PSMB_STR_C_STOP);
	set_state(RS_OFF);
}
static void open_start(void)
{
	char t[100];
	if (!stream) {
		pb_setup(pb_end, TCPCreate);
		*(unsigned char **)(pb_end + 32) = tcp_buf;      /* rcvBuff */
		*(unsigned long *)(pb_end + 36) = TCP_BUF;       /* rcvBuffLen; notifyProc and userDataPtr 0 */
		short e = mac_PBControl(pb_end);
		if (e) { failed("MacTCP could not make a connection (TCPCreate).", e); return; }
		stream = *(unsigned long *)(pb_end + 28);
	}
	pb_setup(pb_ctl, TCPActiveOpen);
	pb_ctl[32] = 30;                             /* ulpTimeoutValue: 30 s to connect */
	pb_ctl[33] = 1;                              /* ulpTimeoutAction: abort */
	pb_ctl[34] = 0xC0;                           /* validityFlags: those two are set */
	*(unsigned long *)(pb_ctl + 36) = addr;      /* remoteHost */
	*(unsigned short *)(pb_ctl + 40) = port;     /* remotePort; localHost, localPort 0: MacTCP's choice */
	mac_PBControlAsync(pb_ctl);
	put_num(put_str(put_addr(put_str(t, "connecting to "), addr), " port "), port)[0] = 0;
	logs(t, 0, 0);
	set_state(RS_OPEN);
}
static char request[400];
static struct { unsigned short len; char *ptr; unsigned short end; } wds;    /* a write data structure */
static void send_request(void)
{
	char *p = put_str(request, "GET ");
	p = put_str(p, path);
	p = put_str(p, http11 ? " HTTP/1.1\r\nHost: " : " HTTP/1.0\r\nHost: ");
	p = put_str(p, host);
	if (port != 80) { *p++ = ':'; p = put_num(p, port); }
	p = put_str(p, "\r\nUser-Agent: Aurora68-Play/1.1 (Macintosh)\r\nAccept: */*\r\nIcy-MetaData: 1\r\nConnection: close\r\n\r\n");
	wds.len = (unsigned short)(p - request); wds.ptr = request; wds.end = 0;
	pb_setup(pb_ctl, TCPSend);
	pb_ctl[32] = 30; pb_ctl[33] = 1; pb_ctl[34] = 0xC0;
	pb_ctl[35] = 1;                              /* pushFlag */
	*(void **)(pb_ctl + 38) = &wds;              /* wdsPtr */
	mac_PBControlAsync(pb_ctl);
	set_state(RS_SEND);
}
static void connect_start(void)
{
	const char *e = parse_url(cur_url);
	if (e) { failed(e, 0); return; }
	if (dotted(host, &addr)) { open_start(); return; }
	if (!open_resolver()) { failed("Aurora68 Play could not find MacTCP's name resolver (the \"MacTCP DNR\"). A station's number address (such as 192.168.1.20) still works.", 0); return; }
	lk = look[0].busy && !look[0].done ? 1 : 0;   /* a lookup given up on may still answer into the other */
	if (look[lk].busy && !look[lk].done) { failed("The last name lookup has not finished yet: try again in a moment.", 0); return; }
	memset(&look[lk].hi, 0, sizeof look[lk].hi);
	look[lk].done = 0; look[lk].busy = 1;
	long r = (short)mac_dnr_call(*dnr_h, STRTOADDR, (long)host, (long)&look[lk].hi, (long)mac_dnr_result, (long)&look[lk].done);
	if (r == cacheFault) { logs("looking up ", host, ""); set_state(RS_RESOLVE); return; }
	look[lk].busy = 0;
	if (r == noNameServer) { failed("MacTCP knows no name server: give it one in the MacTCP control panel, or use the station's number address.", r); return; }
	if (r) { failed("The name could not be found: is the network up?", r); return; }
	addr = look[lk].hi.addr[0];
	open_start();
}
/* the station's answer is in (the headers): what to do with it */
static void redirect(const char *to, const char *why)
{
	char loc[200];
	if (++redirects > 3) { failed("The station sent Aurora68 Play from one address to another more than three times.", 0); return; }
	if (prefix(to, "https://")) { failed("The station sent Aurora68 Play on to a secure (https) address, and Aurora68 Play plays plain http:// streams only.", 0); return; }
	if (to[0] == '/') {                          /* a path on the same server */
		char *p = put_str(loc, "http://");
		p = put_str(p, host);
		if (port != 80) { *p++ = ':'; p = put_num(p, port); }
		copy(p, to, (int)(sizeof loc - (unsigned long)(p - loc)));
	} else copy(loc, to, sizeof loc);
	logs(why, loc, "");
	teardown();
	copy(cur_url, loc, sizeof cur_url);
	connect_start();
}
static void on_headers(void)
{
	struct icy *s = icy;
	char t[120], *p = t;
	p = put_str(p, s->icy ? "ICY " : http11 ? "HTTP/1.1 " : "HTTP ");
	p = put_num(p, (unsigned long)s->status); *p++ = ' ';
	p = put_str(p, s->reason); p = put_str(p, "; ");
	p = put_str(p, s->ctype[0] ? s->ctype : "no Content-Type");
	if (s->br) { p = put_str(p, ", "); p = put_num(p, s->br); p = put_str(p, " kbit/s"); }
	if (s->metaint) { p = put_str(p, ", metadata every "); p = put_num(p, s->metaint); p = put_str(p, " bytes"); }
	if (s->chunked) p = put_str(p, ", chunked");
	*p = 0;
	logs(t, 0, 0);
	if (s->status >= 200 && s->status < 300 && s->kind == ICY_K_AUDIO) {
		if (s->name[0]) { copy(radio.name, s->name, sizeof radio.name); logs("the station says it is \"", s->name, "\""); }
		if (s->br) radio.br = s->br;
		conn_audio = 0;
		if (!connected_once) { connected_once = 1; backoff = 1; }
		if (lost) { lost = 0; radio.reconnects++; logn("reconnected: reconnect ", (long)radio.reconnects, ""); }
		set_state(RS_PLAYING);
		return;
	}
	if (s->status >= 200 && s->status < 300 && s->kind == ICY_K_PLAYLIST) { playlist_hop = 1; return; }   /* its text, then */
	if ((s->status == 301 || s->status == 302 || s->status == 303 || s->status == 307 || s->status == 308) && s->location[0]) {
		char w[24];
		put_str(put_num(put_str(w, "redirected ("), (unsigned long)s->status), ") to ")[0] = 0;
		redirect(s->location, w);
		return;
	}
	if (s->status == 426 && !http11) {          /* "Upgrade Required": some servers and proxies want HTTP/1.1 */
		logs("the server wants HTTP/1.1: asking again so", 0, 0);
		http11 = 1;
		teardown();
		connect_start();
		return;
	}
	static char say[160];
	if (s->status >= 200 && s->status < 300) {
		if (prefix(s->ctype, "audio/aac") || prefix(s->ctype, "audio/mp4") || prefix(s->ctype, "audio/x-aac"))
			put_str(put_str(put_str(say, "This station sends AAC ("), s->ctype), "), and Aurora68 Play plays MP3 streams only.")[0] = 0;
		else if (prefix(s->ctype, "audio/ogg") || prefix(s->ctype, "application/ogg"))
			put_str(say, "This station sends Ogg Vorbis or Opus, and Aurora68 Play plays MP3 streams only.")[0] = 0;
		else if (prefix(s->ctype, "text/"))
			put_str(say, "That address is a web page, not a radio stream.")[0] = 0;
		else put_str(put_str(put_str(say, "That address sends "), s->ctype), ", not an MP3 stream.")[0] = 0;
		connected_once = 0;                      /* nothing to reconnect to */
	} else if (s->status == 404)
		put_str(say, "The station has no stream at that address (404 Not Found).")[0] = 0;
	else {
		p = put_num(put_str(say, s->status >= 500 ? "The station has a problem (" : "The station refused Aurora68 Play's request ("), (unsigned long)s->status);
		*p++ = ' ';
		put_str(put_str(p, s->reason), s->status >= 500 ? "): try again later." : ").")[0] = 0;
	}
	failed(say, 0);
}
/* a playlist's text has come (the connection closed, or its buffer is full): its first http:// stream */
static void playlist_done(void)
{
	char u[200];
	int https;
	playlist_hop = 0;
	if (icy_playlist_url(icy->pl, icy->pl_len, u, sizeof u, &https)) { redirect(u, "the playlist names "); return; }
	failed(https ? "The playlist names only secure (https) streams, and Aurora68 Play plays plain http:// streams only."
	             : "The playlist names no stream Aurora68 Play can play.", 0);
}
/* the receive chain has stopped: why, and what next */
static void chain_ended(void)
{
	short r = rcv_result;
	if (playlist_hop) { playlist_done(); return; }
	if (bad_answer) { failed("The station's answer is not HTTP: is that the address of a stream?", 0); return; }
	if (radio.st != RS_PLAYING) { failed("The station did not answer.", r); return; }
	{
		char t[120], *p = t;
		p = put_str(p, r == connectionClosing ? "the station closed the connection" :
		               r == commandTimeout ? "nothing came for ten seconds" :
		               r == connectionTerminated ? "the connection was broken" : r ? "the connection failed" : "the stream ended");
		if (r) { p = put_str(p, " (MacTCP "); *p++ = '-'; p = put_num(p, (unsigned long)-r); *p++ = ')'; }
		p = put_str(p, " after "); p = put_num(p, icy->audio_bytes); p = put_str(p, " bytes");
		*p = 0;
		failed(t, 0);
	}
}

/* ---- what the event loop does ------------------------------------------------------------------ */
static unsigned long titles_seen, last_under, under_t, rate_bytes;
static int in_underrun;
void radio_poll(void)
{
	unsigned long now = LM_TICKS;
	if (radio.st == RS_OFF) return;
	switch (radio.st) {
	case RS_RESOLVE:
		if (look[lk].done) {
			long r = look[lk].hi.rtnCode;
			look[lk].busy = 0;
			if (r) {
				logs(host, r == authNameErr ? ": the name servers know no such name" :
				           r == noAnsErr ? ": no name server answered" : ": the lookup failed", 0);
				failed("The name could not be found: is the network up?", r);
				return;
			}
			addr = look[lk].hi.addr[0];
			{ char t[130]; put_addr(put_str(put_str(t, host), " is "), addr)[0] = 0; logs(t, 0, 0); }
			open_start();
		} else if (now - t_state > 40 * 60) { logs(host, ": no answer from the resolver in 40 s", 0); failed("The name could not be found: is the network up?", 0); }
		return;
	case RS_OPEN:
		if (busy(pb_ctl)) return;
		if (IO_RESULT(pb_ctl)) { failed("The station did not answer.", IO_RESULT(pb_ctl)); return; }
		{
			char t[100], *q;
			radio.ipv = icy_ip_version(addr);
			radio.seq++;
			q = put_num(put_str(put_addr(put_str(t, "connected to "), addr), ":"), port);
			q = put_str(q, radio.ipv == 6 ? " (IPv6 through Riptide's stand-in); asking for " : " (IPv4); asking for ");
			*q = 0;
			logs(t, path, "");
		}
		send_request();
		return;
	case RS_SEND:
		if (busy(pb_ctl)) return;
		if (IO_RESULT(pb_ctl)) { failed("The station did not take Aurora68 Play's request.", IO_RESULT(pb_ctl)); return; }
		icy_init(icy);
		hdr_seen = bad_answer = 0;
		rcv_issue();
		set_state(RS_HEADERS);
		return;
	case RS_RETRY:
		if ((long)(now - radio.retry_at) >= 0) { redirects = 0; playlist_hop = 0; copy(cur_url, radio.url, sizeof cur_url); connect_start(); }
		return;
	}
	/* RS_HEADERS and RS_PLAYING: the receive chain is running */
	if (rcv_busy && IO_RESULT(pb_rcv) <= 0 && now - last_rx > 60 && now - t_state > 60) {
		/* a completion routine that was never called (it should not happen): take the call as done */
		short sr = ints_off();
		int was = in_done;
		ints_on(sr);
		if (!was && rcv_busy && IO_RESULT(pb_rcv) <= 0) { logs("a receive completed without its completion routine", 0, 0); c_rcv_done(); }
	}
	if (hdr_seen && radio.st == RS_HEADERS && !playlist_hop) { hdr_seen = 0; on_headers(); if (radio.st != RS_PLAYING && !playlist_hop) return; }
	if (playlist_hop && (!rcv_busy || icy->pl_len >= ICY_PLAYLIST)) { if (rcv_busy) teardown(); playlist_done(); return; }
	if (!rcv_busy) {
		if (rcv_wait_room && !rcv_stop) {
			if (ring_room() >= RBUF) { rcv_wait_room = 0; rcv_issue(); }
		} else { chain_ended(); return; }
	}
	if (radio.st == RS_HEADERS) {
		if (now - t_state > 30 * 60) failed("The station did not answer Aurora68 Play's request in 30 seconds.", 0);
		return;
	}
	/* playing */
	if (title_count != titles_seen) {
		char t[128];
		short sr = ints_off();
		titles_seen = title_count;
		copy(t, icy->title, sizeof t);
		ints_on(sr);
		if (strcmp_(t, radio.title)) {         /* a reconnection brings the same title again: said once */
			copy(radio.title, t, sizeof radio.title);
			card_title(t[0] ? t : radio.name[0] ? radio.name : radio.station);
			logs("now playing \"", t, "\"");
			radio.seq++;
		}
	}
	if (icy->audio_bytes - conn_audio > 65536 && backoff > 1) backoff = 1;   /* this connection works: start over */
	if (ps->bitrate && ps->bitrate != radio.br) { radio.br = ps->bitrate; radio.seq++; }
	rate_bytes = (radio.br ? radio.br : 128) * 125;                         /* kbit/s in bytes a second */
	{
		unsigned long held = ps->buffered + (ps->head - ps->tail);
		unsigned long ms = held / (rate_bytes / 100) * 10;
		radio.held_ms = ms;
		if (radio.prebuffering && (held >= 3 * rate_bytes || now - t_state > 20 * 60)) {
			char t[80];
			radio.prebuffering = 0;
			put_str(put_num(put_str(put_num(put_str(t, "buffered "), ms / 1000), "."), ms / 100 % 10), " s: playing")[0] = 0;
			if (!radio.paused) pi_command(PSMB_STR_C_RESUME);
			logs(t, 0, 0);
			radio.seq++;
		}
	}
	if (ps->underruns != last_under) {           /* the card ran dry: count each time once */
		if (!in_underrun) {
			in_underrun = 1;
			radio.underruns++;
			logn("the buffer ran dry: underrun ", (long)radio.underruns, "");
			radio.seq++;
		}
		last_under = ps->underruns; under_t = now;
	} else if (in_underrun && now - under_t > 60) in_underrun = 0;
}

void radio_init(void)
{
	tcp_buf = mac_newptr(TCP_BUF);
	rbuf = mac_newptr(RBUF);
	icy = mac_newptr(sizeof *icy);
	stations = mac_newptr(MAX_STATIONS * sizeof *stations);
}
int radio_play(const char *name, const char *url)
{
	const char *e;
	radio_stop();
	radio.error = 0;
	if (!tcp_buf || !rbuf || !icy || !stations) { radio.error = "There is not enough memory for the radio: give Aurora68 Play more in the Finder's Get Info."; return 0; }
	copy(radio.station, name, sizeof radio.station);
	copy(radio.url, url, sizeof radio.url);
	radio.name[0] = radio.title[0] = 0;
	radio.reconnects = radio.underruns = radio.held_ms = 0;
	radio.ipv = 0;
	radio.br = 0;
	radio.paused = 0;
	if ((e = parse_url(url))) { radio.error = e; return 0; }
	if (!ipp_open()) { radio.error = "MacTCP is not there: Aurora68 Play needs MacTCP (or Riptide) to play internet radio."; return 0; }
	logs(name, ", ", url);
	card_start();
	card_title(name);
	connected_once = 0; backoff = 1; redirects = 0; http11 = 0; playlist_hop = 0; lost = 0;
	titles_seen = title_count;
	last_under = ps->underruns; in_underrun = 0;
	last_problem = 0;
	copy(cur_url, url, sizeof cur_url);
	set_state(RS_RESOLVE);                       /* until connect_start says otherwise */
	connect_start();
	return radio.st != RS_OFF;
}
void radio_stop(void)
{
	char t[120], *p;
	if (radio.st == RS_OFF) return;
	teardown();
	if (look[lk].busy && !look[lk].done) logs("a name lookup is still out: its answer will be ignored", 0, 0);
	if (ps->state != PSMB_STR_S_IDLE) pi_command(PSMB_STR_C_STOP);
	p = put_str(t, "stopped; ");
	p = put_num(p, icy ? icy->audio_bytes : 0); p = put_str(p, " bytes on the last connection, ");
	p = put_num(p, radio.reconnects); p = put_str(p, " reconnects, ");
	p = put_num(p, radio.underruns); p = put_str(p, " underruns");
	*p = 0;
	logs(t, 0, 0);
	radio.prebuffering = radio.paused = 0;
	set_state(RS_OFF);
}
void radio_pause(int on)
{
	radio.paused = on;
	if (!radio.prebuffering) pi_command(on ? PSMB_STR_C_PAUSE : PSMB_STR_C_RESUME);
	logs(on ? "paused (the station goes on into the card's buffer)" : "resumed", 0, 0);
	radio.seq++;
}
void radio_quit(void)
{
	radio_stop();
	close_resolver();
}
const char *radio_status(char *b)
{
	char *p = b;
	switch (radio.st) {
	case RS_OFF: p = put_str(p, "Stopped"); break;
	case RS_RESOLVE: p = put_str(p, "Looking up"); break;
	case RS_OPEN: p = put_str(p, "Connecting"); break;
	case RS_SEND: case RS_HEADERS: p = put_str(p, "Asking"); break;
	case RS_RETRY: {
		long s = ((long)(radio.retry_at - LM_TICKS) + 59) / 60;
		p = put_num(put_str(p, "Retrying in "), (unsigned long)(s < 0 ? 0 : s));
		p = put_str(p, " s");
		break;
	}
	default:
		if (radio.paused) p = put_str(p, "Paused");
		else if (radio.prebuffering) {
			unsigned long pc = radio.held_ms / 30;          /* of three seconds */
			p = put_str(put_num(put_str(p, "Buffering "), pc > 99 ? 99 : pc), "%");
		} else p = put_str(p, "Playing");
	}
	*p = 0;
	return b;
}
const char *radio_problem(void) { return radio.st == RS_RETRY ? last_problem : 0; }

/* ---- the station list: "PiPlay Stations", a text file TeachText opens -------------------------- */
static const char builtin[] =
	"# Aurora68 Play Stations: the internet radio stations in Aurora68 Play's Radio menu.\r"
	"# One station a line: its name, an equals sign, its address:\r"
	"#     Name = http://host[:port]/path\r"
	"# Plain http:// MP3 streams (Shoutcast, Icecast) only; lines that start with # are notes.\r"
	"# Change the list with TeachText; Aurora68 Play reads it again when it comes to the front.\r"
	"# Throw this file away and Aurora68 Play writes the list below again.\r"
	"#\r"
	"# SomaFM: listener-supported, commercial-free radio from San Francisco (somafm.com).\r"
	"Groove Salad = http://ice2.somafm.com/groovesalad-128-mp3\r"
	"Drone Zone = http://ice2.somafm.com/dronezone-128-mp3\r"
	"Secret Agent = http://ice2.somafm.com/secretagent-128-mp3\r"
	"Lush = http://ice2.somafm.com/lush-128-mp3\r"
	"Space Station Soma = http://ice2.somafm.com/spacestation-128-mp3\r"
	"Indie Pop Rocks! = http://ice2.somafm.com/indiepop-128-mp3\r"
	"Beat Blender = http://ice2.somafm.com/beatblender-128-mp3\r"
	"Deep Space One = http://ice2.somafm.com/deepspaceone-128-mp3\r"
	"DEF CON Radio = http://ice2.somafm.com/defcon-128-mp3\r"
	"Left Coast 70s = http://ice2.somafm.com/seventies-128-mp3\r"
	"Underground 80s = http://ice2.somafm.com/u80s-128-mp3\r"
	"Sonic Universe = http://ice2.somafm.com/sonicuniverse-128-mp3\r"
	"#\r"
	"# NPO Radio 2, Dutch public radio, 192 kbit/s: its server has IPv6 as well as IPv4. With Riptide's\r"
	"# \"mactcp ipv6 prefer\" the window says IPv6 after the counts, else IPv4.\r"
	"NPO Radio 2 = http://icecast.omroep.nl/radio2-bb-mp3\r";
static const unsigned char st_name[] = "\x16" "Aurora68 Play Stations";
static const unsigned char st_old_name[] = "\x0F" "PiPlay Stations";   /* the program was PiPlay until 21 November 2026 */
static unsigned long st_mdat;                  /* the file's modification date when it was read */
static int st_have;

static void st_pb(ParamBlock *pb, short vref, long dir)
{
	memset(pb, 0, sizeof *pb);
	pb->ioNamePtr = st_name; pb->ioVRefNum = vref;
	pb->u.f.ioFlNum = dir;                       /* ioDirID */
}
static void parse_stations(const char *t, long n)
{
	long i = 0;
	nstations = 0;
	while (i < n && nstations < MAX_STATIONS) {
		long a = i, e, eq;
		while (i < n && t[i] != '\r' && t[i] != '\n') i++;
		e = i;
		while (i < n && (t[i] == '\r' || t[i] == '\n')) i++;
		while (a < e && (t[a] == ' ' || t[a] == '\t')) a++;
		while (e > a && (t[e - 1] == ' ' || t[e - 1] == '\t')) e--;
		if (a == e || t[a] == '#' || t[a] == ';') continue;
		for (eq = a; eq < e && t[eq] != '='; eq++) { }
		struct station *s = &stations[nstations];
		long ue = e, ua = eq < e ? eq + 1 : a, ne = eq;
		while (ua < ue && (t[ua] == ' ' || t[ua] == '\t')) ua++;
		while (ne > a && (t[ne - 1] == ' ' || t[ne - 1] == '\t')) ne--;
		if (eq >= e) ne = a;                     /* a bare address: its host is its name */
		long k;
		int o = 0;
		for (k = ua; k < ue && o < (int)sizeof s->url - 1; k++) s->url[o++] = t[k];
		s->url[o] = 0;
		if (!o) continue;
		o = 0;
		for (k = a; k < ne && o < (int)sizeof s->name - 1; k++) s->name[o++] = t[k];
		if (!o) {                                /* the host, then */
			const char *h = s->url;
			if (prefix(h, "http://")) h += 7;
			while (*h && *h != '/' && *h != ':' && o < (int)sizeof s->name - 1) s->name[o++] = *h++;
		}
		s->name[o] = 0;
		nstations++;
	}
}
int stations_load(void)
{
	ParamBlock pb;
	short vref, e;
	long dir;
	char t[80];
	if (!stations) return 0;
	pref_place(&vref, &dir);
	st_pb(&pb, vref, dir);
	if (mac_PBHGetFInfo(&pb)) {                  /* PiPlay's list, edited by the user, takes the new name */
		memset(&pb, 0, sizeof pb);
		pb.ioNamePtr = st_old_name; pb.ioVRefNum = vref; pb.u.f.ioFlNum = dir;
		pb.u.io.ioMisc = (long)st_name;          /* ioNewName */
		if (!mac_PBHRename(&pb))
			pi_log("radio: \"PiPlay Stations\" renamed \"Aurora68 Play Stations\"");
		st_pb(&pb, vref, dir);
	}
	if (mac_PBHGetFInfo(&pb)) {                  /* not there: written from the built-in list */
		st_pb(&pb, vref, dir);
		e = mac_PBHCreate(&pb);
		if (!e) {
			st_pb(&pb, vref, dir);
			if (!mac_PBHGetFInfo(&pb)) {
				pb.u.f.fdType = TYPE('T', 'E', 'X', 'T'); pb.u.f.fdCreator = TYPE('t', 't', 'x', 't');   /* TeachText's */
				pb.u.f.ioFlNum = dir;
				mac_PBHSetFInfo(&pb);
			}
			st_pb(&pb, vref, dir);
			pb.ioPermssn = fsWrPerm;
			if (!mac_PBHOpen(&pb)) {
				pb.u.io.ioBuffer = (void *)builtin; pb.u.io.ioReqCount = sizeof builtin - 1;
				pb.u.io.ioPosMode = fsFromStart; pb.u.io.ioPosOffset = 0;
				mac_PBWrite(&pb);
				mac_PBClose(&pb);
				memset(&pb, 0, sizeof pb); pb.ioVRefNum = vref; mac_PBFlushVol(&pb);
				pi_log("radio: \"Aurora68 Play Stations\" written from the built-in list");
			}
		}
		st_pb(&pb, vref, dir);
		if (mac_PBHGetFInfo(&pb)) {              /* a locked disk: the built-in list, from memory */
			parse_stations(builtin, sizeof builtin - 1);
			st_have = 0;
			pi_log("radio: \"Aurora68 Play Stations\" could not be written: the built-in list is used");
			return 1;
		}
	}
	st_mdat = (unsigned long)pb.u.f.ioFlMdDat;
	st_have = 1;
	st_pb(&pb, vref, dir);
	pb.ioPermssn = 1;
	if (mac_PBHOpen(&pb)) { parse_stations(builtin, sizeof builtin - 1); return 1; }
	pb.u.io.ioMisc = 0;
	mac_PBGetEOF(&pb);
	long n = pb.u.io.ioMisc > 16384 ? 16384 : pb.u.io.ioMisc;
	char *text = mac_newptr(n + 1);
	if (text) {
		pb.u.io.ioBuffer = text; pb.u.io.ioReqCount = n; pb.u.io.ioPosMode = fsFromStart; pb.u.io.ioPosOffset = 0;
		mac_PBRead(&pb);
		parse_stations(text, pb.u.io.ioActCount);
		mac_DisposPtr(text);
	}
	mac_PBClose(&pb);
	put_str(put_num(put_str(t, "radio: \"Aurora68 Play Stations\" read: "), (unsigned long)nstations), " stations")[0] = 0;
	pi_log(t);
	return 1;
}
int stations_check(void)
{
	ParamBlock pb;
	short vref;
	long dir;
	if (!st_have) return 0;
	pref_place(&vref, &dir);
	st_pb(&pb, vref, dir);
	if (mac_PBHGetFInfo(&pb)) { st_have = 0; return stations_load(); }   /* thrown away: written again */
	if ((unsigned long)pb.u.f.ioFlMdDat == st_mdat) return 0;
	return stations_load();
}
