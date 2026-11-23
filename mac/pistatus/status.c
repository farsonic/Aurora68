// SPDX-License-Identifier: MPL-2.0
/* mac/pistatus/status.c - "Aurora68 Status" 2.0: the Pi's temperature, clock and speed as gauges. MPL-2.0.
 *
 * Backstory. Aurora68 Status 1 (mac/pistatus.s until 23 November 2026, PiStatus before 21 November) was a
 * text window: the status page's build string, uptime, the cores' loads, the temperature, the JIT's
 * cache and the network counters, redrawn at most every five seconds. On 23 November 2026 the owner's
 * Pi 3B, in his SE's 68000 socket, overheated during a long music video: the firmware capped the ARM's
 * clock and the 68k fell from 169 to 31 MIPS, which only the kernel's log showed. He asked for it on the
 * Mac's screen: "CPU util, temp etc. Gauges in a specific app window". This is that window.
 *
 * What it shows (docs/09, "Aurora68 Status 2.0"), from the status page the kernel's core 3 fills once a
 * second (emu68/src/alice/alice_status.h; alice_psmac_status.c is the Macintosh half), at slot-$9
 * window + 0x50000:
 *   - three dials in the style of the 1984-92 Mac (a round face, a sweep of 240 degrees, a needle): the
 *     SoC temperature (30-90 C, the band past 60 C marked, past 80 C, where a Pi 3 caps its clock,
 *     heavier), the ARM clock against its nominal one, and the 68k's speed in MIPS;
 *   - the firmware's power and heat state as lights, now and since boot: under-voltage, frequency
 *     capped, throttled, soft temperature limit;
 *   - strip charts of the temperature, the clock and the MIPS over the last minutes (a point every two
 *     seconds), so heating during a video shows as a slope and the capping as a step;
 *   - each ARM core's load and what it runs; the Pi's memory (the kernel's heap, the JIT's cache); the
 *     network's throughput; the video decoder card and the sound card while they work.
 * A compact view (View > Compact View, or the zoom box) keeps what matters in 232 x 50 pixels: the
 * temperature, the clock, the MIPS and one light. 0xFFFFFFFF on the page means "not measured" and is
 * shown so: QEMU's Pi has no thermometer, no measured clock and no throttling bits.
 *
 * Cost on the 68k: one 512-byte copy of the page a second, and only what changed is drawn again (a
 * dial whose needle moved, a light that changed, a line of text that differs, the charts scrolled by a
 * pixel with ScrollRect when the window is unobscured). Plain QuickDraw, no offscreen GWorld: the SE's
 * screen is one bit deep and small, colour screens get RGBForeColor in a colour window.
 *
 * The window, as the Human Interface Guidelines (1992) and the project's other programs have it: a
 * document window with a close box, a zoom box (between the normal and the compact view) and a grow box
 * (the normal view grows: the charts cover more minutes); its place, size and view kept in "Aurora68
 * Status Prefs" (the Preferences folder on System 7, the System Folder on System 6) and put back only
 * while a screen still shows its title bar; Apple, File, Edit and View menus with the standard key
 * equivalents, Edit's first items for desk accessories and Copy for a text summary of every number;
 * an About box centred on the main screen with the default button outlined. Keys as in 1.0: R redraws,
 * Q quits (and Command-R, Command-Q). The normal view fits the SE's 512 x 342 screen.
 *
 * Built with the C kit in mac/c (mac/c/README.md), 68020 code, System 6.0.8 and 7. The numbers and
 * their text are in model.c, tested on the host by tools/pistatus_test.c.
 */
#include "toolbox.h"
#include "model.h"
#include "screens.h"

/* ---- low memory and small helpers ---- */
#define LM_MBARHEIGHT (*(volatile short *)0xBAA)
#define LM_DEVICELIST (*(char **volatile *)0x8A8)
#define LM_MAINDEVICE (*(char **volatile *)0x8A4)
#define LM_ROM85      (*(volatile unsigned short *)0x28E)   /* bit 14 clear: Color QuickDraw */
static int mbar(void) { short h = LM_MBARHEIGHT; return h > 0 && h < 64 ? h : 20; }
static int has_cqd(void) { return !(LM_ROM85 & 0x4000) && LM_DEVICELIST && LM_MAINDEVICE; }
static char *deref(char **h)
{
	unsigned long p = (unsigned long)*h;
	if (!LM_MMU32BIT) p &= 0x00FFFFFFUL;
	return (char *)p;
}
static void *tool_trap(short t)
{
	register long d0 __asm__("d0") = t;
	register void *a0 __asm__("a0");
	__asm__ volatile(".short 0xA746" : "+d"(d0), "=a"(a0) : : "d1", "d2", "a1", "memory", "cc");
	return a0;
}
static void *os_trap(short t)
{
	register long d0 __asm__("d0") = t;
	register void *a0 __asm__("a0");
	__asm__ volatile(".short 0xA346" : "+d"(d0), "=a"(a0) : : "d1", "d2", "a1", "memory", "cc");
	return a0;
}
#define UNIMPLEMENTED 0xA89F
static int has_tool_trap(short t) { return tool_trap(t) != tool_trap(UNIMPLEMENTED); }
static int has_os_trap(short t) { return os_trap(t) != tool_trap(UNIMPLEMENTED); }
static short gestalt(long sel, long *resp)
{
	register long d0 __asm__("d0") = sel;
	register long a0 __asm__("a0");
	__asm__ volatile(".short 0xA1AD" : "+d"(d0), "=a"(a0) : : "d1", "d2", "a1", "memory", "cc");
	*resp = a0;
	return (short)d0;
}

/* ---- the page ---- */
#define PSMB_MAGIC  0x50534D42UL                /* 'PSMB': the card's mailbox window starts with it */
#define STATUS_OFF  0x50000UL
static unsigned char raw[ST_PAGE];
static struct st_now reading;
static struct st_model M;
static int no_page;                             /* no Aurora68 card, or no status page on it */
static const volatile unsigned char *page_addr(void)
{
	const volatile unsigned long *w = (const volatile unsigned long *)(LM_MMU32BIT ? 0xF9000000UL : 0x00900000UL);
	if (*w != PSMB_MAGIC) return 0;
	return (const volatile unsigned char *)w + STATUS_OFF;
}
/* one reading: seq read, the page copied, seq again; 1 a new reading, 0 nothing new, -1 no page */
static int read_page(void)
{
	static unsigned long last_seq = 0xFFFFFFFFUL;
	const volatile unsigned char *p = page_addr();
	if (!p) return -1;
	const volatile unsigned long *seqp = (const volatile unsigned long *)(p + O_SEQ);
	unsigned long s1 = *seqp;
	if (*(const volatile unsigned long *)p != ST_MAGIC) return -1;
	if ((s1 & 1) || s1 == last_seq) return 0;
	const volatile unsigned long *src = (const volatile unsigned long *)p;
	unsigned long *dst = (unsigned long *)raw;
	for (int i = 0; i < ST_PAGE / 4; i++) dst[i] = src[i];
	if (*seqp != s1) return 0;                  /* written meanwhile: next time */
	last_seq = s1;
	return st_parse(raw, &reading) ? 1 : -1;
}

static int strcmp_(const char *a, const char *b);

/* ---- drawing: colour or patterns ---- */
typedef struct { unsigned short red, green, blue; } RGBColor;
static const unsigned char pat_gray[8] = { 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55 };
static const unsigned char pat_ltgray[8] = { 0x88, 0x22, 0x88, 0x22, 0x88, 0x22, 0x88, 0x22 };
static const unsigned char pat_dkgray[8] = { 0x77, 0xDD, 0x77, 0xDD, 0x77, 0xDD, 0x77, 0xDD };
static const unsigned char pat_black[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
enum { C_BLACK, C_GREEN, C_AMBER, C_RED, C_BLUE, C_VIOLET, C_TEAL, C_FACE, C_GRID, C_DIM, C_N };
static const RGBColor colours[C_N] = {
	{ 0x0000, 0x0000, 0x0000 },                 /* text, frames, needles */
	{ 0x1F00, 0xA800, 0x4800 },                 /* fine: the aurora's green */
	{ 0xF000, 0xA000, 0x0000 },                 /* warm, or happened since boot */
	{ 0xDD00, 0x1800, 0x1800 },                 /* hot, or now */
	{ 0x1C00, 0x4C00, 0xC800 },                 /* the clock */
	{ 0x7000, 0x3000, 0xB000 },                 /* the 68k's speed: the aurora's violet */
	{ 0x0800, 0x8000, 0x9000 },                 /* loads */
	{ 0xFFFF, 0xFFFF, 0xF400 },                 /* a dial's face */
	{ 0xC000, 0xC000, 0xC800 },                 /* the charts' grid */
	{ 0x7000, 0x7000, 0x7000 },                 /* "not measured" */
};
static int colour;                              /* the window is on a screen of four bits or more */
static void ink(int c) { if (colour) RGBForeColor(&colours[c]); }
static void ink_black(void) { if (colour) RGBForeColor(&colours[C_BLACK]); }
static void paper_white(void) { static const RGBColor w = { 0xFFFF, 0xFFFF, 0xFFFF }; if (colour) RGBBackColor(&w); }
/* a filled shape: the colour on a colour screen, the pattern on a one-bit one */
static void fill_rect(const Rect *r, int c, const unsigned char *pat)
{
	if (colour) { ink(c); PaintRect(r); ink_black(); }
	else FillRect(r, pat);
}
static void fill_oval(const Rect *r, int c, const unsigned char *pat)
{
	if (colour) { ink(c); PaintOval(r); ink_black(); }
	else FillOval(r, pat);
}
static void fill_arc(const Rect *r, int start, int arc, int c, const unsigned char *pat)
{
	if (colour) { ink(c); PaintArc(r, (short)start, (short)arc); ink_black(); }
	else FillArc(r, (short)start, (short)arc, pat);
}

/* text: Geneva 9, plain or bold */
static unsigned char pbuf[256];
static void font(int bold) { TextFont(3); TextSize(9); TextFace(bold ? 1 : 0); }
static int text_w(const char *s) { return TextWidth(s, 0, (short)strlen(s)); }
static void text_at(int x, int y, const char *s) { MoveTo((short)x, (short)y); DrawText(s, 0, (short)strlen(s)); }
static void text_centre(int cx, int y, const char *s) { text_at(cx - text_w(s) / 2, y, s); }
static void text_right(int rx, int y, const char *s) { text_at(rx - text_w(s), y, s); }
static void set_rect(Rect *r, int l, int t, int rr, int b) { r->left = (short)l; r->top = (short)t; r->right = (short)rr; r->bottom = (short)b; }

/* ---- the window ---- */
static void *win, *rgn;
static int compact, quit, fg = 1, use_wne, active = 1;
static void *m_apple, *m_file, *m_edit, *m_view;
enum { M_APPLE = 128, M_FILE, M_EDIT, M_VIEW };
enum { F_CLOSE = 1, F_QUIT = 3 };
enum { E_UNDO = 1, E_CUT = 3, E_COPY, E_PASTE, E_CLEAR };
enum { V_NORMAL = 1, V_COMPACT, V_REDRAW = 4 };
#define NORMAL_W  488
#define NORMAL_H  292
#define COMPACT_W 232
#define COMPACT_H 50
#define MAX_W     1016
#define MAX_H     720

/* the normal view's parts, from the window's size (layout) */
static Rect r_dial[3], r_lights, r_chart[3], r_plot[3], r_cores, r_lines;
static int lines_y;
static void layout(void)
{
	Rect *p = PORT_RECT(win);
	int w = p->right - p->left, h = p->bottom - p->top;
	if (w < NORMAL_W) w = NORMAL_W;
	if (h < NORMAL_H) h = NORMAL_H;
	for (int i = 0; i < 3; i++) set_rect(&r_dial[i], 6 + 94 * i, 2, 6 + 94 * i + 90, 104);
	set_rect(&r_lights, 292, 2, w - 6, 104);
	int extra = h - NORMAL_H, ch = 60 + extra / 2;
	int cw = (w - 12 - 12) / 3;
	for (int i = 0; i < 3; i++) {
		set_rect(&r_chart[i], 6 + (cw + 6) * i, 108, 6 + (cw + 6) * i + cw, 108 + ch);
		set_rect(&r_plot[i], r_chart[i].left, r_chart[i].top + 13, r_chart[i].right, r_chart[i].bottom);
	}
	int y = 108 + ch + 4;
	set_rect(&r_cores, 6, y, w - 6, y + 66);
	lines_y = y + 70;
	set_rect(&r_lines, 6, lines_y, w - 6, h - 2);
}

/* ---- the dials ---- */
struct dial { const char *title; u32 v, lo, hi, mark1, mark2, nominal; char text[32]; int angle, drawn; };
static struct dial D[3] = { { .title = "Temperature" }, { .title = "ARM clock" }, { .title = "68k speed" } };
static void needle(int cx, int cy, int r, int angle, int thick)
{
	int dx = isin(angle) * r / 16384, dy = -icos(angle) * r / 16384;
	PenSize((short)thick, (short)thick);
	MoveTo((short)(cx - thick / 2), (short)(cy - thick / 2));
	Line((short)dx, (short)dy);
	PenNormal();
}
/* the dial's face: its scale, the marks, the needle, the value under it */
static void draw_dial(int i)
{
	struct dial *d = &D[i];
	Rect b = r_dial[i], face, in, hub;
	int cx = (b.left + b.right) / 2, top = b.top + 14, r = 42, cy = top + r;
	set_rect(&face, cx - r, top, cx + r, top + 2 * r);
	EraseRect(&b);
	font(1);
	text_centre(cx, b.top + 10, d->title);
	if (colour) { ink(C_FACE); PaintOval(&face); ink_black(); }
	/* the scale: a band inside the rim, its warm and hot parts filled */
	if (d->hi > d->lo) {
		int a1 = d->mark1 != ST_UNKNOWN ? dial_angle(d->mark1, d->lo, d->hi) : 120;
		int a2 = d->mark2 != ST_UNKNOWN ? dial_angle(d->mark2, d->lo, d->hi) : 120;
		Rect band = face;
		InsetRect(&band, 2, 2);
		if (colour) fill_arc(&band, -120, a1 + 120, C_GREEN, pat_ltgray);
		if (a2 > a1) fill_arc(&band, a1, a2 - a1, C_AMBER, pat_gray);
		if (a2 < 120) fill_arc(&band, a2, 120 - a2, C_RED, pat_black);
		in = band;
		InsetRect(&in, 6, 6);
		if (colour) { ink(C_FACE); PaintOval(&in); ink_black(); } else EraseOval(&in);
		/* ticks every 30 degrees, longer at the ends and the middle */
		for (int a = -120; a <= 120; a += 30) {
			int long_ = a == -120 || a == 0 || a == 120;
			int r1 = r - 2, r2 = long_ ? r - 13 : r - 9;
			MoveTo((short)(cx + isin(a) * r1 / 16384), (short)(cy - icos(a) * r1 / 16384));
			LineTo((short)(cx + isin(a) * r2 / 16384), (short)(cy - icos(a) * r2 / 16384));
		}
		/* the nominal clock: a small triangle's worth, a short thick tick outside the band */
		if (d->nominal != ST_UNKNOWN) {
			int a = dial_angle(d->nominal, d->lo, d->hi);
			PenSize(2, 2);
			MoveTo((short)(cx + isin(a) * (r - 4) / 16384 - 1), (short)(cy - icos(a) * (r - 4) / 16384 - 1));
			LineTo((short)(cx + isin(a) * (r - 15) / 16384 - 1), (short)(cy - icos(a) * (r - 15) / 16384 - 1));
			PenNormal();
		}
	}
	FrameOval(&face);
	/* the needle, or none when not measured */
	if (d->v != ST_UNKNOWN) {
		ink(i == 0 ? C_RED : i == 1 ? C_BLUE : C_VIOLET);
		needle(cx, cy, r - 8, d->angle, 2);
		ink_black();
	}
	set_rect(&hub, cx - 3, cy - 3, cx + 4, cy + 4);
	PaintOval(&hub);
	/* the value, in the gap at the bottom; the scale's ends beside it */
	font(d->v != ST_UNKNOWN);
	if (d->v == ST_UNKNOWN) ink(C_DIM);
	text_centre(cx, cy + 26, d->text);
	ink_black();
	d->drawn = 1;
}
/* the dials' numbers from a reading; each is drawn again only when its needle or its text changed */
static void dials_update(int force)
{
	const struct st_now *n = &M.now;
	struct dial nd[3];
	for (int i = 0; i < 3; i++) nd[i] = D[i];
	nd[0].v = n->temp_mc; nd[0].lo = 30000; nd[0].hi = 90000; nd[0].mark1 = 60000; nd[0].mark2 = 80000; nd[0].nominal = ST_UNKNOWN;
	fmt_temp(nd[0].text, n->temp_mc);
	u32 clk = n->arm_meas_hz != ST_UNKNOWN ? n->arm_meas_hz : n->arm_hz;
	u32 top = n->arm_max_hz != ST_UNKNOWN ? n->arm_max_hz : clk;
	nd[1].v = clk; nd[1].lo = 0; nd[1].nominal = n->arm_max_hz;
	nd[1].hi = top == ST_UNKNOWN ? 1500000000U : (top / 100000000U + 2) * 100000000U;   /* the nominal clock short of the end */
	nd[1].mark1 = nd[1].mark2 = ST_UNKNOWN;
	fmt_mhz(nd[1].text, clk);
	nd[2].v = n->insn_rate; nd[2].lo = 0; nd[2].hi = mips_scale(M.mips_peak) * 1000000U;
	nd[2].mark1 = nd[2].mark2 = nd[2].nominal = ST_UNKNOWN;
	fmt_mips(nd[2].text, n->insn_rate);
	for (int i = 0; i < 3; i++) {
		nd[i].angle = dial_angle(nd[i].v, nd[i].lo, nd[i].hi);
		int same = D[i].drawn && nd[i].angle == D[i].angle && nd[i].hi == D[i].hi && nd[i].nominal == D[i].nominal &&
		           (nd[i].v == ST_UNKNOWN) == (D[i].v == ST_UNKNOWN) && !memcmp(nd[i].text, D[i].text, sizeof D[i].text);
		D[i] = nd[i];
		if (!same || force) draw_dial(i);
	}
}

/* ---- the lights ---- */
static const char *const light_names[4] = { "Under-voltage", "Frequency capped", "Throttled", "Soft temperature limit" };
static u32 lights_shown = 0x12345678U;
static int lights_flags_shown = -1;
static void light(int x, int y, int on, int past)
{
	Rect r;
	set_rect(&r, x, y, x + 10, y + 10);
	if (on) fill_oval(&r, C_RED, pat_black);
	else if (past) fill_oval(&r, C_AMBER, pat_gray);
	else if (colour) { static const RGBColor off = { 0xE000, 0xE800, 0xE000 }; RGBForeColor(&off); PaintOval(&r); ink_black(); }
	else EraseOval(&r);
	FrameOval(&r);
}
static void draw_lights(void)
{
	Rect b = r_lights;
	u32 t = M.now.throttled;
	int x = b.left;
	EraseRect(&b);
	font(1);
	text_at(x, b.top + 10, "Power and heat");
	font(0);
	text_centre(x + 133, b.top + 24, "now");
	text_centre(x + 166, b.top + 24, "since boot");
	for (int i = 0; i < 4; i++) {
		int y = b.top + 30 + 14 * i;
		if (t == ST_UNKNOWN) ink(C_DIM);
		text_at(x, y + 9, light_names[i]);
		ink_black();
		light(x + 128, y, t != ST_UNKNOWN && TH_NOW(t, i), 0);
		light(x + 161, y, 0, t != ST_UNKNOWN && TH_PAST(t, i));
	}
	const char *say;
	if (t == ST_UNKNOWN) say = "The firmware does not report it.";
	else if (t & 0xF) say = TH_NOW(t, 0) ? "Under-voltage: check the power." : "Slowed down by the firmware.";
	else if (t & 0xF0000) say = "Normal now; slowed down earlier.";
	else say = "Normal: full speed.";
	font(1);
	text_at(x, b.top + 98, say);
	if (M.now.flags & STF_SIMULATED) { font(0); ink(C_DIM); text_right(b.right, b.top + 10, "simulated"); ink_black(); }
	lights_shown = t;
	lights_flags_shown = (int)(M.now.flags & STF_SIMULATED);
}

/* ---- the strip charts ---- */
struct chart { const char *title; const u32 *h; u32 lo, hi, m1, m2; int c; char last[24]; int drawn; };
static struct chart C[3] = { { .title = "Temperature" }, { .title = "Clock" }, { .title = "68k MIPS" } };
static void chart_scales(void)
{
	C[0].h = M.h_temp; C[0].lo = 30000; C[0].hi = 90000; C[0].m1 = 60000; C[0].m2 = 80000; C[0].c = C_RED;
	u32 top = M.now.arm_max_hz != ST_UNKNOWN ? M.now.arm_max_hz / 1000000U : 0;
	for (int i = 0; i < M.hist_n; i++) { u32 v = st_hist(&M, M.h_mhz, i); if (v != ST_UNKNOWN && v > top) top = v; }
	C[1].h = M.h_mhz; C[1].lo = 0; C[1].hi = top ? top + top / 8 : 1500; C[1].m1 = M.now.arm_max_hz != ST_UNKNOWN ? M.now.arm_max_hz / 1000000U : ST_UNKNOWN;
	C[1].m2 = ST_UNKNOWN; C[1].c = C_BLUE;
	C[2].h = M.h_mips; C[2].lo = 0; C[2].hi = mips_scale(M.mips_peak) * 10U; C[2].m1 = C[2].m2 = ST_UNKNOWN; C[2].c = C_VIOLET;
}
static int plot_y(const struct chart *c, const Rect *p, u32 v)
{
	int h = p->bottom - p->top - 3;
	if (v <= c->lo) return p->bottom - 2;
	if (v >= c->hi) return p->top + 1;
	return p->bottom - 2 - (int)((v - c->lo) * (u32)h / (c->hi - c->lo));
}
/* the limit lines and the point at column x (x counts from the plot's right edge leftwards) */
static void plot_column_marks(const struct chart *c, const Rect *p, int x)
{
	u32 m[2] = { c->m1, c->m2 };
	for (int k = 0; k < 2; k++)
		if (m[k] != ST_UNKNOWN && ((x + k) & 1) == 0) {
			int y = plot_y(c, p, m[k]);
			Rect d;
			set_rect(&d, x, y, x + 1, y + 1);
			fill_rect(&d, k ? C_RED : C_AMBER, pat_black);
		}
}
static void plot_segment(const struct chart *c, const Rect *p, int i, int x)
{
	u32 v = st_hist(&M, c->h, i), w = st_hist(&M, c->h, i + 1);
	if (v == ST_UNKNOWN) return;
	ink(c->c);
	if (w == ST_UNKNOWN || x <= p->left + 1) { MoveTo((short)x, (short)plot_y(c, p, v)); LineTo((short)x, (short)plot_y(c, p, v)); }
	else { MoveTo((short)(x - 1), (short)plot_y(c, p, w)); LineTo((short)x, (short)plot_y(c, p, v)); }
	ink_black();
}
static void chart_title(int i)
{
	struct chart *c = &C[i];
	Rect t;
	char b[48];
	set_rect(&t, r_chart[i].left, r_chart[i].top, r_chart[i].right, r_plot[i].top);
	EraseRect(&t);
	int minutes = ((r_plot[i].right - r_plot[i].left - 2) * HIST_EVERY + 30) / 60;
	b[0] = 0;
	cat(b, c->title); cat(b, ", "); fmt_num(b + strlen(b), (u32)minutes); cat(b, " min");
	font(1);
	text_at(t.left, t.bottom - 3, b);
	font(0);
	u32 v = st_hist(&M, c->h, 0);
	if (i == 0) fmt_temp(c->last, v);
	else if (v == ST_UNKNOWN) { c->last[0] = 0; cat(c->last, "--"); }
	else if (i == 1) { fmt_num(c->last, v); cat(c->last, " MHz"); }
	else { fmt_num(c->last, v / 10); cat(c->last, "."); fmt_num(c->last + strlen(c->last), v % 10); }
	if (v == ST_UNKNOWN) { c->last[0] = 0; cat(c->last, "--"); }
	text_right(t.right - 1, t.bottom - 3, c->last);
}
static void draw_chart(int i)
{
	struct chart *c = &C[i];
	Rect p = r_plot[i], in = p;
	InsetRect(&in, 1, 1);
	chart_title(i);
	EraseRect(&in);
	if (colour) {                               /* a faint grid: quarters */
		ink(C_GRID);
		for (int q = 1; q < 4; q++) { int y = p.top + (p.bottom - p.top) * q / 4; MoveTo((short)(p.left + 1), (short)y); LineTo((short)(p.right - 2), (short)y); }
		ink_black();
	}
	for (int x = in.right - 1, k = 0; x >= in.left; x--, k++) {
		plot_column_marks(c, &p, x);
		plot_segment(c, &p, k, x);
	}
	FrameRect(&p);
	c->drawn = 1;
}
/* a new point: the plot moved one pixel left (ScrollRect, when the window is unobscured), the new column drawn */
static u32 chart_hi_shown[3];
static void charts_point(int force)
{
	chart_scales();
	for (int i = 0; i < 3; i++) {
		struct chart *c = &C[i];
		Rect p = r_plot[i], in = p;
		InsetRect(&in, 1, 1);
		char **vis = *(char ***)((char *)win + 24);         /* the port's visRgn */
		char *vr = deref(vis);
		Rect *bb = (Rect *)(vr + 2);
		int plain = *(short *)vr == 10 && bb->top <= p.top && bb->left <= p.left && bb->bottom >= p.bottom && bb->right >= p.right;
		if (force || !c->drawn || !plain || chart_hi_shown[i] != c->hi || colour) {
			/* colour screens: the grid would scroll too; a whole plot is cheap there (faster screens) */
			draw_chart(i);
		} else {
			ScrollRect(&in, -1, 0, rgn);
			plot_column_marks(c, &p, in.right - 1);
			plot_segment(c, &p, 0, in.right - 1);
			chart_title(i);
		}
		chart_hi_shown[i] = c->hi;
	}
}

/* ---- the cores ---- */
static u32 cores_shown[4] = { 1, 1, 1, 1 }, mips_shown = 1;
static char core_names_shown[4][25];
static void bar(const Rect *r, u32 hundredths, int c)
{
	Rect in = *r, f;
	FrameRect(r);
	InsetRect(&in, 1, 1);
	EraseRect(&in);
	if (hundredths == ST_UNKNOWN) return;
	if (hundredths > 10000) hundredths = 10000;
	f = in;
	f.right = (short)(in.left + (int)((u32)(in.right - in.left) * hundredths / 10000U));
	if (f.right > f.left) fill_rect(&f, hundredths >= 9000 ? C_RED : hundredths >= 7000 ? C_AMBER : c, pat_dkgray);
}
static void draw_cores(void)
{
	Rect b = r_cores, r;
	char t[48];
	EraseRect(&b);
	font(1);
	text_at(b.left, b.top + 10, "ARM cores");
	font(0);
	text_right(b.right, b.top + 10, "load over the last second");
	for (int i = 0; i < 4; i++) {
		int y = b.top + 14 + 13 * i;
		const struct st_now *n = &M.now;
		t[0] = (char)('0' + i); t[1] = 0;
		text_at(b.left, y + 9, t);
		text_at(b.left + 10, y + 9, n->core[i].name[0] ? n->core[i].name : "?");
		set_rect(&r, b.left + 130, y + 1, b.left + 310, y + 11);
		if (i == 0 && n->core[0].busy == ST_UNKNOWN) {
			/* the 68k's core never idles: its bar is the speed, against the dial's scale */
			u32 top = mips_scale(M.mips_peak) * 1000000U;
			u32 h = n->insn_rate == ST_UNKNOWN ? ST_UNKNOWN : (u32)((n->insn_rate / 10000U) * 10000U / (top / 10000U));
			bar(&r, h, C_VIOLET);
			t[0] = 0; cat(t, "always running: "); fmt_mips(t + strlen(t), n->insn_rate);
		} else {
			bar(&r, n->core[i].busy, C_TEAL);
			if (n->core[i].busy == ST_UNKNOWN) { t[0] = 0; cat(t, "not measured"); }
			else fmt_pct(t, n->core[i].busy);
		}
		text_at(b.left + 316, y + 9, t);
		cores_shown[i] = n->core[i].busy;
		memcpy(core_names_shown[i], n->core[i].name, 25);
	}
	mips_shown = M.now.insn_rate;
}
static int cores_changed(void)
{
	for (int i = 0; i < 4; i++)
		if (cores_shown[i] / 100 != M.now.core[i].busy / 100 || memcmp(core_names_shown[i], M.now.core[i].name, 25)) return 1;
	return mips_shown / 1000000U != M.now.insn_rate / 1000000U;
}

/* ---- the lines of text at the bottom ---- */
#define NLINES 4
static char lines[NLINES][160], lines_shown[NLINES][160];
static void line_memory(char *s)
{
	const struct st_now *n = &M.now;
	char b[24];
	s[0] = 0;
	cat(s, "Pi memory: ");
	if (n->heap_free != ST_UNKNOWN) { cat(s, fmt_mb(b, n->heap_free)); cat(s, " free of "); cat(s, fmt_mb(b, n->heap_total)); cat(s, " (kernel heap)"); }
	else cat(s, "the kernel's heap not measured");
	if (n->jit_total && n->jit_total != ST_UNKNOWN) { cat(s, "; JIT cache "); cat(s, fmt_mb(b, n->jit_free)); cat(s, " free of "); cat(s, fmt_mb(b, n->jit_total)); }
}
static void line_network(char *s)
{
	const struct st_now *n = &M.now;
	char b[24];
	int any = 0;
	s[0] = 0;
	cat(s, "Network: ");
	for (int u = 1; u >= 0; u--) {            /* Ethernet first: the usual one in the testbed and on the SE */
		if (n->net[u].kind != 1 && n->net[u].kind != 2) continue;
		if (any) cat(s, "; ");
		cat(s, n->net[u].kind == 1 ? "Wi-Fi " : "Ethernet ");
		if (!n->net[u].link) cat(s, "down");
		else { cat(s, "in "); cat(s, fmt_rate(b, M.rx_bps[u])); cat(s, ", out "); cat(s, fmt_rate(b, M.tx_bps[u])); }
		any = 1;
	}
	if (!any) cat(s, "no network units");
}
static void line_video(char *s)
{
	static const char *const st[] = { "idle", "buffering", "playing", "paused", "at the end", "stopped by an error" };
	const struct st_now *n = &M.now;
	char b[24];
	s[0] = 0;
	cat(s, "Video card: ");
	if (n->vc_state == ST_UNKNOWN) { cat(s, "not in this Mac"); return; }
	cat(s, n->vc_state < 6 ? st[n->vc_state] : "?");
	if (n->vc_state == 0 || n->vc_state > 3) return;
	cat(s, ", "); fmt_num(b, (n->vc_load + 5) / 10); cat(s, b); cat(s, "% of core 1; decode ");
	fmt_num(b, (n->vc_decode_us + 500) / 1000); cat(s, b); cat(s, " ms, draw ");
	fmt_num(b, (n->vc_draw_us + 500) / 1000); cat(s, b); cat(s, " ms; ");
	if (n->vc_buffer_ms != ST_UNKNOWN) { cat(s, fmt_secs(b, n->vc_buffer_ms)); cat(s, " buffered; "); }
	fmt_num(b, n->vc_dropped); cat(s, b); cat(s, " late, ");
	fmt_num(b, n->vc_underruns); cat(s, b); cat(s, n->vc_underruns == 1 ? " time dry" : " times dry");
}
static void line_sound(char *s)
{
	static const char *const outs[] = { "no output", "HDMI", "the jack", "a file" };
	static const char *const sst[] = { "idle", "playing", "paused", "at the end", "stopped by an error" };
	const struct st_now *n = &M.now;
	char b[24];
	s[0] = 0;
	cat(s, "Sound card: ");
	if (n->snd_out == ST_UNKNOWN) { cat(s, "not in this Mac"); return; }
	cat(s, n->snd_out < 4 ? outs[n->snd_out] : "?");
	cat(s, n->snd_playing == 1 ? ", the Mac's sound playing" : ", the Mac's sound quiet");
	fmt_num(b, n->snd_underruns); cat(s, " ("); cat(s, b); cat(s, " frames short)");
	if (n->str_state != ST_UNKNOWN && n->str_state != 0) {
		cat(s, "; music "); cat(s, n->str_state < 5 ? sst[n->str_state] : "?");
		if (n->str_state == 1) { cat(s, ", "); fmt_num(b, (n->str_load + 5) / 10); cat(s, b); cat(s, "% of core 1"); }
		if (n->str_underruns) { cat(s, ", "); fmt_num(b, n->str_underruns); cat(s, b); cat(s, " frames short"); }
	}
}
static void lines_update(int force)
{
	line_memory(lines[0]); line_network(lines[1]); line_video(lines[2]); line_sound(lines[3]);
	font(0);
	for (int i = 0; i < NLINES; i++) {
		if (!force && !strcmp_(lines[i], lines_shown[i])) continue;
		Rect r;
		set_rect(&r, r_lines.left, lines_y + 12 * i, r_lines.right - 12, lines_y + 12 * i + 12);
		EraseRect(&r);
		ClipRect(&r);
		/* the part before the colon in bold, as a label */
		char *colon = lines[i];
		while (*colon && *colon != ':') colon++;
		int k = (int)(colon - lines[i]) + 1;
		font(1);
		MoveTo((short)r.left, (short)(r.top + 9));
		DrawText(lines[i], 0, (short)k);
		font(0);
		DrawText(lines[i], (short)k, (short)(strlen(lines[i]) - (unsigned long)k));
		ClipRect(PORT_RECT(win));
		memcpy(lines_shown[i], lines[i], sizeof lines[i]);
	}
}

/* ---- the compact view: the temperature, the clock, the MIPS, one light ---- */
static char mini_shown[96];
static void mini_bar(int x, int y, int w, u32 v, u32 lo, u32 hi, u32 m1, u32 m2, int c)
{
	Rect r, f;
	set_rect(&r, x, y, x + w, y + 7);
	FrameRect(&r);
	f = r;
	InsetRect(&f, 1, 1);
	EraseRect(&f);
	if (v != ST_UNKNOWN && hi > lo) {
		int a = dial_angle(v, lo, hi) + 120;        /* 0..240 */
		f.right = (short)(f.left + (f.right - f.left) * a / 240);
		if (f.right > f.left) fill_rect(&f, c, pat_dkgray);
	}
	u32 m[2] = { m1, m2 };
	for (int k = 0; k < 2; k++)
		if (m[k] != ST_UNKNOWN) {
			int mx = x + 1 + (w - 2) * (dial_angle(m[k], lo, hi) + 120) / 240;
			MoveTo((short)mx, (short)(y - 2)); LineTo((short)mx, (short)(y + 8));
		}
}
static void draw_mini(int force)
{
	const struct st_now *n = &M.now;
	char a[24], b[24], c[24], key[96];
	u32 clk = n->arm_meas_hz != ST_UNKNOWN ? n->arm_meas_hz : n->arm_hz;
	fmt_temp(a, n->temp_mc); fmt_mhz(b, clk); fmt_mips(c, n->insn_rate);
	if (n->temp_mc == ST_UNKNOWN) { a[0] = 0; cat(a, "-- \xA1" "C"); }
	key[0] = 0; cat(key, a); cat(key, b); cat(key, c);
	{ char t[12]; fmt_num(t, n->throttled); cat(key, t); fmt_num(t, (u32)dial_angle(n->temp_mc, 30000, 90000) + 200); cat(key, t); }
	if (!force && !strcmp_(key, mini_shown)) return;
	memcpy(mini_shown, key, sizeof key);
	EraseRect(PORT_RECT(win));
	font(1);
	text_at(6, 12, a); text_at(80, 12, b); text_at(154, 12, c);
	mini_bar(6, 17, 66, n->temp_mc, 30000, 90000, 60000, 80000,
	         n->temp_mc == ST_UNKNOWN || n->temp_mc < 60000 ? C_GREEN : n->temp_mc < 80000 ? C_AMBER : C_RED);
	u32 top = n->arm_max_hz != ST_UNKNOWN ? n->arm_max_hz : clk;
	mini_bar(80, 17, 66, clk, 0, top == ST_UNKNOWN ? 1500000000U : top, ST_UNKNOWN, ST_UNKNOWN, C_BLUE);
	mini_bar(154, 17, 72, n->insn_rate, 0, mips_scale(M.mips_peak) * 1000000U, ST_UNKNOWN, ST_UNKNOWN, C_VIOLET);
	u32 t = n->throttled;
	light(6, 33, t != ST_UNKNOWN && (t & 0xF), t != ST_UNKNOWN && (t & 0xF0000));
	font(0);
	const char *say = t == ST_UNKNOWN ? "power state not reported" : TH_NOW(t, 0) ? "under-voltage now" : TH_NOW(t, 1) ? "frequency capped now" :
	                  TH_NOW(t, 2) ? "throttled now" : TH_NOW(t, 3) ? "soft temperature limit now" :
	                  (t & 0xF0000) ? "normal now, slowed earlier" : "normal, full speed";
	text_at(22, 42, say);
	if (n->flags & STF_SIMULATED) { ink(C_DIM); text_right(COMPACT_W - 6, 42, "simulated"); ink_black(); }
}

/* ---- everything ---- */
static void no_page_text(void)
{
	Rect r;
	EraseRect(PORT_RECT(win));
	set_rect(&r, 8, 8, PORT_RECT(win)->right - 8, PORT_RECT(win)->bottom - 8);
	font(0);
	const char *s = "No Aurora68 card in slot $9, or no status page on it: this Macintosh is not an Aurora68, or its kernel is too old.";
	TETextBox(s, (long)strlen(s), &r, 0);
}
static void colour_check(void)
{
	colour = 0;
	if (!has_cqd()) return;
	Rect g = *PORT_RECT(win);
	Point p = { 0, 0 };
	LocalToGlobal(&p);
	g.top += p.v; g.bottom += p.v; g.left += p.h; g.right += p.h;
	char **gd = (char **)GetMaxDevice(&g);
	if (!gd) return;
	char **pm = *(char ***)(deref(gd) + 22);
	colour = pm && *(short *)(deref(pm) + 32) >= 4;
}
/* the grow box alone: DrawGrowIcon also draws the lines of scroll bars this window has not */
static void grow_box(void)
{
	Rect c = *PORT_RECT(win);
	c.left = (short)(c.right - 15); c.top = (short)(c.bottom - 15);
	ClipRect(&c);
	DrawGrowIcon(win);
	ClipRect(PORT_RECT(win));
}
static int laid_out;
static void draw_all(void)
{
	SetPort(win);
	colour_check();
	paper_white();
	if (no_page) { no_page_text(); return; }
	if (compact) { draw_mini(1); laid_out = 1; return; }
	EraseRect(PORT_RECT(win));
	layout();
	for (int i = 0; i < 3; i++) D[i].drawn = 0;
	dials_update(1);
	draw_lights();
	for (int i = 0; i < 3; i++) C[i].drawn = 0;
	charts_point(1);
	draw_cores();
	lines_update(1);
	grow_box();
	laid_out = 1;
}
/* a new reading: only what changed */
static void refresh(int new_point)
{
	SetPort(win);
	if (no_page || !laid_out) return;
	if (compact) { draw_mini(0); return; }
	dials_update(0);
	if (lights_shown != M.now.throttled || lights_flags_shown != (int)(M.now.flags & STF_SIMULATED)) draw_lights();
	if (new_point) charts_point(0);
	if (cores_changed()) draw_cores();
	lines_update(0);
}
static int strcmp_(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return (unsigned char)*a - (unsigned char)*b;
}

/* ---- screens and the window's place ---- */
#define MAX_SCR 8
static Rect *screen_bits(void)
{
	char *a5;
	__asm__ volatile("move.l %%a5,%0" : "=r"(a5));
	return (Rect *)(*(char **)a5 - 116);
}
static int screens(Rect *s)
{
	int n = 0;
	if (!has_cqd()) { s[0] = *screen_bits(); return 1; }
	s[n++] = *(Rect *)(deref(LM_MAINDEVICE) + 34);
	for (char **g = LM_DEVICELIST; g && n < MAX_SCR; g = *(char ***)(deref(g) + 30)) {
		char *gd = deref(g);
		if (g == LM_MAINDEVICE || !(*(unsigned short *)(gd + 20) & 0x8000)) continue;
		s[n++] = *(Rect *)(gd + 34);
	}
	return n;
}
static int sect(const Rect *a, const Rect *b, Rect *o)
{
	o->top = a->top > b->top ? a->top : b->top;
	o->left = a->left > b->left ? a->left : b->left;
	o->bottom = a->bottom < b->bottom ? a->bottom : b->bottom;
	o->right = a->right < b->right ? a->right : b->right;
	return o->bottom > o->top && o->right > o->left;
}
static void content_global(Rect *r)
{
	Point p = { 0, 0 };
	SetPort(win);
	*r = *PORT_RECT(win);
	LocalToGlobal(&p);
	r->top += p.v; r->bottom += p.v; r->left += p.h; r->right += p.h;
}
/* is the title bar of a window with this content seen on some screen */
static int on_screen(const Rect *c)
{
	Rect s[MAX_SCR], o, bar = { (short)(c->top - 18), c->left, (short)(c->top - 2), c->right };
	int n = screens(s);
	for (int i = 0; i < n; i++) {
		Rect u = s[i];
		if (i == 0) u.top += mbar();
		if (sect(&bar, &u, &o) && o.right - o.left >= 40 && o.bottom - o.top >= 12) return 1;
	}
	return 0;
}
/* the first window: the normal view at the main screen's top left, as on an SE (8, 40) */
static void default_rect(Rect *b, int mini)
{
	Rect s[MAX_SCR];
	screens(s);
	b->top = (short)(s[0].top + mbar() + 20); b->left = (short)(s[0].left + 8);
	b->right = (short)(b->left + (mini ? COMPACT_W : NORMAL_W));
	b->bottom = (short)(b->top + (mini ? COMPACT_H : NORMAL_H));
}

/* ---- the File Manager with directory IDs, for the preferences (as mac/pislots) ---- */
#define FS_CALL(name, trap) static short name(ParamBlock *pb) { \
	register ParamBlock *a0 __asm__("a0") = pb; register long d0 __asm__("d0"); \
	__asm__ volatile(".short " #trap : "=d"(d0), "+a"(a0) : : "d1", "d2", "a1", "memory", "cc"); \
	return (short)d0; }
FS_CALL(fs_hopen, 0xA200)
FS_CALL(fs_read, 0xA002)
FS_CALL(fs_hcreate, 0xA208)
FS_CALL(fs_hgetfinfo, 0xA20C)
FS_CALL(fs_hsetfinfo, 0xA20D)
#define DIR_ID(pb) ((pb)->u.f.ioFlNum)
static short find_folder(short vref, long type, short *fvref, long *fdir)
{
	short err;
	__asm__ volatile("clr.w -(%%sp)\n\t"
	                 "move.w %1,-(%%sp)\n\t"
	                 "move.l %2,-(%%sp)\n\t"
	                 "move.w #0x0100,-(%%sp)\n\t"
	                 "move.l %3,-(%%sp)\n\t"
	                 "move.l %4,-(%%sp)\n\t"
	                 "moveq #0,%%d0\n\t"
	                 ".short 0xA823\n\t"
	                 "move.w (%%sp)+,%0"
	                 : "=d"(err) : "d"(vref), "d"(type), "a"(fvref), "a"(fdir) : "d0", "d1", "d2", "a0", "a1", "memory", "cc");
	return err;
}
static short sys_folder(void)
{
	static short rec[8];
	register void *a0 __asm__("a0") = rec;
	register long d0 __asm__("d0") = 1;
	__asm__ volatile(".short 0xA090" : "+d"(d0), "+a"(a0) : : "d1", "d2", "a1", "memory", "cc");
	return rec[7];
}
/* "Aurora68 Status Prefs": 'PST2', the view, the normal view's content rectangle, the compact one's */
#define PREF_MAGIC TYPE('P', 'S', 'T', '2')
static Rect normal_rect, mini_rect;
static int pref_place(short *vref, long *dir)
{
	long resp = 0;
	if (has_os_trap((short)0xA1AD) && gestalt(TYPE('f', 'o', 'l', 'd'), &resp) == 0 && (resp & 1))
		return find_folder(-32768, TYPE('p', 'r', 'e', 'f'), vref, dir) == 0;
	*vref = sys_folder(); *dir = 0;
	return *vref != 0;
}
struct prefs { long magic, compact; Rect normal, mini; };
static int load_prefs(void)
{
	ParamBlock pb;
	short vref, ref;
	long dir;
	struct prefs p;
	if (!pref_place(&vref, &dir)) return 0;
	memset(&pb, 0, sizeof pb);
	pb.ioNamePtr = pstr(pbuf, "Aurora68 Status Prefs"); pb.ioVRefNum = vref; DIR_ID(&pb) = dir; pb.ioPermssn = 1;
	if (fs_hopen(&pb)) return 0;
	ref = pb.ioRefNum;
	memset(&pb, 0, sizeof pb);
	pb.ioRefNum = ref; pb.u.io.ioBuffer = &p; pb.u.io.ioReqCount = sizeof p; pb.u.io.ioPosMode = fsFromStart;
	short err = fs_read(&pb);
	mac_PBClose(&pb);
	if (err || pb.u.io.ioActCount != sizeof p || p.magic != (long)PREF_MAGIC) return 0;
	compact = p.compact != 0;
	normal_rect = p.normal; mini_rect = p.mini;
	return 1;
}
static void save_prefs(void)
{
	ParamBlock pb;
	short vref, ref;
	long dir;
	struct prefs p;
	if (!win || !pref_place(&vref, &dir)) return;
	if (compact) content_global(&mini_rect); else content_global(&normal_rect);
	p.magic = (long)PREF_MAGIC; p.compact = compact; p.normal = normal_rect; p.mini = mini_rect;
	pstr(pbuf, "Aurora68 Status Prefs");
	memset(&pb, 0, sizeof pb);
	pb.ioNamePtr = pbuf; pb.ioVRefNum = vref; DIR_ID(&pb) = dir;
	if (!fs_hcreate(&pb)) {
		memset(&pb, 0, sizeof pb);
		pb.ioNamePtr = pbuf; pb.ioVRefNum = vref; DIR_ID(&pb) = dir;
		if (!fs_hgetfinfo(&pb)) {
			pb.u.f.fdType = TYPE('p', 'r', 'e', 'f'); pb.u.f.fdCreator = TYPE('P', 'I', 'S', 'T');
			DIR_ID(&pb) = dir;
			fs_hsetfinfo(&pb);
		}
	}
	memset(&pb, 0, sizeof pb);
	pb.ioNamePtr = pbuf; pb.ioVRefNum = vref; DIR_ID(&pb) = dir; pb.ioPermssn = 3;
	if (fs_hopen(&pb)) return;
	ref = pb.ioRefNum;
	memset(&pb, 0, sizeof pb);
	pb.ioRefNum = ref; pb.u.io.ioBuffer = &p; pb.u.io.ioReqCount = sizeof p; pb.u.io.ioPosMode = fsFromStart;
	mac_PBWrite(&pb);
	pb.u.io.ioMisc = sizeof p;
	mac_PBSetEOF(&pb);
	mac_PBClose(&pb);
	memset(&pb, 0, sizeof pb);
	pb.ioVRefNum = vref;
	mac_PBFlushVol(&pb);
}

/* ---- the two views ---- */
static void fix_view_menu(void) { CheckItem(m_view, V_NORMAL, !compact); CheckItem(m_view, V_COMPACT, compact); }
static void set_view(int mini)
{
	Rect c;
	if (mini == compact) return;
	content_global(&c);
	if (compact) mini_rect = c; else normal_rect = c;
	compact = mini;
	Rect *want = compact ? &mini_rect : &normal_rect;
	int w = compact ? COMPACT_W : (want->right - want->left >= NORMAL_W ? want->right - want->left : NORMAL_W);
	int h = compact ? COMPACT_H : (want->bottom - want->top >= NORMAL_H ? want->bottom - want->top : NORMAL_H);
	/* the top left corner stays where it is; the window grows into the screen it is on */
	Rect s[MAX_SCR], nr = { c.top, c.left, (short)(c.top + h), (short)(c.left + w) };
	int n = screens(s);
	for (int i = 0; i < n; i++) {
		Rect o;
		if (!sect(&c, &s[i], &o)) continue;
		if (nr.right > s[i].right - 4) { int d = nr.right - (s[i].right - 4); nr.left -= d; nr.right -= d; }
		if (nr.bottom > s[i].bottom - 4) { int d = nr.bottom - (s[i].bottom - 4); nr.top -= d; nr.bottom -= d; }
		int mt = s[i].top + (i == 0 ? mbar() : 0) + 20;
		if (nr.top < mt) { nr.bottom += mt - nr.top; nr.top = (short)mt; }
		if (nr.left < s[i].left + 4) { nr.right += s[i].left + 4 - nr.left; nr.left = (short)(s[i].left + 4); }
		break;
	}
	MoveWindow(win, nr.left, nr.top, 0);
	SizeWindow(win, (short)w, (short)h, 1);
	SetPort(win);
	InvalRect(PORT_RECT(win));
	fix_view_menu();
}

/* ---- the About box (as mac/pislots) ---- */
static int next_event(EventRecord *ev, long sleep)
{
	if (use_wne) return WaitNextEvent(everyEvent, ev, sleep, 0);
	SystemTask();
	return GetNextEvent(everyEvent, ev);
}
static void update(void *w);
static void modal_box(int w, int h, const char *head, const char *body)
{
	Rect s[MAX_SCR];
	screens(s);
	int sw = s[0].right - s[0].left, sh = s[0].bottom - s[0].top - mbar();
	Rect r;
	r.left = (short)(s[0].left + (sw - w) / 2); r.right = (short)(r.left + w);
	r.top = (short)(s[0].top + mbar() + (sh - h) / 3); r.bottom = (short)(r.top + h);
	void *box = NewWindow(0, &r, pstr(pbuf, ""), 1, 1 /* dBoxProc */, (void *)-1, 0, 0);
	SetPort(box);
	Rect ok = { (short)(h - 34), (short)(w - 82), (short)(h - 14), (short)(w - 18) };
	Rect text = { 38, 18, (short)(h - 42), (short)(w - 18) };
	void *btn = NewControl(box, &ok, pstr(pbuf, "OK"), 1, 0, 0, 1, pushButProc, 0);
	EventRecord ev;
	int done = 0;
	while (!done) {
		if (!next_event(&ev, 10)) continue;
		if (ev.what == updateEvt && (void *)ev.message != box) { update((void *)ev.message); SetPort(box); continue; }
		switch (ev.what) {
		case updateEvt:
			BeginUpdate(box);
			TextFont(0); TextSize(12); TextFace(1); MoveTo(18, 28); DrawString(pstr(pbuf, head)); TextFace(0);
			TextFont(3); TextSize(10);
			TETextBox(body, (long)strlen(body), &text, 0);
			TextFont(0); TextSize(12);
			DrawControls(box);
			{ Rect o = ok; InsetRect(&o, -4, -4); PenSize(3, 3); FrameRoundRect(&o, 16, 16); PenNormal(); }
			EndUpdate(box);
			break;
		case mouseDown: {
			Point p = ev.where;
			void *w2 = 0, *ctl = 0;
			if (FindWindow(pt_long(p), &w2) != inContent || w2 != box) { SysBeep(1); break; }
			GlobalToLocal(&p);
			if (FindControl(pt_long(p), box, &ctl) && ctl == btn && TrackControl(btn, pt_long(p), 0)) done = 1;
			break;
		}
		case keyDown: case autoKey: {
			int c = (int)(ev.message & charCodeMask);
			if (c == 0x0D || c == 0x03 || c == 0x1B || ((ev.modifiers & cmdKey) && c == '.')) {
				HiliteControl(btn, 1);
				unsigned long t = LM_TICKS + 8;
				while (LM_TICKS < t) { }
				done = 1;
			}
			break;
		}
		case osEvt:
			if (((unsigned long)ev.message >> 24) == suspendResumeMessage) fg = ev.message & resumeFlag;
			break;
		}
	}
	DisposeWindow(box);
	SetPort(win);
}
static void about(void)
{
	modal_box(360, 178, "Aurora68 Status 2.0",
	          "The Raspberry Pi inside this Macintosh: its temperature, its clock and whether the firmware is "
	          "slowing it down, the 68k's speed, what each ARM core is doing, its memory, its network and its "
	          "cards, once a second.\r\r"
	          "Part of Aurora68. Free software under the Mozilla Public License 2.0.");
}

/* ---- Edit > Copy: every number as text ---- */
static char report[1400];
static void copy_report(void)
{
	const struct st_now *n = &M.now;
	char b[32];
	report[0] = 0;
	cat(report, "Aurora68 Status: "); cat(report, n->build); cat(report, "\r");
	cat(report, "Uptime: "); fmt_num(b, n->uptime_ms / 1000); cat(report, b); cat(report, " s\r");
	cat(report, "Temperature: "); cat(report, fmt_temp(b, n->temp_mc));
	if (n->temp_max_mc != ST_UNKNOWN) { cat(report, " (limit "); cat(report, fmt_temp(b, n->temp_max_mc)); cat(report, ")"); }
	cat(report, "\rARM clock: "); cat(report, fmt_mhz(b, n->arm_hz)); cat(report, ", measured "); cat(report, fmt_mhz(b, n->arm_meas_hz));
	cat(report, ", nominal "); cat(report, fmt_mhz(b, n->arm_max_hz));
	cat(report, "\r68k: "); cat(report, fmt_mips(b, n->insn_rate));
	cat(report, "\rPower: ");
	if (n->throttled == ST_UNKNOWN) cat(report, "not reported");
	else {
		int any = 0;
		for (int i = 0; i < 4; i++) if (TH_NOW(n->throttled, i)) { cat(report, any ? ", " : ""); cat(report, light_names[i]); cat(report, " now"); any = 1; }
		for (int i = 0; i < 4; i++) if (TH_PAST(n->throttled, i)) { cat(report, any ? ", " : ""); cat(report, light_names[i]); cat(report, " since boot"); any = 1; }
		if (!any) cat(report, "normal");
	}
	if (n->flags & STF_SIMULATED) cat(report, " (simulated)");
	cat(report, "\r");
	for (int i = 0; i < 4; i++) {
		b[0] = (char)('0' + i); b[1] = 0;
		cat(report, "Core "); cat(report, b); cat(report, " ("); cat(report, n->core[i].name); cat(report, "): ");
		cat(report, n->core[i].busy == ST_UNKNOWN ? (i ? "not measured" : "always running") : fmt_pct(b, n->core[i].busy));
		cat(report, "\r");
	}
	for (int i = 0; i < NLINES; i++) { cat(report, lines[i]); cat(report, "\r"); }
	ZeroScrap();
	PutScrap((long)strlen(report), TYPE('T', 'E', 'X', 'T'), report);
}

/* ---- menus ---- */
static void setup_menus(void)
{
	unsigned char t[2] = { 1, 0x14 };
	m_apple = NewMenu(M_APPLE, t);
	AppendMenu(m_apple, pstr(pbuf, "About Aurora68 Status\xC9;(-"));
	AddResMenu(m_apple, TYPE('D', 'R', 'V', 'R'));
	InsertMenu(m_apple, 0);
	m_file = NewMenu(M_FILE, pstr(pbuf, "File"));
	AppendMenu(m_file, pstr(pbuf, "Close/W;(-;Quit/Q"));
	InsertMenu(m_file, 0);
	m_edit = NewMenu(M_EDIT, pstr(pbuf, "Edit"));
	AppendMenu(m_edit, pstr(pbuf, "Undo/Z;(-;Cut/X;Copy/C;Paste/V;Clear"));
	InsertMenu(m_edit, 0);
	m_view = NewMenu(M_VIEW, pstr(pbuf, "View"));
	AppendMenu(m_view, pstr(pbuf, "Normal View/1;Compact View/2;(-;Redraw/R"));
	InsertMenu(m_view, 0);
	DrawMenuBar();
}
static void enable(void *m, int item, int on) { if (on) EnableItem(m, item); else DisableItem(m, item); }
static void fix_menus(void)
{
	int da = FrontWindow() != win;
	enable(m_edit, E_UNDO, da); enable(m_edit, E_CUT, da); enable(m_edit, E_PASTE, da); enable(m_edit, E_CLEAR, da);
	enable(m_edit, E_COPY, da || !no_page);
	fix_view_menu();
}
static void do_menu(long mr)
{
	int id = (int)(mr >> 16), item = (int)(mr & 0xFFFF);
	SetPort(win);
	if (id == M_APPLE) {
		if (item == 1) about();
		else { GetItem(m_apple, item, pbuf); OpenDeskAcc(pbuf); SetPort(win); }
	} else if (id == M_FILE) {
		if (item == F_CLOSE || item == F_QUIT) quit = 1;
	} else if (id == M_EDIT) {
		if (item <= E_CLEAR && SystemEdit(item - 1)) { HiliteMenu(0); return; }
		if (item == E_COPY) copy_report();
	} else if (id == M_VIEW) {
		if (item == V_NORMAL) set_view(0);
		else if (item == V_COMPACT) set_view(1);
		else if (item == V_REDRAW) InvalRect(PORT_RECT(win));
	}
	HiliteMenu(0);
}

/* ---- events ---- */
static void mouse_down(EventRecord *ev)
{
	void *w = 0;
	int part = FindWindow(pt_long(ev->where), &w);
	switch (part) {
	case inMenuBar: fix_menus(); do_menu(MenuSelect(pt_long(ev->where))); break;
	case inSysWindow: SystemClick(ev, w); break;
	case inDrag:
		DragWindow(w, pt_long(ev->where), RGN_BBOX(LM_GRAYRGN));
		if (w == win) { int was = colour; SetPort(win); colour_check(); if (was != colour) InvalRect(PORT_RECT(win)); }
		break;
	case inGoAway: if (w == win && TrackGoAway(w, pt_long(ev->where))) quit = 1; break;
	case inGrow:
		if (w == win && !compact) {
			Rect lim = { NORMAL_H, NORMAL_W, MAX_H, MAX_W };
			long sz = GrowWindow(w, pt_long(ev->where), &lim);
			if (sz) { SizeWindow(w, (short)(sz & 0xFFFF), (short)(sz >> 16), 1); SetPort(win); InvalRect(PORT_RECT(win)); }
		}
		break;
	case inZoomIn: case inZoomOut:
		if (w == win && TrackBox(w, pt_long(ev->where), part)) set_view(!compact);
		break;
	case inContent:
		if (w != FrontWindow()) SelectWindow(w);
		break;
	}
}
static void key_down(EventRecord *ev)
{
	int c = (int)(ev->message & charCodeMask);
	if (ev->modifiers & cmdKey) { if (c != '.') { fix_menus(); do_menu(MenuKey(c)); } return; }
	if (c == 'r' || c == 'R') { SetPort(win); InvalRect(PORT_RECT(win)); }
	else if (c == 'q' || c == 'Q') quit = 1;
}
static void update(void *w)
{
	if (w != win) return;
	SetPort(win);
	BeginUpdate(win);
	draw_all();
	EndUpdate(win);
}

int main(void)
{
	use_wne = has_tool_trap((short)0xA860);
	setup_menus();
	rgn = (void *)NewRgn();
	Rect b;
	if (!load_prefs()) { compact = 0; default_rect(&normal_rect, 0); default_rect(&mini_rect, 1); }
	/* each view's place, put back only where a screen still shows its title bar */
	if (normal_rect.right - normal_rect.left < NORMAL_W) normal_rect.right = (short)(normal_rect.left + NORMAL_W);
	if (normal_rect.bottom - normal_rect.top < NORMAL_H) normal_rect.bottom = (short)(normal_rect.top + NORMAL_H);
	if (normal_rect.right - normal_rect.left > MAX_W || normal_rect.bottom - normal_rect.top > MAX_H || !on_screen(&normal_rect)) default_rect(&normal_rect, 0);
	mini_rect.right = (short)(mini_rect.left + COMPACT_W); mini_rect.bottom = (short)(mini_rect.top + COMPACT_H);
	if (!on_screen(&mini_rect)) default_rect(&mini_rect, 1);
	/* a kept place, all of it on the screen it is mostly on: from the HDMI screen to the SE's, the normal view
	   could reach past the SE's right and bottom edges, its grow and zoom boxes out of reach (23 November 2026) */
	scr_fit(&normal_rect); scr_fit(&mini_rect);
	b = compact ? mini_rect : normal_rect;
	pstr(pbuf, "Aurora68 Status");
	win = has_cqd() ? (void *)NewCWindow(0, &b, pbuf, 1, zoomDocProc, (void *)-1, 1, 0)
	                : (void *)NewWindow(0, &b, pbuf, 1, zoomDocProc, (void *)-1, 1, 0);
	SetPort(win);
	fix_view_menu();

	int r = read_page();
	no_page = r < 0;
	st_update(&M, &reading);
	unsigned long last_look = LM_TICKS;

	EventRecord ev;
	while (!quit) {
		if (next_event(&ev, fg ? 20 : 40)) {
			switch (ev.what) {
			case mouseDown: mouse_down(&ev); break;
			case keyDown: case autoKey: key_down(&ev); break;
			case updateEvt: update((void *)ev.message); break;
			case activateEvt:
				if ((void *)ev.message == win) { active = ev.modifiers & activeFlag; SetPort(win); if (!compact) grow_box(); }
				break;
			case osEvt:
				if (((unsigned long)ev.message >> 24) == suspendResumeMessage) fg = ev.message & resumeFlag;
				break;
			}
		}
		/* the page moves once a second: looked at twice a second */
		if (LM_TICKS - last_look >= 30) {
			last_look = LM_TICKS;
			r = read_page();
			if (r < 0 && !no_page) { no_page = 1; SetPort(win); InvalRect(PORT_RECT(win)); }
			else if (r > 0) {
				int had = M.hist_n, head = M.hist_head;
				st_update(&M, &reading);
				if (no_page) { no_page = 0; SetPort(win); InvalRect(PORT_RECT(win)); }
				else refresh(M.hist_n != had || M.hist_head != head);
			}
		}
	}
	save_prefs();
	return 0;
}
