/* SPDX-License-Identifier: MPL-2.0
 *
 * mac/a68pc/pc.c - Aurora68 PC: the PC on the Aurora68 PC Card (slot $D) in a window on the Mac's desktop
 * (docs/19-pc-card.md; Aurora86's docs/DESIGN.md, "P3: the window").
 *
 * Backstory (23 November 2026). The owner: "Aurora86 needs to run in a window on the Mac desktop." The PC itself
 * (Aurora86: Bochs, Libreboot's firmware) runs on a core of the Pi, and the card draws its screen into the Mac's
 * frame buffer, inside this program's window and only where the window shows (the card reads the window's visRgn
 * through the Mac's memory, as the video decoder card does for Aurora68 Play and Aurora68 Video). So this program
 * does what any Mac program does with a window and nothing per picture: it says where the window's content is and
 * hands over its visRgn whenever the window moves, is resized, shown or brought forward; on an update event it asks
 * the card to draw it all again; its menus power the PC on and off, reset it and press Control-Alt-Delete; and while
 * the PC has the keyboard and mouse ("captured", as the DOS cards of 1993 had it) it passes key presses and releases
 * as Mac virtual key codes (the card turns them into the PC's) and the mouse's movement, the pointer hidden and held
 * in the window's middle.
 *
 * Capture: a click in the window, Keyboard > Capture, or Control-Option-Command-P while the window is in front;
 * Control-Option-Command-P again, a Command-click, or the window going to the back give the keyboard and mouse back.
 * The window's title says which. Key releases come as keyUp events, which the system sends only when asked
 * (SysEvtMask, $144: this program adds keyUpMask while it runs and takes it out again when it quits); auto-key
 * events are not passed on, the PC repeats keys itself; the modifier keys are their own presses, from the
 * events' modifiers compared with the last ones.
 *
 * The window: a document window with a grow box and a zoom box (zoomed: the PC's picture at its natural size, which
 * the card publishes: 480 x 300 for 80 x 25 text on the SE's screen, 640 x 400 or 720 x 400 in colour, the PC's
 * resolution for graphics). It fits the SE's 512 x 342 screen with the menu bar. The card leaves the grow box's
 * corner alone (A68P_O_GROWBOX) so that the box can be drawn. Human Interface Guidelines (1992); System 6 and 7.
 */
#include "toolbox.h"
#include "psmac_pc.h"

short drv_open(void *pb), drv_control(void *pb), drv_status(void *pb);

#define LM_SYSEVTMASK (*(volatile short *)0x144)
#define LM_MTEMP      (*(volatile Point *)0x828)
#define LM_RAWMOUSE   (*(volatile Point *)0x82C)
#define LM_MOUSE      (*(volatile Point *)0x830)
#define LM_CRSRNEW    (*(volatile unsigned char *)0x8CE)
#define LM_CRSRCOUPLE (*(volatile unsigned char *)0x8CF)
#define keyUpMask     0x0010
#define alphaLock     0x0400
#define controlKey    0x1000
#define VK_P          0x23
#define inZoomIn      7
#define inZoomOut     8
#define inGrow        5

enum { M_APPLE = 128, M_FILE, M_EDIT, M_PC, M_DISKS, M_VIEW, M_KEYS };
static void *m_apple, *m_file, *m_edit, *m_pc, *m_disks, *m_view, *m_keys;
static void *win;
static unsigned char pbuf[256];

/* ---- the card ------------------------------------------------------------------------------------- */
typedef struct {
	void *qLink; short qType, ioTrap; void *ioCmdAddr, *ioCompletion; short ioResult;
	const unsigned char *ioNamePtr; short ioVRefNum, ioCRefNum, csCode;
	long csParam[6];
} CntrlParam;
static short drv;
static volatile unsigned char *card;
static volatile struct a68p_block *blk;

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
	pb.ioNamePtr = (const unsigned char *)"\x06.A68PC";
	if (drv_open(&pb)) return 0;
	drv = pb.ioCRefNum;
	memset(&pb, 0, sizeof pb);
	pb.ioCRefNum = drv; pb.csCode = 100;
	if (drv_status(&pb) || !pb.csParam[0]) return 0;
	card = (volatile unsigned char *)pb.csParam[0];
	blk = (volatile struct a68p_block *)card;
	return blk->magic == A68P_MAGIC;
}
static void command(unsigned long c, unsigned long a0, unsigned long a1) { if (drv) ctl(100, (long)c, (long)a0, (long)a1); }

/* ---- the window's place ---------------------------------------------------------------------------- */
static int hidden_by_us, captured, scale1, bigtext, pcsense, quit;
static int sized_by_user;               /* the window keeps the PC's natural size until the user resizes or zooms it */
/* the screen a global point is on: 0 for slot 9's, 1 for slot $A's (the GDevice's driver's slot), its top left */
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
			return slot == 0xA ? A68P_SCREEN_HDMI : A68P_SCREEN_FIRST;
		}
		gd = *(void ***)(d + 30);                      /* gdNextGD */
	}
	origin->h = origin->v = 0;
	return 0;
}
static struct { long screen, top, left, bottom, right, visible; } place, sent;
static struct { long scale, sx, sy, options; } view, sent_view;
static void tell_place(int force)
{
	if (!win || !drv) return;
	Rect r = *PORT_RECT(win);
	Point tl = { r.top, r.left }, br = { r.bottom, r.right }, org;
	SetPort(win);
	LocalToGlobal(&tl); LocalToGlobal(&br);
	place.screen = screen_of(tl, &org);
	place.top = tl.v - org.v; place.left = tl.h - org.h; place.bottom = br.v - org.v; place.right = br.h - org.h;
	place.visible = !hidden_by_us;
	view.scale = scale1 ? A68P_SCALE_1 : A68P_SCALE_FIT; view.sx = view.sy = 0;
	view.options = A68P_O_GROWBOX | (bigtext ? A68P_O_BIGTEXT : 0) | (pcsense ? A68P_O_PCSENSE : 0);
	if (force || memcmp(&view, &sent_view, sizeof view)) { sent_view = view; ctl4(114, view.scale, view.sx, view.sy, view.options); force = 1; }   /* its geom_seq: the region again with the place */
	if (!force && !memcmp(&place, &sent, sizeof place)) return;
	sent = place;
	/* the window's visible region, read by the card at every picture: written before the Control 112 that goes with it */
	blk->vis_rgn = (unsigned long)*(void **)((char *)win + 24);    /* visRgn */
	blk->vis_flags = LM_MMU32BIT ? 0 : A68P_VF_24BIT;
	blk->clip_tag = A68P_CLIP_TAG + blk->geom_seq + 1;
	ctl(112, (long)&place, 0, 0);
}

/* ---- the title and the menus ------------------------------------------------------------------------- */
static char *put(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static unsigned long shown_state = ~0u;
static int shown_capture = -1;
static void title(void)
{
	unsigned long st = drv ? blk->state : 99;
	if (st == shown_state && captured == shown_capture) return;
	shown_state = st; shown_capture = captured;
	char t[100], *p = put(t, "Aurora68 PC");
	const char *w = st == A68P_S_OFF ? " (off)" : st == A68P_S_STARTING ? " (starting)" : st == A68P_S_PAUSED ? " (paused)" :
	                st == A68P_S_STOPPED ? " (switched off)" : st == A68P_S_FAILED ? " (stopped by an error)" : st == 99 ? " (no card)" : "";
	p = put(p, w);
	if (captured) p = put(p, ": keyboard and mouse to the PC");
	*p = 0;
	SetWTitle(win, pstr(pbuf, t));
}
static void fix_menus(void)
{
	unsigned long st = drv ? blk->state : 99;
	int on = st == A68P_S_RUNNING || st == A68P_S_PAUSED || st == A68P_S_STARTING;
	if (drv && !on) EnableItem(m_pc, 1); else DisableItem(m_pc, 1);
	if (drv && on) { EnableItem(m_pc, 2); EnableItem(m_pc, 3); EnableItem(m_pc, 4); EnableItem(m_pc, 5); }
	else { DisableItem(m_pc, 2); DisableItem(m_pc, 3); DisableItem(m_pc, 4); DisableItem(m_pc, 5); }
	CheckItem(m_pc, 5, st == A68P_S_PAUSED);
	CheckItem(m_view, 1, !scale1); CheckItem(m_view, 2, scale1);
	CheckItem(m_view, 4, bigtext); CheckItem(m_view, 5, pcsense);
	CheckItem(m_keys, 1, captured);
}
/* the image list (the card's disks block) as the Disks menu: what is attached is checked */
static unsigned long disks_seq = ~0u;
static int ndisks;
static void disks_menu(void)
{
	if (!drv) return;
	volatile struct a68p_disks *dk = (volatile struct a68p_disks *)(card + A68P_OFF_DISKS);
	if (dk->seq == disks_seq) return;
	disks_seq = dk->seq;
	unsigned n = dk->count < A68P_DISKS_MAX ? dk->count : A68P_DISKS_MAX, i;
	for (i = 0; i < (unsigned)ndisks; i++) DisableItem(m_disks, (short)(i + 1));   /* the Menu Manager has no DeleteItem before System 7: items are kept */
	for (i = (unsigned)ndisks; i < n; i++) {
		char t[70]; unsigned k = 0;
		while (k < 63 && dk->d[i].name[k]) { char c = dk->d[i].name[k]; t[k++] = (c == ';' || c == '(' || c == '/' || c == '^' || c == '!' || c == '<') ? '_' : c; }
		t[k] = 0;
		AppendMenu(m_disks, pstr(pbuf, k ? t : "(no name)"));
	}
	if ((unsigned)ndisks < n) ndisks = (int)n;
	for (i = 0; i < n; i++) { EnableItem(m_disks, (short)(i + 1)); CheckItem(m_disks, (short)(i + 1), dk->d[i].place >= 0); }
}

/* ---- capture: keys and the mouse to the PC ----------------------------------------------------------- */
static short last_mods;
static Point centre;
static void send(unsigned type, unsigned code, short mods, short dx, short dy, unsigned buttons)
{
	struct a68p_in e;
	memset(&e, 0, sizeof e);
	e.type = (unsigned short)type; e.code = (unsigned short)code; e.modifiers = (unsigned short)mods;
	e.dx = dx; e.dy = dy; e.buttons = (unsigned short)buttons;
	ctl(113, (long)&e, 0, 0);
}
static void warp(Point p)
{
	LM_MTEMP = p; LM_RAWMOUSE = p;
	LM_CRSRNEW = LM_CRSRCOUPLE;
}
static void find_centre(void)
{
	Rect r = *PORT_RECT(win);
	Point c = { (short)((r.top + r.bottom) / 2), (short)((r.left + r.right) / 2) };
	SetPort(win);
	LocalToGlobal(&c);
	centre = c;
}
static unsigned buttons;
static void capture(int on)
{
	if (!drv || on == captured) return;
	captured = on;
	if (on) {
		find_centre();
		HideCursor();
		warp(centre);
		last_mods = 0;
		buttons = 0;
	} else {
		send(A68P_IN_RELEASE, 0, 0, 0, 0, 0);
		ShowCursor();
	}
	blk->focus = (unsigned long)on;
	title();
}
/* the modifier keys as presses and releases of their own: Command (Windows), Shift, Caps Lock, Option (Alt), Control */
static void modifiers(short m)
{
	static const struct { short bit; unsigned char vk; } mk[] = {
		{ cmdKey, 0x37 }, { shiftKey, 0x38 }, { alphaLock, 0x39 }, { optionKey, 0x3A }, { controlKey, 0x3B } };
	short ch = (short)(m ^ last_mods);
	for (unsigned i = 0; i < sizeof mk / sizeof mk[0]; i++) {
		if (!(ch & mk[i].bit)) continue;
		if (mk[i].vk == 0x39) {                         /* Caps Lock stays down on the Mac: a press and a release on each change */
			send(A68P_IN_KEYDOWN, 0x39, m, 0, 0, 0); send(A68P_IN_KEYUP, 0x39, m, 0, 0, 0);
		} else send(m & mk[i].bit ? A68P_IN_KEYDOWN : A68P_IN_KEYUP, mk[i].vk, m, 0, 0, 0);
	}
	last_mods = m;
}
static void mouse_moved(void)
{
	Point p = LM_MOUSE;
	short dx = (short)(p.h - centre.h), dy = (short)(p.v - centre.v);
	if (dx || dy) { send(A68P_IN_MOUSE, 0, last_mods, dx, dy, buttons); warp(centre); }
}
static int switch_key(EventRecord *ev)
{
	return ((ev->message >> 8) & 0xFF) == VK_P && (ev->modifiers & (cmdKey | optionKey | controlKey)) == (cmdKey | optionKey | controlKey);
}

/* ---- the window ----------------------------------------------------------------------------------- */
static void natural(short *w, short *h)
{
	long nw = drv ? (long)blk->nat_w : 0, nh = drv ? (long)blk->nat_h : 0;
	if (nw < 160 || nh < 100) { nw = 480; nh = 300; }
	Rect s = *RGN_BBOX(LM_GRAYRGN);
	if (nw > s.right - 8 - 2) nw = s.right - 8 - 2;          /* the window's content at 8, 40: room for its frame and shadow */
	if (nh > s.bottom - 40 - 2) nh = s.bottom - 40 - 2;       /* the SE's 342 lines: 300, 25 rows of 12 */
	*w = (short)nw; *h = (short)nh;
}
static void draw_window(void)
{
	Rect r = *PORT_RECT(win);
	SetPort(win);
	if (!drv) {                                     /* no card: two lines, and nothing else to do */
		EraseRect(&r);
		MoveTo(10, 30); DrawString(pstr(pbuf, "The Aurora68 PC Card does not answer."));
		MoveTo(10, 48); DrawString(pstr(pbuf, "It is in slot $D of an Aurora68 Pi built with the PC"));
		MoveTo(10, 66); DrawString(pstr(pbuf, "(pc=on in cmdline.txt)."));
	}
	DrawGrowIcon(win);                              /* the card leaves the grow box's corner alone */
}
static void resized(void)
{
	SetPort(win);
	InvalRect(PORT_RECT(win));
	find_centre();
	tell_place(1);
}
static void about(void)
{
	Rect b = { 90, 56, 200, 456 };
	void *w = (void *)NewWindow(0, &b, pstr(pbuf, ""), 1, 1 /* dBoxProc */, (void *)-1, 0, 0);
	SetPort(w);
	MoveTo(16, 24); DrawString(pstr(pbuf, "Aurora68 PC 1.0"));
	MoveTo(16, 44); DrawString(pstr(pbuf, "A PC (Aurora86: Bochs and Libreboot) on the Aurora68"));
	MoveTo(16, 60); DrawString(pstr(pbuf, "PC Card, in a window. Control-Option-Command-P moves"));
	MoveTo(16, 76); DrawString(pstr(pbuf, "the keyboard and mouse between the Mac and the PC."));
	MoveTo(16, 98); DrawString(pstr(pbuf, "Click to continue."));
	EventRecord ev;
	for (;;) if (WaitNextEvent(1 << mouseDown | 1 << keyDown, &ev, 30, 0) && (ev.what == mouseDown || ev.what == keyDown)) break;
	DisposeWindow(w);
	SetPort(win);
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
	case M_PC:
		if (item == 1) command(A68P_C_POWER_ON, 0, 0);
		else if (item == 2) command(A68P_C_POWER_OFF, 0, 0);
		else if (item == 3) command(A68P_C_RESET, 0, 0);
		else if (item == 4) command(A68P_C_CAD, 0, 0);
		else if (item == 5) command(blk->state == A68P_S_PAUSED ? A68P_C_RESUME : A68P_C_PAUSE, 0, 0);
		break;
	case M_DISKS: command(A68P_C_ATTACH, (unsigned long)(item - 1), 0); break;
	case M_VIEW:
		if (item == 1 || item == 2) scale1 = item == 2;
		else if (item == 4) bigtext = !bigtext;
		else if (item == 5) pcsense = !pcsense;
		tell_place(1); command(A68P_C_REDRAW, 0, 0);
		break;
	case M_KEYS: if (item == 1) capture(!captured); break;
	}
	HiliteMenu(0);
}
static void mouse_down(EventRecord *ev)
{
	void *w = 0;
	int part = FindWindow(pt_long(ev->where), &w);
	if (captured && w == win && part == 3 /* inContent */) {
		if (ev->modifiers & cmdKey) { capture(0); return; }   /* Command-click: the mouse back to the Mac */
		buttons |= 1; send(A68P_IN_MOUSE, 0, ev->modifiers, 0, 0, buttons);
		return;
	}
	if (captured) capture(0);
	switch (part) {
	case 1: /* inMenuBar */ fix_menus(); disks_menu(); do_menu(MenuSelect(pt_long(ev->where))); break;
	case 2: /* inSysWindow */ SystemClick(ev, w); break;
	case 4: /* inDrag */
		hidden_by_us = 1; tell_place(0);
		DragWindow(w, pt_long(ev->where), RGN_BBOX(LM_GRAYRGN));
		hidden_by_us = 0; find_centre(); tell_place(0);
		break;
	case inGrow: {
		Rect lim = { 80, 160, 2000, 2000 };
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
			/* the zoomed size is the PC's natural one: the window's state record's stdState */
			void **wsh = *(void ***)((char *)w + 130);          /* dataHandle: the WStateData */
			if (wsh && *wsh) {
				Rect *std = (Rect *)((char *)*wsh + 8);
				Rect r = *PORT_RECT(w); Point tl = { r.top, r.left };
				SetPort(w); LocalToGlobal(&tl);
				std->top = tl.v; std->left = tl.h; std->bottom = (short)(tl.v + nh); std->right = (short)(tl.h + nw);
			}
			SetPort(w); EraseRect(PORT_RECT(w));
			ZoomWindow(w, part, 0);
			sized_by_user = 1;
			resized();
		}
		break;
	case 6: /* inGoAway */ if (TrackGoAway(w, pt_long(ev->where))) quit = 1; break;
	case 3: /* inContent */
		if (w != FrontWindow()) { SelectWindow(w); break; }
		capture(1);                                     /* a click in the window: the PC takes the keyboard and mouse */
		break;
	}
}

int main(void)
{
	short mask_was = LM_SYSEVTMASK;
	m_apple = NewMenu(M_APPLE, pstr(pbuf, "\x14"));
	AppendMenu(m_apple, pstr(pbuf, "About Aurora68 PC\xC9;(-"));
	AddResMenu(m_apple, TYPE('D', 'R', 'V', 'R'));
	InsertMenu(m_apple, 0);
	m_file = NewMenu(M_FILE, pstr(pbuf, "File"));
	AppendMenu(m_file, pstr(pbuf, "Quit/Q"));
	InsertMenu(m_file, 0);
	m_edit = NewMenu(M_EDIT, pstr(pbuf, "Edit"));
	AppendMenu(m_edit, pstr(pbuf, "Undo/Z;(-;Cut/X;Copy/C;Paste/V;Clear"));
	InsertMenu(m_edit, 0);
	m_pc = NewMenu(M_PC, pstr(pbuf, "PC"));
	AppendMenu(m_pc, pstr(pbuf, "Power On;Power Off;Reset;Send Control-Alt-Delete;Pause"));
	InsertMenu(m_pc, 0);
	m_disks = NewMenu(M_DISKS, pstr(pbuf, "Disks"));
	InsertMenu(m_disks, 0);
	m_view = NewMenu(M_VIEW, pstr(pbuf, "View"));
	AppendMenu(m_view, pstr(pbuf, "Scaled to the Window;Actual Size;(-;Large Text (8 x 16);White on Black"));
	InsertMenu(m_view, 0);
	m_keys = NewMenu(M_KEYS, pstr(pbuf, "Keyboard"));
	AppendMenu(m_keys, pstr(pbuf, "Capture Keyboard and Mouse (Control-Option-Command-P)"));
	InsertMenu(m_keys, 0);
	DrawMenuBar();

	if (!card_open()) drv = 0;
	short nw, nh;
	natural(&nw, &nh);
	Rect b = { 40, 8, (short)(40 + nh), (short)(8 + nw) };
	win = (void *)NewWindow(0, &b, pstr(pbuf, "Aurora68 PC"), 1, zoomDocProc, (void *)-1, 1, 0);
	SetPort(win);
	LM_SYSEVTMASK = (short)(mask_was | keyUpMask);
	title();
	if (drv && (blk->state == A68P_S_OFF || blk->state == A68P_S_STOPPED)) command(A68P_C_POWER_ON, 0, 0);
	tell_place(1);

	EventRecord ev;
	while (!quit) {
		int got = WaitNextEvent(everyEvent, &ev, captured ? 0 : 10, 0);
		if (got) switch (ev.what) {
		case mouseDown: mouse_down(&ev); break;
		case mouseUp:
			if (captured) { buttons &= ~1u; send(A68P_IN_MOUSE, 0, ev.modifiers, 0, 0, buttons); }
			break;
		case keyDown: case autoKey: case keyUp: {
			if (switch_key(&ev)) { if (ev.what == keyDown && FrontWindow() == win) capture(!captured); break; }
			if (captured) {
				if (ev.what == autoKey) break;          /* the PC repeats keys itself */
				modifiers(ev.modifiers);
				send(ev.what == keyUp ? A68P_IN_KEYUP : A68P_IN_KEYDOWN, (unsigned)((ev.message >> 8) & 0xFF), ev.modifiers, 0, 0, 0);
				break;
			}
			if (ev.what == keyDown && (ev.modifiers & cmdKey)) { fix_menus(); do_menu(MenuKey((char)(ev.message & charCodeMask))); }
			break;
		}
		case updateEvt:
			if ((void *)ev.message == win) { BeginUpdate(win); draw_window(); EndUpdate(win); tell_place(1); command(A68P_C_REDRAW, 0, 0); }
			break;
		case activateEvt:
			if (!(ev.modifiers & activeFlag)) capture(0);   /* the window to the back: the keyboard and mouse back to the Mac */
			DrawGrowIcon(win);
			tell_place(1);
			break;
		case osEvt:
			if (((unsigned long)ev.message >> 24) == suspendResumeMessage && !(ev.message & resumeFlag)) capture(0);
			tell_place(1);
			break;
		}
		if (!drv) continue;
		if (captured) { modifiers(ev.modifiers); mouse_moved(); }
		if (!sized_by_user && !captured) {             /* the PC's picture changed size (a mode, the font): the window follows */
			short nw, nh;
			Rect r = *PORT_RECT(win);
			natural(&nw, &nh);
			if (nw != r.right - r.left || nh != r.bottom - r.top) { SizeWindow(win, nw, nh, 1); resized(); }
		}
		tell_place(0);
		title();
	}
	capture(0);
	LM_SYSEVTMASK = mask_was;
	return 0;
}
