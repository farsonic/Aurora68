/* SPDX-License-Identifier: MPL-2.0
 *
 * mac/a68player/video.c - Aurora68 Video: MPEG-1 films from the Mac's own disks, played by the Aurora68
 * Video Decoder card (docs/16-video-card.md, GitHub issues #24 and #25).
 *
 * Backstory (22-23 November 2026, docs/08 item 112). Issue #24 first planned Aurora68 Video as a network
 * program: a helper computer converting YouTube to MPEG-1, the Mac fetching it over MacTCP. The owner then
 * asked for a card with "no external dependencies", usable "through traditional means", and set YouTube
 * aside. So Aurora68 Video became the card's own player for files on the Mac: an MPEG-1 program stream
 * (MPEG-1 video, MP2 sound: .mpg, .mpeg, what the helper also makes) read from disk into the card's ring and
 * decoded by a Pi core into the window, its sound mixed with the Mac's own. The network browsing stays a
 * later addition.
 *
 * How it uses the card: the driver ".A68Video" (in the card's ROM, mac/a68video.s) for everything but the
 * bytes: its Status 100 for the window's address, Control 110 for start, pause, resume and stop, 112 for the
 * picture's place, 113 for the volume, 101 for the QuickTime switch. The bytes go straight from the File
 * Manager into the ring (a PBRead whose buffer is the ring, at the card's address), and head is moved after
 * them, as docs/video-decoder-api.md allows a program that streams at a high rate; so a film costs the 68k no
 * copying at all. Reading happens in the event loop's idle time, 32 KB at a time while the ring has room.
 *
 * The picture: the program draws nothing in it. It tells the card the window's content rectangle in the
 * coordinates of the screen it is on (screen 0 is slot 9's, the SE's own on the SE board; screen 1 the SE
 * board's HDMI screen in slot $A), and that it is not to be drawn while it is dragged and while a menu is down
 * (until 1.1, 23 November 2026, also while the window was not in front: now the card is handed the window's
 * visible region and draws where the window shows, in the background too, docs/08 item 124); the card draws
 * every picture into that screen's frame buffer at its depth, and draws the last one again when the window
 * moves or is uncovered while paused.
 *
 * Seeking is a click on the position bar: the card is stopped, the file read from that fraction of its
 * length (on a 2 KB boundary: MPEG program streams are packs of 2 KB from the usual encoders, and the card's
 * demultiplexer finds the next pack header itself), and started again. The length in time is estimated from
 * the bytes the card has taken against its position, so the times shown are approximate until the film has
 * played a few seconds. Human Interface Guidelines (1992); the window fits 512 x 342.
 */
#include "toolbox.h"
#include "psmac_video.h"

short mac_PBRead(ParamBlock *pb), mac_PBGetEOF(ParamBlock *pb);
short drv_open(void *pb), drv_control(void *pb), drv_status(void *pb), mac_SetFPos(ParamBlock *pb);

#define CHUNK     32768u                 /* one read into the ring: a tenth of a second at 2.5 Mbit/s */
#define BAR_H     30                     /* the control strip under the picture */
#define DEF_W     320                    /* the picture's place before the film says its size */
#define DEF_H     240

enum { M_APPLE = 128, M_FILE, M_EDIT, M_MOVIE, M_CARD };
static void *m_apple, *m_file, *m_edit, *m_movie, *m_card;
static void *win, *btn;
static unsigned char pbuf[256];

/* ---- the card ------------------------------------------------------------------------------------- */
typedef struct {
	void *qLink; short qType, ioTrap; void *ioCmdAddr, *ioCompletion; short ioResult;
	const unsigned char *ioNamePtr; short ioVRefNum, ioCRefNum, csCode;
	long csParam[6];
} CntrlParam;
static short drv;
static volatile unsigned char *card;            /* the card's window in this addressing mode */
static volatile struct a68v_block *blk;
#define RING_AT(o) (card + A68V_OFF_RING + (o))

static short ctl(short code, long a, long b)
{
	CntrlParam pb;
	memset(&pb, 0, sizeof pb);
	pb.ioCRefNum = drv; pb.csCode = code; pb.csParam[0] = a; pb.csParam[1] = b;
	return drv_control(&pb);
}
static int card_open(void)
{
	CntrlParam pb;
	memset(&pb, 0, sizeof pb);
	pb.ioNamePtr = (const unsigned char *)"\x09.A68Video";
	if (drv_open(&pb)) return 0;
	drv = pb.ioCRefNum;
	memset(&pb, 0, sizeof pb);
	pb.ioCRefNum = drv; pb.csCode = 100;
	if (drv_status(&pb) || !pb.csParam[0]) return 0;
	card = (volatile unsigned char *)pb.csParam[0];
	blk = (volatile struct a68v_block *)card;
	return blk->magic == A68V_MAGIC;
}
static int qt_on(void)
{
	CntrlParam pb;
	memset(&pb, 0, sizeof pb);
	pb.ioCRefNum = drv; pb.csCode = 100;
	return !drv_status(&pb) && pb.csParam[3];
}

/* ---- the film ----------------------------------------------------------------------------------- */
static short fref;                              /* the open file, 0 when none */
static long feof_len, fpos;                     /* its length, and where the next read starts */
static int playing, paused, sent_eof, suspended, hidden_by_us;
static unsigned long start_tail;                /* the card's tail when this stream started */
static long base_ms, base_pos;                  /* the film's time and byte at this stream's start (after a seek) */
static long dur_ms;                             /* the estimated length, 0 until known */
static int dur_fixed;                           /* it came from the file's mux_rate */
static int dbl;                                 /* Double Size */
static unsigned vol = 0x100;
static char status[100] = "";
static unsigned char fname[64];

static void say(const char *s) { int i = 0; while (s[i] && i < 99) { status[i] = s[i]; i++; } status[i] = 0; }

/* bytes into the ring while there is room: read straight into it, head moved after the bytes */
static void feed(void)
{
	int k;
	if (!fref || !playing) return;
	for (k = 0; k < 4 && fpos < feof_len; k++) {
		unsigned long h = blk->head, room = A68V_RING_SIZE - (h - blk->tail), o = h & (A68V_RING_SIZE - 1), n = CHUNK;
		if (room < CHUNK / 2) return;
		if (n > room) n = room;
		if (n > A68V_RING_SIZE - o) n = A68V_RING_SIZE - o;
		if ((long)n > feof_len - fpos) n = (unsigned long)(feof_len - fpos);
		ParamBlock pb;
		memset(&pb, 0, sizeof pb);
		pb.ioRefNum = fref;
		pb.u.io.ioBuffer = (void *)RING_AT(o);
		pb.u.io.ioReqCount = (long)n;
		pb.u.io.ioPosMode = fsFromStart;
		pb.u.io.ioPosOffset = fpos;
		short e = mac_PBRead(&pb);
		long got = pb.u.io.ioActCount;
		if (got > 0) { fpos += got; blk->head = h + (unsigned long)got; }
		if (e || got <= 0) { fpos = feof_len; break; }
	}
	if (fpos >= feof_len && !sent_eof) { ctl(114, 1, 0); sent_eof = 1; }
}

/* ---- the picture's place ------------------------------------------------------------------------- */
static Rect pic_rect(void)
{
	Rect r = *PORT_RECT(win);
	r.bottom = (short)(r.bottom - BAR_H);
	return r;
}
/* the screen a global point is on: 0 for slot 9's, 1 for slot $A's (the GDevice's driver's slot), and its
   top left corner */
static int screen_of(Point g, Point *origin)
{
	void **gd = *(void ***)0x8A8;                   /* DeviceList */
	while (gd && *gd) {
		unsigned char *d = *gd;
		Rect *r = (Rect *)(d + 34);                    /* gdRect */
		if (g.h >= r->left && g.h < r->right && g.v >= r->top && g.v < r->bottom) {
			short ref = *(short *)d;                   /* gdRefNum: the screen's driver */
			origin->h = r->left; origin->v = r->top;
			void ***ut = *(void ****)0x11C;            /* UTableBase */
			int slot = 9;
			if (ref < 0 && ut && ut[~ref] && *ut[~ref]) slot = *((unsigned char *)*ut[~ref] + 40);   /* dCtlSlot */
			return slot == 0xA ? A68V_SCREEN_HDMI : A68V_SCREEN_FIRST;
		}
		gd = *(void ***)(d + 30);                      /* gdNextGD */
	}
	origin->h = origin->v = 0;
	return 0;
}
static struct { long screen, top, left, bottom, right, visible; } place, sent;
static void tell_place(int force)
{
	if (!win || !drv) return;
	Rect r = pic_rect();
	Point tl = { r.top, r.left }, br = { r.bottom, r.right }, org;
	SetPort(win);
	LocalToGlobal(&tl); LocalToGlobal(&br);
	place.screen = screen_of(tl, &org);
	place.top = tl.v - org.v; place.left = tl.h - org.h; place.bottom = br.v - org.v; place.right = br.h - org.h;
	/* 1.1 (23 November 2026, docs/08 item 124): drawn wherever the window shows, in front or not, Aurora68 Video in front or
	   not: the card is handed the window's visible region and reads it at every picture (Aurora68 Play 1.8's way);
	   still not while a menu of ours is down, the window is dragged or Standard File's dialog is up (hidden_by_us), and
	   the card itself draws nothing while any program's menu is down */
	place.visible = !hidden_by_us && (blk->version >= 3 || (!suspended && FrontWindow() == win));   /* 1.0's rule for an older card */
	if (!force && !memcmp(&place, &sent, sizeof place)) return;
	sent = place;
	if (blk->version >= 3) {
		blk->vis_rgn = (unsigned long)*(void **)((char *)win + 24);    /* visRgn */
		blk->vis_flags = LM_MMU32BIT ? 0 : A68V_VF_24BIT;
		blk->clip_tag = A68V_CLIP_TAG + blk->geom_seq + 1;            /* for the geom_seq this Control 112 makes */
	}
	ctl(112, (long)&place, 0);
}

/* ---- starting, seeking, stopping ---------------------------------------------------------------- */
static void stop_film(void)
{
	if (drv && playing) ctl(110, A68V_C_STOP, 0);
	playing = paused = 0;
}
static void start_at(long pos)
{
	if (!fref) return;
	stop_film();
	pos &= ~2047L;
	fpos = base_pos = pos;
	base_ms = dur_ms && feof_len ? (long)((double)dur_ms * pos / feof_len) : 0;
	sent_eof = 0;
	ctl(114, 0, 0);                                 /* eof cleared */
	playing = 1;
	start_tail = blk->tail;
	if (pos > 0) {
		/* the film's first pack before the jump: its system header and the video's sequence header are written
		   once, at the start, by the usual encoders, and the card's demultiplexer and decoder need both (the
		   first seek in the testbed answered "no MPEG-1 video found"). The pictures until the next key frame
		   are drawn from a reference that is not theirs: under a second of blocks, as on any MPEG player of the
		   period that seeks without an index */
		unsigned long h = blk->head, o = h & (A68V_RING_SIZE - 1);
		ParamBlock pb;
		memset(&pb, 0, sizeof pb);
		pb.ioRefNum = fref; pb.u.io.ioBuffer = (void *)RING_AT(o); pb.u.io.ioReqCount = 2048;
		pb.u.io.ioPosMode = fsFromStart; pb.u.io.ioPosOffset = 0;
		if (o + 2048 <= A68V_RING_SIZE && !mac_PBRead(&pb)) blk->head = h + (unsigned long)pb.u.io.ioActCount;
		start_tail = blk->tail;
	}
	feed();
	tell_place(1);
	ctl(113, (long)(vol << 16 | vol), 0);
	ctl(110, A68V_C_START, 0);
	SetCTitle(btn, pstr(pbuf, "Pause"));
	say("Starting");
}
static void close_film(void)
{
	stop_film();
	if (fref) { ParamBlock pb; memset(&pb, 0, sizeof pb); pb.ioRefNum = fref; mac_PBClose(&pb); fref = 0; }
}
static int open_film(const unsigned char *name, short vref)
{
	ParamBlock pb;
	unsigned char head[12];
	close_film();
	memset(&pb, 0, sizeof pb);
	pb.ioNamePtr = name; pb.ioVRefNum = vref; pb.ioPermssn = 1;
	if (mac_PBOpen(&pb)) { say("That file could not be opened."); return 0; }
	fref = pb.ioRefNum;
	mac_PBGetEOF(&pb);
	feof_len = pb.u.io.ioMisc;
	memset(&pb, 0, sizeof pb);
	pb.ioRefNum = fref; pb.u.io.ioBuffer = head; pb.u.io.ioReqCount = 12; pb.u.io.ioPosMode = fsFromStart;
	mac_PBRead(&pb);
	if (pb.u.io.ioActCount != 12 || head[0] || head[1] || head[2] != 1 || (head[3] != 0xBA && head[3] != 0xB3)) {
		close_film();
		say("That is not an MPEG-1 film: Aurora68 Video plays MPEG program streams (.mpg).");
		return 0;
	}
	memcpy(fname, name, name[0] + 1);
	SetWTitle(win, fname);
	dur_ms = 0; dur_fixed = 0;
	/* the length from the first pack's mux_rate (MPEG-1: '0010' after the start code, the rate in units of 50
	   bytes a second in bits 1-22 of bytes 9-11); without it, estimated once the card has played a while */
	if (head[3] == 0xBA && (head[4] & 0xF0) == 0x20) {
		unsigned long rate = ((unsigned long)(head[9] & 0x7F) << 15 | (unsigned long)head[10] << 7 | head[11] >> 1) * 50;
		/* believed only between 8 KB and 2 MB a second (MPEG-1's own limits); FFmpeg writes a figure far off for
		   some sources (55 MB a second for the testbed's test film) */
		if (rate >= 8000 && rate <= 2000000) { dur_ms = (long)((double)feof_len * 1000.0 / rate); dur_fixed = 1; }
	}
	start_at(0);
	return 1;
}

/* ---- the window ----------------------------------------------------------------------------------- */
static Rect bar_rect(void)
{
	Rect r = *PORT_RECT(win), b;
	b.top = (short)(r.bottom - BAR_H + 10); b.bottom = (short)(b.top + 10);
	b.left = 80; b.right = (short)(r.right - 90);
	return b;
}
static char *put(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *num2(char *p, unsigned long v) { *p++ = (char)('0' + v / 10 % 10); *p++ = (char)('0' + v % 10); return p; }
static char *put_time(char *p, long ms)
{
	unsigned long s = ms < 0 ? 0 : (unsigned long)ms / 1000;
	if (s >= 3600) { p = num2(p, s / 3600); *p++ = ':'; }
	if (s / 60 % 60 >= 10 || s >= 3600) p = num2(p, s / 60 % 60); else *p++ = (char)('0' + s / 60 % 60);
	*p++ = ':';
	return num2(p, s % 60);
}
static long now_ms(void) { return playing ? base_ms + (long)blk->pos_ms : 0; }
static void draw_bar(void)
{
	Rect r = *PORT_RECT(win), strip = r, b = bar_rect(), f;
	char t[40], *p;
	strip.top = (short)(r.bottom - BAR_H);
	SetPort(win);
	EraseRect(&strip);
	MoveTo(r.left, strip.top); LineTo(r.right, strip.top);
	DrawControls(win);
	FrameRect(&b);
	f = b; InsetRect(&f, 1, 1);
	long frac = 0;
	if (dur_ms) frac = (long)((double)now_ms() * (f.right - f.left) / dur_ms);
	else if (feof_len) frac = (long)((double)base_pos * (f.right - f.left) / feof_len);
	if (!playing) frac = 0;
	if (frac > f.right - f.left) frac = f.right - f.left;
	f.right = (short)(f.left + frac);
	PaintRect(&f);
	p = put_time(t, now_ms());
	if (dur_ms) { *p++ = '/'; p = put_time(p, dur_ms); }
	*p = 0;
	Rect tr = { b.top - 2, (short)(b.right + 6), (short)(b.top + 12), r.right };
	EraseRect(&tr);
	MoveTo((short)(b.right + 6), (short)(b.top + 9));
	TextSize(9);
	DrawString(pstr(pbuf, t));
	if (status[0]) {
		Rect sr = { (short)(r.bottom - BAR_H + 21), 80, r.bottom, (short)(b.right) };
		EraseRect(&sr);
		MoveTo(80, (short)(r.bottom - 2));
		DrawString(pstr(pbuf, status));
	}
	TextSize(12);
}
static void draw_window(void)
{
	Rect p = pic_rect();
	SetPort(win);
	if (!drv) {                                     /* no card: two lines, and nothing else to do */
		EraseRect(&p);
		MoveTo(10, 30); DrawString(pstr(pbuf, "The Aurora68 Video Decoder card does not answer."));
		MoveTo(10, 48); DrawString(pstr(pbuf, "It is in slot $C of an Aurora68 Pi: check mac.video= in cmdline.txt."));
	} else if (!playing) {
		PaintRect(&p);                              /* black until a film plays: the card draws over it */
		if (!fref) {
			ForeColor(30); MoveTo(10, 30); DrawString(pstr(pbuf, "File > Open... an MPEG-1 film (.mpg)."));
			ForeColor(33);
		}
	}
	draw_bar();
}
static void fit_window(void)
{
	long w = blk->video_w, h = blk->video_h;
	if (!w || !h) { w = DEF_W; h = DEF_H; }
	if (dbl) { w *= 2; h *= 2; }
	Rect s = *RGN_BBOX(LM_GRAYRGN);                 /* never larger than the screens' room */
	while (w > s.right - s.left - 20 || h + BAR_H > s.bottom - s.top - 40) { w /= 2; h /= 2; }
	if (w < 240) { h = h * 240 / w; w = 240; }
	Rect r = *PORT_RECT(win);
	if (r.right - r.left == w && r.bottom - r.top == h + BAR_H) return;
	SizeWindow(win, (short)w, (short)(h + BAR_H), 1);
	MoveControl(btn, 8, (short)(h + 6));
	SetPort(win);
	InvalRect(PORT_RECT(win));
	tell_place(1);
}

/* ---- menus and events -------------------------------------------------------------------------- */
static void fix_menus(void)
{
	CheckItem(m_movie, 4, !dbl); CheckItem(m_movie, 5, dbl);
	CheckItem(m_movie, 7, vol == 0x100); CheckItem(m_movie, 8, vol == 0x80); CheckItem(m_movie, 9, vol == 0);
	CheckItem(m_card, 1, drv && qt_on());
	if (fref) { EnableItem(m_movie, 1); EnableItem(m_movie, 2); } else { DisableItem(m_movie, 1); DisableItem(m_movie, 2); }
}
static void play_pause(void)
{
	if (!fref) return;
	if (!playing || blk->state == A68V_S_ENDED) { start_at(0); return; }
	paused = !paused;
	ctl(110, paused ? A68V_C_PAUSE : A68V_C_RESUME, 0);
	SetCTitle(btn, pstr(pbuf, paused ? "Play" : "Pause"));
}
static void open_dialog(void)
{
	SFReply rep;
	Point where = { 60, 40 };
	hidden_by_us = 1; tell_place(0);
	SFGetFile(pt_long(where), pstr(pbuf, ""), 0, -1, 0, 0, &rep, 2);
	hidden_by_us = 0;
	if (rep.good) open_film(rep.fName, rep.vRefNum);
	tell_place(0);
	SetPort(win); InvalRect(PORT_RECT(win));
}
static int quit;
static void do_menu(long mi)
{
	short menu = (short)(mi >> 16), item = (short)mi;
	switch (menu) {
	case M_APPLE:
		if (item == 1) { say("Aurora68 Video 1.1: MPEG-1 on the Aurora68 Video Decoder card"); SetPort(win); InvalRect(PORT_RECT(win)); }
		else { GetItem(m_apple, item, pbuf); OpenDeskAcc(pbuf); SetPort(win); }
		break;
	case M_FILE:
		if (item == 1) open_dialog();
		else if (item == 2) { close_film(); SetWTitle(win, pstr(pbuf, "Aurora68 Video")); SetCTitle(btn, pstr(pbuf, "Play")); SetPort(win); InvalRect(PORT_RECT(win)); }
		else if (item == 4) quit = 1;
		break;
	case M_EDIT: SystemEdit(item - 1); break;
	case M_MOVIE:
		if (item == 1) play_pause();
		else if (item == 2) { if (fref) start_at(0); }
		else if (item == 4 || item == 5) { dbl = item == 5; fit_window(); }
		else if (item >= 7 && item <= 9) { vol = item == 7 ? 0x100 : item == 8 ? 0x80 : 0; ctl(113, (long)(vol << 16 | vol), 0); }
		break;
	case M_CARD:
		if (item == 1) ctl(101, (long)(qt_on() ? 0 : 1) << 16, 0);   /* a word: the high half of the first long */
		break;
	}
	HiliteMenu(0);
}
static void mouse_down(EventRecord *ev)
{
	void *w = 0, *c = 0;
	int part = FindWindow(pt_long(ev->where), &w);
	switch (part) {
	case inMenuBar:
		hidden_by_us = 1; tell_place(0);
		fix_menus();
		do_menu(MenuSelect(pt_long(ev->where)));
		hidden_by_us = 0; tell_place(0);
		break;
	case inSysWindow: SystemClick(ev, w); break;
	case inDrag:
		hidden_by_us = 1; tell_place(0);
		DragWindow(w, pt_long(ev->where), RGN_BBOX(LM_GRAYRGN));
		hidden_by_us = 0; tell_place(0);
		break;
	case inGoAway:
		if (TrackGoAway(w, pt_long(ev->where))) quit = 1;
		break;
	case inContent:
		if (w != FrontWindow()) { SelectWindow(w); break; }
		SetPort(win);
		{
			Point p = ev->where;
			GlobalToLocal(&p);
			if (FindControl(pt_long(p), win, &c) && c == btn) { if (TrackControl(c, pt_long(p), 0)) play_pause(); break; }
			Rect b = bar_rect();
			if (fref && PtInRect(pt_long(p), &b)) {
				long pos = (long)((double)(p.h - b.left) * feof_len / (b.right - b.left));
				start_at(pos);
			}
		}
		break;
	}
}
static void finder_file(void)
{
	unsigned char **h = *(unsigned char ***)0xAEC;  /* AppParmHandle: message, count, {vRefNum, fType, versNum, fName} */
	if (!h || !*h) return;
	unsigned char *p = *h;
	if (p[1] != 0 || (p[2] << 8 | p[3]) < 1) return;
	p += 4;
	unsigned char name[64];
	memcpy(name, p + 8, p[8] < 63 ? p[8] + 1 : 64);
	open_film(name, (short)(p[0] << 8 | p[1]));
}

int main(void)
{
	m_apple = NewMenu(M_APPLE, pstr(pbuf, "\x14"));
	AppendMenu(m_apple, pstr(pbuf, "About Aurora68 Video\xC9;(-"));
	AddResMenu(m_apple, TYPE('D', 'R', 'V', 'R'));
	InsertMenu(m_apple, 0);
	m_file = NewMenu(M_FILE, pstr(pbuf, "File"));
	AppendMenu(m_file, pstr(pbuf, "Open\xC9/O;Close/W;(-;Quit/Q"));
	InsertMenu(m_file, 0);
	m_edit = NewMenu(M_EDIT, pstr(pbuf, "Edit"));
	AppendMenu(m_edit, pstr(pbuf, "Undo/Z;(-;Cut/X;Copy/C;Paste/V;Clear"));
	InsertMenu(m_edit, 0);
	m_movie = NewMenu(M_MOVIE, pstr(pbuf, "Movie"));
	AppendMenu(m_movie, pstr(pbuf, "Play/Pause/P;Back to Start/B;(-;Normal Size/1;Double Size/2;(-;Loud;Half Volume;Silent"));
	InsertMenu(m_movie, 0);
	m_card = NewMenu(M_CARD, pstr(pbuf, "Card"));
	AppendMenu(m_card, pstr(pbuf, "Decode QuickTime Movies on the Card"));
	InsertMenu(m_card, 0);
	DrawMenuBar();

	int have = card_open();
	if (!have) drv = 0;
	Rect b = { 50, 20, (short)(50 + DEF_H + BAR_H), (short)(20 + DEF_W) };
	win = (void *)NewWindow(0, &b, pstr(pbuf, "Aurora68 Video"), 1, documentProc, (void *)-1, 1, 0);
	SetPort(win);
	Rect br = { (short)(DEF_H + 6), 8, (short)(DEF_H + 24), 68 };
	btn = (void *)NewControl(win, &br, pstr(pbuf, "Play"), 1, 0, 0, 1, pushButProc, 0);
	if (have) { ctl(110, A68V_C_STOP, 0); finder_file(); }

	EventRecord ev;
	unsigned long last_bar = 0;
	unsigned long last_w = 0;
	while (!quit) {
		int got = WaitNextEvent(everyEvent, &ev, playing ? 1 : 20, 0);
		if (got) switch (ev.what) {
		case mouseDown: mouse_down(&ev); break;
		case keyDown: case autoKey: {
			char ch = (char)(ev.message & charCodeMask);
			if (ev.modifiers & cmdKey) { fix_menus(); do_menu(MenuKey(ch)); }
			else if (ch == ' ') play_pause();
			break;
		}
		case updateEvt:
			if ((void *)ev.message == win) { BeginUpdate(win); draw_window(); EndUpdate(win); tell_place(1); }
			break;
		case activateEvt: tell_place(0); break;
		case osEvt:
			if (((unsigned long)ev.message >> 24) == suspendResumeMessage) { suspended = !(ev.message & resumeFlag); tell_place(0); }
			break;
		}
		if (!drv) continue;
		feed();
		tell_place(0);
		if (playing) {
			if (!dur_fixed && blk->state == A68V_S_PLAYING && !paused && blk->pos_ms > 3000) {   /* the length, from the rate so far */
				unsigned long ring = blk->head - blk->tail, inside = blk->buffered > ring ? blk->buffered - ring : 0;
				unsigned long took = blk->tail - start_tail - inside;      /* the bytes the decoder has used */
				if (took > 65536) dur_ms = base_ms + (long)((double)blk->pos_ms * (feof_len - base_pos) / (double)took);
			}
			if (blk->video_w != last_w) { last_w = blk->video_w; fit_window(); }
			const char *s = blk->state == A68V_S_ERROR ? (const char *)blk->message :
			                blk->state == A68V_S_ENDED ? "Ended" : blk->state == A68V_S_PAUSED ? "Paused" :
			                blk->state == A68V_S_BUFFERING ? "Buffering" : blk->state == A68V_S_PLAYING ? "" : "";
			char t[100], *p = put(t, s);
			if (blk->state == A68V_S_PLAYING) {
				p = put(p, "Playing, ");
				char n[12]; int k = 0; unsigned long v = blk->frames_shown;
				do { n[k++] = (char)('0' + v % 10); v /= 10; } while (v);
				while (k) *p++ = n[--k];
				p = put(p, " pictures");
			}
			*p = 0;
			if (LM_TICKS - last_bar > 20) {
				int changed = memcmp(t, status, (unsigned long)(p - t) + 1) != 0;
				if (changed) say(t);
				draw_bar();
				last_bar = LM_TICKS;
			}
			if (blk->state == A68V_S_ENDED) { SetCTitle(btn, pstr(pbuf, "Play")); }
		}
	}
	close_film();
	return 0;
}
