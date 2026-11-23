/* SPDX-License-Identifier: MPL-2.0
 *
 * mac/piplay/vcard.c - Aurora68 Play's music videos shown by the Aurora68 Video Decoder card.
 *
 * Backstory (23 November 2026, docs/08 item 114). The classic Play 1.5 played a music video's sound only
 * (/vaudio/ID, item 109) and told the server it showed no video, because the SE had no way to show one. The
 * kernel's video decoder card (docs/16, items 110 and 112) then gained an MPEG-1 stream channel and the music
 * server an MPEG-1 program stream of any video at the size asked (/vmpeg/ID?w=&h=&start=). This file is the
 * card's half of the classic Play 1.6: the card's driver, found as Aurora68 Video finds it (".A68Video", its
 * Status 100 for the card's window and the block's magic), the ring the receive chain pours the stream into,
 * and a window for the picture. The network is radio.c's as it was for songs: the same MacTCP or Riptide
 * .IPP6 connection, the same asynchronous receive chain at interrupt time, writing into this card's ring
 * instead of the sound card's (docs/video-decoder-api.md allows a program that streams at a high rate to
 * write the ring itself: the bytes, then head). The core (play/core/proto.c) builds the /vmpeg path and chooses
 * the size.
 *
 * The card, through its driver: Control 110 (start, pause, resume, stop), 112 (the picture's place: the window's
 * content rectangle in the pixels of the screen it is on, and whether it may be drawn), 113 (the volume, from
 * Play's own), 114 (the end of the stream). Status 100 once, for the window's address; the rest is read from the
 * card's block, as Aurora68 Video reads it (its state, position, size and numbers).
 *
 * The window (Human Interface Guidelines, 1992): a document window with a close box (and in 1.7 no grow box), as large
 * as the picture the server sends (320 x 240 on the SE's 512 x 342 screen, larger on a large HDMI screen:
 * a68_vmpeg_size), black where the card has drawn nothing yet. Play draws nothing in it: the card draws every
 * picture into the screen's frame buffer at its depth (dithered to one bit on the SE's own screen). In 1.6 and 1.7
 * it was not to draw while the window was covered by another (a window of Play's in front of it that overlapped it,
 * or a dialog), dragged, under a menu, hidden or while another program was in front (1.8: only where the window
 * does not show, see below and tell_place); the kernel draws the last picture again when
 * the place changes while the stream is paused or dry. A click in it plays or pauses; its close box hides the
 * picture and the sound goes on (Controls > Show Video brings it back). It opens on the screen the player window
 * is on, where the last video's window was if it is still on a screen.
 *
 * Its size (Aurora68 Play 1.7, 23 November 2026, docs/08 item 120). The macOS Play 1.5's player bar has a picture that
 * expands the video to fill the window and shrinks it back. Here the same is the window's zoom box and Controls > Normal
 * Size, Double Size and Fill Screen (Command-1, 2, 3, as Aurora68 Video has Normal and Double): the stream stays the size
 * it was asked at (the network carries what it carried), and the card, which scales a picture to the rectangle it is
 * given, draws it larger. Fill Screen is the largest of the picture's shape that fits the screen the window is on, below
 * the menu bar on the main screen; Double Size is twice the picture, or Fill Screen where twice would not fit. Back to
 * Normal Size the window goes where it was before.
 *
 * Aurora68 Play 1.8 (23 November 2026, docs/08 item 124), from the owner's use on the real SE with an HDMI screen of
 * 2560 x 1440: "Resizable windows", and "Video in the window stops playing if it isn't focused. This should work in
 * the background". The window became a zoomDocProc with a size box, which sits in a strip of 15 pixels below the
 * picture (the card's picture would cover it inside); dragging it makes the picture the video's own shape nearest to
 * the outline (vc_grow), 160 pixels wide at least and Fill Screen at most, the card not drawing while the outline is
 * dragged; that width is kept as a fourth size, VC_CUSTOM, for the next video and in the preferences, and the zoom box
 * toggles Fill Screen and whichever size the window had. The strip shows the time. And the card is handed the window's
 * visible region (tell_place): it draws where the window shows, whatever is in front, also while another program is
 * the front one (see tell_place for what still stops it).
 */
#include "toolbox.h"
#include "psmac_video.h"
#include "proto.h"
#include "vcard.h"

short mac_PBControl(void *pb), mac_PBStatus(void *pb);
void pi_log(const char *text);
char *put_str(char *p, const char *s);
char *put_num(char *p, unsigned long v);

/* ---- the card's driver ------------------------------------------------------------------------------ */
typedef struct {
	void *qLink; short qType, ioTrap; void *ioCmdAddr, *ioCompletion; short ioResult;
	const unsigned char *ioNamePtr; short ioVRefNum, ioCRefNum, csCode;
	long csParam[6];
} CntrlParam;
static short drv;
static volatile unsigned char *card;                  /* the card's window in this addressing mode */
static volatile struct a68v_block *blk;
#define RING_AT(o) (card + A68V_OFF_RING + (o))

static short ctl(short code, long a, long b)
{
	CntrlParam pb;
	memset(&pb, 0, sizeof pb);
	pb.ioCRefNum = drv; pb.csCode = code; pb.csParam[0] = a; pb.csParam[1] = b;
	return mac_PBControl(&pb);
}
int vc_init(void)
{
	ParamBlock op;
	CntrlParam pb;
	char t[160];
	if (blk) return 1;
	memset(&op, 0, sizeof op);
	op.ioNamePtr = (const unsigned char *)"\x09.A68Video";
	if (mac_PBOpen(&op)) { pi_log("video: no Aurora68 Video Decoder card (\".A68Video\" did not open): videos play their sound"); return 0; }
	drv = op.ioRefNum;
	memset(&pb, 0, sizeof pb);
	pb.ioCRefNum = drv; pb.csCode = 100;
	if (mac_PBStatus(&pb) || !pb.csParam[0]) { pi_log("video: the card's driver gave no window: videos play their sound"); drv = 0; return 0; }
	card = (volatile unsigned char *)pb.csParam[0];
	if (((volatile struct a68v_block *)card)->magic != A68V_MAGIC) { pi_log("video: the card's block is not there: videos play their sound"); drv = 0; card = 0; return 0; }
	blk = (volatile struct a68v_block *)card;
	put_str(put_num(put_str(put_num(put_str(t, "video: the video decoder card is there (stream block "), pb.csParam[1]),
	                ", ring "), (unsigned long)pb.csParam[4] >> 10), " KB): videos are shown")[0] = 0;
	pi_log(t);                                         /* under 118 characters: pi_log's line */
	if (blk->state != A68V_S_IDLE) ctl(110, A68V_C_STOP, 0);   /* whatever an earlier program left */
	return 1;
}
int vc_present(void) { return blk != 0; }

/* ---- the stream ------------------------------------------------------------------------------------------ */
static unsigned long bytes_in, t_begin, level_min, level_max, dry_ticks, dry_from, low_ticks, low_from;
static int started, was_playing;
unsigned long vc_room(void) { return blk ? A68V_RING_SIZE - (blk->head - blk->tail) : 0; }
/* at interrupt time, from the receive chain's completion routine: memory only, no trap */
void vc_write(const unsigned char *p, unsigned long n)
{
	unsigned long h = blk->head;
	bytes_in += n;
	while (n) {
		unsigned long off = h & (A68V_RING_SIZE - 1), c = A68V_RING_SIZE - off;
		if (c > n) c = n;
		memcpy((void *)RING_AT(off), p, c);
		p += c; n -= c; h += c;
	}
	blk->head = h;
}
void vc_begin(void)
{
	if (!blk) return;
	if (blk->state != A68V_S_IDLE) ctl(110, A68V_C_STOP, 0);   /* the ring emptied: tail = head */
	ctl(114, 0, 0);                                    /* the end cleared */
	started = was_playing = 0;
}
static void tell_place(int force);
static void quiet(int on);
static unsigned vol_now = 0x100;
void vc_go(void)
{
	if (!blk) return;
	tell_place(1);
	ctl(113, (long)(vol_now << 16 | vol_now), 0);
	ctl(110, A68V_C_START, 0);
	started = 1;
	bytes_in = blk->head - blk->tail;                  /* what came with the answer's headers */
	t_begin = LM_TICKS;
	level_min = 0xFFFFFFFFul; level_max = 0; dry_ticks = 0; dry_from = 0; low_ticks = 0; low_from = 0;
}
void vc_pause(int on) { if (blk && started) ctl(110, on ? A68V_C_PAUSE : A68V_C_RESUME, 0); }
void vc_stop(void)
{
	if (!blk) return;
	if (started) vc_report("stopped");
	if (blk->state != A68V_S_IDLE) ctl(110, A68V_C_STOP, 0);
	started = was_playing = 0;
}
void vc_end(void) { if (blk) ctl(114, 1, 0); }
int vc_state(void) { return blk ? (int)blk->state : 0; }
/* what the card holds (a card with its buffer in the Pi, block version 2 and later, docs/08 item 125): the N of "Buffering
   N%" and the seconds held; 0 from an older card */
int vc_buffer(unsigned long *pct, unsigned long *ms)
{
	if (!blk || blk->version < 2) return 0;
	*pct = blk->buffer_pct; *ms = blk->buffer_ms;
	return 1;
}
unsigned long vc_pos_ms(void) { return blk && started ? blk->pos_ms : 0; }
const char *vc_message(void) { return blk ? (const char *)blk->message : ""; }
void vc_volume(int v)
{
	if (v < 0) v = 0;
	if (v > 100) v = 100;
	vol_now = (unsigned)(v * 256 / 100);
	if (blk) ctl(113, (long)(vol_now << 16 | vol_now), 0);
}
/* the numbers this testbed and the owner's SE are judged by: what the network sustained into the ring, the ring's
   level, the card's pictures (shown and not drawn), the times and ticks it ran dry, its decoding and drawing */
void vc_report(const char *why)
{
	char t[200], *p;                                        /* two lines: pi_log takes 118 characters a line */
	unsigned long ticks = LM_TICKS - t_begin, rate;
	if (!blk) return;
	rate = ticks ? bytes_in / ticks * 60 : 0;                 /* bytes a second */
	p = put_str(put_str(t, "video: "), why);
	p = put_str(p, ": "); p = put_num(p, blk->video_w); *p++ = 'x'; p = put_num(p, blk->video_h);
	p = put_str(p, " at "); p = put_num(p, blk->pos_ms / 1000); *p++ = '.'; p = put_num(p, blk->pos_ms / 100 % 10);
	p = put_str(p, " s; network "); p = put_num(p, bytes_in >> 10); p = put_str(p, " KB in ");
	p = put_num(p, ticks / 60); p = put_str(p, " s, "); p = put_num(p, rate * 8 / 1000); p = put_str(p, " kbit/s; ring ");
	p = put_num(p, (blk->head - blk->tail) >> 10); p = put_str(p, " KB (");
	p = put_num(p, level_min == 0xFFFFFFFFul ? 0 : level_min >> 10); *p++ = '-'; p = put_num(p, level_max >> 10);
	p = put_str(p, " KB)");
	*p = 0;
	pi_log(t);
	p = put_str(t, "video: pictures "); p = put_num(p, blk->frames_decoded); p = put_str(p, " decoded, ");
	p = put_num(p, blk->frames_shown); p = put_str(p, " shown, "); p = put_num(p, blk->frames_dropped);
	p = put_str(p, " not drawn; dry "); p = put_num(p, blk->underruns); p = put_str(p, " times, ");
	p = put_num(p, dry_ticks * 1000 / 60); p = put_str(p, " ms; decode "); p = put_num(p, blk->decode_us);
	p = put_str(p, " us, draw "); p = put_num(p, blk->draw_us); p = put_str(p, " us; core 1 ");
	p = put_num(p, blk->load); p = put_str(p, " per mille");
	*p = 0;
	pi_log(t);
	/* the card's "dry" counts a picture that could not be decoded; with sound, the sound is its clock, and a stream
	   that comes too slowly stretches the sound and the pictures with it rather than running dry. So also: how long
	   the card held less than 8 KB while playing (the stream not keeping up), against the film's own time */
	p = put_str(t, "video: under 8 KB in hand for "); p = put_num(p, low_ticks * 50 / 3); p = put_str(p, " ms of ");
	p = put_num(p, ticks * 50 / 3); p = put_str(p, " ms played; the film's time ");
	p = put_num(p, blk->pos_ms); p = put_str(p, " ms");
	*p = 0;
	pi_log(t);
}

/* ---- the screens ----------------------------------------------------------------------------------------- */
/* the screen a global point is on: its rectangle, whether it is the main one (the menu bar), and the card's number
   for it: 0 for slot 9's (the SE's own on the SE board), 1 for slot $A's (the SE board's HDMI screen), from the
   GDevice's driver's slot, as Aurora68 Video tells them apart */
static int screen_of(Point g, Rect *rect, int *main)
{
	void **gd = *(void ***)0x8A8;                      /* DeviceList */
	while (gd && *gd) {
		unsigned char *d = *gd;
		Rect *r = (Rect *)(d + 34);                     /* gdRect */
		if (g.h >= r->left && g.h < r->right && g.v >= r->top && g.v < r->bottom) {
			short ref = *(short *)d;                    /* gdRefNum: the screen's driver */
			void ***ut = *(void ****)0x11C;             /* UTableBase */
			int slot = 9;
			*rect = *r;
			*main = gd == *(void ***)0x8A4;             /* MainDevice */
			if (ref < 0 && ut && ut[~ref] && *ut[~ref]) slot = *((unsigned char *)*ut[~ref] + 40);   /* dCtlSlot */
			return slot == 0xA ? A68V_SCREEN_HDMI : A68V_SCREEN_FIRST;
		}
		gd = *(void ***)(d + 30);                       /* gdNextGD */
	}
	*rect = *RGN_BBOX(LM_GRAYRGN);                      /* no Color QuickDraw: the one screen */
	*main = 1;
	return A68V_SCREEN_FIRST;
}
#define MBAR_H (*(volatile short *)0xBAA)             /* MBarHeight */
#define TITLE_H 20                                     /* a document window's title bar and frame */
/* the strip below the picture (1.8): the size box, which the card's picture would cover inside the picture, and the
   time; a document window's size box is the content's bottom right 15 x 15 */
#define STRIP 15
#define MIN_W 160                                      /* the smallest picture the size box makes */
static Point win_tl(void *w)                           /* a window's content's top left, global */
{
	Rect *b = RGN_BBOX(*(void ***)((char *)w + 118));  /* contRgn, in global coordinates */
	Point p = { b->top, b->left };
	return p;
}
void vc_size_for(void *near_win, unsigned long vw, unsigned long vh, unsigned *w, unsigned *h)
{
	Rect s;
	int main;
	Point g = { 60, 60 };
	if (near_win) g = win_tl(near_win);
	screen_of(g, &s, &main);
	unsigned sw = (unsigned)(s.right - s.left), sh = (unsigned)(s.bottom - s.top);
	unsigned room_w = sw > 16 ? sw - 16 : sw, room_h = sh - (main ? (unsigned)MBAR_H : 0) - TITLE_H - 8 - STRIP;
	a68_vmpeg_size(vw, vh, sw, sh, room_w, room_h, w, h);
}

/* ---- the window ------------------------------------------------------------------------------------------ */
static void *vw;
static int shown, colour_qd, holds;
static unsigned cur_w, cur_h;                          /* the picture as the server sends it */
static int zoom, user_zoom;                            /* VC_*; the last that is not Fill Screen (the zoom box's other state) */
static long custom_w;                                  /* the width the size box gave (1.8), kept in the preferences */
static Point user_tl;                                  /* where the window was (global) before Fill Screen */
static int have_user_tl;
static Rect last_place;                                /* where the last video's window was (global), 0 if none yet */
static char strip_text[48];
void *vc_window(void) { return vw; }
int vc_window_shown(void) { return vw && shown; }
static int on_a_screen(const Rect *r)
{
	Point a = { (short)(r->top + 4), (short)(r->left + 4) };
	return PtInRgn(pt_long(a), LM_GRAYRGN);
}
/* the content, global: from the port (a window not yet shown has an empty contRgn, and 1.7's Double Size for a new
   window took that for a window at 0,0 and moved it to the screen's top left corner; seen in the testbed, 1.8) */
static Rect content_g(void)
{
	void *port;
	Rect c = *PORT_RECT(vw);
	Point tl = { c.top, c.left }, br = { c.bottom, c.right };
	GetPort(&port);
	SetPort(vw);
	LocalToGlobal(&tl); LocalToGlobal(&br);
	SetPort(port);
	c.top = tl.v; c.left = tl.h; c.bottom = br.v; c.right = br.h;
	return c;
}
static Rect pic_g(void) { Rect c = content_g(); c.bottom = (short)(c.bottom - STRIP); return c; }
static void quiet(int on);
/* the largest picture of the video's shape on screen s, below the menu bar on the main one, with the title bar and the
   strip */
static void fill_size(const Rect *s, int main, long *fw, long *fh)
{
	long rw = s->right - s->left - 4, rh = s->bottom - s->top - (main ? MBAR_H : 0) - TITLE_H - 2 - STRIP;
	if ((long)cur_w * rh >= (long)cur_h * rw) { *fw = rw; *fh = (long)cur_h * rw / (long)cur_w; }
	else { *fh = rh; *fw = (long)cur_w * rh / (long)cur_h; }
}
/* the picture's size for z on screen s */
static void zoom_size(int z, const Rect *s, int main, long *W, long *H)
{
	long fw, fh;
	fill_size(s, main, &fw, &fh);
	*W = (long)cur_w; *H = (long)cur_h;
	if (z == VC_DOUBLE) { *W *= 2; *H *= 2; }
	else if (z == VC_CUSTOM && custom_w) { *W = custom_w < MIN_W ? MIN_W : custom_w; *H = *W * (long)cur_h / (long)cur_w; }
	if (z == VC_FILL || *W > fw || *H > fh) { *W = fw; *H = fh; }
	if (*W < 64) *W = 64;
	if (*H < 48) *H = 48;
}
/* the window at its size for zoom: the size, and for Fill Screen the place, on the screen it is on now */
static void apply_zoom(void)
{
	Rect s, c;
	int main;
	long W, H, left, top;
	if (!vw || !cur_w || !cur_h) return;
	c = content_g();
	{ Point tl = { c.top, c.left }; screen_of(tl, &s, &main); }
	zoom_size(zoom, &s, main, &W, &H);
	left = c.left; top = c.top;
	if (zoom == VC_FILL) {
		if (!have_user_tl) { user_tl.v = c.top; user_tl.h = c.left; have_user_tl = 1; }
		left = s.left + (s.right - s.left - W) / 2; top = s.top + (main ? MBAR_H : 0) + TITLE_H;
	} else if (have_user_tl) {                         /* back from Fill Screen: where it was */
		Rect b = { user_tl.v, user_tl.h, (short)(user_tl.v + 8), (short)(user_tl.h + 8) };
		if (on_a_screen(&b)) { left = user_tl.h; top = user_tl.v; }
		have_user_tl = 0;
	}
	if (left + W > s.right - 2) left = s.right - 2 - W;
	if (top + H + STRIP > s.bottom - 2) top = s.bottom - 2 - H - STRIP;
	if (left < s.left + 2) left = s.left + 2;
	if (top < s.top + (main ? MBAR_H : 0) + TITLE_H) top = s.top + (main ? MBAR_H : 0) + TITLE_H;
	quiet(1);
	if (c.right - c.left != W || c.bottom - c.top != H + STRIP) SizeWindow(vw, (short)W, (short)(H + STRIP), 1);
	if (left != c.left || top != c.top) MoveWindow(vw, (short)left, (short)top, 0);
	quiet(0);
	SetPort(vw);
	InvalRect(PORT_RECT(vw));
	tell_place(1);
}
static void say_size(const char *how)
{
	char t[100], *p;
	Rect c;
	if (!vw) return;
	c = pic_g();
	p = put_str(put_str(t, "video: the window at "), how);
	p = put_str(p, ", the picture "); p = put_num(p, (unsigned long)(c.right - c.left)); *p++ = 'x'; p = put_num(p, (unsigned long)(c.bottom - c.top));
	*p = 0;
	pi_log(t);
}
void vc_set_zoom(int z)
{
	if (z < VC_NORMAL || z > VC_CUSTOM) z = VC_NORMAL;
	if (z == VC_CUSTOM && !custom_w) z = VC_NORMAL;
	zoom = z;
	if (z != VC_FILL) user_zoom = z;
	apply_zoom();
	say_size(z == VC_FILL ? "Fill Screen" : z == VC_DOUBLE ? "Double Size" : z == VC_CUSTOM ? "the size chosen with its size box" : "Normal Size");
}
int vc_zoom(void) { return zoom; }
void vc_zoom_box(void) { vc_set_zoom(zoom == VC_FILL ? user_zoom : VC_FILL); }
long vc_custom_w(void) { return custom_w; }
void vc_set_custom_w(long w) { custom_w = w >= MIN_W && w <= 4096 ? w : 0; }
/* the size box (1.8): GrowWindow's outline, the card not drawing meanwhile; then the picture of the video's own shape
   nearest to what was drawn (by the side dragged further, for its size), the window's content snapped to it, at least
   MIN_W wide and at most the screen's Fill Screen; the card told the new place, and the size kept for the next video */
void vc_grow(long where)
{
	Rect s, c;
	int main;
	long fw, fh, pw, ph, cw, ch, W, H, dw, dh, sz;
	if (!vw || !cur_w || !cur_h) return;
	c = pic_g();
	{ Point tl = { c.top, c.left }; screen_of(tl, &s, &main); }
	fill_size(&s, main, &fw, &fh);
	{
		long minh = MIN_W * (long)cur_h / (long)cur_w;
		Rect lim = { (short)(minh + STRIP), MIN_W, (short)(s.bottom - s.top), (short)(s.right - s.left) };
		vc_hold(1);
		sz = GrowWindow(vw, where, &lim);
		vc_hold(0);
	}
	if (!sz) return;
	pw = sz & 0xFFFF; ph = (sz >> 16 & 0xFFFF) - STRIP;
	cw = c.right - c.left; ch = c.bottom - c.top;
	dw = (pw > cw ? pw - cw : cw - pw) * ch;          /* the change of each side, for its size: the one dragged further */
	dh = (ph > ch ? ph - ch : ch - ph) * cw;
	if (dw >= dh) { W = pw; H = W * (long)cur_h / (long)cur_w; }
	else { H = ph; W = H * (long)cur_w / (long)cur_h; }
	if (W < MIN_W) { W = MIN_W; H = W * (long)cur_h / (long)cur_w; }
	if (W > fw || H > fh) { W = fw; H = fh; }
	custom_w = W;
	zoom = user_zoom = W == (long)cur_w ? VC_NORMAL : VC_CUSTOM;
	have_user_tl = 0;
	quiet(1);
	SizeWindow(vw, (short)W, (short)(H + STRIP), 1);
	{   /* still on its screen, the strip and the size box too */
		long left = c.left, top = c.top;
		if (left + W > s.right - 2) left = s.right - 2 - W;
		if (top + H + STRIP > s.bottom - 2) top = s.bottom - 2 - H - STRIP;
		if (left < s.left + 2) left = s.left + 2;
		if (top < s.top + (main ? MBAR_H : 0) + TITLE_H) top = s.top + (main ? MBAR_H : 0) + TITLE_H;
		if (left != c.left || top != c.top) MoveWindow(vw, (short)left, (short)top, 0);
	}
	quiet(0);
	SetPort(vw);
	InvalRect(PORT_RECT(vw));
	tell_place(1);
	say_size("the size of its size box");
}
void vc_window_open(const char *title, unsigned w, unsigned h, void *near)
{
	Rect b;
	unsigned char t[64];
	int n = 0;
	while (title[n] && n < 63) { t[n + 1] = (unsigned char)title[n]; n++; }
	t[0] = (unsigned char)n;
	strip_text[0] = 0;
	if (!vw) {
		Rect s;
		int main;
		Point g = { 60, 60 };
		Rect nr = { 0, 0, 0, 0 };
		short wh = (short)(h + STRIP);
		if (!near) near = FrontWindow();
		if (near) { g = win_tl(near); nr = *RGN_BBOX(*(void ***)((char *)near + 114)); }   /* its strucRgn */
		screen_of(g, &s, &main);
		short top = (short)(s.top + (main ? MBAR_H : 0) + TITLE_H + 4);
		short left = (short)(s.left + ((s.right - s.left) - (short)w) / 2);
		/* below the player window where the screen has room for it (a 640 x 480 screen does), so that neither covers
		   the other; on the SE's 512 x 342 it goes at the top, over the player window, which a click brings back */
		if (near && nr.bottom + TITLE_H + 4 + wh + 4 <= s.bottom) {
			top = (short)(nr.bottom + TITLE_H + 4);
			if (nr.left + (short)w + 4 <= s.right) left = nr.left;
		}
		if (left < s.left + 4) left = (short)(s.left + 4);
		b.top = top; b.left = left; b.bottom = (short)(top + wh); b.right = (short)(left + w);
		if (last_place.bottom) {                       /* where the last one was, if this one fits there on a screen */
			Rect l = { last_place.top, last_place.left, (short)(last_place.top + wh - 8), (short)(last_place.left + w - 8) };
			Rect br = { (short)(l.bottom), (short)(l.right), 0, 0 };
			if (on_a_screen(&l) && on_a_screen(&br)) { b.top = l.top; b.left = l.left; b.bottom = (short)(b.top + wh); b.right = (short)(b.left + w); }
		}
		colour_qd = !(*(volatile unsigned short *)0x28E & 0x4000);
		/* 8: zoomDocProc, a document window with a zoom box and a size box (1.8; 1.7's had no size box): the zoom box
		   toggles Fill Screen and the size it had */
		vw = colour_qd ? (void *)NewCWindow(0, &b, t, 0, 8, (void *)-1, 1, 0)
		               : (void *)NewWindow(0, &b, t, 0, 8, (void *)-1, 1, 0);
		if (!vw) return;
		cur_w = w; cur_h = h;
		have_user_tl = 0;
		if (zoom != VC_NORMAL) apply_zoom();
	} else {
		SetWTitle(vw, t);
		if (w != cur_w || h != cur_h) {
			cur_w = w; cur_h = h;
			if (zoom != VC_NORMAL) apply_zoom();
			else { quiet(1); SizeWindow(vw, (short)w, (short)(h + STRIP), 1); quiet(0); }
		}
	}
	ShowWindow(vw);
	SelectWindow(vw);
	shown = 1;
	SetPort(vw);
	InvalRect(PORT_RECT(vw));
	tell_place(1);
}
/* the card told to stop drawing before the Window Manager paints what a window uncovers: the desktop is painted at
   once, by no one's update event, and a picture the card drew after that would stay on it (seen in the testbed: a
   corner of the picture left on the desktop where a closed window had been) */
static void quiet(int on) { holds += on ? 1 : -1; tell_place(0); }
void vc_window_hide(void)
{
	if (!vw || !shown) return;
	quiet(1);
	HideWindow(vw);
	shown = 0;
	quiet(0);
	pi_log("video: the window closed: the picture hidden, the sound goes on (Controls > Show Video)");
}
void vc_window_show(void)
{
	if (!vw || shown) return;
	ShowWindow(vw);
	SelectWindow(vw);
	shown = 1;
	tell_place(1);
}
void vc_window_dispose(void)
{
	if (!vw) return;
	quiet(1);
	DisposeWindow(vw);
	vw = 0;
	shown = 0;
	have_user_tl = 0;
	quiet(0);
	tell_place(1);
}
/* the strip: a line along its top, the time at its left, the size box (alone: no scroll bars, so no lines along the
   window's edges) */
static void draw_strip(int all)
{
	Rect p = *PORT_RECT(vw), st = p, tr, box, wide = { -32000, -32000, 32000, 32000 };
	st.top = (short)(p.bottom - STRIP);
	tr = st; tr.top++; tr.right = (short)(st.right - 16);
	if (all) {
		EraseRect(&st);
		MoveTo(st.left, st.top); LineTo(st.right, st.top);
	} else EraseRect(&tr);
	TextFont(3); TextSize(9); TextFace(0);
	MoveTo(6, (short)(st.bottom - 4));
	DrawText(strip_text, 0, (int)strlen(strip_text));
	if (all) {
		box.top = (short)(p.bottom - 15); box.left = (short)(p.right - 15); box.bottom = p.bottom; box.right = p.right;
		ClipRect(&box);
		DrawGrowIcon(vw);
		ClipRect(&wide);
	}
}
void vc_update(void)
{
	void *port;
	Rect pic;
	if (!vw) return;
	GetPort(&port);
	SetPort(vw);
	pic = *PORT_RECT(vw);
	pic.bottom = (short)(pic.bottom - STRIP);
	PaintRect(&pic);                                   /* black: the card draws its picture over it */
	draw_strip(1);
	SetPort(port);
	tell_place(1);                                     /* a paused picture drawn again by the card */
}
void vc_activate(int on)
{
	void *port;
	(void)on;
	if (!vw || !shown) return;
	GetPort(&port);
	SetPort(vw);
	draw_strip(1);                                     /* the size box drawn, or left empty when the window is not in front */
	SetPort(port);
}
/* the strip's words (library.c, once a second: "0:46 of 3:49") */
void vc_strip(const char *t)
{
	void *port;
	int i;
	for (i = 0; t[i] && t[i] == strip_text[i]; i++) { }
	if (!t[i] && !strip_text[i]) return;
	for (i = 0; t[i] && i < (int)sizeof strip_text - 1; i++) strip_text[i] = t[i];
	strip_text[i] = 0;
	if (!vw || !shown) return;
	GetPort(&port);
	SetPort(vw);
	draw_strip(0);
	SetPort(port);
}
void vc_hold(int on) { holds += on ? 1 : -1; if (holds < 0) holds = 0; tell_place(0); }

/* The stream's size on the HDMI screen (1.8.1, docs/08 item 127). Until 1.8 the size came from the screen the player
   window was on (a68_vmpeg_size): the owner's 720p video came at 320 x 212 because the player window was on the SE's
   screen, and the card then scaled it up into a large video window on the 2560 x 1440 HDMI screen. Now, when the video
   window is on the HDMI screen (slot $A's GDevice), the stream fits the window's picture (Video Quality > Automatic) or
   the width chosen, 960 x 540 at most (a68_vmpeg_hdmi); library.c asks again for a larger one when the window grows.
   The window's sizes (Normal, Double, the size box) stay those of the size it was opened at; the card scales the
   stream into it. On the SE's screen nothing changes: 0 here, and the size by today's rule */
static int quality;                                    /* 0: Automatic; else the width chosen */
void vc_set_quality(int q) { quality = q == 320 || q == 480 || q == 640 || q == 960 ? q : 0; }
int vc_quality(void) { return quality; }
int vc_hdmi_size(unsigned long vidw, unsigned long vidh, unsigned *w, unsigned *h)
{
	Rect s, c;
	int main;
	if (!vw || !shown || holds) return 0;              /* not while it is dragged or sized */
	c = pic_g();
	{ Point tl = { c.top, c.left }; if (screen_of(tl, &s, &main) != A68V_SCREEN_HDMI) return 0; }
	a68_vmpeg_hdmi(vidw, vidh, (unsigned)(c.right - c.left), (unsigned)(c.bottom - c.top), (unsigned)quality, w, h);
	return 1;
}

/* The card is told the picture's place and, since 1.8 (docs/08 item 124), the window's visible region: the card reads
   that region at every picture and draws only where the window shows. 1.6 and 1.7 told the card "not visible" whenever
   a window was in front of the video's (a corner covered hid the whole picture) and whenever another program was in
   front; the owner: "Video in the window stops playing if it isn't focused. This should work in the background". The
   Window Manager keeps every window's visRgn current, another program's windows included, and the card reads it as
   it changes, also while Play gets no time. Still not drawn: while the window is hidden, while Play's own menu is down
   or its window is dragged or sized or a dialog of Play's is up (the holds; the outlines and menus are drawn over the
   screen, outside any window's region), and while any program's menu is down (the card looks at TheMenu itself) */
/* a kernel whose card is older than the region (its block's version 1 or 2): 1.7's rule, the picture drawn only with Play in
   front and no window over it (covered: a visible window in front of ours, in the Window Manager's list from the
   front, whose structure meets the picture; the card draws a rectangle, so a corner covered hides the whole picture) */
static int fg = 1;
static int covered(const Rect *c)
{
	unsigned char *w = *(unsigned char **)0x9D6;      /* WindowList */
	for (; w && w != vw; w = *(unsigned char **)(w + 144)) {    /* nextWindow */
		Rect *s;
		if (!w[110]) continue;                         /* visible */
		s = RGN_BBOX(*(void ***)(w + 114));            /* strucRgn */
		if (s->left < c->right && s->right > c->left && s->top < c->bottom && s->bottom > c->top) return 1;
	}
	return 0;
}
static struct { long screen, top, left, bottom, right, visible; } place, sent;
static void tell_place(int force)
{
	Rect c, s;
	int main;
	if (!blk) return;
	memset(&place, 0, sizeof place);
	if (vw && shown) {
		c = pic_g();
		Point tl = { c.top, c.left };
		place.screen = screen_of(tl, &s, &main);
		place.top = c.top - s.top; place.left = c.left - s.left; place.bottom = c.bottom - s.top; place.right = c.right - s.left;
		place.visible = !holds && (blk->version >= 3 || (fg && !covered(&c)));
		last_place = content_g();                      /* where the next video's window opens, if still on a screen */
	}
	if (!force && !memcmp(&place, &sent, sizeof place)) return;
	sent = place;
	if (vw && shown && blk->version >= 3) {            /* the region, for the geom_seq this Control 112 makes */
		blk->vis_rgn = (unsigned long)*(void **)((char *)vw + 24);    /* visRgn (a GrafPort's and a CGrafPort's) */
		blk->vis_flags = LM_MMU32BIT ? 0 : A68V_VF_24BIT;
		blk->clip_tag = A68V_CLIP_TAG + blk->geom_seq + 1;
	} else {
		blk->clip_tag = 0;
		blk->vis_rgn = 0;
	}
	ctl(112, (long)&place, 0);
}
void vc_poll(int foreground)
{
	fg = foreground;                                   /* 1.8: only for a card without the region */
	if (!blk) return;
	tell_place(0);
	if (!started) return;
	{   /* the numbers: the ring's level while playing, and how long it was dry after the first picture */
		unsigned long lv = blk->head - blk->tail, st = blk->state;
		if (st == A68V_S_PLAYING) {
			was_playing = 1;
			if (lv < level_min) level_min = lv;
			if (lv > level_max) level_max = lv;
			if (dry_from) { dry_ticks += LM_TICKS - dry_from; dry_from = 0; }
		} else if (st == A68V_S_BUFFERING && was_playing && !dry_from) dry_from = LM_TICKS;
		if (st == A68V_S_PLAYING && blk->buffered < 8192) { if (!low_from) low_from = LM_TICKS; }
		else if (low_from) { low_ticks += LM_TICKS - low_from; low_from = 0; }
	}
}
