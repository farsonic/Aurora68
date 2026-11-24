/* SPDX-License-Identifier: MPL-2.0
 *
 * mac/a68apple/apple.c - Aurora68 Apple II: the 8-bit Apple II on the Aurora68 Apple II Card (slot $E) in a window on
 * the Mac's desktop (docs/20-apple-ii-card.md; Aurora65's docs/DESIGN.md, "The card in Aurora68").
 *
 * Backstory (23 November 2026). The owner asked for the Apple II family beside the PC card, "8-bit first", in a Mac
 * window, 1:1 on the SE's screen. The Apple (Aurora65: our own 6502, the II, II+, IIe, enhanced IIe and IIc) runs on a
 * core of the Pi, and the card draws its screen into the Mac's frame buffer, inside this program's window and only where
 * the window shows (the card reads the window's visRgn through the Mac's memory, as for Aurora68 PC). So this program
 * does what Aurora68 PC does (mac/a68pc/pc.c, whose habits it shares): it says where the window's content is and hands
 * over its visRgn whenever the window moves, is resized, shown or brought forward; on an update event it asks the card
 * to draw it all again; its menus power the Apple on and off, reset it, choose the model and the disks. Unlike the PC,
 * the Apple takes the keyboard whenever its window is in front (an Apple II has no mouse of its own to capture): keys
 * typed go to it as the Mac's character and key codes (the card makes them the Apple's: Command is Open-Apple, Option
 * Solid-Apple, Delete the IIe's DEL), Command-keys to the menus unless the keyboard is captured.
 *
 *  - Reset: Apple II > Reset, or Control-Command-Delete in the window (the Apple's Control-Reset); Shift too, or
 *    Apple II > Restart Cold, for a cold start (memory scrambled, as Control-Open-Apple-Reset).
 *  - Capture: Keyboard > Capture, a click in the window, or Control-Option-Command-A: Command-keys go to the Apple too
 *    (Open-Apple-letter for its programs), and the mouse is the joystick (paddles 0 and 1 from the pointer's place in
 *    the window, button 0 the mouse button). The same key, a Command-click or the window going to the back end it.
 *  - Front: the window's activate and the program's suspend and resume set the block's front word, and the guest
 *    scheduler gives the front window's machine the core first (psmac_guest.c), the PC's or the Apple's.
 *  - The window: a document window with a grow box and a zoom box, the Apple's picture at its natural size (the card
 *    publishes it: 284 x 192 for 40 columns, lores and hires at 1:1 on the SE's screen, 484 x 192 for 80 columns, 560 x
 *    384 on a colour screen); View > Scaled to the Window lets it be any size. It fits the SE's 512 x 342 screen with
 *    the menu bar (mac/c/screens.h's scr_fit). The card leaves the grow box's corner alone (A68A_O_GROWBOX).
 *  - Disks (24 November 2026, disks.c): the card's own images, and disks from the Mac: Disks > Insert Disk... (any Mac
 *    volume), the units' items, images dropped on the program's icon, the "Apple II Disks" folder beside the program as
 *    submenus; the card asks for their bytes and this program answers from every loop it runs (docs/20 "Disks from the
 *    Mac"). The program is high-level-event aware for that (SIZE flags 0x58C0, the Makefile).
 *  - Repairs (24 November 2026, 1.3): a Finder window dragged over the busy Apple's window on the owner's SE kept a
 *    rectangle of the Apple's screen; when the card finds it drew outside the window as the window is now, it asks, and
 *    this program has the Mac paint that again (../a68pc/repair.h: PaintBehind), in the background too (beat()).
 * Human Interface Guidelines (1992); System 6 and 7.
 */
#include "apple.h"
#include "screens.h"
#include "../a68pc/place.h"
#include "../a68pc/repair.h"

short drv_open(void *pb), drv_control(void *pb), drv_status(void *pb);

#define LM_SYSEVTMASK (*(volatile short *)0x144)
#define LM_MOUSE      (*(volatile Point *)0x830)
#define LM_MBSTATE    (*(volatile unsigned char *)0x172)   /* the mouse button: bit 7 clear while it is down */
#define keyUpMask     0x0010
#define controlKey    0x1000
#define VK_A          0x00
#define VK_DELETE     0x33

enum { M_APPLE = 128, M_FILE, M_EDIT, M_A2, M_DISKS, M_VIEW, M_KEYS };
_Static_assert(M_DISKS == M_DISKS_ID, "the Disks menu's ID, as disks.c knows it");
static void *m_apple, *m_file, *m_edit, *m_a2, *m_view, *m_keys;
void *m_disks;
static void *win;
unsigned char pbuf[256];
void disks_idle(void);
#define A2_MODEL_FIRST 8                  /* the Apple II menu's first model item */

/* ---- the card ------------------------------------------------------------------------------------- */
typedef struct {
	void *qLink; short qType, ioTrap; void *ioCmdAddr, *ioCompletion; short ioResult;
	const unsigned char *ioNamePtr; short ioVRefNum, ioCRefNum, csCode;
	long csParam[6];
} CntrlParam;
static short drv;
volatile unsigned char *card;
volatile struct a68a_block *blk;

static short ctl4(short code, long a, long b, long c, long d)
{
	CntrlParam pb;
	memset(&pb, 0, sizeof pb);
	pb.ioCRefNum = drv; pb.csCode = code; pb.csParam[0] = a; pb.csParam[1] = b; pb.csParam[2] = c; pb.csParam[3] = d;
	return drv_control(&pb);
}
static short ctl(short code, long a, long b, long c) { return ctl4(code, a, b, c, 0); }
static int card_open(void)
{
	CntrlParam pb;
	memset(&pb, 0, sizeof pb);
	pb.ioNamePtr = (const unsigned char *)"\x06.A68A2";
	if (drv_open(&pb)) return 0;
	drv = pb.ioCRefNum;
	memset(&pb, 0, sizeof pb);
	pb.ioCRefNum = drv; pb.csCode = 100;
	if (drv_status(&pb) || !pb.csParam[0]) return 0;
	card = (volatile unsigned char *)pb.csParam[0];
	blk = (volatile struct a68a_block *)card;
	return blk->magic == A68A_MAGIC;
}
void command(unsigned long c, unsigned long a0, unsigned long a1) { if (drv) ctl(100, (long)c, (long)a0, (long)a1); }
/* Alive and gone (23 November 2026; Aurora68 PC's fix, docs/19-pc-card.md "The owner's SE": the PC card drew through a quit
   program's freed visRgn handle, and this card was made after its pattern). alive + 1 at every turn of an event loop (the
   card draws only while it moves); at the end prog = A68A_PROG_GONE, the region taken back, A68A_C_NOWINDOW; and a patch
   on ExitToShell says GONE on any other way out (a crash the system survives, Force Quit), then puts the trap back */
/* Repairs (24 November 2026, Aurora68 Apple II 1.3; docs/20 "Drawn over another window"): at every beat, any request of
   the card's to have the Mac paint again what it drew outside the window as the window is now (repair.h) */
void beat(void)
{
	if (!blk) return;
	blk->alive = blk->alive + 1;
	if (drv) rp_take((volatile struct a68_repair *)(card + A68A_OFF_REPAIR), &blk->repair_done, win);
}
static void gone(void)
{
	if (!blk) return;
	blk->prog = A68A_PROG_GONE;
	blk->focus = 0;
	blk->front = 0;
	blk->visible = 0;
	blk->clip_tag = 0;
	blk->vis_rgn = 0;
	blk->geom_seq = blk->geom_seq + 1;
}
unsigned long a2_ets_old __attribute__((used));
void a68apple_ets_gone(void)
{
	if (a2_ets_old) {
		unsigned long o = a2_ets_old;
		__asm__ volatile("move.l %0,%%a0\n\tmove.w #0xA9F4,%%d0\n\t.short 0xA647" :: "g"(o) : "d0", "d1", "d2", "a0", "a1", "memory");
	}
	gone();
}
__asm__("	.text\n"
        "	.globl	a68apple_ets_patch\n"
        "a68apple_ets_patch:\n"
        "	movem.l	%d0-%d2/%a0-%a1,-(%sp)\n"
        "	jsr	a68apple_ets_gone\n"
        "	movem.l	(%sp)+,%d0-%d2/%a0-%a1\n"
        "	move.l	a2_ets_old,-(%sp)\n"
        "	rts\n");
static void ets_patch(void)
{
	unsigned long o;
	__asm__ volatile("move.w #0xA9F4,%%d0\n\t.short 0xA746\n\tmove.l %%a0,%0" : "=g"(o) :: "d0", "d1", "d2", "a0", "a1", "memory");
	if (!o) return;
	a2_ets_old = o;
	__asm__ volatile("lea a68apple_ets_patch,%%a0\n\tmove.w #0xA9F4,%%d0\n\t.short 0xA647" ::: "d0", "d1", "d2", "a0", "a1", "memory");
}

/* ---- the window's place ---------------------------------------------------------------------------- */
static int hidden_by_us, captured, scaled, applesense, colour, quit, front;
static int sized_by_user;               /* the window keeps the Apple's natural size until the user resizes or zooms it */
static struct { long screen, top, left, bottom, right, visible; } place, sent;
static struct { long scale, sx, sy, options; } view, sent_view;
static void tell_place(int force)
{
	if (!win || !drv) return;
	Rect r = *PORT_RECT(win);
	Point tl = { r.top, r.left }, br = { r.bottom, r.right };
	SetPort(win);
	LocalToGlobal(&tl); LocalToGlobal(&br);
	struct pl_rect c = { tl.v, tl.h, br.v, br.h };
	struct pl_place pp;
	pl_place_of(&c, &pp, 0);                        /* the screen holding most of the window (Aurora68 PC's place.h) */
	place.screen = pp.screen; place.top = pp.top; place.left = pp.left; place.bottom = pp.bottom; place.right = pp.right;
	place.visible = !hidden_by_us && !quit;
	view.scale = scaled ? A68A_SCALE_FIT : A68A_SCALE_1; view.sx = view.sy = 0;
	view.options = A68A_O_GROWBOX | (applesense ? A68A_O_APPLESENSE : 0) | (colour ? A68A_O_COLOUR : 0);
	if (force || memcmp(&view, &sent_view, sizeof view)) { sent_view = view; ctl4(114, view.scale, view.sx, view.sy, view.options); force = 1; }
	if (!force && !memcmp(&place, &sent, sizeof place)) return;
	sent = place;
	/* the window's visible region, read by the card at every picture: written before the Control 112 that goes with it */
	blk->vis_rgn = (unsigned long)*(void **)((char *)win + 24);    /* visRgn */
	blk->vis_flags = LM_MMU32BIT ? 0 : A68A_VF_24BIT;
	blk->clip_tag = A68A_CLIP_TAG + blk->geom_seq + 1;
	ctl(112, (long)&place, 0, 0);
}
/* in front or not: the scheduler's word, and the keyboard's */
static void set_front(int on)
{
	front = on;
	if (!drv) return;
	blk->front = (unsigned long)on;
	ctl(115, on, 0, 0);                             /* focus: the Apple takes the keys while its window is in front */
}

/* ---- the title and the menus ------------------------------------------------------------------------- */
static char *put(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static unsigned long shown_state = ~0u, shown_model = ~0u;
static int shown_capture = -1;
static const char *const model_names[A68A_MODELS] = { "Apple ][", "Apple ][+", "Apple //e", "Apple //e Enhanced", "Apple //c" };
static void title(void)
{
	unsigned long st = drv ? blk->state : 99, mo = drv ? blk->model : 99;
	if (st == shown_state && captured == shown_capture && mo == shown_model) return;
	shown_state = st; shown_capture = captured; shown_model = mo;
	char t[100], *p = put(t, mo < A68A_MODELS ? model_names[mo] : "Aurora68 Apple II");
	const char *w = st == A68A_S_OFF ? " (off)" : st == A68A_S_STARTING ? " (starting)" : st == A68A_S_PAUSED ? " (paused)" :
	                st == A68A_S_HALTED ? " (halted)" : st == A68A_S_FAILED ? " (stopped)" : st == 99 ? " (no card)" : "";
	p = put(p, w);
	if (captured) p = put(p, ": keyboard and joystick");
	*p = 0;
	SetWTitle(win, pstr(pbuf, t));
}
static void fix_menus(void)
{
	unsigned long st = drv ? blk->state : 99;
	int on = st == A68A_S_RUNNING || st == A68A_S_PAUSED || st == A68A_S_HALTED;
	if (drv && !on && st != A68A_S_STARTING) EnableItem(m_a2, 1); else DisableItem(m_a2, 1);
	for (short i = 2; i <= 5; i++) { if (drv && on) EnableItem(m_a2, i); else DisableItem(m_a2, i); }
	CheckItem(m_a2, 5, st == A68A_S_PAUSED);
	for (short i = 0; i < (short)A68A_MODELS; i++) {
		if (drv) EnableItem(m_a2, (short)(A2_MODEL_FIRST + i)); else DisableItem(m_a2, (short)(A2_MODEL_FIRST + i));
		CheckItem(m_a2, (short)(A2_MODEL_FIRST + i), drv && blk->model == (unsigned long)i);
	}
	CheckItem(m_view, 1, !scaled); CheckItem(m_view, 2, scaled);
	CheckItem(m_view, 4, applesense); CheckItem(m_view, 5, colour);
	CheckItem(m_keys, 1, captured);
}
/* the card's answer when it is not what was asked for (an image a drive refused, a model without its ROM), in a box like
   the About box's, its words wrapped at 52 characters */
void notice(const volatile char *text)
{
	Rect b = { 90, 56, 200, 456 };
	void *w = (void *)NewWindow(0, &b, pstr(pbuf, ""), 1, 1 /* dBoxProc */, (void *)-1, 0, 0);
	SetPort(w);
	short y = 24;
	unsigned i = 0;
	while (text[i] && y <= 72) {
		char line[60]; unsigned n = 0, cut = 0;
		while (text[i + n] && n < 52) { line[n] = text[i + n]; if (line[n] == ' ') cut = n; n++; }
		if (text[i + n] && cut) n = cut;
		line[n] = 0;
		MoveTo(16, y); DrawString(pstr(pbuf, line));
		i += n;
		while (text[i] == ' ') i++;
		y = (short)(y + 16);
	}
	MoveTo(16, 98); DrawString(pstr(pbuf, "Click to continue."));
	EventRecord ev;
	for (;;) { beat(); disks_serve(); if (WaitNextEvent(1 << mouseDown | 1 << keyDown, &ev, disks_sleep(30), 0) && (ev.what == mouseDown || ev.what == keyDown)) break; }
	DisposeWindow(w);
	if (win) SetPort(win);
}
static void model_chosen(unsigned m)
{
	if (!drv || m >= A68A_MODELS || blk->model == m) return;
	command(A68A_C_MODEL, m, 0);
	if (blk->model != m && blk->message[0]) notice(blk->message);  /* no ROM for it on the card: the card says so */
}

/* ---- the keyboard and the joystick ----------------------------------------------------------------- */
static short last_mods;
static void send(unsigned type, unsigned code, unsigned ch, short mods, short dx, short dy, unsigned buttons)
{
	struct a68a_in e;
	memset(&e, 0, sizeof e);
	e.type = (unsigned short)type; e.code = (unsigned short)code; e.modifiers = (unsigned short)mods;
	e.dx = (short)(dx ? dx : (short)ch); e.dy = dy; e.buttons = (unsigned short)buttons;
	ctl(113, (long)&e, 0, 0);
}
static void modifiers(short m)
{
	if ((m ^ last_mods) & (cmdKey | optionKey)) send(A68A_IN_MODS, 0, 0, m, 0, 0, 0);   /* Open-Apple, Solid-Apple */
	last_mods = m;
}
static struct { short x, y; unsigned b; } joy = { -1, -1, 0 };
static void joystick(void)
{
	Rect r = *PORT_RECT(win);
	Point p = LM_MOUSE;
	SetPort(win);
	GlobalToLocal(&p);
	long w = r.right - r.left, h = r.bottom - r.top;
	if (w < 2 || h < 2) return;
	long x = (p.h - r.left) * 255 / (w - 1), y = (p.v - r.top) * 255 / (h - 1);
	if (x < 0) x = 0;
	if (x > 255) x = 255;
	if (y < 0) y = 0;
	if (y > 255) y = 255;
	unsigned b = !(LM_MBSTATE & 0x80);
	if (x == joy.x && y == joy.y && b == joy.b) return;
	joy.x = (short)x; joy.y = (short)y; joy.b = b;
	send(A68A_IN_JOY, 0, 0, last_mods, (short)x, (short)y, b);
}
static void capture(int on)
{
	if (!drv || on == captured) return;
	captured = on;
	if (!on) send(A68A_IN_RELEASE, 0, 0, 0, 0, 0, 0);
	joy.x = joy.y = -1;
	title();
}
/* Control-Option-Command-A: capture; Control-Command-Delete: the Apple's Control-Reset (Shift too: cold) */
static int special_key(EventRecord *ev)
{
	unsigned k = (unsigned)((ev->message >> 8) & 0xFF);
	short m = ev->modifiers;
	if (k == VK_A && (m & (cmdKey | optionKey | controlKey)) == (cmdKey | optionKey | controlKey)) {
		if (ev->what == keyDown) capture(!captured);
		return 1;
	}
	if (k == VK_DELETE && (m & (cmdKey | controlKey)) == (cmdKey | controlKey)) {
		if (ev->what == keyDown) command(A68A_C_RESET, (m & shiftKey) ? 1 : 0, 0);
		return 1;
	}
	return 0;
}

/* ---- the window ----------------------------------------------------------------------------------- */
static void natural(short *w, short *h)
{
	long nw = drv ? (long)blk->nat_w : 0, nh = drv ? (long)blk->nat_h : 0;
	if (nw < 100 || nh < 100) { nw = 284; nh = 192; }
	Rect s[SCR_MAX], room;
	if (win) { int m; Rect c = scr_content(win), sc = scr_of(&c, &m); room = scr_room(&sc, m); }
	else { scr_list(s); room = scr_room(&s[0], 1); }
	if (nw > room.right - room.left + 2) nw = room.right - room.left + 2;
	if (nh > room.bottom - room.top + 2) nh = room.bottom - room.top + 2;
	*w = (short)nw; *h = (short)nh;
}
static void draw_window(void)
{
	Rect r = *PORT_RECT(win);
	SetPort(win);
	if (!drv) {                                     /* no card: three lines, and nothing else to do */
		EraseRect(&r);
		MoveTo(10, 30); DrawString(pstr(pbuf, "The Aurora68 Apple II Card does"));
		MoveTo(10, 48); DrawString(pstr(pbuf, "not answer. It is in slot $E of a"));
		MoveTo(10, 66); DrawString(pstr(pbuf, "Pi built with it (a2=on)."));
	}
	DrawGrowIcon(win);                              /* the card leaves the grow box's corner alone */
}
static void keep_on_screen(void)
{
	Rect c = scr_content(win), f = c;
	if (scr_fit(&f)) {
		MoveWindow(win, f.left, f.top, 0);
		if (f.right - f.left != c.right - c.left || f.bottom - f.top != c.bottom - c.top)
			SizeWindow(win, (short)(f.right - f.left), (short)(f.bottom - f.top), 1);
	}
}
static void resized(void)
{
	SetPort(win);
	InvalRect(PORT_RECT(win));
	tell_place(1);
}
static void about(void)
{
	Rect b = { 90, 56, 200, 456 };
	void *w = (void *)NewWindow(0, &b, pstr(pbuf, ""), 1, 1 /* dBoxProc */, (void *)-1, 0, 0);
	SetPort(w);
	MoveTo(16, 24); DrawString(pstr(pbuf, "Aurora68 Apple II 1.3"));
	MoveTo(16, 44); DrawString(pstr(pbuf, "An 8-bit Apple II (Aurora65) on the Aurora68 Apple II"));
	MoveTo(16, 60); DrawString(pstr(pbuf, "Card, in a window. Command is Open-Apple, Option is"));
	MoveTo(16, 76); DrawString(pstr(pbuf, "Solid-Apple; Control-Command-Delete is Control-Reset."));
	MoveTo(16, 98); DrawString(pstr(pbuf, "Click to continue."));
	EventRecord ev;
	for (;;) { beat(); disks_serve(); if (WaitNextEvent(1 << mouseDown | 1 << keyDown, &ev, disks_sleep(30), 0) && (ev.what == mouseDown || ev.what == keyDown)) break; }
	DisposeWindow(w);
	if (win) SetPort(win);
}
static void do_menu(long mi)
{
	short menu = (short)(mi >> 16), item = (short)mi;
	switch (menu) {
	case M_APPLE:
		if (item == 1) about();
		else { GetItem(m_apple, item, pbuf); OpenDeskAcc(pbuf); SetPort(win); }
		break;
	case M_FILE: if (item == 1) quit = 1; break;
	case M_EDIT: SystemEdit(item - 1); break;
	case M_A2:
		if (item == 1) command(A68A_C_POWER_ON, 0, 0);
		else if (item == 2) command(A68A_C_POWER_OFF, 0, 0);
		else if (item == 3) command(A68A_C_RESET, 0, 0);
		else if (item == 4) command(A68A_C_RESET, 1, 0);
		else if (item == 5) command(blk->state == A68A_S_PAUSED ? A68A_C_RESUME : A68A_C_PAUSE, 0, 0);
		else if (item >= A2_MODEL_FIRST) model_chosen((unsigned)(item - A2_MODEL_FIRST));
		break;
	default: if (disks_is_menu(menu)) disks_chosen(menu, item); break;
	case M_VIEW:
		if (item == 1 || item == 2) {
			scaled = item == 2;
			if (!scaled) sized_by_user = 0;       /* 1:1: the window takes the picture's size again */
		}
		else if (item == 4) applesense = !applesense;
		else if (item == 5) colour = !colour;
		tell_place(1); command(A68A_C_REDRAW, 0, 0);
		break;
	case M_KEYS: if (item == 1) capture(!captured); break;
	}
	HiliteMenu(0);
}
static void mouse_down(EventRecord *ev)
{
	void *w = 0;
	int part = FindWindow(pt_long(ev->where), &w);
	if (captured && w == win && part == inContent) {
		if (ev->modifiers & cmdKey) capture(0);  /* Command-click: the keyboard's Command-keys back to the Mac */
		return;                                   /* the button is the joystick's (read at each event) */
	}
	if (captured) capture(0);
	switch (part) {
	case inMenuBar: fix_menus(); disks_menu(); { long mi = MenuSelect(pt_long(ev->where)); beat(); do_menu(mi); } break;
	case inSysWindow: SystemClick(ev, w); break;
	case inDrag:
		hidden_by_us = 1; tell_place(0);
		DragWindow(w, pt_long(ev->where), RGN_BBOX(LM_GRAYRGN));
		hidden_by_us = 0; tell_place(0);
		break;
	case inGrow: {
		Rect lim = { 80, 140, 2000, 2000 };
		hidden_by_us = 1; tell_place(0);
		long sz = GrowWindow(w, pt_long(ev->where), &lim);
		hidden_by_us = 0;
		if (sz) { SizeWindow(w, (short)sz, (short)(sz >> 16), 1); sized_by_user = 1; resized(); }
		else tell_place(0);
		break;
	}
	case inZoomIn: case inZoomOut:
		if (TrackBox(w, pt_long(ev->where), part)) {
			short nw, nh;
			natural(&nw, &nh);
			void **wsh = *(void ***)((char *)w + 130);          /* dataHandle: the WStateData; zoomed is the natural size */
			if (wsh && *wsh) {
				Rect *std = (Rect *)((char *)*wsh + 8);
				Rect r = *PORT_RECT(w); Point tl = { r.top, r.left };
				SetPort(w); LocalToGlobal(&tl);
				std->top = tl.v; std->left = tl.h; std->bottom = (short)(tl.v + nh); std->right = (short)(tl.h + nw);
				scr_fit(std);
			}
			SetPort(w); EraseRect(PORT_RECT(w));
			ZoomWindow(w, part, 0);
			sized_by_user = 1;
			resized();
		}
		break;
	case inGoAway: if (TrackGoAway(w, pt_long(ev->where))) quit = 1; break;
	case inContent:
		if (w != FrontWindow()) { SelectWindow(w); break; }
		capture(1);                                     /* a click in the window: Command-keys and the joystick too */
		break;
	}
}

int main(void)
{
	short mask_was = LM_SYSEVTMASK;
	m_apple = NewMenu(M_APPLE, pstr(pbuf, "\x14"));
	AppendMenu(m_apple, pstr(pbuf, "About Aurora68 Apple II\xC9;(-"));
	AddResMenu(m_apple, TYPE('D', 'R', 'V', 'R'));
	InsertMenu(m_apple, 0);
	m_file = NewMenu(M_FILE, pstr(pbuf, "File"));
	AppendMenu(m_file, pstr(pbuf, "Quit/Q"));
	InsertMenu(m_file, 0);
	m_edit = NewMenu(M_EDIT, pstr(pbuf, "Edit"));
	AppendMenu(m_edit, pstr(pbuf, "Undo/Z;(-;Cut/X;Copy/C;Paste/V;Clear"));
	InsertMenu(m_edit, 0);
	m_a2 = NewMenu(M_A2, pstr(pbuf, "Apple II"));
	/* "(" anywhere in AppendMenu's text disables the item (a metacharacter): items with parentheses get their words
	   through SetItem, which takes them as they are (found in the testbed: "Actual Size (1:1)" came up dimmed) */
	AppendMenu(m_a2, pstr(pbuf, "Power On;Power Off;Reset;Restart Cold;Pause;(-;(Model:"));
	SetItem(m_a2, 3, pstr(pbuf, "Reset (Control-Command-Delete)"));
	for (unsigned i = 0; i < A68A_MODELS; i++) { AppendMenu(m_a2, pstr(pbuf, "x")); SetItem(m_a2, (short)(A2_MODEL_FIRST + i), pstr(pbuf, model_names[i])); }
	InsertMenu(m_a2, 0);
	m_disks = NewMenu(M_DISKS, pstr(pbuf, "Disks"));
	disks_menu_build();
	InsertMenu(m_disks, 0);
	m_view = NewMenu(M_VIEW, pstr(pbuf, "View"));
	AppendMenu(m_view, pstr(pbuf, "Actual Size;Scaled to the Window;(-;White on Black;Hires in Colour"));
	SetItem(m_view, 1, pstr(pbuf, "Actual Size (1:1)"));
	InsertMenu(m_view, 0);
	m_keys = NewMenu(M_KEYS, pstr(pbuf, "Keyboard"));
	AppendMenu(m_keys, pstr(pbuf, "Capture Keyboard and Joystick"));
	SetItem(m_keys, 1, pstr(pbuf, "Capture Keyboard and Joystick (Control-Option-Command-A)"));
	InsertMenu(m_keys, 0);
	DrawMenuBar();

	if (!card_open()) { drv = 0; blk = 0; }
	if (drv) { blk->prog = A68A_PROG_LIVE; beat(); ets_patch(); }
	else card = 0;
	short nw, nh;
	natural(&nw, &nh);
	Rect b = { 40, 8, (short)(40 + nh), (short)(8 + nw) };
	win = (void *)NewWindow(0, &b, pstr(pbuf, "Aurora68 Apple II"), 1, zoomDocProc, (void *)-1, 1, 0);
	SetPort(win);
	keep_on_screen();
	LM_SYSEVTMASK = (short)(mask_was | keyUpMask);
	title();
	if (drv && (blk->state == A68A_S_OFF || blk->state == A68A_S_FAILED)) command(A68A_C_POWER_ON, 0, 0);
	set_front(1);
	tell_place(1);
	disks_init();                                   /* the disks from the Mac: the card's session, the folder, the Finder's files */

	EventRecord ev;
	while (!quit) {
		beat();
		int got = WaitNextEvent(everyEvent, &ev, disks_sleep(captured ? 1 : 6), 0);
		if (got) switch (ev.what) {
		case 23: disks_hle(&ev); break;          /* kHighLevelEvent: an Apple event (Open Documents: images dropped on the icon) */
		case mouseDown: mouse_down(&ev); break;
		case keyDown: case autoKey: case keyUp: {
			if (special_key(&ev)) break;
			if (FrontWindow() != win) break;
			if (ev.what != keyUp && (ev.modifiers & cmdKey) && !captured) { fix_menus(); disks_menu(); do_menu(MenuKey((char)(ev.message & charCodeMask))); break; }
			if (!drv) break;
			modifiers(ev.modifiers);
			if (ev.what == keyUp) send(A68A_IN_KEYUP, (unsigned)((ev.message >> 8) & 0xFF), 0, ev.modifiers, 0, 0, 0);
			else send(A68A_IN_KEYDOWN, (unsigned)((ev.message >> 8) & 0xFF), (unsigned)(ev.message & 0xFF), ev.modifiers, 0, 0, 0);
			break;
		}
		case updateEvt:
			if ((void *)ev.message == win) { BeginUpdate(win); draw_window(); EndUpdate(win); tell_place(1); command(A68A_C_REDRAW, 0, 0); }
			break;
		case activateEvt:
			if ((void *)ev.message == win) {
				set_front((ev.modifiers & activeFlag) != 0);
				if (!front) capture(0);               /* the window to the back: everything back to the Mac */
				DrawGrowIcon(win);
				tell_place(1);
			}
			break;
		case osEvt:
			if (((unsigned long)ev.message >> 24) == suspendResumeMessage) {
				int resumed = (ev.message & resumeFlag) != 0;
				set_front(resumed && FrontWindow() == win);
				if (!resumed) capture(0);
				else disks_resume();                  /* the Apple II Disks folder looked at again (the Finder may have changed it) */
			}
			tell_place(1);
			break;
		}
		disks_idle();
		if (disks_quit_asked()) quit = 1;
		if (!drv) continue;
		if (front) modifiers(ev.modifiers);
		if (captured) joystick();
		if (!sized_by_user && !scaled && !captured) {   /* the picture changed size (a mode, the screen): the window follows */
			Rect r = *PORT_RECT(win);
			natural(&nw, &nh);
			if (nw != r.right - r.left || nh != r.bottom - r.top) { SizeWindow(win, nw, nh, 1); keep_on_screen(); resized(); }
		}
		tell_place(0);
		title();
	}
	capture(0);
	disks_quit();                                   /* every disk from the Mac written back to its file and let go, then closed */
	set_front(0);
	tell_place(1);                                  /* quit: not visible, so the card draws nothing where the window was */
	if (drv) { gone(); command(A68A_C_NOWINDOW, 0, 0); }   /* and the window gone: the card stops before this heap is freed */
	LM_SYSEVTMASK = mask_was;
	return 0;
}
