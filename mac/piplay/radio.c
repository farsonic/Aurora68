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
 * Everything is logged as "[MAC] A68 Play: radio: ...". Plain words for the user when something fails:
 * "The station did not answer.", "The name could not be found: is the network up?" ...
 *
 * Riptide's .IPP6 (21 November 2026, docs/08 item 88). Riptide now offers a second driver beside .IPP,
 * ".IPP6", with MacTCP's calls and blocks but 20-byte addresses that hold IPv6 (IPv4 as ::ffff:a.b.c.d)
 * and a resolver of its own (Riptide's docs/IPP6.md; IPP6.h is its header, copied). When Gestalt
 * 'ipp6' answers version 1 or later and .IPP6 opens, the radio uses it: a name goes to ip6Resolve
 * (asynchronous, IPv6 addresses first), an address in the URL ("[fd68::80]" or dotted) to
 * ip6StrToAddr, and TCPActiveOpen gets each address in turn, at offset 36, until one connects. Every
 * other call (TCPCreate, TCPSend, the receive chain, TCPAbort, TCPRelease) is byte for byte MacTCP's,
 * so that code is shared. The window's "IPv4"/"IPv6" is then the family of the address connected to.
 * Without .IPP6 (Apple's MacTCP, an older Riptide) everything goes through .IPP and the DNR as before.
 *
 * Music videos on the video decoder card (Aurora68 Play 1.6, 23 November 2026, docs/08 item 114). A video is a
 * stream with an end, as a track is, but its bytes are an MPEG-1 program stream (the music server's /vmpeg/ID,
 * "video/mpeg") and go into the Aurora68 Video Decoder card's ring (vcard.c) instead of the sound card's: the same
 * connection, the same receive chain at interrupt time, with ring_room and to_ring asking radio.video which ring.
 * The card is started when the answer's headers have come (its own buffer of 64 KB is the prebuffering); at the
 * end the card is told so and the stream drains (RS_DRAIN until the card says "ended"), and the library plays
 * what follows: a video is never sent gaplessly after a song or another video, since each is a stream of its own
 * for its card. A connection lost on the way stops the card and asks again from the picture shown (resume()
 * with its seconds). The server makes one MPEG-1 stream at a time (VIDEO_TRANSCODES): a 503 while the last
 * one's ffmpeg is still being stopped (a seek, the next video) is asked again after a second, four times.
 *
 * The station list "Aurora68 Play Stations" is read and written here; what is in it (the built-in list,
 * the groups of the Radio menu's submenus, the defaults a user's file lacks) is stations.c's (item 87).
 */
#include "toolbox.h"
#include "psmb.h"
#include "icy.h"
#include "address.h"
#include "radio.h"
#include "vcard.h"
#include "psmac_video.h"
#define __MACTCP__ 1                             /* MacTCP's calls and results are named below, not in IPP6.h */
#include "IPP6.h"

extern volatile struct psmb_stream *ps;          /* piplay.c: the card's stream block and ring */
extern volatile unsigned char *ring;

/* piplay/net.s and fs.s */
short mac_PBControl(void *pb), mac_PBControlAsync(void *pb);
void mac_rcv_completion(void);
long mac_dnr_call(void *code, long sel, long a, long b, long c, long d);
long mac_Gestalt(long selector, long *response);
void mac_dnr_result(void);
void mac_HLock(void *h), mac_HUnlock(void *h), mac_DisposPtr(void *p), mac_DisposHandle(void *h);
short mac_PBRead(ParamBlock *pb), mac_PBGetEOF(ParamBlock *pb), mac_PBHOpen(ParamBlock *pb), mac_PBHCreate(ParamBlock *pb),
      mac_PBHGetFInfo(ParamBlock *pb), mac_PBHSetFInfo(ParamBlock *pb), mac_PBHRename(ParamBlock *pb);
short mac_FindFolder(short vRefNum, long type, int create, short *vref, long *dirid);

struct radio_ui radio;
struct stations *slist;
int net_prefer = 6;

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
#define TCP_BUF   32768u                       /* the stream's receive buffer: MacTCP wants 4 KB at least (64 KB, tried for the
                                                  video's stream on 23 November 2026, carried no more in the testbed) */
#define RBUF      8192u                        /* one TCPRcv: half a second of 128 kbit/s */
#define RCV_TIMEOUT 10                         /* seconds without a byte: the station is gone */
#define PB_SIZE   132                          /* MacTCP's TCPiopb is 104 bytes, .IPP6's TCP6iopb 130 */

static short ipp;                              /* the driver's reference number: .IPP6's or .IPP's */
static int v6;                                 /* 1: Riptide's .IPP6 (IPv6 and IPv4, names by the driver); 0: .IPP */
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

static short driver_open(const unsigned char *name)
{
	ParamBlock pb;
	memset(&pb, 0, sizeof pb);
	pb.ioNamePtr = name;
	short e = mac_PBOpen(&pb);
	if (!e) ipp = pb.ioRefNum;
	return e;
}
/* the network: Riptide's .IPP6 when Gestalt says it is there (docs/IPP6.md in Riptide, section 2: ask
   Gestalt first, as an OpenDriver of a name that is not installed searches the resource files), else
   MacTCP's .IPP with Apple's DNR for names, as every MacTCP program does */
int net_open(void)
{
	long version = 0;
	char t[80];
	if (ipp) return 1;
	if (LM_SYSVERSION >= 0x0604 && !mac_Gestalt((long)gestaltIPP6, &version) && version >= 1) {
		short e = driver_open((const unsigned char *)"\x05.IPP6");
		if (!e) {
			v6 = 1;
			put_num(put_str(t, "radio: Riptide's .IPP6 is there, interface version "), (unsigned long)version)[0] = 0;
			pi_log(t);
			return 1;
		}
		put_num(put_str(t, "radio: Gestalt names .IPP6 but it did not open, error -"), (unsigned long)-e)[0] = 0;
		pi_log(t);
	}
	v6 = 0;
	short e = driver_open((const unsigned char *)"\x04.IPP");
	if (e) {
		put_num(put_str(t, "radio: MacTCP's .IPP driver could not be opened, error -"), (unsigned long)-e)[0] = 0;
		pi_log(t);
		return 0;
	}
	return 1;
}
/* the driver and the resolver for the library's own connection (fetch.c) */
short net_ipp(void) { return ipp; }
int net_is_v6(void) { return v6; }
static int open_resolver(void);
static void **dnr_h;
void **net_dnr(void) { return open_resolver() ? dnr_h : 0; }

/* ---- the receive chain: at interrupt time ------------------------------------------------------ */
static struct icy *icy;
static volatile int rcv_busy;                  /* a TCPRcv is with MacTCP */
static volatile int rcv_wait_room;             /* the chain stopped because the ring was full */
static volatile short rcv_result;              /* why the chain ended: MacTCP's result, 0 if PiPlay ended it */
static volatile int rcv_stop;                  /* teardown: no more receives */
static volatile int hdr_seen, bad_answer;
static volatile unsigned long title_count, rcv_calls, last_rx;
static volatile int in_done, done_again;

static unsigned long ring_room(void) { return radio.video ? vc_room() : PSMB_STR_SIZE - (ps->head - ps->tail); }
/* the audio into the card's ring (room for it was looked at before the receive was made); a video's stream into the
   video decoder card's */
static void to_ring(const unsigned char *p, unsigned long n)
{
	unsigned long h = ps->head;
	if (radio.video) { vc_write(p, n); return; }
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
static int host6;                              /* host is an IPv6 address written in brackets: [fd68::80] */
static unsigned short port;
static unsigned long addr;                     /* .IPP: the station's IPv4 address */
/* .IPP6: ip6Resolve's answers, IPv6 first, each with the port in it; connected to in turn (ai) until an
   open succeeds. The lookup's block stays put until it completes or is withdrawn (ip6ResolveCancel). */
#define MAX_ADDRS 8
static IP6Address addrs[MAX_ADDRS];
static int naddr, ai, resolving;
static IP6iopb rpb;
static int redirects, http11, connected_once, playlist_hop;
static int busy_retries;                       /* a video's 503s asked again (on_headers) */
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
/* "http://host[:port][/path]" (the "http://" may be left out) into host, port and path: play/core/address.c,
   which reads a station's address the same way for both Plays (its words when it cannot) */
static const char *parse_url(const char *u)
{
	unsigned p = 80;
	const char *e = a68_parse_url(u, host, sizeof host, &p, path, sizeof path, &host6);
	port = (unsigned short)p;
	return e;
}
/* a dotted address ("192.168.68.1") as a number; 0 if the host is a name */
static int dotted(const char *h, unsigned long *a) { return a68_ipv4_value(h, a); }
static char *put_addr(char *p, unsigned long a)
{
	int i;
	for (i = 24; i >= 0; i -= 8) { p = put_num(p, a >> i & 255); if (i) *p++ = '.'; }
	return p;
}
/* the host as a URL or Host header writes it: an IPv6 address in brackets, and the port if not 80 */
static char *put_host(char *p)
{
	if (host6) *p++ = '[';
	p = put_str(p, host);
	if (host6) *p++ = ']';
	if (port != 80) { *p++ = ':'; p = put_num(p, port); }
	return p;
}
/* one of .IPP6's addresses: "192.168.68.1" for an IPv4-mapped one, "[fd68::80]" for IPv6 (the text from
   ip6AddrToStr, which may be called synchronously anywhere); put_addr6 adds the port */
static char *put_ip6(char *p, const IP6Address *a)
{
	if (IP6_IS_V4(a)) return put_addr(p, IP6_GET_V4(a));
	IP6iopb b;
	char text[64];
	memset(&b, 0, sizeof b);
	b.ioCRefNum = ipp; b.csCode = ip6AddrToStr;
	b.csParam.resolve.hostName = text; b.csParam.resolve.hostNameLen = sizeof text;
	b.csParam.resolve.addr = *a;
	if (mac_PBControl(&b)) put_str(text, "an IPv6 address")[0] = 0;
	*p++ = '[';
	p = put_str(p, text);
	*p++ = ']';
	return p;
}
static char *put_addr6(char *p, const IP6Address *a)
{
	p = put_ip6(p, a);
	*p++ = ':';
	return put_num(p, a->port);
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
static void cancel_lookup(void)
{
	if (resolving && rpb.ioResult > 0) {         /* withdrawn: it completes with ip6CancelledErr first */
		IP6iopb c;
		memset(&c, 0, sizeof c);
		c.ioCRefNum = ipp; c.csCode = ip6ResolveCancel;
		c.csParam.cancel.target = &rpb;
		mac_PBControl(&c);
		logs("the name lookup withdrawn", 0, 0);
	}
	resolving = 0;
}
static void teardown(void)
{
	unsigned long t0;
	cancel_lookup();
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
	if (radio.video) {                           /* the video decoder card: started when the answer has come */
		vc_begin();
		radio.prebuffering = 0;
		return;
	}
	ps->eof = 0;
	ps->format = PSMB_STR_MPEG;
	ps->start_ms = 0;
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
		if (radio.video && radio.resume) {       /* a video: from the picture shown, in a new stream of the card's */
			radio.resume(radio.url, sizeof radio.url, vc_pos_ms() / 1000);
			vc_begin();
			icy_init(icy);
		} else if (radio.finite && radio.resume) {   /* a track: from where its audio stopped, not its start */
			unsigned long rate = (radio.br ? radio.br : 128) * 125;
			radio.resume(radio.url, sizeof radio.url, icy->audio_bytes / rate);
			icy_init(icy);                       /* so that a failed attempt adds nothing */
		}
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
	if (radio.video) vc_stop();
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
	if (v6) {
		/* TCP6iopb: the 20-byte remoteAddr at 36 where MacTCP has remoteHost and remotePort, localAddr
		   at 56 left zero (the stack's choice); the fields after them 28 bytes on, all zero here */
		memcpy(pb_ctl + 36, &addrs[ai], sizeof addrs[ai]);
		/* more addresses to try: 8 s for this one. Riptide alternates the families in a long answer, so the
		   next is an IPv4 address when this IPv6 one does not answer (a network whose IPv6 does not reach
		   the station costs 8 s, not the whole list) */
		if (naddr - ai > 1) pb_ctl[32] = 8;
		mac_PBControlAsync(pb_ctl);
		put_addr6(put_str(t, "connecting to "), &addrs[ai])[0] = 0;
	} else {
		*(unsigned long *)(pb_ctl + 36) = addr;      /* remoteHost */
		*(unsigned short *)(pb_ctl + 40) = port;     /* remotePort; localHost, localPort 0: MacTCP's choice */
		mac_PBControlAsync(pb_ctl);
		put_num(put_str(put_addr(put_str(t, "connecting to "), addr), " port "), port)[0] = 0;
	}
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
	p = put_host(p);
	p = put_str(p, "\r\nUser-Agent: Aurora68-Play/1.5 (Macintosh)\r\nAccept: */*\r\nIcy-MetaData: 1\r\nConnection: close\r\n\r\n");
	wds.len = (unsigned short)(p - request); wds.ptr = request; wds.end = 0;
	pb_setup(pb_ctl, TCPSend);
	pb_ctl[32] = 30; pb_ctl[33] = 1; pb_ctl[34] = 0xC0;
	pb_ctl[35] = 1;                              /* pushFlag */
	*(void **)(pb_ctl + 38) = &wds;              /* wdsPtr */
	mac_PBControlAsync(pb_ctl);
	set_state(RS_SEND);
}
/* .IPP6: an address written in the URL (IPv6 in brackets, or dotted IPv4) through ip6StrToAddr, which
   answers at once; a name through ip6Resolve, asynchronously, IPv6 addresses first: RFC 6724's default
   order would put a unique local address (the testbed's fd68::) after IPv4 */
static void connect_start6(void)
{
	unsigned long a4;
	memset(&rpb, 0, sizeof rpb);
	rpb.ioCRefNum = ipp;
	rpb.csParam.resolve.hostName = host;
	rpb.csParam.resolve.port = port;
	naddr = ai = 0;
	if (host6 || dotted(host, &a4)) {
		rpb.csCode = ip6StrToAddr;
		short r = mac_PBControl(&rpb);
		if (r) { failed("That is not an IP address Aurora68 Play understands.", r); return; }
		addrs[0] = rpb.csParam.resolve.addr;
		naddr = 1;
		open_start();
		return;
	}
	rpb.csCode = ip6Resolve;
	/* the owner's choice (Connect By): which family first; Riptide alternates the two in its answer, so the
	   other family's address comes next when the first does not connect (open_start's 8 s each) */
	rpb.csParam.resolve.flags = net_prefer == 4 ? kIP6OrderIPv4First : kIP6OrderIPv6First;
	rpb.csParam.resolve.results = addrs;
	rpb.csParam.resolve.maxResults = MAX_ADDRS;
	rpb.csParam.resolve.timeout = 0;             /* the driver's 10 seconds */
	resolving = 1;
	mac_PBControlAsync(&rpb);
	logs("looking up ", host, net_prefer == 4 ? " (IPv4 first, then IPv6)" : " (IPv6 first, then IPv4)");
	set_state(RS_RESOLVE);
}
static void connect_start(void)
{
	const char *e = parse_url(cur_url);
	if (e) { failed(e, 0); return; }
	if (v6) { connect_start6(); return; }
	if (host6) { failed("An IPv6 address needs Riptide's .IPP6, which this Macintosh does not have. Use the station's name instead.", 0); return; }
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
		char *p = put_host(put_str(loc, "http://"));
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
	if (s->status >= 200 && s->status < 300 && (radio.video ? s->kind == ICY_K_VIDEO : s->kind == ICY_K_AUDIO)) {
		if (s->name[0]) { copy(radio.name, s->name, sizeof radio.name); logs("the station says it is \"", s->name, "\""); }
		if (s->br) radio.br = s->br;
		conn_audio = 0;
		if (!connected_once) { connected_once = 1; backoff = 1; }
		if (lost) { lost = 0; radio.reconnects++; logn("reconnected: reconnect ", (long)radio.reconnects, ""); }
		if (radio.video) { vc_go(); busy_retries = 0; logs("the video's stream has come: the card started", 0, 0); }
		set_state(RS_PLAYING);
		return;
	}
	if (radio.video && s->status == 503 && busy_retries < 4) {   /* the server's last MPEG-1 stream still stopping */
		busy_retries++;
		logn("the server makes another video now (503): asking again in a second, try ", (long)busy_retries, "");
		teardown();
		radio.retry_at = LM_TICKS + 60;
		set_state(RS_RETRY);
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
	if (radio.video) {                           /* the music server's words for a video it cannot send */
		p = put_num(put_str(say, "The music server could not send the video ("), (unsigned long)s->status);
		*p++ = ' ';
		put_str(put_str(p, s->status >= 200 && s->status < 300 ? s->ctype : s->reason), ").")[0] = 0;
		connected_once = 0;
		failed(say, 0);
		return;
	}
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
static unsigned long titles_seen;               /* (radio_poll's, below) */
/* a library track's stream has ended where the track does: the next track's stream follows in the
   same card stream, or the card plays out what it holds */
static void track_done(void)
{
	char u[200];
	unsigned long held = ps->buffered + (ps->head - ps->tail);
	unsigned long rate = (radio.br ? radio.br : 128) * 125;
	unsigned long at = ps->pos_ms + held / (rate / 100) * 10;
	logn("the track's stream is in, ", (long)icy->audio_bytes, " bytes");
	teardown();
	if (radio.video) {                           /* the card plays out what it holds; the library starts what follows */
		vc_end();
		vc_report("the stream is in");
		set_state(RS_DRAIN);
		return;
	}
	if (radio.next && radio.next(u, sizeof u, at)) {
		copy(radio.url, u, sizeof radio.url);
		copy(cur_url, u, sizeof cur_url);
		redirects = 0; http11 = 0; playlist_hop = 0; lost = 0;
		titles_seen = title_count;
		set_state(RS_RESOLVE);
		connect_start();
		return;
	}
	ps->eof = 1;                                 /* the last: the card ends when its buffer is played */
	if (radio.prebuffering) { radio.prebuffering = 0; if (!radio.paused) pi_command(PSMB_STR_C_RESUME); }
	set_state(RS_DRAIN);
}
/* the receive chain has stopped: why, and what next */
static void chain_ended(void)
{
	short r = rcv_result;
	if (playlist_hop) { playlist_done(); return; }
	if (bad_answer) { failed("The station's answer is not HTTP: is that the address of a stream?", 0); return; }
	if (radio.st != RS_PLAYING) { failed("The station did not answer.", r); return; }
	if (radio.finite && (r == connectionClosing || !r)) { track_done(); return; }
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
		if (v6) {
			if (rpb.ioResult > 0) {
				if (now - t_state > 40 * 60) { logs(host, ": no answer from .IPP6 in 40 s", 0); teardown(); failed("The name could not be found: is the network up?", 0); }
				return;
			}
			short r = rpb.ioResult;
			resolving = 0;
			if (r || !rpb.csParam.resolve.count) {
				logs(host, r == authNameErr ? ": the name servers know no such name" : r == noAnsErr ? ": no address, or no name server answered" :
				           r == noNameServer ? ": Riptide knows no name server" : ": the lookup failed", 0);
				failed(r == noNameServer ? "Riptide knows no name server: give it one in RiptidePrefs, or use the station's number address."
				                         : "The name could not be found: is the network up?", r);
				return;
			}
			naddr = rpb.csParam.resolve.count;
			{
				char t[200], *q = put_str(put_str(t, host), " is ");
				int k;
				for (k = 0; k < naddr && k < 3; k++) { if (k) q = put_str(q, ", "); q = put_ip6(q, &addrs[k]); }
				if (naddr > 3) q = put_str(q, " ...");
				*q = 0;
				logs(t, 0, 0);
			}
			open_start();
			return;
		}
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
		if (IO_RESULT(pb_ctl)) {
			if (v6 && ai + 1 < naddr) {          /* the next address (an unanswered IPv6 one: ipDestDeadErr in ~3 s) */
				char t[100];
				put_str(put_addr6(put_str(t, "no connection to "), &addrs[ai]), ", error ")[0] = 0;
				logn(t, IO_RESULT(pb_ctl), ": trying the next address");
				ai++;
				open_start();
				return;
			}
			{   /* the step, the address and port, and the stack's number, in the words for the user */
				static char say[200];
				short e = IO_RESULT(pb_ctl);
				char *q = put_str(say, "Aurora68 Play could not connect to ");
				if (v6) q = put_addr6(q, &addrs[ai]);
				else q = put_num(put_str(put_addr(q, addr), ":"), port);
				q = put_str(q, v6 ? " (Riptide error -" : " (MacTCP error -");
				q = put_num(q, (unsigned long)-e);
				q = put_str(q, e == openFailed ? ": no answer, or refused)." : e == -23033 /* ipDestDeadErr */ ? ": the network cannot reach it)." :
				               e == ip6NoSourceErr ? ": this Macintosh has no address of that family to send from)." : ").");
				*q = 0;
				failed(say, e);
			}
			return;
		}
		{
			char t[120], *q;
			if (v6) {                            /* the address connected to, as the open gave it back */
				IP6Address *a = (IP6Address *)(pb_ctl + 36);
				radio.ipv = IP6_IS_V4(a) ? 4 : 6;
				q = put_addr6(put_str(t, "connected to "), a);
				q = put_str(q, radio.ipv == 6 ? " (IPv6); asking for " : " (IPv4); asking for ");
			} else {
				radio.ipv = icy_ip_version(addr);
				q = put_num(put_str(put_addr(put_str(t, "connected to "), addr), ":"), port);
				q = put_str(q, radio.ipv == 6 ? " (IPv6 through Riptide's stand-in); asking for " : " (IPv4); asking for ");
			}
			radio.seq++;
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
	case RS_DRAIN:
		if (radio.video) {
			int vs = vc_state();
			if (vs == A68V_S_PLAYING || vs == A68V_S_PAUSED || vs == A68V_S_BUFFERING) return;
			vc_report(vs == A68V_S_ENDED ? "played to the end" : "ended");
		} else if (ps->state == PSMB_STR_S_PLAYING || ps->state == PSMB_STR_S_PAUSED) return;
		logs("the last track has played", 0, 0);
		radio.ended = 1;
		radio.prebuffering = radio.paused = 0;
		set_state(RS_OFF);
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
	if (radio.video) {                           /* the card's buffering is its own; what is watched here is an error */
		if (vc_state() == A68V_S_ERROR) {
			static char say[200];
			char m[100];
			copy(m, vc_message(), sizeof m);         /* the card's words: 127 characters at most, cut here */
			put_str(put_str(put_str(say, "The video decoder card could not play the video: "), m), ".")[0] = 0;
			vc_report("the card stopped");
			teardown();
			vc_stop();
			radio.error = say;
			set_state(RS_OFF);
		}
		return;
	}
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
	slist = mac_newptr(sizeof *slist);
}
static int play(const char *name, const char *url, int finite);
int radio_play(const char *name, const char *url) { return play(name, url, 0); }
int radio_play_track(const char *name, const char *url) { return play(name, url, 1); }
int radio_play_video(const char *name, const char *url) { return play(name, url, 2); }
static int play(const char *name, const char *url, int finite)
{
	const char *e;
	radio_stop();
	radio.error = 0;
	radio.video = finite == 2 && vc_present();
	busy_retries = 0;
	radio.finite = finite != 0;
	radio.ended = 0;
	if (!tcp_buf || !rbuf || !icy || !slist) { radio.error = "There is not enough memory for the radio: give Aurora68 Play more in the Finder's Get Info."; return 0; }
	copy(radio.station, name, sizeof radio.station);
	copy(radio.url, url, sizeof radio.url);
	radio.name[0] = radio.title[0] = 0;
	radio.reconnects = radio.underruns = radio.held_ms = 0;
	radio.ipv = 0;
	radio.br = 0;
	radio.paused = 0;
	if ((e = parse_url(url))) { radio.error = e; return 0; }
	if (!net_open()) { radio.error = "MacTCP is not there: Aurora68 Play needs MacTCP (or Riptide) to play internet radio."; return 0; }
	logs(name, ", ", url);
	logs(v6 ? "via .IPP6 (Riptide: IPv6 and IPv4 addresses, names by the driver)" : "via .IPP (MacTCP's interface: IPv4, names through the DNR)", 0, 0);
	card_start();
	if (!radio.video) card_title(name);
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
	ps->eof = 0;
	if (look[lk].busy && !look[lk].done) logs("a name lookup is still out: its answer will be ignored", 0, 0);
	if (ps->state != PSMB_STR_S_IDLE) pi_command(PSMB_STR_C_STOP);
	if (radio.video) vc_stop();
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
	if (radio.video) vc_pause(on);
	else if (!radio.prebuffering) pi_command(on ? PSMB_STR_C_PAUSE : PSMB_STR_C_RESUME);
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
	case RS_DRAIN: p = put_str(p, radio.paused ? "Paused" : "Playing"); break;
	case RS_RETRY: {
		long s = ((long)(radio.retry_at - LM_TICKS) + 59) / 60;
		p = put_num(put_str(p, "Retrying in "), (unsigned long)(s < 0 ? 0 : s));
		p = put_str(p, " s");
		break;
	}
	default:
		if (radio.paused) p = put_str(p, "Paused");
		else if (radio.video && vc_state() != A68V_S_PLAYING) p = put_str(p, "Buffering");
		else if (radio.prebuffering) {
			unsigned long pc = radio.held_ms / 30;          /* of three seconds */
			p = put_str(put_num(put_str(p, "Buffering "), pc > 99 ? 99 : pc), "%");
		} else p = put_str(p, "Playing");
	}
	*p = 0;
	return b;
}
const char *radio_problem(void) { return radio.st == RS_RETRY ? last_problem : 0; }

/* ---- the station list: "Aurora68 Play Stations", a text file TeachText opens --------------------- */
/* the built-in list, the file's groups and the defaults it lacks are stations.c's; here the file itself */
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
static void parse_stations(const char *t, long n) { stations_parse(slist, t, n); }
int stations_load(void)
{
	ParamBlock pb;
	short vref, e;
	long dir;
	char t[80];
	if (!slist) return 0;
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
				pb.u.io.ioBuffer = (void *)stations_builtin; pb.u.io.ioReqCount = stations_builtin_len;
				pb.u.io.ioPosMode = fsFromStart; pb.u.io.ioPosOffset = 0;
				mac_PBWrite(&pb);
				mac_PBClose(&pb);
				memset(&pb, 0, sizeof pb); pb.ioVRefNum = vref; mac_PBFlushVol(&pb);
				pi_log("radio: \"Aurora68 Play Stations\" written from the built-in list");
			}
		}
		st_pb(&pb, vref, dir);
		if (mac_PBHGetFInfo(&pb)) {              /* a locked disk: the built-in list, from memory */
			parse_stations(stations_builtin, stations_builtin_len);
			st_have = 0;
			pi_log("radio: \"Aurora68 Play Stations\" could not be written: the built-in list is used");
			return 1;
		}
	}
	st_mdat = (unsigned long)pb.u.f.ioFlMdDat;
	st_have = 1;
	st_pb(&pb, vref, dir);
	pb.ioPermssn = 1;
	if (mac_PBHOpen(&pb)) { parse_stations(stations_builtin, stations_builtin_len); return 1; }
	pb.u.io.ioMisc = 0;
	mac_PBGetEOF(&pb);
	long n = pb.u.io.ioMisc > 32768 ? 32768 : pb.u.io.ioMisc;     /* 128 stations of 250 bytes at most */
	char *text = mac_newptr(n + 1);
	if (text) {
		pb.u.io.ioBuffer = text; pb.u.io.ioReqCount = n; pb.u.io.ioPosMode = fsFromStart; pb.u.io.ioPosOffset = 0;
		mac_PBRead(&pb);
		parse_stations(text, pb.u.io.ioActCount);
		mac_DisposPtr(text);
	}
	mac_PBClose(&pb);
	char *p = put_str(put_num(put_str(t, "radio: \"Aurora68 Play Stations\" read: "), (unsigned long)slist->nstations), " stations");
	if (slist->ngroups) p = put_str(put_num(put_str(p, " in "), (unsigned long)slist->ngroups), " groups");
	*p = 0;
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

/* Radio > Add Default Stations. The user's file is theirs: nothing in it is changed, the built-in stations
   it lacks go at its end under their headings (stations_missing), after a note saying where they came from */
#define fsFromLEOF 2                           /* ioPosMode: from the logical end of the file */
int stations_add_defaults(void)
{
	static const char intro[] = "\r# Added by Aurora68 Play's Radio > Add Default Stations:\r";
	ParamBlock pb;
	short vref, e;
	long dir, n, max = stations_builtin_len + 256;   /* the built-in lines and a Return before each heading */
	int added = 0;
	char t[100], *out;
	stations_check();                            /* the file as it is now (written again if it was thrown away) */
	if (!slist || !st_have) { pi_log("radio: Add Default Stations: there is no stations file to add to"); return -1; }
	if (!(out = mac_newptr(max))) return -1;
	memcpy(out, intro, sizeof intro - 1);
	n = sizeof intro - 1;
	n += stations_missing(slist, stations_builtin, stations_builtin_len, out + n, max - n, &added);
	if (!added) {
		pi_log("radio: Add Default Stations: the file has every one of them");
		mac_DisposPtr(out);
		return 0;
	}
	pref_place(&vref, &dir);
	st_pb(&pb, vref, dir);
	pb.ioPermssn = fsWrPerm;
	e = mac_PBHOpen(&pb);
	if (!e) {
		pb.u.io.ioBuffer = out; pb.u.io.ioReqCount = n;
		pb.u.io.ioPosMode = fsFromLEOF; pb.u.io.ioPosOffset = 0;
		e = mac_PBWrite(&pb);
		mac_PBClose(&pb);
		memset(&pb, 0, sizeof pb); pb.ioVRefNum = vref; mac_PBFlushVol(&pb);
	}
	mac_DisposPtr(out);
	if (e) {                                     /* open in TeachText with write permission, a locked disk ... */
		char *p = put_str(t, "radio: Add Default Stations: the file could not be written (");
		if (e < 0) { *p++ = '-'; e = (short)-e; }
		put_str(put_num(p, (unsigned long)e), ")")[0] = 0;
		pi_log(t);
		return -1;
	}
	put_str(put_num(put_str(t, "radio: Add Default Stations: "), (unsigned long)added), " stations added at the end of the file")[0] = 0;
	pi_log(t);
	stations_load();
	return added;
}
