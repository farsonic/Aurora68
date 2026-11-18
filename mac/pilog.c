/* mac/pilog.c - "PiLog": the Pi kernel's log on the Macintosh. MPL-2.0.
 *
 * The kernel copies every character of its log into a 64 KiB ring in the Aurora68 card's
 * window (emu68/src/psmac/psmb.h, struct psmb_log at window + 0x60000), from boot. PiLog shows
 * the ring in a window that can be moved, resized, zoomed and scrolled; new lines arrive while it
 * runs, in the background too. When the view is at the bottom it follows them; scrolled up, it
 * stays where it is. The text can be selected and copied (Edit menu or Command-C).
 *
 * It costs next to nothing while nothing happens: WaitNextEvent sleeps a quarter of a second
 * between looks at the ring's head, which is one memory read.
 *
 * The check boxes along the top choose which parts of the log show: Kernel (Emu68's own lines and
 * any other tag), Mac (the logic board, the Mac side's messages, SCSI), WiFi, Bluetooth, Network
 * and Card (the Pi's SD card). A line without a tag goes with the line before it. Option-click a
 * box to see that part alone; All shows everything again. A change of filter reads the whole ring
 * again, so earlier lines of that part appear too.
 *
 * TextEdit holds at most 32 KB, so the window keeps the newest 24 KB or so and drops whole lines
 * from the top as new ones come in.
 *
 * Built with the C kit in mac/c (mac/c/README.md). Keys: arrows, Page Up and Down, Home, End.
 */
#include "toolbox.h"

/* ---- the ring (psmb.h) ---- */
#define LOG_OFF    0x60000
#define LOG_MAGIC  0x4136384Cu     /* 'A68L' */
struct psmb_log { unsigned long magic, version, size, data_off, head, pad[3]; };

#define KEEP       24000           /* the most text in the window */
#define LIMIT      30000           /* above this, lines go from the top */
#define POLL_TICKS 15

static volatile unsigned char *win_base;
static volatile struct psmb_log *lg;
static unsigned long pos;          /* the next byte of the log to show */
static int esc;                    /* inside an ANSI escape sequence */

/* ---- the filter ---- */
enum { C_KERNEL, C_MAC, C_WIFI, C_BT, C_NET, C_CARD, NCAT };
static const char *const cat_name[NCAT] = { "Kernel", "Mac", "WiFi", "Bluetooth", "Network", "Card" };
static int show[NCAT] = { 1, 1, 1, 1, 1, 1 };
static void *cat_box[NCAT], *all_btn;
static int line_cat = C_KERNEL;    /* the category of the line being read (an untagged line keeps it) */
static unsigned char line[400];    /* the line being read */
static int line_n;
#define STRIP 22                   /* the height of the row of check boxes */

/* what a line is, from its tag: "[WIFI] ...", "[PSMAC:SOLO] ..." */
static int pfx(const unsigned char *l, int n, const char *p)
{
	int i;
	for (i = 0; p[i]; i++) if (i >= n || l[i] != (unsigned char)p[i]) return 0;
	return 1;
}
static int classify(const unsigned char *l, int n, int prev)
{
	if (!n || l[0] != '[') return prev;
	if (pfx(l, n, "[WIFI")) return C_WIFI;
	if (pfx(l, n, "[BT")) return C_BT;
	if (pfx(l, n, "[NET") || pfx(l, n, "[ETH")) return C_NET;
	if (pfx(l, n, "[CARD") || pfx(l, n, "[WBACK")) return C_CARD;
	if (pfx(l, n, "[PSMAC") || pfx(l, n, "[MAC") || pfx(l, n, "[SCSI")) return C_MAC;
	return C_KERNEL;
}

static void *win, *vbar;
static void refilter(void);
static TEHandle te;
static int top;                    /* the first line in view */
static int fg = 1;                 /* PiLog is the front program */
static int quit;
static void *m_apple, *m_file, *m_edit;
static unsigned char pbuf[256];

enum { M_APPLE = 128, M_FILE, M_EDIT };
enum { F_CLEAR = 1, F_QUIT = 3 };
enum { E_UNDO = 1, E_CUT = 3, E_COPY, E_PASTE, E_CLEAR, E_SELALL = 8 };

/* ---- layout and scrolling ---- */
static int line_h(void) { return (*te)->lineHeight > 0 ? (*te)->lineHeight : 11; }
static int visible(void)
{
	Rect *v = &(*te)->viewRect;
	int n = (v->bottom - v->top) / line_h();
	return n > 0 ? n : 1;
}
static int max_top(void)
{
	TERec *t = *te;
	int n = t->nLines;
	/* a final carriage return starts a line TextEdit does not count yet */
	if (t->teLength && (*t->hText)[t->teLength - 1] == '\r') n++;
	n -= visible();
	return n > 0 ? n : 0;
}
static void scroll_to(int n)
{
	int m = max_top();
	if (n > m) n = m;
	if (n < 0) n = 0;
	if (n != top) {
		TEScroll(0, (top - n) * line_h(), te);
		top = n;
	}
	SetCtlMax(vbar, m);
	SetCtlValue(vbar, top);
}

/* the scroll bar along the right, the text in the rest */
static void layout(void)
{
	Rect r = *PORT_RECT(win), b, v;
	b.top = r.top + STRIP - 1; b.left = r.right - 15; b.bottom = r.bottom - 14; b.right = r.right + 1;
	HideControl(vbar);
	MoveControl(vbar, b.left, b.top);
	SizeControl(vbar, b.right - b.left, b.bottom - b.top);
	v.top = r.top + STRIP + 2; v.left = r.left + 4; v.bottom = r.bottom - 15; v.right = r.right - 17;
	v.bottom = v.top + (v.bottom - v.top) / line_h() * line_h();
	TERec *t = *te;
	t->viewRect = v;
	t->destRect.left = v.left;
	t->destRect.right = v.right;
	t->destRect.top = v.top - top * line_h();
	t->destRect.bottom = t->destRect.top + 30000;
	TECalText(te);
	ShowControl(vbar);
}

/* the row of check boxes, in the system font, and All */
static void make_boxes(void)
{
	int h = 6, i;
	TextFont(0); TextSize(12);
	for (i = 0; i < NCAT; i++) {
		Rect b;
		pstr(pbuf, cat_name[i]);
		b.top = 3; b.bottom = 19; b.left = h; b.right = h + 20 + StringWidth(pbuf);
		cat_box[i] = NewControl(win, &b, pbuf, 1, 1, 0, 1, checkBoxProc, i);
		h = b.right + 6;
	}
	Rect a = { 2, h + 4, 20, h + 4 + 36 };
	all_btn = NewControl(win, &a, pstr(pbuf, "All"), 1, 0, 0, 1, pushButProc, 0);
	TextFont(4); TextSize(9);
}

static void box_click(void *ctl, int option)
{
	int i, k = -1;
	for (i = 0; i < NCAT; i++) if (cat_box[i] == ctl) k = i;
	if (ctl == all_btn) for (i = 0; i < NCAT; i++) show[i] = 1;
	else if (k < 0) return;
	else if (option) for (i = 0; i < NCAT; i++) show[i] = i == k;
	else show[k] = !show[k];
	for (i = 0; i < NCAT; i++) SetCtlValue(cat_box[i], show[i]);
	refilter();
}

static void resized(void)
{
	int follow = top >= max_top();
	SetPort(win);
	InvalRect(PORT_RECT(win));
	layout();
	scroll_to(follow ? 32767 : top);
}

/* ---- new text from the ring ---- */
static void add_text(const unsigned char *s, int n)
{
	int follow = top >= max_top();
	TERec *t = *te;
	int ss = t->selStart, se = t->selEnd;
	if (t->teLength + n > LIMIT) {
		/* drop whole lines from the top: up to the first line end past the cut */
		char *x = *t->hText;
		int cut = t->teLength + n - KEEP, lines = 0, i;
		if (cut > t->teLength) cut = t->teLength;
		for (i = 0; i < cut; i++) if (x[i] == '\r') lines++;
		while (cut < t->teLength && x[cut - 1] != '\r') cut++;
		if (cut && x[cut - 1] == '\r' && cut > i) lines++;
		TESetSelect(0, cut, te);
		TEDelete(te);
		ss -= cut; se -= cut;
		if (ss < 0) ss = 0;
		if (se < 0) se = 0;
		/* the same lines stay in view while the view is scrolled up */
		if (!follow) {
			int k = top < lines ? top : lines;
			TEScroll(0, k * line_h(), te);
			top -= k;
		}
	}
	TESetSelect(32767, 32767, te);
	TEInsert(s, n, te);
	TESetSelect(ss, se, te);
	scroll_to(follow ? 32767 : top);
}

/* The log's bytes from pos to head, line by line through the filter, into out (len bytes at most;
   a full out is handed to flush, which returns what it left there). Returns the bytes in out. */
static int read_log(unsigned long head, unsigned char *out, int len, int n, int (*flush)(unsigned char *, int))
{
	unsigned long size = lg->size;
	while (pos != head) {
		unsigned char c = win_base[lg->data_off + pos % size];
		pos++;
		if (esc) { if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) esc = 0; continue; }
		if (c == 27) { esc = 1; continue; }
		if (c == '\t') c = ' ';
		if (c != '\n') {
			if (c < 32 || c == 127) continue;
			if (line_n < (int)sizeof line) line[line_n++] = c;
			continue;
		}
		line_cat = classify(line, line_n, line_cat);
		if (show[line_cat]) {
			if (n + line_n + 1 > len) n = flush(out, n);
			memcpy(out + n, line, line_n);
			n += line_n;
			out[n++] = '\r';
		}
		line_n = 0;
	}
	return n;
}

static int flush_te(unsigned char *s, int n) { if (n) add_text(s, n); return 0; }

static void poll_log(void)
{
	static unsigned char buf[2048];
	if (!lg) return;
	unsigned long head = lg->head, size = lg->size;
	if (head == pos) return;
	if (head - pos > size) { pos = head - size; line_n = 0; }
	flush_te(buf, read_log(head, buf, sizeof buf, 0, flush_te));
}

/* The filter changed: the whole ring again, through the new filter, into the window. The text is
   gathered in a buffer of its own, which keeps the newest KEEP bytes, and goes in at once. */
static unsigned char *refill_buf;
static int flush_refill(unsigned char *s, int n)
{
	/* the buffer is full: keep the newest half, from a line start */
	int cut = n - KEEP / 2;
	while (cut < n && s[cut - 1] != '\r') cut++;
	memmove(s, s + cut, n - cut);
	return n - cut;
}
static void refilter(void)
{
	if (!lg) return;
	if (!refill_buf) refill_buf = mac_newptr(KEEP + 4096);
	if (!refill_buf) { SysBeep(5); return; }
	unsigned long head = lg->head, size = lg->size;
	pos = head > size ? head - size : 0;
	/* the ring's oldest line is probably cut: start at the first line end */
	if (head > size) while (pos != head && win_base[lg->data_off + pos % size] != '\n') pos++;
	if (pos != head && head > size) pos++;
	line_n = 0; esc = 0; line_cat = C_KERNEL;
	int n = read_log(head, refill_buf, KEEP + 4096, 0, flush_refill);
	if (n > KEEP) n = flush_refill(refill_buf, n);
	SetPort(win);
	TESetText(refill_buf, n, te);
	top = 0;
	layout();
	scroll_to(32767);
	InvalRect(PORT_RECT(win));
}

/* ---- menus ---- */
static void setup_menus(void)
{
	unsigned char t[2] = { 1, 0x14 };       /* the apple */
	m_apple = NewMenu(M_APPLE, t);
	AppendMenu(m_apple, pstr(pbuf, "About PiLog\xC9;(-"));
	AddResMenu(m_apple, TYPE('D', 'R', 'V', 'R'));
	InsertMenu(m_apple, 0);
	m_file = NewMenu(M_FILE, pstr(pbuf, "File"));
	AppendMenu(m_file, pstr(pbuf, "Clear Window/K;(-;Quit/Q"));
	InsertMenu(m_file, 0);
	m_edit = NewMenu(M_EDIT, pstr(pbuf, "Edit"));
	AppendMenu(m_edit, pstr(pbuf, "(Undo/Z;(-;(Cut/X;Copy/C;(Paste/V;(Clear;(-;Select All/A"));
	InsertMenu(m_edit, 0);
	DrawMenuBar();
}

static void about(void)
{
	/* a line in the window itself (an alert would need 'ALRT' and 'DITL' resources) */
	static const char msg[] = "PiLog: the Pi kernel's log (Aurora68). It follows new lines while the view is at the bottom.\r";
	add_text((const unsigned char *)msg, sizeof msg - 1);
}

static void do_menu(long mr)
{
	int id = (int)(mr >> 16), item = (int)(mr & 0xFFFF);
	if (id == M_APPLE) {
		if (item == 1) about();
		else { GetItem(m_apple, item, pbuf); OpenDeskAcc(pbuf); }
	} else if (id == M_FILE) {
		if (item == F_CLEAR) {
			TESetSelect(0, 32767, te);
			TEDelete(te);
			top = 0;
			layout();
			scroll_to(0);
			InvalRect(PORT_RECT(win));
		} else if (item == F_QUIT) quit = 1;
	} else if (id == M_EDIT) {
		if (!SystemEdit(item - 1)) {
			if (item == E_COPY) {
				TECopy(te);
				ZeroScrap();
				PutScrap(LM_TESCRPLEN, TYPE('T', 'E', 'X', 'T'), *LM_TESCRPH);
			} else if (item == E_SELALL) TESetSelect(0, 32767, te);
		}
	}
	HiliteMenu(0);
}

/* ---- events ---- */
void c_ctl_action(void *ctl, int part)
{
	(void)ctl;
	int page = visible() > 1 ? visible() - 1 : 1;
	switch (part) {
	case inUpButton:   scroll_to(top - 1); break;
	case inDownButton: scroll_to(top + 1); break;
	case inPageUp:     scroll_to(top - page); break;
	case inPageDown:   scroll_to(top + page); break;
	}
}

static void activate(int on)
{
	SetPort(win);
	int i;
	if (on) { TEActivate(te); HiliteControl(vbar, 0); }
	else { TEDeactivate(te); HiliteControl(vbar, 255); }
	for (i = 0; i < NCAT; i++) HiliteControl(cat_box[i], on ? 0 : 255);
	HiliteControl(all_btn, on ? 0 : 255);
	DrawGrowIcon(win);
}

static void content_click(EventRecord *ev)
{
	Point p = ev->where;
	void *ctl = 0;
	SetPort(win);
	GlobalToLocal(&p);
	int part = FindControl(pt_long(p), win, &ctl);
	if (ctl && ctl != vbar) {
		if (TrackControl(ctl, pt_long(p), 0)) box_click(ctl, (ev->modifiers & optionKey) != 0);
		return;
	}
	if (ctl == vbar && part) {
		if (part == inThumb) {
			TrackControl(ctl, pt_long(p), 0);
			scroll_to(GetCtlValue(vbar));
		} else TrackControl(ctl, pt_long(p), (void *)pas_ctl_action);
		return;
	}
	if (PtInRect(pt_long(p), &(*te)->viewRect))
		TEClick(pt_long(p), (ev->modifiers & shiftKey) != 0, te);
}

static void mouse_down(EventRecord *ev)
{
	void *w = 0;
	int part = FindWindow(pt_long(ev->where), &w);
	switch (part) {
	case inMenuBar: do_menu(MenuSelect(pt_long(ev->where))); break;
	case inSysWindow: SystemClick(ev, w); break;
	case inDrag: DragWindow(w, pt_long(ev->where), RGN_BBOX(LM_GRAYRGN)); break;
	case inGoAway: if (w == win && TrackGoAway(w, pt_long(ev->where))) quit = 1; break;
	case inGrow:
		if (w == win) {
			Rect lim = { 100, 200, 4000, 4000 };     /* top/left: the smallest height and width */
			long sz = GrowWindow(w, pt_long(ev->where), &lim);
			if (sz) { SizeWindow(w, (short)(sz & 0xFFFF), (short)(sz >> 16), 1); resized(); }
		}
		break;
	case inZoomIn:
	case inZoomOut:
		if (w == win && TrackBox(w, pt_long(ev->where), part)) {
			SetPort(win);
			EraseRect(PORT_RECT(win));
			ZoomWindow(w, part, 0);
			resized();
		}
		break;
	case inContent:
		if (w != FrontWindow()) SelectWindow(w);
		else content_click(ev);
		break;
	}
}

static void key_down(EventRecord *ev)
{
	int c = (int)(ev->message & charCodeMask);
	if (ev->modifiers & cmdKey) { do_menu(MenuKey(c)); return; }
	int page = visible() > 1 ? visible() - 1 : 1;
	switch (c) {
	case 0x1E: scroll_to(top - 1); break;       /* up arrow */
	case 0x1F: scroll_to(top + 1); break;       /* down arrow */
	case 0x0B: scroll_to(top - page); break;    /* page up */
	case 0x0C: scroll_to(top + page); break;    /* page down */
	case 0x01: scroll_to(0); break;             /* home */
	case 0x04: scroll_to(32767); break;         /* end */
	}
}

static void update(void *w)
{
	if (w != win) return;
	SetPort(win);
	BeginUpdate(win);
	EraseRect(PORT_RECT(win));
	TEUpdate(PORT_RECT(win), te);
	DrawControls(win);
	MoveTo(PORT_RECT(win)->left, STRIP - 1);
	LineTo(PORT_RECT(win)->right - 16, STRIP - 1);
	DrawGrowIcon(win);
	EndUpdate(win);
}

int main(void)
{
	setup_menus();
	win_base = LM_MMU32BIT ? (volatile unsigned char *)0xF9000000 : (volatile unsigned char *)0x00900000;
	lg = (volatile struct psmb_log *)(win_base + LOG_OFF);
	if (lg->magic != LOG_MAGIC || lg->size == 0 || lg->data_off != LOG_OFF + 0x1000) lg = 0;

	/* the main screen's top left, room for the menu bar; the SE's screen is 512 x 342 */
	Rect b = { 44, 8, 330, 504 };
	win = NewWindow(0, &b, pstr(pbuf, "PiLog"), 1, zoomDocProc, (void *)-1, 1, 0);
	SetPort(win);
	TextFont(4);        /* Monaco */
	TextSize(9);
	Rect r = *PORT_RECT(win);
	vbar = NewControl(win, &r, pstr(pbuf, ""), 1, 0, 0, 0, scrollBarProc, 0);
	te = (TEHandle)TENew(&r, &r);
	make_boxes();
	layout();
	if (!lg) {
		static const char msg[] = "No kernel log here: PiLog needs an Aurora68 kernel from version 0.5 on.\r";
		add_text((const unsigned char *)msg, sizeof msg - 1);
	}
	refilter();

	unsigned long last = LM_TICKS;
	EventRecord ev;
	while (!quit) {
		int got = WaitNextEvent(everyEvent, &ev, fg ? POLL_TICKS : POLL_TICKS * 2, 0);
		if (got) {
			switch (ev.what) {
			case mouseDown: mouse_down(&ev); break;
			case keyDown: case autoKey: key_down(&ev); break;
			case updateEvt: update((void *)ev.message); break;
			case activateEvt: if ((void *)ev.message == win) activate(ev.modifiers & activeFlag); break;
			case osEvt:
				if (((unsigned long)ev.message >> 24) == suspendResumeMessage) {
					fg = ev.message & resumeFlag;
					activate(fg);
				}
				break;
			}
		}
		if (fg) TEIdle(te);
		if (LM_TICKS - last >= POLL_TICKS) {
			last = LM_TICKS;
			SetPort(win);
			poll_log();
		}
	}
	return 0;
}
